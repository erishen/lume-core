/* lume-llvm — Lume → AST → LLVM IR → Native
 *
 * Driver. Stage 1 (current): parse + typecheck, optionally dump the AST.
 * Stage 2 will add the LLVM IR backend (see README.md).
 *
 * Host language is C11, no third-party deps beyond libc. The vendored
 * frontend under upstream/ comes from ../lume and keeps its own license
 * header (MIT, see LICENSE.upstream).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "lume.h"
#include "codegen.h"

#define OUTDIR "out"

/* Absolute path to the generated program's runtime helpers (src/rt.c),
 * injected by the Makefile so the driver never hardcodes a location. */
#ifndef LUME_RT_SRC
#define LUME_RT_SRC "src/rt.c"
#endif

/* ------------------------------------------------------------------ io ---- */

static char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);

    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);

    buf[got] = '\0';                 /* NUL-terminate; parser requires it */
    if (out_len) *out_len = got;
    return buf;
}

static int write_all(const char *path, const char *data)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(data);
    size_t w = fwrite(data, 1, n, f);
    fclose(f);
    return (w == n) ? 0 : -1;
}

/* "<dir>/<name>" without the trailing slash; used for the output paths. */
static char *stem_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;

    size_t n = strlen(base);
    while (n > 0 && base[n - 1] != '.') n--;      /* stop *at* the last '.' */
    if (n > 0) n--;                               /* drop that '.' too */

    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, base, n);
    out[n] = '\0';
    return out;
}

/* ------------------------------------------------------------- codegen ---- */

/* "<stem>.ll" / "<stem>" inside OUTDIR, malloc'd. */
static char *irpath_for(const char *stem)
{
    size_t n = strlen(stem) + sizeof(OUTDIR) + sizeof ".ll";
    char *p = (char *)malloc(n);
    if (!p) return NULL;
    snprintf(p, n, OUTDIR "/%s.ll", stem);
    return p;
}

static char *binpath_for(const char *stem)
{
    size_t n = strlen(stem) + sizeof(OUTDIR) + 1;
    char *p = (char *)malloc(n);
    if (!p) return NULL;
    snprintf(p, n, OUTDIR "/%s", stem);
    return p;
}

/* Preferred C compiler for the .ll -> native step. Overridable with CC=... */
static const char *pick_cc(void)
{
    const char *cc = getenv("CC");
    if (cc && *cc) return cc;
    if (system("command -v clang >/dev/null 2>&1") == 0) return "clang";
    return "cc";
}

/* The IR names its target triple (codegen.c), so both the runtime object and
 * the link step must be told that *same* triple. Otherwise clang silently
 * overwrites it with its own default and warns (-Woverride-module) — worse,
 * the two could disagree about the ABI. TARGET_TRIPLE is injected as a
 * quoted string literal, so "-target " TARGET_TRIPLE stays shell-safe. */
static const char *target_flag(void)
{
#ifdef TARGET_TRIPLE
    static char buf[256];
    snprintf(buf, sizeof buf, "-target " TARGET_TRIPLE " ");
    return buf;
#else
    return "";
#endif
}

static int run_cmd(const char *cmd)
{
    fflush(stdout);
    int rc = system(cmd);
    if (rc != 0) fprintf(stderr, "lume-llvm: command failed: %s\n", cmd);
    return rc;
}

/* ---------------------------------------------------------------- usage ---- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [options] <file.lume>\n"
            "\n"
            "options:\n"
            "  --check      parse + type-check only (no codegen)\n"
            "  --dump       parse + type-check, then print the AST\n"
            "  --emit-ir    parse + type-check, write LLVM IR to <file>.ll\n"
            "  --compile    parse + type-check, compile to a native binary\n"
            "  -h, --help   this message\n",
            prog);
}

/* ------------------------------------------------------------------ main --- */

int main(int argc, char **argv)
{
    const char *path = NULL;
    int mode = 0;                    /* 0=check 1=dump 2=emit-ir 3=compile */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0)      { mode = 0; continue; }
        if (strcmp(argv[i], "--dump") == 0)       { mode = 1; continue; }
        if (strcmp(argv[i], "--emit-ir") == 0)    { mode = 2; continue; }
        if (strcmp(argv[i], "--compile") == 0)    { mode = 3; continue; }
        if (strcmp(argv[i], "-h") == 0 ||
            strcmp(argv[i], "--help") == 0)       { usage(argv[0]); return 0; }
        if (argv[i][0] == '-' && argv[i][1])      { usage(argv[0]); return 2; }
        if (path)                                 { usage(argv[0]); return 2; }
        path = argv[i];
    }

    if (!path) { usage(argv[0]); return 2; }

    size_t len = 0;
    char *src = slurp(path, &len);
    if (!src) {
        fprintf(stderr, "lume-llvm: cannot read '%s'\n", path);
        return 2;
    }

    char err[1024];

    Node *prog = parse_program(src, err, sizeof err);
    if (!prog) {
        fprintf(stderr, "lume-llvm: parse error: %s\n", err);
        free(src);
        return 1;
    }

    if (!type_check_program(prog, err, sizeof err)) {
        fprintf(stderr, "lume-llvm: type error: %s\n", err);
        free(src);
        return 1;
    }

    if (mode == 1) {                 /* --dump: just show the AST */
        node_print(prog, 0);
        goto done;
    }

    if (mode == 0) {                 /* --check: stop after the type checker */
        printf("ok: %s\n", path);
        goto done;
    }

    /* ---- codegen: mode 2 = .ll text, mode 3 = .ll then native binary ---- */
    char cgerr[1024] = "";
    char *stem = stem_of(path);
    if (!stem) { fprintf(stderr, "lume-llvm: out of memory\n"); goto done; }

    char *ir = codegen_emit_ir(stem, prog, cgerr, sizeof cgerr);
    if (!ir) {
        fprintf(stderr, "lume-llvm: codegen error: %s\n",
                cgerr[0] ? cgerr : "unsupported construct in source");
        goto done;
    }

    mkdir(OUTDIR, 0755);
    char *llpath = irpath_for(stem);

    if (write_all(llpath, ir) != 0) {
        fprintf(stderr, "lume-llvm: cannot write %s\n", llpath);
        free(ir); goto done;
    }
    printf("wrote %s (%zu bytes of IR)\n", llpath, strlen(ir));

    if (mode == 2) { free(ir); goto done; }        /* --emit-ir stops here */

    /* --- --compile: .ll -> native binary via the C compiler --- */
    char *binpath = binpath_for(stem);
    char cmd[1024];

    /* The print helpers in src/rt.c have to be compiled and linked in:
     * the generated IR calls them, and there is no other definition. */
    mkdir(OUTDIR, 0755);
    snprintf(cmd, sizeof cmd, "%s -O2 %s-c -o %s/lume_rt.o %s",
             pick_cc(), target_flag(), OUTDIR, LUME_RT_SRC);
    if (run_cmd(cmd) != 0) {
        fprintf(stderr, "lume-llvm: cannot build the runtime object\n");
        free(ir); goto done;
    }

    /* -Wno-override-module: when clang compiles LLVM IR it rewrites the module
     * triple to its own SDK default (here arm64-apple-darwin25.6.0 in the .ll
     * vs ...macosx26.0.0 in clang) and warns. The rewrite is harmless — same
     * arch, vendor and OS — and -target already pin the ABI, so keep it quiet. */
    snprintf(cmd, sizeof cmd, "%s -O2 %s-Wno-override-module -o %s %s %s/lume_rt.o",
             pick_cc(), target_flag(), binpath, llpath, OUTDIR);
    printf("linking: %s\n", cmd);
    if (run_cmd(cmd) != 0) {
        fprintf(stderr, "lume-llvm: native compile failed\n");
        free(ir); goto done;
    }
    printf("wrote %s\n", binpath);

    free(ir);
    goto done;

done:
    (void)len;
    if (path) { /* nothing further to free: src and prog live for the run */ }
    free(src);
    return 0;
}
