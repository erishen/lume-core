/* backend.c — toolchain side of the `native` backend.
 *
 * Shaped by two things:
 *  - runtime/src/rt.c is compiled from C so the *caller* side of a macOS/arm64
 *    variadic call (`printf`) is set up correctly; a hand-written .ll cannot
 *    do that, which is why the IR backend only ever issues ordinary
 *    non-variadic calls to helpers built here.
 *  - The IR text carries a target triple; without it LLVM lowers for a generic
 *    target, picks the wrong ABI, and even `printf("%ld", 42)` prints garbage.
 *    On macOS/arm64 that games out as "prints garbage" rather than a link
 *    error, so the triple comes from the host toolchain at build time.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backend.h"
#include "codegen.h"

/* Portable directory creation for the `build/` object dir (mirrors main.c). */
#ifdef _WIN32
#include <direct.h>
#define lume_mkdir(p) _mkdir(p)
#else
#define lume_mkdir(p) mkdir(p, 0755)
#endif

#ifndef LUME_RT_SRC
#define LUME_RT_SRC "src/rt.c"          /* fallback when built without -D */
#endif
#define RT_OBJ "build/rt.o"

static struct { char path[PATH_MAX]; struct stat st; } rt_last;

static const char *pick_cc(void)
{
    /* LUME_IR_CC is the IR compiler (must accept .ll); CC is the C compiler.
     * They split on Windows/mingw: the toolchain builds lume-core with gcc
     * (CC=gcc) but IR must go through clang, which gcc cannot parse. Makefile
     * exports LUME_IR_CC as a full Windows path there because cmd.exe cannot
     * see MSYS-style PATH entries, so the PATH probe below would silently
     * fall back to gcc and produce no .o. */
    const char *cc = getenv("LUME_IR_CC");
    if (cc && *cc) return cc;
    cc = getenv("CC");
    if (cc && *cc) return cc;
#ifdef _WIN32
    /* system() goes through cmd.exe on Windows, where `command` is not a
     * valid command, so the POSIX probe below always fails there and we'd
     * silently fall back to gcc (which cannot parse LLVM IR -> no .o ->
     * link fails). clang ships with the mingw-w64 toolchain and is exactly
     * what we need, so probe it directly with a version check cmd.exe gets. */
    if (system("clang --version >nul 2>&1") == 0) return "clang";
#else
    if (system("command -v clang >/dev/null 2>&1") == 0) return "clang";
#endif
    return "cc";
}

static int run_cmd(const char *cmd)
{
    fflush(stdout);
    int rc = system(cmd);
    if (rc != 0) fprintf(stderr, "lume: command failed: %s\n", cmd);
    return rc;
}

static int write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = strlen(data);
    size_t w = fwrite(data, 1, n, f);
    fclose(f);
    return w == n;
}

/* Compile the runtime helper once per build. Cheap to call repeatedly: the
 * object is rebuilt only when rt.c itself is newer. Backend-neutral, so both
 * the text and the libLLVM backends share it. */
int backend_build_rt(char *err, size_t err_size)
{
    struct stat st;
    snprintf(rt_last.path, sizeof rt_last.path, "%s", LUME_RT_SRC);
    if (stat(rt_last.path, &st) != 0) {
        snprintf(err, err_size, "runtime source not found: %s "
                 "(rebuild lume so that -DLUME_RT_SRC is set)", LUME_RT_SRC);
        return 1;
    }

    struct stat ost;
    int have = stat(RT_OBJ, &ost) == 0;
    if (have && ost.st_mtime >= st.st_mtime && rt_last.st.st_mtime >= st.st_mtime)
        return 0;                        /* already up to date */

    lume_mkdir("build");                /* no -p guarantee beyond this one */

    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof cmd, "%s -O2 -c -o %s %s", pick_cc(), RT_OBJ, LUME_RT_SRC);
    if (run_cmd(cmd) != 0) {
        snprintf(err, err_size, "compiling the runtime helper failed");
        return 1;
    }
    if (stat(RT_OBJ, &ost) == 0) rt_last.st = ost;
    return 0;
}

int backend_emit_ir(const char *src_path, struct Node *prog,
                    const char *ll_path, char *err, size_t err_size)
{
    if (err && err_size) err[0] = '\0';
    if (!prog) {
        snprintf(err, err_size, "native backend: no program to compile");
        return 1;
    }

    char cerr[512];
    cerr[0] = '\0';
    char *ir = codegen_emit_ir(src_path ? src_path : "lume", prog,
                               cerr, sizeof cerr);
    if (!ir) {
        /* codegen refuses rather than emitting truncated IR, so this message
         * names the real construct instead of a clang syntax error. */
        snprintf(err, err_size, "%s", cerr[0] ? cerr : "code generation failed");
        return 1;
    }
    if (!write_file(ll_path, ir)) {
        snprintf(err, err_size, "cannot write %s", ll_path);
        free(ir);
        return 1;
    }
    free(ir);
    return 0;
}

int backend_native(const char *src_path, struct Node *prog,
                   const char *out_path, char *err, size_t err_size)
{
    if (err && err_size) err[0] = '\0';

    char ll_path[PATH_MAX];
    snprintf(ll_path, sizeof ll_path, "%s.ll", out_path);
    if (backend_emit_ir(src_path, prog, ll_path, err, err_size) != 0)
        return 1;
    if (backend_build_rt(err, err_size) != 0)
        return 1;

    /* Two steps, not one: compiling the IR and linking it must be separate so
     * -Wno-override-module can be scoped to the IR step. clang ignores -target
     * when the input is .ll and always overrides the module triple with its own
     * built-in default; here that means cc's "darwin25.6.0" versus clang's
     * "macosx26.0.0" — a patch-version difference that is harmless, but it is
     * a warning on every build unless it is silenced. */
    char cmd[PATH_MAX * 3];
    snprintf(cmd, sizeof cmd, "%s -O2 -c -o %s.o %s -Wno-override-module",
             pick_cc(), out_path, ll_path);
    if (run_cmd(cmd) != 0) {
        snprintf(err, err_size, "compiling %s.ll failed", out_path);
        return 1;
    }
    /* -lm is not optional: rt.c pulls in sqrt()/pow(), which libSystem exports
     * implicitly on macOS (so this quietly worked there) but glibc does not
     * link without it. Omitting it made --compile-* fail at the link step on
     * any Linux — including the CI runner's `make asan`. */
    snprintf(cmd, sizeof cmd, "%s -O2 -o %s %s.o %s -lm",
             pick_cc(), out_path, out_path, RT_OBJ);
    if (run_cmd(cmd) != 0) {
        snprintf(err, err_size, "linking %s failed (IR kept in %s.ll)",
                 out_path, out_path);
        return 1;
    }
    return 0;
}
