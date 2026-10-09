#include "lume.h"
#include "backend.h"
#include "codegen.h"          /* codegen_infer_signatures: shared pass */
#include "builtins_internal.h" /* list_push: vm.argv seeding */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/event.h>
#endif

/* Lume entry point: parse + type check + interpret a script, then hand the
 * process over to agenthttpd_run() when the script calls run(). */

#ifdef HAVE_LIBLLVM
#include "backend_llvm.h"
#endif

/* Native backends the CLI can ask for. --compile picks the default, which is
 * the libLLVM one when the binary was built with libLLVM (LLVM then verifies
 * the IR shape while it is built); --compile-text is the opt-out. */
enum { NAT_DEFAULT = 0, NAT_LLVM = 1, NAT_TEXT = 2 };

/* Portable directory creation. POSIX mkdir takes a mode; Windows _mkdir takes
 * only the path (mode is ignored). Used by the --compile output dir. */
#ifdef _WIN32
#include <direct.h>
#define lume_mkdir(p) _mkdir(p)
#else
#define lume_mkdir(p) mkdir(p, 0755)
#endif

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [--check|--dump|--watch|--compile|--compile-llvm|--compile-text|--mcp|--lsp] [--no-fs] [--no-net] [--no-pass] <script.lume>\n"
            "  --check   parse + type check (no side effects)\n"
"  --no-fs   runtime filesystem lock: read_file/write_file/files/\n"
"            mkdir/lock_file fail at runtime instead of touching\n"
"            the disk (also settable as LUME_NO_FS=1)\n"
"  --no-net runtime network lock: http_get() fails instead of\n"
"            opening a socket (also settable as LUME_NO_NET=1)\n"
"  --version print version and exit\n"
"  --dump    parse and dump the AST, then exit\n"
#ifdef HAVE_LIBLLVM
            "  --compile     default native path: ast -> IR built through the\n"
            "                libLLVM C API (LLVM checks its shape while building),\n"
            "                lowered by libLLVM itself; keeps <out>.ll alongside\n"
            "  --compile-llvm same as --compile in this build (libLLVM is default)\n"
            "  --compile-text force the text path: ast -> IR text -> clang\n"
            "  --no-pass     skip the libLLVM optimisation pipeline and lower the\n"
            "                IR exactly as it was generated (same as setting\n"
            "                LUME_NO_PASS=1; `make native-bench` times both)\n"
            "  -o PATH       output binary (default out/<stem>)"
#else
            "  --compile     native path: ast -> IR text -> clang\n"
            "  --compile-text same path, spelled out (built without libLLVM)\n"
            "  --compile-llvm refused here: needs libLLVM (rebuild with\n"
            "                LLVM_CONFIG pointing at llvm-config)\n"
            "  --no-pass     no-op here: this build lowers the IR text straight\n"
            "                through clang, so there is no pipeline to skip\n"
            "  -o PATH       output binary (default out/<stem>)"
#endif
            "\n"
            "  --watch   dev hot reload: validate + restart the child on\n"
            "            every edit of <script.lume> (SIGUSR1 = force reload);\n"
            "            invalid edits keep the old process running\n"
            "\n"
            "The script is Lume source: type/struct declarations, server {},\n"
            "route/tool declarations plus expressions with static type\n"
            "checking; this tree has no embedded server: server{} is parsed\n"
            "and kept inert, and a script that calls run() exits non-zero\n",
            prog);
}

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n && n != 0) { fclose(f); free(buf); return NULL; }
    buf[n] = '\0';
    fclose(f);
    if (len_out) *len_out = (size_t)n;
    return buf;
}

/* ---------- --watch: dev hot reload ------------------------------------
 *
 * The serving process cannot swap its VM in place: routes/tools are
 * registered into two pre-fork tables (agent-httpd g_routes + VM routes[])
 * and `g_framework_started` seals registration once run() begins. So hot
 * reload works at process level instead:
 *
 *   parent (watcher)                 child (serving process)
 *   ┌────────────────────┐           ┌──────────────────────┐
 *   │ kqueue(VNODE)/poll │  edit     │ bin/lume <script>    │
 *   │ self-pipe (SIGUSR1)│ ───────►  │  parse+typecheck+run │
 *   │ validate new edit  │           │  agenthttpd_run      │
 *   │ SIGTERM child      │◄───────── │  (graceful shutdown) │
 *   │ fork+exec new child│  ~350ms   └──────────────────────┘
 *   └────────────────────┘  debounce
 *
 * Invalid edits are reported and the old child keeps serving; a valid edit
 * restarts the child (port is freed by the child's graceful shutdown
 * before the next child binds). SIGUSR1 forces the same restart path. */

#ifndef _WIN32   /* --watch needs fork/exec/kqueue, unavailable on Windows */

#define WATCH_POLL_MS       500
#define WATCH_DEBOUNCE_MS   350
#define WATCH_STOP_GRACE_MS 3000

static volatile sig_atomic_t g_watch_quit = 0;
static volatile sig_atomic_t g_watch_usr1 = 0;
static int g_watch_pipe[2] = {-1, -1};

static void watch_signal(int sig) {
    if (sig == SIGUSR1) g_watch_usr1 = 1;
    else g_watch_quit = 1;
    if (g_watch_pipe[1] >= 0) {
        ssize_t w = write(g_watch_pipe[1], "x", 1);
        (void)w;
    }
}

/* Hand a finished VM's whole object graph back. Order is deliberate:
 *   loader_free()  drops the modules and the AST (node_free recurses), which
 *                  is what ObjFunc/ObjNative bodies point into;
 *   vm_free()      empties the GC heap and the strdup'd route records;
 *   type_release_all() sweeps the Type graph shared by the AST, the checker
 *                  scopes and the module export tables.
 * Exposed as one function so no exit path can forget a step. */
static void vm_teardown(VM *vm) {
    loader_free(vm);
    vm_free(vm);
    type_release_all();
}

/* parse + type check without running; err[] gets the first failure. Goes
 * through the module loader so scripts with `import` validate correctly
 * (dependencies type-checked, cycle detection active). Each validation is
 * now released — see vm_teardown. */
static bool watch_validate(const char *script, char *err, size_t errsz) {
    VM vm;
    vm_init(&vm);
    bridge_init(&vm);
    bool ok = loader_run(&vm, script, true, err, errsz) == 0;
    vm_teardown(&vm);
    return ok;
}

static void file_sig(const char *path, struct timespec *mtime, long long *size) {
    struct stat st;
    if (stat(path, &st) != 0) { mtime->tv_sec = -1; *size = -1; return; }
#ifdef __APPLE__
    *mtime = st.st_mtimespec;
#else
    mtime->tv_sec = st.st_mtim.tv_sec;
    mtime->tv_nsec = st.st_mtim.tv_nsec;
#endif
    *size = (long long)st.st_size;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* SIGTERM, wait up to WATCH_STOP_GRACE_MS, escalate to SIGKILL. */
static void stop_child(pid_t child) {
    if (child <= 0) return;
    kill(child, SIGTERM);
    int status = 0;
    long long deadline = now_ms() + WATCH_STOP_GRACE_MS;
    for (;;) {
        pid_t r = waitpid(child, &status, WNOHANG);
        if (r == child) return;
        if (now_ms() >= deadline) break;
        usleep(50 * 1000);
    }
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) { }
}

static pid_t start_child(const char *argv0, const char *script) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { perror("[watch] fork"); return -1; }
    if (pid == 0) {
        /* child: fresh address space via exec; watcher fds must not leak */
        if (g_watch_pipe[0] >= 0) close(g_watch_pipe[0]);
        if (g_watch_pipe[1] >= 0) close(g_watch_pipe[1]);
        execl(argv0, argv0, script, (char *)NULL);
        fprintf(stderr, "[watch] exec %s failed: %s\n", argv0, strerror(errno));
        _exit(127);
    }
    printf("[watch] server child pid=%d (script=%s)\n", pid, script);
    fflush(stdout);
    return pid;
}

static int run_watch(const char *argv0, const char *script) {
    struct timespec sigmask_mtime;
    long long sigsize = -1;
    file_sig(script, &sigmask_mtime, &sigsize);
    if (sigsize < 0) {
        fprintf(stderr, "lume: cannot read %s\n", script);
        return 1;
    }

    char err[512] = {0};
    if (!watch_validate(script, err, sizeof(err))) {
        fprintf(stderr, "lume: %s\n", err);
        return 1;
    }

    if (pipe(g_watch_pipe) != 0) { perror("[watch] pipe"); return 1; }
    fcntl(g_watch_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_watch_pipe[1], F_SETFL, O_NONBLOCK);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = watch_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int kq = -1, watch_fd = -1;
#if defined(__APPLE__)
    kq = kqueue();
    if (kq >= 0) {
        struct kevent ev;
        EV_SET(&ev, (uintptr_t)g_watch_pipe[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
        kevent(kq, &ev, 1, NULL, 0, NULL);
        watch_fd = open(script, O_RDONLY | O_EVTONLY);
        if (watch_fd >= 0) {
            EV_SET(&ev, (uintptr_t)watch_fd, EVFILT_VNODE,
                   EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_DELETE | NOTE_RENAME |
                                     NOTE_EXTEND | NOTE_ATTRIB,
                   0, NULL);
            kevent(kq, &ev, 1, NULL, 0, NULL);
        }
    }
#endif

    printf("[watch] watching %s (SIGUSR1 = force reload, Ctrl+C = stop)\n",
           script);
    fflush(stdout);

    pid_t child = start_child(argv0, script);
    long long last_change = now_ms();
    long long last_restart = 0;
    long long last_msg = 0;
    int pending = 0;

    while (!g_watch_quit) {
        /* 1) file content changed? (ground truth: stat, not just events —
         *    editors rename/replace the inode under us) */
        struct timespec mt;
        long long sz;
        file_sig(script, &mt, &sz);
        if (sz >= 0 && (mt.tv_sec != sigmask_mtime.tv_sec ||
                        mt.tv_nsec != sigmask_mtime.tv_nsec || sz != sigsize)) {
            sigmask_mtime = mt;
            sigsize = sz;
            last_change = now_ms();
            pending = 1;
        }

        /* 2) act on a pending change / SIGUSR1 (debounced) */
        long long now = now_ms();
        if ((pending || g_watch_usr1) && now - last_change >= WATCH_DEBOUNCE_MS &&
            now - last_restart >= WATCH_DEBOUNCE_MS) {
            g_watch_usr1 = 0;
            pending = 0;
            last_change = now;
            if (!watch_validate(script, err, sizeof(err))) {
                if (now - last_msg > 2000) {
                    fprintf(stderr,
                            "[watch] invalid edit, old server keeps running:\n"
                            "  %s\n", err);
                    last_msg = now;
                }
            } else {
                stop_child(child);
                last_restart = now_ms();
                child = start_child(argv0, script);
            }
        }

        /* 3) reap an unexpectedly dead child (crash / run() failure) */
        if (child > 0) {
            int status = 0;
            pid_t r = waitpid(child, &status, WNOHANG);
            if (r == child) {
                if (!g_watch_quit && now - last_msg > 1000) {
                    fprintf(stderr,
                            "[watch] child pid=%d exited (status %d); waiting "
                            "for next edit or SIGUSR1\n",
                            (int)child, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                    last_msg = now;
                }
                child = 0;
            }
        }

        /* 4) sleep until the next event / file touch / poll tick */
#if defined(__APPLE__)
        if (kq >= 0) {
            struct kevent ev;
            struct timespec to = {0, WATCH_POLL_MS * 1000000L};
            int n = kevent(kq, NULL, 0, &ev, 1, &to);
            if (n > 0 && ev.filter == EVFILT_READ) {
                char drain[16];
                while (read(g_watch_pipe[0], drain, sizeof(drain)) > 0) { }
            } else if (n > 0 && ev.filter == EVFILT_VNODE) {
                /* re-arm on the (possibly replaced) file so NOTE_* keeps firing */
                close(watch_fd);
                watch_fd = open(script, O_RDONLY | O_EVTONLY);
                if (watch_fd >= 0) {
                    struct kevent reg;
                    EV_SET(&reg, (uintptr_t)watch_fd, EVFILT_VNODE,
                           EV_ADD | EV_CLEAR,
                           NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND |
                           NOTE_ATTRIB,
                           0, NULL);
                    kevent(kq, &reg, 1, NULL, 0, NULL);
                }
            }
        } else {
            usleep(WATCH_POLL_MS * 1000);
        }
#else
        usleep(WATCH_POLL_MS * 1000);
#endif
    }

    printf("[watch] stopping child pid=%d\n", (int)child);
    fflush(stdout);
    stop_child(child);
    if (watch_fd >= 0) close(watch_fd);
    if (kq >= 0) close(kq);
    close(g_watch_pipe[0]);
    close(g_watch_pipe[1]);
    return 0;
}

#endif /* _WIN32 guard around --watch */

#ifdef _WIN32
/* Windows has no fork/exec/kqueue, so --watch is compiled out; but the
 * normal run/check/compile paths still tear the VM down here. */
static void vm_teardown(VM *vm) {
    loader_free(vm);
    vm_free(vm);
    type_release_all();
}
#endif

int main(int argc, char **argv) {
    bool do_check = false;
    bool do_dump = false;
    bool do_watch = false;
    bool do_compile = false;
    int  native_choice = NAT_DEFAULT;
    const char *out_path = NULL;
    const char *script = NULL;
    /* Script trailing CLI arguments (everything after the first non-flag
     * argument); seeded into vm.argv so scripts can read them via argv(). */
    const char *cli_args[64];
    int cli_argc = 0;
    bool no_fs = false;
    bool no_net = false;
    /* Accepted in both builds: --no-pass is a no-op without libLLVM, and
     * refusing the flag there would only break scripts that pass it. It is
     * read on the text path too (as a note), so neither build sees it unused. */
    bool no_pass = false;
    bool do_mcp = false;
    bool do_lsp = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) do_check = true;
        else if (strcmp(argv[i], "--no-fs") == 0) no_fs = true;
        else if (strcmp(argv[i], "--no-net") == 0) no_net = true;
        else if (strcmp(argv[i], "--dump") == 0) do_dump = true;
        else if (strcmp(argv[i], "--watch") == 0) do_watch = true;
        else if (strcmp(argv[i], "--compile") == 0) do_compile = true;
        else if (strcmp(argv[i], "--compile-llvm") == 0) {
            do_compile = true;
            native_choice = NAT_LLVM;
        }
        else if (strcmp(argv[i], "--compile-text") == 0) {
            do_compile = true;
            native_choice = NAT_TEXT;
        }
        else if (strcmp(argv[i], "--no-pass") == 0) no_pass = true;
        else if (strcmp(argv[i], "--mcp") == 0) do_mcp = true;
        else if (strcmp(argv[i], "--lsp") == 0) do_lsp = true;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out_path = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("lume-core %s\n", LUME_CORE_VERSION);
            return 0;
        }
        else if (argv[i][0] != '-') {
            if (!script) script = argv[i];      /* first non-flag arg = script */
            else if (cli_argc < 64) cli_args[cli_argc++] = argv[i];
        }
        else { usage(argv[0]); return 2; }
    }
    if (!script && !do_lsp) { usage(argv[0]); return 2; }

    /* LUME_NO_FS is the same switch as --no-fs, so a supervisor can lock the
     * language down without rewriting the argv it execs. Anything but the
     * documented "off" spellings counts as on, which errs towards locking. */
    const char *no_fs_env = getenv("LUME_NO_FS");
    /* An empty value counts as unset: `LUME_NO_FS=` is far more likely to come
     * from a stray export than to mean "lock it down". */
    if (no_fs_env && no_fs_env[0] && strcmp(no_fs_env, "0") != 0 &&
        strcmp(no_fs_env, "off") != 0 && strcmp(no_fs_env, "false") != 0) {
        no_fs = true;
    }

    /* Same env spelling as --no-net (see LUME_NO_FS above for the rationale:
     * a supervisor locks the language down without rewriting argv). */
    const char *no_net_env = getenv("LUME_NO_NET");
    if (no_net_env && no_net_env[0] && strcmp(no_net_env, "0") != 0 &&
        strcmp(no_net_env, "off") != 0 && strcmp(no_net_env, "false") != 0) {
        no_net = true;
    }

#ifndef _WIN32
    if (do_watch) return run_watch(argv[0], script);
#else
    if (do_watch) {
        fprintf(stderr, "lume: --watch is not supported on Windows\n");
        return 2;
    }
#endif

    if (do_dump) {
        /* single-file AST dump (imports are resolved by the loader, which is
         * not needed to inspect one file's tree) */
        size_t len = 0;
        char *source = read_file(script, &len);
        if (!source) {
            fprintf(stderr, "lume: cannot read %s\n", script);
            return 1;
        }
        char derr[512] = {0};
        Node *prog = parse_program(source, derr, sizeof(derr));
        if (!prog) {
            fprintf(stderr, "lume: %s\n", derr[0] ? derr : "parse error");
            free(source);
            return 1;
        }
        node_print(prog, 0);
        node_free(prog);        /* the dump is the parser's only consumer */
        free(source);
        type_release_all();     /* the parser builds Type nodes of its own */
        return 0;
    }

    VM vm;
    vm_init(&vm);
    vm.no_fs = no_fs;
    vm.no_net = no_net;
    bridge_init(&vm);

    /* Seed argv() with the script's trailing CLI arguments (after the script
     * name). Rooted on the VM stack while filling, then reachable via the
     * vm.argv field (marked by gc_collect). */
    if (cli_argc > 0) {
        vm.argv = AS_OBJ(make_list(&vm));
        vm_push(&vm, val_obj(vm.argv));
        for (int i = 0; i < cli_argc; i++)
            list_push(&vm, vm.argv, make_string_cstr(&vm, cli_args[i]));
        vm_pop(&vm);
    }

    /* --lsp: no entry script needed — the language server serves the whole
     * workspace and checks documents as they are opened (parse+typecheck),
     * with hover/completion over the builtin table. */
    if (do_lsp) {
        lsp_run(&vm);
        vm_teardown(&vm);
        return 0;
    }

    /* Multi-file import/export: the loader parses + type-checks the entry
     * script and every module it imports (dependencies first), then executes
     * module top levels in dependency order, the entry's last (entry env ==
     * vm->globals, so builtins and route/tool registrations keep working). */
    char err[512] = {0};
    /* --compile stops after type checking as well: it wants the checked AST,
     * not an interpreted run. The entry module registers itself first (see
     * load_module in loader.c), so modules[0] is the script being built. */
    if (loader_run(&vm, script, do_check || do_compile, err, sizeof(err)) != 0) {
        fprintf(stderr, "lume: %s\n", err[0] ? err : "load error");
        vm_teardown(&vm);
        return 1;
    }

    if (do_check) {
        printf("parse OK (%s)\n", script);
        vm_teardown(&vm);
        return 0;
    }

    /* --mcp: the script (already run, so `tool ...` statements registered their
     * handlers) becomes an MCP stdio server: JSON-RPC on stdin/stdout. */
    if (do_mcp) {
        mcp_run(&vm);
        vm_teardown(&vm);
        return 0;
    }

    if (do_compile) {
        Module *entry = (vm.modules && vm.module_count) ? vm.modules[0] : NULL;
        if (!entry || !entry->prog) {
            fprintf(stderr, "lume: native backend: no entry module to compile\n");
            vm_teardown(&vm);
            return 1;
        }
        /* One signature pass for both native backends: unannotated parameters
         * and return types are worked out here, on the AST, so the libLLVM
         * walker and the IR-text emitter cannot resolve the same call to two
         * different signatures. */
        char ierr[512] = {0};
        if (codegen_infer_signatures(entry->prog, ierr, sizeof ierr) != 0) {
            fprintf(stderr, "lume: %s\n", ierr[0] ? ierr : "cannot infer a signature");
            vm_teardown(&vm);
            return 1;
        }

        char bin[PATH_MAX];
        if (out_path) {
            snprintf(bin, sizeof bin, "%s", out_path);
        } else {
            const char *tail = strrchr(script, '/');
            tail = (tail ? tail + 1 : script);
            size_t tl = strlen(tail);
            if (tl >= 5 && strcmp(tail + tl - 5, ".lume") == 0)
                tl -= 5;                  /* ".lume" is five characters */
            snprintf(bin, sizeof bin, "out/%.*s", (int)tl, tail);
            lume_mkdir("out");
        }
        char berr[512] = {0};
        /* Default is the libLLVM backend when this binary has it: the IR is
         * built as LLVM values, so LLVMVerifyModule rejects a malformed shape
         * right where it is produced. A build without libLLVM, and any
         * --compile-text request, land on the text backend (ast -> IR text ->
         * clang) instead. */
        /* Declared inside the guard on purpose: without libLLVM there is no
         * choice to make and the variable only trips -Wunused-variable. */
#ifdef HAVE_LIBLLVM
        int use_llvm = (native_choice != NAT_TEXT);
#else
        /* --compile-llvm on a build without libLLVM used to fall through to the
         * text backend silently: the caller asked for the libLLVM path and got
         * the text one with only a note on stderr. Refuse instead — a silent
         * backend switch is worse than a loud failure. */
        if (native_choice == NAT_LLVM) {
            fprintf(stderr, "lume: --compile-llvm needs a build with libLLVM "
                            "(this one has HAVE_LIBLLVM off); use --compile-text, "
                            "or rebuild with LLVM_CONFIG=<llvm-config>\n");
            /* Refused after vm_init(), so the VM is live here: returning
             * without tearing it down leaked the whole heap (every builtin
             * native, the globals env, the loaded module table) on a path that
             * exits without ever producing a binary. */
            vm_teardown(&vm);
            return 1;
        }
        if (native_choice != NAT_TEXT)
            fprintf(stderr, "lume: note: built without libLLVM, using the IR text backend\n");
        /* The flag is accepted everywhere, so it has to say so exactly where
         * it cannot do anything — a silently ignored switch is a lie. */
        if (no_pass)
            fprintf(stderr, "lume: note: --no-pass ignored: this build has no "
                            "libLLVM pipeline to skip\n");
#endif
        /* Guarded as a whole, not just use_llvm = 0: without HAVE_LIBLLVM the
         * backend itself is not linked in, so the call below must not survive
         * into the translation unit at all (otherwise the build dies on
         * -Wimplicit-function-declaration and "no libLLVM" would not build). */
#ifdef HAVE_LIBLLVM
        if (use_llvm) {
            if (no_pass) {
                backend_llvm_set_no_pass(1);
                fprintf(stderr, "lume: note: --no-pass: lowering the IR without "
                                "running the optimisation pipeline\n");
            }
            if (backend_llvm_native(script, entry->prog, bin, berr, sizeof(berr)) != 0) {
                fprintf(stderr, "lume: %s\n", berr[0] ? berr : "libLLVM compile failed");
                vm_teardown(&vm);
                return 1;
            }
            printf("wrote %s (libLLVM backend)\n", bin);
            /* No note printed here. The default used to carry one, because
             * this path could not run LLVM's optimisation passes and a
             * compute-bound binary came out one to two orders of magnitude
             * slower than --compile-text. That gap closed 2026-10-03: this
             * path now runs the same default<O2> pipeline the text path gets
             * from clang, and `make native-bench` puts the two in the same
             * band (5ms vs 9ms on the compute-bound example). What still
             * explains the default is the other half of the trade — the IR is
             * built by LLVM itself, so LLVMVerifyModule rejects a bad shape
             * at the point of generation, rather than the text emitter having
             * to hold every invariant by hand and discover it at clang. */
            vm_teardown(&vm);
            return 0;
        }
#endif
        if (backend_native(script, entry->prog, bin, berr, sizeof(berr)) != 0) {
            fprintf(stderr, "lume: %s\n", berr[0] ? berr : "native compile failed");
            vm_teardown(&vm);
            return 1;
        }
        printf("wrote %s (IR text backend)\n", bin);
        vm_teardown(&vm);
        return 0;
    }

    if (vm.error) {
        fprintf(stderr, "lume: %s\n", vm.error_msg);
        vm_teardown(&vm);
        return 1;
    }

    /* run() hands the process to agent-httpd. A script that never calls it
     * is a pure language program (compute + print) — that is fine, just not
     * a server. */
    if (!vm.run_called)
        fprintf(stderr, "lume: note: script completed without run()\n");
    int status = vm.main_status;   /* a top-level main() is the exit status */
    vm_teardown(&vm);
    return status;
}