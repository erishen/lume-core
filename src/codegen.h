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

/* Multi-file variant: imported module ASTs join the emission. mods[0] is the
 * entry program; mods[1..] its imports in dependency order, nsnames[i] the
 * `as` alias the entry uses for mods[i]. Module functions are collected under
 * "<ns>.<name>" for member-call resolution but defined under their own
 * source-name symbols (which is what a member call emits against); module
 * type decls join the struct table under their plain names; module top-level
 * statements run first, inside L_top, so a module function body reads its
 * module's tables through the same @lv_ globals the entry uses. */
char *codegen_emit_ir_multi(const char *mod_name, struct Node **mods,
                            const char **nsnames, int nmods,
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

/* Multi-file variant: imported module ASTs join the signature pass so
 * cross-module member calls (`policy.decide(...)`) resolve their callee's
 * signature. mods[0] is the entry program (unresolved parameters rejected);
 * mods[1..] are its imported modules in dependency order, nsnames[i] being
 * the `as` alias the entry uses for mods[i]. Module functions are registered
 * under "<ns>.<name>"; each module's own pass runs with the unresolved-
 * parameter check disabled (its exported functions are typed by the entry's
 * call sites). Same contract as the single-program form otherwise. */
int codegen_infer_signatures_multi(struct Node **mods, const char **nsnames,
                                   int nmods, char *err, size_t err_size);

#endif /* LUMELLVM_CODEGEN_H */
