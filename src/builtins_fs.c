/* 文件 / 环境 / 目录 / 锁 / 时间内建——从 builtins.c 拆出(2026-09-27)。
 * env(凭据脱敏)、files、read_file、write_file(原子写)、mkdir、
 * lock_file/unlock_file(flock)、strftime、put。 */

#include "builtins_internal.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
/* Advisory file lock + mkdir portability. On POSIX these wrap flock(2) and
 * mkdir(2); on Windows (mingw-w64) they map to LockFileEx / _mkdir so the
 * same source builds a native .exe. LOCK_* are the POSIX <sys/file.h> values,
 * defined here for the Windows branch. */
#ifdef _WIN32
/* Windows implementations live in os_win32.c (compiled only on Windows) so
 * this TU can include lume.h's TokenType without colliding with the
 * TokenType enumerator that <windows.h> pulls in via winnt.h. */
#include <io.h>
#include <fcntl.h>
#ifndef LOCK_SH
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_UN 8
#define LOCK_NB 4
#endif
int lume_mkdir(const char *p);
int lume_flock(int fd, int op);
#else
#include <sys/file.h>
#include <sys/stat.h>
static int lume_mkdir(const char *p) { return mkdir(p, 0700); }
static int lume_flock(int fd, int op) { return flock(fd, op); }
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Append v to a list, rooting it while the backing array may grow. */
void list_push(VM *vm, Obj *list, Value v) {
    vm_push(vm, v);
    if (list->as.list.count == list->as.list.cap) {
        int nc = list->as.list.cap ? list->as.list.cap * 2 : 8;
        Value *ni = realloc(list->as.list.items, sizeof(Value) * (size_t)nc);
        if (ni) { list->as.list.items = ni; list->as.list.cap = nc; }
    }
    list->as.list.items[list->as.list.count++] = v;
    vm_pop(vm);
}

int str_entry_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int skill_entry_cmp(const void *a, const void *b) {
    const SkillInfo *x = *(const SkillInfo *const *)a;
    const SkillInfo *y = *(const SkillInfo *const *)b;
    return strcmp(x->name, y->name);
}

/* Case-insensitive word-boundary match (avoids strcasestr's _GNU_SOURCE
 * requirement on glibc; builtins.c is compiled on both Linux and macOS).
 * A keyword only matches when both sides are non-alphanumeric, so
 * credential-ish substrings inside configuration names stay readable:
 * HTPASSWD_FILE must not match PASSWD (auth path is not a secret), while
 * LLM_API_KEY / ROUTER_API_KEY / MY_TOKEN / LUME_AUTH_PASSWORD do. */
static int str_ci_contains_word(const char *hay, const char *needle) {
    size_t hn = strlen(hay), nn = strlen(needle);
    if (nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++) {
        if (i > 0 && isalnum((unsigned char)hay[i - 1])) continue;
        size_t j = 0;
        for (; j < nn; j++) {
            if (tolower((unsigned char)hay[i + j]) !=
                tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nn &&
            (i + nn >= hn || !isalnum((unsigned char)hay[i + nn])))
            return 1;
    }
    return 0;
}

/* Credential environment variables are masked from .lume scripts. The DSL
 * layer can read any env(), and an untrusted script must not be able to
 * exfiltrate keys (LLM_API_KEY, ROUTER_API_URL bearer creds, ...) via
 * read_file/env. The runtime itself reads these via getenv directly
 * (llm.c), so masking only affects the script surface. Match is a
 * case-insensitive substring on credential keywords — deliberately
 * conservative: configuration names (PORT, DOCROOT, HTPASSWD_FILE,
 * IQUEST_*, ...) never contain these keywords. */
static int env_is_sensitive(const char *k) {
    static const char *const KW[] = {
        "API_KEY", "SECRET", "PASSWORD", "PASSWD", "TOKEN", "CREDENTIAL",
    };
    for (size_t i = 0; i < sizeof KW / sizeof KW[0]; i++) {
        if (str_ci_contains_word(k, KW[i])) return 1;
    }
    return 0;
}

void native_env(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "env() needs a variable name"); return; }
    const char *k = NULL;
    if (!arg_string(vm, args[0], &k)) return;
    const char *v = getenv(k);
    /* Sensitive names read as unset (null), same shape as a missing var. */
    *out = (v && !env_is_sensitive(k)) ? make_string_cstr(vm, v) : val_null();
}

/* CLI script arguments after the script name (`lume-core script.lume a b`
 * → ["a", "b"]); empty list when the run carried none. main() seeds them into
 * vm.argv (a GC root); this getter also covers embedded/other entry paths. */
void native_argv(VM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)args;
    if (vm->argv) { *out = val_obj(vm->argv); return; }
    Obj *list = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)list));
    *out = vm_pop(vm);
}

/* Runtime guard for the opt-in --no-fs switch (see VM.no_fs). env() already
 * masks credentials, but without this a script could still exfiltrate them by
 * read_file(".env"), so a "masked env" story is only as strong as this lock.
 * Call it first in every path-taking builtin; on refusal the builtin raises a
 * runtime error, which is the loudest possible answer (a silently null return
 * would make an untrusted script indistinguishable from a missing file). */
static bool fs_permitted(VM *vm, const char *what) {
    if (!vm->no_fs) return true;
    vm_set_error(vm, "%s(): filesystem access is disabled in this run (--no-fs / LUME_NO_FS=1)", what);
    return false;
}

/* Sorted directory listing; directories carry a trailing "/". Missing or
 * unreadable dirs yield an empty list (a discovery page should degrade).
 * One argument: single-level listing of `dir` (names only, as before). A
 * second truthy argument (`files(dir, 1)`) walks the tree depth-first; entries
 * are then relative paths ("sub/file.txt", "sub/dir/") so read_file() can use
 * them directly. Depth is capped at 64 and each directory's own entry count at
 * 1024 (the single-level listing's cap), so a hostile deep tree cannot blow
 * the stack or the list. */
static void files_walk(VM *vm, Obj *list, const char *dir, const char *prefix,
                       int depth, int recurse) {
    DIR *d = opendir(dir);
    if (!d) return;
    char *names[1024];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 1024) {
        if (e->d_name[0] == '.') continue;
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof names[0], str_entry_cmp);
    char full[4096];
    for (int i = 0; i < n; i++) {
        struct stat st;
        int is_dir = 0;
        if (snprintf(full, sizeof full, "%s/%s", dir, names[i]) <
            (int)sizeof full) {
            is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        }
        /* entry = prefix + name, plus "/" when it is a directory (the marker
         * single-level files() already uses). malloc'd, not a stack buffer:
         * prefix grows with nesting depth. */
        size_t pl = strlen(prefix);
        size_t nl = strlen(names[i]);
        char *entry = malloc(pl + nl + 2);
        if (!entry) { free(names[i]); continue; }
        memcpy(entry, prefix, pl);
        memcpy(entry + pl, names[i], nl + 1);
        if (is_dir) { entry[pl + nl] = '/'; entry[pl + nl + 1] = '\0'; }
        list_push(vm, list, make_string_cstr(vm, entry));
        if (recurse && is_dir && depth < 64)
            files_walk(vm, list, full, entry, depth + 1, recurse);
        free(entry);
        free(names[i]);
    }
}

void native_files(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "files() needs a directory path"); return; }
    if (!fs_permitted(vm, "files")) return;
    const char *dir = NULL;
    if (!arg_string(vm, args[0], &dir)) return;
    int recurse = argc >= 2 && value_truthy(args[1]);
    Obj *list = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)list)); /* root while filling */
    files_walk(vm, list, dir, "", 0, recurse);
    *out = vm_pop(vm);
}

/* Whole file contents as a string, or null when missing/unreadable.
 * 16 MiB cap: this exists for catalog/skill inspection, not memory dumps. */
void native_read_file(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "read_file() needs a path"); return; }
    if (!fs_permitted(vm, "read_file")) return;
    const char *p = NULL;
    if (!arg_string(vm, args[0], &p)) return;
    FILE *f = fopen(p, "rb");
    if (!f) { *out = val_null(); return; }
    sbuf b = {0};
    char buf[16384];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        if (b.len + got > (16u << 20)) {
            fclose(f);
            free(b.p);
            vm_set_error(vm, "read_file() too large: %s", p);
            *out = val_null();
            return;
        }
        sb_mem(&b, buf, got);
    }
    fclose(f);
    if (b.oom || !b.p) {
        free(b.p);
        *out = val_null();
        return;
    }
    *out = make_string(vm, b.p, b.len);
    free(b.p);
}

/* Whole file write from a string (binary-safe); the settings page persists
 * the PSE approval switch into frameworks/autogen-pse/.env through this.
 * Returns true/false. Mirror of native_read_file, capped at 16 MiB.
 * NOTE: string payload lives inline after the Obj (obj_string()), NOT in
 * as.str.data — that pointer field is never set and was a "works until the
 * fopen actually succeeds" latent bug.
 * Atomic: the payload goes to a sibling .tmp.<pid> file which is then
 * rename()d over the target. A crash mid-write can never leave a truncated
 * file behind — product tools overwrite portfolio.json / .env through this
 * and a torn write would otherwise destroy the only copy of the data. */
void native_write_file(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2) { vm_set_error(vm, "write_file() needs a path and content"); return; }
    if (!fs_permitted(vm, "write_file")) return;
    const char *p = NULL;
    if (!arg_string(vm, args[0], &p)) return;
    if (!IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_STRING) {
        vm_set_error(vm, "write_file() content must be a string");
        return;
    }
    const char *data = obj_string(AS_OBJ(args[1]));
    size_t len = obj_string_len(AS_OBJ(args[1]));
    if (len > (16u << 20)) {
        vm_set_error(vm, "write_file() too large: %s", p);
        *out = val_bool(false);
        return;
    }
    if (strlen(p) + 32 >= 4096) { *out = val_bool(false); return; }
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", p, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) { *out = val_bool(false); return; }
    /* 数据文件默认 0600(账本/周报/设置 .env 都经此写;即使用户把
     * IQUEST_REPORTS_DIR 指到 .data 之外,报告也不会随 umask 落成 0644) */
#ifndef _WIN32
    fchmod(fileno(f), 0600);
#endif
    size_t wrote = data && len ? fwrite(data, 1, len, f) : 0;
    int ok = (fclose(f) == 0) && (wrote == len);
    if (ok) {
#ifdef _WIN32
        /* MSVCRT rename() refuses to replace an existing file: drop the old
         * one first (best-effort - it may not exist) so write_file() keeps
         * its "atomically replace" contract on Windows too. */
        remove(p);
        ok = rename(tmp, p) == 0;
#else
        ok = rename(tmp, p) == 0;
#endif
    }
    if (!ok) remove(tmp);
    *out = val_bool(ok);
}

/* Ensure a directory exists, creating it (with parents) when missing. Returns
 * true when the path exists as a directory afterwards. Lets product tools
 * lazily create their private data dirs (`.data/`, `.data/reports/`) instead
 * of hoping a build/deploy step pre-made them.
 * Mode 0700: these dirs hold session/report/portfolio data — 0755 would let
 * other local users read them (matches the 0600 session files). */
void native_mkdir(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "mkdir() needs a directory path"); return; }
    if (!fs_permitted(vm, "mkdir")) return;
    const char *p = NULL;
    if (!arg_string(vm, args[0], &p)) return;
    if (!p[0]) { *out = val_bool(false); return; }
    char tmp[4096];
    if (strlen(p) >= sizeof tmp) { *out = val_bool(false); return; }
    strcpy(tmp, p);
    for (char *c = tmp + 1; *c; c++) {
        if (*c == '/') {
            *c = '\0';
            if (lume_mkdir(tmp) != 0 && errno != EEXIST) { *out = val_bool(false); return; }
            *c = '/';
        }
    }
    if (lume_mkdir(tmp) != 0 && errno != EEXIST) { *out = val_bool(false); return; }
    struct stat st;
    *out = val_bool(stat(tmp, &st) == 0 && S_ISDIR(st.st_mode));
}

/* ---- advisory file lock (single lock per VM process) ---- */

/* flock(2)-based mutual exclusion for product data files: invest.lume wraps
 * its read-modify-write of .data/portfolio.json in lock_file/unlock_file so
 * two workers cannot lose an update to each other. The lock is held on the
 * open fd; it is released automatically when the process dies (no stale lock
 * files to clean up). One lock per VM process: acquiring again replaces the
 * previous lock, which is enough for the single-ledger pattern. */
static int g_lock_fd = -1;

void native_lock_file(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "lock_file() needs a path"); return; }
    if (!fs_permitted(vm, "lock_file")) return;
    const char *p = NULL;
    if (!arg_string(vm, args[0], &p)) return;
    long wait_ms = 2000;
    if (argc >= 2 && IS_NUM(args[1])) {
        wait_ms = (long)AS_NUM(args[1]);
        if (wait_ms < 0) wait_ms = 0;
        if (wait_ms > 30000) wait_ms = 30000;
    }
    if (g_lock_fd >= 0) {
        lume_flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
    }
#ifdef _WIN32
    int fd = open(p, _O_CREAT | _O_RDWR, _S_IREAD | _S_IWRITE);
#else
    int fd = open(p, O_CREAT | O_RDWR, 0600);
#endif
    if (fd < 0) { *out = val_bool(false); return; }
    struct timeval t0;
    gettimeofday(&t0, NULL);
    for (;;) {
        if (lume_flock(fd, LOCK_EX | LOCK_NB) == 0) break;
        if (errno == EINTR) continue;
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            close(fd);
            *out = val_bool(false);
            return;
        }
        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed_ms = (now.tv_sec - t0.tv_sec) * 1000 +
                          (now.tv_usec - t0.tv_usec) / 1000;
        if (elapsed_ms >= wait_ms) {
            close(fd);
            *out = val_bool(false);
            return;
        }
        usleep(25000); /* 25 ms backoff */
    }
    g_lock_fd = fd;
    *out = val_bool(true);
}

void native_unlock_file(VM *vm, int argc, Value *args, Value *out) {
    (void)vm; (void)argc; (void)args;
    if (g_lock_fd >= 0) {
        lume_flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
    }
    *out = val_bool(true);
}

/* Localtime format of a unix timestamp, like strftime(3). The DSL has no date
 * type, so now() alone can't name or timestamp a report file. */
void native_strftime(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2) {
        vm_set_error(vm, "strftime() needs a format and a unix timestamp");
        return;
    }
    const char *fmt = NULL;
    if (!arg_string(vm, args[0], &fmt)) return;
    if (!IS_NUM(args[1])) {
        vm_set_error(vm, "strftime() timestamp must be a number");
        return;
    }
    time_t t = (time_t)AS_NUM(args[1]);
    struct tm tm;
    char buf[160];
#ifdef _WIN32
    if (localtime_s(&tm, &t) == 0 && strftime(buf, sizeof buf, fmt, &tm) > 0)
#else
    if (localtime_r(&t, &tm) && strftime(buf, sizeof buf, fmt, &tm) > 0)
#endif
        *out = make_string_cstr(vm, buf);
    else
        *out = make_string_cstr(vm, "");
}

/* Dynamic-key map write: put(map, key, value) -> the map. Maps hold object
 * references, so this is the one place the DSL can key by a runtime value
 * (a portfolio symbol) instead of a statically-known member name. */
void native_put(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 3) { vm_set_error(vm, "put() needs a map, a key and a value"); return; }
    if (!IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_MAP) {
        vm_set_error(vm, "put() key must be a map");
        return;
    }
    const char *k = NULL;
    if (!arg_string(vm, args[1], &k)) return;
    map_set(vm, AS_OBJ(args[0]), k, args[2]); /* roots map + value internally */
    *out = args[0];
}

/* Read from standard input (2026-10-09, lume-nsgm M2 interactive CLI).
 *   read_stdin()        -> one line, newline stripped ("" at EOF / empty line)
 *   read_stdin("all")   -> everything until EOF
 * Used by lume-nsgm's `init` command to collect the module name and field
 * definitions interactively. EOF and an empty line both yield "" so scripts
 * only have to test emptiness; the 16 MiB cap matches read_file(). */
void native_read_stdin(VM *vm, int argc, Value *args, Value *out) {
    const char *mode = NULL;
    if (argc >= 1 && !arg_string(vm, args[0], &mode)) return;
    int all = mode && strcmp(mode, "all") == 0;
    sbuf b = {0};
    if (all) {
        char buf[16384];
        size_t got;
        while ((got = fread(buf, 1, sizeof buf, stdin)) > 0) {
            if (b.len + got > (16u << 20)) {
                free(b.p);
                vm_set_error(vm, "read_stdin() too large");
                *out = val_null();
                return;
            }
            sb_mem(&b, buf, got);
        }
    } else {
        int c;
        while ((c = fgetc(stdin)) != EOF && c != '\n') {
            if (b.len + 1 > (16u << 20)) {
                free(b.p);
                vm_set_error(vm, "read_stdin() too large");
                *out = val_null();
                return;
            }
            sb_mem(&b, (const char *)&c, 1);
        }
    }
    if (b.oom) { free(b.p); *out = val_null(); return; }
    /* EOF / empty input must be "" (an empty line), not null — interactive
     * scripts test emptiness, and "" + x stays a string concatenation. */
    if (!b.p || b.len == 0) { *out = make_string(vm, "", 0); return; }
    *out = make_string(vm, b.p, b.len);
    free(b.p);
}
