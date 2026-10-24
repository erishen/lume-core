/* Recursive-descent parser with a layered arithmetic grammar
 * (assignment -> or -> and -> equality -> comparison -> term -> factor ->
 * unary -> postfix -> primary). Produces an arena-allocated AST (TODO.md
 * P2, docs/ARENA.md): nothing the parser builds is ever freed
 * individually — the memory dies wholesale in arena_reset() at the points
 * where the tree dies today.
 *
 * 2026-09-27: 语句解析(parse_statement)与表达式解析(parse_expression 的
 * 优先级链)分别拆到 parser_stmt.c / parser_expr.c;Parser 结构与共享
 * 工具见 parser_internal.h。 */

#include "arena.h"
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
    Node *n = arena_alloc(sizeof(Node));
    /* calloc semantics preserved: every node starts zeroed — plenty of
     * union arms (lit.len, funclit.cap_*, route alias, ...) are only set
     * conditionally and read unconditionally. */
    memset(n, 0, sizeof(Node));
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
    return arena_strndup(start, (size_t)len);
}

char *dupcstr(const char *s) {
    return arena_strdup(s);
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
        case TOK_KW_FN: {
            /* `fn (t1, t2) -> ret` — a closure-variable type annotation
             * (SPEC §4.8). `let f: fn(int) -> int = (x) => x * 2;` — the
             * annotation spells the lambda's param/ret types so the checker
             * can verify the literal against them and native backends have a
             * spelled type even before inference pins one. */
            advance(p);
            if (!expect(p, TOK_LPAREN)) return NULL;
            Type **params = NULL;
            int arity = 0;
            if (!check(p, TOK_RPAREN)) {
                do {
                    Type *pt = parse_type(p);
                    if (!pt) return NULL;
                    params = arena_grow(params, sizeof(Type *) * (size_t)arity,
                                        sizeof(Type *) * ((size_t)arity + 1));
                    params[arity++] = pt;
                } while (match(p, TOK_COMMA));
            }
            if (!expect(p, TOK_RPAREN)) return NULL;
            Type *ret = NULL;
            if (match(p, TOK_ARROW)) {
                ret = parse_type(p);
                if (!ret) return NULL;
            }
            ty = type_func(arity, params, ret);
            break;
        }
        case TOK_IDENT: {
            advance(p);
            /* ident_name() allocates the name (arena-owned now), type_struct()
             * copies it into the Type — the throwaway needs no release under
             * the arena, where "leaked a block per named type parsed" was the
             * reason this used to be split into two owning ends.
             *
             * The bare built-in container name `list` is NOT a struct: it is
             * the homogeneous container (TY_LIST). Building `type_struct("list")`
             * here made `let x: list = ...` annotate as a *named struct called
             * "list"* with no definition, so the native emitters (which read
             * llvm_type_of/ty_of off the annotation) reported "no codegen type"
             * while the interpreter — which uses the checker-resolved type —
             * tolerated it. Redirect the keyword to TY_LIST so the annotation
             * matches the way list literals are already typed. (`map` stays a
             * struct reference: a map is an anonymous struct at runtime and
             * there is no singleton TY_MAP representation to annotate.) */
            char *nm = ident_name(p, t);
            if (strcmp(nm, "list") == 0) { ty = type_list(any_type()); break; }
            /* `map` annotates the anonymous-struct shape a map literal and
             * every map-returning builtin actually have. Without this the
             * annotation became a phantom named struct ("map" with no
             * definition), so `let m: map = {...}` bound through the named-
             * struct aggregate path on the native backends and stored the
             * map's length instead of the pointer — `print(m)` answered 2 for
             * a two-key map and get(m, k) read null. Redirect like `list`. */
            if (strcmp(nm, "map") == 0) { ty = type_anon_struct(); break; }
            ty = type_struct(nm);
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

static Node *parse_block_impl(Parser *p);

Node *parse_block(Parser *p) {
    if (++p->depth > PARSER_MAX_DEPTH) { p->depth--; perror_at(p, peek(p).line,
                              "expression nests too deeply", NULL); return NULL; }
    Node *n = parse_block_impl(p);
    p->depth--;
    return n;
}

static Node *parse_block_impl(Parser *p) {
    size_t line = peek(p).line;
    if (!expect(p, TOK_LBRACE)) return NULL;
    Node *n = nalloc(N_BLOCK, line);
    n->as.block.stmts = NULL;
    n->as.block.count = 0;
    while (!check(p, TOK_RBRACE) && !at_end(p)) {
        Node *s = parse_statement(p);
        if (!s) return NULL;
        n->as.block.stmts = arena_grow(n->as.block.stmts,
                                       sizeof(Node *) * (size_t)n->as.block.count,
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
                /* Reached by the comma continuation with arity ≥ 1 live
                 * names (`(http_,` hits it with "http_" pushed): the
                 * libFuzzer leak-3188fd7 finding. Under the arena the
                 * partial arrays are simply abandoned. */
                perror_at(p, peek(p).line, "expected parameter name", NULL);
                return false;
            }
            Token pt = peek(p);
            advance(p);
            names = arena_grow(names, sizeof(char *) * (size_t)arity,
                               sizeof(char *) * ((size_t)arity + 1));
            types = arena_grow(types, sizeof(Type *) * (size_t)arity,
                               sizeof(Type *) * ((size_t)arity + 1));
            names[arity] = ident_name(p, pt);
            types[arity] = NULL;
            if (match(p, TOK_COLON)) {
                types[arity] = parse_type(p);
                if (!types[arity]) {
                    /* names[arity] is already the block above (the name of
                     * the param whose annotation failed). */
                    return false;
                }
            }
            arity++;
        } while (match(p, TOK_COMMA));
    }
    if (!expect(p, TOK_RPAREN)) {
        /* All arity pushed names are live (the old comment's off-by-one
         * reasoning only held for arity == 1; `(a, b` leaked "a" through
         * it). Under the arena the partial arrays are abandoned. */
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
    n->as.route.path = arena_strndup(path.start + 1, (size_t)(path.length - 2));
    /* The handler is optional: `write "/items";` registers the routes with a
     * default response (action/method/body). */
    if (match(p, TOK_COMMA)) {
        n->as.route.handler = parse_expression(p);
        if (!n->as.route.handler) return NULL;
    }
    if (!expect(p, TOK_SEMI)) return NULL;
    return n;
}

/* ---------- pattern lists (match arms and if-let share this) ---------- */

/* `Variant(b, ...)` / bare `Variant` / `_`, `|`-separated into alternatives.
 * Shared by match arms (`A | B => body`) and if-let (`if let A | B = e`).
 * The caller owns the returned array; vnames and binds are arena strings,
 * the resolve_ctors stamps start NULL (parser.c's post-parse pass fills
 * them in once the file's enum table is known). Guards are NOT parsed
 * here — the match-arm caller adds `if cond` after the list. */
bool parse_patterns(Parser *p, MatchPat **out, int *outc) {
    MatchPat *pats = NULL;
    int patc = 0;
    for (;;) {
        Token pt = peek(p);
        if (pt.type != TOK_IDENT) {
            perror_at(p, pt.line, "expected variant name in pattern", NULL);
            return false;
        }
        advance(p);
        pats = arena_grow(pats, sizeof(MatchPat) * (size_t)patc,
                          sizeof(MatchPat) * ((size_t)patc + 1));
        MatchPat *pat = &pats[patc];
        memset(pat, 0, sizeof *pat);
        pat->vname = ident_name(p, pt);
        if (match(p, TOK_LPAREN)) {
            /* positional binds — plain identifiers separated by commas */
            do {
                Token bt = peek(p);
                if (bt.type != TOK_IDENT) {
                    perror_at(p, bt.line,
                              "expected a binding name in pattern", NULL);
                    return false;
                }
                advance(p);
                pat->binds = arena_grow(pat->binds,
                        sizeof(char *) * (size_t)pat->bindc,
                        sizeof(char *) * ((size_t)pat->bindc + 1));
                pat->binds[pat->bindc++] = ident_name(p, bt);
            } while (match(p, TOK_COMMA));
            if (!expect(p, TOK_RPAREN)) return false;
        }
        patc++;
        if (!match(p, TOK_PIPE)) break;
    }
    *out = pats;
    *outc = patc;
    return true;
}

/* ---------- program ---------- */

static void resolve_ctors(Node *prog);

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
    p.toks = toks;   /* depth rides the memset-zero */
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
        prog->as.program.stmts = arena_grow(prog->as.program.stmts,
                                            sizeof(Node *) * (size_t)prog->as.program.count,
                                            sizeof(Node *) * ((size_t)prog->as.program.count + 1));
        prog->as.program.stmts[prog->as.program.count++] = s;
    }

    if (!ok) {
        if (errbuf && errbuf_size)
            snprintf(errbuf, errbuf_size, "%s%s", p.errbuf,
                     p.errbuf[0] ? "" : "parse error");
        /* The partial tree is arena memory: abandoned, not swept. The old
         * orphan-batch release here was what kept failed parses from
         * leaking — the arena reset at the tree's death (the same call
         * sites that ran node_free) reclaims it wholesale now. */
        for (int i = 0; i < count; i++) free(toks[i].istr);
        free(toks);
        return NULL;
    }
    for (int i = 0; i < count; i++) free(toks[i].istr);
    free(toks);
    resolve_ctors(prog);
    return prog;
}

/* ---------- post-parse constructor resolution ---------- */

/* A variant name is a constructor everywhere in its file once its enum is
 * declared (Lean's namespace rule, minus the qualification). The parser
 * cannot know that while it parses — the enum may sit after the use site —
 * so after the program is built this pass runs once:
 *
 *   pass 1 collects every N_ENUM_DECL's variant names (top-level only, so
 *   a flat scan of the program suffices),
 *   pass 2 rewrites
 *     - `V(args...)` — an N_CALL whose callee is the bare name V — into a
 *       proper N_CTOR (arguments moved over, the callee shell freed), and
 *     - a bare `V` (N_VAR, zero-payload variant) in place, by flipping the
 *       node's type and keeping the name.
 *
 * `V()?` (propagate suffix on a constructor) is left as a call on purpose:
 * N_CTOR carries no propagate flag, and a `Result`-valued ctor is not a
 * thing, so the checker reports it as a call of a non-function instead of
 * inventing an error message for a shape nobody should write.
 *
 * Shadowing is prevented, not resolved: typecheck.c rejects a let / param /
 * function whose name collides with a constructor, so this rewrite can
 * never steal a live binding. */

typedef struct {
    char *name;     /* variant name */
    char *ename;    /* owning enum's name */
    char **fields;  /* payload field names (borrowed from the enum decl) */
    Type **ftypes;  /* payload field types — parser-built, and for the four
                     * allowed scalars the shell *is* the resolved def, so
                     * binding allocas can use it without the checker */
    int fieldc;
} CtorEnt;

typedef struct { CtorEnt *v; int count; } CtorTab;

static CtorEnt *ctor_find(CtorTab *tab, const char *name) {
    for (int i = 0; i < tab->count; i++)
        if (strcmp(tab->v[i].name, name) == 0) return &tab->v[i];
    return NULL;
}

static void ctor_rewrite(Node *n, CtorTab *tab);

/* Stamp one pattern with its variant's payload field names/types (borrowed
 * from the enum decl). A "_" pattern and an unknown name stay unstamped —
 * the checker reports the unknown name, and "_" carries no fields. */
static void stamp_pat(MatchPat *pat, CtorTab *tab) {
    CtorEnt *ce = strcmp(pat->vname, "_") == 0
                    ? NULL : ctor_find(tab, pat->vname);
    pat->vfields = ce ? ce->fields : NULL;
    pat->vfieldc = ce ? ce->fieldc : 0;
    pat->vfield_types = ce ? ce->ftypes : NULL;
}

static void ctor_rewrite(Node *n, CtorTab *tab) {
    if (!n) return;
    switch (n->type) {
    case N_PROGRAM:
    case N_BLOCK:
        for (int i = 0; i < n->as.program.count; i++)
            ctor_rewrite(n->as.program.stmts[i], tab);
        return; /* count/stmts are the same union member for both kinds */
    case N_ENUM_DECL: return; /* declarations carry no expressions */
    case N_LET:        ctor_rewrite(n->as.let.init, tab); return;
    case N_LET_PAT:    ctor_rewrite(n->as.letpat.init, tab); return;
    case N_IF:
        ctor_rewrite(n->as.ifs.cond, tab);
        ctor_rewrite(n->as.ifs.then, tab);
        ctor_rewrite(n->as.ifs.els, tab);
        return;
    case N_WHILE:
        ctor_rewrite(n->as.whiles.cond, tab);
        ctor_rewrite(n->as.whiles.body, tab);
        return;
    case N_FOR:
        ctor_rewrite(n->as.fors.init, tab);
        ctor_rewrite(n->as.fors.cond, tab);
        ctor_rewrite(n->as.fors.incr, tab);
        ctor_rewrite(n->as.fors.iterable, tab);
        ctor_rewrite(n->as.fors.body, tab);
        return;
    case N_RETURN:    ctor_rewrite(n->as.ret.expr, tab); return;
    case N_EXPR_STMT: ctor_rewrite(n->as.expr_stmt.expr, tab); return;
    case N_SERVER:
        for (int i = 0; i < n->as.server.count; i++)
            ctor_rewrite(n->as.server.assigns[i], tab);
        return;
    case N_ROUTE: ctor_rewrite(n->as.route.handler, tab); return;
    case N_TOOL:
        ctor_rewrite(n->as.tool.params, tab);
        ctor_rewrite(n->as.tool.handler, tab);
        return;
    case N_VERBS:   ctor_rewrite(n->as.verbs.methods, tab); return;
    case N_FUNC_DECL: ctor_rewrite(n->as.func.body, tab); return;
    case N_FUNC_LIT:  ctor_rewrite(n->as.funclit.body, tab); return;
    case N_ASSIGN:    ctor_rewrite(n->as.assign.value, tab); return;
    case N_ASSIGN_MEMBER:
        ctor_rewrite(n->as.assign_mem.obj, tab);
        ctor_rewrite(n->as.assign_mem.value, tab);
        return;
    case N_MAP_LIT:
        for (int i = 0; i < n->as.map.count; i++)
            ctor_rewrite(n->as.map.vals[i], tab);
        return;
    case N_LIST_LIT:
        for (int i = 0; i < n->as.list.count; i++)
            ctor_rewrite(n->as.list.items[i], tab);
        return;
    case N_MEMBER: ctor_rewrite(n->as.member.obj, tab); return;
    case N_INDEX:
        ctor_rewrite(n->as.index.obj, tab);
        ctor_rewrite(n->as.index.index, tab);
        return;
    case N_UNARY:  ctor_rewrite(n->as.unary.operand, tab); return;
    case N_BINARY:
        ctor_rewrite(n->as.binary.left, tab);
        ctor_rewrite(n->as.binary.right, tab);
        return;
    case N_TERNARY:
        ctor_rewrite(n->as.ternary.cond, tab);
        ctor_rewrite(n->as.ternary.then, tab);
        ctor_rewrite(n->as.ternary.els, tab);
        return;
    case N_MATCH:
        ctor_rewrite(n->as.matchn.scrutinee, tab);
        for (int i = 0; i < n->as.matchn.armc; i++) {
            MatchArm *arm = &n->as.matchn.arms[i];
            for (int k = 0; k < arm->patc; k++) stamp_pat(&arm->pats[k], tab);
            ctor_rewrite(arm->guard, tab);
            ctor_rewrite(arm->body, tab);
        }
        return;
    case N_IF_LET:
        /* patterns are variant-name strings, not expressions — stamp them
         * like match arms do, then walk the three expression slots */
        for (int k = 0; k < n->as.iflet.patc; k++)
            stamp_pat(&n->as.iflet.pats[k], tab);
        ctor_rewrite(n->as.iflet.value, tab);
        ctor_rewrite(n->as.iflet.then, tab);
        ctor_rewrite(n->as.iflet.els, tab);
        return;
    case N_CTOR:
        for (int i = 0; i < n->as.ctor.argc; i++)
            ctor_rewrite(n->as.ctor.args[i], tab);
        return;    case N_CALL:
        /* Rewrite BEFORE walking the callee: `Circle(1.0)` must become one
         * N_CTOR carrying its arguments, not a call whose bare-name callee
         * already got flipped into a zero-arg N_CTOR by the N_VAR case. */
        if (!n->as.call.propagate && n->as.call.callee &&
            n->as.call.callee->type == N_VAR) {
            CtorEnt *ce = ctor_find(tab, n->as.call.callee->as.var.name);
            if (ce) {
                Node *callee = n->as.call.callee;
                n->type = N_CTOR;
                n->as.ctor.name = callee->as.var.name; /* name moved over */
                n->as.ctor.args = n->as.call.args;
                n->as.ctor.argc = n->as.call.argc;
                n->as.ctor.fields = ce->fields;
                n->as.ctor.fieldc = ce->fieldc;
                n->as.ctor.ename = ce->ename;
                /* the callee shell stays — arena memory is never freed
                 * individually, and its name/args moved over above */
                for (int i = 0; i < n->as.ctor.argc; i++)
                    ctor_rewrite(n->as.ctor.args[i], tab);
                return;
            }
        }
        ctor_rewrite(n->as.call.callee, tab);
        for (int i = 0; i < n->as.call.argc; i++)
            ctor_rewrite(n->as.call.args[i], tab);
        return;
    case N_VAR:
        /* A bare zero-payload variant: flip the node in place. The var
         * union's name field is the ctor union's first field, so the name
         * pointer carries over untouched. */
        {
            CtorEnt *ce = ctor_find(tab, n->as.var.name);
            if (ce) {
                n->type = N_CTOR;
                n->as.ctor.args = NULL;
                n->as.ctor.argc = 0;
                n->as.ctor.fields = ce->fields;
                n->as.ctor.fieldc = ce->fieldc;
                n->as.ctor.ename = ce->ename;
            }
        }
        return;
    default:
        return; /* literals, imports, route/verb names — no expressions */
    }
}

static void resolve_ctors(Node *prog) {
    if (!prog || prog->type != N_PROGRAM) return;
    CtorTab tab = { NULL, 0 };
    for (int i = 0; i < prog->as.program.count; i++) {
        Node *s = prog->as.program.stmts[i];
        if (s->type != N_ENUM_DECL) continue;
        for (int j = 0; j < s->as.enum_decl.vcount; j++) {
            tab.v = realloc(tab.v, sizeof(CtorEnt) * ((size_t)tab.count + 1));
            CtorEnt *ce = &tab.v[tab.count++];
            ce->name = s->as.enum_decl.vnames[j];
            ce->ename = s->as.enum_decl.name;
            ce->fields = s->as.enum_decl.vfields[j];
            ce->ftypes = s->as.enum_decl.vfield_types[j];
            ce->fieldc = s->as.enum_decl.vfield_counts[j];
        }
    }
    if (tab.count) ctor_rewrite(prog, &tab);
    /* The table itself is scratch — names/fields are borrowed from the enum
     * decls, which the AST owns and frees. */
    free(tab.v);
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
        case N_LET_PAT:
            indent_print(depth);
            printf("let-pattern {");
            for (int i = 0; i < n->as.letpat.count; i++) {
                const char *f = n->as.letpat.fields[i];
                const char *nm = n->as.letpat.names[i];
                printf("%s%s", i ? ", " : "", f);
                if (strcmp(f, nm) != 0) printf(": %s", nm);
            }
            printf("}\n");
            node_print(n->as.letpat.init, depth + 1);
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