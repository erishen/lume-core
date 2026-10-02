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
    for (int i = 0; i < ft->count; i++)
        scope_decl(c, c->scope, names[i], ft->types[i],
                   body ? body->line : 0); /* 参数间重名/与函数体 let 重名即报错 */
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
            Type *annot = n->as.let.annot ? resolve(c, n->as.let.annot, n->line)
                                          : NULL;
            Type *it = ck_expr(c, n->as.let.init, annot);
            if (annot && !type_compat(c, it, annot, n->line)) {
                ck_fail(c, n->line, "'%s' is not assignable to the declared type '%s' of '%s'",
                        ty_str(it), ty_str(annot), n->as.let.name);
            }
            scope_decl(c, c->scope, n->as.let.name,
                       annot ? annot : (it ? it : any_type()), n->line);
            if (n->is_export)
                export_add(c, n->as.let.name,
                           scope_get(c->scope, n->as.let.name));
            return;
        }
        case N_IF: {
            Type *ct = ck_expr(c, n->as.ifs.cond, NULL);
            is_bool_ok(c, ct, n->line);
            ck_stmt(c, n->as.ifs.then);
            if (n->as.ifs.els) ck_stmt(c, n->as.ifs.els);
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
            Type *ft = scope_get(c->scope, n->as.func.name);
            if (ft && ft->kind == TY_FUNC)
                ck_fn(c, n->as.func.names, ft, n->as.func.body);
            return;
        }
        case N_TYPE_DECL: /* registered in pass A, filled in pass B */
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
