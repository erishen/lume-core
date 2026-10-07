/* 语句级解析:parse_statement——从 parser.c 拆出(2026-09-27)。
 * 覆盖声明(export/import/let/verbs/return/server/route/tool/type/func)、
 * 控制流(if/while/for/break/continue)与方法简写路由(get/post/...)。 */

#include "parser_internal.h"

Node *parse_statement(Parser *p) {
    Token t = peek(p);

    if (t.type == TOK_EXPORT) {
        /* `export let|func|type …` — parse the underlying declaration and
         * mark it; the checker/interp publish it into the module's export
         * table. (importers bind `import "x" as ns` to that table.) */
        advance(p);
        LT_TokenType kt = peek(p).type;
        if (kt != TOK_LET && kt != TOK_FUNC && kt != TOK_TYPE) {
            perror_at(p, peek(p).line,
                      "expected 'let', 'func' or 'type' after 'export'", NULL);
            return NULL;
        }
        Node *n = parse_statement(p);
        if (n) n->is_export = true;
        return n;
    }
    if (t.type == TOK_IMPORT) {
        /* import "path/to/lib.lume" as ns;  — top-level only (the loader
         * rejects imports nested anywhere else). */
        advance(p);
        if (!check(p, TOK_STRING)) {
            perror_at(p, peek(p).line,
                      "expected a path string after 'import'", NULL);
            return NULL;
        }
        Token path = peek(p);
        advance(p);
        if (!expect(p, TOK_AS)) return NULL;
        if (!check(p, TOK_IDENT)) {
            perror_at(p, peek(p).line,
                      "expected a namespace name after 'as'", NULL);
            return NULL;
        }
        Token ns = peek(p);
        advance(p);
        if (!expect(p, TOK_SEMI)) return NULL;
        Node *n = nalloc(N_IMPORT, t.line);
        /* keep the raw quoted path (like N_LITERAL strings); the loader
         * unescapes it and resolves it against this file's directory */
        n->as.imp.path = malloc((size_t)path.length + 1);
        memcpy(n->as.imp.path, path.start, (size_t)path.length);
        n->as.imp.path[path.length] = '\0';
        n->as.imp.ns = ident_name(p, ns);
        return n;
    }
    if (t.type == TOK_LBRACE) {
        return parse_block(p);
    }
    if (t.type == TOK_IF) {
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        Node *cond = parse_expression(p);
        if (!cond) return NULL;
        if (!expect(p, TOK_RPAREN)) return NULL;
        Node *then = parse_statement(p);
        if (!then) return NULL;
        Node *els = NULL;
        if (match(p, TOK_ELSE)) {
            els = parse_statement(p);
            if (!els) return NULL;
        }
        Node *n = nalloc(N_IF, t.line);
        n->as.ifs.cond = cond;
        n->as.ifs.then = then;
        n->as.ifs.els = els;
        return n;
    }
    if (t.type == TOK_WHILE) {
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        Node *cond = parse_expression(p);
        if (!cond) return NULL;
        if (!expect(p, TOK_RPAREN)) return NULL;
        Node *body = parse_statement(p);
        if (!body) return NULL;
        Node *n = nalloc(N_WHILE, t.line);
        n->as.whiles.cond = cond;
        n->as.whiles.body = body;
        return n;
    }
    if (t.type == TOK_FOR) {
        /* for (init; cond; incr) body  — C-style; init may be `let` or an
         * expression, cond/incr optional (`for (;;)`) — and the two for-in
         * forms: `for (x in xs)` / `for (let x in xs)`. */
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        Node *n = nalloc(N_FOR, t.line);
        if (check(p, TOK_LET)) {
            /* `let` head: either `let x in xs` (for-in) or `let i = e` (C) */
            advance(p);
            if (!check(p, TOK_IDENT)) {
                perror_at(p, peek(p).line, "expected variable name after 'let'", NULL);
                return NULL;
            }
            Token vn = peek(p);
            advance(p);
            if (match(p, TOK_IN)) {
                n->as.fors.is_in = true;
                n->as.fors.var = ident_name(p, vn);
                n->as.fors.iterable = parse_expression(p);
                if (!n->as.fors.iterable) return NULL;
                if (!expect(p, TOK_RPAREN)) return NULL;
                n->as.fors.body = parse_statement(p);
                if (!n->as.fors.body) return NULL;
                return n;
            }
            Node *init = nalloc(N_LET, vn.line);
            init->as.let.name = ident_name(p, vn);
            if (match(p, TOK_COLON)) {
                init->as.let.annot = parse_type(p);
                if (!init->as.let.annot) return NULL;
            }
            if (!expect(p, TOK_EQ)) return NULL;
            init->as.let.init = parse_expression(p);
            if (!init->as.let.init) return NULL;
            n->as.fors.init = init;
            if (!expect(p, TOK_SEMI)) return NULL;
            if (!check(p, TOK_SEMI)) {
                n->as.fors.cond = parse_expression(p);
                if (!n->as.fors.cond) return NULL;
            }
            if (!expect(p, TOK_SEMI)) return NULL;
            if (!check(p, TOK_RPAREN)) {
                Node *incr = nalloc(N_EXPR_STMT, peek(p).line);
                incr->as.expr_stmt.expr = parse_expression(p);
                if (!incr->as.expr_stmt.expr) return NULL;
                n->as.fors.incr = incr;
            }
            if (!expect(p, TOK_RPAREN)) return NULL;
            n->as.fors.body = parse_statement(p);
            if (!n->as.fors.body) return NULL;
            return n;
        }
        if (check(p, TOK_IDENT) && peek2(p).type == TOK_IN) {
            Token vn = peek(p);
            advance(p);
            advance(p); /* in */
            n->as.fors.is_in = true;
            n->as.fors.var = ident_name(p, vn);
            n->as.fors.iterable = parse_expression(p);
            if (!n->as.fors.iterable) return NULL;
            if (!expect(p, TOK_RPAREN)) return NULL;
            n->as.fors.body = parse_statement(p);
            if (!n->as.fors.body) return NULL;
            return n;
        }
        /* C-style with an expression init (assignment / call / ...) */
        if (!check(p, TOK_SEMI)) {
            Node *init = nalloc(N_EXPR_STMT, peek(p).line);
            init->as.expr_stmt.expr = parse_expression(p);
            if (!init->as.expr_stmt.expr) return NULL;
            n->as.fors.init = init;
        }
        if (!expect(p, TOK_SEMI)) return NULL;
        if (!check(p, TOK_SEMI)) {
            n->as.fors.cond = parse_expression(p);
            if (!n->as.fors.cond) return NULL;
        }
        if (!expect(p, TOK_SEMI)) return NULL;
        if (!check(p, TOK_RPAREN)) {
            Node *incr = nalloc(N_EXPR_STMT, peek(p).line);
            incr->as.expr_stmt.expr = parse_expression(p);
            if (!incr->as.expr_stmt.expr) return NULL;
            n->as.fors.incr = incr;
        }
        if (!expect(p, TOK_RPAREN)) return NULL;
        n->as.fors.body = parse_statement(p);
        if (!n->as.fors.body) return NULL;
        return n;
    }
    if (t.type == TOK_BREAK || t.type == TOK_CONTINUE) {
        LT_TokenType kt = t.type;
        advance(p);
        if (!expect(p, TOK_SEMI)) return NULL;
        return nalloc(kt == TOK_BREAK ? N_BREAK : N_CONTINUE, t.line);
    }
    if (t.type == TOK_LET) {
        advance(p);
        if (!check(p, TOK_IDENT)) {
            perror_at(p, peek(p).line, "expected variable name after 'let'", NULL);
            return NULL;
        }
        Token name = peek(p);
        advance(p);
        Type *annot = NULL;
        if (match(p, TOK_COLON)) {
            annot = parse_type(p);
            if (!annot) return NULL;
        }
        if (!expect(p, TOK_EQ)) return NULL;
        Node *init = parse_expression(p);
        if (!init) return NULL;
        if (!expect(p, TOK_SEMI)) return NULL;
        Node *n = nalloc(N_LET, t.line);
        n->as.let.name = ident_name(p, name);
        n->as.let.annot = annot;
        n->as.let.init = init;
        return n;
    }
    if (t.type == TOK_VERBS) {
        /* verbs name = ["POST", "PUT", ...];  — a reusable method group that
         * `name "path", handler;` expands to (one route per method). */
        advance(p);
        if (!check(p, TOK_IDENT)) {
            perror_at(p, peek(p).line, "expected verb-group name after 'verbs'", NULL);
            return NULL;
        }
        Token name = peek(p);
        advance(p);
        if (!expect(p, TOK_EQ)) return NULL;
        Node *methods = parse_expression(p);
        if (!methods) return NULL;
        if (!expect(p, TOK_SEMI)) return NULL;
        Node *n = nalloc(N_VERBS, t.line);
        n->as.verbs.name = ident_name(p, name);
        n->as.verbs.methods = methods;
        return n;
    }
    if (t.type == TOK_RETURN) {
        advance(p);
        Node *expr = NULL;
        if (!check(p, TOK_SEMI)) {
            expr = parse_expression(p);
            if (!expr) return NULL;
        }
        if (!expect(p, TOK_SEMI)) return NULL;
        Node *n = nalloc(N_RETURN, t.line);
        n->as.ret.expr = expr;
        return n;
    }
    if (t.type == TOK_SERVER) {
        /* server { name = expr; ... } */
        advance(p);
        Node *n = nalloc(N_SERVER, t.line);
        n->as.server.assigns = NULL;
        n->as.server.count = 0;
        if (!expect(p, TOK_LBRACE)) return NULL;
        while (!check(p, TOK_RBRACE) && !at_end(p)) {
            if (!check(p, TOK_IDENT)) {
                perror_at(p, peek(p).line, "expected config field name", NULL);
                return NULL;
            }
            Token key = peek(p);
            advance(p);
            if (!expect(p, TOK_EQ)) return NULL;
            Node *value = parse_expression(p);
            if (!value) return NULL;
            if (!expect(p, TOK_SEMI)) return NULL;
            Node *a = nalloc(N_ASSIGN, key.line);
            a->as.assign.name = ident_name(p, key);
            a->as.assign.value = value;
            n->as.server.assigns = realloc(n->as.server.assigns,
                                           sizeof(Node *) * ((size_t)n->as.server.count + 1));
            n->as.server.assigns[n->as.server.count++] = a;
        }
        if (!expect(p, TOK_RBRACE)) return NULL;
        return n;
    }
    if (t.type == TOK_ROUTE) {
        /* route "METHOD", "path|path*", handler; */
        advance(p);
        Node *n = nalloc(N_ROUTE, t.line);
        if (!check(p, TOK_STRING)) {
            perror_at(p, peek(p).line, "expected method string in route", NULL);
            return NULL;
        }
        Token m = peek(p);
        advance(p);
        n->as.route.method = malloc((size_t)(m.length - 2) + 1);
        memcpy(n->as.route.method, m.start + 1, (size_t)(m.length - 2));
        n->as.route.method[m.length - 2] = '\0';
        if (!expect(p, TOK_COMMA)) return NULL;
        return parse_route_tail(p, n);
    }
    if (is_method_keyword(t.type)) {
        /* get "path", handler;  ≡  route "GET", "path", handler;
         * Only when the *next* token is a string is this the route form;
         * otherwise it stays a plain expression statement (`get(m, key);`). */
        if (p->pos + 1 < p->count && p->toks[p->pos + 1].type == TOK_STRING) {
            advance(p); /* consume the method keyword */
            Node *n = nalloc(N_ROUTE, t.line);
            n->as.route.method = strdup(method_keyword_name(t.type));
            return parse_route_tail(p, n);
        }
    }
    if (t.type == TOK_IDENT &&
        p->pos + 1 < p->count && p->toks[p->pos + 1].type == TOK_STRING) {
        /* name "path", handler;  — `name` must be a `verbs` group declared
         * earlier; the bridge resolves it to one route per method. A bare
         * identifier followed by a string is not a valid expression, so this
         * form is unambiguous. */
        advance(p); /* consume the group name */
        Node *n = nalloc(N_ROUTE, t.line);
        n->as.route.alias = ident_name(p, t);
        return parse_route_tail(p, n);
    }
    if (t.type == TOK_TOOL) {
        /* tool "name", "desc", {params}, handler; */
        advance(p);
        Node *n = nalloc(N_TOOL, t.line);
        if (!check(p, TOK_STRING)) {
            perror_at(p, peek(p).line, "expected name string in tool", NULL);
            return NULL;
        }
        Token nm = peek(p);
        advance(p);
        n->as.tool.name = malloc((size_t)(nm.length - 2) + 1);
        memcpy(n->as.tool.name, nm.start + 1, (size_t)(nm.length - 2));
        n->as.tool.name[nm.length - 2] = '\0';
        if (!expect(p, TOK_COMMA)) return NULL;
        if (!check(p, TOK_STRING)) {
            perror_at(p, peek(p).line, "expected description string in tool", NULL);
            return NULL;
        }
        Token ds = peek(p);
        advance(p);
        n->as.tool.desc = malloc((size_t)(ds.length - 2) + 1);
        memcpy(n->as.tool.desc, ds.start + 1, (size_t)(ds.length - 2));
        n->as.tool.desc[ds.length - 2] = '\0';
        if (!expect(p, TOK_COMMA)) return NULL;
        n->as.tool.params = parse_expression(p);
        if (!n->as.tool.params) return NULL;
        if (!expect(p, TOK_COMMA)) return NULL;
        n->as.tool.handler = parse_expression(p);
        if (!n->as.tool.handler) return NULL;
        if (!expect(p, TOK_SEMI)) return NULL;
        return n;
    }
    if (t.type == TOK_TYPE) {
        /* type Name = { field: type, ... }; */
        advance(p);
        if (!check(p, TOK_IDENT)) {
            perror_at(p, peek(p).line, "expected type name after 'type'", NULL);
            return NULL;
        }
        Token name = peek(p);
        advance(p);
        if (!expect(p, TOK_EQ)) return NULL;
        if (!expect(p, TOK_LBRACE)) return NULL;
        Node *n = nalloc(N_TYPE_DECL, t.line);
        n->as.type_decl.name = ident_name(p, name);
        n->as.type_decl.field_names = NULL;
        n->as.type_decl.field_types = NULL;
        n->as.type_decl.count = 0;
        while (!check(p, TOK_RBRACE) && !at_end(p)) {
            if (!is_field_token(peek(p).type)) {
                perror_at(p, peek(p).line, "expected field name in type", NULL);
                return NULL;
            }
            Token f = peek(p);
            advance(p);
            if (!expect(p, TOK_COLON)) return NULL;
            Type *ft = parse_type(p);
            if (!ft) return NULL;
            n->as.type_decl.field_names =
                realloc(n->as.type_decl.field_names,
                        sizeof(char *) * ((size_t)n->as.type_decl.count + 1));
            n->as.type_decl.field_types =
                realloc(n->as.type_decl.field_types,
                        sizeof(Type *) * ((size_t)n->as.type_decl.count + 1));
            n->as.type_decl.field_names[n->as.type_decl.count] = ident_name(p, f);
            n->as.type_decl.field_types[n->as.type_decl.count] = ft;
            n->as.type_decl.count++;
            if (!match(p, TOK_COMMA)) break;
            if (check(p, TOK_RBRACE)) break; /* trailing comma */
        }
        if (!expect(p, TOK_RBRACE)) return NULL;
        return n;
    }
    if (t.type == TOK_FUNC) {
        /* top-level named function declaration */
        advance(p);
        if (!check(p, TOK_IDENT)) {
            perror_at(p, peek(p).line, "expected function name after 'func'", NULL);
            return NULL;
        }
        Token name = peek(p);
        advance(p);
        Node *n = nalloc(N_FUNC_DECL, t.line);
        n->as.func.name = ident_name(p, name);
        if (!parse_params(p, &n->as.func.names, &n->as.func.param_types,
                          &n->as.func.arity)) return NULL;
        n->as.func.ret = parse_optional_ret(p);
        n->as.func.body = parse_block(p);
        if (!n->as.func.body) return NULL;
        return n;
    }

    /* expression statement (run(); a = b; f(); ...) */
    Node *expr = parse_expression(p);
    if (!expr) return NULL;
    if (!expect(p, TOK_SEMI)) return NULL;
    Node *n = nalloc(N_EXPR_STMT, t.line);
    n->as.expr_stmt.expr = expr;
    return n;
}
