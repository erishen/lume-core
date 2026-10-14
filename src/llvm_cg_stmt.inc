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
    g->arm_scope.v[g->arm_scope.n].name = xstrdup(name);
    g->arm_scope.v[g->arm_scope.n].ty   = ty;
    g->arm_scope.n++;
}

static void arm_scope_clear(CG *g)
{
    for (int i = 0; i < g->arm_scope.n; i++) free(g->arm_scope.v[i].name);
    free(g->arm_scope.v);
    g->arm_scope = (LTys){0};
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
    LLVMValueRef cond = cg_expr(g, n->as.whiles.cond).v;
    if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, cond, b, e);

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
    const char *fn = is_map ? NULL : list_at_fn(et);
    if (!is_map && !fn)
        ERRX(g, "line %zu: a list of this element type cannot be iterated", n->line);

    LLVMValueRef idx = LLVMBuildAlloca(g->ab, g->i64, "li");
    LLVMBuildStore(g->ab, LLVMConstInt(g->i64, 0, 0), idx);

    /* Snapshot the collection length once, before the loop, to match the
     * interpreter (which captures list.count at loop entry). Re-reading it
     * every iteration made `push(xs, v)` inside the body extend the bound
     * forever and OOM the backend. */
    LLVMValueRef len_slot = LLVMBuildAlloca(g->ab, g->i64, "len");
    LLVMValueRef len0 = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i64, &g->i8ptr, 1, 0),
                                       rt_decl(g, is_map ? "lume_map_len" : "lume_list_len",
                                                g->i64, &g->i8ptr, 1),
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

    case N_ASSIGN_MEMBER:
        (void)cg_assign_mem(g, n);
        break;

    case N_RETURN: {
        AT(g);
        if (!n->as.ret.expr) {
            if (!DONE(g->cur)) LLVMBuildRetVoid(g->ab);
            break;
        }
        /* `return { .. }` needs the function's declared return type. */
        Val v = cg_expr(g, n->as.ret.expr);
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
                   int arity, Type *ret, Node *body, int has_cap, size_t line)
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
    if (arity > 0) {
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

    if (!preset) scan_block(g, body);

    /* Entry: one alloca per local, then seed the non-struct parameters. */
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "entry");
    g->cur = entry;
    LLVMPositionBuilderAtEnd(g->ab, entry);

    for (int i = 0; i < g->locals.n; i++) {
        Asg *a = &g->locals.v[i];
        if (a->slot) continue;                       /* a struct parameter */
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
    snprintf(sym, sizeof sym, "L_%s", fn->as.func.name ? fn->as.func.name : "?");
    cg_def(g, sym, fn->as.func.names, fn->as.func.param_types,
           fn->as.func.arity, fn->as.func.ret, fn->as.func.body, 0, fn->line);
}

static void cg_closure(CG *g, Node *fn)
{
    if (!fn->as.funclit.cname)
        ERRX(g, "line %zu: internal: closure was never given a mangled name",
             fn->line);
    cg_def(g, fn->as.funclit.cname, fn->as.funclit.names,
           fn->as.funclit.param_types, fn->as.funclit.arity,
           fn->as.funclit.ret, fn->as.funclit.body, 1, fn->line);
}
