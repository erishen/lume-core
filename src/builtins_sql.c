/* SQLite builtins: sql_query / sql_write 鈥?ported into the standalone tree
 * (2026-10-12) from the host build, where they lived in lang/builtins_sql.c
 * (argument binding) and agent-httpd's sqlite_tool.c (the actual SQLite
 * work). This file is self-contained: no db_layer, no agent-httpd, no
 * app-specific table policies 鈥?the same single-statement guardrails and
 * row dump, minus the portfolio-mirror rule (that one is host business
 * policy, not language semantics).
 *
 * Compiled only when the Makefile probe found sqlite3.h (HAVE_SQLITE=1);
 * otherwise builtins.c emits native_sql_unavailable stubs and this file is
 * not in SRCS at all.
 *
 * sql_query(sql[, params]) / sql_query(path, sql[, params]) 鈥?read-only
 * SELECT, returns a list of row maps (rows capped at 200).
 * sql_write(sql[, params]) / sql_write(path, sql[, params]) 鈥?guarded write:
 * INSERT / UPDATE / DELETE (UPDATE/DELETE must carry WHERE) or CREATE TABLE;
 * DROP/ALTER/TRUNCATE/VACUUM/ATTACH/DETACH/REINDEX/PRAGMA/GRANT/REVOKE/COPY
 * are rejected. Returns the number of rows affected (0 for DDL).
 * `params` is a list of scalars bound to ? placeholders (null 鈫?SQL NULL);
 * values are converted to text and bound, so they never enter the SQL
 * string and cannot be parsed as SQL.
 *
 * Every call opens a fresh connection, so concurrent writers to the same
 * file (prefork workers) would otherwise hit SQLITE_BUSY instantly 鈥? * sqlite3's default busy timeout is 0. Both open paths therefore arm a
 * busy timeout (see sql_busy_timeout_ms below); single statements stay
 * atomic either way, the timeout only turns an instant failure into a
 * bounded wait.
 */

#include "builtins_internal.h"
#include "sql_guard_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sqlite3.h>

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
 * READONLY, the write path READWRITE 鈥?neither passes SQLITE_OPEN_CREATE,
 * so a missing db file is an error, not a silent empty database. */
static const char *sql_db_path(const char *db) {
    if (db && db[0]) return db;
    const char *p = getenv("SQLITE_DB");
    return (p && p[0]) ? p : NULL;
}

/* Busy timeout (ms) armed on every connection. Without it, sqlite3 fails
 * immediately (default timeout 0) when another connection holds the write
 * lock 鈥?under prefork workers that turns ordinary write contention into
 * spurious "database is locked" errors. 5s covers retry storms on a single
 * file; set LUME_SQLITE_BUSY_TIMEOUT_MS to override (0 restores the stock
 * fail-fast behavior, clamped to [0, 60000]; unparseable falls back). */
static int sql_busy_timeout_ms(void) {
    const char *e = getenv("LUME_SQLITE_BUSY_TIMEOUT_MS");
    if (!e || !e[0]) return 5000;
    char *end = NULL;
    long v = strtol(e, &end, 10);
    if (!end || *end != '\0' || v < 0) return 5000;
    if (v > 60000) v = 60000;
    return (int)v;
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
    sqlite3_busy_timeout(sq, sql_busy_timeout_ms());
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
    sqlite3_busy_timeout(sq, sql_busy_timeout_ms());
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
    /* A top-level RETURNING clause (SQLite 3.35+) makes the first step
     * answer SQLITE_ROW, not SQLITE_DONE — the write has already
     * happened, the rows are the RETURNING output. Draining them keeps
     * sql_write(db, "UPDATE ... RETURNING ...") working instead of
     * failing with sqlite3_errmsg's "not an error". */
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) { /* drain RETURNING */ }
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

/* ---- transactions (P6, 2026-10-20) ---- */

/* A transaction handle owns one connection and one open transaction:
 *   sql_tx_begin(path)  -> handle (GC object; collected = ROLLBACK + close)
 *   sql_tx_exec(tx, sql[, params]) -> affected rows (sql_write whitelist)
 *   sql_tx_commit(tx) / sql_tx_rollback(tx) -> bool, closes the handle
 * Error semantics: a failing statement leaves the transaction OPEN so the
 * caller decides commit vs rollback (a rolled-back statement's effects are
 * already gone; the handle is still usable). Only commit/rollback/GC close
 * the connection. All writes go through sql_validate_write 鈥?this is a
 * dedicated entry point, never through the sql_write whitelist. */

/* Open a READWRITE|CREATE connection and start a deferred transaction.
 * Returns NULL and fills err on failure (no connection is left open). */
static sqlite3 *sql_tx_open(const char *path, char *err, size_t errsz) {
    sqlite3 *sq = NULL;
    if (sqlite3_open_v2(path, &sq,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL)
        != SQLITE_OK) {
        snprintf(err, errsz, "cannot open '%s'", path);
        if (sq) sqlite3_close(sq);
        return NULL;
    }
    sqlite3_busy_timeout(sq, sql_busy_timeout_ms());
    char *m = NULL;
    if (sqlite3_exec(sq, "BEGIN", NULL, NULL, &m) != SQLITE_OK) {
        snprintf(err, errsz, "BEGIN: %s", m ? m : sqlite3_errmsg(sq));
        sqlite3_free(m);
        sqlite3_close(sq);
        return NULL;
    }
    return sq;
}

/* GC finalizer for OBJ_SQLTX (value.c calls this from obj_release). An open
 * transaction is rolled back and the connection closed. SQLite would roll
 * the open transaction back on close anyway; the explicit ROLLBACK keeps
 * the error path honest. */
void sql_tx_finalize(Obj *o) {
    if (!o || o->type != OBJ_SQLTX) return;
    if (o->as.sqltx.db && o->as.sqltx.state == 0) {
        sqlite3 *sq = (sqlite3 *)o->as.sqltx.db;
        sqlite3_exec(sq, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(sq);
        o->as.sqltx.db = NULL;
        o->as.sqltx.state = 2;
    }
    free(o->as.sqltx.path);
    free(o->as.sqltx.last_err);
    o->as.sqltx.path = NULL;
    o->as.sqltx.last_err = NULL;
}

void native_sql_tx_begin(VM *vm, int argc, Value *args, Value *out) {
    *out = val_null();
    if (argc != 1) {
        vm_set_error(vm, "sql_tx_begin needs (path)");
        return;
    }
    const char *db = NULL;
    if (!arg_string(vm, args[0], &db)) return;
    const char *path = sql_db_path(db);
    if (!path) {
        vm_set_error(vm, "sql_tx_begin: no database (set SQLITE_DB or pass a path)");
        return;
    }
    char err[512] = {0};
    sqlite3 *sq = sql_tx_open(path, err, sizeof err);
    if (!sq) {
        vm_set_error(vm, "sql_tx_begin: %s", err);
        return;
    }
    Obj *tx = gc_alloc(vm, OBJ_SQLTX, 0);
    tx->as.sqltx.db = sq;
    tx->as.sqltx.path = gc_strdup(vm, path, strlen(path));
    tx->as.sqltx.state = 0;
    tx->as.sqltx.last_rc = 0;
    tx->as.sqltx.last_err = NULL;
    *out = val_obj(tx);
}

void native_sql_tx_exec(VM *vm, int argc, Value *args, Value *out) {
    *out = val_null();
    if (argc < 2 || argc > 3) {
        vm_set_error(vm, "sql_tx_exec needs (tx, sql[, params])");
        return;
    }
    if (!IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_SQLTX) {
        vm_set_error(vm, "sql_tx_exec: first argument must be a transaction handle");
        return;
    }
    Obj *tx = AS_OBJ(args[0]);
    if (!tx->as.sqltx.db || tx->as.sqltx.state != 0) {
        vm_set_error(vm, "sql_tx_exec: transaction is already closed");
        return;
    }
    const char *dummy = NULL, *sql = NULL, **params = NULL;
    int nparams = 0;
    if (sql_arg_bind(vm, argc - 1, args + 1, &dummy, &sql, &params, &nparams))
        return;   /* vm_set_error already set */
    char body[SQL_MAX];
    const char *verr = sql_validate_write(sql, body, sizeof body);
    if (verr) {
        vm_set_error(vm, "sql_tx_exec: %s", verr);
        free_sql_params(params, nparams);
        return;
    }
    sqlite3 *sq = (sqlite3 *)tx->as.sqltx.db;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(sq, body, -1, &st, NULL) != SQLITE_OK) {
        vm_set_error(vm, "sql_tx_exec: %s", sqlite3_errmsg(sq));
        free_sql_params(params, nparams);
        return;
    }
    char err[512] = {0};
    if (bind_params(st, params, nparams, err, sizeof err)) {
        vm_set_error(vm, "sql_tx_exec: %s", err[0] ? err : "bind failed");
        sqlite3_finalize(st);
        free_sql_params(params, nparams);
        return;
    }
    /* Same RETURNING drain as sql_write_exec_impl: a top-level RETURNING
     * (SQLite 3.35+) answers SQLITE_ROW on the first step. */
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) { /* drain RETURNING */ }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        vm_set_error(vm, "sql_tx_exec: %s", sqlite3_errmsg(sq));
        free_sql_params(params, nparams);
        return;
    }
    int affected = sqlite3_changes(sq);
    free_sql_params(params, nparams);
    *out = val_int(affected);
}

/* sql_tx_query(tx, sql[, params]) — read back YOUR OWN writes inside a
 * transaction. sql_query opens an independent read-only connection per
 * call, so rows written (but not yet committed) by sql_tx_exec on the
 * transaction's private connection are invisible to it; this builtin runs
 * the SELECT on the transaction's own connection instead. Same guard as
 * sql_query (read whitelist), same row-map JSON shape, same 200-row cap,
 * same optional params list. Errors leave the transaction OPEN exactly
 * like sql_tx_exec. */
void native_sql_tx_query(VM *vm, int argc, Value *args, Value *out) {
    *out = val_null();
    if (argc < 2 || argc > 3) {
        vm_set_error(vm, "sql_tx_query needs (tx, sql[, params])");
        return;
    }
    if (!IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_SQLTX) {
        vm_set_error(vm, "sql_tx_query: first argument must be a transaction handle");
        return;
    }
    Obj *tx = AS_OBJ(args[0]);
    if (!tx->as.sqltx.db || tx->as.sqltx.state != 0) {
        vm_set_error(vm, "sql_tx_query: transaction is already closed");
        return;
    }
    const char *dummy = NULL, *sql = NULL, **params = NULL;
    int nparams = 0;
    if (sql_arg_bind(vm, argc - 1, args + 1, &dummy, &sql, &params, &nparams))
        return;   /* vm_set_error already set */
    char body[SQL_MAX];
    const char *verr = sql_validate_read(sql, body, sizeof body);
    if (verr) {
        vm_set_error(vm, "sql_tx_query: %s", verr);
        free_sql_params(params, nparams);
        return;
    }
    sqlite3 *sq = (sqlite3 *)tx->as.sqltx.db;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(sq, body, -1, &st, NULL) != SQLITE_OK) {
        vm_set_error(vm, "sql_tx_query: %s", sqlite3_errmsg(sq));
        free_sql_params(params, nparams);
        return;
    }
    char err[512] = {0};
    if (bind_params(st, params, nparams, err, sizeof err)) {
        vm_set_error(vm, "sql_tx_query: %s", err[0] ? err : "bind failed");
        sqlite3_finalize(st);
        free_sql_params(params, nparams);
        return;
    }
    sbuf b = {0};
    dump_rows(st, &b);
    sqlite3_finalize(st);
    free_sql_params(params, nparams);
    char jerr[256] = {0};
    json_parse(vm, b.p ? b.p : "[]", jerr, sizeof jerr);
    free(b.p);
    if (vm->error) { *out = val_null(); return; }
    *out = vm_pop(vm); /* json_parse pushed the result above the args */
}

/* Shared tail of commit / rollback. On success the connection is closed and
 * the handle is spent (state 1 = committed, 2 = rolled back). On failure the
 * transaction stays open and the caller decides what to do next. */
static void sql_tx_end(VM *vm, Obj *tx, int commit, Value *out) {
    *out = val_null();
    if (!tx->as.sqltx.db || tx->as.sqltx.state != 0) {
        vm_set_error(vm, "sql_tx_%s: transaction is already closed",
                     commit ? "commit" : "rollback");
        return;
    }
    sqlite3 *sq = (sqlite3 *)tx->as.sqltx.db;
    char *m = NULL;
    if (sqlite3_exec(sq, commit ? "COMMIT" : "ROLLBACK", NULL, NULL, &m)
        != SQLITE_OK) {
        vm_set_error(vm, "sql_tx_%s: %s", commit ? "commit" : "rollback",
                     m ? m : sqlite3_errmsg(sq));
        sqlite3_free(m);
        return;
    }
    sqlite3_close(sq);
    tx->as.sqltx.db = NULL;
    tx->as.sqltx.state = commit ? 1 : 2;
    *out = val_bool(true);
}

void native_sql_tx_commit(VM *vm, int argc, Value *args, Value *out) {
    *out = val_null();
    if (argc != 1 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_SQLTX) {
        vm_set_error(vm, "sql_tx_commit needs (tx)");
        return;
    }
    sql_tx_end(vm, AS_OBJ(args[0]), 1, out);
}

void native_sql_tx_rollback(VM *vm, int argc, Value *args, Value *out) {
    *out = val_null();
    if (argc != 1 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_SQLTX) {
        vm_set_error(vm, "sql_tx_rollback needs (tx)");
        return;
    }
    sql_tx_end(vm, AS_OBJ(args[0]), 0, out);
}
