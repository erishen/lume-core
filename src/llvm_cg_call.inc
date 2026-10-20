/* llvm_cg_call.inc - LLVM backend call functions.
 * Textually included by llvm_codegen.c (single translation unit).
 * Do not compile this file on its own. */

/* `fn(<object>, <args...>)`: the object is always the opaque i8* the runtime
 * hands back, the remaining arguments are spelled by the caller so that a
 * map's key (i8*) and value (i64/double/i8*) can differ, and the result is
 * the helper's own return type. */
static Val rt_call(CG *g, Type *rty, const char *fn, int n,
                   LLVMTypeRef *ats, LLVMValueRef *vs)
{
    AT(g);
    LLVMTypeRef fty = LLVMFunctionType(ty_of(g, rty), ats, (unsigned)n, 0);
    LLVMValueRef r = LLVMBuildCall2(g->ab, fty,
                                    rt_decl(g, fn, ty_of(g, rty), ats, n),
                                    vs, (unsigned)n, "r");
    return r ? val_make(rty, r) : (Val){ NULL, NULL };
}

/* Declare a runtime helper (src/rt.c) once, then hand it back. */
static LLVMValueRef rt_decl(CG *g, const char *name, LLVMTypeRef ret,
                            LLVMTypeRef *params, int n)
{
    LLVMValueRef fn = LLVMGetNamedFunction(g->mod, name);
    if (fn) return fn;
    return LLVMAddFunction(g->mod, name, LLVMFunctionType(ret, params, (unsigned)n, 0));
}

/* The value type a literal key maps to in m's static shape — the key set a
 * map literal carries, or a named struct's declared fields — or NULL when the
 * key is not a string literal or the shape does not mention it. Kept in step
 * with the text emitter's copy in codegen_call.c. */
static Type *map_key_shape(CG *g, Val *m, Node *knode)
{
    if (!knode || knode->type != N_LITERAL ||
        knode->as.lit.kind != LIT_STR || !m->ty || m->ty->kind != TY_STRUCT)
        return NULL;
    /* A string literal's text keeps its quotes; map keys do not carry any. */
    const char *kt = knode->as.lit.text;
    size_t kl = (size_t)knode->as.lit.len;
    if (kl >= 2 && kt[0] == '"' && kt[kl - 1] == '"') { kt++; kl -= 2; }
    Type *mt = m->ty;
    if (mt->name) {
        /* A declared struct's shape lives in the SDef table, not on the Type. */
        char kb[128];
        if (kl >= sizeof kb) return NULL;
        memcpy(kb, kt, kl);
        kb[kl] = '\0';
        int idx = struct_field_idx(g, mt->name, kb);
        return idx >= 0 ? struct_field_type(g, mt->name, idx) : NULL;
    }
    for (int i = 0; i < mt->count; i++)
        if (strlen(mt->names[i]) == kl && strncmp(mt->names[i], kt, kl) == 0)
            return mt->types[i];
    return NULL;
}

/* Which lume_map_get_* flavour reads a value of type t, and the static type
 * the read result carries. Returns 0 for a type no getter covers. */
static int get_flavour(Type *t, const char **fn, Type **rty)
{
    if (!t) return 0;
    switch (t->kind) {
    case TY_STRING: *fn = "lume_map_get_s";   *rty = type_prim(TY_STRING);  return 1;
    case TY_FLOAT:  *fn = "lume_map_get_f";   *rty = type_prim(TY_FLOAT);   return 1;
    case TY_INT:
    case TY_BOOL:   *fn = "lume_map_get_i";   *rty = type_prim(TY_INT);     return 1;
    case TY_LIST:
    case TY_STRUCT:
    case TY_RESULT:
    case TY_NULL:   *fn = "lume_map_get_obj"; *rty = type_anon_struct();    return 1;
    default: return 0;
    }
}

/* The zero of a read flavour, as the default argument's operand: the
 * pointer-returning getters take an i8* null, the float getter a double. */
static LLVMValueRef get_zero_default(CG *g, Type *rty)
{
    if (rty->kind == TY_STRING || rty->kind == TY_NULL ||
        rty->kind == TY_LIST || rty->kind == TY_STRUCT ||
        rty->kind == TY_RESULT || rty->kind == TY_ANY)
        return LLVMConstNull(g->i8ptr);
    if (rty->kind == TY_FLOAT)
        return LLVMConstReal(g->dbl, 0.0);
    return LLVMConstInt(g->i64, 0, 0);
}

static const Bi *builtin_find(const char *name)
{
    for (size_t i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i++)
        if (strcmp(BUILTINS[i].name, name) == 0) return &BUILTINS[i];
    return NULL;
}

static LLVMValueRef cg_clo_invoke(CG *g, Node *lambda, LLVMValueRef clo,
                                  LLVMValueRef *args, int argc)
{
    AT(g);
    LLVMTypeRef rec_elems[2] = { g->i8ptr, g->i8ptr };
    LLVMTypeRef rec = LLVMStructTypeInContext(g->ctx, rec_elems, 2, 0);
    LLVMValueRef fnp = LLVMBuildStructGEP2(g->ab, rec, clo, 0, "fnp");
    LLVMValueRef capp = LLVMBuildStructGEP2(g->ab, rec, clo, 1, "capp");
    if (!fnp || !capp) return NULL;
    LLVMValueRef fnv = LLVMBuildLoad2(g->ab, g->i8ptr, fnp, "fn");
    LLVMValueRef capv = LLVMBuildLoad2(g->ab, g->i8ptr, capp, "cap");
    if (!fnv || !capv) return NULL;

    int arity = lambda->as.funclit.arity;
    LLVMTypeRef *pts = (LLVMTypeRef *)xmalloc(sizeof(LLVMTypeRef) * (size_t)(arity + 1));
    pts[0] = g->i8ptr;
    for (int i = 0; i < arity; i++) {
        pts[i + 1] = lambda->as.funclit.param_types
            ? ptr_of(g, lambda->as.funclit.param_types[i]) : NULL;
        if (!pts[i + 1]) { free(pts); return NULL; }
    }
    LLVMTypeRef rty = lambda->as.funclit.ret
        ? ty_of(g, lambda->as.funclit.ret) : g->void_ty;
    LLVMTypeRef fty = LLVMFunctionType(rty ? rty : g->void_ty,
                                       pts, (unsigned)(arity + 1), 0);
    free(pts);
    if (!fty) return NULL;

    LLVMValueRef *all = (LLVMValueRef *)xmalloc(sizeof(LLVMValueRef) * (size_t)(argc + 1));
    all[0] = capv;
    for (int i = 0; i < argc; i++) all[i + 1] = args[i];
    LLVMValueRef r = LLVMBuildCall2(g->ab, fty, fnv, all,
                                    (unsigned)(argc + 1), lambda->as.funclit.ret ? "c" : "");
    free(all);
    return r;
}

static Val cg_clo_call(CG *g, Node *n, Node *lambda)
{
    AT(g);
    if (n->as.call.argc != lambda->as.funclit.arity)
        ERRV(g, "line %zu: this closure expects %d argument(s), got %d",
             n->line, lambda->as.funclit.arity, n->as.call.argc);

    Val clo = cg_expr(g, n->as.call.callee);
    if (!clo.v) ERRV(g, "line %zu: cannot evaluate the closure", n->line);

    LLVMValueRef *avs = NULL;
    if (n->as.call.argc > 0) {
        avs = (LLVMValueRef *)xmalloc(
            sizeof(LLVMValueRef) * (size_t)n->as.call.argc);
        for (int i = 0; i < n->as.call.argc; i++) {
            /* Same expect pinning as a named call: the checker threads the
             * parameter type into the argument (ck_expr(arg, param)), so a
             * struct/tuple literal argument resolves to the parameter's
             * declared type here too. */
            Type *prev = g->expect;
            g->expect = lambda->as.funclit.param_types
                ? lambda->as.funclit.param_types[i] : NULL;
            Val a = cg_expr(g, n->as.call.args[i]);
            g->expect = prev;
            if (a.ty && a.ty->kind == TY_STRUCT && !struct_is_addr(a.v)) {
                LLVMValueRef slot = LLVMBuildAlloca(g->ab, ty_of(g, a.ty), "sa");
                LLVMBuildStore(g->ab, a.v, slot);
                a.v = slot;
            }
            Type *pt = lambda->as.funclit.param_types
                ? lambda->as.funclit.param_types[i] : NULL;
            if (pt) a = coerce(g, pt, a, n->line);
            if (!a.v) { free(avs); ERRV(g, "line %zu: bad argument %d for the "
                                        "closure", n->line, i + 1); }
            avs[i] = a.v;
        }
    }

    LLVMValueRef r = cg_clo_invoke(g, lambda, clo.v, avs, n->as.call.argc);
    free(avs);
    if (!r) ERRV(g, "line %zu: cannot call through this closure", n->line);
    Type *ret = lambda->as.funclit.ret;
    /* A closure whose body yields no value is null when its result is used
     * as one — the interpreter's answer, and what the text emitter now
     * builds (it used to emit the type name as a value token). */
    if (!ret)
        return val_make(type_prim(TY_NULL), LLVMConstNull(g->i8ptr));
    return val_make(ret, r);
}

static Node *hof_lambda(CG *g, Node *a)
{
    if (a && a->type == N_FUNC_LIT) return a;
    if (a && a->type == N_VAR) {
        Type *ct = infer_node_type(g, a);
        if (ct && ct->kind == TY_FUNC && ct->func &&
            ct->func->type == N_FUNC_LIT)
            return ct->func;
    }
    return NULL;
}

static Val cg_builtin(CG *g, Node *n)
{
    const char *bname = n->as.call.callee->as.var.name;
    int argc = n->as.call.argc;
    const Bi *b = builtin_find(bname);
    AT(g);

    if (b) {
        if (argc < 1)
            ERRV(g, "line %zu: '%s' takes at least one argument", n->line, bname);

        Val *av = (Val *)xmalloc((size_t)argc * sizeof *av);
        for (int i = 0; i < argc; i++) av[i] = cg_expr(g, n->as.call.args[i]);

        int isf = av[0].ty && av[0].ty->kind == TY_FLOAT && b->fn_f;
        const char *fn  = isf ? b->fn_f  : b->fn_i;
        const char *pty = isf ? b->pty_f : b->pty_i;
        if (!fn || !pty)
            ERRV(g, "line %zu: '%s' does not apply to this operand type", n->line, bname);

        /* The helper's parameter type is not always the operand's: `min` has an
         * int and a float flavour and the operand picks one. */
        Type *want = isf ? type_prim(TY_FLOAT) : type_prim(TY_INT);
        for (int i = 0; i < argc; i++)
            if (av[i].ty && av[i].ty->kind != want->kind)
                av[i] = coerce(g, want, av[i], n->line);
        if (g->err[0]) return (Val){ NULL, NULL };

        LLVMTypeRef pt = ty_from_spelling(g, pty);
        LLVMTypeRef rt = ty_from_spelling(g, isf ? b->rty_f : b->rty_i);
        if (!pt || !rt) ERRV(g, "line %zu: bad helper signature for '%s'", n->line, bname);

        LLVMTypeRef *pts = (LLVMTypeRef *)xmalloc(sizeof(LLVMTypeRef) * (argc > 0 ? argc : 1));
        LLVMValueRef *vv = (LLVMValueRef *)xmalloc(sizeof(LLVMValueRef) * (argc > 0 ? argc : 1));
        for (int i = 0; i < argc; i++) { pts[i] = pt; vv[i] = av[i].v; }
        LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(rt, pts, (unsigned)argc, 0),
                                        rt_decl(g, fn, rt, pts, argc), vv, (unsigned)argc, "b");
        free(pts); free(vv); free(av);
        return val_make(want, r);
    }

    /* env(name): a host environment variable as a string; unset or masked
     * (credential) names read as null — same promise as the interpreter's
     * native_env (builtins_fs.c), kept by lume_bi_env (bridge_native.c). */
    if (strcmp(bname, "env") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: env() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        LLVMTypeRef p1 = g->i8ptr;
        LLVMValueRef r = LLVMBuildCall2(g->ab,
                                        LLVMFunctionType(g->i8ptr, &p1, 1, 0),
                                        rt_decl(g, "lume_bi_env", g->i8ptr, &p1, 1),
                                        (LLVMValueRef[]){ a.v }, 1, "b");
        return val_make(type_prim(TY_STRING), r);
    }

    /* now(): epoch seconds (SPEC §6.2, matching the interpreter's now() and
     * bridge_native's lume_bi_now). */
    if (strcmp(bname, "now") == 0) {
        if (argc != 0)
            ERRV(g, "line %zu: now() takes no arguments", n->line);
        LLVMValueRef r = LLVMBuildCall2(g->ab,
                                        LLVMFunctionType(g->i64, NULL, 0, 0),
                                        rt_decl(g, "lume_bi_now", g->i64, NULL, 0),
                                        NULL, 0, "now");
        return val_make(type_prim(TY_INT), r);
    }

    /* File builtins (SPEC §6.2, mirroring the text backend's branches):
     * read_file -> string-or-null; write_file/lock_file -> bool;
     * unlock_file -> bool, releasing the process-wide lock. */
    if (strcmp(bname, "read_file") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: read_file() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v) return (Val){ NULL, NULL, 0 };
        LLVMTypeRef pp[1] = { g->i8ptr };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
                                        LLVMFunctionType(g->i8ptr, pp, 1, 0),
                                        rt_decl(g, "lume_bi_read_file", g->i8ptr, pp, 1),
                                        (LLVMValueRef[]){ a.v }, 1, "rf");
        return val_make(type_prim(TY_STRING), r);
    }
    if (strcmp(bname, "write_file") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: write_file() takes exactly two arguments", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !b.v) return (Val){ NULL, NULL, 0 };
        LLVMTypeRef pp[2] = { g->i8ptr, g->i8ptr };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
                                        LLVMFunctionType(g->i64, pp, 2, 0),
                                        rt_decl(g, "lume_bi_write_file", g->i64, pp, 2),
                                        (LLVMValueRef[]){ a.v, b.v }, 2, "wf");
        return val_make(type_prim(TY_INT), r);
    }
    if (strcmp(bname, "lock_file") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: lock_file() takes a path and a wait (ms)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val m = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !m.v) return (Val){ NULL, NULL, 0 };
        LLVMTypeRef pp[2] = { g->i8ptr, g->i64 };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
                                        LLVMFunctionType(g->i64, pp, 2, 0),
                                        rt_decl(g, "lume_bi_lock_file", g->i64, pp, 2),
                                        (LLVMValueRef[]){ a.v, m.v }, 2, "lf");
        return val_make(type_prim(TY_INT), r);
    }
    if (strcmp(bname, "unlock_file") == 0) {
        if (argc != 0)
            ERRV(g, "line %zu: unlock_file() takes no arguments", n->line);
        LLVMValueRef r = LLVMBuildCall2(g->ab,
                                        LLVMFunctionType(g->i64, NULL, 0, 0),
                                        rt_decl(g, "lume_bi_unlock_file", g->i64, NULL, 0),
                                        NULL, 0, "ulf");
        return val_make(type_prim(TY_INT), r);
    }

    /* str() / int() / float(): which helper they need depends on the operand. */
    if (strcmp(bname, "str") == 0 || strcmp(bname, "int") == 0 ||
        strcmp(bname, "float") == 0 || strcmp(bname, "stringify") == 0 ||
        strcmp(bname, "json") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: '%s' takes exactly one argument", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        LLVMTypeRef ft = NULL, pty = NULL, rty = NULL;
        Type *rt = NULL;
        bool is_str = strcmp(bname, "str") == 0;
        bool is_json = strcmp(bname, "json") == 0;

        if (is_str || strcmp(bname, "stringify") == 0 || is_json) {
            switch (a.ty ? a.ty->kind : TY_ANY) {
            case TY_INT:    ft = LLVMFunctionType(g->i8ptr, &g->i64, 1, 0);  pty = g->i64;    break;
            case TY_FLOAT:  ft = LLVMFunctionType(g->i8ptr, &g->dbl, 1, 0);  pty = g->dbl;    break;
            case TY_BOOL:   ft = LLVMFunctionType(g->i8ptr, &g->i64, 1, 0);  pty = g->i64;    break;
            case TY_STRING:
                /* str of a string is itself; stringify quotes it (the
                 * interpreter's json_append_value does); json(str) means
                 * parse: the result is a tagged-any tree (LumeSlot*),
                 * readable through the slot helpers (src/rt.c). Mirrors the
                 * text emitter's json(str) branch in codegen_call.c. */
                if (is_str) return a;
                if (is_json) {
                    LLVMTypeRef ety = LLVMArrayType(LLVMInt8TypeInContext(g->ctx), 256);
                    LLVMValueRef eb = LLVMBuildAlloca(g->ab, ety, "jeb");
                    LLVMValueRef ep = LLVMBuildInBoundsGEP2(g->ab, ety, eb,
                        (LLVMValueRef[]){ LLVMConstInt(g->i64, 0, 0),
                                         LLVMConstInt(g->i64, 0, 0) }, 2, "jep");
                    LLVMBuildStore(g->ab,
                        LLVMConstNull(LLVMInt8TypeInContext(g->ctx)), ep);
                    LLVMTypeRef p3[3] = { g->i8ptr, g->i8ptr, g->i64 };
                    LLVMValueRef v3[3] = { a.v, ep, LLVMConstInt(g->i64, 256, 0) };
                    LLVMValueRef r = LLVMBuildCall2(g->ab,
                        LLVMFunctionType(g->i8ptr, p3, 3, 0),
                        rt_decl(g, "lume_json_parse", g->i8ptr, p3, 3), v3, 3, "j");
                    if (!r) ERRV(g, "line %zu: json() parse failed", n->line);
                    /* Parse failure goes through the recoverable-error
                     * channel (rt.c lume_error_set): inside try() it records
                     * the message (ok stays a null slot, err gets the text),
                     * outside try() it prints and exits — the interpreter's
                     * json() VM error, same shape. The failure branch carries
                     * on to the shared return with r (lume_json_parse returns
                     * NULL on failure = the null slot), mirroring the text
                     * emitter's json branch in codegen_call.c. */
                    LLVMBasicBlockRef jb_bad = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "json_bad");
                    LLVMBasicBlockRef jb_ok  = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "json_ok");
                    LLVMValueRef jn = LLVMBuildIsNull(g->ab, r, "jnull");
                    if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, jn, jb_bad, jb_ok);
                    g->cur = jb_bad; AT(g);
                    LLVMBuildCall2(g->ab,
                        LLVMFunctionType(g->void_ty, (LLVMTypeRef[]){ g->i8ptr }, 1, 0),
                        rt_decl(g, "lume_error_set", g->void_ty, (LLVMTypeRef[]){ g->i8ptr }, 1),
                        (LLVMValueRef[]){ ep }, 1, "");
                    if (!DONE(g->cur)) LLVMBuildBr(g->ab, jb_ok);
                    g->cur = jb_ok; AT(g);
                    return val_make(type_prim(TY_ANY), r);
                }
                ft = LLVMFunctionType(g->i8ptr, &g->i8ptr, 1, 0); pty = g->i8ptr;
                break;
            case TY_LIST:
                /* Containers serialize to the same JSON the interpreter's
                 * print() produces; a.v is the i8* container pointer. */
                ft = LLVMFunctionType(g->i8ptr, &g->i8ptr, 1, 0);
                pty = g->i8ptr;
                break;
            case TY_STRUCT:
                /* An anonymous struct is a map (a LumeMap* i8*) and serializes
                 * directly. A named one is a raw GEP struct with no runtime
                 * header, so build a map from its fields first (the same path
                 * print() takes) and then serialize that map — by retagging
                 * `a` as an anonymous struct the existing lume_rt_to_json_obj
                 * route below picks it up. */
                if (a.ty->name) {
                    struct_addr(g, &a);
                    Val m = emit_struct_to_map(g, a.v, a.ty);
                    if (!m.v) return m;
                    a.v = m.v;
                    a.ty = type_anon_struct();
                    ft = LLVMFunctionType(g->i8ptr, &g->i8ptr, 1, 0);
                    pty = g->i8ptr;
                    break;
                }
                ft = LLVMFunctionType(g->i8ptr, &g->i8ptr, 1, 0);
                pty = g->i8ptr;
                break;
            case TY_ANY:
                /* A tagged-any value serializes by its runtime tag (src/rt.c
                 * lume_slot_str): containers as JSON, strings quoted, scalars
                 * unquoted -- the interpreter's str() rules. */
                ft = LLVMFunctionType(g->i8ptr, &g->i8ptr, 1, 0);
                pty = g->i8ptr;
                break;
            case TY_NULL:
                /* The serializer's null entry point prints the bare word the
                 * interpreter's str(null)/stringify(null) produce. */
                ft = LLVMFunctionType(g->i8ptr, &g->i8ptr, 1, 0);
                pty = g->i8ptr;
                break;
            default:
                ERRV(g, "line %zu: %s() cannot convert this value",
                     n->line, bname);
            }
            rty = g->i8ptr; rt = type_prim(TY_STRING);
        } else if (strcmp(bname, "int") == 0) {
            if (!a.ty || a.ty->kind != TY_FLOAT)
                ERRV(g, "line %zu: int() only converts a float here", n->line);
            ft = LLVMFunctionType(g->i64, &g->dbl, 1, 0); pty = g->dbl; rty = g->i64;
            rt = type_prim(TY_INT);
        } else {
            if (!a.ty || a.ty->kind != TY_INT)
                ERRV(g, "line %zu: float() only converts an int here", n->line);
            ft = LLVMFunctionType(g->dbl, &g->i64, 1, 0); pty = g->i64; rty = g->dbl;
            rt = type_prim(TY_FLOAT);
        }

        const char *name;
        if (a.ty && a.ty->kind == TY_ANY)
            /* str() of a string slot is the bare string, stringify() keeps the
             * JSON quotes (rt.c lume_slot_str_scalar / lume_slot_str) — the
             * text backend's TY_ANY rule, mirrored. */
            name = strcmp(bname, "str") == 0 ? "lume_slot_str_scalar"
                                             : "lume_slot_str";
        else if (a.ty && (a.ty->kind == TY_LIST || a.ty->kind == TY_NULL ||
                     (a.ty->kind == TY_STRUCT && !a.ty->name)))
            name = "lume_rt_to_json_obj";
        else if (a.ty && a.ty->kind == TY_STRING)
            /* only stringify reaches here with a string (str returns it as-is,
             * json(str) is rejected above): the quoted-string flavour. */
            name = "lume_rt_quote_str";
        else if (strcmp(bname, "int") != 0 && strcmp(bname, "float") != 0)
            /* str / stringify / json share the scalar stringifying flavours
             * (int/float take their own conversion helpers below). */
            name = (a.ty && a.ty->kind == TY_FLOAT) ? "lume_bi_str_double"
                 : (a.ty && a.ty->kind == TY_BOOL)  ? "lume_bi_str_bool"
                                                     : "lume_bi_str_i64";
        else
            name = strcmp(bname, "int") == 0 ? "lume_bi_int" : "lume_bi_float";

        /* A comparison result is i1 in LLVM, but the runtime's bool builtins
         * take i64 (the interpreter's bool representation). Zext before the
         * call — otherwise str(1 == 1.0) fails to verify with an i1 argument. */
        LLVMValueRef v[1] = { a.v };
        if (a.ty && a.ty->kind == TY_BOOL && pty == g->i64)
            v[0] = LLVMBuildZExt(g->ab, a.v, g->i64, "boolarg");
        LLVMValueRef r = LLVMBuildCall2(g->ab, ft, rt_decl(g, name, rty, &pty, 1),
                                        v, 1, "b");
        return val_make(rt, r);
    }

    /* len() — the helper follows the static type, so a list and a string both
     * spelled `len(x)` still reach the right one. */
    if (strcmp(bname, "len") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: len() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || (a.ty->kind != TY_LIST && a.ty->kind != TY_STRING &&
                              a.ty->kind != TY_ANY))
            ERRV(g, "line %zu: len() needs a list, a string or an any value", n->line);
        LLVMTypeRef p[1] = { g->i8ptr };
        LLVMValueRef v[1] = { a.v };
        const char *lenfn = a.ty->kind == TY_LIST ? "lume_list_len"
                          : a.ty->kind == TY_ANY  ? "lume_slot_len"
                                                  : "lume_bi_len_s";
        LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i64, p, 1, 0),
                                        rt_decl(g, lenfn, g->i64, p, 1), v, 1, "len");
        if (!r) ERRV(g, "line %zu: cannot take the length of this value", n->line);
        return val_make(type_prim(TY_INT), r);
    }

    /* push(list, item) / put(map, key, value) — both mutate in place and hand
     * the container back, which is what the interpreter does. */
    if (strcmp(bname, "push") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: push() takes exactly two arguments", n->line);
        Val l = cg_expr(g, n->as.call.args[0]);
        Val e = cg_expr(g, n->as.call.args[1]);
        const char *fn = list_push_fn(e.ty);
        if (!l.v || !e.v || !l.ty || l.ty->kind != TY_LIST || !fn)
            ERRV(g, "line %zu: push() needs a list and a scalar", n->line);
        if (e.ty->kind == TY_BOOL) e = coerce(g, type_prim(TY_INT), e, n->line);
        LLVMTypeRef ats[2] = { g->i8ptr, ty_of(g, e.ty) };
        LLVMValueRef vs[2] = { l.v, e.v };
        Val r = rt_call(g, type_prim(TY_INT), fn, 2, ats, vs);
        if (!r.v) ERRV(g, "line %zu: push() failed", n->line);
        return l;                       /* the list itself, as the interpreter returns */
    }

    if (strcmp(bname, "put") == 0) {
        if (argc != 3)
            ERRV(g, "line %zu: put() takes exactly three arguments", n->line);
        Val m = cg_expr(g, n->as.call.args[0]);
        Val k = cg_expr(g, n->as.call.args[1]);
        Val v = cg_expr(g, n->as.call.args[2]);
        const char *fn = map_put_fn(v.ty);
        if (!m.v || !k.v || !v.v || !fn)
            ERRV(g, "line %zu: put() needs a map, a key and a scalar", n->line);
        if (v.ty->kind == TY_BOOL) v = coerce(g, type_prim(TY_INT), v, n->line);
        LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, ty_of(g, v.ty) };
        LLVMValueRef vs[3] = { m.v, k.v, v.v };
        Val r = rt_call(g, type_prim(TY_INT), fn, 3, ats, vs);
        if (!r.v) ERRV(g, "line %zu: put() failed", n->line);
        return m;
    }

    /* keys(m) — the helper builds the list, so the result is a list value. */
    if (strcmp(bname, "keys") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: keys() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind == TY_LIST)
            ERRV(g, "line %zu: keys() needs a map", n->line);
        LLVMTypeRef p[1] = { g->i8ptr };
        LLVMValueRef v[1] = { a.v };
        LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, p, 1, 0),
                                        rt_decl(g, "lume_map_keys", g->i8ptr, p, 1),
                                        v, 1, "keys");
        if (!r) ERRV(g, "line %zu: keys() failed", n->line);
        return val_make(list_type(), r);
    }

    /* range(...) — the helper runs the same double arithmetic the
     * interpreter's native_range() does (whole values become INT slots), so
     * print/for-in agree compiled or interpreted. Numeric args only: a slot
     * holds a statically-typed number, and an `any` has no reader. */
    if (strcmp(bname, "range") == 0) {
        if (argc < 1 || argc > 3)
            ERRV(g, "line %zu: range() takes one to three arguments", n->line);
        Val a = argc >= 2 ? cg_expr(g, n->as.call.args[0]) : (Val){ NULL, NULL };
        Val b = cg_expr(g, n->as.call.args[argc == 1 ? 0 : 1]);
        Val s = argc == 3 ? cg_expr(g, n->as.call.args[2]) : (Val){ NULL, NULL };
        Val *xs[3] = { &a, &b, &s };
        for (int i = 0; i < 3; i++) {
            Val *x = xs[i];
            if (!x->v) continue;                    /* absent start/step    */
            if (!x->ty || (x->ty->kind != TY_INT && x->ty->kind != TY_FLOAT))
                ERRV(g, "line %zu: range() arguments must be numbers", n->line);
            *x = coerce(g, type_prim(TY_FLOAT), *x, n->line);
        }
        LLVMTypeRef ats[3] = { g->dbl, g->dbl, g->dbl };
        LLVMValueRef vs[3] = {
            a.v ? a.v : LLVMConstReal(g->dbl, 0.0),
            b.v,
            s.v ? s.v : LLVMConstReal(g->dbl, 1.0),
        };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->i8ptr, ats, 3, 0),
            rt_decl(g, "lume_range", g->i8ptr, ats, 3), vs, 3, "range");
        if (!r) ERRV(g, "line %zu: range() failed", n->line);
        return val_make(list_type(), r);
    }

    /* get(m, k[, default]) — an absent key yields the default, so the flavour
     * follows the default and an unannotated get(m, k) in a bool context stays
     * an int read rather than silently becoming a string. */
    if (strcmp(bname, "get") == 0) {
        if (argc < 2 || argc > 3)
            ERRV(g, "line %zu: get() takes two or three arguments", n->line);
        Val m = cg_expr(g, n->as.call.args[0]);
        Val k = cg_expr(g, n->as.call.args[1]);
        Val d = argc == 3 ? cg_expr(g, n->as.call.args[2]) : (Val){ NULL, NULL };
        if (!m.v || !k.v)
            ERRV(g, "line %zu: get() needs a map, a key and a scalar default", n->line);

        /* The flavour follows the *shape* of the key when the map's static
         * type declares it: the value at that key is always the declared
         * type. A default of a different type could only ever fire for an
         * absent key, so it is refused here rather than silently reading the
         * slot through the wrong getter. Without a shape the flavour follows
         * the *default*, and the int read when neither says. */
        const char *fn = "lume_map_get_i";
        Type    *rty = type_prim(TY_INT);
        if (m.ty && m.ty->kind == TY_ANY) {
            /* A tagged-any value (a json() parse): the runtime shape is only
             * known at runtime, so the read goes through the slot getters
             * (src/rt.c), chosen by the default's type -- the same mapping
             * the text emitter's copy uses. The result keeps the default's
             * own type where codegen has one: a [] default reads a json
             * array back as a list, a {} default an anonymous map, a string
             * default a string. BOOL keeps the get_flavour mapping (an int
             * read), matching get() on a typed map. */
            if (d.v && d.ty && d.ty->kind != TY_ANY) {
                const char *sfn; Type *srty;
                if (!get_flavour(d.ty, &sfn, &srty))
                    ERRV(g, "line %zu: get() cannot read a %s off an any value",
                         n->line, ty_str(d.ty));
                if (strcmp(sfn, "lume_map_get_i") == 0)      fn = "lume_slot_get_i";
                else if (strcmp(sfn, "lume_map_get_f") == 0) fn = "lume_slot_get_f";
                else if (strcmp(sfn, "lume_map_get_s") == 0) fn = "lume_slot_get_s";
                else if (strcmp(sfn, "lume_map_get_obj") == 0) fn = "lume_slot_get_obj";
                else
                    ERRV(g, "line %zu: get() cannot read a %s off an any value",
                         n->line, ty_str(d.ty));
                rty = d.ty->kind != TY_BOOL ? d.ty : srty;
            } else {
                /* No default (or an any default): read the cell back boxed
                 * as its own tag — the interpreter's dynamic read. Kept in
                 * step with the text emitter's copy. */
                fn = "lume_slot_get_any";
                rty = type_prim(TY_ANY);
            }
        } else if (m.ty && m.ty->kind == TY_LIST) {
            /* get(list, i[, default]): an index read — mirrors the text
             * emitter's branch. The element type drives the accessor. */
            Type *et = m.ty->elem;
            const char *atfn = list_at_fn(et);
            if (!atfn)
                ERRV(g, "line %zu: get() cannot index a list of %s",
                     n->line, et ? ty_str(et) : "unknown");
            if (d.v && d.ty && d.ty->kind != TY_ANY &&
                !(et && d.ty->kind == et->kind) &&
                !(d.ty->kind == TY_BOOL && et && et->kind == TY_INT))
                ERRV(g, "line %zu: get() default is %s but the list holds %s",
                     n->line, ty_str(d.ty), et ? ty_str(et) : "unknown");
            LLVMTypeRef rty_t = et && et->kind == TY_STRING ? g->i8ptr :
                                et && et->kind == TY_FLOAT ? g->dbl : g->i64;
            LLVMTypeRef pat[2] = { g->i8ptr, g->i64 };
            LLVMValueRef rv = LLVMBuildCall2(g->ab,
                                LLVMFunctionType(rty_t, pat, 2, 0),
                                rt_decl(g, atfn, rty_t, pat, 2),
                                (LLVMValueRef[]){ m.v, k.v }, 2, "gl");
            return val_make(et ? et : type_prim(TY_INT), rv);
        } else {
            Type *shape = map_key_shape(g, &m, n->as.call.args[1]);
            if (shape && shape->kind != TY_ANY) {
                const char *sfn; Type *srty;
                if (!get_flavour(shape, &sfn, &srty))
                    ERRV(g, "line %zu: get() cannot read a value of type %s",
                         n->line, ty_str(shape));
                if (d.v && d.ty && d.ty->kind != TY_ANY && d.ty->kind != shape->kind &&
                    !(d.ty->kind == TY_BOOL && shape->kind == TY_INT) &&
                    !(d.ty->kind == TY_INT && shape->kind == TY_BOOL))
                    ERRV(g, "line %zu: get() default is %s but key %s holds %s",
                         n->line, ty_str(d.ty), n->as.call.args[1]->as.lit.text,
                         ty_str(shape));
                fn = sfn; rty = srty;
            } else if (d.v && d.ty && d.ty->kind != TY_ANY) {
                const char *dfn; Type *drty;
                if (get_flavour(d.ty, &dfn, &drty)) { fn = dfn; rty = drty; }
            }
        }
        if (d.v && d.ty && d.ty->kind == TY_BOOL)
            d = coerce(g, type_prim(TY_INT), d, n->line);

        /* Always three operands: the runtime getters are declared with the
         * default parameter, and a two-operand call against that declaration
         * left the default reading stack garbage — an absent key then handed
         * that garbage back (printed as, e.g., 73896). */
        LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, ty_of(g, rty) };
        LLVMValueRef vs[3] = { m.v, k.v,
                               d.v ? d.v : get_zero_default(g, rty) };
        Val r = rt_call(g, rty, fn, 3, ats, vs);
        if (!r.v) ERRV(g, "line %zu: get() failed", n->line);
        return r;
    }

    /* try(fn) — SPEC 6.3: call fn with no arguments and wrap the outcome
     * in { ok, err }, exactly as the interpreter prints. error_begin/end
     * bracket the closure call; the map is filled by branch — the error
     * branch puts obj-null ok + string err, the ok branch puts the result
     * (slot-ized for any payloads) and obj-null err. Mirrors the text
     * emitter's try block in codegen_call.c. */
    if (strcmp(bname, "try") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: try() expects (fn)", n->line);
        Node *lam = hof_lambda(g, n->as.call.args[0]);
        if (!lam)
            ERRV(g, "line %zu: try() needs a lambda or a closure variable",
                 n->line);
        Val fnv = cg_expr(g, n->as.call.args[0]);
        if (!fnv.v) ERRV(g, "line %zu: try() needs a function", n->line);

        LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, NULL, 0, 0),
                       rt_decl(g, "lume_error_begin", g->void_ty, NULL, 0),
                       NULL, 0, "");
        LLVMValueRef res = cg_clo_invoke(g, lam, fnv.v, NULL, 0);
        if (!res) ERRV(g, "line %zu: try() could not call the function", n->line);
        LLVMValueRef emsg = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i8ptr, NULL, 0, 0),
                rt_decl(g, "lume_error_end", g->i8ptr, NULL, 0),
                NULL, 0, "terr");
        LLVMValueRef m = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i8ptr, NULL, 0, 0),
                rt_decl(g, "lume_map_new", g->i8ptr, NULL, 0),
                NULL, 0, "tmap");
        LLVMValueRef kok = cg_string_val(g, "ok", 2);
        LLVMValueRef kerr = cg_string_val(g, "err", 3);

        LLVMBasicBlockRef bb_e = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "try.err");
        LLVMBasicBlockRef bb_o = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "try.ok");
        LLVMBasicBlockRef bb_j = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "try.end");
        LLVMValueRef terr = LLVMBuildIsNull(g->ab, emsg, "tnull");
        LLVMValueRef thas = LLVMBuildNot(g->ab, terr, "thas");
        if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, thas, bb_e, bb_o);

        g->cur = bb_e; AT(g);
        {
            LLVMTypeRef p3[3] = { g->i8ptr, g->i8ptr, g->i8ptr };
            LLVMValueRef vn[3] = { m, kok, LLVMConstNull(g->i8ptr) };
            LLVMBuildCall2(g->ab, LLVMFunctionType(g->i64, p3, 3, 0),
                           rt_decl(g, "lume_map_put_obj", g->i64, p3, 3),
                           vn, 3, "");
            LLVMValueRef ve[3] = { m, kerr, emsg };
            LLVMBuildCall2(g->ab, LLVMFunctionType(g->i64, p3, 3, 0),
                           rt_decl(g, "lume_map_put_s", g->i64, p3, 3),
                           ve, 3, "");
        }
        LLVMBuildBr(g->ab, bb_j);

        g->cur = bb_o; AT(g);
        {
            Type *ret = lam->as.funclit.ret;
            if (ret && ret->kind != TY_NULL) {
                if (ret->kind == TY_ANY) {
                    LLVMTypeRef p3[3] = { g->i8ptr, g->i8ptr, g->i8ptr };
                    LLVMValueRef va[3] = { m, kok, res };
                    LLVMBuildCall2(g->ab, LLVMFunctionType(g->i64, p3, 3, 0),
                                   rt_decl(g, "lume_map_put_slot", g->i64, p3, 3),
                                   va, 3, "");
                } else {
                    const char *pfn = map_put_fn(ret);
                    if (!pfn)
                        ERRV(g, "line %zu: try() result has no codegen type", n->line);
                    LLVMTypeRef pa[3] = { g->i8ptr, g->i8ptr, ty_of(g, ret) };
                    LLVMValueRef va2[3] = { m, kok, res };
                    LLVMBuildCall2(g->ab, LLVMFunctionType(g->i64, pa, 3, 0),
                                   rt_decl(g, pfn, g->i64, pa, 3),
                                   va2, 3, "");
                }
            } else {
                LLVMTypeRef p3[3] = { g->i8ptr, g->i8ptr, g->i8ptr };
                LLVMValueRef vn[3] = { m, kok, LLVMConstNull(g->i8ptr) };
                LLVMBuildCall2(g->ab, LLVMFunctionType(g->i64, p3, 3, 0),
                               rt_decl(g, "lume_map_put_obj", g->i64, p3, 3),
                               vn, 3, "");
            }
            LLVMTypeRef p3[3] = { g->i8ptr, g->i8ptr, g->i8ptr };
            LLVMValueRef vn[3] = { m, kerr, LLVMConstNull(g->i8ptr) };
            LLVMBuildCall2(g->ab, LLVMFunctionType(g->i64, p3, 3, 0),
                           rt_decl(g, "lume_map_put_obj", g->i64, p3, 3),
                           vn, 3, "");
        }
        LLVMBuildBr(g->ab, bb_j);

        g->cur = bb_j; AT(g);
        return val_make(type_anon_struct(), m);
    }

    /* map(fn, list) / filter(fn, list) / reduce(fn, list, init) — SPEC 4.8's
     * higher-order builtins, looped inline. The lambda's parameters were
     * pinned by the shared signature pass (param[0] from the list element,
     * reduce's param[0] from the init), so the loop reads elements through
     * the same accessor table for-in uses and calls the closure with the
     * exact signature its define was built against. Mirrors the text
     * emitter's copy branch for branch. */

    if (strcmp(bname, "map") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: map() takes exactly two arguments", n->line);
        Node *lambda = hof_lambda(g, n->as.call.args[0]);
        if (!lambda)
            ERRV(g, "line %zu: map() needs a lambda as its first argument",
                 n->line);
        Type *ret = lambda->as.funclit.ret;
        const char *pushfn = list_push_fn(ret);
        if (!ret || !pushfn)
            ERRV(g, "line %zu: map() needs a lambda that returns a value "
                    "this backend can put in a list", n->line);
        Type *et = lambda->as.funclit.param_types
            ? lambda->as.funclit.param_types[0] : NULL;
        const char *atfn = list_at_fn(et);
        if (!atfn)
            ERRV(g, "line %zu: a list of this element type cannot be mapped",
                 n->line);

        Val fnv = cg_expr(g, n->as.call.args[0]);
        Val src = cg_expr(g, n->as.call.args[1]);
        if (!fnv.v || !src.v || !src.ty || src.ty->kind != TY_LIST)
            ERRV(g, "line %zu: map() needs a list as its second argument",
                 n->line);

        Val dst = rt_call(g, list_type(), "lume_list_new", 0, NULL, NULL);
        if (!dst.v) ERRV(g, "line %zu: cannot allocate a list", n->line);

        /* index + snapshot length, the for-in shape: the bound is captured
         * once, so a push() the lambda makes cannot extend the iteration. */
        LLVMValueRef idx = LLVMBuildAlloca(g->ab, g->i64, "li");
        LLVMBuildStore(g->ab, LLVMConstInt(g->i64, 0, 0), idx);
        LLVMValueRef len_slot = LLVMBuildAlloca(g->ab, g->i64, "len");
        {
            LLVMTypeRef p1[1] = { g->i8ptr };
            LLVMValueRef v1[1] = { src.v };
            LLVMValueRef len0 = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i64, p1, 1, 0),
                rt_decl(g, "lume_list_len", g->i64, p1, 1), v1, 1, "len0");
            LLVMBuildStore(g->ab, len0, len_slot);
        }

        LLVMBasicBlockRef lc = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "map.cond");
        LLVMBasicBlockRef lb = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "map.body");
        LLVMBasicBlockRef li = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "map.incr");
        LLVMBasicBlockRef le = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "map.end");

        if (!DONE(g->cur)) LLVMBuildBr(g->ab, lc);
        g->cur = lc; AT(g);
        LLVMValueRef len = LLVMBuildLoad2(g->ab, g->i64, len_slot, "len");
        LLVMValueRef cur = LLVMBuildLoad2(g->ab, g->i64, idx, "cur");
        LLVMValueRef cond = LLVMBuildICmp(g->ab, LLVMIntSLT, cur, len, "c");
        LLVMBuildCondBr(g->ab, cond, lb, le);

        g->cur = lb; AT(g);
        Type *rt = et->kind == TY_BOOL ? type_prim(TY_INT) : et;
        LLVMTypeRef p2[2] = { g->i8ptr, g->i64 };
        LLVMValueRef v2[2] = { src.v, cur };
        Val e = rt_call(g, rt, atfn, 2, p2, v2);
        if (!e.v) ERRV(g, "line %zu: cannot read an element", n->line);
        Type *p0 = lambda->as.funclit.param_types
            ? lambda->as.funclit.param_types[0] : NULL;
        if (p0) e = coerce(g, p0, e, n->line);
        if (!e.v) ERRV(g, "line %zu: cannot coerce the map argument", n->line);
        LLVMValueRef rv = cg_clo_invoke(g, lambda, fnv.v, &e.v, 1);
        if (!rv) ERRV(g, "line %zu: cannot call the mapping closure", n->line);
        Val item = val_make(ret, rv);
        if (item.ty && item.ty->kind == TY_BOOL)
            item = coerce(g, type_prim(TY_INT), item, n->line);
        if (!item.v) ERRV(g, "line %zu: cannot coerce the mapped value", n->line);
        {
            LLVMTypeRef pa[2] = { g->i8ptr, ty_of(g, item.ty) };
            LLVMValueRef va[2] = { dst.v, item.v };
            Val pr = rt_call(g, type_prim(TY_INT), pushfn, 2, pa, va);
            if (!pr.v) ERRV(g, "line %zu: cannot append to a list", n->line);
        }
        LLVMBuildBr(g->ab, li);

        g->cur = li; AT(g);
        LLVMValueRef nxt = LLVMBuildAdd(g->ab, cur, LLVMConstInt(g->i64, 1, 0), "nxt");
        LLVMBuildStore(g->ab, nxt, idx);
        LLVMBuildBr(g->ab, lc);

        g->cur = le; AT(g);
        return val_make(type_list(ret), dst.v);
    }

    if (strcmp(bname, "filter") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: filter() takes exactly two arguments", n->line);
        Node *lambda = hof_lambda(g, n->as.call.args[0]);
        if (!lambda)
            ERRV(g, "line %zu: filter() needs a lambda as its first argument",
                 n->line);
        Type *ret = lambda->as.funclit.ret;
        if (!ret || (ret->kind != TY_BOOL && ret->kind != TY_INT &&
                     ret->kind != TY_FLOAT))
            ERRV(g, "line %zu: filter() needs a lambda that returns a bool "
                    "(the native backend tests the truth of a bool, an int "
                    "or a float)", n->line);
        Type *et = lambda->as.funclit.param_types
            ? lambda->as.funclit.param_types[0] : NULL;
        const char *atfn = list_at_fn(et);
        if (!atfn)
            ERRV(g, "line %zu: a list of this element type cannot be filtered",
                 n->line);

        Val fnv = cg_expr(g, n->as.call.args[0]);
        Val src = cg_expr(g, n->as.call.args[1]);
        if (!fnv.v || !src.v || !src.ty || src.ty->kind != TY_LIST)
            ERRV(g, "line %zu: filter() needs a list as its second argument",
                 n->line);

        Val dst = rt_call(g, list_type(), "lume_list_new", 0, NULL, NULL);
        if (!dst.v) ERRV(g, "line %zu: cannot allocate a list", n->line);

        LLVMValueRef idx = LLVMBuildAlloca(g->ab, g->i64, "li");
        LLVMBuildStore(g->ab, LLVMConstInt(g->i64, 0, 0), idx);
        LLVMValueRef len_slot = LLVMBuildAlloca(g->ab, g->i64, "len");
        {
            LLVMTypeRef p1[1] = { g->i8ptr };
            LLVMValueRef v1[1] = { src.v };
            LLVMValueRef len0 = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i64, p1, 1, 0),
                rt_decl(g, "lume_list_len", g->i64, p1, 1), v1, 1, "len0");
            LLVMBuildStore(g->ab, len0, len_slot);
        }

        LLVMBasicBlockRef lc = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "flt.cond");
        LLVMBasicBlockRef lb = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "flt.body");
        LLVMBasicBlockRef lp = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "flt.push");
        LLVMBasicBlockRef li = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "flt.incr");
        LLVMBasicBlockRef le = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "flt.end");

        if (!DONE(g->cur)) LLVMBuildBr(g->ab, lc);
        g->cur = lc; AT(g);
        LLVMValueRef len = LLVMBuildLoad2(g->ab, g->i64, len_slot, "len");
        LLVMValueRef cur = LLVMBuildLoad2(g->ab, g->i64, idx, "cur");
        LLVMValueRef cond = LLVMBuildICmp(g->ab, LLVMIntSLT, cur, len, "c");
        LLVMBuildCondBr(g->ab, cond, lb, le);

        g->cur = lb; AT(g);
        Type *rt = et->kind == TY_BOOL ? type_prim(TY_INT) : et;
        LLVMTypeRef p2[2] = { g->i8ptr, g->i64 };
        LLVMValueRef v2[2] = { src.v, cur };
        Val e = rt_call(g, rt, atfn, 2, p2, v2);
        if (!e.v) ERRV(g, "line %zu: cannot read an element", n->line);
        /* The element travels twice: the raw runtime value into the result
         * list when the test passes, and a copy — narrowed to the
         * parameter's spelling — into the closure. coerce() replaces the
         * value it converts, so the closure argument is built from a
         * separate Val and the raw e stays owned for the push below. */
        Val arg = e;
        Type *p0 = lambda->as.funclit.param_types
            ? lambda->as.funclit.param_types[0] : NULL;
        if (p0) arg = coerce(g, p0, arg, n->line);
        if (!arg.v) ERRV(g, "line %zu: cannot coerce the filter argument", n->line);
        LLVMValueRef rv = cg_clo_invoke(g, lambda, fnv.v, &arg.v, 1);
        if (!rv) ERRV(g, "line %zu: cannot call the filter closure", n->line);

        LLVMValueRef test;
        if (ret->kind == TY_BOOL)      test = rv;
        else if (ret->kind == TY_FLOAT) test = LLVMBuildFCmp(g->ab, LLVMRealUNE,
                                                             rv, LLVMConstReal(g->dbl, 0.0), "t");
        else                            test = LLVMBuildICmp(g->ab, LLVMIntNE,
                                                             rv, LLVMConstInt(g->i64, 0, 0), "t");
        if (!test) ERRV(g, "line %zu: cannot test the filter result", n->line);
        LLVMBuildCondBr(g->ab, test, lp, li);

        g->cur = lp; AT(g);
        {
            LLVMTypeRef pa[2] = { g->i8ptr, ty_of(g, rt) };
            LLVMValueRef va[2] = { dst.v, e.v };
            Val pr = rt_call(g, type_prim(TY_INT), list_push_fn(et), 2, pa, va);
            if (!pr.v) ERRV(g, "line %zu: cannot append to a list", n->line);
        }
        LLVMBuildBr(g->ab, li);

        g->cur = li; AT(g);
        LLVMValueRef nxt = LLVMBuildAdd(g->ab, cur, LLVMConstInt(g->i64, 1, 0), "nxt");
        LLVMBuildStore(g->ab, nxt, idx);
        LLVMBuildBr(g->ab, lc);

        g->cur = le; AT(g);
        return val_make(type_list(et), dst.v);
    }

    if (strcmp(bname, "reduce") == 0) {
        if (argc != 3)
            ERRV(g, "line %zu: reduce() takes exactly three arguments", n->line);
        Node *lambda = hof_lambda(g, n->as.call.args[0]);
        if (!lambda)
            ERRV(g, "line %zu: reduce() needs a lambda as its first argument",
                 n->line);
        if (lambda->as.funclit.arity != 2)
            ERRV(g, "line %zu: reduce()'s lambda takes (accumulator, item)",
                 n->line);
        Type *et = lambda->as.funclit.param_types && lambda->as.funclit.arity > 1
            ? lambda->as.funclit.param_types[1] : NULL;
        const char *atfn = list_at_fn(et);
        if (!atfn)
            ERRV(g, "line %zu: a list of this element type cannot be reduced",
                 n->line);

        Val fnv = cg_expr(g, n->as.call.args[0]);
        Val src = cg_expr(g, n->as.call.args[1]);
        Val acc0 = cg_expr(g, n->as.call.args[2]);
        if (!fnv.v || !src.v || !src.ty || src.ty->kind != TY_LIST ||
            !acc0.v || !acc0.ty || acc0.ty->kind == TY_STRUCT)
            ERRV(g, "line %zu: reduce() needs a list and a scalar or list "
                    "accumulator", n->line);
        LLVMTypeRef acc_ty = ty_of(g, acc0.ty);
        if (!acc_ty) ERRV(g, "line %zu: the accumulator has no codegen type",
                          n->line);

        /* The accumulator lives in a slot: every iteration loads it, calls
         * the closure with (acc, item) and stores the result back. */
        LLVMValueRef acc_slot = LLVMBuildAlloca(g->ab, acc_ty, "acc");
        {
            Type *p0 = lambda->as.funclit.param_types
                ? lambda->as.funclit.param_types[0] : NULL;
            Val acc_init = p0 ? coerce(g, p0, acc0, n->line) : acc0;
            if (!acc_init.v)
                ERRV(g, "line %zu: cannot seed the accumulator", n->line);
            LLVMBuildStore(g->ab, acc_init.v, acc_slot);
        }

        LLVMValueRef idx = LLVMBuildAlloca(g->ab, g->i64, "li");
        LLVMBuildStore(g->ab, LLVMConstInt(g->i64, 0, 0), idx);
        LLVMValueRef len_slot = LLVMBuildAlloca(g->ab, g->i64, "len");
        {
            LLVMTypeRef p1[1] = { g->i8ptr };
            LLVMValueRef v1[1] = { src.v };
            LLVMValueRef len0 = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i64, p1, 1, 0),
                rt_decl(g, "lume_list_len", g->i64, p1, 1), v1, 1, "len0");
            LLVMBuildStore(g->ab, len0, len_slot);
        }

        LLVMBasicBlockRef lc = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "rd.cond");
        LLVMBasicBlockRef lb = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "rd.body");
        LLVMBasicBlockRef li = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "rd.incr");
        LLVMBasicBlockRef le = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "rd.end");

        if (!DONE(g->cur)) LLVMBuildBr(g->ab, lc);
        g->cur = lc; AT(g);
        LLVMValueRef len = LLVMBuildLoad2(g->ab, g->i64, len_slot, "len");
        LLVMValueRef cur = LLVMBuildLoad2(g->ab, g->i64, idx, "cur");
        LLVMValueRef cond = LLVMBuildICmp(g->ab, LLVMIntSLT, cur, len, "c");
        LLVMBuildCondBr(g->ab, cond, lb, le);

        g->cur = lb; AT(g);
        Type *rt = et->kind == TY_BOOL ? type_prim(TY_INT) : et;
        LLVMTypeRef p2[2] = { g->i8ptr, g->i64 };
        LLVMValueRef v2[2] = { src.v, cur };
        Val e = rt_call(g, rt, atfn, 2, p2, v2);
        LLVMValueRef accv = LLVMBuildLoad2(g->ab, acc_ty, acc_slot, "acc");
        if (!e.v || !accv)
            ERRV(g, "line %zu: cannot read an element or the accumulator", n->line);
        Val argv[2] = { { accv, acc0.ty }, e };
        Type *p0 = lambda->as.funclit.param_types
            ? lambda->as.funclit.param_types[0] : NULL;
        Type *p1 = lambda->as.funclit.param_types && lambda->as.funclit.arity > 1
            ? lambda->as.funclit.param_types[1] : NULL;
        if (p0) argv[0] = coerce(g, p0, argv[0], n->line);
        if (p1) argv[1] = coerce(g, p1, argv[1], n->line);
        if (!argv[0].v || !argv[1].v)
            ERRV(g, "line %zu: cannot coerce the reduce arguments", n->line);
        /* cg_clo_invoke takes a bare LLVMValueRef array — the coerced Vals
         * must be unwrapped first (a Val* reinterpreted would hand the call
         * each Val's Type* as the argument value). */
        LLVMValueRef cargs[2] = { argv[0].v, argv[1].v };
        LLVMValueRef rv = cg_clo_invoke(g, lambda, fnv.v, cargs, 2);
        if (!rv) ERRV(g, "line %zu: cannot call the reducing closure", n->line);
        Val acc_next = val_make(acc0.ty, rv);
        acc_next = coerce(g, acc0.ty, acc_next, n->line);
        if (!acc_next.v)
            ERRV(g, "line %zu: the reducing closure's result does not fit "
                    "the accumulator", n->line);
        LLVMBuildStore(g->ab, acc_next.v, acc_slot);
        LLVMBuildBr(g->ab, li);

        g->cur = li; AT(g);
        LLVMValueRef nxt = LLVMBuildAdd(g->ab, cur, LLVMConstInt(g->i64, 1, 0), "nxt");
        LLVMBuildStore(g->ab, nxt, idx);
        LLVMBuildBr(g->ab, lc);

        g->cur = le; AT(g);
        LLVMValueRef res = LLVMBuildLoad2(g->ab, acc_ty, acc_slot, "res");
        if (!res) ERRV(g, "line %zu: cannot read the accumulator", n->line);
        return val_make(acc0.ty, res);
    }

    /* sort(l): same contract as the text emitter's copy — in-place, returns
     * l; the runtime helper (src/rt.c lume_list_sort) applies the
     * interpreter's homogeneity rule by scanning the element slots. */
    if (strcmp(bname, "sort") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: sort() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: sort() needs a list", n->line);
        LLVMTypeRef ats[1] = { g->i8ptr };
        LLVMValueRef vs[1] = { a.v };
        LLVMBuildCall2(g->ab,
            LLVMFunctionType(LLVMVoidTypeInContext(g->ctx), ats, 1, 0),
            rt_decl(g, "lume_list_sort", LLVMVoidTypeInContext(g->ctx), ats, 1),
            vs, 1, "");
        return a;   /* sorted in place; the same list is returned */
    }

    return (Val){ NULL, NULL };
}

/* Is *v* already an address rather than an aggregate value?
 *
 * A struct travels as a pointer, but which of the two shapes is in hand cannot
 * be read off the frontend type: `cg_var` hands back a variable's address,
 * `cg_struct_lit` hands back an aggregate insertvalue chain, and a call that
 * returns a struct hands back an aggregate. Pointers stopped being typed in
 * LLVM 15, so the frontend type cannot distinguish them either; the value's
 * own LLVM type still can — an aggregate is the struct type, an address is
 * always the bare `ptr`. Passing the wrong shape here is silent: opaque
 * pointers let the verifier accept `%Point**` where `%Point*` is expected and
 * the callee simply loads the pointer's bytes as the first field. */
static int struct_is_addr(LLVMValueRef v)
{
    return v && LLVMTypeOf(v) && LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMPointerTypeKind;
}

static Val cg_call(CG *g, Node *n)
{
    AT(g);

    /* run(): hand control to the native server loop (blocking; mirrors
     * codegen_call.c). */
    if (n->as.call.callee && n->as.call.callee->type == N_VAR &&
        strcmp(n->as.call.callee->as.var.name, "run") == 0) {
        if (n->as.call.argc != 0)
            ERRV(g, "line %zu: run() takes no arguments", n->line);
        LLVMBuildCall2(g->ab,
                       LLVMFunctionType(g->void_ty, NULL, 0, 0),
                       rt_decl(g, "lume_srv_run", g->void_ty, NULL, 0),
                       NULL, 0, "");
        return val_make(type_prim(TY_NULL), LLVMConstNull(g->i8ptr));
    }

    if (n->as.call.callee && n->as.call.callee->type == N_VAR &&
        strcmp(n->as.call.callee->as.var.name, "print") == 0) {
        if (n->as.call.argc != 1)
            ERRV(g, "line %zu: print() takes exactly one argument", n->line);
        return cg_print(g, n, cg_expr(g, n->as.call.args[0]));
    }

    /* Native server handler: get(req.query_params, key, default?) lowers to
     * lume_srv_qp with the handler's query FFI argument (mirrors
     * codegen_call.c's in-handler branch). */
    if (g->in_handler && n->as.call.callee &&
        n->as.call.callee->type == N_VAR &&
        strcmp(n->as.call.callee->as.var.name, "get") == 0) {
        Node *a0 = n->as.call.argc >= 1 ? n->as.call.args[0] : NULL;
        int qp = a0 && a0->type == N_MEMBER && a0->as.member.obj &&
                 a0->as.member.obj->type == N_VAR &&
                 strcmp(a0->as.member.obj->as.var.name, g->hreq) == 0 &&
                 strcmp(a0->as.member.name, "query_params") == 0;
        if (!qp)
            ERRV(g, "line %zu: native server: get() must be called on "
                    "req.query_params", n->line);
        if (n->as.call.argc < 2 || n->as.call.argc > 3)
            ERRV(g, "line %zu: get() takes (query_params, key, default?)",
                 n->line);
        Node *a1 = n->as.call.args[1];
        if (!(a1->type == N_LITERAL && a1->as.lit.kind == LIT_STR))
            ERRV(g, "line %zu: native server: get() key must be a string "
                    "literal", n->line);
        const char *kt = a1->as.lit.text;
        size_t kl = (size_t)a1->as.lit.len;
        if (kl >= 2 && kt[0] == '"' && kt[kl - 1] == '"') { kt++; kl -= 2; }
        LLVMValueRef key = cg_string_val(g, kt, (int)kl);
        LLVMValueRef dflt;
        if (n->as.call.argc == 3) {
            Node *a2 = n->as.call.args[2];
            if (!(a2->type == N_LITERAL && a2->as.lit.kind == LIT_STR))
                ERRV(g, "line %zu: native server: get() default must be a "
                        "string literal", n->line);
            const char *dt = a2->as.lit.text;
            size_t dl = (size_t)a2->as.lit.len;
            if (dl >= 2 && dt[0] == '"' && dt[dl - 1] == '"') { dt++; dl -= 2; }
            dflt = cg_string_val(g, dt, (int)dl);
        } else {
            dflt = LLVMConstNull(g->i8ptr);
        }
        /* query string = SrvReq field 2; reuse the struct registration from
         * cg_handler_req_field. */
        LLVMTypeRef srv = LLVMGetTypeByName(g->mod, "struct.SrvReq");
        if (!srv) {
            srv = LLVMStructCreateNamed(g->ctx, "struct.SrvReq");
            LLVMTypeRef els[6] = { g->i8ptr, g->i8ptr, g->i8ptr, g->i8ptr,
                                   g->i64, g->i8ptr };
            LLVMStructSetBody(srv, els, 6, 0);
        }
        Asg *req = asg_find(&g->locals, g->hreq, n->line);
        if (!req || !req->slot)
            ERRV(g, "line %zu: internal: req parameter missing", n->line);
        LLVMValueRef rp = LLVMBuildBitCast(g->ab, req->slot,
                                           LLVMPointerType(srv, 0), "rp");
        LLVMValueRef idxv[2] = { LLVMConstInt(g->i32, 0, 0),
                                 LLVMConstInt(g->i32, 2, 0) };
        LLVMValueRef gp = LLVMBuildInBoundsGEP2(g->ab, srv, rp, idxv, 2, "rq");
        LLVMValueRef q = LLVMBuildLoad2(g->ab, g->i8ptr, gp, "rqv");
        LLVMTypeRef qpt[3] = { g->i8ptr, g->i8ptr, g->i8ptr };
        LLVMValueRef qa[3] = { q, key, dflt };
        LLVMValueRef call = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i8ptr, qpt, 3, 0),
                rt_decl(g, "lume_srv_qp", g->i8ptr, qpt, 3),
                qa, 3, "qp");
        return val_make(type_prim(TY_STRING), call);
    }

    /* Native server handler: int(...) — string/number to i64 via atoi
     * (mirrors codegen_call.c's in-handler branch). */
    if (g->in_handler && n->as.call.callee &&
        n->as.call.callee->type == N_VAR &&
        strcmp(n->as.call.callee->as.var.name, "int") == 0) {
        if (n->as.call.argc != 1)
            ERRV(g, "line %zu: int() takes one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v) ERRV(g, "line %zu: bad int() operand", n->line);
        if (a.ty && a.ty->kind == TY_INT) return a;
        LLVMValueRef call = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i64, &g->i8ptr, 1, 0),
                rt_decl(g, "lume_srv_atoi", g->i64, &g->i8ptr, 1),
                &a.v, 1, "atoi");
        return val_make(type_prim(TY_INT), call);
    }

    Sig *s = NULL;
    const char *name = NULL;
    Node *callee = n->as.call.callee;
    if (callee && callee->type == N_MEMBER && callee->as.member.obj &&
        callee->as.member.obj->type == N_VAR) {
        /* ns member call: policy.decide(...) — the module signature injected
         * by codegen_infer_signatures_multi resolves under "<ns>.<name>"; the
         * call emits against the module function's own symbol (@L_<fname>),
         * which cg_function defines under its source name. */
        char qname[512];
        snprintf(qname, sizeof qname, "%s.%s",
                 callee->as.member.obj->as.var.name, callee->as.member.name);
        s = sig_find(&g->sigs, qname);
        if (!s)
            ERRV(g, "line %zu: call to unknown function '%s'", n->line, qname);
        name = s->node ? (s->node->as.func.cname ? s->node->as.func.cname
                                                 : s->node->as.func.name)
                       : qname;
    } else {
        if (!callee || callee->type != N_VAR)
            ERRV(g, "line %zu: only direct calls to named functions are supported", n->line);

        /* A closure-valued variable: `f(3)` where f was bound to a lambda. The
         * variable's TY_FUNC type carries the back-pointer to the lambda node
         * the signature pass pinned; a plain named-function reference has no
         * back-pointer and falls through to the sig_find path below. */
        {
            Type *ct = infer_node_type(g, callee);
            if (ct && ct->kind == TY_FUNC && ct->func &&
                ct->func->type == N_FUNC_LIT)
                return cg_clo_call(g, n, ct->func);
        }

        name = callee->as.var.name;
        s = sig_find(&g->sigs, name);
        if (!s) {
            /* Only after sig_find: a script that shadows a builtin with its own
             * function must still call the script's. */
            Val bv = cg_builtin(g, n);
            if (bv.v) return bv;
            ERRV(g, "line %zu: call to unknown function '%s'", n->line, name);
        }
    }
    if (n->as.call.argc != s->arity)
        ERRV(g, "line %zu: '%s' expects %d argument(s), got %d",
            n->line, name, s->arity, n->as.call.argc);

    LLVMTypeRef rty = ty_of(g, s->ret);
    /* Named struct returns need their aggregate defined; anonymous ones are
     * heap maps (i8*) and need nothing (mirrors codegen_call.c). */
    if (s->ret && s->ret->kind == TY_STRUCT && s->ret->name &&
        !sdef_find(&g->structs, s->ret->name))
        ERRV(g, "line %zu: '%s' returns a struct this backend cannot build", n->line, name);

    /* A call needs the callee's *symbol* to exist, and bodies are emitted in
     * source order: IR text declares by name the moment it sees a call, but
     * through the C API a forward call (the usual shape — main calling a
     * helper defined below it) has to create it here. cg_function adds the
     * same name and signature later, which returns this symbol and attaches
     * the body to it. */
    {
        char fname[160];
        snprintf(fname, sizeof fname, "L_%s", name);
        if (!LLVMGetNamedFunction(g->mod, fname)) {
            LLVMTypeRef *pts = (LLVMTypeRef *)xmalloc(
                sizeof(LLVMTypeRef) * (s->arity > 0 ? (size_t)s->arity : 1));
            for (int i = 0; i < s->arity; i++)
                pts[i] = ptr_of(g, s->params ? s->params[i] : NULL);
            LLVMTypeRef ft = LLVMFunctionType(rty ? rty : g->void_ty,
                                              s->arity > 0 ? pts : NULL,
                                              (unsigned)s->arity, 0);
            LLVMAddFunction(g->mod, fname, ft);
            free(pts);
        }
    }

    /* Arguments are evaluated first, so a type error cannot leave a half-built
     * call behind. */
    int argc = n->as.call.argc;
    LLVMValueRef *avs = (LLVMValueRef *)xmalloc(sizeof(LLVMValueRef) * (argc > 0 ? argc : 1));
    LLVMTypeRef *ats = (LLVMTypeRef *)xmalloc(sizeof(LLVMTypeRef) * (argc > 0 ? argc : 1));
    for (int i = 0; i < argc; i++) {
        /* Pin each argument's expected type to the parameter's, so a
         * struct/tuple literal in argument position resolves to the
         * parameter's declared type — the same rule the type checker applies
         * (ck_expr(arg, callee_t->types[i]) in typecheck_expr.c) and the same
         * pinning codegen_call.c's copy of this loop makes. Without this an
         * unannotated `(7, 9)` passed to a `T2` parameter would stay an
         * anonymous runtime map while the callee's body reads declared fields
         * off a %T2 aggregate. */
        Type *prev = g->expect;
        g->expect = s->params ? s->params[i] : NULL;
        Val a = cg_expr(g, n->as.call.args[i]);
        g->expect = prev;
        avs[i] = a.v;
        if (a.ty && a.ty->kind == TY_STRUCT && !struct_is_addr(a.v)) {
            /* An aggregate (an insertvalue chain) has to be materialised into
             * a stack slot before it can travel as a pointer. An address that
             * already is one — a struct variable — is passed straight through:
             * storing it into a slot again would build a `%Point**` and the
             * callee would read the pointer instead of the struct. */
            LLVMValueRef slot = LLVMBuildAlloca(g->ab, ty_of(g, a.ty), "sa");
            LLVMBuildStore(g->ab, a.v, slot);
            a.v    = slot;
            ats[i] = ptr_of(g, a.ty);
        } else {
            ats[i] = ptr_of(g, a.ty);
        }
        avs[i] = a.v;
        if (!ats[i]) ERRV(g, "line %zu: bad argument type for '%s'", n->line, name);
    }

    char fname[160];
    snprintf(fname, sizeof fname, "L_%s", name);
    LLVMValueRef fcallee = LLVMGetNamedFunction(g->mod, fname);
    if (!fcallee) ERRV(g, "line %zu: '%s' was never defined", n->line, name);

    /* The call type is rebuilt from the signature rather than read back off the
     * callee: LLVMGetElementType(LLVMTypeOf(callee)) no longer yields the
     * function type on LLVM ≥ 20 (opaque pointers), and a wrong call type is
     * reported by the verifier as "Incorrect number of arguments", which
     * points nowhere near the real mistake. Going through the same helpers
     * cg_function used guarantees the two agree. */
    if (s->arity > 64)
        ERRV(g, "line %zu: '%s' has too many parameters", n->line, name);
    LLVMTypeRef pt[65];
    for (int i = 0; i < s->arity; i++)
        pt[i] = ptr_of(g, s->params ? s->params[i] : NULL);
    LLVMTypeRef ft = LLVMFunctionType(rty ? rty : g->void_ty,
                                      s->arity > 0 ? pt : NULL,
                                      (unsigned)s->arity, 0);

    /* The result, not an undef: handing back an undef placeholder here is what
     * turns `n * fact(n - 1)` into `mul i64 %v, undef` and quietly produces
     * wrong numbers instead of a failed build. A void call still has a value
     * (the call instruction); its type is what tells callers to ignore it. */
    /* A void call must stay anonymous (the verifier rejects a call with a name
     * that returns nothing) and the name slot must not be NULL (this libLLVM
     * segfaults on it), so an empty string is the only safe spelling — see the
     * L_top call in llvm_codegen_module. */
    LLVMValueRef res = LLVMBuildCall2(g->ab, ft, fcallee, avs,
                                      (unsigned)argc, s->ret ? "c" : "");
    free(ats); free(avs);
    if (!res) ERRV(g, "line %zu: failed to emit the call to '%s'", n->line, name);
    /* Only the captured call survives past cg_function: it is the value the
     * synthetic top body returns, and from there the entry's exit status. */
    if (g->capture_main && rty && strcmp(name, "main") == 0)
        g->exit_val = res;

    /* `f()?` — see the matching block in cg_call() in codegen_expr.c for why
     * this has to branch rather than read `ok` directly. Kept in step with that
     * copy deliberately: the two emitters are separate implementations of one
     * lowering, and a change made to only one of them shows up as a backend
     * that accepts what the other rejects. */
    if (n->as.call.propagate) {
        Type *payload = s->ret ? s->ret->elem : NULL;
        if (!payload || payload->kind == TY_ANY)
            ERRV(g, "line %zu: '?' needs '%s' to return at least one "
                    "{ ok: <value> } whose type is known", n->line, name);
        const char *get_fn = map_get_fn(payload);
        if (!get_fn)
            ERRV(g, "line %zu: '?' cannot unwrap a payload of type '%s'",
                 n->line, src_type_name(payload));

        /* Same three-block shape the short-circuit and/or lowering uses: park
         * the payload in an alloca rather than a phi, which is what the rest of
         * this emitter does for every local. The `err` block returns the Result
         * unchanged -- `?` moves the failure out of this function, it does not
         * rewrite it -- and the join block reads the payload back. */
        LLVMTypeRef pty   = ty_of(g, payload);
        LLVMValueRef slot = LLVMBuildAlloca(g->ab, pty, "prop");

        LLVMBasicBlockRef bb_err = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "prop_err");
        LLVMBasicBlockRef bb_ok  = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "prop_ok");
        LLVMBasicBlockRef bb_end = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "prop_end");

        LLVMValueRef errk = cg_string_val(g, "err", 3);
        LLVMValueRef has  = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i64, (LLVMTypeRef[]){ g->i8ptr, g->i8ptr }, 2, 0),
                rt_decl(g, "lume_map_has", g->i64,
                        (LLVMTypeRef[]){ g->i8ptr, g->i8ptr }, 2),
                (LLVMValueRef[]){ res, errk }, 2, "has");
        LLVMValueRef cond = LLVMBuildICmp(g->ab, LLVMIntNE, has,
                                          LLVMConstInt(g->i64, 0, 0), "iserr");
        if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, cond, bb_err, bb_ok);

        g->cur = bb_err; AT(g);
        if (!DONE(g->cur)) LLVMBuildRet(g->ab, res);
        if (!DONE(g->cur)) LLVMBuildBr(g->ab, bb_end);

        g->cur = bb_ok; AT(g);
        LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, pty };
        LLVMValueRef dflt = payload->kind == TY_FLOAT ? LLVMConstReal(pty, 0.0)
                            : pty == g->i8ptr         ? LLVMConstNull(g->i8ptr)
                                                     : LLVMConstInt(pty, 0, 0);
        LLVMValueRef okk = cg_string_val(g, "ok", 2);
        LLVMValueRef pay = LLVMBuildCall2(g->ab,
                LLVMFunctionType(pty, ats, 3, 0),
                rt_decl(g, get_fn, pty, ats, 3),
                (LLVMValueRef[]){ res, okk, dflt }, 3, "p");
        if (!DONE(g->cur)) LLVMBuildStore(g->ab, pay, slot);
        if (!DONE(g->cur)) LLVMBuildBr(g->ab, bb_end);

        g->cur = bb_end; AT(g);
        return val_make(payload, LLVMBuildLoad2(g->ab, pty, slot, "pv"));
    }

    /* A call to a function with no return type yields null when its result
     * is used as a value (the interpreter's answer, and the text emitter's
     * — see cg_clo_call above). */
    if (!s->ret)
        return val_make(type_prim(TY_NULL), LLVMConstNull(g->i8ptr));
    return val_make(s->ret, res);
}

/* Returns the stored value: `p.x = e` is both a statement and an expression,
 * and the parser wraps a bare `p.x = e;` in an expr-stmt, so the store has to
 * be reachable from cg_expr too. */
static Val cg_assign_mem(CG *g, Node *n)
{
    AT(g);
    /* The node carries no type: the parser's assign_mem is only {obj, name,
     * value}, so the struct spelling comes from the object's own type.
     * Reading n->as.member.type here would read the value node through a
     * Type * and compare garbage against the struct names. */
    Val obj = cg_expr(g, n->as.assign_mem.obj);
    struct_addr(g, &obj);
    Type *st = obj.ty;
    if (!st || st->kind != TY_STRUCT || !st->name)
        ERRV(g, "line %zu: field '%s' on a non-struct value",
             n->line, n->as.assign_mem.name);
    int idx = struct_field_idx(g, st->name, n->as.assign_mem.name);
    if (idx < 0) ERRV(g, "line %zu: unknown field '%s' on '%s'",
                      n->line, n->as.assign_mem.name, st->name);

    Val val = cg_expr(g, n->as.assign_mem.value);

    Type *fty = struct_field_type(g, st->name, idx);
    if (!fty || !ty_of(g, fty))
        ERRV(g, "line %zu: field '%s' has no codegen type", n->line, n->as.assign_mem.name);

    LLVMValueRef p = gep_field(g, st, obj.v, idx);
    if (!p) ERRV(g, "line %zu: cannot take the address of '%s'", n->line, n->as.assign_mem.name);
    LLVMBuildStore(g->ab, val.v, p);
    return val;
}
