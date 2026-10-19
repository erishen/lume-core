/* llvm_cg_scan.inc - LLVM backend scan functions.
 * Textually included by llvm_codegen.c (single translation unit).
 * Do not compile this file on its own. */

/* The element type of a list literal, read off its own elements. Only ever
 * asked about a list *literal*: a value is just an i8*, and what a for-in has
 * to know is which reader to call, which is a static question. A heterogeneous
 * literal (`[1.5, 2]`) yields `list<any>`, and iterating that natively would
 * read one slot through the wrong accessor, so it is reported as unknown. */
static Type *infer_list_elem(CG *g, Node *lit)
{
    if (!lit || lit->type != N_LIST_LIT) return NULL;

    Type *et = NULL;
    for (int i = 0; i < lit->as.list.count; i++) {
        Node *e = lit->as.list.items[i];
        Type *t = NULL;

        if (e->type == N_LITERAL) {
            switch (e->as.lit.kind) {
            case LIT_NUM:   t = type_prim(e->as.lit.is_float ? TY_FLOAT : TY_INT); break;
            case LIT_STR:   t = type_prim(TY_STRING); break;
            case LIT_TRUE:
            case LIT_FALSE: t = type_prim(TY_BOOL);   break;
            default: break;
            }
        }
        else if (e->type == N_CTOR) {
            /* A bare constructor value (`[Red, Green]`): resolve_ctors
             * stamped the enum's name onto the node, so the element type is
             * statically spelled. Same branch as codegen_expr.c's shared
             * copy of this function. */
            t = e->as.ctor.ename ? type_enum(e->as.ctor.ename) : NULL;
        }
        else if (e->type == N_MAP_LIT && e->as.map.is_tuple && g) {
            /* A tuple element `(1, 2)` is a positional anonymous struct: its
             * shape is exactly what infer_node_type computes for it. This is
             * what keeps `ts[0].0` off the list<any> path, where a member read
             * has no static field type to dispatch a getter with. */
            t = infer_node_type(g, e);
        }
        if (!t) return NULL;          /* a call, another list, ... is not static */
        if (et && et->kind != t->kind) return NULL;
        /* Two anonymous shapes unify only field-for-field: `(1, 2)` and
         * `(3, "x")` are both TY_STRUCT but must not melt into one element
         * type — the checker's heterogeneous rule (ck_list) falls back to
         * any, and so does this. */
        if (et && et->kind == TY_STRUCT && !et->name && !t->name) {
            if (et->count != t->count) return NULL;
            for (int j = 0; j < et->count; j++)
                if (!et->types[j] || !t->types[j] ||
                    strcmp(et->names[j], t->names[j]) != 0 ||
                    et->types[j]->kind != t->types[j]->kind) return NULL;
        }
        et = t;
    }
    return et;   /* `[]` has no element kind; it iterates zero times anyway */
}

static Type *infer_node_type(CG *g, Node *n)
{
    if (!n) return NULL;
    switch (n->type) {
    case N_LITERAL:
        switch (n->as.lit.kind) {
        case LIT_NUM:   return type_prim(n->as.lit.is_float ? TY_FLOAT : TY_INT);
        case LIT_STR:   return type_prim(TY_STRING);
        case LIT_TRUE:
        case LIT_FALSE: return type_prim(TY_BOOL);
        case LIT_NULL:  return type_prim(TY_NULL);
        default:        return NULL;
        }
    case N_LET_EXPR:
        /* `let x = e1 in e2` — the expression's type is the body's (the
         * annotation wins when spelled). Prefer the checker's stamp, same
         * reason as codegen_scan.c: a scan-time descent into the body would
         * resolve the binding against the outer scope. */
        if (n->as.letx.annot) return n->as.letx.annot;
        if (n->as.letx.ty) return n->as.letx.ty;
        return infer_node_type(g, n->as.letx.body);
    case N_VAR: {
        Asg *a = asg_find(&g->locals, n->as.var.name, n->line);
        if (a) return a->ty;
        /* A top-level binding lifts to a module global (SPEC 8.1 #10). */
        a = gvar_find(&g->gvars, n->as.var.name);
        if (a) return a->ty;
        /* A match arm whose body is being type-inferred: its binds. */
        return lt_find(&g->arm_scope, n->as.var.name);
    }
    case N_FUNC_LIT: {
        /* A closure value; thread the node through Type.func (mirrors
         * codegen_scan.c). */
        Type *ft = type_func(n->as.funclit.arity, n->as.funclit.param_types,
                             n->as.funclit.ret);
        ft->func = n;
        return ft;
    }
    case N_CALL: {
        if (!n->as.call.callee || n->as.call.callee->type != N_VAR) return NULL;
        const char *name = n->as.call.callee->as.var.name;

        /* Closure-valued variable call: `f(3)` where f : func. */
        {
            Type *ct = infer_node_type(g, n->as.call.callee);
            if (ct && ct->kind == TY_FUNC && ct->func &&
                ct->func->type == N_FUNC_LIT)
                return ct->func->as.funclit.ret;
        }

        /* Higher-order builtins that consume a closure. */
        if (strcmp(name, "map") == 0) {
            Node *clo = n->as.call.argc > 0 ? n->as.call.args[0] : NULL;
            Node *lambda = NULL;
            if (clo && clo->type == N_FUNC_LIT) lambda = clo;
            else if (clo && clo->type == N_VAR) {
                Type *ct = infer_node_type(g, clo);
                if (ct && ct->kind == TY_FUNC && ct->func &&
                    ct->func->type == N_FUNC_LIT)
                    lambda = ct->func;
            }
            if (lambda && lambda->as.funclit.ret)
                return type_list(lambda->as.funclit.ret);
            return type_list(any_type());
        }
        if (strcmp(name, "filter") == 0) {
            Type *lt = infer_node_type(g, n->as.call.args[1]);
            if (lt && lt->kind == TY_LIST) return lt;
            return type_list(any_type());
        }
        if (strcmp(name, "reduce") == 0)
            return infer_node_type(g, n->as.call.args[2]);

        /* len() is always an int, whoever it measures. Same branch as
         * codegen_scan.c's shared copy. */
        if (strcmp(name, "len") == 0 && n->as.call.argc == 1)
            return type_prim(TY_INT);

        /* try(fn) — { ok, err } where ok is the fn's return type and err is
         * null. Carrying the members here is what lets `r.ok` / `r.err` pick
         * the right getter, exactly like a map literal does. Same branch as
         * codegen_scan.c's shared copy. */
        if (strcmp(name, "try") == 0 && n->as.call.argc == 1) {
            Node *fn = n->as.call.args[0];
            Type *ft = NULL;
            if (fn->type == N_FUNC_LIT)
                ft = fn->as.funclit.ret;
            else if (fn->type == N_VAR) {
                Type *ct = infer_node_type(g, fn);
                if (ct && ct->kind == TY_FUNC) ft = ct->ret;
            }
            Type *at = type_anon_struct();
            type_add_member(at, "ok", ft ? ft : any_type());
            type_add_member(at, "err", type_prim(TY_NULL));
            return at;
        }

        /* range(...) builds a list of numbers: all-int arguments iterate as
         * ints, one float argument makes the whole range float (the helper's
         * whole-value rule keeps every element readable either way). An arg
         * without a static numeric type leaves the call untyped — the usual
         * explicit-annotation diagnostic explains. Same branch as
         * codegen_scan.c's shared copy. */
        if (strcmp(name, "range") == 0 &&
            n->as.call.argc >= 1 && n->as.call.argc <= 3) {
            Type *et = type_prim(TY_INT);
            for (int i = 0; i < n->as.call.argc; i++) {
                Type *at = infer_node_type(g, n->as.call.args[i]);
                if (!at || (at->kind != TY_INT && at->kind != TY_FLOAT))
                    return NULL;
                if (at->kind == TY_FLOAT) et = type_prim(TY_FLOAT);
            }
            return type_list(et);
        }

        /* sort(l) returns the same list it sorted, so the result keeps the
         * argument's element type. A non-list argument falls through to
         * NULL — the explicit-annotation path then explains. Kept in step
         * with codegen_scan.c's copy. */
        if (strcmp(name, "sort") == 0 && n->as.call.argc == 1) {
            Type *lt = infer_node_type(g, n->as.call.args[0]);
            if (lt && lt->kind == TY_LIST) return lt;
            return NULL;
        }

        /* json()/stringify() serialize to a JSON string. json(str) means
         * parse, whose result type is dynamic — NULL here keeps the slot
         * opaque and lets the emit-time diagnostic do the explaining. */
        if ((strcmp(name, "stringify") == 0 || strcmp(name, "json") == 0) &&
            n->as.call.argc == 1) {
            Type *at = infer_node_type(g, n->as.call.args[0]);
            if (strcmp(name, "json") == 0 && at && at->kind == TY_STRING)
                return any_type();
            return type_prim(TY_STRING);
        }

        /* get(m, "k"): the value type the key's static shape declares, so
         * `let x = get(m, "n")` infers without an annotation the way the
         * interpreter's dynamic read already does. A key that is not a
         * literal, or that the shape does not declare, falls through to
         * NULL -- the explicit-annotation path then applies. Kept in step
         * with codegen_scan.c's copy. */
        if (strcmp(name, "get") == 0 && n->as.call.argc >= 2) {
            Type *mt = infer_node_type(g, n->as.call.args[0]);
            /* A tagged-any receiver (a json() parse) has no static shape:
             * the result follows the default, or is `any` without one.
             * Kept in step with codegen_scan.c. */
            if (mt && mt->kind == TY_ANY) {
                if (n->as.call.argc == 3)
                    return infer_node_type(g, n->as.call.args[2]);
                return any_type();
            }
            Node *kn = n->as.call.args[1];
            if (kn->type == N_LITERAL && kn->as.lit.kind == LIT_STR) {
                if (mt && mt->kind == TY_STRUCT) {
                    /* A string literal's text keeps its quotes; keys do not. */
                    const char *kt = kn->as.lit.text;
                    size_t kl = (size_t)kn->as.lit.len;
                    if (kl >= 2 && kt[0] == '"' && kt[kl - 1] == '"') { kt++; kl -= 2; }
                    if (mt->name) {
                        char kb[128];
                        if (kl < sizeof kb) {
                            memcpy(kb, kt, kl);
                            kb[kl] = '\0';
                            int idx = struct_field_idx(g, mt->name, kb);
                            if (idx >= 0) {
                                Type *ft = struct_field_type(g, mt->name, idx);
                                if (ft && ft->kind != TY_ANY) return ft;
                            }
                        }
                    } else {
                        for (int i = 0; i < mt->count; i++)
                            if (strlen(mt->names[i]) == kl &&
                                strncmp(mt->names[i], kt, kl) == 0) {
                                Type *ft = mt->types[i];
                                if (ft->kind != TY_ANY) return ft;
                            }
                    }
                }
            }
            /* A key that is not a literal, or that the shape does not
             * declare, falls through to NULL -- the explicit-annotation
             * path then applies (same tail as codegen_scan.c). */
            return NULL;
        }

        Sig *s = sig_find(&g->sigs, name);
        if (!s) return NULL;
        /* `f()?` is the `ok` payload, not a Result -- see the same branch in
         * codegen_scan.c. Reading the declared return type here alloca'd a
         * pointer for an int payload and the emitter then printed the integer
         * through lume_map_print, which segfaulted. */
        if (n->as.call.propagate) return s->ret ? s->ret->elem : NULL;
        return s->ret;
    }
    case N_LIST_LIT: { Type *et = infer_list_elem(g, n); return type_list(et ? et : any_type()); }
    case N_INDEX: {
        /* Kept in step with codegen_scan.c's copy -- the two backends each carry
         * their own infer_node_type, and a kind handled in one but not the other
         * shows up as a backend that refuses what the other accepts. */
        Type *ot = infer_node_type(g, n->as.index.obj);
        if (ot && ot->kind == TY_LIST)
            return ot->elem && ot->elem->kind != TY_ANY ? ot->elem : any_type();
        return type_anon_struct();
    }
    case N_MAP_LIT: {
        /* Kept in step with codegen_scan.c's copy: the literal's key ->
         * value-type shape is what get(m, "k") dispatches its runtime getter
         * on. A value whose own type cannot be inferred lands as `any`. */
        Type *at = type_anon_struct();
        for (int i = 0; i < n->as.map.count; i++) {
            Type *vt = infer_node_type(g, n->as.map.vals[i]);
            type_add_member(at, n->as.map.keys[i], vt ? vt : any_type());
        }
        return at;
    }
    case N_STRUCT_UPDATE: {
        /* Kept in step with codegen_scan.c's copy: a `let` whose init is a
         * `with` update must infer a slot type. Named struct keeps its schema,
         * map becomes a fresh anonymous struct. */
        Type *bt = infer_node_type(g, n->as.supd.base);
        if (bt && bt->kind == TY_STRUCT && bt->name)
            return bt;
        return type_anon_struct();
    }
    case N_MEMBER:   return member_field_type(g, n);
    case N_ASSIGN:
        /* An assignment expression yields the *target*, which is what `for
         * (i = 0; ...)` and `a = b` want to carry. Mirrors codegen_scan.c;
         * without it the LLVM emitter answers "cannot infer a type" where
         * the text emitter (and the interpreter) accept the assignment. */
        return infer_node_type(g, n->as.assign.value);
    case N_ASSIGN_MEMBER:
        return infer_node_type(g, n->as.assign_mem.value);
    case N_UNARY: {
        /* `let neg = -7` / `let b = not flag` must infer a type so the
         * binding's alloca gets an IR type; otherwise the native backend
         * falls back to "cannot infer a type for 'neg'" while the
         * interpreter accepts it (§8.1 #2). */
        Type *ot = infer_node_type(g, n->as.unary.operand);
        if (n->as.unary.op == OP_NOT) return type_prim(TY_BOOL);
        if (!ot) return NULL;
        return type_prim(ot->kind == TY_FLOAT ? TY_FLOAT : TY_INT);
    }
    case N_BINARY: {
        int op = (int)n->as.binary.op;
        if (op == OP_AND || op == OP_OR) return type_prim(TY_BOOL);
        if (op >= OP_EQ && op <= OP_GE)  return type_prim(TY_BOOL);
        Type *lt = infer_node_type(g, n->as.binary.left);
        Type *rt = infer_node_type(g, n->as.binary.right);
        bool lf = lt && lt->kind == TY_FLOAT;
        bool rf = rt && rt->kind == TY_FLOAT;
        /* Catch float % at inference so the libLLVM backend rejects it with
         * the same "'%' does not apply to floats" the text backend and the
         * interpreter use, instead of a downstream "print() cannot print
         * this value" (§8.1 #1). */
        if (op == OP_MOD && (lf || rf))
            ERR(g, "line %zu: '%%' does not apply to floats", n->line);
        if (op == OP_ADD && lt && rt &&
            lt->kind == TY_STRING && rt->kind == TY_STRING)
            return type_prim(TY_STRING);
        if (lf || rf) return type_prim(TY_FLOAT);
        return type_prim(TY_INT);
    }
    case N_TERNARY: {
        /* 与 codegen_scan.c / typecheck_expr.c 同一套统一规则：同型→它，
         * int/float 混合→float，任一 any/未知→any。cg_ternary 拿它给
         * 结果 alloca 定型。 */
        Type *tt = infer_node_type(g, n->as.ternary.then);
        Type *et = infer_node_type(g, n->as.ternary.els);
        if (!tt || !et || tt->kind == TY_ANY || et->kind == TY_ANY)
            return any_type();
        if (tt->kind == et->kind)
            return tt;
        bool tf = tt->kind == TY_INT || tt->kind == TY_FLOAT;
        bool ef = et->kind == TY_INT || et->kind == TY_FLOAT;
        if (tf && ef)
            return type_prim(TY_FLOAT);
        return NULL;
    }
    case N_CTOR:
        /* 构造值的类型 = 它的 enum（resolve_ctors 盖了 ename）。 */
        return n->as.ctor.ename ? type_enum(n->as.ctor.ename) : NULL;
    case N_MATCH: {
        /* 臂体按三元的统一规则收拢——与 codegen_scan.c 同一套。臂内绑定
         * 只在它自己的臂里可见（arm_scope 推断窗口，嵌套 match 恢复外层
         * 视图，最外层收尾释放）。 */
        int saved = g->arm_scope.n;
        Type *result = NULL;
        for (int i = 0; i < n->as.matchn.armc; i++) {
            MatchArm *arm = &n->as.matchn.arms[i];
            arm_scope_truncate(g, saved);   /* drop the previous arm's binds */
            /* the arm's single binding set: the checker guarantees every
             * alternative binds the same names with the same types */
            MatchPat *p0 = &arm->pats[0];
            for (int b = 0; b < p0->bindc; b++)
                arm_scope_push(g, p0->binds[b],
                               p0->vfield_types ? p0->vfield_types[b] : NULL);
            Type *bt = infer_node_type(g, arm->body);
            /* Same rule as codegen_scan.c: an arm with no statically
             * readable type is skipped (its real type exists at emit time);
             * only an explicit `any` arm widens the match. */
            if (!bt) continue;
            if (bt->kind == TY_ANY) { result = any_type(); break; }
            if (!result) { result = bt; continue; }
            if (result->kind == bt->kind) continue;
            bool r_num = result->kind == TY_INT || result->kind == TY_FLOAT;
            bool b_num = bt->kind == TY_INT || bt->kind == TY_FLOAT;
            if (r_num && b_num) { result = type_prim(TY_FLOAT); continue; }
            result = NULL;
            break;
        }
        arm_scope_truncate(g, saved);
        if (saved == 0) arm_scope_clear(g);
        return result ? result : any_type();
    }
    default:
        return NULL;
    }
}

static LLVMValueRef gvar_make(CG *g, const char *name, Type *ty)
{
    static int s_uid = 0;
    char buf[128];
    snprintf(buf, sizeof buf, "lv_%s_%d", name, s_uid++);
    LLVMTypeRef lt = ty_of(g, ty);
    if (!lt) return NULL;
    LLVMValueRef gv = LLVMAddGlobal(g->mod, lt, buf);
    LLVMSetInitializer(gv, LLVMConstNull(lt));
    return gv;
}

static void scan_stmt(CG *g, Node *n)
{
    if (!n) return;

    switch (n->type) {
    case N_LET: {
        Type *ty = n->as.let.annot ? n->as.let.annot : infer_node_type(g, n->as.let.init);
        if (!ty)
            ERRX(g, "line %zu: cannot infer a type for '%s' (add an explicit annotation)",
                 n->line, n->as.let.name);
        /* `let f: fn(int) => int = (x) => x * 2;` (SPEC §4.8): the annotated
         * TY_FUNC carries no back-pointer of its own, but `f(3)` recovers the
         * lambda through Type.func — thread the literal through so the call
         * emitter can pin the parameter types. Mirrors codegen_scan.c's
         * N_LET; without it the llvm backend refuses what the text backend
         * accepts. */
        if (ty->kind == TY_FUNC && !ty->func && n->as.let.init &&
            n->as.let.init->type == N_FUNC_LIT)
            ty->func = n->as.let.init;
        if (g->scanning_top) {
            /* Module storage, registered twice (mirrors codegen_scan.c):
             * gvars for what function bodies resolve through, L_top's own
             * locals for what the top level's stores hit. */
            LLVMValueRef slot = gvar_make(g, n->as.let.name, ty);
            if (!slot)
                ERRX(g, "line %zu: top-level variable '%s' has no codegen type",
                     n->line, n->as.let.name);
            asg_push(&g->gvars, n->as.let.name, ty, slot, n->line);
            asg_push(&g->locals, n->as.let.name, ty, slot, n->line);
            break;
        }
        asg_push(&g->locals, n->as.let.name, ty, NULL, n->line);
        break;
    }
    case N_LET_PAT: {
        /* One binding per pattern name, registered exactly like a `let`
         * (mirrors codegen_scan.c): a top-level name becomes module
         * storage, a function-local name a plain local. The types were
         * stamped by the checker; a NULL stamp means the map's shape was
         * unknown, which has no codegen type — reported here because a
         * binding needs a slot. */
        for (int i = 0; i < n->as.letpat.count; i++) {
            Type *ty = n->as.letpat.types[i];
            const char *nm = n->as.letpat.names[i];
            if (!ty)
                ERRX(g, "line %zu: cannot infer a type for '%s' (the "
                        "destructured map has no known shape)",
                     n->line, nm);
            if (g->scanning_top) {
                LLVMValueRef slot = gvar_make(g, nm, ty);
                if (!slot)
                    ERRX(g, "line %zu: top-level variable '%s' has no "
                            "codegen type", n->line, nm);
                asg_push(&g->gvars, nm, ty, slot, n->line);
                asg_push(&g->locals, nm, ty, slot, n->line);
            } else {
                asg_push(&g->locals, nm, ty, NULL, n->line);
            }
        }
        break;
    }
    case N_FOR: {
        /* `for (x in xs)` binds the loop variable — it is not a `let`, so
         * without registering it here the body's use of it dies in codegen.
         * It binds in the enclosing scope (mirrors codegen_scan.c). At the
         * top level that scope is the module: a function the top level calls
         * after the loop must see the rebound value. */
        if (n->as.fors.is_in) {
            Type *et = for_in_elem_type(g, n);
            if (g->scanning_top) {
                LLVMValueRef slot = gvar_make(g, n->as.fors.var, et);
                if (!slot)
                    ERRX(g, "line %zu: variable '%s' has no codegen type",
                         n->line, n->as.fors.var);
                asg_push(&g->gvars, n->as.fors.var, et, slot, n->line);
                asg_push(&g->locals, n->as.fors.var, et, slot, n->line);
            } else {
                asg_push(&g->locals, n->as.fors.var, for_in_elem_type(g, n), NULL,
                         n->line);
            }
            scan_stmt(g, n->as.fors.body);
            break;
        }
        Node *init = n->as.fors.init;
        if (init && init->type == N_LET) {
            Type *ty = init->as.let.annot ? init->as.let.annot
                                          : infer_node_type(g, init->as.let.init);
            if (!ty)
                ERRX(g, "line %zu: cannot infer a type for '%s' (add an explicit annotation)",
                     init->line, init->as.let.name);
            /* A C-style for's init `let` stays a plain local (mirrors
             * codegen_scan.c: the interpreter binds it for the loop). */
            asg_push(&g->locals, init->as.let.name, ty, NULL, init->line);
        } else if (init) {
            scan_stmt(g, init);
        }
        if (n->as.fors.incr &&
            (n->as.fors.incr->type == N_ASSIGN ||
             n->as.fors.incr->type == N_EXPR_STMT))
            scan_stmt(g, n->as.fors.incr);
        scan_stmt(g, n->as.fors.body);
        break;
    }
    /* An assignment to a name that is not a local defines a fresh
     * function-local cell (the interpreter never assigns through to a
     * top-level binding) — register it so the alloca pass sees it. Mirrors
     * codegen_scan.c's N_ASSIGN case. */
    case N_ASSIGN: {
        if (!g->scanning_top &&
            !asg_find(&g->locals, n->as.assign.name, n->line)) {
            Asg *gv = gvar_find(&g->gvars, n->as.assign.name);
            if (gv) {
                Type *ty = infer_node_type(g, n->as.assign.value);
                /* shadow-marked: the entry block seeds this local from the
                 * module global, matching the interpreter's call-time env
                 * lookup (mirrors codegen_scan.c). */
                asg_push_shadow(&g->locals, n->as.assign.name,
                                ty ? ty : gv->ty, NULL, n->line);
            }
        }
        break;
    }
    /* A bare `x = e;` wraps its assignment in an expr-stmt: unwrap so the
     * registration above can fire (mirrors codegen_scan.c). */
    case N_EXPR_STMT: {
        Node *e = n->as.expr_stmt.expr;
        if (e && (e->type == N_ASSIGN || e->type == N_ASSIGN_MEMBER))
            scan_stmt(g, e);
        break;
    }
    /* A nested block is the body of if/while/for and any bare { } — its `let`s
     * have to be registered too, or every use of such a local dies in codegen
     * ("bad operands" on a read, "unknown variable" on an assignment). */
    case N_BLOCK:  scan_block(g, n); break;
    case N_IF:     scan_stmt(g, n->as.ifs.then); scan_stmt(g, n->as.ifs.els); break;
    /* if-let binds are registered at emit time (like match arms); the
     * branches still need walking for their lets/assignments. */
    case N_IF_LET: scan_stmt(g, n->as.iflet.then); scan_stmt(g, n->as.iflet.els); break;
    case N_WHILE:  scan_stmt(g, n->as.whiles.body); break;
    default: break;
    }
}

static void scan_block(CG *g, Node *blk)
{
    if (!blk) return;
    for (int i = 0; i < blk->as.block.count; i++) scan_stmt(g, blk->as.block.stmts[i]);
}
