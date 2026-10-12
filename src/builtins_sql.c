/* SQLite builtins: sql_query / sql_write — ported into the standalone tree
 * (2026-10-12) from the host build, where they lived in lang/builtins_sql.c
 * (argument binding) and agent-httpd's sqlite_tool.c (the actual SQLite
 * work). This file is self-contained: no db_layer, no agent-httpd, no
 * app-specific table policies — the same single-statement guardrails and
 * row dump, minus the portfolio-mirror rule (that one is host business
 * policy, not language semantics).
 *
 * Compiled only when the Makefile probe found sqlite3.h (HAVE_SQLITE=1);
 * otherwise builtins.c emits native_sql_unavailable stubs and this file is
 * not in SRCS at all.
 *
 * sql_query(sql[, params]) / sql_query(path, sql[, params]) — read-only
 * SELECT, returns a list of row maps (rows capped at 200).
 * sql_write(sql[, params]) / sql_write(path, sql[, params]) — guarded write:
 * INSERT / UPDATE / DELETE (UPDATE/DELETE must carry WHERE) or CREATE TABLE;
 * DROP/ALTER/TRUNCATE/VACUUM/ATTACH/DETACH/REINDEX/PRAGMA/GRANT/REVOKE/COPY
 * are rejected. Returns the number of rows affected (0 for DDL).
 * `params` is a list of scalars bound to ? placeholders (null → SQL NULL);
 * values are converted to text and bound, so they never enter the SQL
 * string and cannot be parsed as SQL.
 */

#include "builtins_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sqlite3.h>

#define SQL_MAX 8192
#define SQL_ROW_CAP 200

/* ---- argument binding (port of host lang/builtins_sql.c) ---- */

/* Resolve sql_query / sql_write args: (sql[, params]) or (path, sql[,
 * params]). Returns 0 and fills the out-params on success (caller frees
 * *params with free_sql_params). */
int sql_arg_bind(VM *vm, int argc, Value *args, const char **db,
                 const char **sql, const char ***params, int *nparams) {
    *db = NULL; *sql = NULL; *params = NULL; *nparams = 0;
    if (argc < 1 || argc > 3) {
        vm_set_error(vm, "sql needs (sql[, params]) or (path, sql[, params])");
        return 1;
    }
    const char *s0 = NULL, *s1 = NULL;
    if (!arg_string(vm, args[0], &s0)) return 1;
    Value pv = val_null();
    if (argc == 1) {
        *sql = s0;
    } else if (IS_OBJ(args[1]) && AS_OBJ(args[1])->type == OBJ_STRING) {
        if (!arg_string(vm, args[1], &s1)) return 1;
        *db = s0; *sql = s1;
        if (argc == 3) pv = args[2];
    } else {
        *sql = s0;
        pv = args[1];
        if (argc == 3) {
            vm_set_error(vm, "sql has too many arguments");
            return 1;
        }
    }
    if (IS_NULL(pv)) return 0; /* no params */
    if (!IS_OBJ(pv) || AS_OBJ(pv)->type != OBJ_LIST) {
        vm_set_error(vm, "params must be a list");
        return 1;
    }
    Obj *o = AS_OBJ(pv);
    int cnt = o->as.list.count;
    const char **arr = calloc((size_t)cnt + 1, sizeof(char *));
    if (!arr) { vm_set_error(vm, "out of memory"); return 1; }
    for (int i = 0; i < cnt; i++) {
        Value e = o->as.list.items[i];
        if (IS_NULL(e)) { arr[i] = NULL; continue; }
        const char *txt = NULL;
        if (IS_OBJ(e) && AS_OBJ(e)->type == OBJ_STRING) {
            txt = obj_string(AS_OBJ(e));
        } else {
            Value sv = val_null();
            str_of_value(vm, e, &sv);
            if (vm->error || !IS_OBJ(sv) || AS_OBJ(sv)->type != OBJ_STRING) {
                vm_set_error(vm, "param %d is not a scalar", i + 1);
                for (int j = 0; j < i; j++) if (arr[j]) free((void *)arr[j]);
                free(arr);
                return 1;
            }
            txt = obj_string(AS_OBJ(sv));
        }
        arr[i] = strdup(txt);
        if (!arr[i]) {
            vm_set_error(vm, "out of memory");
            for (int j = 0; j < i; j++) if (arr[j]) free((void *)arr[j]);
            free(arr);
            return 1;
        }
    }
    *params = arr;
    *nparams = cnt;
    return 0;
}

void free_sql_params(const char **params, int n) {
    if (!params) return;
    for (int i = 0; i < n; i++) if (params[i]) free((void *)params[i]);
    free((void *)params);
}

/* ---- guardrails (port of agent-httpd sqlite_tool.c) ---- */

/* Strip leading SQL comments so the statement-type check starts at real SQL. */
static void strip_comments(const char *sql, char *buf, size_t n) {
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
static const char *sql_validate_read(const char *sql, char *body, size_t n) {
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
static int kw_at(const char *s, const char *w) {
    size_t n = strlen(w);
    return strncasecmp(s, w, n) == 0 &&
           !(isalnum((unsigned char)s[n]) || s[n] == '_');
}

static int kw_anywhere(const char *s, const char *w) {
    size_t n = strlen(w);
    for (const char *p = s; (p = strcasestr(p, w)) != NULL; p += n) {
        int left_ok = (p == s) ||
                      !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        if (left_ok && !(isalnum((unsigned char)p[n]) || p[n] == '_')) return 1;
    }
    return 0;
}

/* Return an error message if the statement must not run, else NULL.
 * Allowed: INSERT / UPDATE / DELETE (UPDATE/DELETE must carry WHERE) and
 * CREATE TABLE. Everything else (dangerous DDL, multi-statement tricks) is
 * rejected. body receives the comment-stripped SQL for prepare. */
static const char *sql_validate_write(const char *sql, char *body, size_t n) {
    const char *t = sql;
    while (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n') t++;
    if (!*t) return "empty SQL";
    size_t len = strlen(t);
    if (t[len - 1] == ';') len--;
    if (memchr(t, ';', len)) return "multiple statements are not allowed";
    strip_comments(t, body, n);
    char *b = body;
    while (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n') b++;

    static const char *const dangerous[] = {
        "drop", "alter", "truncate", "vacuum", "attach", "detach",
        "reindex", "pragma", "grant", "revoke", "copy", NULL };
    for (int i = 0; dangerous[i]; i++) {
        if (kw_at(b, dangerous[i]))
            return "dangerous statement rejected (DROP/ALTER/TRUNCATE/…)";
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

/* ---- row dump (port of agent-httpd sqlite_tool.c) ---- */

/* JSON-escape a C string into an sbuf (same rules as bridge_serve.c). */
static void sql_json_str(sbuf *b, const char *s) {
    sb_chr(b, '"');
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"': sb_str(b, "\\\""); break;
        case '\\': sb_str(b, "\\\\"); break;
        case '\n': sb_str(b, "\\n"); break;
        case '\r': sb_str(b, "\\r"); break;
        case '\t': sb_str(b, "\\t"); break;
        case '\b': sb_str(b, "\\b"); break;
        case '\f': sb_str(b, "\\f"); break;
        default:
            if (c < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof tmp, "\\u%04x", c);
                sb_str(b, tmp);
            } else {
                sb_chr(b, (char)c);
            }
        }
    }
    sb_chr(b, '"');
}

static void json_cell(sqlite3_stmt *st, int col, sbuf *out) {
    switch (sqlite3_column_type(st, col)) {
    case SQLITE_INTEGER: {
        char num[64];
        snprintf(num, sizeof num, "%lld", sqlite3_column_int64(st, col));
        sb_str(out, num);
        break;
    }
    case SQLITE_FLOAT: {
        char num[64];
        snprintf(num, sizeof num, "%g", sqlite3_column_double(st, col));
        sb_str(out, num);
        break;
    }
    case SQLITE_NULL:
        sb_str(out, "null");
        break;
    default: {
        const char *txt = (const char *)sqlite3_column_text(st, col);
        sql_json_str(out, txt ? txt : "");
        break;
    }
    }
}

/* Append up to SQL_ROW_CAP rows as a JSON array of objects. */
static void dump_rows(sqlite3_stmt *st, sbuf *out) {
    int ncol = sqlite3_column_count(st);
    int first = 1;
    int seen = 0;
    int rc;
    while (seen < SQL_ROW_CAP && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        sb_str(out, first ? "[" : ",");
        if (first) first = 0;
        sb_str(out, "{");
        for (int i = 0; i < ncol; i++) {
            if (i) sb_str(out, ",");
            char key[160];
            snprintf(key, sizeof key, "\"%s\":",
                     sqlite3_column_name(st, i) ? sqlite3_column_name(st, i)
                                                : "col");
            sb_str(out, key);
            json_cell(st, i, out);
        }
        sb_str(out, "}");
        seen++;
    }
    sb_str(out, first ? "[]" : "]");
    (void)rc;
}

/* Bind ? placeholders; a NULL entry binds SQL NULL. Text binding is enough
 * because SQLite column affinity converts numbers for numeric comparison. */
static int bind_params(sqlite3_stmt *st, const char **params, int nparams,
                       char *err, size_t errsz) {
    for (int i = 0; i < nparams; i++) {
        int rc = params[i]
                     ? sqlite3_bind_text(st, i + 1, params[i], -1,
                                         SQLITE_TRANSIENT)
                     : sqlite3_bind_null(st, i + 1);
        if (rc != SQLITE_OK) {
            sqlite3 *sq = sqlite3_db_handle(st);
            snprintf(err, errsz, "bind param %d: %s", i + 1,
                     sqlite3_errmsg(sq));
            return 1;
        }
    }
    return 0;
}

/* DSN semantics kept identical to the host build: a bare path is the db
 * file; empty/NULL falls back to env SQLITE_DB. The read path opens
 * READONLY, the write path READWRITE — neither passes SQLITE_OPEN_CREATE,
 * so a missing db file is an error, not a silent empty database. */
static const char *sql_db_path(const char *db) {
    if (db && db[0]) return db;
    const char *p = getenv("SQLITE_DB");
    return (p && p[0]) ? p : NULL;
}

static int sql_query_json_impl(const char *db, const char *sql,
                               const char **params, int nparams, sbuf *out,
                               char *err, size_t errsz) {
    const char *path = sql_db_path(db);
    if (!path) {
        snprintf(err, errsz, "no database (set SQLITE_DB or pass a path)");
        return 1;
    }
    if (!sql || !sql[0]) {
        snprintf(err, errsz, "empty SQL");
        return 1;
    }
    char body[SQL_MAX];
    const char *verr = sql_validate_read(sql, body, sizeof body);
    if (verr) {
        snprintf(err, errsz, "%s", verr);
        return 1;
    }
    sqlite3 *sq = NULL;
    if (sqlite3_open_v2(path, &sq, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        snprintf(err, errsz, "cannot open '%s' read-only", path);
        if (sq) sqlite3_close(sq);
        return 1;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(sq, body, -1, &st, NULL) != SQLITE_OK) {
        snprintf(err, errsz, "%s", sqlite3_errmsg(sq));
        sqlite3_close(sq);
        return 1;
    }
    if (bind_params(st, params, nparams, err, errsz)) {
        sqlite3_finalize(st);
        sqlite3_close(sq);
        return 1;
    }
    dump_rows(st, out);
    sqlite3_finalize(st);
    sqlite3_close(sq);
    return 0;
}

static int sql_write_exec_impl(const char *db, const char *sql,
                               const char **params, int nparams,
                               int *affected, char *err, size_t errsz) {
    const char *path = sql_db_path(db);
    if (!path) {
        snprintf(err, errsz, "no database (set SQLITE_DB or pass a path)");
        return 1;
    }
    if (!sql || !sql[0]) {
        snprintf(err, errsz, "empty SQL");
        return 1;
    }
    char body[SQL_MAX];
    const char *verr = sql_validate_write(sql, body, sizeof body);
    if (verr) {
        snprintf(err, errsz, "%s", verr);
        return 1;
    }
    sqlite3 *sq = NULL;
    if (sqlite3_open_v2(path, &sq, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        snprintf(err, errsz, "cannot open '%s' for writing", path);
        if (sq) sqlite3_close(sq);
        return 1;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(sq, body, -1, &st, NULL) != SQLITE_OK) {
        snprintf(err, errsz, "%s", sqlite3_errmsg(sq));
        sqlite3_close(sq);
        return 1;
    }
    if (bind_params(st, params, nparams, err, errsz)) {
        sqlite3_finalize(st);
        sqlite3_close(sq);
        return 1;
    }
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        snprintf(err, errsz, "%s", sqlite3_errmsg(sq));
        sqlite3_close(sq);
        return 1;
    }
    *affected = sqlite3_changes(sq);
    sqlite3_close(sq);
    return 0;
}

/* ---- native builtins ---- */

void native_sql_query(VM *vm, int argc, Value *args, Value *out) {
    const char *db = NULL, *sql = NULL, **params = NULL;
    int nparams = 0;
    if (sql_arg_bind(vm, argc, args, &db, &sql, &params, &nparams)) return;
    sbuf b = {0};
    char err[512] = {0};
    if (sql_query_json_impl(db, sql, params, nparams, &b, err, sizeof err) != 0) {
        vm_set_error(vm, "sql_query: %s", err[0] ? err : "failed");
        free(b.p);
        free_sql_params(params, nparams);
        *out = val_null();
        return;
    }
    char jerr[256] = {0};
    json_parse(vm, b.p ? b.p : "[]", jerr, sizeof jerr);
    free(b.p);
    free_sql_params(params, nparams);
    if (vm->error) { *out = val_null(); return; }
    *out = vm_pop(vm); /* json_parse pushed the result above the args */
}

void native_sql_write(VM *vm, int argc, Value *args, Value *out) {
    const char *db = NULL, *sql = NULL, **params = NULL;
    int nparams = 0;
    if (sql_arg_bind(vm, argc, args, &db, &sql, &params, &nparams)) return;
    int affected = 0;
    char err[512] = {0};
    if (sql_write_exec_impl(db, sql, params, nparams, &affected,
                            err, sizeof err) != 0) {
        vm_set_error(vm, "sql_write: %s", err[0] ? err : "failed");
        free_sql_params(params, nparams);
        *out = val_null();
        return;
    }
    free_sql_params(params, nparams);
    *out = val_int(affected);
}
