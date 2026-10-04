/* llvm_codegen.h — Lume AST → LLVM IR through the libLLVM C API. */

#ifndef LUME_BACKEND_LLVM_CODEGEN_H
#define LUME_BACKEND_LLVM_CODEGEN_H

#include <stddef.h>
#include <llvm-c/Core.h>

struct Node;                  /* the vendored lume AST (upstream/lume.h) */

/* Initialize libLLVM once — targets, asm printers. Idempotent, main-thread. */
void llvm_codegen_init_targets(void);

/* Build (and hand back) the LLVM module for an already type-checked program.
 *
 * The walker never emits IR *text*: values are built through LLVMBuild*, so
 * LLVM itself enforces the IR shape (one terminator per block, matching
 * operand types, no dangling references). That is the whole point of this
 * backend — codegen.c writes text by hand and has to enforce all of that by
 * itself.
 *
 * Returns the module on success (caller disposes it with LLVMDisposeModule),
 * or NULL with a human-readable message in err.
 *
 * The context is handed back as well, through out_ctx when out_ctx is
 * non-NULL. Each call does its own LLVMContextCreate() and the module does not
 * own that context — it only borrows it, so disposing the module leaves the
 * context and everything it owns alive. The caller disposes the context after
 * disposing the module: the module keeps its types and values inside the
 * context, so the two orders are not interchangeable. On the failure path
 * everything is already disposed here and out_ctx is left untouched, so a
 * caller that never got a module must not dispose a context it never got. */
LLVMModuleRef llvm_codegen_module(const char *mod_name, struct Node *prog,
                                  LLVMContextRef *out_ctx, char *err,
                                  size_t err_size);

/* Optimization level applied by the driver, not by this file: the module
 * comes back already verified (LLVMVerifyModule) so a bad build is reported
 * as a compile error rather than as broken machine code. */

#endif /* LUME_BACKEND_LLVM_CODEGEN_H */
