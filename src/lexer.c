#include "lume.h"
#include <errno.h>
#include <limits.h>

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
    /* Must zero the numeric-literal fields for EVERY token, not just numbers:
     * emit_number() sets them, but a non-number token never goes through it, so
     * leaving them unset made the parser read indeterminate values — a stray
     * neg_min made an ordinary literal report "does not fit in int". */
    tok.inum = 0;
    tok.is_int = 0;
    tok.neg_min = 0;
    /* Same story for the interpolation payload: a stale non-NULL istr would be
     * freed twice (or a garbage pointer freed once) by the token sweep. */
    tok.istr = NULL;
    tok.istr_len = 0;
    o->toks[o->count++] = tok;
}

/* Lex a numeric literal. Integers are the language's `int` (i64, two's
 * complement — see docs/SPEC.md) and must not pass through double: strtod
 * silently rounds anything past 2^53, so `9007199254740993` used to become
 * ...992 with no diagnostic anywhere, and the native backends (which work in
 * i64) disagreed with the interpreter about it. Only literals carrying '.',
 * 'e' or 'E' take the strtod path and are `float`.
 *
 * An integer too large for i64 is an error rather than a silent wrap: a value
 * the source cannot represent is a source bug, and quietly changing it is how
 * the two backends drifted apart in the first place. */
static void emit_number(LexOut *o, const char *start, int len) {
    emit(o, TOK_NUMBER, start, len);
    if (len == 0) return;
    char buf[128];
    if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
    memcpy(buf, start, (size_t)len);
    buf[len] = '\0';

    bool is_float = false;
    for (int i = 0; i < len; i++)
        if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') { is_float = true; break; }

    Token *t = &o->toks[o->count - 1];
    t->is_int = is_float ? 0 : 1;
    if (!is_float) {
        errno = 0;
        char *end = NULL;
        long long v = strtoll(buf, &end, 10);
        /* ERANGE covers both under- and overflow; a non-NUL end means the text
         * was not a plain integer (the scanner admits 'e', e.g. `1e5`).
         *
         * One exception: 9223372036854775808 (= 2^63) is out of range for
         * strtoll but is a perfectly good literal here, because the parser
         * applies a unary minus to it — `-9223372036854775808` is the minimum
         * i64. The minus is a separate token, so accept the magnitude at the
         * boundary and let the parser negate it in i64. A bare
         * `9223372036854775808` with no minus in front still errors, because
         * nothing can represent it. */
        bool negatable = (errno == ERANGE && v == LLONG_MAX);
        if (end == buf || *end != '\0' || (errno == ERANGE && !negatable)) {
            t->is_int = 0;
            lx_error(&o->lx, "integer literal does not fit in int (i64)");
            return;
        }
        t->inum = v;
        t->num = (double)v;  /* kept for diagnostics/float-widening paths */
        /* 2^63 arrives here clamped to LLONG_MAX, but it is only representable
         * as LLONG_MIN. Flag it; the parser folds it into the minimum when it
         * sees the unary minus (see the neg_min note in lume.h), and rejects it
         * when there is no minus, because nothing else can hold the value. */
        t->neg_min = negatable;
        return;
    }
    t->num = strtod(buf, NULL);
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
    {"fn",     TOK_KW_FN},
    {"get", TOK_GET}, {"head", TOK_HEAD}, {"post", TOK_POST},
    {"put", TOK_PUT}, {"patch", TOK_PATCH}, {"delete", TOK_DELETE},
    {"options", TOK_OPTIONS},
    {"verbs", TOK_VERBS},
    {"enum", TOK_ENUM}, {"match", TOK_MATCH},
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

/* ---------- interpolated strings: s!"..." ----------
 *
 * Segment scan (Lean 4's s! rules): in TEXT state `{{` and `}}` are literal
 * braces and a lone `{` opens an expression segment; in EXPR state braces
 * nest by depth and a `"` skips a whole string literal (so `{"x"}` works),
 * with the matching `}` closing the segment. A lone `}` in TEXT state is a
 * literal (it can only be the tail of a `}}` pair, which the two-char peek
 * already consumed, or an unpaired brace — Lean keeps it verbatim too).
 *
 * The segments are serialized into the token's istr payload:
 *   'L' + int32 len + bytes   literal segment, already unescaped
 *   'E' + int32 len + bytes   expression segment, raw source text
 * The parser turns that into str(...) + ... — the checker and the emitters
 * never learn interpolation exists. */

/* Append one segment (tag + length + payload) to a growable buffer. The
 * length is written as int32_t — the parser reads exactly that width, and a
 * wider write here silently shifts every following segment (the first version
 * of this stored a size_t, which truncated `"hi "` to `"i"` on little-endian
 * because only the low 4 bytes were consumed as the length and the high 4
 * zero bytes then parsed as an empty tag-0 segment). */
static void istr_push(char **buf, size_t *len, size_t *cap,
                      char tag, const char *data, size_t n) {
    int32_t n32 = (int32_t)n;
    size_t need = *len + 1 + sizeof(int32_t) + n;
    if (need > *cap) {
        *cap = need * 2;
        *buf = realloc(*buf, *cap);
    }
    (*buf)[(*len)++] = tag;
    memcpy(*buf + *len, &n32, sizeof(int32_t));
    *len += sizeof(int32_t);
    if (n) memcpy(*buf + *len, data, n);
    *len += n;
}

static void lex_istr(LexOut *o, const char *str_start) {
    Lexer *lx = &o->lx;
    /* str_start points at the `s`; the body opens at str_start + 3. */
    const char *p = str_start + 3;
    char *segs = NULL;
    size_t slen = 0, scap = 0;

    /* Literal-segment accumulator: source bytes with the `{{` / `}}` folds
     * already collapsed. The escape table is NOT applied here — the segment
     * becomes an ordinary quote-wrapped LIT_STR (istr_build adds the quotes),
     * and every consumer of a LIT_STR unescapes exactly once. Unescaping in
     * the lexer instead would double-unescape (`\\n` -> newline in the
     * segment, then again at eval). The folds cannot ride on a pointer+length
     * into the source, because the source spells them with two characters. */
    char *lit = NULL;
    size_t llen = 0, lcap = 0;

    enum { TEXT, EXPR, EXPR_STR } state = TEXT;
    int depth = 0;
    const char *expr_start = NULL;

    while (true) {
        char c = *p;
        if (c == '\0') {
            lx_error(lx, state == TEXT
                    ? "unterminated interpolated string literal"
                    : "unterminated interpolation segment (missing '}')");
            goto fail;
        }
        if (c == '\n' && state == TEXT) {
            /* Same rule as plain strings: no raw newline in the literal part. */
            lx_error(lx, "unterminated interpolated string literal");
            goto fail;
        }
        if (state == TEXT) {
            if (c == '{' && p[1] == '{') {
                if (llen + 1 > lcap) { lcap = lcap ? lcap * 2 : 32; lit = realloc(lit, lcap); }
                lit[llen++] = '{';
                p += 2;
                continue;
            }
            if (c == '}' && p[1] == '}') {
                if (llen + 1 > lcap) { lcap = lcap ? lcap * 2 : 32; lit = realloc(lit, lcap); }
                lit[llen++] = '}';
                p += 2;
                continue;
            }
            if (c == '{') {
                if (llen)
                    istr_push(&segs, &slen, &scap, 'L', lit, llen);
                llen = 0;
                p++;
                state = EXPR;
                depth = 1;
                expr_start = p;
                continue;
            }
            if (c == '"') {
                p++; /* closing quote of the s!"..." spelling */
                goto done;
            }
            if (llen + 1 > lcap) { lcap = lcap ? lcap * 2 : 32; lit = realloc(lit, lcap); }
            lit[llen++] = c;
            p++;
            continue;
        }
        if (state == EXPR) {
            if (c == '"') { state = EXPR_STR; p++; continue; }
            if (c == '{') { depth++; p++; continue; }
            if (c == '}') {
                depth--;
                if (depth == 0) {
                    istr_push(&segs, &slen, &scap, 'E', expr_start,
                              (size_t)(p - expr_start));
                    p++;
                    state = TEXT;
                    continue;
                }
                p++;
                continue;
            }
            p++;
            continue;
        }
        /* EXPR_STR: inside a string literal within the expression — the only
         * character that matters is an unescaped closing quote. */
        if (c == '\\') {
            if (!p[1]) {
                /* A trailing backslash at EOF must land on the NUL, not
                 * stride past it (heap-buffer-overflow read; CI fuzz leg
                 * found it 2026-10-19, minimized to `s!"{"a\`). The next
                 * iteration then reports the unterminated string. Same
                 * guard lex_string applies to escapes. */
                p++;
                continue;
            }
            p += 2;
            continue;
        }
        if (c == '"') { state = EXPR; p++; continue; }
        p++;
    }

done:
    if (llen)
        istr_push(&segs, &slen, &scap, 'L', lit, llen);
    free(lit);
    lx->pos = (size_t)(p - lx->src);
    {
        Token t;
        memset(&t, 0, sizeof(t));
        t.type = TOK_ISTR;
        t.start = str_start;
        t.length = (int)(p - str_start);
        t.line = lx->line;
        t.istr = segs;
        t.istr_len = (int)slen;
        /* emit() zeroes the numeric fields but not istr, so append directly. */
        if (o->count >= o->cap) {
            o->cap = o->cap ? o->cap * 2 : 64;
            o->toks = realloc(o->toks, sizeof(Token) * (size_t)o->cap);
        }
        o->toks[o->count++] = t;
    }
    return;

fail:
    free(segs);
    free(lit);
    emit(o, TOK_ERROR, str_start, 0);
    /* Skip to end of line so one bad interpolation doesn't re-lex forever. */
    while (*p && *p != '\n') p++;
    lx->pos = (size_t)(p - lx->src);
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
            /* Scientific notation: an 'e'/'E' followed by an optional sign and
             * at least one decimal digit. emit_number() already routes a literal
             * containing 'e'/'E' through strtod (float), so the scanner only
             * needs to collect the exponent. A bare 'e' with no valid exponent
             * (e.g. `1e`, `1eX`) is left for the identifier scan so the prior
             * behavior is preserved for malformed input. */
            if (*p == 'e' || *p == 'E') {
                const char *e = p;
                p++;
                if (*p == '+' || *p == '-') p++;
                if (is_digit(*p)) {
                    while (is_digit(*p)) p++;
                } else {
                    p = e;  /* not a real exponent; let 'e' become an ident */
                }
            }
            o.lx.pos = (size_t)(p - source);
            emit_number(&o, tok_start, (int)(p - tok_start));
            continue;
        }
        if (is_ident_start(c)) {
            const char *p = tok_start;
            while (is_ident_char(*p)) p++;
            /* `s!"..."` is an interpolated string, not an identifier `s`
             * followed by junk. The ident scan stops right after the `s`
             * because `!` and `"` are not ident chars, so this is the one
             * place the spelling can be recognized. */
            if ((size_t)(p - tok_start) == 1 && tok_start[0] == 's' &&
                p[0] == '!' && p[1] == '"') {
                o.lx.pos = (size_t)(p - source);
                lex_istr(&o, tok_start);
                continue;
            }
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
                /* `||` stays the logical or; a lone `|` separates enum
                 * variants — it used to be a lex error, so no existing
                 * program can read the new meaning. */
                t = source[o.lx.pos + 1] == '|' ? (width = 2, TOK_OR) : TOK_PIPE;
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
        for (int i = 0; i < o.count; i++) free(o.toks[i].istr);
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