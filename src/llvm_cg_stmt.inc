/* llvm_cg_stmt.inc - LLVM backend stmt functions.
 * Textually included by llvm_codegen.c (single translation unit).
 * Do not compile this file on its own. */

/* Match-arm binds, for the window their arm body's type is inferred in.
 * Mirrors codegen_scan.c's copy: a repeated name overwrites (no new
 * strdup), distinct names are strdup'd once and freed by arm_scope_clear
 * when the outermost match is done. */
static void arm_scope_push(CG *g, const char *name, Type *ty)
{
    if (!name || !ty) return;
    for (int i = 0; i < g->arm_scope.n; i++)
        if (strcmp(g->arm_scope.v[i].name, name) == 0) {
            g->arm_scope.v[i].ty = ty;
            return;
        }
    if (g->arm_scope.n == g->arm_scope.cap) {
        g->arm_scope.cap = g->arm_scope.cap ? g->arm_scope.cap * 2 : 8;
        g->arm_scope.v = (LTyp *)xrealloc(g->arm_scope.v,
                                          (size_t)g->arm_scope.cap * sizeof *g->arm_scope.v);
    }
    g->arm_scope.v[g->arm_scope.n].name = arena_strdup(name);
    g->arm_scope.v[g->arm_scope.n].ty   = ty;
    g->arm_scope.n++;
}

static void arm_scope_clear(CG *g)
{
    /* names are arena memory now (TODO.md P2 Stage 3): only the malloc'd
     * vector shell dies here. */
    free(g->arm_scope.v);
    g->arm_scope = (LTys){0};
}

/* LLVM twin of codegen_scan.c's arm_scope_truncate: resetting the arm-bind
 * scope between match arms. This used to free the discarded entries'
 * xstrdup'd names (they leaked when only the truncated table was cleared);
 * the names are arena memory now, so the reset is a bare count assignment
 * and the arena reclaims everything at end-of-emission. */
static void arm_scope_truncate(CG *g, int n)
{
    if (n < 0) n = 0;
    g->arm_scope.n = n;
}

/* The type of a `for (x in xs)` loop variable: a list yields its element type,
 * a map yields strings (it iterates its keys), anything else is an error --
 * a value is just an i8* here, so which reader a for-in has to call is a
 * static question and `list<any>` (a heterogeneous literal, say) has none. */
static Type *for_in_elem_type(CG *g, Node *n)
{
    Type *t = infer_node_type(g, n->as.fors.iterable);
    if (t && t->kind == TY_LIST) {
        if (!t->elem)
            ERR(g, "line %zu: cannot infer what a list of this type holds to "
                   "iterate it (annotate the list)", n->line);
        return t->elem;
    }
    /* An anonymous struct literal is a runtime map, which iterates its keys. */
    if (t && t->kind == TY_STRUCT && !t->name) return type_prim(TY_STRING);
    /* An `any` iterable (a json() array) iterates its runtime elements as
     * tagged slots; the loop variable is `any`. */
    if (t && t->kind == TY_ANY) return any_type();
    ERR(g, "line %zu: the native backend can only iterate a list or a map in "
           "a 'for ... in' loop", n->line);
    return NULL;   /* the checker is never the one that reads this path */
}

/* Branch unless the current block already ended. This is what makes
 * `if (c) { return 1; }` legal without a trailing `unreachable`. */
static void br_to(CG *g, LLVMBasicBlockRef target)
{
    if (!DONE(g->cur)) LLVMBuildBr(g->ab, target);
}

static void cg_while(CG *g, Node *n)
{
    AT(g);
    LLVMBasicBlockRef c = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "while.cond");
    LLVMBasicBlockRef b = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "while.body");
    LLVMBasicBlockRef e = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "while.end");

    push_bb(&g->brk, &g->nbrk, &g->cbrk, e);
    push_bb(&g->cnt, &g->ncnt, &g->ccnt, c);

    if (!DONE(g->cur)) LLVMBuildBr(g->ab, c);

    g->cur = c; AT(g);
    /* The condition sits in the enclosing scope: the body's shadow `let`s
     * (wh_saved marks where they start, recorded at scan time) must not
     * capture its variable reads — interp reads the outer binding at
     * condition time. Hide them for the condition, restore for the body. */
    {
        int full_n = g->locals.n;
        if (g->wh_idx < g->wh_cap)
            g->locals.n = g->wh_saved[g->wh_idx];
        g->wh_idx++;
        LLVMValueRef cond = cg_expr(g, n->as.whiles.cond).v;
        g->locals.n = full_n;
        if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, cond, b, e);
    }

    g->cur = b; AT(g);
    cg_stmt(g, n->as.whiles.body);
    br_to(g, c);

    g->cur = e; AT(g);
    g->nbrk--; g->ncnt--;
}

/* `for (x in xs)` / `for (k in m)`: the loop variable is an index into a heap
 * object reached through an opaque pointer, read through the accessor the
 * static element type picked. The iterable is evaluated once, but the length
 * is re-read every iteration — that is what the interpreter's for-in does and
 * what makes a `push(xs, v)` inside the body visible to the loop. */
static void cg_for_in(CG *g, Node *n)
{
    AT(g);
    Val it = cg_expr(g, n->as.fors.iterable);
    if (!it.v) ERRX(g, "line %zu: bad iterable", n->line);

    /* Only a map iterates its keys, so that spelling is the test. */
    bool is_map = it.ty && it.ty->kind == TY_STRUCT && !it.ty->name;

    Asg *a = asg_find(&g->locals, n->as.fors.var, n->line);
    if (!a) ERRX(g, "line %zu: unknown variable '%s'", n->line, n->as.fors.var);
    Type *et = a->ty;
    /* An `any` element (a list that decayed to list<any>, e.g. [null, 2, null])
     * is boxed as a LumeSlot*; read it back through lume_list_at_slot, which
     * returns the slot for ANY tag. lume_list_at_obj only serves LUME_SLOT_OBJ.
     * ty_of(g, TY_ANY) returns i8*, so the loop variable's alloca and the
     * store below stay consistent. */
    const char *fn = is_map ? NULL
        : (it.ty && it.ty->kind == TY_ANY ? "lume_slot_at"
          : (et->kind == TY_ANY ? "lume_list_at_slot" : list_at_fn(et)));
    if (!is_map && !fn)
        ERRX(g, "line %zu: a list of this element type cannot be iterated", n->line);

    LLVMValueRef idx = LLVMBuildAlloca(g->ab, g->i64, "li");
    LLVMBuildStore(g->ab, LLVMConstInt(g->i64, 0, 0), idx);

    /* Snapshot the collection length once, before the loop, to match the
     * interpreter (which captures list.count at loop entry). Re-reading it
     * every iteration made `push(xs, v)` inside the body extend the bound
     * forever and OOM the backend. */
    LLVMValueRef len_slot = LLVMBuildAlloca(g->ab, g->i64, "len");
    const char *lenfn = is_map ? "lume_map_len"
                       : (it.ty && it.ty->kind == TY_ANY ? "lume_slot_len"
                                                         : "lume_list_len");
    LLVMValueRef len0 = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i64, &g->i8ptr, 1, 0),
                                       rt_decl(g, lenfn, g->i64, &g->i8ptr, 1),
                                       &it.v, 1, "len0");
    LLVMBuildStore(g->ab, len0, len_slot);

    LLVMBasicBlockRef c = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.cond");
    LLVMBasicBlockRef b = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.body");
    LLVMBasicBlockRef i = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.incr");
    LLVMBasicBlockRef e = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.end");

    push_bb(&g->brk, &g->nbrk, &g->cbrk, e);
    /* `continue` has to reach the increment block, not the condition: jumping
     * back to `c` would skip the index bump and spin forever on element 0.
     * The incr block ends with a branch to `c`, so the re-test still runs. */
    push_bb(&g->cnt, &g->ncnt, &g->ccnt, i);

    if (!DONE(g->cur)) LLVMBuildBr(g->ab, c);

    g->cur = c; AT(g);
    LLVMValueRef len = LLVMBuildLoad2(g->ab, g->i64, len_slot, "len");
    LLVMValueRef cur = LLVMBuildLoad2(g->ab, g->i64, idx, "cur");
    LLVMValueRef cond = LLVMBuildICmp(g->ab, LLVMIntSLT, cur, len, "c");
    if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, cond, b, e);

    g->cur = b; AT(g);
    LLVMValueRef arg[2] = { it.v, cur };
    /* The accessor is declared with its real signature rather than through
     * rt_call, because the result type differs per element kind. */
    {
        LLVMTypeRef pts[2] = { g->i8ptr, g->i64 };
        Type *rt = et;
        if (is_map) rt = type_prim(TY_STRING);
        else if (rt->kind == TY_BOOL) rt = type_prim(TY_INT);   /* narrowed below */
        LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(ty_of(g, rt), pts, 2, 0),
                                        rt_decl(g, is_map ? "lume_map_key_at" : fn,
                                                ty_of(g, rt), pts, 2),
                                        arg, 2, "e");
        LLVMValueRef store = r;
        if (et->kind == TY_BOOL) store = LLVMBuildICmp(g->ab, LLVMIntNE,
                                                        r, LLVMConstInt(g->i64, 0, 0), "n");
        LLVMBuildStore(g->ab, store, a->slot);
    }
    cg_stmt(g, n->as.fors.body);
    br_to(g, i);

    g->cur = i; AT(g);
    LLVMValueRef nxt = LLVMBuildAdd(g->ab, cur, LLVMConstInt(g->i64, 1, 0), "nxt");
    LLVMBuildStore(g->ab, nxt, idx);
    br_to(g, c);

    g->cur = e; AT(g);
    g->nbrk--; g->ncnt--;
}

static void cg_for(CG *g, Node *n)
{
    AT(g);
    if (n->as.fors.is_in) { cg_for_in(g, n); return; }

    if (n->as.fors.init) cg_stmt(g, n->as.fors.init);

    LLVMBasicBlockRef c = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.cond");
    LLVMBasicBlockRef b = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.body");
    LLVMBasicBlockRef i = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.incr");
    LLVMBasicBlockRef e = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.end");

    push_bb(&g->brk, &g->nbrk, &g->cbrk, e);
    /* Same rule as the `for ... in` loop above: `continue` must land on the
     * increment block, or the loop variable never advances. */
    push_bb(&g->cnt, &g->ncnt, &g->ccnt, i);

    if (!DONE(g->cur)) LLVMBuildBr(g->ab, c);

    g->cur = c; AT(g);
    if (n->as.fors.cond) {
        LLVMValueRef cond = cg_expr(g, n->as.fors.cond).v;
        if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, cond, b, e);
    } else if (!DONE(g->cur)) {
        LLVMBuildBr(g->ab, b);
    }

    g->cur = b; AT(g);
    cg_stmt(g, n->as.fors.body);
    br_to(g, i);

    g->cur = i; AT(g);
    if (n->as.fors.incr) {
        /* The increment is a *statement*: `k = k + 1` is an assignment node
         * wrapped in an expression statement. */
        if (n->as.fors.incr->type == N_ASSIGN || n->as.fors.incr->type == N_EXPR_STMT)
            cg_stmt(g, n->as.fors.incr);
        else
            (void)cg_expr(g, n->as.fors.incr);
    }
    br_to(g, c);

    g->cur = e; AT(g);
    g->nbrk--; g->ncnt--;
}

static void cg_if(CG *g, Node *n)
{
    AT(g);
    LLVMBasicBlockRef t = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "if.then");
    LLVMBasicBlockRef f = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "if.else");
    LLVMBasicBlockRef e = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "if.end");

    LLVMValueRef cond = cg_expr(g, n->as.ifs.cond).v;
    if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, cond, t, f);

    g->cur = t; AT(g);
    cg_stmt(g, n->as.ifs.then);
    br_to(g, e);

    g->cur = f; AT(g);
    if (n->as.ifs.els) cg_stmt(g, n->as.ifs.els);
    br_to(g, e);

    g->cur = e; AT(g);
}

static void cg_stmt(CG *g, Node *n)
{
    if (!n) return;

    switch (n->type) {
    case N_LET: {
        AT(g);
        /* Hand the annotation to struct literals before evaluating them. */
        Type *prev = g->expect;
        g->expect = n->as.let.annot;
        Val v = cg_expr(g, n->as.let.init);
        g->expect = prev;

        /* Prefer the annotation so the stored value matches the slot. */
        Type *ty = n->as.let.annot ? n->as.let.annot : v.ty;
        v = coerce(g, ty, v, n->line);
        if (g->err[0]) break;   /* coerce refused (e.g. null into a scalar) */
        if (!ty_of(g, ty))
            ERRX(g, "line %zu: cannot store this into a typed local", n->line);

        /* A named struct may arrive as an address (a variable read or a field
         * load) rather than an aggregate value (a call or a literal), but the
         * slot holds the aggregate — load it first, exactly as a struct
         * literal's field does (llvm_cg_lit.c's struct_is_addr test). Without
         * this, `let inner = o.inner` and `let o2 = o` store pointer bits
         * into a struct slot and the next read sees garbage. */
        if (ty->kind == TY_STRUCT && ty->name && struct_is_addr(v.v))
            v.v = LLVMBuildLoad2(g->ab, ty_of(g, ty), v.v, "ld");

        Asg *a = asg_find(&g->locals, n->as.let.name, n->line);
        if (!a) ERRX(g, "internal: '%s' was not allocated by the scan pass",
                     n->as.let.name);
        if (!a->slot) a->slot = LLVMBuildAlloca(g->ab, ty_of(g, ty), "lv");
        LLVMBuildStore(g->ab, v.v, a->slot);
        break;
    }

    case N_ASSIGN:
        (void)cg_assign_expr(g, n);
        break;

    case N_LET_PAT:  cg_let_pat(g, n); break;

    case N_ASSIGN_MEMBER:
        (void)cg_assign_mem(g, n);
        break;

    case N_RETURN: {
        AT(g);
        /* Native server route handler: `return` serialises into the FFI
         * response pointers instead of returning a value (mirrors
         * codegen_stmt.c's cg_handler_ret). */
        if (g->in_handler) { cg_handler_ret(g, n); break; }
        if (!n->as.ret.expr) {
            if (!DONE(g->cur)) LLVMBuildRetVoid(g->ab);
            break;
        }
        /* `return { .. }` needs the function's declared return type. Pin the
         * expect type the way the text emitter's copy (codegen_stmt.c) and the
         * checker (ck_stmt's return pass) both do, so a struct/tuple literal
         * in return position resolves to the declared return type instead of
         * staying anonymous. */
        Type *prev_exp = g->expect;
        g->expect = g->cur_ret;
        Val v = cg_expr(g, n->as.ret.expr);
        g->expect = prev_exp;
        LLVMTypeRef rty = ty_of(g, g->cur_ret);
        if (!rty) rty = ty_of(g, v.ty);
        if (!rty) ERRX(g, "line %zu: cannot return this value", n->line);
        if (rty == g->void_ty) { if (!DONE(g->cur)) LLVMBuildRetVoid(g->ab); break; }
        LLVMValueRef val = v.v;
        if (v.ty && g->cur_ret && v.ty != g->cur_ret) {
            val = coerce(g, g->cur_ret, v, n->line).v;
            if (!val) ERRX(g, "line %zu: cannot return this value", n->line);
        }
        if (!DONE(g->cur)) LLVMBuildRet(g->ab, val);
        break;
    }

    case N_IF:      cg_if(g, n); break;
    case N_IF_LET:  cg_if_let(g, n); break;
    case N_WHILE:   cg_while(g, n); break;
    case N_FOR:     cg_for(g, n); break;

    case N_BREAK:
        if (g->nbrk == 0) ERRX(g, "line %zu: 'break' outside a loop", n->line);
        br_to(g, g->brk[g->nbrk - 1]);
        break;

    case N_CONTINUE:
        if (g->ncnt == 0) ERRX(g, "line %zu: 'continue' outside a loop", n->line);
        br_to(g, g->cnt[g->ncnt - 1]);
        break;

    case N_EXPR_STMT:
        (void)cg_expr(g, n->as.expr_stmt.expr);
        break;

    case N_BLOCK:
        cg_block(g, n);
        break;

    case N_FUNC_DECL:
        /* A local function: hoisted by lume_resolve_localfns into its own
         * module-level define (emitted from the collected function set), so
         * the declaration site itself contributes no code. Same rule as the
         * text backend's cg_stmt. */
        break;

    case N_SERVER: {
        /* server { host=..; port=N; } -> lume_srv_init(host, port) (mirrors
         * codegen_stmt.c's N_SERVER: only literal host/port are read, the
         * serve-env shape feeds env() values through let bindings that the
         * runtime resolves at lume_srv_init time). */
        AT(g);
        const char *host = "127.0.0.1";
        long long port = 8082;
        char hostbuf[256];
        for (int i = 0; i < n->as.server.count; i++) {
            Node *a = n->as.server.assigns[i];
            if (!a || a->type != N_ASSIGN) continue;
            Node *v = a->as.assign.value;
            if (strcmp(a->as.assign.name, "host") == 0 && v &&
                v->type == N_LITERAL && v->as.lit.kind == LIT_STR) {
                const char *t = v->as.lit.text;
                size_t l = (size_t)v->as.lit.len;
                if (l >= 2 && t[0] == '"' && t[l - 1] == '"') { t++; l -= 2; }
                if (l >= sizeof hostbuf) l = sizeof hostbuf - 1;
                memcpy(hostbuf, t, l);
                hostbuf[l] = '\0';
                host = hostbuf;
            } else if (strcmp(a->as.assign.name, "port") == 0 && v &&
                       v->type == N_LITERAL && v->as.lit.kind == LIT_NUM &&
                       !v->as.lit.is_float) {
                port = v->as.lit.inum;
            }
        }
        LLVMValueRef hs = LLVMBuildGlobalStringPtr(g->ab, host, "srvhost");
        LLVMTypeRef p2[2] = { g->i8ptr, g->i64 };
        LLVMValueRef v2[2] = { hs, LLVMConstInt(g->i64,
                              (unsigned long long)port, 0) };
        LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p2, 2, 0),
                       rt_decl(g, "lume_srv_init", g->void_ty, p2, 2),
                       v2, 2, "");
        break;
    }

    case N_ROUTE: {
        /* get/post/... "path", (req) => { .. } -> lume_srv_route(method,
         * path, L_route_N). The handler compiles to a module-level define
         * with the fixed FFI signature the runtime expects. An alias (verb
         * group) expands to one registration per method — built-in groups
         * write (POST/PUT/PATCH/DELETE) and read (GET/HEAD), the native
         * mirror of interp.c's seed_verb_groups; a handler-less route
         * registers lume_srv_default_handler ({ action, method, got },
         * mirroring builtins.c's native_default_route). */
        AT(g);
        Node *h = n->as.route.handler;
        if (h && h->type != N_FUNC_LIT)
            ERRX(g, "line %zu: native server: route handler must be a "
                    "function literal", n->line);
        int rid = -1;
        if (h) {
            rid = g->hrid++;
            /* cg_route_handler emits a separate define: it moves g->fn and
             * g->cur inside, so the caller's block must be recovered before
             * the route-registration calls below. */
            LLVMBasicBlockRef rcur = g->cur;
            cg_route_handler(g, h, rid);
            g->cur = rcur;
            AT(g);
        }
        const char *methods[4];
        int nmethods = 0;
        if (n->as.route.alias) {
            if (strcmp(n->as.route.alias, "write") == 0) {
                methods[0] = "POST"; methods[1] = "PUT";
                methods[2] = "PATCH"; methods[3] = "DELETE"; nmethods = 4;
            } else if (strcmp(n->as.route.alias, "read") == 0) {
                methods[0] = "GET"; methods[1] = "HEAD"; nmethods = 2;
            } else {
                ERRX(g, "line %zu: unknown verb group '%s' (the native "
                        "backend has only the built-in write/read groups; "
                        "declare `verbs ...` in the interpreter)",
                     n->line, n->as.route.alias);
            }
        } else {
            methods[0] = n->as.route.method; nmethods = 1;
        }
        LLVMValueRef pc = LLVMBuildGlobalStringPtr(g->ab,
                                                   n->as.route.path, "rtp");
        LLVMTypeRef hpt[4] = { g->i8ptr, LLVMPointerType(g->i8ptr, 0),
                               LLVMPointerType(g->i64, 0),
                               LLVMPointerType(g->i32, 0) };
        LLVMTypeRef hfty = LLVMFunctionType(g->void_ty, hpt, 4, 0);
        LLVMValueRef dhf = rt_decl(g, "lume_srv_default_handler",
                                   g->void_ty, hpt, 4);
        for (int i = 0; i < nmethods; i++) {
            LLVMValueRef mc = LLVMBuildGlobalStringPtr(g->ab, methods[i],
                                                       "rtm");
            LLVMValueRef hf;
            if (h) {
                char fn[48];
                snprintf(fn, sizeof fn, "L_route_%d", rid);
                hf = LLVMGetNamedFunction(g->mod, fn);
            } else {
                hf = dhf;
            }
            LLVMValueRef hp = LLVMBuildBitCast(g->ab, hf,
                                               LLVMPointerType(hfty, 0), "hfn");
            LLVMTypeRef p3[3] = { g->i8ptr, g->i8ptr,
                                  LLVMPointerType(hfty, 0) };
            LLVMValueRef v3[3] = { mc, pc, hp };
            LLVMBuildCall2(g->ab,
                           LLVMFunctionType(g->void_ty, p3, 3, 0),
                           rt_decl(g, "lume_srv_route", g->void_ty, p3, 3),
                           v3, 3, "");
        }
        break;
    }

    default:
        ERRX(g, "%s", bad(g, n, "this construct"));
    }
}

static void cg_block(CG *g, Node *blk)
{
    if (!blk) return;
    for (int i = 0; i < blk->as.block.count; i++) cg_stmt(g, blk->as.block.stmts[i]);
}

static void cg_def(CG *g, const char *sym, char **names, Type **params,
                   int arity, Type *ret, Node *body, int has_cap,
                   Node *capfn, size_t line)
{
    int np = arity + (has_cap ? 1 : 0);
    LLVMTypeRef *pts = NULL;
    if (np > 0) {
        pts = (LLVMTypeRef *)xmalloc(sizeof(LLVMTypeRef) * (size_t)np);
        if (has_cap) pts[0] = g->i8ptr;
        for (int i = 0; i < arity; i++) {
            pts[i + (has_cap ? 1 : 0)] = params ? ptr_of(g, params[i]) : NULL;
            if (!pts[i + (has_cap ? 1 : 0)])
                ERRX(g, "line %zu: parameter '%s' has no codegen type",
                     line, names ? names[i] : "?");
        }
    }

    LLVMTypeRef rty = ty_of(g, ret);
    LLVMTypeRef fty = LLVMFunctionType(rty ? rty : g->void_ty,
                                       np > 0 ? pts : NULL, (unsigned)np, 0);

    /* LLVMAddFunction always creates and then *uniques* the name (L_g.1, ...)
     * — the get-or-insert semantics live in the C++ helper a call goes
     * through, not in this API. A forward call (or the boxing call a lambda
     * literal emits) already declared this symbol, so it is reused instead;
     * otherwise the body would land on a copy. */
    LLVMValueRef prev = LLVMGetNamedFunction(g->mod, sym);
    g->fn = prev ? prev : LLVMAddFunction(g->mod, sym, fty);

    /* Locals move aside: a variable of the caller must not be visible here.
     * The loop-label stacks too: a `break` inside this body can never target
     * a loop outside it, and with the stacks cleared the emitter reports
     * "'break' outside a loop" instead of branching into another function.
     * L_top is the one body that arrives with its locals already scanned
     * (the driver's pre-scan, which lifted the top-level bindings into
     * module globals) — reuse that table instead of scanning a second time. */
    Asgs saved = g->locals;
    int preset = g->scanning_top && g->preset_locals;
    if (preset)
        g->locals = *g->preset_locals;
    else
        memset(&g->locals, 0, sizeof g->locals);
    /* While-statement scope marks pair up per function: the scan below
     * pushes saved entries from the current global while-count on; reset
     * the emit counter to the same base so this function's cg_while reads
     * its own entries (L_top's pre-scan happened in the driver, so its
     * base is whatever the pre-scan advanced the counter to). Preset mode
     * (L_top) is special: pass 1.7 recorded L_top's while marks from
     * counter 0, while every named function re-uses the post-pre-scan
     * counter as its own base — so preset mode must read from 0 or it
     * picks up the last emitted function's marks and truncates the locals
     * table to a foreign size (reading uninitialized Asg entries). */
    int wh_base = g->wh_idx;
    if (preset) wh_base = 0;
    LLVMBasicBlockRef *saved_brk = g->brk; int snbrk = g->nbrk, scbrk = g->cbrk;
    LLVMBasicBlockRef *saved_cnt = g->cnt; int sncnt = g->ncnt, sccnt = g->ccnt;
    g->brk = g->cnt = NULL; g->nbrk = g->ncnt = g->cbrk = g->ccnt = 0;
    Type *saved_ret = g->cur_ret;
    g->cur_ret = ret;

    /* Parameters first, so their slots exist before the body is scanned. A
     * struct parameter is *already* a pointer, so it keeps the parameter value
     * as its slot. With a capture context the closure's own parameters start
     * at LLVM param index 1. */
    LLVMValueRef *pv = NULL;
    if (np > 0) {
        pv = (LLVMValueRef *)xmalloc(sizeof(LLVMValueRef) * (size_t)np);
        /* LLVMGetParams fills one entry per function parameter, the capture
         * context first when has_cap — so the readback below indexes
         * pv[i + 1] for a closure's own parameters. */
        LLVMGetParams(g->fn, pv);
        for (int i = 0; i < arity; i++)
            asg_push(&g->locals, names[i], params[i],
                     (params[i] && params[i]->kind == TY_STRUCT)
                         ? pv[i + (has_cap ? 1 : 0)] : NULL,
                     0);
    }

    /* Phase-2 captures (mirrors codegen_stmt.c): each captured name gets a
     * binding whose slot is a typed pointer into the closure's capture
     * array, materialized in the entry block below. Registering them
     * before scan_block keeps the body scan from re-declaring a free
     * variable as a fresh local. */
    int ncap = capfn ? capfn->as.funclit.cap_count : 0;
    for (int i = 0; i < ncap; i++)
        asg_push_cap(&g->locals, capfn->as.funclit.cap_names[i],
                     capfn->as.funclit.cap_types[i], NULL, 0);

    if (!preset) scan_block(g, body);
    g->wh_idx = wh_base;   /* emit reads this function's saved entries */

    /* Entry: one alloca per local, then seed the non-struct parameters. */
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "entry");
    g->cur = entry;
    LLVMPositionBuilderAtEnd(g->ab, entry);

    for (int i = 0; i < g->locals.n; i++) {
        Asg *a = &g->locals.v[i];
        if (a->slot) continue;                       /* a struct parameter */
        if (a->cap) continue;  /* a capture: the slot is the capture-array
                                * pointer materialized below, no stack cell
                                * — the %cap-prefix skip of the text alloca
                                * pass */
        /* A local whose type the frontend could not infer has no IR type to
         * allocate, and NULL is not something libLLVM survives: this used to
         * SIGSEGV the compiler on `for (x in [])`, where the loop variable is
         * registered with an element type there is none to find. Reporting it
         * is the whole point — llvm_codegen_module turns g->err into a clean
         * exit 1 at the failure cleanup below, which is what --compile-text
         * already did for the same input. */
        if (!ty_of(g, a->ty))
            ERRX(g, "line %zu: variable '%s' has no codegen type "
                    "(its type could not be inferred, e.g. an empty list)",
                 line, a->name);
        a->slot = LLVMBuildAlloca(g->ab, ty_of(g, a->ty), "lv");
    }

    /* Seed the globals-shadowing locals the scan's N_ASSIGN arm registered:
     * the interpreter's env lookup hands a body the top-level name's current
     * value, so an unseeded alloca first-reads stack garbage (mirrors
     * codegen_stmt.c). Named structs are reference-shaped at their global
     * and stay skipped. */
    for (int i = 0; i < g->locals.n; i++) {
        Asg *a = &g->locals.v[i];
        if (!a->shadow || a->cap) continue;
        if (a->ty && a->ty->kind == TY_STRUCT && a->ty->name) continue;
        Asg *gv = gvar_find(&g->gvars, a->name);
        if (!gv || !gv->slot || !a->slot) continue;
        LLVMValueRef cur = LLVMBuildLoad2(g->ab, ty_of(g, a->ty), gv->slot, "gvseed");
        if (!cur) continue;
        LLVMBuildStore(g->ab, cur, a->slot);
    }

    /* Capture slots: the i8* context parameter points at the creation
     * site's array of slot addresses (cg_funclit builds it). A
     * captured binding enters the alloca pass with slot NULL and cap 1,
     * so here its entry is loaded and bitcast to the typed pointer the
     * body's loads/stores use directly — the text backend's
     * bitcast-gep-load-%cap<i> chain, resolved once per name. */
    if (ncap > 0 && has_cap) {
        LLVMValueRef capp = LLVMBuildBitCast(g->ab, pv[0],
                                             LLVMPointerType(g->i8ptr, 0), "capv");
        for (int i = 0; i < g->locals.n; i++) {
            Asg *a = &g->locals.v[i];
            if (!a->cap || a->slot) continue;
            size_t idx = 0;
            int found = 0;
            for (int j = 0; j < ncap; j++)
                if (strcmp(capfn->as.funclit.cap_names[j], a->name) == 0
                    && capfn->as.funclit.cap_types[j] == a->ty) {
                    idx = (size_t)j; found = 1; break;
                }
            if (!found)
                ERRX(g, "line %zu: internal: captured '%s' missing from the "
                        "signature pass list", line, a->name);
            LLVMValueRef idxv = LLVMConstInt(g->i64, idx, 0);
            LLVMValueRef el = LLVMBuildGEP2(g->ab, g->i8ptr, capp,
                                            &idxv, 1, "cape");
            if (!el)
                ERRX(g, "line %zu: cannot address captured '%s'", line, a->name);
            LLVMValueRef raw = LLVMBuildLoad2(g->ab, g->i8ptr, el, "capl");
            if (!raw)
                ERRX(g, "line %zu: cannot load captured '%s'", line, a->name);
            LLVMTypeRef vt = ptr_of(g, a->ty);
            LLVMTypeRef pt = vt ? LLVMPointerType(vt, 0) : NULL;
            if (!pt)
                ERRX(g, "line %zu: captured variable '%s' has no codegen type",
                     line, a->name);
            LLVMValueRef tp = LLVMBuildBitCast(g->ab, raw, pt, "cap");
            if (!tp)
                ERRX(g, "line %zu: cannot capture '%s'", line, a->name);
            a->slot = tp;
        }
    }
    if (pv) {
        for (int i = 0; i < arity; i++) {
            if (params[i] && params[i]->kind == TY_STRUCT) continue;
            LLVMBuildStore(g->ab, pv[i + (has_cap ? 1 : 0)],
                           asg_find(&g->locals, names[i], 0)->slot);
        }
    }

    if (body) cg_block(g, body);

    /* Close the function: any block the body left open still needs exactly one
     * terminator. A struct return has no zeroinitialiser to hand back — such a
     * body is expected to have returned already. */
    if (!DONE(g->cur)) {
        /* The synthetic top body carries whatever main() gave back, not a
         * zero: translating the interpreter's exit code is the whole point. */
        if (g->exit_val)                       LLVMBuildRet(g->ab, g->exit_val);
        else if (!rty || rty == g->void_ty)    LLVMBuildRetVoid(g->ab);
        else                                   LLVMBuildRet(g->ab, LLVMConstNull(rty));
    }

    if (preset) {
        /* The pre-scanned table stays owned by the driver; only the shallow
         * copy here goes away. */
        memset(&g->locals, 0, sizeof g->locals);
    } else {
        asgs_free(&g->locals);
    }
    g->locals = saved;
    /* The body ran with its own break/continue stacks (cg_def NULLs them on
     * the way in); they were grown by push_bb inside this function, so free
     * them before the outer pointers come back -- restoring over them used
     * to drop the blocks and CI's asan leg caught exactly that (a top-level
     * for-in makes L_top itself a body that grows both stacks). */
    free(g->brk); free(g->cnt);
    g->brk = saved_brk; g->nbrk = snbrk; g->cbrk = scbrk;
    g->cnt = saved_cnt; g->ncnt = sncnt; g->ccnt = sccnt;
    g->cur_ret = saved_ret;
    g->fn = NULL;
    /* The two scratch vectors: LLVMFunctionType and LLVMGetParams only read
     * them, the IR owns the values, so nothing survives past this point. */
    free(pts);
    free(pv);
}

static void cg_function(CG *g, Node *fn)
{
    char sym[160];
    /* A local function is defined (and called) under its synthetic cname;
     * top-level functions keep their source name. Same rule as the text
     * backend's cg_function. */
    const char *fname = fn->as.func.cname ? fn->as.func.cname
                        : (fn->as.func.name ? fn->as.func.name : "?");
    snprintf(sym, sizeof sym, "L_%s", fname);
    cg_def(g, sym, fn->as.func.names, fn->as.func.param_types,
           fn->as.func.arity, fn->as.func.ret, fn->as.func.body, 0, NULL,
           fn->line);
}

static void cg_closure(CG *g, Node *fn)
{
    if (!fn->as.funclit.cname)
        ERRX(g, "line %zu: internal: closure was never given a mangled name",
             fn->line);
    cg_def(g, fn->as.funclit.cname, fn->as.funclit.names,
           fn->as.funclit.param_types, fn->as.funclit.arity,
           fn->as.funclit.ret, fn->as.funclit.body, 1, fn, fn->line);
}

/* --- native server: route handler FFI compilation (mirrors the text
 * backend's cg_route_handler in codegen_stmt.c) --------------------------
 *
 * A handler `(req) => { ... }` compiles to
 *
 *   void @L_route_N(i8* %p0, i8** %p1, i64* %p2, i32* %p3)
 *
 * %p0 is the request (an opaque SrvReq* here — req.* member access on the
 * LLVM backend is still an open gap, so handlers that only read env()/locals
 * compile today). %p1/%p2/%p3 receive the response buffer and its length,
 * and %p3 the content-type flag (0 = text/html, 1 = application/json).
 * With in_handler set, N_RETURN serialises instead of returning a value. */
static void cg_route_handler(CG *g, Node *fn, int rid)
{
    if (fn->as.funclit.arity != 1)
        ERRX(g, "line %zu: native server: a route handler must take exactly "
                "one (req) argument", fn->line);

    LLVMTypeRef hpt[4] = { g->i8ptr, LLVMPointerType(g->i8ptr, 0),
                           LLVMPointerType(g->i64, 0),
                           LLVMPointerType(g->i32, 0) };
    LLVMTypeRef hfty = LLVMFunctionType(g->void_ty, hpt, 4, 0);
    char sym[48];
    snprintf(sym, sizeof sym, "L_route_%d", rid);
    LLVMValueRef prev = LLVMGetNamedFunction(g->mod, sym);
    g->fn = prev ? prev : LLVMAddFunction(g->mod, sym, hfty);

    Asgs saved = g->locals;
    memset(&g->locals, 0, sizeof g->locals);
    Type *saved_ret = g->cur_ret;
    g->cur_ret = NULL;
    LLVMBasicBlockRef *saved_brk = g->brk; int snbrk = g->nbrk, scbrk = g->cbrk;
    LLVMBasicBlockRef *saved_cnt = g->cnt; int sncnt = g->ncnt, sccnt = g->ccnt;
    g->brk = g->cnt = NULL; g->nbrk = g->ncnt = g->cbrk = g->ccnt = 0;

    g->in_handler = 1;
    g->hreq = fn->as.funclit.names ? fn->as.funclit.names[0] : "req";

    LLVMValueRef pv[4];
    LLVMGetParams(g->fn, pv);
    /* The req parameter keeps its FFI slot directly (an opaque SrvReq*; its
     * static type, when the signature pass inferred one, is registered so
     * the name resolves). The response pointers are not lume variables. */
    Type *reqty = fn->as.funclit.param_types
                  ? fn->as.funclit.param_types[0] : NULL;
    asg_push(&g->locals, g->hreq, reqty, pv[0], 0);
    g->hout = pv[1]; g->hlen = pv[2]; g->hjson = pv[3];

    /* The handler body is its own function: scanning it while the driver's
     * scanning_top window is open would lift its locals into module globals
     * that every request then shares (mirrors the text backend fix in
     * codegen_stmt.c's cg_route_handler). */
    int saved_scan_top = g->scanning_top;
    g->scanning_top = 0;
    scan_block(g, fn->as.funclit.body);
    g->scanning_top = saved_scan_top;

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(g->ctx, g->fn,
                                                           "entry");
    g->cur = entry;
    LLVMPositionBuilderAtEnd(g->ab, entry);

    for (int i = 0; i < g->locals.n; i++) {
        Asg *a = &g->locals.v[i];
        if (a->slot) continue;
        if (!ty_of(g, a->ty))
            ERRX(g, "line %zu: variable '%s' has no codegen type",
                 fn->line, a->name);
        a->slot = LLVMBuildAlloca(g->ab, ty_of(g, a->ty), "lv");
    }
    /* req's slot is pv[0] itself, so there is nothing to seed; non-param
     * locals start uninitialised exactly like cg_def's. */

    if (fn->as.funclit.body) cg_block(g, fn->as.funclit.body);

    if (!DONE(g->cur)) LLVMBuildRetVoid(g->ab);

    g->in_handler = 0;
    g->hreq = NULL;
    asgs_free(&g->locals);
    g->locals = saved;
    g->cur_ret = saved_ret;
    free(g->brk); free(g->cnt);
    g->brk = saved_brk; g->nbrk = snbrk; g->cbrk = scbrk;
    g->cnt = saved_cnt; g->ncnt = sncnt; g->ccnt = sccnt;
    g->fn = NULL;
}

/* Emit one JSON value from a struct field already loaded into `fv`. */
static void llvm_json_field_val(CG *g, Type *ft, LLVMValueRef fv,
                                LLVMTypeRef p3s[3], LLVMTypeRef p3i[3],
                                LLVMTypeRef p3f[3], size_t line)
{
    if (ft->kind == TY_STRING) {
        LLVMValueRef sv3[3] = { g->hout, g->hlen, fv };
        LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p3s, 3, 0),
                       rt_decl(g, "lume_srv_json_str", g->void_ty, p3s, 3),
                       sv3, 3, "");
    } else if (ft->kind == TY_INT) {
        LLVMValueRef iv[3] = { g->hout, g->hlen, fv };
        LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p3i, 3, 0),
                       rt_decl(g, "lume_srv_json_int", g->void_ty, p3i, 3),
                       iv, 3, "");
    } else if (ft->kind == TY_BOOL) {
        LLVMValueRef z = LLVMBuildZExt(g->ab, fv, g->i64, "jz");
        LLVMValueRef iv[3] = { g->hout, g->hlen, z };
        LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p3i, 3, 0),
                       rt_decl(g, "lume_srv_json_int", g->void_ty, p3i, 3),
                       iv, 3, "");
    } else if (ft->kind == TY_FLOAT) {
        LLVMValueRef fv3[3] = { g->hout, g->hlen, fv };
        LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p3f, 3, 0),
                       rt_decl(g, "lume_srv_json_float", g->void_ty,
                               p3f, 3), fv3, 3, "");
    } else {
        ERRX(g, "line %zu: field must be a scalar to serialise", line);
    }
}

/* Serialise a struct (map-shaped) value as a JSON object — the LLVM mirror
 * of codegen_stmt.c's cg_json_struct: begin, each declared field as a key +
 * value dispatched on its codegen type, end. A named struct is a raw GEP
 * object; an anonymous one is a runtime map read through the map getters.
 * Container fields error. */
static void llvm_json_struct(CG *g, Val sv, size_t line)
{
    Type *st = sv.ty;
    if (!st || st->kind != TY_STRUCT)
        ERRX(g, "line %zu: cannot serialise non-struct", line);
    LLVMTypeRef p2[2] = { LLVMPointerType(g->i8ptr, 0),
                          LLVMPointerType(g->i64, 0) };
    LLVMTypeRef p3s[3] = { p2[0], p2[1], g->i8ptr };
    LLVMTypeRef p3i[3] = { p2[0], p2[1], g->i64 };
    LLVMTypeRef p3f[3] = { p2[0], p2[1], g->dbl };
    LLVMValueRef v2[2] = { g->hout, g->hlen };
    LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p2, 2, 0),
                   rt_decl(g, "lume_srv_json_begin", g->void_ty, p2, 2),
                   v2, 2, "");
    if (!st->name) {
        for (int i = 0; i < st->count; i++) {
            LLVMValueRef kc = LLVMBuildGlobalStringPtr(g->ab, st->names[i], "jk");
            LLVMValueRef k3[3] = { g->hout, g->hlen, kc };
            LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p3s, 3, 0),
                           rt_decl(g, "lume_srv_json_key", g->void_ty, p3s, 3),
                           k3, 3, "");
            Type *ft = st->types[i];
            const char *gfn = map_get_fn(ft);
            if (!gfn)
                ERRX(g, "line %zu: field '%s' has no codegen type",
                     line, st->names[i]);
            LLVMTypeRef rty = ft->kind == TY_FLOAT ? g->dbl :
                              ft->kind == TY_STRING ? g->i8ptr : g->i64;
            LLVMValueRef dflt = ft->kind == TY_FLOAT ?
                                LLVMConstReal(g->dbl, 0.0) :
                                ft->kind == TY_STRING ?
                                LLVMConstNull(g->i8ptr) :
                                LLVMConstInt(g->i64, 0, 0);
            LLVMTypeRef pg[3] = { g->i8ptr, g->i8ptr, rty };
            LLVMValueRef fv = LLVMBuildCall2(g->ab,
                                LLVMFunctionType(rty, pg, 3, 0),
                                rt_decl(g, gfn, rty, pg, 3),
                                (LLVMValueRef[]){ sv.v, kc, dflt }, 3, "jf");
            llvm_json_field_val(g, ft, fv, p3s, p3i, p3f, line);
        }
        LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p2, 2, 0),
                       rt_decl(g, "lume_srv_json_end", g->void_ty, p2, 2),
                       v2, 2, "");
        return;
    }
    struct_addr(g, &sv);
    LLVMTypeRef stt = ty_of(g, st);
    if (!stt)
        ERRX(g, "line %zu: struct '%s' has no codegen type", line, st->name);
    for (int i = 0; i < st->count; i++) {
        LLVMValueRef kc = LLVMBuildGlobalStringPtr(g->ab, st->names[i], "jk");
        LLVMValueRef k3[3] = { g->hout, g->hlen, kc };
        LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p3s, 3, 0),
                       rt_decl(g, "lume_srv_json_key", g->void_ty, p3s, 3),
                       k3, 3, "");
        Type *ft = st->types[i];
        LLVMValueRef gep = LLVMBuildStructGEP2(g->ab, stt, sv.v,
                                               (unsigned)i, "fgep");
        LLVMValueRef fv = LLVMBuildLoad2(g->ab,
                                         LLVMStructGetTypeAtIndex(stt, i),
                                         gep, "fl");
        llvm_json_field_val(g, ft, fv, p3s, p3i, p3f, line);
    }
    LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p2, 2, 0),
                   rt_decl(g, "lume_srv_json_end", g->void_ty, p2, 2),
                   v2, 2, "");
}

/* return in a native handler: string -> plain text, map literal -> JSON.
 * Mirrors codegen_stmt.c's cg_handler_ret — the response is written through
 * the FFI pointers hout/hlen/hjson (the handler's p1/p2/p3). */
static void cg_handler_ret(CG *g, Node *n)
{
    Node *e = n->as.ret.expr;
    if (!e) { if (!DONE(g->cur)) LLVMBuildRetVoid(g->ab); return; }

    if (e->type == N_MAP_LIT) {
        LLVMTypeRef p2[2] = { LLVMPointerType(g->i8ptr, 0),
                              LLVMPointerType(g->i64, 0) };
        /* The value helpers differ in their third parameter: str takes an
         * i8*, int an i64, bool an i32 (mirrors the text declare block in
         * codegen_call.c). */
        LLVMTypeRef p3s[3] = { LLVMPointerType(g->i8ptr, 0),
                               LLVMPointerType(g->i64, 0), g->i8ptr };
        LLVMTypeRef p3i[3] = { LLVMPointerType(g->i8ptr, 0),
                               LLVMPointerType(g->i64, 0), g->i64 };
        LLVMTypeRef p3b[3] = { LLVMPointerType(g->i8ptr, 0),
                               LLVMPointerType(g->i64, 0), g->i32 };
        LLVMTypeRef v2ty = LLVMFunctionType(g->void_ty, p2, 2, 0);
        LLVMTypeRef vsty = LLVMFunctionType(g->void_ty, p3s, 3, 0);
        LLVMTypeRef vity = LLVMFunctionType(g->void_ty, p3i, 3, 0);
        LLVMTypeRef vbty = LLVMFunctionType(g->void_ty, p3b, 3, 0);
        LLVMValueRef v2[2] = { g->hout, g->hlen };
        LLVMBuildCall2(g->ab, v2ty, rt_decl(g, "lume_srv_json_begin",
                                            g->void_ty, p2, 2), v2, 2, "");
        for (int i = 0; i < e->as.map.count; i++) {
            LLVMValueRef kc = LLVMBuildGlobalStringPtr(g->ab,
                                                       e->as.map.keys[i], "jk");
            LLVMValueRef k3[3] = { g->hout, g->hlen, kc };
            LLVMBuildCall2(g->ab, vsty, rt_decl(g, "lume_srv_json_key",
                                                g->void_ty, p3s, 3), k3, 3, "");
            Node *v = e->as.map.vals[i];
            if (v->type == N_LITERAL && v->as.lit.kind == LIT_STR) {
                const char *vt = v->as.lit.text;
                size_t vl = (size_t)v->as.lit.len;
                if (vl >= 2 && vt[0] == '"' && vt[vl - 1] == '"') { vt++; vl -= 2; }
                char buf[1024];
                if (vl >= sizeof buf) vl = sizeof buf - 1;
                memcpy(buf, vt, vl); buf[vl] = '\0';
                LLVMValueRef sc = LLVMBuildGlobalStringPtr(g->ab, buf, "js");
                LLVMValueRef sv[3] = { g->hout, g->hlen, sc };
                LLVMBuildCall2(g->ab, vsty, rt_decl(g, "lume_srv_json_str",
                                                    g->void_ty, p3s, 3), sv, 3, "");
            } else if (v->type == N_LITERAL &&
                       (v->as.lit.kind == LIT_TRUE ||
                        v->as.lit.kind == LIT_FALSE)) {
                LLVMValueRef bv[3] = { g->hout, g->hlen,
                    LLVMConstInt(g->i32, v->as.lit.kind == LIT_TRUE ? 1 : 0, 0) };
                LLVMBuildCall2(g->ab, vbty, rt_decl(g, "lume_srv_json_bool",
                                                    g->void_ty, p3b, 3), bv, 3, "");
            } else if (v->type == N_LITERAL && v->as.lit.kind == LIT_NUM &&
                       !v->as.lit.is_float) {
                LLVMValueRef iv[3] = { g->hout, g->hlen,
                    LLVMConstInt(g->i64, (unsigned long long)v->as.lit.inum, 0) };
                LLVMBuildCall2(g->ab, vity, rt_decl(g, "lume_srv_json_int",
                                                    g->void_ty, p3i, 3), iv, 3, "");
            } else {
                Val vv = cg_expr(g, v);
                if (!vv.v)
                    ERRX(g, "line %zu: cannot serialise map value", v->line);
                if (vv.ty && vv.ty->kind == TY_INT) {
                    LLVMValueRef iv[3] = { g->hout, g->hlen, vv.v };
                    LLVMBuildCall2(g->ab, vity, rt_decl(g, "lume_srv_json_int",
                                                        g->void_ty, p3i, 3), iv, 3, "");
                } else if (vv.ty && vv.ty->kind == TY_STRING) {
                    LLVMValueRef sv[3] = { g->hout, g->hlen, vv.v };
                    LLVMBuildCall2(g->ab, vsty, rt_decl(g, "lume_srv_json_str",
                                                        g->void_ty, p3s, 3), sv, 3, "");
                } else if (vv.ty && vv.ty->kind == TY_BOOL) {
                    LLVMValueRef z = LLVMBuildZExt(g->ab, vv.v, g->i32, "jb");
                    LLVMValueRef bv[3] = { g->hout, g->hlen, z };
                    LLVMBuildCall2(g->ab, vbty, rt_decl(g, "lume_srv_json_bool",
                                                        g->void_ty, p3b, 3), bv, 3, "");
                } else if (vv.ty && vv.ty->kind == TY_FLOAT) {
                    LLVMTypeRef p3f[3] = { p2[0], p2[1], g->dbl };
                    LLVMValueRef fv[3] = { g->hout, g->hlen, vv.v };
                    LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p3f, 3, 0),
                                   rt_decl(g, "lume_srv_json_float",
                                           g->void_ty, p3f, 3), fv, 3, "");
                } else if (vv.ty && vv.ty->kind == TY_STRUCT) {
                    Val s = vv;
                    vv = (Val){ NULL, NULL };
                    llvm_json_struct(g, s, v->line);
                } else if (vv.ty && vv.ty->kind == TY_LIST) {
                    /* array: [ sep elem sep elem ... ] — iterate the list
                     * with lume_list_len / lume_list_at_*; scalar elements
                     * only (mirrors codegen_stmt.c's branch). */
                    Type *et = vv.ty->elem;
                    const char *atfn = list_at_fn(et);
                    if (!atfn)
                        ERRX(g, "line %zu: native server: list element has no "
                                "codegen type", v->line);
                    LLVMTypeRef atrt_ty = et && et->kind == TY_STRING ? g->i8ptr :
                                          et && et->kind == TY_FLOAT ? g->dbl :
                                          g->i64;
                    LLVMBasicBlockRef cur = LLVMGetInsertBlock(g->ab);
                    LLVMValueRef fnv = LLVMGetBasicBlockParent(cur);
                    LLVMBasicBlockRef lc = LLVMAppendBasicBlock(fnv, "ljc");
                    LLVMBasicBlockRef lb = LLVMAppendBasicBlock(fnv, "ljb");
                    LLVMBasicBlockRef le = LLVMAppendBasicBlock(fnv, "lje");
                    LLVMValueRef iv = LLVMBuildAlloca(g->ab, g->i64, "jiv");
                    LLVMBuildStore(g->ab, LLVMConstInt(g->i64, 0, 0), iv);
                    LLVMTypeRef p2b[2] = { p2[0], p2[1] };
                    LLVMValueRef v2b[2] = { g->hout, g->hlen };
                    LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p2b, 2, 0),
                                   rt_decl(g, "lume_srv_json_arr_begin",
                                           g->void_ty, p2b, 2), v2b, 2, "");
                    LLVMBuildBr(g->ab, lc);
                    LLVMPositionBuilderAtEnd(g->ab, lc);
                    LLVMValueRef ic = LLVMBuildLoad2(g->ab, g->i64, iv, "ji");
                    LLVMTypeRef p1l[1] = { g->i8ptr };
                    LLVMValueRef ln = LLVMBuildCall2(g->ab,
                                        LLVMFunctionType(g->i64, p1l, 1, 0),
                                        rt_decl(g, "lume_list_len", g->i64,
                                                p1l, 1),
                                        (LLVMValueRef[]){ vv.v }, 1, "jl");
                    LLVMValueRef cm = LLVMBuildICmp(g->ab, LLVMIntSLT, ic, ln,
                                                    "jcmp");
                    LLVMBuildCondBr(g->ab, cm, lb, le);
                    LLVMPositionBuilderAtEnd(g->ab, lb);
                    LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p2b, 2, 0),
                                   rt_decl(g, "lume_srv_json_arr_sep",
                                           g->void_ty, p2b, 2), v2b, 2, "");
                    LLVMTypeRef pat[2] = { g->i8ptr, g->i64 };
                    LLVMValueRef at = LLVMBuildCall2(g->ab,
                                        LLVMFunctionType(atrt_ty, pat, 2, 0),
                                        rt_decl(g, atfn, atrt_ty, pat, 2),
                                        (LLVMValueRef[]){ vv.v, ic }, 2, "je");
                    if (et && et->kind == TY_STRING) {
                        LLVMValueRef sv3[3] = { g->hout, g->hlen, at };
                        LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p3s, 3, 0),
                                       rt_decl(g, "lume_srv_json_str",
                                               g->void_ty, p3s, 3), sv3, 3, "");
                    } else if (et && (et->kind == TY_INT || et->kind == TY_BOOL)) {
                        LLVMValueRef iv3[3] = { g->hout, g->hlen, at };
                        LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p3i, 3, 0),
                                       rt_decl(g, "lume_srv_json_int",
                                               g->void_ty, p3i, 3), iv3, 3, "");
                    } else if (et && et->kind == TY_FLOAT) {
                        LLVMTypeRef p3f[3] = { p2[0], p2[1], g->dbl };
                        LLVMValueRef fv3[3] = { g->hout, g->hlen, at };
                        LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p3f, 3, 0),
                                       rt_decl(g, "lume_srv_json_float",
                                               g->void_ty, p3f, 3), fv3, 3, "");
                    } else {
                        ERRX(g, "line %zu: native server: list element must "
                                "be a scalar", v->line);
                    }
                    LLVMValueRef nv = LLVMBuildAdd(g->ab, ic,
                                                   LLVMConstInt(g->i64, 1, 0), "jn");
                    LLVMBuildStore(g->ab, nv, iv);
                    LLVMBuildBr(g->ab, lc);
                    LLVMPositionBuilderAtEnd(g->ab, le);
                    LLVMBuildCall2(g->ab, LLVMFunctionType(g->void_ty, p2b, 2, 0),
                                   rt_decl(g, "lume_srv_json_arr_end",
                                           g->void_ty, p2b, 2), v2b, 2, "");
                } else {
                    ERRX(g, "line %zu: native server: map value must be int, "
                            "string or bool", v->line);
                }
            }
        }
        LLVMValueRef ev[2] = { g->hout, g->hlen };
        LLVMBuildCall2(g->ab, v2ty, rt_decl(g, "lume_srv_json_end",
                                            g->void_ty, p2, 2), ev, 2, "");
        LLVMBuildStore(g->ab, LLVMConstInt(g->i32, 1, 0), g->hjson);
        if (!DONE(g->cur)) LLVMBuildRetVoid(g->ab);
        return;
    }

    /* String-valued expression -> plain text. The length is unknown at
     * compile time (concatenations, str(), ...), so out_len = -1 asks the
     * runtime to strlen the buffer. */
    Val sv = cg_expr(g, e);
    if (sv.ty && sv.ty->kind == TY_STRING) {
        LLVMBuildStore(g->ab, sv.v, g->hout);
        LLVMBuildStore(g->ab, LLVMConstInt(g->i64, -1, 1), g->hlen);
        LLVMBuildStore(g->ab, LLVMConstInt(g->i32, 0, 0), g->hjson);
        if (!DONE(g->cur)) LLVMBuildRetVoid(g->ab);
        return;
    }
    if (sv.ty && sv.ty->kind == TY_STRUCT) {
        Val s = sv;
        sv = (Val){ NULL, NULL };
        llvm_json_struct(g, s, n->line);
        LLVMBuildStore(g->ab, LLVMConstInt(g->i32, 1, 0), g->hjson);
        if (!DONE(g->cur)) LLVMBuildRetVoid(g->ab);
        return;
    }

    ERRX(g, "line %zu: native server: handler must return a string or a map "
            "literal", n->line);
}
