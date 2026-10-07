/* Recursive-descent parser with a layered arithmetic grammar
 * (assignment -> or -> and -> equality -> comparison -> term -> factor ->
 * unary -> postfix -> primary). Produces a malloc'd AST that lives for the
 * process lifetime (parsed once before agenthttpd_run forks workers).
 *
 * 2026-09-27: 语句解析(parse_statement)与表达式解析(parse_expression 的
 * 优先级链)分别拆到 parser_stmt.c / parser_expr.c;Parser 结构与共享
 * 工具见 parser_internal.h。 */

#include "parser_internal.h"

/* ---------- orphan journal ----------
 *
 * A node is created unowned and linked in later (the parent stores it and
 * hands the subtree up). Recursive descent returns NULL without a tree when a
 * statement fails, and every function on the way out drops the partial
 * subtree it built: `print("a" 1);` leaks the `print` reference and the call
 * node it had already linked together. So nalloc() records each node here,
 * and parse_program() — the only entry point — drops the batch when it gives
 * up. On success nothing is freed (every node is reachable from prog, which
 * the caller owns); on failure every node in the batch is garbage, because
 * the whole program is discarded.
 *
 * One parser runs at a time, so the list is a file global; parse_program()
 * empties it on both exits. */
static Node **g_orphans;
static size_t g_orphan_len;
static size_t g_orphan_cap;

static void orphan_add(Node *n) {
    if (g_orphan_len == g_orphan_cap) {
        size_t cap = g_orphan_cap ? g_orphan_cap * 2 : 64;
        Node **v = realloc(g_orphans, sizeof(Node *) * cap);
        if (!v) return;   /* OOM: stop journaling, parsing still works */
        g_orphans = v;
        g_orphan_cap = cap;
    }
    g_orphans[g_orphan_len++] = n;
}

static void orphan_remove(Node *n) {
    for (size_t i = 0; i < g_orphan_len; i++) {
        if (g_orphans[i] == n) {
            g_orphans[i] = g_orphans[--g_orphan_len];
            return;
        }
    }
}

/* Free every node the parser built during the parse that just failed. Order
 * does not matter (node_free_orphan() never walks into a child), and the
 * batch is exhausted either way: a failed parse discards the whole program. */
void node_unjournal(Node *n) { orphan_remove(n); }

static void orphan_release_all(void) {
    while (g_orphan_len > 0) node_free_orphan(g_orphans[--g_orphan_len]);
    g_orphan_len = 0;
}

/* Release the tracking list itself, keeping nothing. */
static void orphan_drop(void) {
    free(g_orphans);
    g_orphans = NULL;
    g_orphan_len = g_orphan_cap = 0;
}

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
    orphan_add(n);
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
        case TOK_IDENT: {
            advance(p);
            /* ident_name() allocates the name, type_struct() copies it into
             * the Type — so the throwaway has to be released here. Owning
             * both ends in one expression silently leaked a block per named
             * type parsed (`let p: Pt = ...`, `func f(): Pt`, ...). */
            char *nm = ident_name(p, t);
            ty = type_struct(nm);
            free(nm);
            break;
        }
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
                    /* names[arity] is already the block above (the name of
                     * the param whose annotation failed) — freeing the first
                     * `arity` names only would drop that one. */
                    for (int i = 0; i <= arity; i++) free(names[i]);
                    free(names);
                    free(types);
                    return false;
                }
            }
            arity++;
        } while (match(p, TOK_COMMA));
    }
    if (!expect(p, TOK_RPAREN)) {
        /* Same hole as above, minus the off-by-one: arity() has run past the
         * parameter just parsed, and names[] was sized for `arity + 1` slots
         * back when arity was still the previous count — so the live name is
         * names[arity - 1]. When arity is 0 names is NULL (the
         * !check(TOK_IDENT) branch above returned first). */
        if (names && arity > 0) free(names[arity - 1]);
        free(names);
        free(types);
        return false;
    }
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
        orphan_drop();
        return NULL;
    }

    Parser p;
    memset(&p, 0, sizeof(p));
    p.toks = toks;
    p.count = count;

    Node *prog = nalloc(N_PROGRAM, 1);
    orphan_remove(prog); /* this one belongs to this function, not the batch */
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
        /* Every node the parser allocated is in the orphan batch (nalloc
         * journals each one), and the statements parsed before the failure
         * are in prog -- the same nodes, by the same pointers. So the sweep
         * below is what frees them: releasing prog with node_free() here
         * walked the whole tree and freed every one of them a second time,
         * which aborted the process (SIGABRT, no diagnostic) on the *first
         * failing parse of any script that had already parsed one statement.
         * prog itself is not in the batch -- orphan_remove() took it out --
         * so only its own shell goes here. */
        free(prog->as.program.stmts);
        free(prog);
        free(toks);
        orphan_release_all();
        orphan_drop();
        return NULL;
    }
    free(toks);
    orphan_drop(); /* prog owns every node in the batch now */
    return prog;
}

/* ---------- teardown ---------- */

/* Free an array of malloc'd strings plus the array itself. */
static void free_name_array(char **a, int n) {
    if (!a) return;
    for (int i = 0; i < n; i++) free(a[i]);
    free(a);
}

/* Release an AST subtree and everything malloc'd inside it.
 *
 * Ownership, per field kind:
 *   - whatever ident_name() / parse_route_tail() / the `tool "..", ".."`
 *     and `import ".." as ns` parsers handed out (name, keys, field_names,
 *     route.alias/method/path, tool.name/desc, imp.path/ns) is a copy owned
 *     by this tree;
 *   - the char**, Node** and Type** array shells themselves go here too;
 *   - Type* fields (let.annot, func.ret, param_types[i], field_types[i],
 *     member.type) are NOT released: Type is a shared graph — AST, checker
 *     scope and the module export table all point at the same blocks — and
 *     one type_release_all() sweep owns them. Only the array shells are
 *     touched here, never the Type behind them;
 *   - lit.text is owned exactly when kind == LIT_STR (it is unset otherwise,
 *     and every reader is gated on kind);
 *
 * Call it when nothing walks the tree any more. ObjFunc.body keeps a Node*
 * into the tree, so free the VM heap before this if the interpreter might
 * still be holding a function value. */
/* Two passes, on purpose.
 *
 * Pass 1 hands the children back; pass 2 releases what this block owns
 * itself. Splitting them lets the parser sweep a batch of nodes it abandoned
 * mid-statement (see the orphan journal at the top of this file): the sweep
 * releases every node of the batch itself, so a child may already be gone and
 * walking into it from here would be a use-after-free. */
static void node_free_kids(Node *n);
static void node_free_own(Node *n);

void node_free_orphan(Node *n);

static void node_free_kids(Node *n) {
    if (!n) return;
    switch (n->type) {
    case N_PROGRAM:
    case N_BLOCK: {
        int c   = n->type == N_PROGRAM ? n->as.program.count : n->as.block.count;
        Node **s = n->type == N_PROGRAM ? n->as.program.stmts : n->as.block.stmts;
        for (int i = 0; i < c; i++) node_free(s[i]);
        break;
    }
    case N_LET:  node_free(n->as.let.init); break;
    case N_IF:
        node_free(n->as.ifs.cond);
        node_free(n->as.ifs.then);
        node_free(n->as.ifs.els);
        break;
    case N_WHILE:
        node_free(n->as.whiles.cond);
        node_free(n->as.whiles.body);
        break;
    case N_FOR:
        node_free(n->as.fors.init);
        node_free(n->as.fors.cond);
        node_free(n->as.fors.incr);
        node_free(n->as.fors.iterable);
        node_free(n->as.fors.body);
        break;
    case N_EXPR_STMT: node_free(n->as.expr_stmt.expr); break;
    case N_RETURN:    node_free(n->as.ret.expr); break;
    case N_SERVER:
        for (int i = 0; i < n->as.server.count; i++)
            node_free(n->as.server.assigns[i]);
        break;
    case N_ROUTE:  node_free(n->as.route.handler); break;
    case N_VERBS:  node_free(n->as.verbs.methods); break;
    case N_TOOL:
        node_free(n->as.tool.params);
        node_free(n->as.tool.handler);
        break;
    case N_FUNC_DECL: node_free(n->as.func.body); break;
    case N_FUNC_LIT:  node_free(n->as.funclit.body); break;
    case N_ASSIGN:    node_free(n->as.assign.value); break;
    case N_ASSIGN_MEMBER:
        node_free(n->as.assign_mem.obj);
        node_free(n->as.assign_mem.value);
        break;
    case N_MAP_LIT:
        for (int i = 0; i < n->as.map.count; i++) node_free(n->as.map.vals[i]);
        break;
    case N_LIST_LIT:
        for (int i = 0; i < n->as.list.count; i++)
            node_free(n->as.list.items[i]);
        break;
    case N_CALL:
        node_free(n->as.call.callee);
        for (int i = 0; i < n->as.call.argc; i++)
            node_free(n->as.call.args[i]);
        break;
    case N_MEMBER: node_free(n->as.member.obj); break;
    case N_UNARY:  node_free(n->as.unary.operand); break;
    case N_BINARY:
        node_free(n->as.binary.left);
        node_free(n->as.binary.right);
        break;
    default: break; /* no children (literals, imports, declarations, ...) */
    }
}

/* The strings and array shells named in the header comment above. Never
 * touches a child, so the sweep may call this in any order. */
static void node_free_own(Node *n) {
    switch (n->type) {
    case N_PROGRAM:
    case N_BLOCK: {
        /* the statements themselves are children (pass 1) */
        Node **s = n->type == N_PROGRAM ? n->as.program.stmts : n->as.block.stmts;
        free(s);
        break;
    }
    case N_LET:           free(n->as.let.name); break;
    case N_FOR:           free(n->as.fors.var); break;
    case N_ROUTE:
        free(n->as.route.method);
        free(n->as.route.alias);
        free(n->as.route.path);
        break;
    case N_VERBS:         free(n->as.verbs.name); break;
    case N_TOOL:
        free(n->as.tool.name);
        free(n->as.tool.desc);
        break;
    case N_FUNC_DECL:
        free(n->as.func.name);
        free_name_array(n->as.func.names, n->as.func.arity);
        free(n->as.func.param_types);  /* Type* elements: type_release_all() */
        break;
    case N_FUNC_LIT:
        free_name_array(n->as.funclit.names, n->as.funclit.arity);
        free(n->as.funclit.param_types);
        break;
    case N_TYPE_DECL:
        free(n->as.type_decl.name);
        free_name_array(n->as.type_decl.field_names, n->as.type_decl.count);
        free(n->as.type_decl.field_types);
        break;
    case N_VAR:               free(n->as.var.name); break;
    case N_ASSIGN:            free(n->as.assign.name); break;
    case N_ASSIGN_MEMBER:     free(n->as.assign_mem.name); break;
    case N_SERVER:            free(n->as.server.assigns); break;
    case N_MAP_LIT:
        free_name_array(n->as.map.keys, n->as.map.count);
        free(n->as.map.vals);
        break;
    case N_LIST_LIT:          free(n->as.list.items); break;
    case N_CALL:              free(n->as.call.args); break;
    case N_MEMBER:            free(n->as.member.name); break;
    case N_IMPORT:
        free(n->as.imp.path);
        free(n->as.imp.ns);
        break;
    case N_LITERAL:
        /* Only string literals own their text: parse_primary() mallocs a
         * quote-including copy for LIT_STR (and reallocs it while merging
         * adjacent literals). The other kinds keep a borrowed pointer into
         * the source buffer, so freeing those would be a double free. */
        /* The field owns this one (see the note on Node.lit in lume.h) — no
         * cast, unlike every other free() here. */
        if (n->as.lit.kind == LIT_STR) free(n->as.lit.text);
        break;
    case N_BREAK:
    case N_CONTINUE:
    case N_WHILE:
    case N_RETURN:
    case N_IF:
    case N_EXPR_STMT:
    case N_UNARY:
    case N_BINARY:
    default:
        break; /* no strings or shells of its own */
    }
}

void node_free(Node *n) {
    if (!n) return;
    node_free_kids(n);
    node_free_own(n);
    free(n);
}

/* Release one node the parser abandoned mid-statement: children are NOT
 * walked, the caller releases every node of the batch itself. */
void node_free_orphan(Node *n) {
    if (!n) return;
    node_free_own(n);
    free(n);
}

/* ---------- debug dump ---------- */

static void indent_print(int n) { for (int i = 0; i < n; i++) printf("  "); }

static void dump_lit(Node *n, int depth) {
    switch (n->as.lit.kind) {
        case LIT_NUM:
            indent_print(depth);
            /* Integers print as i64 (%lld), not %g: the double slot cannot
             * hold anything past 2^53, so dumping it there would misreport a
             * literal the compiler itself keeps exactly. */
            if (n->as.lit.is_float) printf("num %g\n", n->as.lit.num);
            else printf("num %lld\n", n->as.lit.inum);
            break;
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