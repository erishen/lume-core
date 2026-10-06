/* backend_llvm.c — toolchain side of the libLLVM native backend.
 *
 * The division of labour with backend.c:
 *   - llvm_codegen.c  builds the IR through the LLVM C API (values, not text)
 *   - this file       keeps that module alive, lowers it with a target machine,
 *                     and only shells out to clang/cc for the final link
 *
 * Everything the text backend has to be careful about on its own is handled by
 * libLLVM here: the target triple comes from LLVMGetDefaultTargetTriple (no
 * -DTARGET_TRIPLE, no clang -Woverride-module fight), every instruction is
 * type-checked as it is created, and LLVMVerifyModule rejects the module before
 * codegen ever sees a bad shape.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include "backend.h"
#include "backend_llvm.h"
#include "llvm_codegen.h"

#define RT_OBJ "build/rt.o"

/* Requested by --no-pass; reset per process, so a single flag never leaks
 * from one compile into the next. */
static int g_no_pass = 0;

/* ---------------------------------------------------------------- helpers --- */

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

/* One target machine per build, from the host triple. The triple is the whole
 * reason the text backend needs -DTARGET_TRIPLE and a two-step clang: here
 * libLLVM picks it, and LLVMCreateTargetMachine is the only place it exists. */
static LLVMTargetMachineRef make_target_machine(char *err, size_t err_size)
{
    llvm_codegen_init_targets();

    char *triple = LLVMGetDefaultTargetTriple();
    if (!triple) {
        snprintf(err, err_size, "libLLVM: cannot determine the target triple");
        return NULL;
    }
    LLVMTargetRef t = NULL;
    char *msg = NULL;
    if (LLVMGetTargetFromTriple(triple, &t, &msg) != 0 || !t) {
        snprintf(err, err_size, "libLLVM: unknown target '%s'%s%s",
                 triple, msg ? ": " : "", msg ? msg : "");
        if (msg) LLVMDisposeMessage(msg);
        LLVMDisposeMessage(triple);
        return NULL;
    }

    char *cpu  = LLVMGetHostCPUName();
    char *feat = LLVMGetHostCPUFeatures();
    /* PIC + default code model, not the large code model this used to ask for.
     * On x86-64 a large code model cannot be combined with PIC — the backend
     * falls back to absolute addressing, which puts relocations in the
     * read-only text section. GNU ld reports that as
     *   `warning: relocation in read-only section '.ltext'`
     * plus `creating DT_TEXTREL in a PIE`, and native_backends.sh treats any
     * stderr from --compile-llvm as a failed emit, so the ubuntu leg went red
     * on a binary that ran correctly. ld64 never says anything, which is why
     * the darwin leg and every local run stayed green.
     *
     * PIC is also the honest choice here: the emitted object is linked by
     * clang, which builds a PIE by default on Linux, so static relocations
     * would be wrong even without the warning. The text backend goes through
     * clang with its own defaults and was never affected. */
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        t, triple, cpu ? cpu : "generic", feat ? feat : "",
        LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);
    if (cpu)  LLVMDisposeMessage(cpu);
    if (feat) LLVMDisposeMessage(feat);
    LLVMDisposeMessage(triple);
    if (!tm)
        snprintf(err, err_size, "libLLVM: cannot create a target machine");
    return tm;
}

/* The `--no-pass` switch, and the env spelling it stands in for. Probing the
 * env once keeps everybody from paying for getenv on every emit; the CLI flag
 * wins because it is spelled out and knows the run it belongs to. */
static int skip_pipeline(void)
{
    static int env_probed = -1;
    if (env_probed < 0) env_probed = getenv("LUME_NO_PASS") ? 1 : 0;
    return g_no_pass || env_probed;
}

void backend_llvm_set_no_pass(int no_pass) { g_no_pass = no_pass ? 1 : 0; }

/* --------------------------------------------------------------- emit ------ */

int backend_llvm_emit_ir(const char *src_path, struct Node *prog,
                         const char *ll_path, char *err, size_t err_size)
{
    if (err && err_size) err[0] = '\0';
    if (!prog) {
        snprintf(err, err_size, "libLLVM backend: no program to compile");
        return 1;
    }

    char cerr[512];
    cerr[0] = '\0';
    LLVMContextRef ctx = NULL;
    LLVMModuleRef mod = llvm_codegen_module(src_path ? src_path : "lume", prog,
                                            &ctx, cerr, sizeof cerr);
    if (!mod) {
        snprintf(err, err_size, "%s", cerr[0] ? cerr : "code generation failed");
        return 1;
    }

    char *ir = LLVMPrintModuleToString(mod);
    int ok = ir && write_file(ll_path, ir);
    if (ir) LLVMDisposeMessage(ir);
    if (!ok) {
        snprintf(err, err_size, "cannot write %s", ll_path);
        LLVMDisposeModule(mod);
        LLVMContextDispose(ctx);
        return 1;
    }
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    return 0;
}

int backend_llvm_native(const char *src_path, struct Node *prog,
                        const char *out_path, char *err, size_t err_size)
{
    if (err && err_size) err[0] = '\0';

    char ll_path[PATH_MAX];
    snprintf(ll_path, sizeof ll_path, "%s.ll", out_path);
    char obj_path[PATH_MAX];
    snprintf(obj_path, sizeof obj_path, "%s.o", out_path);

    char cerr[512];
    cerr[0] = '\0';
    LLVMContextRef ctx = NULL;
    LLVMModuleRef mod = llvm_codegen_module(src_path ? src_path : "lume", prog,
                                            &ctx, cerr, sizeof cerr);
    if (!mod) {
        snprintf(err, err_size, "%s", cerr[0] ? cerr : "code generation failed");
        return 1;
    }

    /* Keep the IR on disk even when codegen fails later — the same reason the
     * text backend writes out_path.ll. */
    char *ir = LLVMPrintModuleToString(mod);
    if (ir) { write_file(ll_path, ir); LLVMDisposeMessage(ir); }

    char *msg = NULL;
    LLVMTargetMachineRef tm = make_target_machine(err, err_size);
    if (!tm) {
        LLVMDisposeModule(mod);
        LLVMContextDispose(ctx);
        return 1;
    }
    /* The optimisation pipeline sits here, between codegen and lowering.
     *
     * 2026-10-02 left this out, recording that LLVMRunPasses "aborts when used
     * from a C-only TU on this libLLVM build, whatever the pass string", and
     * the alloca-heavy IR got lowered without mem2reg — which is why
     * --compile-llvm ran one to two orders of magnitude slower than
     * --compile-text. Re-probed 2026-10-03 against the real IR, and the
     * premise was wrong twice over:
     *   - the entry point is llvm-c/Transforms/PassBuilder.h, which this file
     *     already included above. The 2026-10-02 probe was standalone and did
     *     not pull it in, so LLVMRunPasses compiled as an *implicit
     *     declaration* returning int and was then stored into an LLVMErrorRef.
     *     That is what SIGSEGV'd. Nothing to do with a C-only TU: LLVMErrorRef
     *     is a pointer and the truncated int pointed nowhere.
     *   - "verify", "mem2reg" and "default<O2>" all return success at the opt
     *     level the target machine already carries (LLVMCodeGenLevelDefault).
     *   - Opt level *None*, not Default, is the one that dies (SIGSEGV). Keep
     *     the machine's Level at anything but 0.
     * A failure below is a performance detail, not a correctness one: the
     * module was already verified by LLVMVerifyModule back in
     * llvm_codegen_module, so a pipeline that refuses to run must not stop
     * the emit — it downgrades to the old behaviour and says so on stderr. */
    {
        LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
        LLVMPassBuilderOptionsSetVerifyEach(opts, 1);
        LLVMErrorRef pe = skip_pipeline() ? NULL
                                          : LLVMRunPasses(mod, "default<O2>", tm, opts);
        char *pmsg = pe ? LLVMGetErrorMessage(pe) : NULL;
        LLVMDisposePassBuilderOptions(opts);
        if (pmsg) {
            fprintf(stderr, "lume: note: libLLVM optimisation pipeline did not run "
                            "(%s); emitting the unoptimised IR\n", pmsg);
            LLVMDisposeErrorMessage(pmsg);
        } else {
            /* The .ll on disk was written above as the pipeline's *input*, so
             * refresh it with what EmitToFile is actually about to lower. */
            char *final = LLVMPrintModuleToString(mod);
            if (final) { write_file(ll_path, final); LLVMDisposeMessage(final); }
        }
    }

    if (backend_build_rt(err, err_size) != 0) {
        LLVMDisposeTargetMachine(tm);
        LLVMDisposeModule(mod);
        LLVMContextDispose(ctx);
        return 1;
    }

    /* Lowered by libLLVM, not by clang — this object file is libLLVM's. */
    /* Note the polarity: this returns 0 on success and 1 with a message on
     * failure — the reverse of the LLVMBool convention used elsewhere in the
     * C API, and the header only says "returns any error in ErrorMessage".
     * Verified both ways: a writable target writes the file and returns 0, an
     * unwritable one returns 1 with "No such file or directory" set. */
    int emit_rc = LLVMTargetMachineEmitToFile(tm, mod, obj_path, LLVMObjectFile, &msg);
    if (emit_rc != 0) {
        snprintf(err, err_size, "libLLVM codegen failed (kept in %s) [%s] errno=%d %s",
                 ll_path, msg && *msg ? msg : "(no message from libLLVM)",
                 errno, strerror(errno));
        if (msg) LLVMDisposeMessage(msg);
        LLVMDisposeTargetMachine(tm);
        LLVMDisposeModule(mod);
        LLVMContextDispose(ctx);
        return 1;
    }
    LLVMDisposeTargetMachine(tm);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);

    char cmd[PATH_MAX * 3];
    /* -lm is not optional: rt.c pulls in sqrt()/pow(), which libSystem exports
     * implicitly on macOS (so this quietly worked there) but glibc does not
     * link without it. Omitting it made --compile-* fail at the link step on
     * any Linux — including the CI runner's `make asan`. */
    snprintf(cmd, sizeof cmd, "%s -O2 -o %s %s %s -lm", pick_cc(), out_path,
             obj_path, RT_OBJ);
    if (run_cmd(cmd) != 0) {
        snprintf(err, err_size, "linking %s failed (ir kept in %s)",
                 out_path, ll_path);
        return 1;
    }
    return 0;
}
