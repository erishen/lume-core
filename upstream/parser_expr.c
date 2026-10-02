/* 表达式解析:parse_primary → parse_expression 的优先级链——
 * 从 parser.c 拆出(2026-09-27)。入口 parse_expression 供语句/路由尾
 * 调用;链内各层(primary/postfix/unary/factor/term/comparison/equality/
 * and/or)仅本文件内部使用。 */

#include "parser_internal.h"

static Node *parse_primary(Parser *p) {
    Token t = peek(p);

    if (t.type == TOK_NUMBER) {
        advance(p);
        Node *n = nalloc(N_LITERAL, t.line);
        n->as.lit.kind = LIT_NUM;
        n->as.lit.num = t.num;
        bool is_float = false;
        for (int i = 0; i < t.length && !is_float; i++)
            if (t.start[i] == '.' || t.start[i] == 'e' || t.start[i] == 'E')
                is_float = true;
        n->as.lit.is_float = is_float;
        return n;
    }
    if (t.type == TOK_STRING) {
        advance(p);
        /* JS-style adjacent string literals: "a" "b" is "ab". Merge the
         * quote-inclusive spans here; the combined literal unescapes at
         * eval time like any other, so \" and \\n inside either part keep
         * their meaning across the join. */
        size_t mlen = (size_t)t.length;
        char *merged = malloc(mlen + 1);
        memcpy(merged, t.start, mlen);
        merged[mlen] = '\0';
        while (peek(p).type == TOK_STRING) {
            Token u = peek(p);
            advance(p);
            size_t inner = (size_t)u.length - 2; /* drop u's quotes */
            merged = realloc(merged, mlen - 1 + inner + 1 + 1);
            memcpy(merged + mlen - 1, u.start + 1, inner); /* overwrite our closing quote */
            mlen = mlen - 1 + inner + 1;
            merged[mlen - 1] = '"';
            merged[mlen] = '\0';
        }
        Node *n = nalloc(N_LITERAL, t.line);
        n->as.lit.kind = LIT_STR;
        n->as.lit.text = merged; /* includes quotes; unescaped at eval */
        n->as.lit.len = mlen;
        return n;
    }
    if (t.type == TOK_TRUE || t.type == TOK_FALSE || t.type == TOK_NULL) {
        advance(p);
        Node *n = nalloc(N_LITERAL, t.line);
        n->as.lit.kind = (t.type == TOK_TRUE) ? LIT_TRUE
                       : (t.type == TOK_FALSE) ? LIT_FALSE : LIT_NULL;
        return n;
    }
    if (t.type == TOK_LPAREN) {
        /* `(a, b) => { ... }`: arrow-function params. Probe parse_params and
         * roll back to a plain grouped expression when the next token isn't
         * `=>` (e.g. `(a) * 3` or `if ((a))`). */
        size_t save_pos = p->pos;
        bool err_was_empty = !p->errbuf[0];
        char **arrow_names = NULL;
        Type **arrow_types = NULL;
        int arrow_arity = 0;
        if (parse_params(p, &arrow_names, &arrow_types, &arrow_arity) &&
            check(p, TOK_ARROW)) {
            advance(p); /* consume `=>` */
            Node *n = nalloc(N_FUNC_LIT, t.line);
            n->as.funclit.names = arrow_names;
            n->as.funclit.param_types = arrow_types;
            n->as.funclit.arity = arrow_arity;
            n->as.funclit.ret = NULL;    /* arrows carry no return annotation */
            if (check(p, TOK_LBRACE)) {
                n->as.funclit.body = parse_block(p);  /* `=> { ... }` 块体 */
            } else {
                /* `=> expr` 表达式体: 隐式 return, 合成 { return expr; }。
                 * 注意 `=> {` 一律按块体(与 map 字面量体的歧义按 block 优先,
                 * 想返回 map 字面量写 `=> ({...})` 即可)。 */
                Node *expr = parse_expression(p);
                if (!expr) return NULL;
                Node *ret = nalloc(N_RETURN, expr->line);
                ret->as.ret.expr = expr;
                n->as.funclit.body = nalloc(N_BLOCK, expr->line);
                n->as.funclit.body->as.block.stmts =
                    malloc(sizeof(Node *));
                n->as.funclit.body->as.block.stmts[0] = ret;
                n->as.funclit.body->as.block.count = 1;
            }
            if (!n->as.funclit.body) return NULL;
            return n;
        }
        /* Not an arrow function: free the probe and re-parse as grouping. */
        for (int i = 0; i < arrow_arity; i++) free(arrow_names[i]);
        free(arrow_names);
        free(arrow_types);
        p->pos = save_pos;
        if (err_was_empty) p->errbuf[0] = '\0';
        advance(p);
        Node *e = parse_expression(p);
        if (!e) return NULL;
        if (!expect(p, TOK_RPAREN)) return NULL;
        return e;
    }
    if (t.type == TOK_LBRACE) {
        /* map literal */
        advance(p);
        Node *n = nalloc(N_MAP_LIT, t.line);
        n->as.map.keys = NULL;
        n->as.map.vals = NULL;
        n->as.map.count = 0;
        while (!check(p, TOK_RBRACE) && !at_end(p)) {
            if (!is_field_token(peek(p).type)) {
                perror_at(p, peek(p).line, "expected map key (name or string)", NULL);
                return NULL;
            }
            Token k = peek(p);
            advance(p);
            if (!expect(p, TOK_COLON)) return NULL;
            Node *v = parse_expression(p);
            if (!v) return NULL;
            n->as.map.keys = realloc(n->as.map.keys,
                                     sizeof(char *) * ((size_t)n->as.map.count + 1));
            n->as.map.vals = realloc(n->as.map.vals,
                                     sizeof(Node *) * ((size_t)n->as.map.count + 1));
            n->as.map.keys[n->as.map.count] = ident_name(p, k);
            n->as.map.vals[n->as.map.count] = v;
            n->as.map.count++;
            if (!match(p, TOK_COMMA)) break;
            if (check(p, TOK_RBRACE)) break; /* trailing comma */
        }
        if (!expect(p, TOK_RBRACE)) return NULL;
        return n;
    }
    if (t.type == TOK_LBRACKET) {
        advance(p);
        Node *n = nalloc(N_LIST_LIT, t.line);
        n->as.list.items = NULL;
        n->as.list.count = 0;
        while (!check(p, TOK_RBRACKET) && !at_end(p)) {
            Node *e = parse_expression(p);
            if (!e) return NULL;
            n->as.list.items = realloc(n->as.list.items,
                                       sizeof(Node *) * ((size_t)n->as.list.count + 1));
            n->as.list.items[n->as.list.count++] = e;
            if (!match(p, TOK_COMMA)) break;
            if (check(p, TOK_RBRACKET)) break;
        }
        if (!expect(p, TOK_RBRACKET)) return NULL;
        return n;
    }
    if (t.type == TOK_FUNC) {
        size_t line = t.line;
        advance(p);
        return parse_func_literal(p, line);
    }
    if (t.type == TOK_IDENT || t.type == TOK_TYPE || t.type == TOK_INT ||
        t.type == TOK_FLOAT || t.type == TOK_KW_STRING || t.type == TOK_BOOL ||
        t.type == TOK_RESULT || is_method_keyword(t.type)) {
        advance(p);
        Node *n = nalloc(N_VAR, t.line);
        n->as.var.name = ident_name(p, t);
        return n;
    }

    perror_at(p, t.line, "unexpected token '%s' in expression",
              token_type_name(t.type));
    return NULL;
}

static Node *parse_postfix(Parser *p) {
    Node *n = parse_primary(p);
    if (!n) return NULL;
    while (true) {
        if (match(p, TOK_LPAREN)) {
            Node *args = nalloc(N_LIST_LIT, previous_line(p));
            args->as.list.items = NULL;
            args->as.list.count = 0;
            if (!check(p, TOK_RPAREN)) {
                for (;;) {
                    Node *e = parse_expression(p);
                    if (!e) return NULL;
                    args->as.list.items = realloc(args->as.list.items,
                                                  sizeof(Node *) * ((size_t)args->as.list.count + 1));
                    args->as.list.items[args->as.list.count++] = e;
                    if (!match(p, TOK_COMMA)) break;
                    if (check(p, TOK_RPAREN)) break; /* trailing comma */
                }
            }
            if (!expect(p, TOK_RPAREN)) return NULL;
            Node *call = nalloc(N_CALL, previous_line(p));
            call->as.call.callee = n;
            call->as.call.args = args->as.list.items;
            call->as.call.argc = args->as.list.count;
            call->as.call.propagate = false;
            free(args);
            if (match(p, TOK_QUESTION))
                call->as.call.propagate = true;
            n = call;
        } else if (match(p, TOK_DOT)) {
            if (!is_field_token(peek(p).type)) {
                perror_at(p, peek(p).line, "expected field name after '.'", NULL);
                return NULL;
            }
            Token f = peek(p);
            advance(p);
            Node *member = nalloc(N_MEMBER, f.line);
            member->as.member.obj = n;
            member->as.member.name = ident_name(p, f);
            n = member;
        } else {
            break;
        }
    }
    return n;
}

static Node *parse_unary(Parser *p) {
    Token t = peek(p);
    if (t.type == TOK_NOT || t.type == TOK_MINUS) {
        advance(p);
        Node *operand = parse_unary(p);
        if (!operand) return NULL;
        Node *n = nalloc(N_UNARY, t.line);
        n->as.unary.op = (t.type == TOK_NOT) ? OP_NOT : OP_NEG;
        n->as.unary.operand = operand;
        return n;
    }
    return parse_postfix(p);
}

static Node *parse_factor(Parser *p) {
    Node *left = parse_unary(p);
    if (!left) return NULL;
    while (check(p, TOK_STAR) || check(p, TOK_SLASH) || check(p, TOK_PERCENT)) {
        Token t = peek(p);
        advance(p);
        Node *right = parse_unary(p);
        if (!right) return NULL;
        Node *n = nalloc(N_BINARY, t.line);
        n->as.binary.op = (t.type == TOK_STAR) ? OP_MUL
                        : (t.type == TOK_SLASH) ? OP_DIV : OP_MOD;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

static Node *parse_term(Parser *p) {
    Node *left = parse_factor(p);
    if (!left) return NULL;
    while (check(p, TOK_PLUS) || check(p, TOK_MINUS)) {
        Token t = peek(p);
        advance(p);
        Node *right = parse_factor(p);
        if (!right) return NULL;
        Node *n = nalloc(N_BINARY, t.line);
        n->as.binary.op = (t.type == TOK_PLUS) ? OP_ADD : OP_SUB;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

static Node *parse_comparison(Parser *p) {
    Node *left = parse_term(p);
    if (!left) return NULL;
    while (check(p, TOK_LT) || check(p, TOK_LE) || check(p, TOK_GT) || check(p, TOK_GE)) {
        Token t = peek(p);
        advance(p);
        Node *right = parse_term(p);
        if (!right) return NULL;
        Node *n = nalloc(N_BINARY, t.line);
        n->as.binary.op = (t.type == TOK_LT) ? OP_LT
                        : (t.type == TOK_LE) ? OP_LE
                        : (t.type == TOK_GT) ? OP_GT : OP_GE;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

static Node *parse_equality(Parser *p) {
    Node *left = parse_comparison(p);
    if (!left) return NULL;
    while (check(p, TOK_EQEQ) || check(p, TOK_NEQ)) {
        Token t = peek(p);
        advance(p);
        Node *right = parse_comparison(p);
        if (!right) return NULL;
        Node *n = nalloc(N_BINARY, t.line);
        n->as.binary.op = (t.type == TOK_EQEQ) ? OP_EQ : OP_NE;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

static Node *parse_and(Parser *p) {
    Node *left = parse_equality(p);
    if (!left) return NULL;
    while (check(p, TOK_AND)) {
        Token t = peek(p);
        advance(p);
        Node *right = parse_equality(p);
        if (!right) return NULL;
        Node *n = nalloc(N_BINARY, t.line);
        n->as.binary.op = OP_AND;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

static Node *parse_or(Parser *p) {
    Node *left = parse_and(p);
    if (!left) return NULL;
    while (check(p, TOK_OR)) {
        Token t = peek(p);
        advance(p);
        Node *right = parse_and(p);
        if (!right) return NULL;
        Node *n = nalloc(N_BINARY, t.line);
        n->as.binary.op = OP_OR;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

Node *parse_expression(Parser *p) {
    Node *expr = parse_or(p);
    if (!expr) return NULL;
    /* assignment: lhs must be a variable or a member access. */
    if (match(p, TOK_EQ)) {
        Node *value = parse_expression(p);
        if (!value) return NULL;
        if (expr->type == N_VAR) {
            Node *n = nalloc(N_ASSIGN, previous_line(p));
            n->as.assign.name = expr->as.var.name;
            n->as.assign.value = value;
            free(expr);
            return n;
        }
        if (expr->type == N_MEMBER) {
            Node *n = nalloc(N_ASSIGN_MEMBER, previous_line(p));
            n->as.assign_mem.obj = expr->as.member.obj;
            n->as.assign_mem.name = expr->as.member.name;
            n->as.assign_mem.value = value;
            free(expr);
            return n;
        }
        perror_at(p, previous_line(p), "invalid assignment target", NULL);
        return NULL;
    }
    return expr;
}
