/* builtins_spawn.c — P14: gated, rooted child processes for lume-core.
 *
 * Read TODO.md P14 before changing any of this. Four invariants:
 *
 *  1. spawn is OFF by default. `--spawn` / `LUME_SPAWN=1` turns the surface
 *     on; `LUME_SPAWN_ALLOW` is a MANDATORY comma-separated command
 *     allowlist. Enabling spawn without an allowlist permits nothing
 *     (fail closed) — an ungated shell primitive is exactly the disclosure
 *     the no_fs / no_net / LUME_FS_ROOT work exists to prevent.
 *  2. No shell, and no PATH search. argv is a list of strings; the executable
 *     is resolved to an absolute realpath and handed to execv()/execve().
 *     There is no system(), no "sh -c", no execvp(), no command line built
 *     from script text. Bare command names only resolve inside the
 *     LUME_SPAWN_ROOT directory set; without it they are refused.
 *  3. The script never receives a raw fd. spawn() returns a 1-based opaque
 *     handle into a fixed process table and every sibling takes that handle.
 *     A deterministic slot index (not the OS pid / fd numbers) is also the
 *     only shape in which the two native emitters could ever agree
 *     byte-identically in tests/native-consistency.lume.
 *  4. An explicit env map is filtered: LD_ and DYLD_ loader variables and
 *     PATH overrides are refused, and only a small baseline is inherited.
 *  5. POSIX only. The Windows build filters this file out and compiles the
 *     stubs in builtins.c (LUME_HAS_SPAWN=0), same shape as builtins_http.c.
 *
 * A slot is reclaimed only after its child has been waited for, and every
 * (re)use bumps a generation counter embedded in the handle. A stale handle
 * therefore never addresses a different child, while a long-lived VM is no
 * longer capped at 31 spawns — the original fixed-table design would have
 * made a sidecar executor fail with "process table is full" after 31 jobs.
 */
#include "builtins_internal.h"

#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* POSIX leaves environ undeclared unless a feature macro is set. */
extern char **environ;

#define LUME_SPAWN_SLOTS   32
#define LUME_SPAWN_ARGC_MAX 64
#define LUME_SPAWN_ARG_LEN  4096
#define LUME_SPAWN_INPUT_MAX 4096   /* <= PIPE_BUF: one atomic, non-blocking write */
#define LUME_SPAWN_READ_MAX  65536
#define LUME_SPAWN_DEFAULT_TIMEOUT_MS 500
#define LUME_SPAWN_PATH_MAX  8192
#define LUME_SPAWN_ENV_MAX   64      /* max explicit env entries */
#define LUME_SPAWN_ENV_KEY_MAX 64
#define LUME_SPAWN_ENV_VAL_MAX 4096

typedef struct {
    pid_t pid;
    int in_fd, out_fd, err_fd;
    bool waited;
    bool active;              /* slot denotes a child until reclaimed */
    unsigned long long gen;   /* generation tag embedded in the handle */
    int exit_code;
} ProcSlot;

static ProcSlot g_slots[LUME_SPAWN_SLOTS];
static unsigned long long g_next_gen;
static bool g_cleanup_registered;

/* ------------------------------------------------------------------ *
 * Result maps. Every builtin here answers with {ok, ..., err} instead
 * of raising a VM error: the caller can inspect a refused spawn and keep
 * running, which is what a supervisor script needs. Mirrors http_get().
 * ------------------------------------------------------------------ */
static Value result(VM *vm, bool ok, const char *err) {
    Obj *r = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(r));              /* make_string_cstr may GC */
    map_set(vm, r, "ok", val_bool(ok));
    map_set(vm, r, "err", make_string_cstr(vm, err ? err : ""));
    return vm_pop(vm);
}

static Value spawn_result(VM *vm, bool ok, long long handle, long long pid,
                          const char *err) {
    Value v = result(vm, ok, err);
    Obj *r = AS_OBJ(v);
    vm_push(vm, v);
    map_set(vm, r, "handle", val_int(handle));
    map_set(vm, r, "pid", val_int(pid));
    return vm_pop(vm);
}

static Value read_result(VM *vm, bool ok, const char *data, size_t len,
                         bool eof, const char *err) {
    Value v = result(vm, ok, err);
    Obj *r = AS_OBJ(v);
    vm_push(vm, v);
    map_set(vm, r, "data", make_string(vm, data ? data : "", len));
    map_set(vm, r, "eof", val_bool(eof));
    return vm_pop(vm);
}

static Value wait_result(VM *vm, bool ok, long long code, const char *err) {
    Value v = result(vm, ok, err);
    Obj *r = AS_OBJ(v);
    vm_push(vm, v);
    map_set(vm, r, "exit", val_int(code));
    return vm_pop(vm);
}

/* ------------------------------------------------------------------ *
 * Gate. Deliberately two independent checks: a supervisor can enable the
 * surface globally but still refuse every command by leaving the allowlist
 * empty, and an allowlist alone never enables spawn.
 * ------------------------------------------------------------------ */
static bool gate_open(VM *vm, const char *what, Value *out) {
    if (vm->spawn_enabled) return true;
    *out = result(vm, false,
                  "spawn is disabled in this run (--spawn / LUME_SPAWN=1)");
    /* Same loud phrasing as fs_permitted() in builtins_fs.c, kept in the map
     * rather than vm_set_error() so a script can branch on ok. */
    (void)what;
    return false;
}

/* Exact, case-sensitive match against one comma/space separated allowlist
 * entry. A command containing '/' must be listed verbatim as an absolute
 * path — matching on basename would let /tmp/evil/echo pass an "echo" rule. */
static bool allowlist_has(const char *allow, const char *cmd) {
    size_t n = strlen(cmd);
    const char *p = allow;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        const char *s = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
        size_t m = (size_t)(p - s);
        if (m == n && strncmp(s, cmd, m) == 0) return true;
    }
    return false;
}

static bool command_allowed(VM *vm, const char *what, const char *cmd,
                            Value *out) {
    const char *allow = vm->spawn_allow;
    if (!allow || !allow[0]) {
        *out = result(vm, false,
                      "spawn is enabled but LUME_SPAWN_ALLOW is empty; no command is permitted");
        (void)what; (void)cmd;
        return false;
    }
    if (!allowlist_has(allow, cmd)) {
        /* Do not echo the rejected command back: it is script-controlled and
         * would let a caller probe the allowlist through error text. */
        *out = result(vm, false, "command is not in LUME_SPAWN_ALLOW");
        (void)what; (void)cmd;
        return false;
    }
    return true;
}

static bool handle_ok(VM *vm, const char *what, Value arg, ProcSlot **slot,
                      Value *out) {
    if (!IS_INT(arg)) {
        *out = result(vm, false, "handle must be an int from spawn()");
        return false;
    }
    long long h = AS_INT(arg);
    /* handle = generation * SLOT_COUNT + slot. Both halves are checked:
     * a recycled slot carries a new generation, so an old handle cannot
     * silently start addressing a different child. */
    long long gen = h / LUME_SPAWN_SLOTS;
    int idx = (int)(h % LUME_SPAWN_SLOTS);
    if (gen < 1 || idx < 1 || !g_slots[idx].active ||
        g_slots[idx].gen != (unsigned long long)gen) {
        *out = result(vm, false, "unknown or closed process handle");
        (void)what;
        return false;
    }
    *slot = &g_slots[idx];
    return true;
}

static const char *string_of(Value v) {
    if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING)
        return obj_string(AS_OBJ(v));
    return NULL;
}

/* ------------------------------------------------------------------ *
 * Executable resolution (P14 step 2, TODO item "env + LUME_SPAWN_ROOT").
 * execvp()'s PATH search was the last way to execute a binary the
 * allowlist did not actually name: a writable directory earlier in PATH
 * could shadow an allowlisted "echo". Nothing here consults PATH.
 *
 *  - bare names (no slash) are looked up only in the LUME_SPAWN_ROOT set;
 *    without a root they are refused;
 *  - explicit paths are realpath()'ed and, when a root is configured,
 *    must resolve inside one of the roots;
 *  - every candidate is realpath()'ed again and checked with X_OK, so a
 *    symlink inside a root cannot point at an executable outside it.
 *
 * main.c already validated the root set at startup; we re-resolve per
 * spawn so a symlink swapped after startup is still seen. Errors are
 * constant strings: script-controlled argv[0] is never echoed back.
 * ------------------------------------------------------------------ */
static bool path_within(const char *root_real, const char *rp) {
    size_t bl = strlen(root_real);
    if (strncmp(rp, root_real, bl) != 0) return false;
    char c = rp[bl];
    /* "/opt/bin" must not match "/opt/bin-evil" */
    return c == '\0' || c == '/';
}

/* Copy one colon-separated root segment into buf. Returns false for an
 * over-long or empty segment (refused rather than silently truncated). */
static bool root_segment(const char **cursor, char *buf, size_t cap) {
    const char *p = *cursor;
    const char *s = p;
    while (*p && *p != ':') p++;
    size_t n = (size_t)(p - s);
    if (*p == ':') p++;
    *cursor = p;
    if (n == 0 || n >= cap) return false;
    memcpy(buf, s, n);
    buf[n] = '\0';
    return true;
}

static bool root_contains(const char *roots, const char *rp) {
    const char *p = roots;
    char seg[LUME_SPAWN_PATH_MAX];
    while (*p) {
        if (root_segment(&p, seg, sizeof seg)) {
            char *rr = lume_realpath(seg);
            if (rr) {
                bool hit = path_within(rr, rp);
                free(rr);
                if (hit) return true;
            }
        }
    }
    return false;
}

static char *resolve_exec(VM *vm, const char *cmd, const char **why) {
    *why = NULL;
    const char *roots = vm->spawn_root;
    bool rooted = roots && roots[0];
    bool bare = strchr(cmd, '/') == NULL;
    if (bare && !rooted) {
        *why = "bare command names require LUME_SPAWN_ROOT or an absolute path (PATH is never searched)";
        return NULL;
    }

    char cand[LUME_SPAWN_PATH_MAX];
    char *resolved = NULL;
    if (bare) {
        const char *p = roots;
        char seg[LUME_SPAWN_PATH_MAX];
        while (*p && !resolved) {
            if (!root_segment(&p, seg, sizeof seg)) continue;
            char *rr = lume_realpath(seg);
            if (!rr) continue;
            size_t rl = strlen(rr), cl = strlen(cmd);
            if (rl + 1 + cl + 1 <= sizeof cand) {
                memcpy(cand, rr, rl);
                cand[rl] = '/';
                memcpy(cand + rl + 1, cmd, cl + 1);
                char *rp = lume_realpath(cand);
                if (rp) {
                    if (path_within(rr, rp) && access(rp, X_OK) == 0)
                        resolved = rp;
                    else
                        free(rp);
                }
            }
            free(rr);
        }
    } else {
        char *rp = lume_realpath(cmd);
        if (rp) {
            bool ok = access(rp, X_OK) == 0;
            if (ok && rooted) ok = root_contains(roots, rp);
            if (ok) resolved = rp; else free(rp);
        }
    }
    if (!resolved)
        *why = rooted
             ? "executable not found in LUME_SPAWN_ROOT, outside it, or not executable"
             : "executable not found or not executable";
    return resolved;
}

/* ------------------------------------------------------------------ *
 * Optional env map (P14 step 2). An explicit env does NOT inherit the
 * whole parent environment: only a small baseline (PATH/HOME/locale/
 * TERM/...) plus the script's entries. LD_ and DYLD_ loader variables
 * and PATH / IFS / SHELL overrides are refused — otherwise `env` would
 * simply re-open the injection surface the executable root just closed.
 * Absent env means inherit the parent environment (execv, old behaviour).
 * ------------------------------------------------------------------ */
static bool env_key_valid(const char *k) {
    if (!k || !k[0] || strlen(k) > LUME_SPAWN_ENV_KEY_MAX) return false;
    unsigned char c = (unsigned char)k[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return false;
    for (const char *p = k + 1; *p; p++) {
        c = (unsigned char)*p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

static bool env_key_denied(const char *k) {
    static const char *blocked[] = {
        "PATH", "IFS", "ENV", "BASH_ENV", "SHELL", "LD_PRELOAD",
        "LD_LIBRARY_PATH", "LD_AUDIT", "LD_DEBUG",
        "DYLD_INSERT_LIBRARIES", "DYLD_LIBRARY_PATH",
        "DYLD_FRAMEWORK_PATH", "DYLD_FALLBACK_LIBRARY_PATH", NULL
    };
    for (int i = 0; blocked[i]; i++)
        if (strcmp(k, blocked[i]) == 0) return true;
    return strncmp(k, "LD_", 3) == 0 || strncmp(k, "DYLD_", 5) == 0;
}

static bool env_baseline(const char *k, size_t n) {
    static const char *base[] = {
        "PATH", "HOME", "LANG", "LC_ALL", "LC_CTYPE", "TMPDIR",
        "TERM", "TZ", "USER", "LOGNAME", NULL
    };
    for (int i = 0; base[i]; i++)
        if (n == strlen(base[i]) && strncmp(k, base[i], n) == 0) return true;
    return n >= 3 && strncmp(k, "LC_", 3) == 0;
}

static void free_envp(char **p) {
    if (!p) return;
    for (char **e = p; *e; e++) free(*e);
    free(p);
}

/* NULL + *why == NULL: no env option (inherit). NULL + *why set: refusal.
 * Otherwise a malloc'd, NULL-terminated envp the caller must free_envp(). */
static char **build_env(VM *vm, Obj *opts, const char **why) {
    *why = NULL;
    if (!opts) return NULL;
    int found = 0;
    Value ev = map_get(vm, opts, "env", &found);
    if (!found || IS_NULL(ev)) return NULL;
    if (!IS_OBJ(ev) || AS_OBJ(ev)->type != OBJ_MAP) {
        *why = "spawn(): env must be a map of strings";
        return NULL;
    }
    Obj *m = AS_OBJ(ev);
    if (m->as.map.count > LUME_SPAWN_ENV_MAX) {
        *why = "spawn(): env has too many entries";
        return NULL;
    }
    for (int i = 0; i < m->as.map.count; i++) {
        const char *k = m->as.map.keys[i];
        const char *v = string_of(m->as.map.vals[i]);
        if (!env_key_valid(k) || env_key_denied(k)) {
            *why = "spawn(): env contains an invalid or restricted key";
            return NULL;
        }
        if (!v || strlen(v) > LUME_SPAWN_ENV_VAL_MAX) {
            *why = "spawn(): env values must be strings <= 4096 bytes";
            return NULL;
        }
    }

    size_t inherited = 0;
    for (char **e = environ; e && *e; e++) inherited++;
    char **out = calloc(inherited + (size_t)m->as.map.count + 1, sizeof(char *));
    if (!out) { *why = "spawn(): out of memory"; return NULL; }
    size_t n = 0;
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        size_t kl = (size_t)(eq - *e);
        if (!env_baseline(*e, kl)) continue;
        bool overridden = false;
        for (int i = 0; i < m->as.map.count; i++) {
            const char *k = m->as.map.keys[i];
            if (strlen(k) == kl && strncmp(*e, k, kl) == 0) { overridden = true; break; }
        }
        if (overridden) continue;
        out[n] = strdup(*e);
        if (!out[n]) { free_envp(out); *why = "spawn(): out of memory"; return NULL; }
        n++;
    }
    for (int i = 0; i < m->as.map.count; i++) {
        const char *k = m->as.map.keys[i];
        const char *v = string_of(m->as.map.vals[i]);
        size_t kl = strlen(k), vl = strlen(v);
        char *entry = malloc(kl + 1 + vl + 1);
        if (!entry) { free_envp(out); *why = "spawn(): out of memory"; return NULL; }
        memcpy(entry, k, kl);
        entry[kl] = '=';
        memcpy(entry + kl + 1, v, vl + 1);
        out[n++] = entry;
    }
    /* calloc already zero-terminated at out[n] */
    return out;
}

static long long timeout_of(Value *args, int argc, int idx) {
    if (argc > idx && IS_INT(args[idx]) && AS_INT(args[idx]) >= 0)
        return AS_INT(args[idx]);
    return LUME_SPAWN_DEFAULT_TIMEOUT_MS;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Kill + reap anything the script forgot about. Registered once, at the
 * first successful spawn; without it a long-lived VM that spawned children
 * and exited would leave zombies until its own parent reaps them. */
static void cleanup_children(void) {
    for (int i = 1; i < LUME_SPAWN_SLOTS; i++) {
        ProcSlot *s = &g_slots[i];
        if (!s->active || s->waited) continue;
        kill(s->pid, SIGKILL);
        int status = 0;
        while (waitpid(s->pid, &status, 0) < 0 && errno == EINTR) { }
        s->waited = true;
        if (s->in_fd >= 0) close(s->in_fd);
        if (s->out_fd >= 0) close(s->out_fd);
        if (s->err_fd >= 0) close(s->err_fd);
    }
}

static int slot_alloc(void) {
    for (int i = 1; i < LUME_SPAWN_SLOTS; i++)
        if (!g_slots[i].active) return i;
    /* All 31 slots are occupied. A waited child is already reaped and its
     * fds closed, so retiring that slot is safe; its old handle stops
     * working because handle_ok() checks the generation. Never displace a
     * child that is still running. */
    for (int i = 1; i < LUME_SPAWN_SLOTS; i++)
        if (g_slots[i].waited) { g_slots[i].active = false; return i; }
    return -1;
}

static long long handle_encode(int slot) {
    g_slots[slot].gen = ++g_next_gen;
    g_slots[slot].active = true;
    return (long long)(g_slots[slot].gen * LUME_SPAWN_SLOTS + (unsigned)slot);
}

static void closefd(int *fd) {
    if (*fd >= 0) { close(*fd); *fd = -1; }
}

/* ------------------------------------------------------------------ *
 * spawn(argv, {input?, env?}) -> {ok, handle, pid, err}
 * ------------------------------------------------------------------ */
void native_spawn(VM *vm, int argc, Value *args, Value *out) {
    *out = spawn_result(vm, false, 0, 0, "");
    if (argc < 1) {
        *out = spawn_result(vm, false, 0, 0, "spawn(): argv is required");
        return;
    }
    if (!gate_open(vm, "spawn", out)) {
        /* gate_open already wrote a plain {ok,err}; normalise to the spawn
         * shape so callers can always read .handle / .pid. */
        *out = spawn_result(vm, false, 0, 0,
                            "spawn is disabled in this run (--spawn / LUME_SPAWN=1)");
        return;
    }
    if (!IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_LIST) {
        *out = spawn_result(vm, false, 0, 0, "spawn(): argv must be a list of strings");
        return;
    }
    Obj *av_obj = AS_OBJ(args[0]);
    int n = av_obj->as.list.count;
    if (n < 1 || n > LUME_SPAWN_ARGC_MAX) {
        *out = spawn_result(vm, false, 0, 0, "spawn(): argv length out of range");
        return;
    }

    char *av[LUME_SPAWN_ARGC_MAX + 1];
    for (int i = 0; i <= LUME_SPAWN_ARGC_MAX; i++) av[i] = NULL;
    for (int i = 0; i < n; i++) {
        const char *s = string_of(av_obj->as.list.items[i]);
        if (!s || strlen(s) > LUME_SPAWN_ARG_LEN) {
            for (int j = 0; j < i; j++) free(av[j]);
            *out = spawn_result(vm, false, 0, 0,
                                "spawn(): argv must contain only strings <= 4096 bytes");
            return;
        }
        av[i] = strdup(s);
        if (!av[i]) {
            for (int j = 0; j < i; j++) free(av[j]);
            *out = spawn_result(vm, false, 0, 0, "spawn(): out of memory");
            return;
        }
    }

    Obj *opts = NULL;
    if (argc >= 2 && IS_OBJ(args[1]) && AS_OBJ(args[1])->type == OBJ_MAP)
        opts = AS_OBJ(args[1]);

    const char *input = NULL;
    if (opts) {
        int found = 0;
        Value in = map_get(vm, opts, "input", &found);
        if (found && !IS_NULL(in)) {
            input = string_of(in);
            if (!input || strlen(input) > LUME_SPAWN_INPUT_MAX) {
                for (int j = 0; j < n; j++) free(av[j]);
                *out = spawn_result(vm, false, 0, 0,
                                    "spawn(): input must be a string <= 4096 bytes");
                return;
            }
        }
    }

    if (!command_allowed(vm, "spawn", av[0], out)) {
        for (int j = 0; j < n; j++) free(av[j]);
        /* command_allowed wrote a plain result; normalise the shape. */
        const char *msg = "command is not in LUME_SPAWN_ALLOW";
        if (!vm->spawn_allow || !vm->spawn_allow[0])
            msg = "spawn is enabled but LUME_SPAWN_ALLOW is empty; no command is permitted";
        *out = spawn_result(vm, false, 0, 0, msg);
        return;
    }

    /* Resolve only AFTER the allowlist check: a refused command must not
     * leak filesystem facts (which roots exist, which paths resolve). */
    const char *why = NULL;
    char *exec_path = resolve_exec(vm, av[0], &why);
    if (!exec_path) {
        for (int j = 0; j < n; j++) free(av[j]);
        *out = spawn_result(vm, false, 0, 0, why);
        return;
    }
    char **envp = build_env(vm, opts, &why);
    if (why) {
        free(exec_path);
        for (int j = 0; j < n; j++) free(av[j]);
        *out = spawn_result(vm, false, 0, 0, why);
        return;
    }

    int slot = slot_alloc();
    if (slot < 0) {
        free(exec_path);
        free_envp(envp);
        for (int j = 0; j < n; j++) free(av[j]);
        *out = spawn_result(vm, false, 0, 0,
                            "spawn(): process table is full (31 children still running)");
        return;
    }

    int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0 || pipe(err_pipe) < 0) {
        closefd(&in_pipe[0]); closefd(&in_pipe[1]);
        closefd(&out_pipe[0]); closefd(&out_pipe[1]);
        closefd(&err_pipe[0]); closefd(&err_pipe[1]);
        free(exec_path);
        free_envp(envp);
        for (int j = 0; j < n; j++) free(av[j]);
        *out = spawn_result(vm, false, 0, 0, "spawn(): pipe() failed");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        closefd(&in_pipe[0]); closefd(&in_pipe[1]);
        closefd(&out_pipe[0]); closefd(&out_pipe[1]);
        closefd(&err_pipe[0]); closefd(&err_pipe[1]);
        free(exec_path);
        free_envp(envp);
        for (int j = 0; j < n; j++) free(av[j]);
        *out = spawn_result(vm, false, 0, 0, "spawn(): fork() failed");
        return;
    }
    if (pid == 0) {
        /* Child: no shell, no inherited watcher/HTTP fds beyond stdio.
         * exec_path is an absolute realpath; PATH is never consulted. */
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        if (envp) execve(exec_path, av, envp);
        else execv(exec_path, av);
        fprintf(stderr, "lume spawn: exec failed: %s\n", strerror(errno));
        _exit(127);
    }

    /* Parent-only: the child either exec'd or _exit'd with these maps. */
    free(exec_path);
    free_envp(envp);
    closefd(&in_pipe[0]);
    closefd(&out_pipe[1]);
    closefd(&err_pipe[1]);
    if (input) {
        size_t len = strlen(input), off = 0;
        while (off < len) {
            ssize_t w = write(in_pipe[1], input + off, len - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) break;
            off += (size_t)w;
        }
    }
    closefd(&in_pipe[1]);

    fcntl(out_pipe[0], F_SETFL, fcntl(out_pipe[0], F_GETFL) | O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, fcntl(err_pipe[0], F_GETFL) | O_NONBLOCK);

    g_slots[slot].pid = pid;
    g_slots[slot].in_fd = -1;
    g_slots[slot].out_fd = out_pipe[0];
    g_slots[slot].err_fd = err_pipe[0];
    g_slots[slot].waited = false;
    g_slots[slot].exit_code = -1;

    if (!g_cleanup_registered) {
        atexit(cleanup_children);
        g_cleanup_registered = true;
    }
    for (int j = 0; j < n; j++) free(av[j]);
    *out = spawn_result(vm, true, handle_encode(slot), (long long)pid, "");
}

/* ------------------------------------------------------------------ *
 * proc_read(handle, "stdout"|"stderr", timeout_ms?) -> {ok, data, eof, err}
 * ------------------------------------------------------------------ */
void native_proc_read(VM *vm, int argc, Value *args, Value *out) {
    *out = read_result(vm, false, "", 0, false, "");
    if (!gate_open(vm, "proc_read", out)) return;
    ProcSlot *s = NULL;
    if (argc < 1 || !handle_ok(vm, "proc_read", args[0], &s, out)) return;

    const char *which = "stdout";
    if (argc >= 2) {
        which = string_of(args[1]);
        if (!which) {
            *out = read_result(vm, false, "", 0, false, "stream must be a string");
            return;
        }
    }
    int fd;
    if (strcmp(which, "stdout") == 0) fd = s->out_fd;
    else if (strcmp(which, "stderr") == 0) fd = s->err_fd;
    else {
        *out = read_result(vm, false, "", 0, false,
                           "stream must be \"stdout\" or \"stderr\"");
        return;
    }
    if (fd < 0) {
        *out = read_result(vm, true, "", 0, true, "");
        return;
    }

    long long timeout = timeout_of(args, argc, 2);
    struct pollfd p = { .fd = fd, .events = POLLIN | POLLHUP };
    int pr = poll(&p, 1, (int)(timeout > 60000 ? 60000 : timeout));
    if (pr < 0) {
        if (errno == EINTR) { *out = read_result(vm, true, "", 0, false, ""); return; }
        *out = read_result(vm, false, "", 0, false, "poll() failed");
        return;
    }
    if (pr == 0) { *out = read_result(vm, true, "", 0, false, ""); return; }

    char buf[LUME_SPAWN_READ_MAX];
    size_t used = 0;
    bool eof = false;
    while (used < sizeof buf) {
        ssize_t r = read(fd, buf + used, sizeof buf - used);
        if (r > 0) { used += (size_t)r; continue; }
        if (r == 0) { eof = true; break; }
        if (errno == EINTR) continue;
        break; /* EAGAIN: drained for now */
    }
    if (eof) closefd(fd == s->out_fd ? &s->out_fd : &s->err_fd);
    *out = read_result(vm, true, buf, used, eof, "");
}

/* ------------------------------------------------------------------ *
 * proc_wait(handle, timeout_ms?) -> {ok, exit, err}
 * ------------------------------------------------------------------ */
void native_proc_wait(VM *vm, int argc, Value *args, Value *out) {
    *out = wait_result(vm, false, -1, "");
    if (!gate_open(vm, "proc_wait", out)) return;
    ProcSlot *s = NULL;
    if (argc < 1 || !handle_ok(vm, "proc_wait", args[0], &s, out)) return;
    if (s->waited) {
        *out = wait_result(vm, true, s->exit_code, "");
        return;
    }

    long long deadline = now_ms() + timeout_of(args, argc, 1);
    for (;;) {
        int status = 0;
        pid_t r = waitpid(s->pid, &status, WNOHANG);
        if (r == s->pid) {
            s->waited = true;
            s->exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                         : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
            closefd(&s->out_fd);
            closefd(&s->err_fd);
            *out = wait_result(vm, true, s->exit_code, "");
            return;
        }
        if (r < 0 && errno != EINTR) {
            *out = wait_result(vm, false, -1, "waitpid() failed");
            return;
        }
        if (now_ms() >= deadline) {
            *out = wait_result(vm, false, -1, "proc_wait(): timeout");
            return;
        }
        usleep(10000);
    }
}

/* ------------------------------------------------------------------ *
 * proc_kill(handle) -> bool
 * ------------------------------------------------------------------ */
void native_proc_kill(VM *vm, int argc, Value *args, Value *out) {
    *out = val_bool(false);
    if (!vm->spawn_enabled || argc < 1) return;
    ProcSlot *s = NULL;
    Value sink = val_null();
    if (!handle_ok(vm, "proc_kill", args[0], &s, &sink)) return;
    if (s->waited) { *out = val_bool(false); return; }
    *out = val_bool(kill(s->pid, SIGTERM) == 0);
}

#endif /* !_WIN32 */
