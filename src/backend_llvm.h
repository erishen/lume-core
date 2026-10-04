/* backend_llvm.h — the second `native` backend: Lume AST → libLLVM-built IR →
 * native binary, with libLLVM doing the codegen (not clang).
 *
 * Where backend.c hands an .ll buffer to clang, this file keeps the module in
 * memory the whole way through: the IR is assembled through the LLVM C API in
 * llvm_codegen.c, verified there, and lowered to an object file by
 * LLVMTargetMachineEmitToFile. clang only performs the final link.
 *
 * Both backends are built into one binary; `--compile` picks the text backend
 * and `--compile-llvm` this one. A build without libLLVM on the host simply
 * leaves this translation unit (and llvm_codegen.c) out and keeps --compile.
 */

#ifndef LUME_BACKEND_LLVM_H
#define LUME_BACKEND_LLVM_H

#include <stddef.h>

struct Node;                  /* lume's AST (lume.h) */

/* Ask for the optimisation pipeline to be skipped: the module is lowered
 * exactly as llvm_codegen.c built it, which is what `--no-pass` and the
 * LUME_NO_PASS=1 env var (a `make native-bench` convenience) both mean.
 * Set before an entry point runs; it only affects that run. */
void backend_llvm_set_no_pass(int no_pass);

/* Write the IR built through libLLVM to `ll_path`. Returns 0 on success,
 * non-zero with a message in `err`. */
int backend_llvm_emit_ir(const char *src_path, struct Node *prog,
                         const char *ll_path, char *err, size_t err_size);

/* Build the module, lower it to an object file with libLLVM, link against the
 * runtime helper and place the executable at `out_path`; `out_path.ll` keeps
 * the IR so a failing build can still be read. Returns 0 on success. */
int backend_llvm_native(const char *src_path, struct Node *prog,
                        const char *out_path, char *err, size_t err_size);

#endif /* LUME_BACKEND_LLVM_H */
