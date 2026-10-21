/* SQLite statement guardrails, shared by the interpreter (builtins_sql.c)
 * and the native backends (bridge_native.c, --compile-text / --compile).
 *
 * Single-copy header so the whitelist cannot drift between runtimes — the
 * P6 parity trap ("classic double-copy sync trap") applies to guardrails
 * just as much as to emitters. Pure string logic, no VM dependency, so
 * both TUs inline the same predicates.
 *
 * Contract: exactly one known-shape statement per call. Read path allows
 * only a bare SELECT; write path allows INSERT / UPDATE / DELETE
 * (UPDATE/DELETE must carry WHERE), CREATE TABLE, the strictly-shaped
 * additive ALTER TABLE ADD COLUMN, and a WITH prefix whose subqueries are
 * read-only (SELECT/VALUES) or RETURNING DML and whose trailing statement
 * passes the same bare-write whitelist. Everything else is rejected.
 */

#ifndef LUME_SQL_GUARD_H
#define LUME_SQL_GUARD_H

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define SQL_MAX 8192

/* Strip leading SQL comments so the statement-type check starts at real SQL. */
static inline void strip_comments(const char *sql, char *buf, size_t n) {
    const char *s = sql;
    for (int k = 0; k < 64; k++) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        if (s[0] == '-' && s[1] == '-') {
            const char *nl = strchr(s, '\n');
            if (!nl) { buf[0] = '\0'; return; }
            s = nl + 1;
        } else if (s[0] == '/' && s[1] == '*') {
            const char *end = strstr(s + 2, "*/");
            if (!end) { buf[0] = '\0'; return; }
            s = end + 2;
        } else {
            break;
        }
    }
    snprintf(buf, n, "%s", s);
}

/* Return an error message if the statement must not run, else NULL.
 * body receives the comment-stripped SQL for prepare. */
static inline const char *sql_validate_read(const char *sql, char *body, size_t n) {
    const char *t = sql;
    while (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n') t++;
    if (!*t) return "empty SQL";
    size_t len = strlen(t);
    if (t[len - 1] == ';') len--;
    if (memchr(t, ';', len)) return "multiple statements are not allowed";
    strip_comments(t, body, n);
    char *b = body;
    while (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n') b++;
    if (strncasecmp(b, "select", 6) != 0 ||
        (b[6] != ' ' && b[6] != '\t' && b[6] != '\n' && b[6] != '\r'))
        return "only SELECT statements are allowed";
    return NULL;
}

/* keyword appears at s with a word boundary on both sides */
static inline int kw_at(const char *s, const char *w) {
    size_t n = strlen(w);
    return strncasecmp(s, w, n) == 0 &&
           !(isalnum((unsigned char)s[n]) || s[n] == '_');
}

/* Case-insensitive substring search. strcasestr is a GNU/BSD extension and
 * does not exist on Windows; strncasecmp is portable everywhere we build. */
static inline const char *ci_strstr(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return hay;
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0) return hay;
    return NULL;
}

static inline int kw_anywhere(const char *s, const char *w) {
    size_t n = strlen(w);
    for (const char *p = s; (p = ci_strstr(p, w)) != NULL; p += n) {
        int left_ok = (p == s) ||
                      !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        if (left_ok && !(isalnum((unsigned char)p[n]) || p[n] == '_')) return 1;
    }
    return 0;
}

/* word boundary after a keyword: next char is neither alnum nor '_'
 * (end-of-string counts as a boundary). Same predicate as kw_at's tail. */
static inline int sql_break(unsigned char c) {
    return !(isalnum(c) || c == '_');
}

/* Scan one identifier — bare [A-Za-z0-9_]+ or a simple "double-quoted"
 * name (no embedded quotes; no caller needs SQL-standard doubling).
 * Returns the position just past it, or NULL when nothing
 * identifier-shaped sits at p. */
static inline const char *sql_scan_ident(const char *p) {
    if (*p == '"') {
        const char *e = strchr(p + 1, '"');
        return e ? e + 1 : NULL;
    }
    const char *s = p;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    return p > s ? p : NULL;
}

/* Return NULL when b is exactly ALTER TABLE <ident> ADD [COLUMN] <ident>
 * <typedef>, else an error message. This is the only ALTER form allowed:
 * DROP/RENAME COLUMN, ALTER COLUMN and table constraints all stay rejected.
 * The typedef is charset-restricted to identifiers, numbers, spaces,
 * parens and commas — enough for TEXT / INTEGER / REAL / VARCHAR(120) /
 * DECIMAL(10,2), unable to smuggle a second statement or a string literal.
 * SQLite independently refuses the shapes that would be unsafe here anyway
 * (ADD COLUMN takes no PRIMARY KEY/UNIQUE and no CURRENT_TIMESTAMP default),
 * and schema-evolution callers emit a bare "<col> <type>". */
static inline const char *sql_validate_alter_add(const char *b) {
    const char *p = b + 5; /* past "alter" (kw_at already matched) */
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "table", 5) != 0 || !sql_break((unsigned char)p[5]))
        return "only ALTER TABLE ... ADD COLUMN is allowed";
    p += 5;
    while (*p == ' ' || *p == '\t') p++;
    p = sql_scan_ident(p);
    if (!p) return "ALTER TABLE needs a table name";
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "add", 3) != 0 || !sql_break((unsigned char)p[3]))
        return "only ALTER TABLE ... ADD COLUMN is allowed";
    p += 3;
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "column", 6) == 0 && sql_break((unsigned char)p[6])) {
        p += 6;
        while (*p == ' ' || *p == '\t') p++;
    }
    p = sql_scan_ident(p);
    if (!p) return "ALTER TABLE ADD needs a column name";
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return "ALTER TABLE ADD needs a column type";
    for (; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '_' || c == ' ' || c == '\t' ||
              c == '(' || c == ')' || c == ','))
            return "unsafe column type in ALTER TABLE";
    }
    return NULL;
}

/* ---- write CTE guardrail (P6 gray zone, 2026-10-19) ----
 * A WITH prefix is accepted: WITH [RECURSIVE] name [(cols)]
 * AS (read-only subquery) [, ...] <one whitelisted write statement>.
 * Subqueries must be SELECT/VALUES (a nested WITH is allowed) and may
 * not contain write or dangerous keywords; the trailing statement is
 * checked by the exact same single-statement whitelist as a bare write.
 * Because the WHERE-must-exist check runs on the trailing statement text
 * only, a WHERE inside a subquery never exempts a WHERE-less UPDATE or
 * DELETE. The whole thing is still one statement (no ';'), so SQLite
 * runs the entire CTE write atomically inside the single per-call
 * connection — the stateless/fork-safe/guardrail contracts all hold. */

/* Skip whitespace and SQL comments (both -- line and block forms). */
static inline const char *sql_skip_ws_comment(const char *p) {
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (p[0] == '-' && p[1] == '-') {
            const char *nl = strchr(p, '\n');
            if (!nl) return p + strlen(p);
            p = nl + 1;
        } else if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            if (!e) return p + strlen(p);
            p = e + 2;
        } else {
            return p;
        }
    }
}

/* Skip a '...' or "..." literal; SQLite doubling ('' / "") honored.
 * Caller guarantees *p is a quote. */
static inline const char *sql_skip_string(const char *p) {
    char q = *p;
    for (p++; *p; p++) {
        if (*p == q) {
            if (p[1] == q) { p++; continue; }
            return p + 1;
        }
    }
    return p;
}

/* p points at '('; return the matching ')' (nested parens, strings and
 * comments skipped), or NULL when unbalanced. */
static inline const char *sql_match_paren(const char *p) {
    int depth = 0;
    for (; *p; p++) {
        if (*p == '\'' || *p == '"') { p = sql_skip_string(p) - 1; continue; }
        if (*p == '-' && p[1] == '-') {
            const char *nl = strchr(p, '\n');
            if (!nl) return NULL;
            p = nl;
            continue;
        }
        if (*p == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            if (!e) return NULL;
            p = e + 1;
            continue;
        }
        if (*p == '(') depth++;
        else if (*p == ')' && --depth == 0) return p;
    }
    return NULL;
}

/* Validate one WITH subquery body [s, end).
 *  - SELECT/VALUES/nested-WITH head: the body must be read-only — DML
 *    words are rejected at any depth.
 *  - DML head (INSERT/UPDATE/DELETE, SQLite 3.35+ data-modifying CTE):
 *    a top-level RETURNING is mandatory; an UPDATE/DELETE must carry a
 *    top-level WHERE before that RETURNING (a WHERE nested in a
 *    parenthesized expression never exempts it — the whole point of the
 *    gray zone is that a WHERE-less UPDATE inside a WITH is still a
 *    WHERE-less UPDATE). One DML verb per body; DML inside a nested
 *    expression is rejected.
 *  - Dangerous words (DROP/PRAGMA/ALTER/CREATE/...) are rejected at any
 *    depth. Strings and comments are skipped everywhere. */
static inline const char *sql_cte_body_ok(const char *s, const char *end) {
    const char *p = sql_skip_ws_comment(s);
    if (p >= end) return "empty WITH subquery";

    int head = 0; /* 0 = select/values/with, 1 = insert, 2 = update/delete */
    if (kw_at(p, "insert")) head = 1;
    else if (kw_at(p, "update") || kw_at(p, "delete")) head = 2;
    else if (!kw_at(p, "select") && !kw_at(p, "values") && !kw_at(p, "with"))
        return "WITH subquery must be SELECT/VALUES or a RETURNING DML";

    static const char *const dml_words[] = {
        "insert", "update", "delete", NULL };
    static const char *const forb[] = {
        "drop", "truncate", "vacuum", "attach", "detach",
        "reindex", "pragma", "grant", "revoke", "copy",
        "alter", "create", "begin", "commit", "rollback", NULL };

    int depth = 0, top_ret = 0, top_where = 0, first = 1;
    for (p = s; p < end; ) {
        p = sql_skip_ws_comment(p);
        if (p >= end) break;
        if (*p == '\'' || *p == '"') { p = sql_skip_string(p); continue; }
        if (*p == '(') { depth++; p++; continue; }
        if (*p == ')') { depth--; p++; continue; }
        if (!(isalnum((unsigned char)*p) || *p == '_')) { p++; continue; }

        if (head == 0 && depth >= 0) {
            for (int i = 0; dml_words[i]; i++)
                if (kw_at(p, dml_words[i]))
                    return "write keyword in read-only WITH subquery";
        }
        if (depth > 0) {
            for (int i = 0; dml_words[i]; i++)
                if (kw_at(p, dml_words[i]))
                    return "DML inside a WITH subquery expression";
        }
        if (head > 0 && depth == 0 && !first) {
            for (int i = 0; dml_words[i]; i++)
                if (kw_at(p, dml_words[i]))
                    return "only one statement per WITH subquery";
        }
        if (depth == 0) {
            if (kw_at(p, "where")) {
                if (head == 2 && top_ret)
                    return "WHERE after RETURNING in WITH subquery";
                top_where = 1;
            } else if (kw_at(p, "returning")) {
                if (head == 2 && !top_where)
                    return "UPDATE/DELETE in WITH must carry a WHERE clause";
                top_ret = 1;
            }
        }
        for (int i = 0; forb[i]; i++)
            if (kw_at(p, forb[i]))
                return "dangerous statement rejected (DROP/TRUNCATE/…)";

        p = sql_scan_ident(p);
        if (!p) return "bad token in WITH subquery";
        first = 0;
    }
    if (head > 0 && !top_ret)
        return "DML in WITH must carry RETURNING";
    return NULL;
}

static inline const char *sql_validate_write_stmt(const char *b);

/* b starts at "with". Parse the CTE list, then whitelist the trailing
 * statement. */
static inline const char *sql_validate_write_cte(const char *b) {
    const char *p = b + 4; /* past "with" */
    p = sql_skip_ws_comment(p);
    if (kw_at(p, "recursive")) {
        p += 9;
        p = sql_skip_ws_comment(p);
    }
    for (;;) {
        p = sql_skip_ws_comment(p);
        const char *name = sql_scan_ident(p);
        if (!name || name == p) return "WITH needs a CTE name";
        p = sql_skip_ws_comment(name);
        if (*p == '(') {
            /* optional column alias list: idents/commas only */
            const char *q = p + 1;
            for (;;) {
                q = sql_skip_ws_comment(q);
                if (!*q || *q == ')') break;
                q = sql_scan_ident(q);
                if (!q) return "bad WITH column list";
                q = sql_skip_ws_comment(q);
                if (*q == ',') { q++; continue; }
                break;
            }
            if (*q != ')') return "bad WITH column list";
            p = sql_skip_ws_comment(q + 1);
        }
        if (strncasecmp(p, "as", 2) != 0 || !sql_break((unsigned char)p[2]))
            return "WITH CTE needs AS (...)";
        p += 2;
        p = sql_skip_ws_comment(p);
        if (*p != '(') return "WITH CTE body must be parenthesized";
        const char *close = sql_match_paren(p);
        if (!close) return "unbalanced parentheses in WITH subquery";
        const char *verr = sql_cte_body_ok(p + 1, close);
        if (verr) return verr;
        p = sql_skip_ws_comment(close + 1);
        if (*p == ',') { p++; continue; }
        break;
    }
    if (!*p) return "WITH needs a write statement after the CTE list";
    /* SQLite only allows WITH to prefix SELECT/INSERT/UPDATE/DELETE, so a
     * CREATE/ALTER body can never be valid — reject instead of letting
     * prepare fail on it. */
    if (kw_at(p, "alter") ||
        (strncasecmp(p, "create", 6) == 0 &&
         sql_break((unsigned char)p[6])))
        return "WITH can only prefix INSERT / UPDATE / DELETE";
    return sql_validate_write_stmt(p);
}

/* Return an error message if the statement must not run, else NULL.
 * body receives the comment-stripped SQL for prepare. A WITH prefix is
 * parsed by sql_validate_write_cte; a bare statement goes straight to
 * sql_validate_write_stmt. */
static inline const char *sql_validate_write(const char *sql, char *body, size_t n) {
    const char *t = sql;
    while (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n') t++;
    if (!*t) return "empty SQL";
    size_t len = strlen(t);
    if (t[len - 1] == ';') len--;
    if (memchr(t, ';', len)) return "multiple statements are not allowed";
    strip_comments(t, body, n);
    char *b = body;
    while (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n') b++;
    if (kw_at(b, "with"))
        return sql_validate_write_cte(b);
    return sql_validate_write_stmt(b);
}

/* Single non-CTE statement against the whitelist. Allowed: INSERT /
 * UPDATE / DELETE (UPDATE/DELETE must carry WHERE), CREATE TABLE, and
 * the strictly-shaped additive ALTER TABLE ADD COLUMN above. Everything
 * else (dangerous DDL, multi-statement tricks) is rejected. */
static inline const char *sql_validate_write_stmt(const char *b) {
    /* Additive schema evolution is the one DDL beyond CREATE TABLE:
     * generated apps add columns to existing tables on startup. */
    if (kw_at(b, "alter"))
        return sql_validate_alter_add(b);

    static const char *const dangerous[] = {
        "drop", "truncate", "vacuum", "attach", "detach",
        "reindex", "pragma", "grant", "revoke", "copy", NULL };
    for (int i = 0; dangerous[i]; i++) {
        if (kw_at(b, dangerous[i]))
            return "dangerous statement rejected (DROP/TRUNCATE/…)";
    }

    if (kw_at(b, "insert") || kw_at(b, "update") || kw_at(b, "delete")) {
        if ((kw_at(b, "update") || kw_at(b, "delete")) &&
            !kw_anywhere(b, "where"))
            return "UPDATE/DELETE must include a WHERE clause";
        return NULL;
    }
    if (strncasecmp(b, "create", 6) == 0 &&
        (b[6] == ' ' || b[6] == '\t' || b[6] == '\n' || b[6] == '\r')) {
        if (strncasecmp(b + 7, "table", 5) == 0 &&
            (b[12] == ' ' || b[12] == '\t' || b[12] == '\n' || b[12] == '\r'))
            return NULL;
        return "only CREATE TABLE DDL is allowed (new tables only)";
    }
    return "only INSERT / UPDATE / DELETE / CREATE TABLE allowed";
}

#endif /* LUME_SQL_GUARD_H */
