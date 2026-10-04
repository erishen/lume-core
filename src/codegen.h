/* codegen.h — AST → LLVM IR (text). */

#ifndef LUME_BACKEND_CODEGEN_H
#define LUME_BACKEND_CODEGEN_H

#include <stddef.h>

struct Node;                  /* the vendored lume AST (upstream/lume.h) */

/* Emit LLVM IR text for the parsed program.
 *
 * Returns a malloc'd buffer holding the .ll text (caller frees), or NULL with
 * a human-readable message written into err. Language features the native
 * backend does not cover yet are rejected with an explicit error rather than
 * silently producing broken IR. */
char *codegen_emit_ir(const char *mod_name, struct Node *prog,
                      char *err, size_t err_size);

/* Fill in the `ret` / `params[i]` slots the source left NULL, from the call
 * sites and the `return` statements, and write the result back onto the AST.
 * No IR is emitted.
 *
 * The driver calls this once before it picks a native backend, so the libLLVM
 * walker in llvm_codegen.c and this emitter agree on one signature per name;
 * codegen_emit_ir re-runs it as a guard against a caller that skipped the
 * driver. Idempotent: every slot it fills was NULL to begin with.
 * Returns 0, or non-zero with a message in err. */
int codegen_infer_signatures(struct Node *prog, char *err, size_t err_size);

#endif /* LUMELLVM_CODEGEN_H */
