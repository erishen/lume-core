/* tests/vm_reuse.c — P11 memory-safety audit: a failed/aborted run must
 * not leak VM-owned state into a subsequent run on the same VM handle.
 *
 * The CLI always tears the VM down after one script, so the shipped binary
 * never hits this — but lume.h is an embeddable API, and an embedded host
 * (agent-httpd / a long-lived worker) can call loader_run twice on one VM.
 * Regression for the two failure modes found in the audit:
 *   (a) vm->error latch: exec_program stops while vm->error is set, so a
 *       rejected script would make the next run silently no-op;
 *   (b) value-stack residue: an aborted exec leaves partial frames.
 * loader_run resets both at its entry; this test proves the contract.
 *
 * Build: cc tests/vm_reuse.c $(CORE_OBJS) -o out/vm-reuse-test   (make vm-reuse-test)
 * Run with ASAN/LSAN (make asan-vm-reuse) to also catch leaks beyond the
 * intentional arena lifetime.
 */
#include <stdio.h>
#include "../src/lume.h"

static int run_one(VM *vm, const char *path) {
    char err[512] = {0};
    return loader_run(vm, path, false, err, sizeof err);
}

int main(void) {
    VM vm;
    vm_init(&vm);
    bridge_init(&vm);

    /* 1) a must-reject fixture aborts mid-execution (type error). */
    int rc_bad = run_one(&vm, "tests/ai-bad/type-mismatch.lume");
    if (rc_bad == 0) {
        fprintf(stderr, "FAIL: bad fixture accepted by the gate fixture set\n");
        return 1;
    }

    /* 2) the SAME VM handle must run a clean program afterwards: the
     *    error latch is reset and the stack starts empty. */
    int rc_good = run_one(&vm, "examples/ai-generated/norm-stats.lume");
    if (rc_good != 0) {
        fprintf(stderr, "FAIL: good fixture rejected on a reused VM (rc=%d)\n", rc_good);
        return 1;
    }

    /* 3) a third run must work too — the reset is not a one-shot. */
    if (run_one(&vm, "examples/hello.lume") != 0) {
        fprintf(stderr, "FAIL: third run on the same VM failed\n");
        return 1;
    }

    loader_free(&vm);
    vm_free(&vm);
    type_release_all();
    printf("ok vm-reuse: failed run does not gate or poison the next run on the same VM\n");
    return 0;
}
