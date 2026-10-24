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

#ifndef LUME_NATIVE_SRC
#define LUME_NATIVE_SRC "src/bridge_native.c"
#endif
/* The interpreter's own build also produces build/bridge_native.o (without
 * the sql block — it only needs the serve{} runtime), so the native backends
 * compile their own copy under a distinct name. Without this split the
 * backend would link the interpreter's sql-free object and the sql_* calls
 * would dangle as undefined symbols. */
#define NATIVE_OBJ "build/bridge_native_sql.o"
/* mingw has no system regex library; rt.c's lume_str_match_* resolve to the
 * compat regex objects the lume build itself ships (regcomp/regexec/...),
 * so the helper link must include them wherever the runtime object is used. */
#define REGEX_OBJS "build/compat/regex/regcomp.o build/compat/regex/regexec.o " \
                   "build/compat/regex/regerror.o build/compat/regex/tre-mem.o"

static struct { char path[PATH_MAX]; struct stat st; } rt_last;

static const char *pick_cc(void)
{
    const char *cc = getenv("CC");
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

/* The compiler for the IR-text steps. CC may legitimately point at gcc (the
 * C build's compiler), which cannot parse .ll — so this probes clang (or a
 * clang-compatible driver such as `zig cc`) first and only then falls back
 * to CC. The rt.o / bridge_native.o steps are plain C and keep pick_cc(). */
static const char *pick_ir_cc(void)
{
#ifdef _WIN32
    /* system() goes through cmd.exe on Windows, where `command -v` is not
     * a valid command; probe clang the way cmd.exe can (see pick_cc). */
    if (system("clang --version >nul 2>&1") == 0) return "clang";
#else
    if (system("command -v clang >/dev/null 2>&1") == 0) return "clang";
#endif
    const char *cc = getenv("CC");
    if (cc && *cc) return cc;
    return "cc";
}

/* Cross-compilation: LUME_TARGET (e.g. x86_64-windows-gnu) is passed to every
 * compile/link step and switches the link libraries (winsock vs -lm). */
static const char *xt_target(void)
{
    const char *xt = getenv("LUME_TARGET");
    return (xt && *xt) ? xt : NULL;
}

static int xt_windows(void)
{
    const char *xt = xt_target();
    return xt && strstr(xt, "windows") != NULL;
}

/* Cross-compiling writes the helper objects under separate names so a
 * windows COFF never overwrites (and later breaks) the native objects. */
static const char *rt_obj_path(void)
{
    return xt_target() ? "build/rt_win.o" : RT_OBJ;
}

static const char *nat_obj_path(void)
{
    return xt_target() ? "build/bridge_native_win.o" : NATIVE_OBJ;
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

/* -I path to the compat headers living beside a backend runtime source
 * (e.g. src/compat/regex for src/rt.c). mingw has no system <regex.h>;
 * both native runtime helpers include it via the compat layer, so the
 * helper compile command needs the same include the lume build itself uses. */
static void src_compat_inc(char *buf, size_t n, const char *src)
{
    const char *slash = NULL;
    for (const char *q = src; *q; q++)
        if (*q == '/' || *q == '\\') slash = q;
    if (slash && (size_t)(slash - src) < n - 12)
        snprintf(buf, n, "%.*s/compat/regex", (int)(slash - src), src);
    else
        snprintf(buf, n, "src/compat/regex");
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
    int have = stat(rt_obj_path(), &ost) == 0;
    /* Cross-compiling rebuilds unconditionally: build/rt.o is a native
     * object and a windows .o must not be mixed into a posix link. */
    if (!xt_target() && have && ost.st_mtime >= st.st_mtime &&
        rt_last.st.st_mtime >= st.st_mtime)
        return 0;                        /* already up to date */

    lume_mkdir("build");                /* no -p guarantee beyond this one */

    char cmd[PATH_MAX * 2];
    /* Cross-compiling a C helper needs a clang-like driver too: CC may point
     * at gcc, which does not accept -target. */
    const char *cc = xt_target() ? pick_ir_cc() : pick_cc();
    /* mingw has no system <regex.h>; lume ships its own next to rt.c
     * (src/compat/regex), and the runtime uses it (lume_str_match_*), so the
     * helper compile must see the same compat include the lume build uses. */
    char rinc[PATH_MAX];
    src_compat_inc(rinc, sizeof rinc, LUME_RT_SRC);
    snprintf(cmd, sizeof cmd, "%s -O2%s%s -I%s -c -o %s %s",
             cc, xt_target() ? " -target " : "",
             xt_target() ? xt_target() : "",
             rinc, rt_obj_path(), LUME_RT_SRC);
    if (run_cmd(cmd) != 0) {
        snprintf(err, err_size, "compiling the runtime helper failed");
        return 1;
    }
    if (stat(RT_OBJ, &ost) == 0) rt_last.st = ost;
    return 0;
}

/* Compile the compat regex objects rt.c's lume_str_match_* needs, next to
 * LUME_RT_SRC (src/compat/regex/…). The lume build links these into the
 * binary itself, but an installed lume-core has no prebuilt objects — the
 * gate must compile them on the fly, cached by mtime like backend_build_rt. */
int backend_build_regex(char *err, size_t err_size)
{
    char dir[PATH_MAX];
    src_compat_inc(dir, sizeof dir, LUME_RT_SRC);   /* <srcdir>/compat/regex */
    static const char *srcs[] = { "regcomp.c", "regexec.c", "regerror.c", "tre-mem.c" };
    lume_mkdir("build");               /* single-level mkdir: make each */
    lume_mkdir("build/compat");        /* layer by hand, no -p guarantee */
    lume_mkdir("build/compat/regex");
    for (size_t i = 0; i < sizeof srcs / sizeof srcs[0]; i++) {
        char src[PATH_MAX], obj[PATH_MAX];
        if (snprintf(src, sizeof src, "%s/%s", dir, srcs[i]) >= (int)sizeof src) {
            snprintf(err, err_size, "compat regex source path too long: %s", srcs[i]);
            return 1;
        }
        if (snprintf(obj, sizeof obj, "build/compat/regex/%.*s.o",
                     (int)(strlen(srcs[i]) - 2), srcs[i]) >= (int)sizeof obj) {   /* strip ".c" */
            snprintf(err, err_size, "compat regex object path too long: %s", srcs[i]);
            return 1;
        }
        struct stat s, o;
        if (stat(src, &s) != 0) {
            snprintf(err, err_size, "compat regex source not found: %s", src);
            return 1;
        }
        if (stat(obj, &o) == 0 && o.st_mtime >= s.st_mtime) continue;
        char cmd[PATH_MAX * 2];
        if (snprintf(cmd, sizeof cmd, "%s -O2 -I%s -c %s -o %s",
                     pick_cc(), dir, src, obj) >= (int)sizeof cmd) {
            snprintf(err, err_size, "build command too long for %s", srcs[i]);
            return 1;
        }
        if (run_cmd(cmd) != 0) {
            snprintf(err, err_size, "compiling %s failed", srcs[i]);
            return 1;
        }
    }
    return 0;
}

/* Compile the native server runtime (bridge_native.c) once per build,
 * rebuilt when the source is newer. Backend-neutral, like backend_build_rt:
 * both native backends link it for lume_srv_*, lume_bi_now and lume_bi_env. */
int backend_build_native(char *err, size_t err_size)
{
    struct stat st;
    if (stat(LUME_NATIVE_SRC, &st) != 0) {
        snprintf(err, err_size, "native runtime source not found: %s "
                 "(rebuild lume so that -DLUME_NATIVE_SRC is set)",
                 LUME_NATIVE_SRC);
        return 1;
    }

    struct stat ost;
    int have = stat(nat_obj_path(), &ost) == 0;
    if (!xt_target() && have && ost.st_mtime >= st.st_mtime)
        return 0;                        /* already up to date */

    lume_mkdir("build");

    char cmd[PATH_MAX * 2];
    const char *cc = xt_target() ? pick_ir_cc() : pick_cc();
    char ninc[PATH_MAX];
    src_compat_inc(ninc, sizeof ninc, LUME_NATIVE_SRC);
#if LUME_HAS_SQLITE
    /* bridge_native.c carries the native sql_* implementations, which
     * #include <sqlite3.h>; the include path and the LUME_HAS_SQLITE flag
     * come from the lume-core build (Makefile, SQLITE_PREFIX). */
    snprintf(cmd, sizeof cmd, "%s -O2%s%s -I%s -c -o %s %s %s -DLUME_HAS_SQLITE=1",
             cc, xt_target() ? " -target " : "",
             xt_target() ? xt_target() : "",
             ninc, nat_obj_path(), LUME_NATIVE_SRC, LUME_SQLITE_INC);
#else
    snprintf(cmd, sizeof cmd, "%s -O2%s%s -I%s -c -o %s %s",
             cc, xt_target() ? " -target " : "",
             xt_target() ? xt_target() : "",
             ninc, nat_obj_path(), LUME_NATIVE_SRC);
#endif
    if (run_cmd(cmd) != 0) {
        snprintf(err, err_size, "compiling the native server runtime failed");
        return 1;
    }
    return 0;
}

int backend_emit_ir(const char *src_path, struct Node *prog,
                    struct Node **mods, const char ***alias_list,
                    const int *alias_cnt, int nmods,
                    const char *ll_path, char *err, size_t err_size)
{
    if (err && err_size) err[0] = '\0';
    if (!prog) {
        snprintf(err, err_size, "native backend: no program to compile");
        return 1;
    }

    char cerr[512];
    cerr[0] = '\0';
    char *ir;
    if (nmods > 1 && mods)
        ir = codegen_emit_ir_multi(src_path ? src_path : "lume", mods,
                                   alias_list, alias_cnt, nmods, cerr,
                                   sizeof cerr);
    else
        ir = codegen_emit_ir(src_path ? src_path : "lume", prog,
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
                   struct Node **mods, const char ***alias_list,
                   const int *alias_cnt, int nmods,
                   const char *out_path, char *err, size_t err_size)
{
    if (err && err_size) err[0] = '\0';

    char ll_path[PATH_MAX];
    snprintf(ll_path, sizeof ll_path, "%s.ll", out_path);
    if (backend_emit_ir(src_path, prog, mods, alias_list, alias_cnt,
                        nmods, ll_path, err, err_size) != 0)
        return 1;
    if (backend_build_rt(err, err_size) != 0)
        return 1;
    if (backend_build_native(err, err_size) != 0)
        return 1;
    if (backend_build_regex(err, err_size) != 0)
        return 1;

    /* Two steps, not one: compiling the IR and linking it must be separate so
     * -Wno-override-module can be scoped to the IR step. clang ignores -target
     * when the input is .ll and always overrides the module triple with its own
     * built-in default; here that means cc's "darwin25.6.0" versus clang's
     * "macosx26.0.0" — a patch-version difference that is harmless, but it is
     * a warning on every build unless it is silenced. */
    char cmd[PATH_MAX * 3];
    snprintf(cmd, sizeof cmd, "%s -O2%s%s -c -o %s.o %s -Wno-override-module",
             pick_ir_cc(), xt_target() ? " -target " : "",
             xt_target() ? xt_target() : "",
             out_path, ll_path);
    if (run_cmd(cmd) != 0) {
        snprintf(err, err_size, "compiling %s.ll failed", out_path);
        return 1;
    }
    /* -lm is not optional: rt.c pulls in sqrt()/pow(), which libSystem exports
     * implicitly on macOS (so this quietly worked there) but glibc does not
     * link without it. Omitting it made --compile-* fail at the link step on
     * any Linux — including the CI runner's `make asan`. */
    char outexe[PATH_MAX * 2];
    const char *oname = out_path;
    if (xt_windows() && !strstr(out_path, ".exe")) {
        snprintf(outexe, sizeof outexe, "%s.exe", out_path);
        oname = outexe;
    }
    int win_link = xt_windows();
#ifdef _WIN32
    win_link = 1;                        /* native windows build: winsock */
#endif
#if LUME_HAS_SQLITE
    /* sqlite lib dir comes as its own macro (whitespace-free by contract);
     * empty means the compiler's default paths (-lsqlite3 alone). */
    snprintf(cmd, sizeof cmd, "%s -O2%s%s -o %s %s.o %s %s %s %s %s%s -lsqlite3",
             pick_ir_cc(), xt_target() ? " -target " : "",
             xt_target() ? xt_target() : "",
             oname, out_path, rt_obj_path(), nat_obj_path(),
             REGEX_OBJS, win_link ? "-lws2_32" : "-lm",
             LUME_SQLITE_LIBDIR[0] ? "-L" : "",
             LUME_SQLITE_LIBDIR[0] ? LUME_SQLITE_LIBDIR : "");
#else
    snprintf(cmd, sizeof cmd, "%s -O2%s%s -o %s %s.o %s %s %s %s",
             pick_ir_cc(), xt_target() ? " -target " : "",
             xt_target() ? xt_target() : "",
             oname, out_path, rt_obj_path(), nat_obj_path(),
             REGEX_OBJS, win_link ? "-lws2_32" : "-lm");
#endif
    if (run_cmd(cmd) != 0) {
        snprintf(err, err_size, "linking %s failed (IR kept in %s.ll)",
                 out_path, out_path);
        return 1;
    }
    return 0;
}
