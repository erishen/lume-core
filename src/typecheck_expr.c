/* 表达式类型检查:ck_expr / ck_list——从 typecheck.c 拆出(2026-09-27)。
 * 覆盖字面量、变量、map/list 字面量、函数字面量、成员访问、
 * 一元/二元运算与调用(含 Result `?` 传播规则)。 */

#include "typecheck_internal.h"

static Type *ck_list(Checker *c, Node *n, Type *expected) {
    if (n->as.list.count == 0) {
        /* `[]` — type comes from the expected annotation, else any[] */
        if (expected && expected->kind == TY_LIST) return expected;
        return type_list(any_type());
    }
    /* feed the first element the expected element type, then unify others */
    Type *elem = expected && expected->kind == TY_LIST ? expected->elem : NULL;
    Type *t0 = ck_expr(c, n->as.list.items[0], elem);
    expect_compat(c, t0, elem, n->as.list.items[0]->line, "list element");
    for (int i = 1; i < n->as.list.count; i++) {
        Type *ti = ck_expr(c, n->as.list.items[i], elem);
        expect_compat(c, ti, elem, n->as.list.items[i]->line, "list element");
        if (!type_compat(c, ti, t0, n->as.list.items[i]->line) &&
            !type_compat(c, t0, ti, n->as.list.items[i]->line)) {
            if (elem) {   /* annotated lists stay homogeneous */
                ck_fail(c, n->line, "list literal has incompatible element types");
                return type_list(any_type());
            }
            /* un-annotated heterogeneous list is fine; the element type
             * falls back to any (needed for ? parameter lists, e.g.
             * sql_write(sql, [3, "c"])). */
            t0 = any_type();
        }
    }
    if (elem) return expected;   /* annotated list: honor the expected type */
    return type_list(t0);
}

Type *ck_expr(Checker *c, Node *n, Type *expected) {
    if (c->failed) return any_type();
    if (!n) return any_type();

    switch (n->type) {
        case N_LITERAL:
            switch (n->as.lit.kind) {
                case LIT_NUM:
                    return n->as.lit.is_float ? type_prim(TY_FLOAT)
                                              : type_prim(TY_INT);
                case LIT_STR:  return type_prim(TY_STRING);
                case LIT_TRUE:
                case LIT_FALSE: return type_prim(TY_BOOL);
                case LIT_NULL:  return type_prim(TY_NULL);
            }
            return any_type();

        case N_VAR: {
            Type *t = scope_get(c->scope, n->as.var.name);
            if (!t) {
                ck_fail(c, n->line, "undefined variable '%s'", n->as.var.name);
                return any_type();
            }
            return t;
        }

        case N_MAP_LIT: {
            /* single-key { ok: .. } / { err: .. } literals are Results */
            if (n->as.map.count == 1 &&
                (strcmp(n->as.map.keys[0], "ok") == 0 ||
                 strcmp(n->as.map.keys[0], "err") == 0)) {
                ck_expr(c, n->as.map.vals[0], NULL);
                return type_result();
            }
            /* duplicate keys are almost certainly a bug */
            for (int i = 0; i < n->as.map.count; i++)
                for (int j = i + 1; j < n->as.map.count; j++)
                    if (strcmp(n->as.map.keys[i], n->as.map.keys[j]) == 0)
                        ck_fail(c, n->line, "duplicate key '%s' in map literal",
                                n->as.map.keys[i]);

            Type *sel = expected ? resolve(c, expected, n->line) : NULL;
            if (sel && sel->kind == TY_STRUCT && sel->name) {
                /* strict struct: every literal key must be a declared field */
                for (int i = 0; i < n->as.map.count; i++) {
                    int idx = -1;
                    for (int j = 0; j < sel->count; j++)
                        if (strcmp(n->as.map.keys[i], sel->names[j]) == 0) {
                            idx = j;
                            break;
                        }
                    if (idx < 0) {
                        ck_fail(c, n->line, "type %s has no field '%s'",
                                sel->name, n->as.map.keys[i]);
                        continue;
                    }
                    expect_compat(c,
                                  ck_expr(c, n->as.map.vals[i],
                                           sel->types[idx]),
                                  sel->types[idx], n->line, "struct field");
                }
                /* ...and every declared field must be present, otherwise the
                 * per-field runtime lookup would fail on the missing key. */
                for (int j = 0; j < sel->count; j++) {
                    bool present = false;
                    for (int i = 0; i < n->as.map.count; i++)
                        if (strcmp(n->as.map.keys[i], sel->names[j]) == 0) {
                            present = true;
                            break;
                        }
                    if (!present)
                        ck_fail(c, n->line, "type %s is missing field '%s'",
                                sel->name, sel->names[j]);
                }
                return sel;
            }

            /* anonymous struct literal */
            Type *at = type_anon_struct();
            for (int i = 0; i < n->as.map.count; i++) {
                Type *vt = ck_expr(c, n->as.map.vals[i], NULL);
                type_add_member(at, n->as.map.keys[i], vt);
            }
            return at;
        }

        case N_LIST_LIT:
            return ck_list(c, n, expected);

        case N_FUNC_LIT: {
            Type **params = calloc((size_t)n->as.funclit.arity + 1,
                                   sizeof(Type *));
            Type *hint = c->param_hint;
            c->param_hint = NULL; /* one-shot */
            for (int i = 0; i < n->as.funclit.arity; i++)
                params[i] = (hint && i == 0 && !n->as.funclit.param_types[i])
                                ? hint
                                : (n->as.funclit.param_types[i]
                                       ? resolve(c, n->as.funclit.param_types[i],
                                                 n->line)
                                       : any_type());
            Type *ret = n->as.funclit.ret ? resolve(c, n->as.funclit.ret,
                                                    n->line) : any_type();
            Type *ft = type_func(n->as.funclit.arity, params, ret);
            ck_fn(c, n->as.funclit.names, ft, n->as.funclit.body);
            return ft;
        }

        case N_MEMBER: {
            /* `ns.member` — the base is a module namespace */
            if (n->as.member.obj->type == N_VAR) {
                Module *nsmod = scope_get_ns(c->scope, n->as.member.obj->as.var.name);
                if (nsmod) {
                    for (int i = 0; i < nsmod->export_types.count; i++)
                        if (strcmp(nsmod->export_types.names[i],
                                   n->as.member.name) == 0) {
                            n->as.member.type = nsmod->export_types.types[i];
                            return nsmod->export_types.types[i];
                        }
                    ck_fail(c, n->line, "module '%s' has no export '%s'",
                            n->as.member.obj->as.var.name, n->as.member.name);
                    return any_type();
                }
            }
            Type *ot = ck_expr(c, n->as.member.obj, NULL);
            ot = resolve(c, ot, n->line);
            if (ot->kind == TY_ANY) return any_type();
            /* `.len` / `.length` is a property of the container, not a field:
             * a string, a list, and an anonymous struct (a runtime map) all
             * answer it. Checked *before* the struct walk because both native
             * backends dispatch it the same way; a *named* struct that really
             * declares a `len` field still reads its field below. */
            if ((ot->kind == TY_STRING || ot->kind == TY_LIST ||
                 (ot->kind == TY_STRUCT && !ot->name)) &&
                (strcmp(n->as.member.name, "len") == 0 ||
                 strcmp(n->as.member.name, "length") == 0))
                return type_prim(TY_INT);
            if (ot->kind == TY_STRUCT) {
                for (int i = 0; i < ot->count; i++)
                    if (strcmp(n->as.member.name, ot->names[i]) == 0) {
                        /* Sticky type annotation: the interpreter returns the
                         * member's zero value on a missing key (tool arg). */
                        n->as.member.type = ot->types[i];
                        return ot->types[i];
                    }
                ck_fail(c, n->line, "type has no field '%s'",
                        n->as.member.name);
                return any_type();
            }
            ck_fail(c, n->line, "cannot read field '%s' on this type",
                    n->as.member.name);
            return any_type();
        }

        case N_UNARY:
            if (n->as.unary.op == OP_NOT) {
                Type *ot = ck_expr(c, n->as.unary.operand, NULL);
                is_bool_ok(c, ot, n->line);
                /* `!x` is bool. This branch has to leave the switch by a
                 * return on every path: an unguarded fall-through would
                 * re-enter N_BINARY, where the shared union slot makes
                 * binary.op == OP_NOT and binary.left == this operand, while
                 * binary.right reads an unrelated slot as NULL. Arithmetic
                 * then calls arith_result(operand, NULL), which short-circuits
                 * to any_type() — so `!b` stopped being bool and type-loose
                 * assignments like `let i: int = !b;` checked clean. */
                return type_prim(TY_BOOL);
            } else { /* OP_NEG */
                Type *ot = ck_expr(c, n->as.unary.operand, NULL);
                if (ot && ot->kind != TY_ANY && ot->kind != TY_INT &&
                    ot->kind != TY_FLOAT)
                    ck_fail(c, n->line, "cannot negate this type");
                /* negation keeps the numeric type (-5 is int, -5.5 float) */
                if (ot && ot->kind == TY_FLOAT) return type_prim(TY_FLOAT);
                if (ot && ot->kind == TY_ANY) return any_type();
                return type_prim(TY_INT);
            }

        case N_BINARY: {
            Op op = n->as.binary.op;
            if (op == OP_AND || op == OP_OR) {
                Type *lt = ck_expr(c, n->as.binary.left, NULL);
                Type *rt = ck_expr(c, n->as.binary.right, NULL);
                is_bool_ok(c, lt, n->line);
                is_bool_ok(c, rt, n->line);
                return type_prim(TY_BOOL);
            }
            if (op == OP_EQ || op == OP_NE) {
                ck_expr(c, n->as.binary.left, NULL);
                ck_expr(c, n->as.binary.right, NULL);
                return type_prim(TY_BOOL);
            }
            if (op == OP_LT || op == OP_LE || op == OP_GT || op == OP_GE) {
                Type *lt = ck_expr(c, n->as.binary.left, NULL);
                Type *rt = ck_expr(c, n->as.binary.right, NULL);
                /* comparison is numeric or matching strings, never mixed */
                if (lt && rt && lt->kind != TY_ANY && rt->kind != TY_ANY) {
                    bool a_num = lt->kind == TY_INT || lt->kind == TY_FLOAT;
                    bool b_num = rt->kind == TY_INT || rt->kind == TY_FLOAT;
                    bool a_str = lt->kind == TY_STRING;
                    bool b_str = rt->kind == TY_STRING;
                    if (!((a_num && b_num) || (a_str && b_str)))
                        ck_fail(c, n->line,
                                "comparison needs numbers or strings");
                }
                return type_prim(TY_BOOL);
            }
            /* arithmetic */
            Type *lt = ck_expr(c, n->as.binary.left, NULL);
            Type *rt = ck_expr(c, n->as.binary.right, NULL);
            if (op == OP_ADD) {
                /* string + string concatenates */
                if (lt && rt &&
                    (lt->kind == TY_STRING || lt->kind == TY_ANY) &&
                    (rt->kind == TY_STRING || rt->kind == TY_ANY))
                    return lt->kind == TY_STRING ? type_prim(TY_STRING)
                                                 : any_type();
            }
            if (lt && rt && lt->kind != TY_ANY && rt->kind != TY_ANY &&
                !type_compat(c, lt, type_prim(TY_FLOAT), n->line))
                ck_fail(c, n->line, "arithmetic needs numbers");
            return arith_result(lt, rt);
        }

        case N_CALL: {
            Type *callee_t = ck_expr(c, n->as.call.callee, NULL);
            callee_t = resolve(c, callee_t, n->line);
            if (callee_t->kind == TY_ANY) {
                for (int i = 0; i < n->as.call.argc; i++)
                    ck_expr(c, n->as.call.args[i], NULL);
                return any_type();
            }
            if (callee_t->kind != TY_FUNC) {
                ck_fail(c, n->line, "calling a non-function value");
                for (int i = 0; i < n->as.call.argc; i++)
                    ck_expr(c, n->as.call.args[i], NULL);
                return any_type();
            }
            if (callee_t->count != n->as.call.argc) {
                ck_fail(c, n->line, "function expects %d arguments, got %d",
                        callee_t->count, n->as.call.argc);
                return any_type();
            }
            for (int i = 0; i < n->as.call.argc; i++)
                expect_compat(c, ck_expr(c, n->as.call.args[i],
                                         callee_t->types[i]),
                              callee_t->types[i], n->line, "argument");

            Type *ret = callee_t->ret ? callee_t->ret : any_type();
            if (n->as.call.propagate) {
                Type *src = callee_t->ret;
                if (!src || (src->kind != TY_RESULT && src->kind != TY_ANY))
                    ck_fail(c, n->line, "'?' used on a call that does not return Result");
                if (!c->in_func)
                    ck_fail(c, n->line, "'?' is only allowed inside a function");
                if (c->cur_ret && c->cur_ret->kind != TY_RESULT &&
                    c->cur_ret->kind != TY_ANY)
                    ck_fail(c, n->line,
                            "'?' needs the enclosing function to return Result");
                return any_type(); /* payload type is unknown without generics */
            }
            return ret;
        }

        default:
            return any_type(); /* non-expression encountered while atom-checking */
    }
}
