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

/* get(m, k[, default]) emission — the shared core of the get() branch in
 * cg_builtin and of the str()/int()/float()/stringify()/json() accessor
 * shorthand: the interpreter spells str(m, k[, d]) as str(get(m, k[, d]))
 * (builtins.c value_from_map), so the casts desugar to a get() read here
 * too. dnode == NULL is the two-argument form. Kept in step with the text
 * emitter's copy in codegen_call.c. */
static Val emit_get_call(CG *g, Node *mnode, Node *knode, Node *dnode,
                         size_t line)
{
    Val m = cg_expr(g, mnode);
    Val k = cg_expr(g, knode);
    Val d = dnode ? cg_expr(g, dnode) : (Val){ NULL, NULL };
    if (!m.v || !k.v)
        ERRV(g, "line %zu: get() needs a map, a key and a scalar default", line);

    /* The key's value shape, resolved while the receiver still carries its
     * declared field list: boxing a named struct into a map below keeps the
     * field names only at runtime, so the shape has to be read off the SDef
     * first. NULL for an any/list receiver (map_key_shape needs a struct). */
    Type *shape = map_key_shape(g, &m, knode);

    /* A named struct is a raw GEP struct with no runtime map header, so the
     * lume_map_get_* getters would read its first field as the header — a
     * type confusion that segfaulted both native backends on get(p, "f")
     * while the interpreter answered correctly. Box it into a real map
     * first, the conversion print()/str() already use for named structs. */
    if (m.ty && m.ty->name) {
        struct_addr(g, &m);
        Val mm = emit_struct_to_map(g, m.v, m.ty);
        if (!mm.v)
            ERRV(g, "line %zu: get() cannot read struct '%s'",
                 line, m.ty->name);
        m.v = mm.v;
        m.ty = mm.ty;
    }

    /* The flavour follows the *shape* of the key when the map's static
     * type declares it: the value at that key is always the declared
     * type. A default of a different type could only ever fire for an
     * absent key, so it is refused here rather than silently reading the
     * slot through the wrong getter. Without a shape the flavour follows
     * the *default*, and the int read when neither says. */
    const char *fn = "lume_map_get_i";
    Type    *rty = type_prim(TY_INT);
    if (m.ty && m.ty->kind == TY_ANY &&
        knode && knode->type == N_LITERAL && knode->as.lit.kind == LIT_NUM &&
        !knode->as.lit.is_float) {
        /* An any receiver with an integer key is a list-element read —
         * get(j, 0) on a json array, a str_match list. lume_slot_at takes
         * (slot, i64) and returns a boxed slot, so the read is always
         * `any`; the three-argument form is refused (the slot helper has
         * no absent-key fallback). Mirrors the text emitter's copy. */
        if (d.v)
            ERRV(g, "line %zu: get() on an any list with a default is not "
                    "supported on the native backends", line);
        LLVMTypeRef patA[2] = { g->i8ptr, g->i64 };
        LLVMValueRef rvA = LLVMBuildCall2(g->ab,
                            LLVMFunctionType(g->i8ptr, patA, 2, 0),
                            rt_decl(g, "lume_slot_at", g->i8ptr, patA, 2),
                            (LLVMValueRef[]){ m.v, k.v }, 2, "gl");
        return val_make(type_prim(TY_ANY), rvA);
    }
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
                     line, ty_str(d.ty));
            if (strcmp(sfn, "lume_map_get_i") == 0)      fn = "lume_slot_get_i";
            else if (strcmp(sfn, "lume_map_get_f") == 0) fn = "lume_slot_get_f";
            else if (strcmp(sfn, "lume_map_get_s") == 0) fn = "lume_slot_get_s";
            else if (strcmp(sfn, "lume_map_get_obj") == 0) fn = "lume_slot_get_obj";
            else
                ERRV(g, "line %zu: get() cannot read a %s off an any value",
                     line, ty_str(d.ty));
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
        /* A bare `list` annotation carries no element type: the read is
         * the dynamic slot path (lume_list_at_slot / any). The default
         * check below still sees the raw et (NULL skips it, as before). */
        const char *atfn = et ? list_at_fn(et) : "lume_list_at_slot";
        if (!atfn)
            ERRV(g, "line %zu: get() cannot index a list of %s",
                 line, et ? ty_str(et) : "unknown");
        if (d.v && d.ty && d.ty->kind != TY_ANY &&
            !(et && d.ty->kind == et->kind) &&
            !(d.ty->kind == TY_BOOL && et && et->kind == TY_INT))
            ERRV(g, "line %zu: get() default is %s but the list holds %s",
                 line, ty_str(d.ty), et ? ty_str(et) : "unknown");
        LLVMTypeRef rty_t = et && et->kind == TY_STRING ? g->i8ptr :
                            et && et->kind == TY_FLOAT ? g->dbl :
                            (!et || et->kind == TY_ANY) ? g->i8ptr : g->i64;
        Type *rty = et && et->kind != TY_ANY ? et : any_type();
        LLVMTypeRef pat[2] = { g->i8ptr, g->i64 };
        LLVMValueRef rv = LLVMBuildCall2(g->ab,
                            LLVMFunctionType(rty_t, pat, 2, 0),
                            rt_decl(g, atfn, rty_t, pat, 2),
                            (LLVMValueRef[]){ m.v, k.v }, 2, "gl");
        return val_make(rty, rv);
    } else {
        if (shape && shape->kind != TY_ANY) {
            const char *sfn; Type *srty;
            if (!get_flavour(shape, &sfn, &srty))
                ERRV(g, "line %zu: get() cannot read a value of type %s",
                     line, ty_str(shape));
            if (d.v && d.ty && d.ty->kind != TY_ANY && d.ty->kind != shape->kind &&
                !(d.ty->kind == TY_BOOL && shape->kind == TY_INT) &&
                !(d.ty->kind == TY_INT && shape->kind == TY_BOOL))
                ERRV(g, "line %zu: get() default is %s but key %s holds %s",
                     line, ty_str(d.ty), knode->as.lit.text, ty_str(shape));
            fn = sfn; rty = srty;
        } else if (m.ty && m.ty->kind == TY_STRUCT && !m.ty->name &&
                   struct_common_shape(m.ty) == NULL && !d.v) {
            /* Undeclared struct receiver (a list-of-maps literal element,
             * a str_match row, a json-parsed row) with no default: no static
             * field the typed getters could read and no default to infer the
             * flavour from, so this is the interpreter's dynamic read. Hand
             * back a boxed slot (any) -- lume_map_get_slot -- the same boxed
             * representation for-in and get-on-any use. When a default IS
             * present the next branch picks the typed getter by the default's
             * type, which is the correct (and verifier-clean) path for a
             * literal struct like `{ "count": 7 }` whose anon type carries no
             * field names natively. */
            fn = "lume_map_get_slot";
            rty = type_prim(TY_ANY);
        } else if (d.v && d.ty && d.ty->kind != TY_ANY) {
            const char *dfn; Type *drty;
            if (get_flavour(d.ty, &dfn, &drty)) { fn = dfn; rty = drty; }
        }
    }
    if (d.v && d.ty && d.ty->kind == TY_BOOL)
        d = coerce(g, type_prim(TY_INT), d, line);

    /* Always three operands: the runtime getters are declared with the
     * default parameter, and a two-operand call against that declaration
     * left the default reading stack garbage — an absent key then handed
     * that garbage back (printed as, e.g., 73896). */
    LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, ty_of(g, rty) };
    LLVMValueRef vs[3] = { m.v, k.v,
                           d.v ? d.v : get_zero_default(g, rty) };
    Val r = rt_call(g, rty, fn, 3, ats, vs);
    if (!r.v) ERRV(g, "line %zu: get() failed", line);
    return r;
}

/* Convert an accessor's default expression to the cast's own type —
 * the same lowerings the direct casts build below (int("12x") parses a
 * leading number, float(3) widens, str(3) renders). The interpreter
 * hands the default back verbatim — whatever type it has — but the
 * static accessor answers in the cast's type, so the default crosses
 * the same bridge the cast would build. A bool default widens
 * numerically (the getters' bool defaults always did); a null default
 * is treated as absent, since null is not representable in a scalar
 * cast type and the two-argument form's absent-key answer is the
 * closest static answer. Kept in step with the text emitter's copy. */
static Val acc_default(CG *g, Type *rty, Val d, size_t line)
{
    if (!d.v) return d;
    if (d.ty && d.ty->kind == TY_NULL) return (Val){ NULL, NULL };
    /* A bool default travels as the interpreter's bool representation,
     * i64 — widen it before the same-kind shortcut below can pass an
     * i1 through to a helper that takes i64. */
    if (d.ty && d.ty->kind == TY_BOOL && rty->kind == TY_BOOL)
        return coerce(g, type_prim(TY_INT), d, line);
    if (!d.ty || d.ty->kind == rty->kind) return d;

    if (d.ty->kind == TY_BOOL &&
        (rty->kind == TY_INT || rty->kind == TY_FLOAT || rty->kind == TY_BOOL)) {
        Val i = coerce(g, type_prim(TY_INT), d, line);
        if (!i.v || rty->kind != TY_FLOAT) return i;
        return coerce(g, type_prim(TY_FLOAT), i, line);
    }

    const char *name = NULL;
    LLVMTypeRef pty = NULL, rt = NULL;
    switch (rty->kind) {
    case TY_INT:
        if (d.ty->kind == TY_STRING)     { name = "lume_bi_parse_i64";    pty = g->i8ptr; rt = g->i64; }
        else if (d.ty->kind == TY_FLOAT) { name = "lume_bi_int";          pty = g->dbl;   rt = g->i64; }
        break;
    case TY_FLOAT:
        if (d.ty->kind == TY_STRING)     { name = "lume_bi_parse_double"; pty = g->i8ptr; rt = g->dbl; }
        else if (d.ty->kind == TY_INT)   { name = "lume_bi_float";        pty = g->i64;   rt = g->dbl; }
        break;
    case TY_STRING:
        if (d.ty->kind == TY_INT)        { name = "lume_bi_str_i64";      pty = g->i64;   rt = g->i8ptr; }
        else if (d.ty->kind == TY_FLOAT) { name = "lume_bi_str_double";   pty = g->dbl;   rt = g->i8ptr; }
        else if (d.ty->kind == TY_BOOL)  { name = "lume_bi_str_bool";     pty = g->i64;   rt = g->i8ptr; }
        else if (d.ty->kind == TY_ANY)   { name = "lume_slot_str_scalar"; pty = g->i8ptr; rt = g->i8ptr; }
        else if (d.ty->kind == TY_LIST ||
                 (d.ty->kind == TY_STRUCT && !d.ty->name))
                                        { name = "lume_rt_to_json_obj";  pty = g->i8ptr; rt = g->i8ptr; }
        break;
    case TY_BOOL:
        /* A bool accessor's default crosses by its own truthiness — the
         * same helpers the direct bool() cast dispatches to (value.c
         * value_truthy), answering in the interpreter's bool
         * representation (i64). */
        if (d.ty->kind == TY_INT)        { name = "lume_bi_bool_i";      pty = g->i64;   rt = g->i64; }
        else if (d.ty->kind == TY_STRING){ name = "lume_bi_bool_s";      pty = g->i8ptr; rt = g->i64; }
        else if (d.ty->kind == TY_FLOAT) { name = "lume_bi_bool_f";      pty = g->dbl;   rt = g->i64; }
        else if (d.ty->kind == TY_ANY)   { name = "lume_bi_bool_slot";   pty = g->i8ptr; rt = g->i64; }
        else if (d.ty->kind == TY_LIST ||
                 (d.ty->kind == TY_STRUCT && !d.ty->name))
                                        { name = "lume_bi_bool_obj";    pty = g->i8ptr; rt = g->i64; }
        break;
    default: break;
    }
    if (!name)
        ERRV(g, "line %zu: accessor default cannot convert to %s",
             line, src_type_name(rty));

    LLVMValueRef v[1] = { d.v };
    if (d.ty->kind == TY_BOOL && pty == g->i64)
        v[0] = LLVMBuildZExt(g->ab, d.v, g->i64, "boolarg");
    LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(rt, &pty, 1, 0),
                                    rt_decl(g, name, rt, &pty, 1), v, 1, "b");
    if (!r) ERRV(g, "line %zu: accessor default failed to lower", line);
    return val_make(rty, r);
}

/* str/int/float/bool accessor emission: int(m, "k"[, d]) and friends —
 * the interpreter's value_from_map + native_int/native_float/
 * native_str/native_bool (src/builtins.c), where the answer is ALWAYS
 * the cast's own type no matter what the field holds or what the
 * default is. That is why this exists next to emit_get_call instead of
 * routing through it: there, a default whose type differs from the
 * key's declared shape is refused (a get() default of the wrong type
 * would read the slot through the wrong getter), but for an accessor a
 * mismatched default is the normal case — `int(acc, "n", 0)` reading
 * the string "42" is the point of the sugar. The runtime helpers
 * lume_map_acc_* / lume_slot_acc_* (src/rt.c) carry the interpreter's
 * whole table, so neither the field's static type nor the default's
 * type picks the flavour here. Kept in step with the text emitter's
 * copy in codegen_call.c. */
static Val emit_acc_call(CG *g, Node *mnode, Node *knode, Node *dnode,
                         bool is_str, bool is_float, bool is_bool,
                         bool is_any, size_t line)
{
    Val m = cg_expr(g, mnode);
    Val k = cg_expr(g, knode);
    Val d = dnode ? cg_expr(g, dnode) : (Val){ NULL, NULL };
    if (!m.v || !k.v)
        ERRV(g, "line %zu: accessor needs a map and a key", line);

    /* A named struct is a raw GEP struct with no runtime map header, so
     * the helper would read its first field as the header — the same
     * boxing get() does before its getters run. An any receiver is
     * already the boxed slot the slot helpers unpack. */
    if (!is_any && m.ty && m.ty->name) {
        struct_addr(g, &m);
        Val mm = emit_struct_to_map(g, m.v, m.ty);
        if (!mm.v)
            ERRV(g, "line %zu: accessor cannot read struct '%s'",
                 line, m.ty->name);
        m.v = mm.v;
        m.ty = mm.ty;
    }

    /* The helpers answer the cast's own type; bool answers the
     * interpreter's bool representation (i64), narrowed to i1 below
     * like the direct bool() cast does. */
    Type *rty = is_str ? type_prim(TY_STRING)
             : is_float ? type_prim(TY_FLOAT)
                        : type_prim(TY_INT);
    const char *fn = is_str ? (is_any ? "lume_slot_acc_s" : "lume_map_acc_s")
                   : is_float ? (is_any ? "lume_slot_acc_f" : "lume_map_acc_f")
                   : is_bool ? (is_any ? "lume_slot_acc_b" : "lume_map_acc_b")
                             : (is_any ? "lume_slot_acc_i" : "lume_map_acc_i");

    if (d.v) d = acc_default(g, is_bool ? type_prim(TY_BOOL) : rty, d, line);
    if (g->err[0]) return (Val){ NULL, NULL };

    LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, ty_of(g, rty) };
    LLVMValueRef vs[3] = { m.v, k.v, d.v ? d.v : get_zero_default(g, rty) };
    Val r = rt_call(g, rty, fn, 3, ats, vs);
    if (!r.v) ERRV(g, "line %zu: accessor failed", line);
    if (is_bool) {
        LLVMValueRef b = LLVMBuildICmp(g->ab, LLVMIntNE, r.v,
                                       LLVMConstInt(g->i64, 0, 0), "accb");
        if (!b) ERRV(g, "line %zu: accessor failed", line);
        return val_make(type_prim(TY_BOOL), b);
    }
    return r;
}

static Val cg_builtin(CG *g, Node *n)
{
    const char *bname = n->as.call.callee->as.var.name;
    int argc = n->as.call.argc;
    const Bi *b = builtin_find(bname);
    AT(g);

    /* argv(): the script's trailing CLI arguments as a string list. Zero
     * arguments, so the table path — which requires argc >= 1 — cannot serve
     * it; the list the runtime hands back is stashed by lume_cli_init in the
     * generated entry point (see llvm_codegen.c). Mirrors the text backend's
     * argv() handling in codegen_call.c. */
    if (strcmp(bname, "argv") == 0) {
        if (argc != 0)
            ERRV(g, "line %zu: argv() takes no arguments", n->line);
        LLVMTypeRef av_ft = LLVMFunctionType(g->i8ptr, NULL, 0, 0);
        LLVMValueRef r = LLVMBuildCall2(g->ab, av_ft,
                                        rt_decl(g, "lume_bi_argv", g->i8ptr, NULL, 0),
                                        NULL, 0, "b");
        return val_make(list_type(), r);
    }

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

    /* Monte Carlo RNG (2026-10-24, mirroring the text backend's branches):
     * deterministic PCG32 in rt.c; ints pass straight through, floats
     * widen/truncate through lume_bi_float / lume_bi_ftoi exactly as the
     * interpreter truncates its double arguments. */
    if (strcmp(bname, "seed") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: seed() takes exactly one argument", n->line);
        Val s = cg_expr(g, n->as.call.args[0]);
        if (!s.v || !s.ty || (s.ty->kind != TY_INT && s.ty->kind != TY_FLOAT))
            ERRV(g, "line %zu: seed() needs a number", n->line);
        if (s.ty->kind == TY_FLOAT) {
            LLVMTypeRef p1[1] = { g->dbl };
            LLVMValueRef v1[1] = { s.v };
            s.v = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i64, p1, 1, 0),
                rt_decl(g, "lume_bi_ftoi", g->i64, p1, 1), v1, 1, "si");
        }
        LLVMTypeRef p1[1] = { g->i64 };
        LLVMValueRef v1[1] = { s.v };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->i64, p1, 1, 0),
            rt_decl(g, "lume_bi_seed", g->i64, p1, 1), v1, 1, "seed");
        return val_make(type_prim(TY_INT), r);
    }
    if (strcmp(bname, "rand") == 0) {
        if (argc != 0)
            ERRV(g, "line %zu: rand() takes no arguments", n->line);
        LLVMValueRef r = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->dbl, NULL, 0, 0),
            rt_decl(g, "lume_bi_rand", g->dbl, NULL, 0), NULL, 0, "rnd");
        return val_make(type_prim(TY_FLOAT), r);
    }
    if (strcmp(bname, "rand_range") == 0 || strcmp(bname, "rand_normal") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: %s() takes two numbers", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !a.ty || (a.ty->kind != TY_INT && a.ty->kind != TY_FLOAT) ||
            !b.v || !b.ty || (b.ty->kind != TY_INT && b.ty->kind != TY_FLOAT))
            ERRV(g, "line %zu: %s() needs two numbers", n->line, bname);
        if (a.ty->kind == TY_INT) {
            LLVMTypeRef p1[1] = { g->i64 };
            LLVMValueRef v1[1] = { a.v };
            a.v = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->dbl, p1, 1, 0),
                rt_decl(g, "lume_bi_float", g->dbl, p1, 1), v1, 1, "pf");
        }
        if (b.ty->kind == TY_INT) {
            LLVMTypeRef p1[1] = { g->i64 };
            LLVMValueRef v1[1] = { b.v };
            b.v = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->dbl, p1, 1, 0),
                rt_decl(g, "lume_bi_float", g->dbl, p1, 1), v1, 1, "pf");
        }
        const char *fn = strcmp(bname, "rand_range") == 0
                             ? "lume_bi_rand_range" : "lume_bi_rand_normal";
        LLVMTypeRef p2[2] = { g->dbl, g->dbl };
        LLVMValueRef v2[2] = { a.v, b.v };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->dbl, p2, 2, 0),
            rt_decl(g, fn, g->dbl, p2, 2), v2, 2, "rnd");
        return val_make(type_prim(TY_FLOAT), r);
    }
    if (strcmp(bname, "rand_int") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: rand_int() takes two numbers", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !a.ty || (a.ty->kind != TY_INT && a.ty->kind != TY_FLOAT) ||
            !b.v || !b.ty || (b.ty->kind != TY_INT && b.ty->kind != TY_FLOAT))
            ERRV(g, "line %zu: rand_int() needs two numbers", n->line);
        if (a.ty->kind == TY_FLOAT) {
            LLVMTypeRef p1[1] = { g->dbl };
            LLVMValueRef v1[1] = { a.v };
            a.v = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i64, p1, 1, 0),
                rt_decl(g, "lume_bi_ftoi", g->i64, p1, 1), v1, 1, "ai");
        }
        if (b.ty->kind == TY_FLOAT) {
            LLVMTypeRef p1[1] = { g->dbl };
            LLVMValueRef v1[1] = { b.v };
            b.v = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i64, p1, 1, 0),
                rt_decl(g, "lume_bi_ftoi", g->i64, p1, 1), v1, 1, "bi");
        }
        LLVMTypeRef p2[2] = { g->i64, g->i64 };
        LLVMValueRef v2[2] = { a.v, b.v };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->i64, p2, 2, 0),
            rt_decl(g, "lume_bi_rand_int", g->i64, p2, 2), v2, 2, "ri");
        return val_make(type_prim(TY_INT), r);
    }
    /* Distribution family (2026-10-25, mirroring the text backend): the one
     * numeric builtins widen an int via lume_bi_float; shuffle passes the
     * list through as i8*. */
    if (strcmp(bname, "rand_bool") == 0 || strcmp(bname, "rand_exponential") == 0 ||
        strcmp(bname, "rand_poisson") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: %s() takes one number", n->line, bname);
        Val p = cg_expr(g, n->as.call.args[0]);
        if (!p.v || !p.ty || (p.ty->kind != TY_INT && p.ty->kind != TY_FLOAT))
            ERRV(g, "line %zu: %s() needs a number", n->line, bname);
        if (p.ty->kind == TY_INT) {
            LLVMTypeRef p1[1] = { g->i64 };
            LLVMValueRef v1[1] = { p.v };
            p.v = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->dbl, p1, 1, 0),
                rt_decl(g, "lume_bi_float", g->dbl, p1, 1), v1, 1, "pf");
        }
        int is_exp = strcmp(bname, "rand_exponential") == 0;
        const char *fn = is_exp ? "lume_bi_rand_exponential"
                      : strcmp(bname, "rand_bool") == 0 ? "lume_bi_rand_bool"
                      : "lume_bi_rand_poisson";
        LLVMTypeRef p1[1] = { g->dbl };
        LLVMValueRef v1[1] = { p.v };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
            LLVMFunctionType(is_exp ? g->dbl : g->i64, p1, 1, 0),
            rt_decl(g, fn, is_exp ? g->dbl : g->i64, p1, 1), v1, 1, "rd");
        return val_make(is_exp ? type_prim(TY_FLOAT) : type_prim(TY_INT), r);
    }
    if (strcmp(bname, "shuffle") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: shuffle() takes one list", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: shuffle() needs a list", n->line);
        LLVMTypeRef p1[1] = { g->i8ptr };
        LLVMValueRef v1[1] = { a.v };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->i8ptr, p1, 1, 0),
            rt_decl(g, "lume_bi_shuffle", g->i8ptr, p1, 1), v1, 1, "q");
        return val_make(type_list(type_prim(TY_FLOAT)), r);
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
        if (!a.v) return (Val){ NULL, NULL };
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
        if (!a.v || !b.v) return (Val){ NULL, NULL };
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
        if (!a.v || !m.v) return (Val){ NULL, NULL };
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

#if LUME_HAS_SQLITE
    /* SQLite (SPEC 6.4, P6 three-backend parity) — mirror of the text
     * backend's branches; the native runtime is the same bridge_native.c.
     * sql_query(sql[, params]) / sql_query(path, sql[, params]) decided by
     * arg 1's static type; a null i8* stands in for the absent db/params. */
    if (strcmp(bname, "sql_query") == 0 || strcmp(bname, "sql_write") == 0) {
        int isq = strcmp(bname, "sql_query") == 0;
        if (argc < 1 || argc > 3)
            ERRV(g, "line %zu: %s() takes 1 to 3 arguments", n->line, bname);
        Val *av = (Val *)xmalloc((size_t)argc * sizeof *av);
        for (int i = 0; i < argc; i++) av[i] = cg_expr(g, n->as.call.args[i]);
        int bad = 0;
        for (int i = 0; i < argc; i++) if (!av[i].v) bad = 1;
        if (bad) {
            free(av);
            return (Val){ NULL, NULL };
        }
        LLVMValueRef dbv = LLVMConstNull(g->i8ptr);
        LLVMValueRef sqlv, pv = LLVMConstNull(g->i8ptr);
        if (argc == 1) {
            sqlv = av[0].v;
        } else if (argc == 2) {
            if (av[1].ty && av[1].ty->kind == TY_STRING) {
                dbv = av[0].v;
                sqlv = av[1].v;
            } else {
                sqlv = av[0].v;
                pv = av[1].v;
            }
        } else {
            dbv = av[0].v;
            sqlv = av[1].v;
            pv = av[2].v;
        }
        LLVMTypeRef pp[3] = { g->i8ptr, g->i8ptr, g->i8ptr };
        LLVMTypeRef rty = isq ? g->i8ptr : g->i64;
        LLVMValueRef r = LLVMBuildCall2(g->ab,
                                        LLVMFunctionType(rty, pp, 3, 0),
                                        rt_decl(g, isq ? "lume_bi_sql_query"
                                                        : "lume_bi_sql_write",
                                               rty, pp, 3),
                                        (LLVMValueRef[]){ dbv, sqlv, pv }, 3,
                                        isq ? "sq" : "sw");
        free(av);
        return val_make(isq ? list_type() : type_prim(TY_INT), r);
    }
    if (strcmp(bname, "sql_tx_begin") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: sql_tx_begin() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v) return (Val){ NULL, NULL };
        LLVMTypeRef pp[1] = { g->i8ptr };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
                                        LLVMFunctionType(g->i8ptr, pp, 1, 0),
                                        rt_decl(g, "lume_bi_sql_tx_begin",
                                               g->i8ptr, pp, 1),
                                        (LLVMValueRef[]){ a.v }, 1, "txb");
        return val_make(type_prim(TY_STRING), r);
    }
    if (strcmp(bname, "sql_tx_exec") == 0) {
        if (argc < 2 || argc > 3)
            ERRV(g, "line %zu: sql_tx_exec() takes a handle, SQL and optional params",
                 n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        Val c = argc == 3 ? cg_expr(g, n->as.call.args[2])
                          : (Val){ NULL, NULL };
        if (!a.v || !b.v || (argc == 3 && !c.v)) {
            return (Val){ NULL, NULL };
        }
        LLVMTypeRef pp[3] = { g->i8ptr, g->i8ptr, g->i8ptr };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
                                        LLVMFunctionType(g->i64, pp, 3, 0),
                                        rt_decl(g, "lume_bi_sql_tx_exec",
                                               g->i64, pp, 3),
                                        (LLVMValueRef[]){ a.v, b.v,
                                                          argc == 3 ? c.v
                                                                    : LLVMConstNull(g->i8ptr) },
                                        3, "txe");
        return val_make(type_prim(TY_INT), r);
    }
    if (strcmp(bname, "sql_tx_commit") == 0 || strcmp(bname, "sql_tx_rollback") == 0) {
        int isc = strcmp(bname, "sql_tx_commit") == 0;
        if (argc != 1)
            ERRV(g, "line %zu: %s() takes exactly one argument", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v) return (Val){ NULL, NULL };
        LLVMTypeRef pp[1] = { g->i8ptr };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
                                        LLVMFunctionType(g->i64, pp, 1, 0),
                                        rt_decl(g, isc ? "lume_bi_sql_tx_commit"
                                                       : "lume_bi_sql_tx_rollback",
                                               g->i64, pp, 1),
                                        (LLVMValueRef[]){ a.v }, 1,
                                        isc ? "txc" : "txr");
        /* bool like the interpreter (print -> lume_print_bool), and every
         * other bool consumer expects an i1 — the helper returns i64, so
         * truncate to i1. */
        LLVMValueRef t = LLVMBuildTrunc(g->ab, r, g->i1,
                                        isc ? "txcb" : "txrb");
        return val_make(type_prim(TY_BOOL), t);
    }
#endif /* LUME_HAS_SQLITE */

    /* str() / int() / float(): which helper they need depends on the operand.
     * `string` is the interpreter's direct alias for str (b_string forwards to
     * native_str), so it takes the same branch and the same accessor desugar. */
    if (strcmp(bname, "str") == 0 || strcmp(bname, "string") == 0 ||
        strcmp(bname, "int") == 0 ||
        strcmp(bname, "float") == 0 || strcmp(bname, "stringify") == 0 ||
        strcmp(bname, "json") == 0) {
        if (argc < 1)
            ERRV(g, "line %zu: '%s' needs an argument", n->line, bname);
        /* str(m, k[, d]) is the interpreter's accessor shorthand —
         * value_from_map + the cast (src/builtins.c) — but only in the
         * shape value_from_map accepts: a map receiver and a string
         * key. Anything else keeps the interpreter's fallback
         * (serialize args[0]; the extra arguments are ignored there),
         * so the desugar fires only when the static types promise
         * that shape. Only str/int/float take the accessor:
         * stringify/json look at args[0] alone and ignore the rest
         * (native_stringify / native_json). Both receiver shapes go
         * to emit_acc_call — the accessor answers in the cast's own
         * type whatever the field holds, so neither the field's
         * declared shape nor the default's type may pick the flavour;
         * routing through emit_get_call instead would refuse
         * `int(acc, "n", 0)` on a string field, the sugar's primary
         * use. */
        bool is_str = strcmp(bname, "str") == 0 ||
                      strcmp(bname, "string") == 0;
        bool is_json = strcmp(bname, "json") == 0;
        Type *a0t = infer_node_type(g, n->as.call.args[0]);
        Type *a1t = argc >= 2 ? infer_node_type(g, n->as.call.args[1]) : NULL;
        bool accessor = argc >= 2 && a0t && a1t && a1t->kind == TY_STRING &&
                        (a0t->kind == TY_STRUCT || a0t->kind == TY_ANY) &&
                        (is_str || strcmp(bname, "int") == 0 ||
                         strcmp(bname, "float") == 0);
        if (accessor) {
            bool is_flt = strcmp(bname, "float") == 0;
            return emit_acc_call(g, n->as.call.args[0], n->as.call.args[1],
                                 argc == 3 ? n->as.call.args[2] : NULL,
                                 is_str, is_flt, false, a0t->kind == TY_ANY,
                                 n->line);
        }
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v)
            ERRV(g, "line %zu: '%s' failed to lower", n->line, bname);
        LLVMTypeRef ft = NULL, pty = NULL, rty = NULL;
        Type *rt = NULL;

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
            /* Identity on an int; a float truncates; a string parses a
             * leading number like the interpreter's atoll on the direct
             * cast path ("12x" -> 12, "abc" -> 0). */
            if (a.ty && a.ty->kind == TY_INT) return a;
            if (a.ty && a.ty->kind == TY_STRING) {
                ft = LLVMFunctionType(g->i64, &g->i8ptr, 1, 0); pty = g->i8ptr; rty = g->i64;
                rt = type_prim(TY_INT);
            } else if (a.ty && a.ty->kind == TY_FLOAT) {
                ft = LLVMFunctionType(g->i64, &g->dbl, 1, 0); pty = g->dbl; rty = g->i64;
                rt = type_prim(TY_INT);
            } else {
                ERRV(g, "line %zu: int() only converts a float or a string here", n->line);
            }
        } else {
            if (a.ty && a.ty->kind == TY_FLOAT) return a;
            if (a.ty && a.ty->kind == TY_STRING) {
                ft = LLVMFunctionType(g->dbl, &g->i8ptr, 1, 0); pty = g->i8ptr; rty = g->dbl;
                rt = type_prim(TY_FLOAT);
            } else if (a.ty && a.ty->kind == TY_INT) {
                ft = LLVMFunctionType(g->dbl, &g->i64, 1, 0); pty = g->i64; rty = g->dbl;
                rt = type_prim(TY_FLOAT);
            } else {
                ERRV(g, "line %zu: float() only converts an int or a string here", n->line);
            }
        }

        const char *name;
        if (a.ty && a.ty->kind == TY_STRING &&
            (strcmp(bname, "int") == 0 || strcmp(bname, "float") == 0))
            /* int("12x") / float("1.5x") parse a leading number (atoll/
             * strtod), the interpreter's direct cast path ("12x" -> 12,
             * "abc" -> 0). */
            name = strcmp(bname, "int") == 0 ? "lume_bi_parse_i64"
                                             : "lume_bi_parse_double";
        else if (a.ty && a.ty->kind == TY_ANY)
            /* str() of a string slot is the bare string, stringify() keeps the
             * JSON quotes (rt.c lume_slot_str_scalar / lume_slot_str) — the
             * text backend's TY_ANY rule, mirrored. `string` is str's alias. */
            name = is_str ? "lume_slot_str_scalar" : "lume_slot_str";
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
        return emit_get_call(g, n->as.call.args[0], n->as.call.args[1],
                             argc == 3 ? n->as.call.args[2] : NULL, n->line);
    }

    /* ---- string tools (interpreter parity, builtins_str.c) ----
     * String parameters keep every one of these out of the numeric
     * BUILTINS table (that path coerces its arguments to i64/double), so
     * each gets its own branch, the same shape read_file/env() use. The
     * helpers live in src/rt.c and mirror the interpreter's native_*
     * contracts: ASCII-only case mapping, byte-based split/substr, a
     * 16 MiB result ceiling on replace/join, and str_match's ERE
     * findall -> [{text, index, groups}]. Mirrors the text backend. */
    if (strcmp(bname, "upper") == 0 || strcmp(bname, "lower") == 0 ||
        strcmp(bname, "capitalize") == 0 || strcmp(bname, "trim") == 0) {
        if (argc < 1)
            ERRV(g, "line %zu: %s() needs a string", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_STRING)
            ERRV(g, "line %zu: %s() needs a string", n->line, bname);
        const char *fn = strcmp(bname, "upper") == 0 ? "lume_bi_upper"
                       : strcmp(bname, "lower") == 0 ? "lume_bi_lower"
                       : strcmp(bname, "capitalize") == 0
                             ? "lume_bi_capitalize" : "lume_bi_trim";
        LLVMTypeRef p1[1] = { g->i8ptr };
        LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, p1, 1, 0),
                                        rt_decl(g, fn, g->i8ptr, p1, 1),
                                        (LLVMValueRef[]){ a.v }, 1, "s");
        if (!r) ERRV(g, "line %zu: %s() failed", n->line, bname);
        return val_make(type_prim(TY_STRING), r);
    }

    if (strcmp(bname, "contains") == 0) {
        if (argc < 2)
            ERRV(g, "line %zu: contains() needs (s, sub)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !b.v || !a.ty || !b.ty ||
            a.ty->kind != TY_STRING || b.ty->kind != TY_STRING)
            ERRV(g, "line %zu: contains() needs two strings", n->line);
        LLVMTypeRef p2[2] = { g->i8ptr, g->i8ptr };
        LLVMValueRef iv = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i64, p2, 2, 0),
                                         rt_decl(g, "lume_bi_contains", g->i64, p2, 2),
                                         (LLVMValueRef[]){ a.v, b.v }, 2, "c");
        if (!iv) ERRV(g, "line %zu: contains() failed", n->line);
        LLVMValueRef bv = LLVMBuildTrunc(g->ab, iv, g->i1, "cb");
        return val_make(type_prim(TY_BOOL), bv);
    }

    if (strcmp(bname, "split") == 0) {
        if (argc < 2)
            ERRV(g, "line %zu: split() needs (s, sep)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !b.v || !a.ty || !b.ty ||
            a.ty->kind != TY_STRING || b.ty->kind != TY_STRING)
            ERRV(g, "line %zu: split() needs two strings", n->line);
        LLVMTypeRef p2[2] = { g->i8ptr, g->i8ptr };
        LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, p2, 2, 0),
                                        rt_decl(g, "lume_bi_split", g->i8ptr, p2, 2),
                                        (LLVMValueRef[]){ a.v, b.v }, 2, "sp");
        if (!r) ERRV(g, "line %zu: split() failed", n->line);
        return val_make(list_type(), r);
    }

    if (strcmp(bname, "join") == 0) {
        if (argc < 2)
            ERRV(g, "line %zu: join() needs (list, sep)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !b.v || !a.ty || !b.ty ||
            a.ty->kind != TY_LIST || b.ty->kind != TY_STRING)
            ERRV(g, "line %zu: join() needs a list and a string", n->line);
        LLVMTypeRef p2[2] = { g->i8ptr, g->i8ptr };
        LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, p2, 2, 0),
                                        rt_decl(g, "lume_bi_join", g->i8ptr, p2, 2),
                                        (LLVMValueRef[]){ a.v, b.v }, 2, "jn");
        if (!r) ERRV(g, "line %zu: join() failed", n->line);
        return val_make(type_prim(TY_STRING), r);
    }

    if (strcmp(bname, "replace") == 0) {
        if (argc < 3)
            ERRV(g, "line %zu: replace() needs (s, from, to)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        Val c = cg_expr(g, n->as.call.args[2]);
        if (!a.v || !b.v || !c.v || !a.ty || !b.ty || !c.ty ||
            a.ty->kind != TY_STRING || b.ty->kind != TY_STRING ||
            c.ty->kind != TY_STRING)
            ERRV(g, "line %zu: replace() needs three strings", n->line);
        LLVMTypeRef p3[3] = { g->i8ptr, g->i8ptr, g->i8ptr };
        LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, p3, 3, 0),
                                        rt_decl(g, "lume_bi_replace", g->i8ptr, p3, 3),
                                        (LLVMValueRef[]){ a.v, b.v, c.v }, 3, "rp");
        if (!r) ERRV(g, "line %zu: replace() failed", n->line);
        return val_make(type_prim(TY_STRING), r);
    }

    if (strcmp(bname, "substr") == 0) {
        if (argc < 2 || argc > 3)
            ERRV(g, "line %zu: substr() needs (s, start[, len])", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        Val c = argc == 3 ? cg_expr(g, n->as.call.args[2])
                          : (Val){ NULL, NULL };
        if (!a.v || !b.v || !a.ty || !b.ty ||
            a.ty->kind != TY_STRING || b.ty->kind != TY_INT ||
            (argc == 3 && (!c.v || !c.ty || c.ty->kind != TY_INT)))
            ERRV(g, "line %zu: substr() needs a string and integer bounds", n->line);
        LLVMTypeRef p3[3] = { g->i8ptr, g->i64, g->i64 };
        LLVMValueRef cv = argc == 3 ? c.v : LLVMConstInt(g->i64, -1, 1);
        LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, p3, 3, 0),
                                        rt_decl(g, "lume_bi_substr", g->i8ptr, p3, 3),
                                        (LLVMValueRef[]){ a.v, b.v, cv }, 3, "sub");
        if (!r) ERRV(g, "line %zu: substr() failed", n->line);
        return val_make(type_prim(TY_STRING), r);
    }

    if (strcmp(bname, "str_match") == 0 || strcmp(bname, "regex") == 0) {
        if (argc < 2)
            ERRV(g, "line %zu: %s() needs (pattern, text)", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !b.v || !a.ty || !b.ty ||
            a.ty->kind != TY_STRING || b.ty->kind != TY_STRING)
            ERRV(g, "line %zu: %s() needs two strings", n->line, bname);
        LLVMTypeRef p2[2] = { g->i8ptr, g->i8ptr };
        LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, p2, 2, 0),
                                        rt_decl(g, "lume_bi_str_match", g->i8ptr, p2, 2),
                                        (LLVMValueRef[]){ a.v, b.v }, 2, "sm");
        if (!r) ERRV(g, "line %zu: %s() failed", n->line, bname);
        return val_make(list_type(), r);
    }

    if (strcmp(bname, "exp") == 0 || strcmp(bname, "log") == 0 ||
        strcmp(bname, "ln") == 0) {
        if (argc < 1)
            ERRV(g, "line %zu: %s() needs a number", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty ||
            (a.ty->kind != TY_INT && a.ty->kind != TY_FLOAT))
            ERRV(g, "line %zu: %s() needs a number", n->line, bname);
        a = coerce(g, type_prim(TY_FLOAT), a, n->line);
        if (!a.v) ERRV(g, "line %zu: %s() failed to lower", n->line, bname);
        const char *fn = strcmp(bname, "exp") == 0 ? "lume_bi_exp"
                                                   : "lume_bi_log";
        LLVMTypeRef p1[1] = { g->dbl };
        LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(g->dbl, p1, 1, 0),
                                        rt_decl(g, fn, g->dbl, p1, 1),
                                        (LLVMValueRef[]){ a.v }, 1, "e");
        if (!r) ERRV(g, "line %zu: %s() failed", n->line, bname);
        return val_make(type_prim(TY_FLOAT), r);
    }

    /* Statistical CDFs / special functions: normcdf/erf/gamma/lgamma take
     * one double; igamma/tdist_cdf/chisq_cdf take two; ibeta takes three.
     * Same helpers the interpreter uses (rt.c lume_bi_*); kept in step with
     * codegen_call.c's Bi table and the inference lists in codegen_scan.c /
     * llvm_cg_scan.inc. */
    {
        static const struct {
            const char *name;
            const char *fn;
            int arity;
        } stats[] = {
            {"normcdf", "lume_bi_normcdf", 1},
            {"erf",     "lume_bi_erf",     1},
            {"gamma",   "lume_bi_gamma",   1},
            {"lgamma",  "lume_bi_lgamma",  1},
            {"igamma",  "lume_bi_igamma",  2},
            {"ibeta",   "lume_bi_ibeta",   3},
            {"tdist_cdf", "lume_bi_tdist_cdf", 2},
            {"chisq_cdf", "lume_bi_chisq_cdf", 2},
        };
        for (size_t i = 0; i < sizeof(stats) / sizeof(stats[0]); i++) {
            if (strcmp(bname, stats[i].name) == 0) {
                int na = stats[i].arity;
                if (argc != na)
                    ERRV(g, "line %zu: %s() needs %d argument(s)", n->line, bname, na);
                LLVMValueRef av[3];
                LLVMTypeRef pt[3];
                for (int k = 0; k < na; k++) {
                    Val a = cg_expr(g, n->as.call.args[k]);
                    if (!a.v || !a.ty ||
                        (a.ty->kind != TY_INT && a.ty->kind != TY_FLOAT))
                        ERRV(g, "line %zu: %s() needs a number", n->line, bname);
                    a = coerce(g, type_prim(TY_FLOAT), a, n->line);
                    if (!a.v)
                        ERRV(g, "line %zu: %s() failed to lower", n->line, bname);
                    av[k] = a.v;
                    pt[k] = g->dbl;
                }
                LLVMValueRef r = LLVMBuildCall2(g->ab,
                    LLVMFunctionType(g->dbl, pt, na, 0),
                    rt_decl(g, stats[i].fn, g->dbl, pt, na),
                    av, na, "s");
                if (!r) ERRV(g, "line %zu: %s() failed", n->line, bname);
                return val_make(type_prim(TY_FLOAT), r);
            }
        }
    }

    /* bool(x) — value_truthy by static type. An any value dispatches on its
     * runtime tag (lume_bi_bool_slot); bool(m, "k"[, d]) is the same
     * accessor desugar the casts take — the interpreter checks
     * value_from_map first (native_bool, builtins.c) and answers the
     * truthiness of the found value, or of the default when the key is
     * absent. A named struct is boxed first. */
    if (strcmp(bname, "bool") == 0) {
        if (argc < 1)
            ERRV(g, "line %zu: bool() needs an argument", n->line);
        Type *a0t = infer_node_type(g, n->as.call.args[0]);
        Type *a1t = argc >= 2 ? infer_node_type(g, n->as.call.args[1]) : NULL;
        int accessor = argc >= 2 && a0t && a1t && a1t->kind == TY_STRING &&
                        (a0t->kind == TY_STRUCT || a0t->kind == TY_ANY);
        if (accessor) {
            return emit_acc_call(g, n->as.call.args[0], n->as.call.args[1],
                                 argc == 3 ? n->as.call.args[2] : NULL,
                                 false, false, true, a0t->kind == TY_ANY,
                                 n->line);
        }
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v)
            ERRV(g, "line %zu: bool() failed to lower", n->line);
        if (a.ty && a.ty->kind == TY_BOOL) return a;
        if (a.ty && a.ty->kind == TY_NULL)
            return val_make(type_prim(TY_BOOL), LLVMConstInt(g->i1, 0, 0));
        const char *fn = NULL;
        LLVMTypeRef pt = NULL;
        switch (a.ty ? a.ty->kind : TY_ANY) {
        case TY_INT:    fn = "lume_bi_bool_i";    pt = g->i64;    break;
        case TY_FLOAT:  fn = "lume_bi_bool_f";    pt = g->dbl;    break;
        case TY_STRING: fn = "lume_bi_bool_s";    pt = g->i8ptr;  break;
        case TY_LIST:   fn = "lume_bi_bool_obj";  pt = g->i8ptr;  break;
        case TY_STRUCT: fn = "lume_bi_bool_obj";  pt = g->i8ptr;  break;
        case TY_ANY:    fn = "lume_bi_bool_slot"; pt = g->i8ptr;  break;
        default:
            ERRV(g, "line %zu: bool() cannot convert this value", n->line);
        }
        if (a.ty && a.ty->name) {
            struct_addr(g, &a);
            Val m = emit_struct_to_map(g, a.v, a.ty);
            if (!m.v)
                ERRV(g, "line %zu: bool() cannot convert struct '%s'",
                     n->line, a.ty->name);
            a.v = m.v;
            a.ty = m.ty;
        }
        LLVMTypeRef p1[1] = { pt };
        LLVMValueRef iv = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i64, p1, 1, 0),
                                         rt_decl(g, fn, g->i64, p1, 1),
                                         (LLVMValueRef[]){ a.v }, 1, "b");
        if (!iv) ERRV(g, "line %zu: bool() failed", n->line);
        LLVMValueRef bv = LLVMBuildTrunc(g->ab, iv, g->i1, "bb");
        return val_make(type_prim(TY_BOOL), bv);
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

    /* Quant builtins (2026-10-24): same contract as the text emitter's
     * copy — descriptive statistics, covariance / correlation / linear
     * regression and the time-series helpers, all list-argument, so like
     * sort() they live outside the numeric BUILTINS table. The helpers
     * (src/rt.c lume_bi_*) mirror the interpreter's native_* in
     * builtins_math.c. */
    if (strcmp(bname, "mean") == 0 || strcmp(bname, "variance") == 0 ||
        strcmp(bname, "stddev") == 0 || strcmp(bname, "median") == 0 ||
        strcmp(bname, "max_drawdown") == 0 || strcmp(bname, "skew") == 0 ||
        strcmp(bname, "kurtosis") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: %s() takes exactly one argument", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: %s() needs a list", n->line, bname);
        const char *fn = strcmp(bname, "mean") == 0 ? "lume_bi_mean"
                       : strcmp(bname, "variance") == 0 ? "lume_bi_variance"
                       : strcmp(bname, "stddev") == 0 ? "lume_bi_stddev"
                       : strcmp(bname, "median") == 0 ? "lume_bi_median"
                       : strcmp(bname, "max_drawdown") == 0 ? "lume_bi_max_drawdown"
                       : strcmp(bname, "skew") == 0 ? "lume_bi_skew"
                       : "lume_bi_kurtosis";
        LLVMTypeRef p[1] = { g->i8ptr };
        LLVMValueRef vs[1] = { a.v };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->dbl, p, 1, 0),
            rt_decl(g, fn, g->dbl, p, 1), vs, 1, "q");
        return val_make(type_prim(TY_FLOAT), r);
    }

    if (strcmp(bname, "percentile") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: percentile() takes (list, p)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: percentile() needs a list", n->line);
        Val p = cg_expr(g, n->as.call.args[1]);
        if (!p.v || !p.ty || (p.ty->kind != TY_INT && p.ty->kind != TY_FLOAT))
            ERRV(g, "line %zu: percentile() needs a number for p", n->line);
        if (p.ty->kind == TY_INT) {
            LLVMTypeRef p1[1] = { g->i64 };
            LLVMValueRef v1[1] = { p.v };
            p.v = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->dbl, p1, 1, 0),
                rt_decl(g, "lume_bi_float", g->dbl, p1, 1), v1, 1, "pf");
        }
        LLVMTypeRef p2[2] = { g->i8ptr, g->dbl };
        LLVMValueRef v2[2] = { a.v, p.v };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->dbl, p2, 2, 0),
            rt_decl(g, "lume_bi_percentile", g->dbl, p2, 2), v2, 2, "q");
        return val_make(type_prim(TY_FLOAT), r);
    }

    if (strcmp(bname, "covariance") == 0 || strcmp(bname, "correlation") == 0 ||
        strcmp(bname, "linreg") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: %s() takes (x, y)", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: %s() needs a list", n->line, bname);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!b.v || !b.ty || b.ty->kind != TY_LIST)
            ERRV(g, "line %zu: %s() needs a list", n->line, bname);
        LLVMTypeRef p2[2] = { g->i8ptr, g->i8ptr };
        LLVMValueRef v2[2] = { a.v, b.v };
        if (strcmp(bname, "linreg") == 0) {
            LLVMValueRef r = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i8ptr, p2, 2, 0),
                rt_decl(g, "lume_bi_linreg", g->i8ptr, p2, 2), v2, 2, "q");
            return val_make(type_anon_struct(), r);
        }
        const char *fn = strcmp(bname, "covariance") == 0
                             ? "lume_bi_covariance" : "lume_bi_correlation";
        LLVMValueRef r = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->dbl, p2, 2, 0),
            rt_decl(g, fn, g->dbl, p2, 2), v2, 2, "q");
        return val_make(type_prim(TY_FLOAT), r);
    }

    if (strcmp(bname, "pct_change") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: pct_change() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: pct_change() needs a list", n->line);
        LLVMTypeRef p[1] = { g->i8ptr };
        LLVMValueRef vs[1] = { a.v };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->i8ptr, p, 1, 0),
            rt_decl(g, "lume_bi_pct_change", g->i8ptr, p, 1), vs, 1, "q");
        return val_make(type_list(type_prim(TY_FLOAT)), r);
    }

    /* rolling_mean/rolling_stddev (list, window) and ewma (list, alpha):
     * one list plus one scalar, returning a fresh float list. The window
     * is an integer semantic: an int passes straight through as i64, a
     * float truncates via lume_bi_ftoi — the same truncation the
     * interpreter applies to its double window argument. */
    if (strcmp(bname, "rolling_mean") == 0 || strcmp(bname, "rolling_stddev") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: %s() takes (list, window)", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: %s() needs a list", n->line, bname);
        Val w = cg_expr(g, n->as.call.args[1]);
        if (!w.v || !w.ty || (w.ty->kind != TY_INT && w.ty->kind != TY_FLOAT)) {
            ERRV(g, "line %zu: %s() needs a window size", n->line, bname);
        }
        if (w.ty->kind == TY_FLOAT) {
            LLVMTypeRef p1[1] = { g->dbl };
            LLVMValueRef v1[1] = { w.v };
            w.v = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i64, p1, 1, 0),
                rt_decl(g, "lume_bi_ftoi", g->i64, p1, 1), v1, 1, "wi");
        }
        const char *fn = strcmp(bname, "rolling_mean") == 0
                             ? "lume_bi_rolling_mean" : "lume_bi_rolling_stddev";
        LLVMTypeRef p2[2] = { g->i8ptr, g->i64 };
        LLVMValueRef v2[2] = { a.v, w.v };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->i8ptr, p2, 2, 0),
            rt_decl(g, fn, g->i8ptr, p2, 2), v2, 2, "q");
        return val_make(type_list(type_prim(TY_FLOAT)), r);
    }

    if (strcmp(bname, "ewma") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: ewma() takes (list, alpha)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: ewma() needs a list", n->line);
        Val p = cg_expr(g, n->as.call.args[1]);
        if (!p.v || !p.ty || (p.ty->kind != TY_INT && p.ty->kind != TY_FLOAT)) {
            ERRV(g, "line %zu: ewma() needs a number for alpha", n->line);
        }
        if (p.ty->kind == TY_INT) {
            LLVMTypeRef p1[1] = { g->i64 };
            LLVMValueRef v1[1] = { p.v };
            p.v = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->dbl, p1, 1, 0),
                rt_decl(g, "lume_bi_float", g->dbl, p1, 1), v1, 1, "pf");
        }
        LLVMTypeRef p2[2] = { g->i8ptr, g->dbl };
        LLVMValueRef v2[2] = { a.v, p.v };
        LLVMValueRef r = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->i8ptr, p2, 2, 0),
            rt_decl(g, "lume_bi_ewma", g->i8ptr, p2, 2), v2, 2, "q");
        return val_make(type_list(type_prim(TY_FLOAT)), r);
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
        s = NULL;
        if (g->cur_ns) {
            /* Inside a module function, a plain call spells the bare name but
             * the multi-file signature table registered it under "<ns>.<name>"
             * (see llvm_cg_emit_ir_multi pass 1); resolve the module key first,
             * then fall back to the bare name (entry functions / builtins). */
            char qname[512];
            snprintf(qname, sizeof qname, "%s.%s", g->cur_ns, name);
            s = sig_find(&g->sigs, qname);
        }
        if (!s) s = sig_find(&g->sigs, name);
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
        if (snprintf(fname, sizeof fname, "L_%s", name) >= (int)sizeof fname)
            ERRV(g, "line %zu: function name too long for symbol: '%s'", n->line, name);
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
    if (snprintf(fname, sizeof fname, "L_%s", name) >= (int)sizeof fname)
        ERRV(g, "line %zu: function name too long for symbol: '%s'", n->line, name);
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
