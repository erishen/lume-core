/* 表达式类型检查:ck_expr / ck_list——从 typecheck.c 拆出(2026-09-27)。
 * 覆盖字面量、变量、map/list 字面量、函数字面量、成员访问、
 * 一元/二元运算与调用(含 Result `?` 传播规则)。 */

#include "typecheck_internal.h"
#include "arena.h"

/* Equality classes for `==` / `!=` (SPEC 1.8). Two operands in different
 * classes are never comparable: natively that shape is invalid IR (icmp i64
 * against a ptr), and in the interpreter it was a quiet false. `null` gets
 * its own class because it is an i8* natively — comparable with the other
 * i8*-lowered kinds, never with a scalar. TY_ANY (and anything unknown)
 * returns -1: the wildcard stays permissive, the runtime decides. */
enum { EQ_NUM, EQ_STR, EQ_BOOL, EQ_CTR, EQ_FUNC, EQ_NULL, EQ_CLASSES };

static int eq_class(Type *t) {
    if (!t) return -1;
    switch (t->kind) {
    case TY_INT: case TY_FLOAT:                   return EQ_NUM;
    case TY_STRING:                               return EQ_STR;
    case TY_BOOL:                                 return EQ_BOOL;
    case TY_LIST: case TY_STRUCT:
    case TY_ENUM: case TY_RESULT:                 return EQ_CTR;
    case TY_FUNC:                                 return EQ_FUNC;
    case TY_NULL:                                 return EQ_NULL;
    default:                                      return -1;  /* any / unknown */
    }
}

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
                    /* neg_min still set here means the 2^63 magnitude reached
                     * the checker without a unary minus in front of it, so
                     * nothing ever supplied the negation and no i64 can hold
                     * the value. parse_unary clears the flag when the minus
                     * is there. */
                    if (n->as.lit.neg_min)
                        ck_fail(c, n->line,
                                "integer literal 9223372036854775808 does not fit in int "
                                "(i64); write -9223372036854775808 for the minimum");
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
                Type *payload = ck_expr(c, n->as.map.vals[0], NULL);
                Type *rt = type_result();
                /* Only the `ok` half is the payload `?` unwraps. An `err`
                 * literal says nothing about it, and a function that only ever
                 * fails leaves it unknown -- which the emitters report rather
                 * than guess. */
                if (strcmp(n->as.map.keys[0], "ok") == 0)
                    record_ok_payload(c, rt, payload, n->line);
                return rt;
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

        case N_STRUCT_UPDATE: {
            /* `{ base with k: v, ... }` — a fresh map/struct. The base keeps
             * its declared shape: a named struct has a fixed field set (Lean's
             * rule too — `with` overrides values, never the schema), an
             * anonymous map may gain keys. Anything that is not a map/struct
             * (scalar, list, func, enum, Result) is refused here rather than
             * at runtime. */
            Type *bt = ck_expr(c, n->as.supd.base, expected);
            if (bt->kind != TY_STRUCT && bt->kind != TY_ANY) {
                ck_fail(c, n->line,
                        "'with' update requires a map or struct base, got %s",
                        ty_str(bt));
                for (int i = 0; i < n->as.supd.count; i++)
                    ck_expr(c, n->as.supd.vals[i], NULL);
                return any_type();
            }
            if (bt->kind == TY_ANY) {
                for (int i = 0; i < n->as.supd.count; i++)
                    ck_expr(c, n->as.supd.vals[i], NULL);
                return any_type();
            }
            if (bt->name) {
                /* named struct: every key must be a declared field */
                for (int i = 0; i < n->as.supd.count; i++) {
                    int idx = -1;
                    for (int j = 0; j < bt->count; j++)
                        if (strcmp(n->as.supd.keys[i], bt->names[j]) == 0) {
                            idx = j;
                            break;
                        }
                    if (idx < 0) {
                        ck_fail(c, n->line,
                                "unknown field '%s' in struct update (%s)",
                                n->as.supd.keys[i], bt->name);
                        ck_expr(c, n->as.supd.vals[i], NULL);
                        continue;
                    }
                    expect_compat(c,
                                  ck_expr(c, n->as.supd.vals[i],
                                           bt->types[idx]),
                                  bt->types[idx], n->line, "struct field");
                }
                return bt;
            }
            /* anonymous map: the base's members plus the with-overrides */
            Type *at = type_anon_struct();
            for (int j = 0; j < bt->count; j++)
                type_add_member(at, bt->names[j], bt->types[j]);
            for (int i = 0; i < n->as.supd.count; i++) {
                Type *vt = ck_expr(c, n->as.supd.vals[i], NULL);
                bool found = false;
                for (int j = 0; j < at->count; j++)
                    if (strcmp(n->as.supd.keys[i], at->names[j]) == 0) {
                        at->types[j] = vt;
                        found = true;
                        break;
                    }
                if (!found) type_add_member(at, n->as.supd.keys[i], vt);
            }
            return at;
        }

        case N_LIST_LIT:
            return ck_list(c, n, expected);

        case N_FUNC_LIT: {
            /* Scratch resolved-param vector; type_func() copies it out, so
             * it lives in the arena and is abandoned (TODO.md P2). */
            Type **params = arena_alloc(((size_t)n->as.funclit.arity + 1)
                                        * sizeof(Type *));
            memset(params, 0,
                   ((size_t)n->as.funclit.arity + 1) * sizeof(Type *));
            Type *hint = c->param_hint;
            c->param_hint = NULL; /* one-shot */
            /* A `fn (...) => ...` annotation on the binding (`let f: fn(int)
             * => int = (x) => x * 2;`, SPEC §4.8) spells the lambda's
             * parameter types: use them for every unannotated parameter, not
             * just the map/filter/reduce one-shot hint. */
            Type *exp_fn = (expected && expected->kind == TY_FUNC) ? expected : NULL;
            for (int i = 0; i < n->as.funclit.arity; i++)
                params[i] = (!n->as.funclit.param_types[i] && exp_fn &&
                             i < exp_fn->count)
                                ? resolve(c, exp_fn->types[i], n->line)
                                : (hint && i == 0 && !n->as.funclit.param_types[i])
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

        case N_INDEX: {
            /* `m["k"]` / `l[0]`. The container decides which accessor reads
             * it, so the index type is checked against the container and the
             * result type comes back from the element. Both cases are strict:
             * a missing key or an out-of-range index is a runtime error, not a
             * silent null -- see SPEC 6.2. */
            Type *ot = resolve(c, ck_expr(c, n->as.index.obj, NULL), n->line);
            if (ot->kind == TY_ANY) { ck_expr(c, n->as.index.index, NULL); return any_type(); }

            if (ot->kind == TY_LIST) {
                /* The index has to be an int: `l["x"]` is a type error, not a
                 * runtime one, so it cannot reach the accessor. */
                Type *it = ck_expr(c, n->as.index.index, NULL);
                it = resolve(c, it, n->line);
                if (it->kind != TY_ANY && it->kind != TY_INT)
                    ck_fail(c, n->line, "a list index must be an int, got '%s'",
                            ty_str(it));
                /* A negative literal is allowed (it counts from the end), so
                 * the sign is not checked here -- rt.c normalises it. Any int
                 * expression is accepted; whether the value is in range is a
                 * runtime question, exactly as for `el`. */
                return ot->elem && ot->elem->kind != TY_ANY ? ot->elem : any_type();
            }
            if (ot->kind == TY_STRUCT && !ot->name) {
                /* A runtime map: the key must be a string, since that is what
                 * `get` takes. */
                Type *kt = ck_expr(c, n->as.index.index, NULL);
                kt = resolve(c, kt, n->line);
                if (kt->kind != TY_ANY && kt->kind != TY_STRING)
                    ck_fail(c, n->line, "a map key must be a string, got '%s'",
                            ty_str(kt));
                /* The value type is not knowable from the literal -- a map's
                 * values are whatever was put in -- so any() it is. */
                return any_type();
            }
            ck_expr(c, n->as.index.index, NULL);
            ck_fail(c, n->line, "cannot index into a value of type '%s'",
                    ty_str(ot));
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
                Type *lt = ck_expr(c, n->as.binary.left, NULL);
                Type *rt = ck_expr(c, n->as.binary.right, NULL);
                /* `==` used to accept any two operands and let the runtime
                 * answer — which for mixed classes meant "invalid IR" natively
                 * (icmp i64 against a ptr) and a quiet false in the
                 * interpreter. Structural equality (SPEC 1.8) needs a static
                 * gate: values in different equality classes are never
                 * comparable, and `null` — an i8* natively — only compares
                 * against other i8*-lowered kinds (an int is never null: the
                 * native backends already refuse to build one). */
                int lc = eq_class(lt), rc = eq_class(rt);
                bool ok = true;
                if (lc >= 0 && rc >= 0) {
                    if (lc == EQ_NULL || rc == EQ_NULL) {
                        int other = lc == EQ_NULL ? rc : lc;
                        ok = other != EQ_NUM && other != EQ_BOOL;
                    } else {
                        ok = lc == rc;
                    }
                }
                if (!ok)
                    ck_fail(c, n->line, "'%s' does not apply to %s and %s",
                            op == OP_EQ ? "==" : "!=",
                            ty_str(lt), ty_str(rt));
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
            /* Both sides must be numeric; the string-concat case is handled
             * above. A mixed numeric/string operand (e.g. `1 + "x"`) is a type
             * error, not string concat. The numeric check must inspect BOTH
             * sides — the old code only looked at the left, so `1 + "x"`
             * slipped through --check. */
            if (lt && rt && lt->kind != TY_ANY && rt->kind != TY_ANY &&
                (!type_compat(c, lt, type_prim(TY_FLOAT), n->line) ||
                 !type_compat(c, rt, type_prim(TY_FLOAT), n->line)))
                ck_fail(c, n->line, "arithmetic needs numbers");
            return arith_result(lt, rt);
        }

        case N_TERNARY: {
            /* 条件要 bool-ok；两分支各自检查后统一：同型→它，int/float
             * 混合→float（与算术提升一致），任一 any→any，其余报错。 */
            Type *ct = ck_expr(c, n->as.ternary.cond, NULL);
            is_bool_ok(c, ct, n->line);
            Type *tt = ck_expr(c, n->as.ternary.then, NULL);
            Type *et = ck_expr(c, n->as.ternary.els, NULL);
            if (!tt || !et || tt->kind == TY_ANY || et->kind == TY_ANY)
                return any_type();
            if (tt->kind == et->kind)
                return tt;
            bool t_num = tt->kind == TY_INT || tt->kind == TY_FLOAT;
            bool e_num = et->kind == TY_INT || et->kind == TY_FLOAT;
            if (t_num && e_num)
                return type_prim(TY_FLOAT);
            ck_fail(c, n->line,
                    "ternary branches have different types ('%s' vs '%s')",
                    ty_str(tt), ty_str(et));
            return any_type();
        }

        case N_LET_EXPR: {
            /* `let x = e1 in e2` — e1 checks against the annotation (if
             * any) and its type (or the annotation) becomes x's type in a
             * child scope that lives exactly as long as e2; the whole
             * expression's type is e2's. The child scope is created fresh
             * on every walk (pass D walks statements twice), so re-declaring
             * into it can never trip scope_decl's same-scope duplicate
             * check — unlike a statement `let`, no collecting branch is
             * needed here. */
            ck_ctor_shadow(c, n->as.letx.name, n->line);
            Type *annot = n->as.letx.annot
                              ? resolve(c, n->as.letx.annot, n->line) : NULL;
            /* Write the resolved annotation back for the native emitters,
             * same write-back a statement let does. */
            if (n->as.letx.annot) n->as.letx.annot = annot;
            Type *it = ck_expr(c, n->as.letx.init, annot);
            if (annot && it && !type_compat(c, it, annot, n->line)) {
                ck_fail(c, n->line,
                        "'%s' is not assignable to the declared type '%s' of '%s'",
                        ty_str(it), ty_str(annot), n->as.letx.name);
            }
            CScope *saved = c->scope;
            c->scope = scope_new(saved);
            scope_decl(c, c->scope, n->as.letx.name,
                       annot ? annot : (it ? it : any_type()), n->line);
            Type *bt = ck_expr(c, n->as.letx.body, expected);
            c->scope = saved;
            /* An assignment body yields the assigned value, but ck_expr has
             * no N_ASSIGN/N_ASSIGN_MEMBER case — the default answers any,
             * which would mistype the whole expression as opaque i8* storage.
             * Both emitters coerce an assignment's result to the target
             * slot's type (the binding's own), so that is the honest type
             * here. A genuinely-any body keeps its any: the binding's type
             * is derived from the same init, so the fallback can only narrow
             * the checker's non-answer, never a real one. */
            if (bt && bt->kind == TY_ANY && n->as.letx.body &&
                (n->as.letx.body->type == N_ASSIGN ||
                 n->as.letx.body->type == N_ASSIGN_MEMBER))
                bt = (annot ? annot : it);
            /* Stamp the result for the native backends' infer_node_type: the
             * emitters' tables have no child scope, so a scan-time infer that
             * descended into the body would resolve the binding's name
             * against the OUTER scope — an outer same-name binding of a
             * different type would then disagree with what the emitter
             * stores (the resolve-vs-infer split this stamp closes). */
            n->as.letx.ty = bt;
            return bt ? bt : (annot ? annot : (it ? it : any_type()));
        }

        case N_CTOR: {
            /* A rewritten constructor application (parser.c). Arity and
             * payload types come from the enum table; the result is the
             * enum's own type. */
            int vi = -1;
            EnumDef *ed = find_ctor(c, n->as.ctor.name, &vi);
            if (!ed) {
                ck_fail(c, n->line, "unknown constructor '%s'",
                        n->as.ctor.name);
                for (int i = 0; i < n->as.ctor.argc; i++)
                    ck_expr(c, n->as.ctor.args[i], NULL);
                return any_type();
            }
            if (ed->vfield_counts[vi] != n->as.ctor.argc) {
                ck_fail(c, n->line,
                        "constructor '%s' expects %d argument(s), got %d",
                        n->as.ctor.name, ed->vfield_counts[vi],
                        n->as.ctor.argc);
                for (int i = 0; i < n->as.ctor.argc; i++)
                    ck_expr(c, n->as.ctor.args[i], NULL);
                return any_type();
            }
            for (int i = 0; i < n->as.ctor.argc; i++) {
                Type *pt = ed->vfield_types[vi]
                             ? ed->vfield_types[vi][i] : any_type();
                expect_compat(c, ck_expr(c, n->as.ctor.args[i], pt), pt,
                              n->line, "constructor argument");
            }
            return ed->type;
        }

        case N_MATCH: {
            /* The scrutinee must be a declared enum (or any, in which case
             * arm names cannot be validated and exhaustiveness is skipped —
             * the runtime tag dispatch is still exact). Arm bodies unify
             * with the ternary rules. */
            Type *st = ck_expr(c, n->as.matchn.scrutinee, NULL);
            st = resolve(c, st, n->line);
            EnumDef *ed = NULL;
            if (st && st->kind == TY_ENUM)
                ed = find_enum(c, st->name);
            else if (st && st->kind != TY_ANY) {
                ck_fail(c, n->line, "match needs an enum value (got %s)",
                        ty_str(st));
            }

            Type *result = NULL;
            bool any_arm = false;
            for (int i = 0; i < n->as.matchn.armc; i++) {
                MatchArm *arm = &n->as.matchn.arms[i];
                int bindc = ck_pat_list(c, arm->pats, arm->patc, ed,
                                        arm->line);
                /* Cross-arm conflicts: only an UNCONDITIONAL earlier arm
                 * shadows. A guarded arm may fall through at runtime, so
                 * `A if g => .., A => ..` is the legal guard idiom, not a
                 * duplicate. */
                if (!arm->guard) {
                    for (int j = 0; j < i; j++) {
                        MatchArm *prev = &n->as.matchn.arms[j];
                        if (prev->guard) continue;
                        bool dead = false;
                        for (int pj = 0; pj < prev->patc && !dead; pj++) {
                            if (strcmp(prev->pats[pj].vname, "_") == 0) {
                                ck_fail(c, arm->line,
                                        "match arm is unreachable: an "
                                        "earlier '_' arm catches every "
                                        "variant");
                                dead = true;
                                break;
                            }
                            for (int pi = 0; pi < arm->patc; pi++)
                                if (strcmp(prev->pats[pj].vname,
                                           arm->pats[pi].vname) == 0) {
                                    ck_fail(c, arm->line,
                                            "duplicate match arm '%s'",
                                            arm->pats[pi].vname);
                                    dead = true;
                                    break;
                                }
                        }
                    }
                }
                /* Bindings (and the guard, which may read them) live in a
                 * fresh scope, exactly like a block. The first (collecting)
                 * walk uses scope_put for the same reason top-level lets
                 * do: the second walk re-declares. */
                CScope *saved = c->scope;
                c->scope = scope_new(saved);
                ck_put_pat_binds(c, arm->pats, bindc, arm->line);
                if (arm->guard) {
                    Type *gt = ck_expr(c, arm->guard, NULL);
                    is_bool_ok(c, gt, arm->line);
                }
                Type *bt = ck_expr(c, arm->body, NULL);
                c->scope = saved;
                if (!bt || bt->kind == TY_ANY) { any_arm = true; continue; }
                if (!result) { result = bt; continue; }
                if (result->kind == bt->kind) continue;
                bool r_num = result->kind == TY_INT || result->kind == TY_FLOAT;
                bool b_num = bt->kind == TY_INT || bt->kind == TY_FLOAT;
                if (r_num && b_num) { result = type_prim(TY_FLOAT); continue; }
                ck_fail(c, n->line,
                        "match arms have different types ('%s' vs '%s')",
                        ty_str(result), ty_str(bt));
            }
            /* Exhaustiveness: every variant covered by an UNCONDITIONAL
             * arm (or a `_` one) — a guard can fail at runtime, so guarded
             * arms do not count (Lean's rule). Skipped when the scrutinee
             * is untyped and when the check already failed. */
            if (ed && !c->failed) {
                for (int j = 0; j < ed->vcount; j++) {
                    bool covered = false;
                    for (int i = 0; i < n->as.matchn.armc && !covered; i++) {
                        MatchArm *arm = &n->as.matchn.arms[i];
                        if (arm->guard) continue;
                        for (int pi = 0; pi < arm->patc; pi++)
                            if (strcmp(arm->pats[pi].vname, "_") == 0 ||
                                strcmp(arm->pats[pi].vname,
                                       ed->vnames[j]) == 0) {
                                covered = true;
                                break;
                            }
                    }
                    if (!covered) {
                        ck_fail(c, n->line,
                                "match on '%s' does not cover variant '%s' "
                                "(add an arm or '_')", ed->name, ed->vnames[j]);
                        break;
                    }
                }
            }
            (void)any_arm;
            return (result && !any_arm) ? result : any_type();
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
                /* The payload is whatever the callee's own `return { ok: X }`
                 * statements said, recorded onto its declared Result type by
                 * record_ok_payload(). Two passes over the function bodies run
                 * before this one, so a `?` on a function defined further down
                 * reads the same payload. `elem` is NULL when the callee only
                 * ever returns `err` -- nothing to unwrap, so there is no type
                 * to hand the emitters and they refuse to compile the call.
                 * That used to be every `?`, which is why the emitters could
                 * not support the operator at all. */
                return (src && src->elem) ? src->elem : any_type();
            }
            return ret;
        }

        default:
            return any_type(); /* non-expression encountered while atom-checking */
    }
}
