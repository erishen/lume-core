/* Recursive-descent parser with a layered arithmetic grammar
 * (assignment -> or -> and -> equality -> comparison -> term -> factor ->
 * unary -> postfix -> primary). Produces a malloc'd AST that lives for the
 * process lifetime (parsed once before agenthttpd_run forks workers).
 *
 * 2026-09-27: 语句解析(parse_statement)与表达式解析(parse_expression 的
 * 优先级链)分别拆到 parser_stmt.c / parser_expr.c;Parser 结构与共享
 * 工具见 parser_internal.h。 */

#include "parser_internal.h"

void perror_at(Parser *p, size_t line, const char *fmt, ...) {
    if (p->errbuf[0]) return;
    va_list ap;
    va_start(ap, fmt);
    snprintf(p->errbuf, sizeof(p->errbuf), "line %zu: ", line);
    size_t used = strlen(p->errbuf);
    vsnprintf(p->errbuf + used, sizeof(p->errbuf) - used, fmt, ap);
    va_end(ap);
}

Node *nalloc(NodeType type, size_t line) {
    Node *n = calloc(1, sizeof(Node));
    n->type = type;
    n->line = line;
    return n;
}

Token peek(const Parser *p)        { return p->toks[p->pos]; }
Token peek2(const Parser *p)       { return p->toks[p->pos + 1]; }
size_t previous_line(const Parser *p) { return p->toks[p->pos - 1].line; }
bool at_end(const Parser *p)       { return peek(p).type == TOK_EOF; }
bool check(const Parser *p, TokenType t) { return peek(p).type == t; }

bool advance(Parser *p) {
    if (p->pos < p->count) p->pos++;
    return peek(p).type != TOK_EOF;
}

bool match(Parser *p, TokenType t) {
    if (check(p, t)) {
        advance(p);
        return true;
    }
    return false;
}

bool expect(Parser *p, TokenType t) {
    if (check(p, t)) {
        advance(p);
        return true;
    }
    Token got = peek(p);
    if (got.type == TOK_EOF) {
        perror_at(p, got.line, "expected %s, got end of input",
                  token_type_name(t));
    } else {
        /* 带上实际 token 原文:'expected }, got [ ('[')' 比只有类型名
         * 更容易定位(之前 'expected }, got [' 完全看不出是什么字符)。 */
        perror_at(p, got.line, "expected %s, got %s ('%.*s')",
                  token_type_name(t), token_type_name(got.type),
                  got.length > 0 ? got.length : 1,
                  got.start ? got.start : "");
    }
    return false;
}

char *ident_name(Parser *p, Token t) {
    (void)p;
    const char *start = t.start;
    int len = t.length;
    /* 字符串字面量作 map 键 / 成员名时剥掉首尾引号(键内转义序列原样保留,
     * 与运行时字符串求值口径不同;含转义的键极罕见,见 docs/PITFALLS.md) */
    if (t.type == TOK_STRING && len >= 2) {
        start++;
        len -= 2;
    }
    char *s = malloc((size_t)len + 1);
    memcpy(s, start, (size_t)len);
    s[len] = '\0';
    return s;
}

/* `type`/`int`/... are keywords in type position but stay usable as map keys
 * and member field names (e.g. a route response's `type` field). */
/* `get`/`post`/... are route-shorthand keywords at statement level, but stay
 * usable as identifiers/keys elsewhere (same rule as `type`/`int`) so the
 * `get(m, key)` builtin and `{ get: 1 }` maps keep working. */
bool is_method_keyword(TokenType t) {
    return t == TOK_GET || t == TOK_HEAD || t == TOK_POST || t == TOK_PUT ||
           t == TOK_PATCH || t == TOK_DELETE || t == TOK_OPTIONS;
}

const char *method_keyword_name(TokenType t) {
    switch (t) {
        case TOK_GET:     return "GET";
        case TOK_HEAD:    return "HEAD";
        case TOK_POST:    return "POST";
        case TOK_PUT:     return "PUT";
        case TOK_PATCH:   return "PATCH";
        case TOK_DELETE:  return "DELETE";
        case TOK_OPTIONS: return "OPTIONS";
        default:          return "";
    }
}

bool is_field_token(TokenType t) {
    return t == TOK_IDENT || t == TOK_TYPE || t == TOK_INT || t == TOK_FLOAT ||
           t == TOK_KW_STRING || t == TOK_BOOL || t == TOK_RESULT ||
           t == TOK_STRING || /* 字符串字面量 map 键 {"a": 1} / 成员名 (2026-09-27) */
           is_method_keyword(t);
}

/* ---------- types ---------- */

/* type := ('int' | 'float' | 'string' | 'bool' | 'Result' | IDENT) '[' ']'* */
Type *parse_type(Parser *p) {
    Token t = peek(p);
    Type *ty = NULL;
    switch (t.type) {
        case TOK_INT:    advance(p); ty = type_prim(TY_INT); break;
        case TOK_FLOAT:  advance(p); ty = type_prim(TY_FLOAT); break;
        case TOK_KW_STRING: advance(p); ty = type_prim(TY_STRING); break;
        case TOK_BOOL:   advance(p); ty = type_prim(TY_BOOL); break;
        case TOK_RESULT: advance(p); ty = type_result(); break;
        case TOK_IDENT:  advance(p); ty = type_struct(ident_name(p, t)); break;
        default:
            perror_at(p, t.line, "expected a type, got %s",
                      token_type_name(t.type));
            return NULL;
    }
    while (match(p, TOK_LBRACKET)) {
        if (!expect(p, TOK_RBRACKET)) return NULL;
        ty = type_list(ty);
    }
    return ty;
}

/* ---------- blocks & statements ---------- */

Node *parse_block(Parser *p) {
    size_t line = peek(p).line;
    if (!expect(p, TOK_LBRACE)) return NULL;
    Node *n = nalloc(N_BLOCK, line);
    n->as.block.stmts = NULL;
    n->as.block.count = 0;
    while (!check(p, TOK_RBRACE) && !at_end(p)) {
        Node *s = parse_statement(p);
        if (!s) return NULL;
        n->as.block.stmts = realloc(n->as.block.stmts,
                                    sizeof(Node *) * ((size_t)n->as.block.count + 1));
        n->as.block.stmts[n->as.block.count++] = s;
    }
    if (!expect(p, TOK_RBRACE)) return NULL;
    return n;
}

/* func ( a, b ) — params consumed, names + types + arity returned.
 * A param type (nullable Type*) is NULL when unannotated. */
bool parse_params(Parser *p, char ***names_out, Type ***types_out,
                         int *arity_out) {
    char **names = NULL;
    Type **types = NULL;
    int arity = 0;
    if (!expect(p, TOK_LPAREN)) return false;
    if (!check(p, TOK_RPAREN)) {
        do {
            if (!check(p, TOK_IDENT)) {
                perror_at(p, peek(p).line, "expected parameter name", NULL);
                free(names);
                free(types);
                return false;
            }
            Token pt = peek(p);
            advance(p);
            names = realloc(names, sizeof(char *) * ((size_t)arity + 1));
            types = realloc(types, sizeof(Type *) * ((size_t)arity + 1));
            names[arity] = ident_name(p, pt);
            types[arity] = NULL;
            if (match(p, TOK_COLON)) {
                types[arity] = parse_type(p);
                if (!types[arity]) {
                    for (int i = 0; i < arity; i++) free(names[i]);
                    free(names);
                    free(types);
                    return false;
                }
            }
            arity++;
        } while (match(p, TOK_COMMA));
    }
    if (!expect(p, TOK_RPAREN)) { free(names); free(types); return false; }
    *names_out = names;
    *types_out = types;
    *arity_out = arity;
    return true;
}

/* optional ':' return-type annotation after params (). */
Type *parse_optional_ret(Parser *p) {
    Type *ret = NULL;
    if (match(p, TOK_COLON)) {
        ret = parse_type(p);
        if (!ret) return NULL;
    }
    return ret;
}

Node *parse_func_literal(Parser *p, size_t kw_line) {
    /* TOK_FUNC already consumed. */
    Node *n = nalloc(N_FUNC_LIT, kw_line);
    if (!parse_params(p, &n->as.funclit.names, &n->as.funclit.param_types,
                      &n->as.funclit.arity)) return NULL;
    n->as.funclit.ret = parse_optional_ret(p);
    if (peek(p).type == TOK_EOF && !n->as.funclit.ret) {
        perror_at(p, kw_line, "expected function body or return type", NULL);
        return NULL;
    }
    n->as.funclit.body = parse_block(p);
    if (!n->as.funclit.body) return NULL;
    return n;
}

/* `"path", handler;` — shared tail of the `route` statement and the
 * `get`/`post`/.../verbs-alias shorthand forms. The caller must have set
 * exactly one of n->as.route.method / n->as.route.alias. */
Node *parse_route_tail(Parser *p, Node *n) {
    if (!check(p, TOK_STRING)) {
        perror_at(p, peek(p).line, "expected path string in route", NULL);
        return NULL;
    }
    Token path = peek(p);
    advance(p);
    n->as.route.path = malloc((size_t)(path.length - 2) + 1);
    memcpy(n->as.route.path, path.start + 1, (size_t)(path.length - 2));
    n->as.route.path[path.length - 2] = '\0';
    /* The handler is optional: `write "/items";` registers the routes with a
     * default response (action/method/body). */
    if (match(p, TOK_COMMA)) {
        n->as.route.handler = parse_expression(p);
        if (!n->as.route.handler) return NULL;
    }
    if (!expect(p, TOK_SEMI)) return NULL;
    return n;
}

/* ---------- program ---------- */

Node *parse_program(const char *source, char *errbuf, size_t errbuf_size) {
    char lexerr[256] = {0};
    int count = 0;
    Token *toks = al_lex(source, lexerr, sizeof(lexerr), &count);
    if (!toks) {
        if (errbuf && errbuf_size) snprintf(errbuf, errbuf_size, "%s", lexerr);
        return NULL;
    }

    Parser p;
    memset(&p, 0, sizeof(p));
    p.toks = toks;
    p.count = count;

    Node *prog = nalloc(N_PROGRAM, 1);
    prog->as.program.stmts = NULL;
    prog->as.program.count = 0;

    bool ok = true;
    while (!at_end(&p) && ok) {
        if (peek(&p).type == TOK_SEMI) {
            advance(&p); /* tolerate stray semicolons */
            continue;
        }
        Node *s = parse_statement(&p);
        if (!s) {
            ok = false;
            break;
        }
        prog->as.program.stmts = realloc(prog->as.program.stmts,
                                         sizeof(Node *) * ((size_t)prog->as.program.count + 1));
        prog->as.program.stmts[prog->as.program.count++] = s;
    }

    if (!ok) {
        if (errbuf && errbuf_size)
            snprintf(errbuf, errbuf_size, "%s%s", p.errbuf,
                     p.errbuf[0] ? "" : "parse error");
        free(toks);
        return NULL;
    }
    free(toks);
    return prog;
}

/* ---------- debug dump ---------- */

static void indent_print(int n) { for (int i = 0; i < n; i++) printf("  "); }

static void dump_lit(Node *n, int depth) {
    switch (n->as.lit.kind) {
        case LIT_NUM: indent_print(depth); printf("num %g\n", n->as.lit.num); break;
        case LIT_STR:
            indent_print(depth);
            printf("str \"%.*s\"\n", n->as.lit.len, n->as.lit.text);
            break;
        case LIT_TRUE:  indent_print(depth); printf("true\n"); break;
        case LIT_FALSE: indent_print(depth); printf("false\n"); break;
        case LIT_NULL:  indent_print(depth); printf("null\n"); break;
    }
}

void node_print(Node *n, int depth) {
    if (!n) { indent_print(depth); printf("<null>\n"); return; }
    switch (n->type) {
        case N_PROGRAM:
            indent_print(depth); printf("program\n");
            for (int i = 0; i < n->as.program.count; i++)
                node_print(n->as.program.stmts[i], depth + 1);
            break;
        case N_BLOCK:
            indent_print(depth); printf("block\n");
            for (int i = 0; i < n->as.block.count; i++)
                node_print(n->as.block.stmts[i], depth + 1);
            break;
        case N_EXPR_STMT:
            indent_print(depth); printf("expr-stmt\n");
            node_print(n->as.expr_stmt.expr, depth + 1);
            break;
        case N_LITERAL:
            dump_lit(n, depth);
            break;
        case N_LET:
            indent_print(depth); printf("let %s\n", n->as.let.name);
            node_print(n->as.let.init, depth + 1);
            break;
        case N_ROUTE:
            indent_print(depth);
            if (n->as.route.alias)
                printf("route (verbs %s) %s\n", n->as.route.alias, n->as.route.path);
            else
                printf("route %s %s\n", n->as.route.method, n->as.route.path);
            if (n->as.route.handler)
                node_print(n->as.route.handler, depth + 1);
            else {
                indent_print(depth + 1);
                printf("(default handler)\n");
            }
            break;
        case N_VERBS:
            indent_print(depth); printf("verbs %s =\n", n->as.verbs.name);
            node_print(n->as.verbs.methods, depth + 1);
            break;
        case N_TYPE_DECL:
            indent_print(depth); printf("type %s\n", n->as.type_decl.name);
            for (int i = 0; i < n->as.type_decl.count; i++) {
                indent_print(depth + 1);
                printf("%s: ", n->as.type_decl.field_names[i]);
                type_print(n->as.type_decl.field_types[i]);
                printf("\n");
            }
            break;
        case N_FUNC_DECL:
            indent_print(depth);
            printf("func %s(", n->as.func.name);
            for (int i = 0; i < n->as.func.arity; i++) {
                if (i) printf(", ");
                printf("%s", n->as.func.names[i]);
                if (n->as.func.param_types[i]) {
                    printf(": ");
                    type_print(n->as.func.param_types[i]);
                }
            }
            printf(")");
            if (n->as.func.ret) { printf(": "); type_print(n->as.func.ret); }
            printf("\n");
            node_print(n->as.func.body, depth + 1);
            break;
        case N_IMPORT:
            indent_print(depth);
            printf("import \"%.*s\" as %s%s\n",
                   (int)strlen(n->as.imp.path), n->as.imp.path,
                   n->as.imp.ns, n->is_export ? " (export)" : "");
            break;
        default:
            indent_print(depth); printf("node(type=%d)\n", (int)n->type);
            break;
    }
}