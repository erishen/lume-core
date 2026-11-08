/* builtins_spawn.c — P14: gated, rooted child processes for lume-core.
 *
 * Read TODO.md P14 before changing any of this. Four invariants:
 *
 *  1. spawn is OFF by default. `--spawn` / `LUME_SPAWN=1` turns the surface
 *     on; `LUME_SPAWN_ALLOW` is a MANDATORY comma-separated command
 *     allowlist. Enabling spawn without an allowlist permits nothing
 *     (fail closed) — an ungated shell primitive is exactly the disclosure
 *     the no_fs / no_net / LUME_FS_ROOT work exists to prevent.
 *  2. No shell. argv is a list of strings handed to execvp(); there is no
 *     system(), no "sh -c", no command line assembled from script text.
 *  3. The script never receives a raw fd. spawn() returns a 1-based opaque
 *     handle into a fixed process table and every sibling takes that handle.
 *     A deterministic slot index (not the OS pid / fd numbers) is also the
 *     only shape in which the two native emitters could ever agree
 *     byte-identically in tests/native-consistency.lume.
 *  4. POSIX only. The Windows build filters this file out and compiles the
 *     stubs in builtins.c (LUME_HAS_SPAWN=0), same shape as builtins_http.c.
 *
 * Slots are never reused: once a handle is handed out it keeps denoting the
 * same child for the life of the VM, so a stale handle can never accidentally
 * operate on a different process after a slot was recycled.
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

#define LUME_SPAWN_SLOTS   32
#define LUME_SPAWN_ARGC_MAX 64
#define LUME_SPAWN_ARG_LEN  4096
#define LUME_SPAWN_INPUT_MAX 4096   /* <= PIPE_BUF: one atomic, non-blocking write */
#define LUME_SPAWN_READ_MAX  65536
#define LUME_SPAWN_DEFAULT_TIMEOUT_MS 500

typedef struct {
    pid_t pid;
    int in_fd, out_fd, err_fd;
    bool waited;
    int exit_code;
} ProcSlot;

static ProcSlot g_slots[LUME_SPAWN_SLOTS];
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
    if (h < 1 || h >= LUME_SPAWN_SLOTS || g_slots[h].pid <= 0) {
        *out = result(vm, false, "unknown or closed process handle");
        (void)what;
        return false;
    }
    *slot = &g_slots[h];
    return true;
}

static const char *string_of(Value v) {
    if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING)
        return obj_string(AS_OBJ(v));
    return NULL;
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
        if (s->pid <= 0 || s->waited) continue;
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
        if (g_slots[i].pid == 0) return i;
    return -1;
}

static void closefd(int *fd) {
    if (*fd >= 0) { close(*fd); *fd = -1; }
}

/* ------------------------------------------------------------------ *
 * spawn(argv, {input?}) -> {ok, handle, pid, err}
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

    const char *input = NULL;
    if (argc >= 2 && IS_OBJ(args[1]) && AS_OBJ(args[1])->type == OBJ_MAP) {
        int found = 0;
        Value in = map_get(vm, AS_OBJ(args[1]), "input", &found);
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

    int h = slot_alloc();
    if (h < 0) {
        for (int j = 0; j < n; j++) free(av[j]);
        *out = spawn_result(vm, false, 0, 0, "spawn(): process table is full");
        return;
    }

    int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0 || pipe(err_pipe) < 0) {
        closefd(&in_pipe[0]); closefd(&in_pipe[1]);
        closefd(&out_pipe[0]); closefd(&out_pipe[1]);
        closefd(&err_pipe[0]); closefd(&err_pipe[1]);
        for (int j = 0; j < n; j++) free(av[j]);
        *out = spawn_result(vm, false, 0, 0, "spawn(): pipe() failed");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        closefd(&in_pipe[0]); closefd(&in_pipe[1]);
        closefd(&out_pipe[0]); closefd(&out_pipe[1]);
        closefd(&err_pipe[0]); closefd(&err_pipe[1]);
        for (int j = 0; j < n; j++) free(av[j]);
        *out = spawn_result(vm, false, 0, 0, "spawn(): fork() failed");
        return;
    }
    if (pid == 0) {
        /* Child: no shell, no inherited watcher/HTTP fds beyond stdio. */
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        execvp(av[0], av);
        fprintf(stderr, "lume spawn: execvp failed: %s\n", strerror(errno));
        _exit(127);
    }

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

    g_slots[h].pid = pid;
    g_slots[h].in_fd = -1;
    g_slots[h].out_fd = out_pipe[0];
    g_slots[h].err_fd = err_pipe[0];
    g_slots[h].waited = false;
    g_slots[h].exit_code = -1;

    if (!g_cleanup_registered) {
        atexit(cleanup_children);
        g_cleanup_registered = true;
    }
    for (int j = 0; j < n; j++) free(av[j]);
    *out = spawn_result(vm, true, h, (long long)pid, "");
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
