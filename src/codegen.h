/* codegen.h — AST → LLVM IR (text). */

#ifndef LUMELLVM_CODEGEN_H
#define LUMELLVM_CODEGEN_H

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

#endif /* LUMELLVM_CODEGEN_H */
