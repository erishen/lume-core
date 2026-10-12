/* 表达式解析:parse_primary → parse_expression 的优先级链——
 * 从 parser.c 拆出(2026-09-27)。入口 parse_expression 供语句/路由尾
 * 调用;链内各层(primary/postfix/unary/factor/term/comparison/equality/
 * and/or)仅本文件内部使用。 */

#include "parser_internal.h"
#include <limits.h>

static Node *parse_primary(Parser *p) {
    Token t = peek(p);

    if (t.type == TOK_NUMBER) {
        advance(p);
        Node *n = nalloc(N_LITERAL, t.line);
        n->as.lit.kind = LIT_NUM;
        /* The lexer already classified the literal (it had to, to pick
         * strtoll over strtod) and carries the i64 value intact, so this no
         * longer re-derives floatness from the text nor squeezes the integer
         * through a double. */
        n->as.lit.is_float = t.is_int == 0;
        n->as.lit.inum = t.inum;
        n->as.lit.num = t.num;
        n->as.lit.neg_min = t.neg_min;
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
    if (t.type == TOK_MATCH) {
        /* match (scrutinee) { Variant(b, ...) => expr, _ => expr, ... }
         * An expression, not a statement — it carries a value like the
         * ternary does. Arms are comma-separated; the trailing comma is
         * allowed. `_` is the catch-all variant name (binds forbidden). */
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        Node *scrutinee = parse_expression(p);
        if (!scrutinee) return NULL;
        if (!expect(p, TOK_RPAREN)) return NULL;
        if (!expect(p, TOK_LBRACE)) return NULL;
        Node *n = nalloc(N_MATCH, t.line);
        n->as.matchn.scrutinee = scrutinee;
        n->as.matchn.arms = NULL;
        n->as.matchn.armc = 0;
        if (check(p, TOK_RBRACE)) {
            perror_at(p, peek(p).line, "match needs at least one arm", NULL);
            return NULL;
        }
        while (!check(p, TOK_RBRACE) && !at_end(p)) {
            Token pt = peek(p);
            if (pt.type != TOK_IDENT) {
                perror_at(p, pt.line,
                          "expected variant name in match arm", NULL);
                return NULL;
            }
            advance(p);
            n->as.matchn.arms = realloc(n->as.matchn.arms,
                    sizeof(MatchArm) * ((size_t)n->as.matchn.armc + 1));
            MatchArm *arm = &n->as.matchn.arms[n->as.matchn.armc];
            memset(arm, 0, sizeof *arm);
            arm->vname = ident_name(p, pt);
            arm->line = pt.line;
            if (match(p, TOK_LPAREN)) {
                /* positional binds — plain identifiers separated by commas */
                do {
                    Token bt = peek(p);
                    if (bt.type != TOK_IDENT) {
                        perror_at(p, bt.line,
                                  "expected a binding name in match arm", NULL);
                        return NULL;
                    }
                    advance(p);
                    arm->binds = realloc(arm->binds,
                            sizeof(char *) * ((size_t)arm->bindc + 1));
                    arm->binds[arm->bindc++] = ident_name(p, bt);
                } while (match(p, TOK_COMMA));
                if (!expect(p, TOK_RPAREN)) return NULL;
            }
            if (!expect(p, TOK_ARROW)) return NULL;
            arm->body = parse_expression(p);
            if (!arm->body) return NULL;
            n->as.matchn.armc++;
            if (!match(p, TOK_COMMA)) break;
            if (check(p, TOK_RBRACE)) break; /* trailing comma */
        }
        if (!expect(p, TOK_RBRACE)) return NULL;
        return n;
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

/* 三元前瞻：当前位置是 `?`（call 后缀处的传播/三元歧义）。向后扫描——
 * 在遇到 `;`/EOF、深度回落到 0 以下的闭括号、或 depth 0 的 `,` 之前，
 * 存在 depth 0 的配对 `:` 则这是三元运算符（条件 ?: 结果）。`?` 后紧跟
 * `:` 也按传播处理（三元中段不能为空）。
 *
 * 消歧主规则是**贴邻**：`f()?` 紧贴（无空白）一律传播；`f() ? a : b`
 * 空格分隔时再看冒号前瞻。token 流上 `f()? - 1` 与 `f() ? -1 : x` 本征
 * 不可区分，贴邻给了确定解，也让传播与三元自由连用
 * （`divide(8,2)? > 3 ? "big" : "small"`）。已知极限：空格分隔且无配对
 * 冒号的 `f() ? - 1` 会按传播兜底（本就不是合法三元）。 */
static bool ternary_colon_ahead(Parser *p)
{
    size_t save = p->pos; /* 指向 '?' */
    advance(p);           /* 跳过 '?' */
    if (check(p, TOK_COLON)) { p->pos = save; return false; }
    int depth = 0;
    bool found = false;
    while (!at_end(p)) {
        Token t = peek(p);
        if (t.type == TOK_SEMI) break;
        if (t.type == TOK_LPAREN || t.type == TOK_LBRACE ||
            t.type == TOK_LBRACKET) {
            depth++;
        } else if (t.type == TOK_RPAREN || t.type == TOK_RBRACE ||
                   t.type == TOK_RBRACKET) {
            if (depth == 0) break;
            depth--;
        } else if (t.type == TOK_COMMA && depth == 0) {
            break;
        } else if (t.type == TOK_COLON && depth == 0) {
            found = true;
            break;
        }
        advance(p);
    }
    p->pos = save;
    return found;
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
            /* The items array now belongs to call; only the shell goes away
             * here, and it has to leave the orphan batch first (node_free()
             * would reach the items array a second time via node_free_own). */
            node_unjournal(args);
            free(args);
            /* `?` 歧义裁决（贴邻优先）：`f()?` 紧贴 → Result 传播；空格
             * 分隔时做冒号前瞻——有配对冒号 → 三元，`?` 不消费、留给
             * parse_ternary（此处的 `?` 后不可能跟 `(`/`.`/`[` 链，提前
             * 返回安全）；无配对冒号 → 兜底按传播。 */
            if (check(p, TOK_QUESTION)) {
                Token qtok = peek(p);
                Token prev = p->toks[p->pos - 1];
                bool adjacent = qtok.line == prev.line &&
                                qtok.start == prev.start + prev.length;
                if (adjacent) {
                    advance(p);
                    call->as.call.propagate = true;
                } else if (ternary_colon_ahead(p)) {
                    n = call;
                    return n;
                } else {
                    advance(p);
                    call->as.call.propagate = true;
                }
            }
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
        } else if (check(p, TOK_LBRACKET)) {
            /* `m["k"]` / `l[0]`. A postfix loop rather than part of parse_primary
             * so it chains: `m["a"][0]`, `rows[i]["name"]`. The index is a full
             * expression, not a literal, because the key is usually a variable
             * and `m[k]` is the case worth having. */
            Token br = peek(p);
            advance(p);
            Node *ix = parse_expression(p);
            if (!ix) return NULL;
            if (!expect(p, TOK_RBRACKET)) return NULL;
            Node *index = nalloc(N_INDEX, br.line);
            index->as.index.obj = n;
            index->as.index.index = ix;
            n = index;
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
        /* `-9223372036854775808` is the one literal whose magnitude overflows
         * i64: the lexer kept the clamped 2^63 and flagged it, and this is the
         * only place that can supply the missing negation. Fold it here so the
         * AST holds LLONG_MIN exactly, and drop the OP_NEG wrapper since the
         * value is already correct — leaving it in would negate twice.
         *
         * The shell has to be freed before returning: it came from nalloc, so
         * it is on the orphan journal, and it never becomes part of the tree
         * (nothing holds a pointer to it) — returning it without releasing
         * stranded exactly one node per run, which is what LeakSanitizer
         * reported on Linux CI (80 bytes, one object, constant regardless of
         * how many statements the script had). Released the way this file
         * releases every other discarded shell: unjournal, then free. A plain
         * free rather than node_free_own() because an N_UNARY carries no
         * owned arrays — only `operand`, which stays untouched and is the node
         * being returned. */
        if (n->as.unary.op == OP_NEG && operand->type == N_LITERAL &&
            operand->as.lit.kind == LIT_NUM && operand->as.lit.neg_min) {
            node_unjournal(n);
            free(n);
            operand->as.lit.inum = LLONG_MIN;
            operand->as.lit.num = (double)LLONG_MIN;
            operand->as.lit.neg_min = 0;
            return operand;
        }
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

/* 三元 `cond ? then : els`：介于逻辑或与赋值之间，右结合。中段是完整
 * 表达式（允许再套三元/赋值），else 分支递归 parse_ternary 实现
 * `a ? b : c ? d : e` 的右折叠。postfix 的 `f()?` 传播已由
 * ternary_colon_ahead 前瞻让位，这里看到的 `?` 一律是三元。 */
static Node *parse_ternary(Parser *p) {
    Node *cond = parse_or(p);
    if (!cond) return NULL;
    if (!check(p, TOK_QUESTION)) return cond;
    size_t line = peek(p).line;
    advance(p);
    Node *then_e = parse_expression(p);
    if (!then_e) return NULL;
    if (!expect(p, TOK_COLON)) return NULL;
    Node *else_e = parse_ternary(p);
    if (!else_e) return NULL;
    Node *n = nalloc(N_TERNARY, line);
    n->as.ternary.cond = cond;
    n->as.ternary.then = then_e;
    n->as.ternary.els  = else_e;
    return n;
}

Node *parse_expression(Parser *p) {
    Node *expr = parse_ternary(p);
    if (!expr) return NULL;
    /* assignment: lhs must be a variable or a member access. */
    if (match(p, TOK_EQ)) {
        Node *value = parse_expression(p);
        if (!value) return NULL;
        if (expr->type == N_VAR) {
            Node *n = nalloc(N_ASSIGN, previous_line(p));
            n->as.assign.name = expr->as.var.name;
            n->as.assign.value = value;
            node_unjournal(expr); /* shell only: name and value moved over */
            free(expr);
            return n;
        }
        if (expr->type == N_MEMBER) {
            Node *n = nalloc(N_ASSIGN_MEMBER, previous_line(p));
            n->as.assign_mem.obj = expr->as.member.obj;
            n->as.assign_mem.name = expr->as.member.name;
            n->as.assign_mem.value = value;
            node_unjournal(expr); /* shell only: obj and name moved over */
            free(expr);
            return n;
        }
        perror_at(p, previous_line(p), "invalid assignment target", NULL);
        return NULL;
    }
    return expr;
}
