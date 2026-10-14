/* 表达式解析:parse_primary → parse_expression 的优先级链——
 * 从 parser.c 拆出(2026-09-27)。入口 parse_expression 供语句/路由尾
 * 调用;链内各层(primary/postfix/unary/factor/term/comparison/equality/
 * and/or)仅本文件内部使用。 */

#include "parser_internal.h"
#include <limits.h>

/* ---------- interpolated string segments ---------- */

/* Parse one expression segment of an s!"..." with a fresh lexer+parser. The
 * segment text is a slice of the source buffer, so it is copied to a
 * NUL-terminated scratch first. The sub-parser's own line numbers restart at
 * 1, so its message is re-stamped with the interpolation's line: "line 5:
 * expected ..." instead of a nested "line 5: line 1: ...". */
static Node *parse_istr_segment(Parser *p, const char *text, int n,
                                size_t line) {
    char *scratch = malloc((size_t)n + 1);
    if (n) memcpy(scratch, text, (size_t)n);
    scratch[n] = '\0';

    char lexerr[256] = {0};
    int count = 0;
    Token *toks = al_lex(scratch, lexerr, sizeof(lexerr), &count);
    Node *result = NULL;

    if (!toks) {
        perror_at(p, line, "%s", lexerr);
    } else {
        Parser sp;
        memset(&sp, 0, sizeof(sp));
        sp.toks = toks;
        sp.count = count;
        Node *e = parse_expression(&sp);
        if (!e) {
            /* Strip the sub-parser's "line N: " prefix (it counted lines in
             * the scratch copy, where the segment is line 1). */
            const char *msg = sp.errbuf;
            const char *colon = strchr(msg, ':');
            if (colon && colon > msg && strncmp(msg, "line ", 5) == 0)
                msg = colon + 1;
            while (*msg == ' ') msg++;
            perror_at(p, line, "%s", msg);
        } else if (!at_end(&sp)) {
            perror_at(p, line,
                      "unexpected trailing tokens in interpolation segment");
        } else {
            result = e;
        }
        for (int i = 0; i < count; i++) free(toks[i].istr);
        free(toks);
    }
    free(scratch);
    return result;
}

/* Desugar a scanned s!"..." into `str(seg) + str(seg) + ...`: literal segments
 * become string literals, expression segments become str() calls. The tree is
 * made of ordinary nodes, so the checker and both emitters never learn that
 * interpolation exists — one grammar, three consumers. Adjacent literals were
 * already merged by the scanner (a literal run is a single 'L' segment). */
static Node *istr_build(Parser *p, Token t) {
    const char *s = t.istr;
    int len = t.istr_len;
    int off = 0;
    Node *acc = NULL;

    while (off < len) {
        char tag = s[off];
        off += 1;
        int32_t n = 0;
        memcpy(&n, s + off, sizeof(n));
        off += (int)sizeof(n);
        const char *data = s + off;
        off += n;

        Node *part;
        if (tag == 'L') {
            /* Quote-wrapped, exactly like parse_primary's merged literal: the
             * LIT_STR contract is "text includes the quotes, consumers
             * unescape" — interp.c and both emitters rely on it. The lexer
             * already collapsed `{{`/`}}` here, so the braces survive as
             * literal characters with no escape meaning. */
            part = nalloc(N_LITERAL, t.line);
            part->as.lit.kind = LIT_STR;
            part->as.lit.text = malloc((size_t)n + 3);
            part->as.lit.text[0] = '"';
            if (n) memcpy(part->as.lit.text + 1, data, (size_t)n);
            part->as.lit.text[n + 1] = '"';
            part->as.lit.text[n + 2] = '\0';
            part->as.lit.len = n + 2;
        } else if (tag == 'E') {
            Node *e = parse_istr_segment(p, data, n, t.line);
            if (!e) return NULL;
            part = nalloc(N_CALL, t.line);
            part->as.call.callee = nalloc(N_VAR, t.line);
            part->as.call.callee->as.var.name = strdup("str");
            part->as.call.args = malloc(sizeof(Node *));
            part->as.call.args[0] = e;
            part->as.call.argc = 1;
        } else {
            perror_at(p, t.line, "corrupt interpolation segment (tag '%c')",
                      tag);
            return NULL;
        }

        if (!acc) {
            acc = part;
        } else {
            Node *b = nalloc(N_BINARY, t.line);
            b->as.binary.op = OP_ADD;
            b->as.binary.left = acc;
            b->as.binary.right = part;
            acc = b;
        }
    }

    if (!acc) {
        /* s!"" — no segments at all: an empty string literal, still spelled
         * quote-inclusive like every other LIT_STR (len 0 here would underflow
         * the n-2 body span every consumer computes). */
        acc = nalloc(N_LITERAL, t.line);
        acc->as.lit.kind = LIT_STR;
        acc->as.lit.text = strdup("\"\"");
        acc->as.lit.len = 2;
    }
    return acc;
}

/* `{ base with k: v, ... }` — Lean's structure update. The base was already
 * parsed by the caller (this is only reached when the lookahead proved the
 * brace is not a map literal: a map's first token is a key, and a key is never
 * followed by `with`, `(`, `.` or `[`). */
static Node *parse_struct_update(Parser *p, Node *base, size_t line) {
    if (!(check(p, TOK_IDENT) && peek(p).length == 4 &&
          strncmp(peek(p).start, "with", 4) == 0)) {
        perror_at(p, peek(p).line,
                  "expected 'with' after the base of a structure update "
                  "('{ base with field: value, ... }')", NULL);
        return NULL;
    }
    advance(p); /* with */

    Node *n = nalloc(N_STRUCT_UPDATE, line);
    n->as.supd.base = base;
    n->as.supd.keys = NULL;
    n->as.supd.vals = NULL;
    n->as.supd.count = 0;

    while (!check(p, TOK_RBRACE) && !at_end(p)) {
        if (!is_field_token(peek(p).type)) {
            perror_at(p, peek(p).line,
                      "expected field name after 'with' (name or string)",
                      NULL);
            return NULL;
        }
        Token k = peek(p);
        advance(p);
        if (!expect(p, TOK_COLON)) return NULL;
        Node *v = parse_expression(p);
        if (!v) return NULL;
        /* Duplicate keys are a source bug, not an override: Lean rejects them
         * too. A linear scan is fine — with-clauses are a handful of fields. */
        char *key = ident_name(p, k);
        for (int i = 0; i < n->as.supd.count; i++) {
            if (strcmp(n->as.supd.keys[i], key) == 0) {
                free(key);
                perror_at(p, k.line,
                          "duplicate field '%.*s' in with update",
                          k.length, k.start);
                return NULL;
            }
        }
        free(key);
        n->as.supd.keys = realloc(n->as.supd.keys,
                                  sizeof(char *) * ((size_t)n->as.supd.count + 1));
        n->as.supd.vals = realloc(n->as.supd.vals,
                                  sizeof(Node *) * ((size_t)n->as.supd.count + 1));
        n->as.supd.keys[n->as.supd.count] = ident_name(p, k);
        n->as.supd.vals[n->as.supd.count] = v;
        n->as.supd.count++;
        if (!match(p, TOK_COMMA)) break;
        if (check(p, TOK_RBRACE)) break; /* trailing comma */
    }
    if (!expect(p, TOK_RBRACE)) return NULL;
    if (n->as.supd.count == 0) {
        perror_at(p, line,
                  "with update needs at least one field "
                  "('{ base with field: value }')", NULL);
        return NULL;
    }
    return n;
}

static Node *parse_ternary(Parser *p);

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
    if (t.type == TOK_ISTR) {
        /* s!"..." — the lexer scanned the segments; desugar to str()+concat.
         * The token's payload stays owned by the token sweep (parse_program
         * frees every token's istr), so nothing is released here. */
        advance(p);
        return istr_build(p, t);
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
        if (check(p, TOK_COMMA)) {
            /* Tuple literal `(a, b, ...)`: an anonymous struct with
             * positional fields `_0`, `_1`, ... A struct in lume is just a
             * map literal whose keys are identifiers (see N_LET_PAT's
             * tuple destructuring and the destructuring tests), so we build
             * an N_MAP_LIT with auto-numbered identifier keys. `(a)` with no
             * comma stays a plain grouped expression; `(a,)` is a 1-tuple. */
            Node **items = NULL;
            int count = 0;
            items = realloc(items, sizeof(Node *) * ((size_t)count + 1));
            items[count++] = e;
            for (;;) {
                if (!match(p, TOK_COMMA)) break;
                if (check(p, TOK_RPAREN)) break;   /* trailing comma */
                Node *it = parse_expression(p);
                if (!it) {
                    for (int i = 0; i < count; i++) node_free(items[i]);
                    free(items);
                    return NULL;
                }
                items = realloc(items, sizeof(Node *) * ((size_t)count + 1));
                items[count++] = it;
            }
            if (!expect(p, TOK_RPAREN)) {
                for (int i = 0; i < count; i++) node_free(items[i]);
                free(items);
                return NULL;
            }
            Node *n = nalloc(N_MAP_LIT, t.line);
            n->as.map.keys = malloc(sizeof(char *) * (size_t)count);
            n->as.map.vals = malloc(sizeof(Node *) * (size_t)count);
            n->as.map.count = count;
            n->as.map.is_tuple = true;
            for (int i = 0; i < count; i++) {
                char fbuf[16];
                snprintf(fbuf, sizeof fbuf, "_%d", i);
                n->as.map.keys[i] = dupcstr(fbuf);
                n->as.map.vals[i] = items[i];
            }
            free(items);
            return n;
        }
        if (!expect(p, TOK_RPAREN)) return NULL;
        return e;
    }
    if (t.type == TOK_LBRACE) {
        /* `{ ... }` is a map literal or a structure update, decided by pure
         * lookahead (no rollback, so no abandoned nodes): a map's first token
         * is a key (ident or string), and a key is never followed by `with`,
         * `(`, `.` or `[`. `{ p with ... }` / `{ f(x) with ... }` /
         * `{ {..} with ... }` therefore take the update path, while every map
         * spelling — including bare-ident keys — keeps the literal path and
         * its existing diagnostics. */
        Token t1 = p->toks[p->pos + 1]; /* first token after `{`; the array is
                                         * EOF-terminated, so this is in range */
        Token t2 = t1.type != TOK_EOF ? p->toks[p->pos + 2] : t1;
        bool is_update;
        if (t1.type == TOK_RBRACE)
            is_update = false;                       /* {} empty map literal */
        else if (t1.type == TOK_STRING)
            is_update = false;                       /* {"k": v} */
        else if (t1.type == TOK_IDENT && t2.type == TOK_IDENT &&
                 t2.length == 4 && strncmp(t2.start, "with", 4) == 0)
            is_update = true;                        /* { p with ... } */
        else if (t1.type == TOK_IDENT && t2.type == TOK_COLON)
            is_update = false;                       /* { k: v } bare key */
        else if (t1.type == TOK_IDENT &&
                 (t2.type == TOK_LPAREN || t2.type == TOK_DOT ||
                  t2.type == TOK_LBRACKET))
            is_update = true;                        /* { f(x) with ... } */
        else if (is_field_token(t1.type))
            is_update = false;                       /* map: { k, ... } error */
        else
            is_update = true;                        /* { (e) with ... } etc */

        if (is_update) {
            advance(p); /* consume `{` */
            Node *base = parse_expression(p);
            if (!base) return NULL;
            return parse_struct_update(p, base, t.line);
        }

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
        /* match (scrutinee) { Pattern [| Pattern] [if guard] => expr, ... }
         * An expression, not a statement — it carries a value like the
         * ternary does. Arms are comma-separated; the trailing comma is
         * allowed. `_` is the catch-all pattern (binds forbidden). */
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
            size_t pline = peek(p).line;
            MatchPat *pats = NULL;
            int patc = 0;
            if (!parse_patterns(p, &pats, &patc)) return NULL;
            /* Optional guard: `if cond` after the pattern list, before `=>`.
             * The expression parser stops at `=>` (not a binary operator),
             * so no lookahead games are needed. */
            Node *guard = NULL;
            if (match(p, TOK_IF)) {
                guard = parse_expression(p);
                if (!guard) return NULL;
            }
            if (!expect(p, TOK_ARROW)) return NULL;
            Node *body = parse_expression(p);
            if (!body) return NULL;
            n->as.matchn.arms = realloc(n->as.matchn.arms,
                    sizeof(MatchArm) * ((size_t)n->as.matchn.armc + 1));
            MatchArm *arm = &n->as.matchn.arms[n->as.matchn.armc];
            memset(arm, 0, sizeof *arm);
            arm->pats = pats;
            arm->patc = patc;
            arm->guard = guard;
            arm->body = body;
            arm->line = pline;
            n->as.matchn.armc++;
            if (!match(p, TOK_COMMA)) break;
            if (check(p, TOK_RBRACE)) break; /* trailing comma */
        }
        if (!expect(p, TOK_RBRACE)) return NULL;
        return n;
    }
    if (t.type == TOK_IF) {
        /* `if (cond) expr else expr` in expression position — the Lean
         * if-then-else. Folded onto the very same N_TERNARY the `? :`
         * operator builds, so the checker and all three emitters reuse the
         * ternary path verbatim: only the taken branch is evaluated, and the
         * branch types unify under the ternary rules (same type -> that type;
         * int/float mix -> float; anything else is a type error).
         *
         * The else is required: without it the false branch has no value, the
         * same reason a bare `c ? a` is not a ternary. else-if chains fold to
         * the right exactly like `a ? b : c ? d : e`.
         *
         * No braces: `{` already means a map literal / structure update in
         * expression position, so `if (c) { 1 } else { 2 }` would be ambiguous.
         * The statement form `if (cond) { ... }` in parse_statement() keeps
         * its braces and stays a statement (it needs no else either). */
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        Node *cond = parse_expression(p);
        if (!cond) return NULL;
        if (!expect(p, TOK_RPAREN)) return NULL;
        Node *then_e = parse_expression(p);
        if (!then_e) return NULL;
        if (!check(p, TOK_ELSE)) {
            perror_at(p, peek(p).line,
                      "an if-expression needs an else branch", NULL);
            return NULL;
        }
        advance(p); /* consume `else` */
        Node *els_e = parse_ternary(p);   /* right-assoc: else-if chains */
        if (!els_e) return NULL;
        Node *n = nalloc(N_TERNARY, t.line);
        n->as.ternary.cond = cond;
        n->as.ternary.then = then_e;
        n->as.ternary.els  = els_e;
        return n;
    }
    if (t.type == TOK_LET) {
        /* `let x = e1 in e2` in expression position — Lean's let-in. The
         * binding is visible only inside e2, and e2 is the expression's
         * value. Statement position never reaches here (parse_statement
         * claims a leading `let` first and ends a plain binding with `;`);
         * this path fires where an expression may start, e.g. call
         * arguments, return values, match-arm bodies, grouped operands.
         * The body is a full expression, so it extends as far right as
         * possible: `let x = 1 in x + f(x)` puts both operands inside. */
        advance(p);
        if (!check(p, TOK_IDENT)) {
            perror_at(p, peek(p).line, "expected a name after 'let'", NULL);
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
        if (!check(p, TOK_IN)) {
            perror_at(p, peek(p).line,
                      "a let-expression needs 'in' (a statement let ends with ';')",
                      NULL);
            return NULL;
        }
        advance(p); /* consume `in` */
        Node *body = parse_expression(p);
        if (!body) return NULL;
        Node *n = nalloc(N_LET_EXPR, t.line);
        n->as.letx.name = ident_name(p, name);
        n->as.letx.annot = annot;
        n->as.letx.init = init;
        n->as.letx.body = body;
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

/* `{ |a, b| body }` — the trailing-closure form, reached only when the
 * caller proved by lookahead that `{` is followed by `|`. The body is a
 * statement sequence whose final expression (the one with no trailing
 * `;`) becomes the closure's value, exactly like a Rust block: a trailing
 * `;` discards it. Parameters are plain names — types come from inference,
 * exactly as in the arrow form. Folds into an ordinary N_FUNC_LIT, so the
 * checker and both emitters never learn that trailing closures exist. */
static Node *parse_trailing_closure(Parser *p) {
    size_t line = peek(p).line;
    char **names = NULL;
    Type **types = NULL;
    int arity = 0;
    advance(p);                       /* { */
    if (check(p, TOK_OR)) {
        /* `{ || body }` — zero parameters. The lexer fused the two pipes
         * into one `||` (TOK_OR) token, so this is both the opening and
         * the closing delimiter at once. */
        advance(p);
    } else {
        if (!expect(p, TOK_PIPE)) goto fail;
        if (!check(p, TOK_PIPE)) {
            for (;;) {
                if (!check(p, TOK_IDENT)) {
                    perror_at(p, peek(p).line,
                              "trailing closure parameters must be plain names "
                              "('{ |a, b| body }')", NULL);
                    goto fail;
                }
                Token nm = peek(p);
                advance(p);
                names = realloc(names, sizeof(char *) * ((size_t)arity + 1));
                types = realloc(types, sizeof(Type *) * ((size_t)arity + 1));
                names[arity] = ident_name(p, nm);
                types[arity] = NULL;
                /* Optional `: type`, same as the arrow form — the native
                 * backends need a known element type when the callee's own
                 * signature left the lambda's parameters unconstrained. */
                if (match(p, TOK_COLON)) {
                    types[arity] = parse_type(p);
                    if (!types[arity]) {
                        /* names[arity] is live; freeing the first `arity`
                         * names only would drop it (parse_params' lesson). */
                        for (int i = 0; i <= arity; i++) free(names[i]);
                        free(names);
                        free(types);
                        return NULL;
                    }
                }
                arity++;
                if (!match(p, TOK_COMMA)) break;
                if (check(p, TOK_PIPE)) break;   /* trailing comma */
            }
        }
        if (!expect(p, TOK_PIPE)) goto fail;
    }

    Node *blk = nalloc(N_BLOCK, line);
    blk->as.block.stmts = NULL;
    blk->as.block.count = 0;
    while (!check(p, TOK_RBRACE) && !at_end(p)) {
        /* Dispatch on the first token instead of probing with save/restore:
         * an abandoned probe subtree would sit in the orphan journal with no
         * owner and leak on the success path. Only tokens that can *only*
         * start an expression take the expression path; every statement
         * keyword (let/if/while/for/return/func/...) goes to parse_statement,
         * so the body accepts the same statements a plain block does. */
        TokenType t = peek(p).type;
        bool expr_only = t == TOK_IDENT || t == TOK_NUMBER ||
                         t == TOK_STRING || t == TOK_ISTR ||
                         t == TOK_TRUE || t == TOK_FALSE || t == TOK_NULL ||
                         t == TOK_LPAREN || t == TOK_LBRACKET ||
                         t == TOK_MINUS || t == TOK_NOT || t == TOK_MATCH;
        if (expr_only) {
            Node *e = parse_expression(p);
            if (!e) goto fail;
            if (match(p, TOK_SEMI)) {
                /* statement position: `e;` runs for effect */
                Node *es = nalloc(N_EXPR_STMT, e->line);
                es->as.expr_stmt.expr = e;
                blk->as.block.stmts = realloc(blk->as.block.stmts,
                        sizeof(Node *) * ((size_t)blk->as.block.count + 1));
                blk->as.block.stmts[blk->as.block.count++] = es;
                continue;
            }
            /* no `;` — the closure's value: an implicit return */
            Node *ret = nalloc(N_RETURN, e->line);
            ret->as.ret.expr = e;
            blk->as.block.stmts = realloc(blk->as.block.stmts,
                    sizeof(Node *) * ((size_t)blk->as.block.count + 1));
            blk->as.block.stmts[blk->as.block.count++] = ret;
            break;
        }
        Node *s = parse_statement(p);
        if (!s) goto fail;
        blk->as.block.stmts = realloc(blk->as.block.stmts,
                sizeof(Node *) * ((size_t)blk->as.block.count + 1));
        blk->as.block.stmts[blk->as.block.count++] = s;
    }
    if (!expect(p, TOK_RBRACE)) goto fail;

    Node *n = nalloc(N_FUNC_LIT, line);
    n->as.funclit.names = names;
    n->as.funclit.param_types = types;
    n->as.funclit.arity = arity;
    n->as.funclit.ret = NULL;
    n->as.funclit.body = blk;
    return n;
fail:
    for (int i = 0; i < arity; i++) free(names[i]);
    free(names);
    free(types);
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
            /* Trailing closure: `f(args) { |x| body }` — sugar for passing
             * the closure as one more argument. Recognised by pure lookahead
             * (`{` followed by `|`, or by `||` which the lexer fuses into one
             * TOK_OR token for the zero-parameter form), which no other
             * construct can produce: a map literal's first token is a key, a
             * struct update starts with the base name, and no statement or
             * match arm starts with `|`. That is what keeps `if (f()) { ... }`
             * and `match (f()) { ... }` intact — their braces are followed by
             * a statement or a pattern, never `|`. */
            if (check(p, TOK_LBRACE) &&
                (peek2(p).type == TOK_PIPE || peek2(p).type == TOK_OR)) {
                Node *clo = parse_trailing_closure(p);
                if (!clo) return NULL;
                call->as.call.args = realloc(call->as.call.args,
                        sizeof(Node *) * ((size_t)call->as.call.argc + 1));
                call->as.call.args[call->as.call.argc++] = clo;
            }
            n = call;
        } else if (match(p, TOK_DOT)) {
            Token f = peek(p);
            bool is_int_idx = (f.type == TOK_NUMBER && f.is_int);
            if (!is_field_token(f.type) && !is_int_idx) {
                perror_at(p, f.line, "expected field name after '.'", NULL);
                return NULL;
            }
            advance(p);
            char *fname;
            if (is_int_idx) {
                /* tuple index: `p.0` / `p.1` read the positional fields
                 * `_0` / `_1` of the struct a tuple is (numbers are
                 * TOK_NUMBER with is_int, not a dedicated TOK_INT). */
                size_t need = (size_t)f.length + 2;
                fname = malloc(need);
                fname[0] = '_';
                memcpy(fname + 1, f.start, (size_t)f.length);
                fname[f.length + 1] = '\0';
            } else {
                fname = ident_name(p, f);
            }
            Node *member = nalloc(N_MEMBER, f.line);
            member->as.member.obj = n;
            member->as.member.name = fname;
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
