#include "lume.h"

/* Lexer: source (NUL-terminated) -> Token array.
 * Errors are reported by emitting a single TOK_ERROR token followed by
 * TOK_EOF; the parser surfaces the message. Comments (`//` and block
 * comments) and whitespace are skipped. Numbers accept ints and decimals;
 * strings support the common \\n \\t \\r \\\\ \\" \\0 escapes. */

typedef struct {
    const char *src;
    size_t pos;
    size_t line;
    char err[256];
} Lexer;

typedef struct {
    Token *toks;
    int count;
    int cap;
    Lexer lx;
} LexOut;

static void lx_error(Lexer *lx, const char *msg) {
    snprintf(lx->err, sizeof(lx->err), "line %zu: %s", lx->line, msg);
}

static void emit(LexOut *o, TokenType t, const char *start, int len) {
    if (o->count >= o->cap) {
        o->cap = o->cap ? o->cap * 2 : 64;
        o->toks = realloc(o->toks, sizeof(Token) * (size_t)o->cap);
        /* realloc failure: keep static size, but drop the program. */
    }
    Token tok;
    tok.type = t;
    tok.start = start;
    tok.length = len;
    tok.line = o->lx.line;
    tok.num = 0;
    o->toks[o->count++] = tok;
}

static void emit_number(LexOut *o, const char *start, int len) {
    emit(o, TOK_NUMBER, start, len);
    if (len == 0) return;
    char buf[128];
    if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
    memcpy(buf, start, (size_t)len);
    buf[len] = '\0';
    double d = strtod(buf, NULL);
    o->toks[o->count - 1].num = d;
}

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool is_ident_char(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static const struct {
    const char *word;
    TokenType type;
} KEYWORDS[] = {
    {"server", TOK_SERVER}, {"route",  TOK_ROUTE}, {"tool",    TOK_TOOL},
    {"func",   TOK_FUNC},   {"return", TOK_RETURN}, {"if",     TOK_IF},
    {"else",   TOK_ELSE},   {"while",  TOK_WHILE}, {"for",    TOK_FOR},
    {"in",     TOK_IN},     {"break",  TOK_BREAK}, {"continue", TOK_CONTINUE},
    {"let",    TOK_LET},
    {"import", TOK_IMPORT}, {"export", TOK_EXPORT}, {"as",     TOK_AS},
    {"true",   TOK_TRUE},   {"false",  TOK_FALSE}, {"null",   TOK_NULL},
    {"and",    TOK_AND},    {"or",     TOK_OR},    {"not",    TOK_NOT},
    {"type",   TOK_TYPE},   {"int",    TOK_INT},   {"float",  TOK_FLOAT},
    {"string", TOK_KW_STRING}, {"bool",   TOK_BOOL},  {"Result", TOK_RESULT},
    {"get", TOK_GET}, {"head", TOK_HEAD}, {"post", TOK_POST},
    {"put", TOK_PUT}, {"patch", TOK_PATCH}, {"delete", TOK_DELETE},
    {"options", TOK_OPTIONS},
    {"verbs", TOK_VERBS},
};

static TokenType keyword_type(const char *start, int len) {
    for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++) {
        if ((int)strlen(KEYWORDS[i].word) == len &&
            strncmp(start, KEYWORDS[i].word, (size_t)len) == 0)
            return KEYWORDS[i].type;
    }
    return TOK_IDENT;
}

static void lex_string(LexOut *o, const char *str_start) {
    Lexer *lx = &o->lx;
    const char *p = lx->src + lx->pos + 1; /* past the opening quote */
    /* Validate the body: find the closing quote, honoring escapes. */
    while (*p && *p != '"') {
        if (*p == '\n') {
            lx_error(lx, "unterminated string literal");
            emit(o, TOK_ERROR, str_start, 0);
            lx->pos += 1; /* past the opening quote: without this the lexer
                              re-processes the same '"' forever (infinite
                              loop) instead of surfacing the error */
            return;
        }
        if (*p == '\\') {
            p++;
            if (!*p) break;
        }
        p++;
    }
    if (*p != '"') {
        lx_error(lx, "unterminated string literal");
        emit(o, TOK_ERROR, str_start, 0);
        lx->pos += 1; /* same as above: escape the infinite loop */
        return;
    }
    p++; /* closing quote */
    lx->pos = (size_t)(p - lx->src);
    emit(o, TOK_STRING, str_start, (int)(p - str_start));
}

static void lex_comment(LexOut *o, const char *start) {
    Lexer *lx = &o->lx;
    const char *p = lx->src + lx->pos;
    /* Already consumed "//" or "/" "*". */
    if (start[1] == '/') {
        while (*p && *p != '\n') {
            p++;
        }
    } else {
        while (*p && !(*p == '*' && *(p + 1) == '/')) {
            if (*p == '\n') lx->line++;
            p++;
        }
        if (*p) p += 2;
    }
    lx->pos = (size_t)(p - lx->src);
}

Token *lex_all(const char *source, char *errbuf, size_t errbuf_size,
               int *out_count) {
    LexOut o;
    memset(&o, 0, sizeof(o));
    o.lx.src = source;
    o.lx.line = 1;

    while (true) {
        char c = source[o.lx.pos];
        if (c == '\0') {
            emit(&o, TOK_EOF, source + o.lx.pos, 0);
            break;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            o.lx.pos++;
            continue;
        }
        if (c == '\n') {
            o.lx.line++;
            o.lx.pos++;
            continue;
        }
        if (c == '/' && (source[o.lx.pos + 1] == '/' ||
                         source[o.lx.pos + 1] == '*')) {
            lex_comment(&o, source + o.lx.pos);
            continue;
        }
        const char *tok_start = source + o.lx.pos;
        size_t save_pos = o.lx.pos;

        if (is_digit(c)) {
            const char *p = tok_start;
            while (is_digit(*p)) p++;
            if (*p == '.') {
                p++;
                while (is_digit(*p)) p++;
            }
            o.lx.pos = (size_t)(p - source);
            emit_number(&o, tok_start, (int)(p - tok_start));
            continue;
        }
        if (is_ident_start(c)) {
            const char *p = tok_start;
            while (is_ident_char(*p)) p++;
            o.lx.pos = (size_t)(p - source);
            TokenType t = keyword_type(tok_start, (int)(p - tok_start));
            emit(&o, t, tok_start, (int)(p - tok_start));
            continue;
        }
        if (c == '"') {
            lex_string(&o, tok_start);
            continue;
        }

        TokenType t = TOK_EOF;
        int width = 1;
        switch (c) {
            case '(': t = TOK_LPAREN; break;
            case ')': t = TOK_RPAREN; break;
            case '{': t = TOK_LBRACE; break;
            case '}': t = TOK_RBRACE; break;
            case '[': t = TOK_LBRACKET; break;
            case ']': t = TOK_RBRACKET; break;
            case ',': t = TOK_COMMA; break;
            case ';': t = TOK_SEMI; break;
            case '.': t = TOK_DOT; break;
            case ':': t = TOK_COLON; break;
            case '?': t = TOK_QUESTION; break;
            case '+': t = TOK_PLUS; break;
            case '-': t = TOK_MINUS; break;
            case '*': t = TOK_STAR; break;
            case '/': t = TOK_SLASH; break;
            case '%': t = TOK_PERCENT; break;
            case '<':
                t = source[o.lx.pos + 1] == '=' ? (width = 2, TOK_LE) : TOK_LT;
                break;
            case '>':
                t = source[o.lx.pos + 1] == '=' ? (width = 2, TOK_GE) : TOK_GT;
                break;
            case '=':
                t = source[o.lx.pos + 1] == '=' ? (width = 2, TOK_EQEQ)
                    : source[o.lx.pos + 1] == '>' ? (width = 2, TOK_ARROW)
                    : TOK_EQ;
                break;
            case '!':
                t = source[o.lx.pos + 1] == '=' ? (width = 2, TOK_NEQ) : TOK_NOT;
                break;
            case '|':
                t = source[o.lx.pos + 1] == '|' ? (width = 2, TOK_OR) : TOK_ERROR;
                break;
            case '&':
                t = source[o.lx.pos + 1] == '&' ? (width = 2, TOK_AND) : TOK_ERROR;
                break;
            default:
                t = TOK_ERROR;
                break;
        }
        (void)save_pos;
        if (t == TOK_ERROR) {
            lx_error(&o.lx, "unexpected character");
            emit(&o, TOK_ERROR, tok_start, 0);
            break;
        }
        o.lx.pos += (size_t)width;
        emit(&o, t, tok_start, width);
    }

    if (o.lx.err[0]) {
        if (errbuf && errbuf_size)
            snprintf(errbuf, errbuf_size, "%s", o.lx.err);
        if (out_count) *out_count = 0;
        free(o.toks);
        return NULL;
    }
    if (out_count) *out_count = o.count;
    return o.toks;
}

/* Re-exported through lume.h's parse_program; the token array is caller-freed. */
Token *al_lex(const char *source, char *errbuf, size_t errbuf_size,
              int *out_count) {
    return lex_all(source, errbuf, errbuf_size, out_count);
}