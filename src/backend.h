/* backend.h — the `native` backend: Lume AST → LLVM IR text → native binary.
 *
 * The frontend (lexer / parser / typecheck) is lume's own; the codegen*.c
 * tu's own the AST → IR step (codegen.c is the entry, see its header), and
 * this file owns the toolchain step (write .ll, compile the runtime helper,
 * link) plus the entry points the CLI calls.
 *
 * IR is emitted as text and finished off by clang/cc, so lume itself never
 * links libLLVM — a ~2 MB binary stays a ~2 MB binary.
 */

#ifndef LUME_BACKEND_H
#define LUME_BACKEND_H

#include <stddef.h>

struct Node;                  /* lume's AST (lume.h) */

/* Write LLVM IR text for an already type-checked program to `ll_path`.
 * Returns 0 on success, non-zero with a message written into `err`.
 * mods/nsnames/nmods are the multi-file form (see codegen_emit_ir_multi):
 * pass NULL/0 for a single-file compile, or an array whose [0] is `prog`
 * with imports in dependency order and nsnames[i] their `as` aliases. */
int backend_emit_ir(const char *src_path, struct Node *prog,
                    struct Node **mods, const char **nsnames, int nmods,
                    const char *ll_path, char *err, size_t err_size);

/* Emit IR, compile the runtime helper, and link a native executable at
 * `out_path`. The IR is written next to it as `out_path.ll` so a failing
 * compile can be inspected. Returns 0 on success, non-zero with `err` set.
 * Multi-file form as in backend_emit_ir. */
int backend_native(const char *src_path, struct Node *prog,
                   struct Node **mods, const char **nsnames, int nmods,
                   const char *out_path, char *err, size_t err_size);

/* Internal, shared by both native backends: compile src/rt.c into build/rt.o
 * once per build (rebuilt only when the source is newer). The IR backends need
 * the same helpers the text backend does — they call them, they do not
 * reimplement the "has this been built yet" bookkeeping. */
int backend_build_rt(char *err, size_t err_size);

/* Same bookkeeping for the native server runtime (src/bridge_native.c →
 * build/bridge_native.o), which carries lume_srv_*, lume_bi_now and
 * lume_bi_env. Rebuilt only when the source is newer; both native backends
 * must link it, not just the text backend. */
int backend_build_native(char *err, size_t err_size);

#endif /* LUME_BACKEND_H */
