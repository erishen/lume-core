/* 语句与函数体类型检查:ck_stmt / ck_blk / ck_fn——
 * 从 typecheck.c 拆出(2026-09-27)。tool 参数 schema 到 handler 参数
 * 结构的推导(scalar_from_word / tool_param_struct)也在此。 */

#include "typecheck_internal.h"

void ck_blk(Checker *c, Node *n) {
    if (n->type != N_BLOCK) { ck_stmt(c, n); return; }
    CScope *saved = c->scope;
    c->scope = scope_new(saved);
    for (int i = 0; i < n->as.block.count; i++) {
        ck_stmt(c, n->as.block.stmts[i]);
        if (c->failed) break;
    }
    c->scope = saved;
}

/* Check a function body with its params bound and its return type current. */
void ck_fn(Checker *c, char **names, Type *ft, Node *body) {
    CScope *saved = c->scope;
    Type *ret_saved = c->cur_ret;
    bool in_saved = c->in_func;
    int loop_saved = c->loop_depth;
    c->scope = scope_new(saved);
    for (int i = 0; i < ft->count; i++) {
        ck_ctor_shadow(c, names[i], body ? body->line : 0);
        /* Same reason as the top-level `let` in ck_stmt: the body is walked
         * twice, and re-running scope_decl on the parameters would report a
         * duplicate binding the second time around. */
        if (c->collecting)
            scope_put(c->scope, names[i], ft->types[i]);
        else
            scope_decl(c, c->scope, names[i], ft->types[i],
                       body ? body->line : 0); /* 参数间重名/与函数体 let 重名即报错 */
    }
    c->cur_ret = ft->ret;
    c->in_func = true;
    c->loop_depth = 0;
    ck_blk(c, body);
    c->loop_depth = loop_saved;
    c->in_func = in_saved;
    c->cur_ret = ret_saved;
    c->scope = saved;
}

/* Map a bare schema type word to a scalar Type; NULL for unknown words. */
static Type *scalar_from_word(const char *w, size_t len) {
    if (len == 3 && strncmp(w, "int", 3) == 0)    return type_prim(TY_INT);
    if (len == 5 && strncmp(w, "float", 5) == 0)  return type_prim(TY_FLOAT);
    if (len == 6 && strncmp(w, "string", 6) == 0) return type_prim(TY_STRING);
    if (len == 3 && strncmp(w, "str", 3) == 0)    return type_prim(TY_STRING);
    if (len == 4 && strncmp(w, "bool", 4) == 0)   return type_prim(TY_BOOL);
    return NULL;
}

/* A string literal's `text`/`len` include the surrounding quotes (the
 * interpreter unescapes at eval time); strip them to read a bare type word. */
static Type *scalar_from_strlit(Node *v) {
    const char *s = v->as.lit.text;
    size_t n = (size_t)v->as.lit.len;
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') { s++; n -= 2; }
    return scalar_from_word(s, n);
}

/* Derive the tool handler's `arg` struct type straight from the params
 * expression, honoring both `{ a: int }` and `{ a: "int" }`. Non-scalar or
 * non-literal entries degrade to `any` (no zero-default at runtime). */
static Type *tool_param_struct(Node *p) {
    Type *at = type_anon_struct();
    if (!p || p->type != N_MAP_LIT) return at;
    for (int i = 0; i < p->as.map.count; i++) {
        Type *mt = NULL;
        Node *v = p->as.map.vals[i];
        if (v->type == N_VAR)
            mt = scalar_from_word(v->as.var.name, strlen(v->as.var.name));
        else if (v->type == N_LITERAL && v->as.lit.kind == LIT_STR)
            mt = scalar_from_strlit(v);
        else if (v->type == N_MAP_LIT) {
            /* already-schema form: { a: { type: "int" } } */
            for (int j = 0; j < v->as.map.count; j++) {
                if (strcmp(v->as.map.keys[j], "type") != 0) continue;
                Node *tv = v->as.map.vals[j];
                if (tv->type == N_LITERAL && tv->as.lit.kind == LIT_STR)
                    mt = scalar_from_strlit(tv);
                else if (tv->type == N_VAR)
                    mt = scalar_from_word(tv->as.var.name, strlen(tv->as.var.name));
                break;
            }
        }
        type_add_member(at, p->as.map.keys[i], mt ? mt : any_type());
    }
    return at;
}

/* Walk one statement. `collect` selects the first of Pass D's two walks: the
 * collecting one, which records Result payloads and notes diagnostics instead of
 * reporting them, and the ordinary one. Both walks are the same traversal --
 * ck_stmt never sees the difference -- because the only thing that has to
 * happen differently is whether ck_fail() stops the walk, and that is already
 * handled inside ck_fail(). Kept as a wrapper so the pass loop in
 * type_check_module() can say which walk it is on without duplicating the
 * per-statement dispatch. */
void ck_stmt_mode(Checker *c, Node *n, bool collect) {
    c->collecting = collect;
    ck_stmt(c, n);
    c->collecting = false;
}

void ck_stmt(Checker *c, Node *n) {
    if (c->failed || !n) return;
    switch (n->type) {
        case N_IMPORT: {
            /* `import "x.lume" as ns;` — bind ns to the dependency's export
             * table. The loader has already resolved paths and filled self's
             * import_paths/ns_names; node->as.imp.path is the canonical path. */
            if (!c->self) {
                ck_fail(c, n->line, "'import' is only available in a file module");
                return;
            }
            /* 与 let/func 同语义: 内置名允许遮蔽(scope_decl 也是这么处理的,
             * P1 重名拦截只拦用户声明之间)——`import "m.lume" as tools` 不会
             * 撞内置 tools()。仍拦: 命名空间重名(scope_get_ns)与用户声明重名。 */
            if (scope_get_ns(c->scope, n->as.imp.ns) ||
                (!is_builtin_name(n->as.imp.ns) &&
                 scope_get(c->scope, n->as.imp.ns))) {
                ck_fail(c, n->line, "duplicate name '%s'",
                        n->as.imp.ns);
                return;
            }
            Module *dep = NULL;
            for (int i = 0; i < c->mod_count; i++)
                if (strcmp(c->mods[i]->path, n->as.imp.path) == 0) {
                    dep = c->mods[i];
                    break;
                }
            if (!dep) {
                ck_fail(c, n->line, "module '%s' not loaded", n->as.imp.path);
                return;
            }
            if (!dep->typechecked) {
                ck_fail(c, n->line, "module '%s' failed type checking",
                        dep->path);
                return;
            }
            scope_put_ns(c->scope, n->as.imp.ns, dep);
            return;
        }
        case N_BLOCK:
            ck_blk(c, n);
            return;
        case N_LET: {
            ck_ctor_shadow(c, n->as.let.name, n->line);
            Type *annot = n->as.let.annot ? resolve(c, n->as.let.annot, n->line)
                                          : NULL;
            /* Write the resolved annotation back for the native emitters —
             * they read let.annot directly and have no checker tables, so a
             * `Shape` shell must arrive as the TY_ENUM def (see pass C in
             * typecheck.c for the param/ret side of the same story). */
            /* Write the resolved annotation back only on the reporting walk:
             * a collecting-walk write can overwrite an unresolvable phantom
             * (`let x: num`) with any_type(), so the second walk cannot
             * reproduce ck_fail's "unknown type" and the noted error dies.
             * Pass C's param/ret write-back is safe: it never runs collecting. */
            if (n->as.let.annot && !c->collecting) n->as.let.annot = annot;
            Type *it = ck_expr(c, n->as.let.init, annot);
            if (annot && !type_compat(c, it, annot, n->line)) {
                ck_fail(c, n->line, "'%s' is not assignable to the declared type '%s' of '%s'",
                        ty_str(it), ty_str(annot), n->as.let.name);
            }
            /* Pass D walks every statement twice (see type_check_module()), so a
             * top-level `let` arrives here twice with its name already bound by
             * the first walk. Re-declaring it would trip scope_decl's
             * same-scope check and report a duplicate that does not exist --
             * which is why this goes in through scope_put while collecting. The
             * binding it makes is the same one the second walk will make. */
            if (c->collecting)
                scope_put(c->scope, n->as.let.name,
                          annot ? annot : (it ? it : any_type()));
            else
                scope_decl(c, c->scope, n->as.let.name,
                           annot ? annot : (it ? it : any_type()), n->line);
            if (n->is_export)
                export_add(c, n->as.let.name,
                           scope_get(c->scope, n->as.let.name));
            return;
        }
        case N_LET_PAT: {
            /* `let { a, b } = e;` — bind every name to the like-named key
             * of e's map, in the current scope, e evaluated once. The rules
             * mirror m.x exactly: a shaped map (a literal or a named
             * struct) must declare every pattern name, and the stamped
             * type is what the emitters dispatch on and what the
             * interpreter's missing-key fallback materialises; a map of
             * unknown shape (any) is accepted dynamically — the native
             * backends refuse those at emit time, just as they refuse
             * `m.x` on a dynamic map. */
            Type *it = ck_expr(c, n->as.letpat.init, NULL);
            it = resolve(c, it, n->line);
            if (it->kind == TY_ENUM)
                ck_fail(c, n->line,
                        "destructuring needs a map value (got %s) — use "
                        "if-let to destructure an enum", ty_str(it));
            else if (it->kind != TY_STRUCT && it->kind != TY_ANY)
                ck_fail(c, n->line,
                        "destructuring needs a map value (got %s)",
                        ty_str(it));
            for (int i = 0; i < n->as.letpat.count; i++) {
                const char *nm = n->as.letpat.names[i];
                const char *fld = n->as.letpat.fields[i];
                ck_ctor_shadow(c, nm, n->line);
                Type *vt = NULL;
                if (it->kind == TY_STRUCT) {
                    for (int j = 0; j < it->count; j++)
                        if (strcmp(fld, it->names[j]) == 0) {
                            vt = it->types[j];
                            break;
                        }
                    if (!vt) {
                        ck_fail(c, n->line, "type has no field '%s'", fld);
                        vt = any_type();
                    }
                }
                /* NULL stays NULL for a dynamic map: the same state
                 * N_MEMBER.type is in after `m.x` on an any-typed base. */
                n->as.letpat.types[i] = vt;
                if (c->collecting)
                    scope_put(c->scope, nm, vt ? vt : any_type());
                else
                    scope_decl(c, c->scope, nm, vt ? vt : any_type(),
                               n->line);
            }
            if (n->is_export)
                for (int i = 0; i < n->as.letpat.count; i++)
                    export_add(c, n->as.letpat.names[i],
                               scope_get(c->scope, n->as.letpat.names[i]));
            return;
        }
        case N_IF: {
            Type *ct = ck_expr(c, n->as.ifs.cond, NULL);
            is_bool_ok(c, ct, n->line);
            ck_stmt(c, n->as.ifs.then);
            if (n->as.ifs.els) ck_stmt(c, n->as.ifs.els);
            return;
        }
        case N_IF_LET: {
            /* `if let P [| P] = value { then } [else ...]`. The value must
             * be an enum (or any — patterns unvalidated, like match). Binds
             * are declared in a child scope around the then-branch ONLY:
             * the else-branch and everything after the statement see
             * nothing, which is what makes the binding block-scoped (match
             * arms behave the same way). */
            Type *vt = ck_expr(c, n->as.iflet.value, NULL);
            vt = resolve(c, vt, n->line);
            EnumDef *ed = NULL;
            if (vt && vt->kind == TY_ENUM)
                ed = find_enum(c, vt->name);
            else if (vt && vt->kind != TY_ANY) {
                ck_fail(c, n->line, "if-let needs an enum value (got %s)",
                        ty_str(vt));
            }
            int bindc = ck_pat_list(c, n->as.iflet.pats, n->as.iflet.patc,
                                    ed, n->line);
            CScope *saved = c->scope;
            c->scope = scope_new(saved);
            ck_put_pat_binds(c, n->as.iflet.pats, bindc, n->line);
            ck_stmt(c, n->as.iflet.then);
            c->scope = saved;
            if (n->as.iflet.els) ck_stmt(c, n->as.iflet.els);
            return;
        }
        case N_WHILE: {
            Type *ct = ck_expr(c, n->as.whiles.cond, NULL);
            is_bool_ok(c, ct, n->line);
            c->loop_depth++;
            ck_stmt(c, n->as.whiles.body);
            c->loop_depth--;
            return;
        }
        case N_FOR: {
            if (n->as.fors.is_in) {
                Type *it = ck_expr(c, n->as.fors.iterable, NULL);
                Type *elem = NULL;
                if (it && it->kind == TY_LIST) elem = it->elem;
                /* maps/structs iterate their keys; anything else degrades to
                 * `any` (the runtime enforces list-or-map)。迭代变量是"绑定
                 * 复用"而非声明: 外层已有同名时复用该变量(运行时 env_set
                 * 覆盖值), 所以不走 scope_decl 的重名检测。 */
                scope_put(c->scope, n->as.fors.var, elem ? elem : any_type());
                c->loop_depth++;
                ck_stmt(c, n->as.fors.body);
                c->loop_depth--;
                return;
            }
            if (n->as.fors.init) ck_stmt(c, n->as.fors.init);
            if (n->as.fors.cond) {
                Type *ct = ck_expr(c, n->as.fors.cond, NULL);
                is_bool_ok(c, ct, n->line);
            }
            c->loop_depth++;
            ck_stmt(c, n->as.fors.body);
            c->loop_depth--;
            if (n->as.fors.incr) ck_stmt(c, n->as.fors.incr);
            return;
        }
        case N_BREAK:
        case N_CONTINUE:
            if (c->loop_depth <= 0)
                ck_fail(c, n->line, "'%s' outside a loop",
                        n->type == N_BREAK ? "break" : "continue");
            return;
        case N_RETURN: {
            if (n->as.ret.expr) {
                Type *rt = ck_expr(c, n->as.ret.expr, c->cur_ret);
                if (c->cur_ret && !type_compat(c, rt, c->cur_ret, n->line))
                    ck_fail(c, n->line, "return value does not match the "
                            "function's return type");
            } /* bare `return` == null, nullable means that's always ok */
            return;
        }
        case N_EXPR_STMT:
            ck_expr(c, n->as.expr_stmt.expr, NULL);
            return;
        case N_FUNC_DECL: {
            /* A local function was registered (and its call sites rewritten)
             * under its synthetic cname; a top-level function keeps its
             * source name. Same lookup rule in both cases. */
            const char *fname = n->as.func.cname ? n->as.func.cname
                                                 : n->as.func.name;
            Type *ft = scope_get(c->scope, fname);
            if (ft && ft->kind == TY_FUNC)
                ck_fn(c, n->as.func.names, ft, n->as.func.body);
            return;
        }
        case N_TYPE_DECL: /* registered in pass A, filled in pass B */
            return;
        case N_ENUM_DECL: /* registered in pass A2, filled in pass B2 */
            return;
        case N_SERVER:
            for (int i = 0; i < n->as.server.count; i++)
                ck_expr(c, n->as.server.assigns[i]->as.assign.value, NULL);
            return;
        case N_ROUTE:
            if (n->as.route.handler) ck_expr(c, n->as.route.handler, NULL);
            return;
        case N_TOOL:
            ck_expr(c, n->as.tool.params, NULL);
            if (n->as.tool.handler &&
                n->as.tool.handler->type == N_FUNC_LIT) {
                if (n->as.tool.handler->as.funclit.arity != 1) {
                    ck_fail(c, n->line,
                            "tool handler must take exactly one argument");
                    return;
                }
                /* the handler's `arg` is typed by the tool's own param
                 * schema, so `arg.a` is checkable and defaults on missing */
                c->param_hint = tool_param_struct(n->as.tool.params);
            }
            ck_expr(c, n->as.tool.handler, NULL);
            return;
        case N_VERBS:
            ck_expr(c, n->as.verbs.methods, NULL);
            scope_decl(c, c->scope, n->as.verbs.name, any_type(), n->line);
            return;
        case N_ASSIGN: {
            Type *vt = scope_get(c->scope, n->as.assign.name);
            if (!vt) {
                ck_fail(c, n->line, "assignment to undefined variable '%s'",
                        n->as.assign.name);
                return;
            }
            /* Closure-variable reassignment (SPEC §8.1 #9): the variable's
             * type locks to the FIRST lambda's signature (the back pointer),
             * every call lowers against it, while the runtime box holds
             * whatever was assigned last. A lambda of a DIFFERENT signature
             * is a silent mismatch on the interpreter and refused on the
             * native backends — reject here so all three agree. Parameters
             * or returns still unresolved (NULL) defer the verdict to the
             * native signature pass's fixed point (which propagates the old
             * signature into the new lambda and compares again). */
            if (vt->kind == TY_FUNC && vt->func &&
                vt->func->type == N_FUNC_LIT &&
                n->as.assign.value &&
                n->as.assign.value->type == N_FUNC_LIT) {
                Node *old = vt->func, *new_ = n->as.assign.value;
                int same = 1, undecided = 0;
                if (old->as.funclit.arity != new_->as.funclit.arity) {
                    same = 0;
                } else {
                    for (int k = 0; k < old->as.funclit.arity && same; k++) {
                        Type *a = old->as.funclit.param_types[k];
                        Type *b = new_->as.funclit.param_types[k];
                        if (!a || !b) { undecided = 1; same = 0; break; }
                        a = resolve(c, a, n->line);
                        b = resolve(c, b, n->line);
                        if (!(type_compat(c, a, b, n->line) &&
                              type_compat(c, b, a, n->line)))
                            same = 0;
                    }
                    if (same) {
                        if (!old->as.funclit.ret || !new_->as.funclit.ret) {
                            undecided = 1;
                            same = 0;
                        } else {
                            Type *ar = resolve(c, old->as.funclit.ret, n->line);
                            Type *br = resolve(c, new_->as.funclit.ret, n->line);
                            if (!(type_compat(c, ar, br, n->line) &&
                                  type_compat(c, br, ar, n->line)))
                                same = 0;
                        }
                    }
                }
                if (!undecided && !same)
                    ck_fail(c, n->line, "cannot reassign a closure variable "
                            "with a different signature — the variable keeps "
                            "the first lambda's signature");
            }
            Type *it = ck_expr(c, n->as.assign.value, vt);
            if (!type_compat(c, it, vt, n->line))
                ck_fail(c, n->line, "assignment type mismatch for '%s'",
                        n->as.assign.name);
            return;
        }
        case N_ASSIGN_MEMBER: {
            Type *ot = ck_expr(c, n->as.assign_mem.obj, NULL);
            ot = resolve(c, ot, n->line);
            if (ot->kind == TY_ANY) { ck_expr(c, n->as.assign_mem.value, NULL); return; }
            if (ot->kind != TY_STRUCT) {
                ck_fail(c, n->line, "cannot assign a member on this type");
                return;
            }
            int idx = -1;
            for (int i = 0; i < ot->count; i++)
                if (strcmp(n->as.assign_mem.name, ot->names[i]) == 0) { idx = i; break; }
            if (idx < 0) {
                ck_fail(c, n->line, "type has no field '%s'",
                        n->as.assign_mem.name);
                return;
            }
            expect_compat(c, ck_expr(c, n->as.assign_mem.value, ot->types[idx]),
                          ot->types[idx], n->line, "member assignment");
            return;
        }
        default:
            ck_expr(c, n, NULL);
            return;
    }
}
