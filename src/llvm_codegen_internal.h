/* llvm_codegen_internal.h - shared declarations for the LLVM backend.
 * Included by llvm_codegen.c and (textually) by its .inc fragments.
 * Mirrors codegen_internal.h on the text backend side. This backend's
 * CG/Val/helpers are deliberately private and NOT shared with codegen_internal.h.
 */
#ifndef LUME_LLVM_CODEGEN_INTERNAL_H
#define LUME_LLVM_CODEGEN_INTERNAL_H

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm/Config/llvm-config.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lume.h"
#include "llvm_codegen.h"

/* ---- backend-private types (from the original monolithic file) ---- */
typedef struct { char *name; Type *ty; LLVMValueRef slot; size_t line;
                 int cap;        /* cap: slot points into the closure's
                                  * capture array, not a local alloca */
                 int shadow; } Asg;  /* shadows a module global: seed at entry */
typedef struct { Asg *v; int n, cap; } Asgs;

/* A name whose type is already known but which has no slot yet — the
 * match-arm binds live here only while their arm body's type is inferred. */
typedef struct { char *name; Type *ty; } LTyp;
typedef struct { LTyp *v; int n, cap; } LTys;

/* Shared with the signature pass (codegen_sig.c): linear lookup by name. */
Type *lt_find(LTys *t, const char *name);

/* Mirrors codegen_internal.h's Sig field-for-field: llvm_cg_scan.inc shares
 * codegen_scan.c's infer_node_type logic verbatim, and that logic reads
 * `node` (the lambda a Sig was registered from) and `phmask` (placeholder
 * pinning). A leaner struct here compiled until 8ac8450 started mirroring
 * the HOF-element fix into llvm_cg_scan.inc. The extra fields (pnames,
 * body, is_lambda) are carried for parity so the next mirrored change does
 * not have to touch this struct again — sig_push below must set every one. */
typedef struct { char *name; Type *ret; Type **params; char **pnames;
                 int arity; struct Node *body; struct Node *node;
                 int is_lambda;
                 /* Bit i set: params[i] is a placeholder pinned from an
                  * any/empty list element type (INT), not a real element
                  * type — a later fixed-point round may upgrade it. */
                 unsigned int phmask; } Sig;
typedef struct { Sig *v; int n, cap; } Sigs;

typedef struct { char *name; char **names; Type **types; int count; } SDef;
typedef struct { SDef *v; int n, cap; } SDefs;

#define MAX_STRUCT_TYPES 64
typedef struct { char name[128]; LLVMTypeRef ty; } StructEnt;

typedef struct {
    LLVMContextRef    ctx;
    LLVMModuleRef     mod;
    LLVMBuilderRef    ab;          /* the moving builder                     */
    LLVMValueRef      fn;          /* function whose body is being emitted   */
    LLVMBasicBlockRef cur;         /* block the builder points into          */
    LLVMTypeRef       i1, i64, dbl, i8ptr, void_ty, i32;

    Asgs   locals;                 /* variables of the function emitted now  */
    Asgs   gvars;                  /* top-level bindings lifted to module
                                    * globals (SPEC 8.1 #10): a function body
                                    * that misses in `locals` reads here      */
    Sigs   sigs;                   /* every top-level func                   */
    SDefs  structs;                /* every `type X = {...}`                 */
    Type  *expect;                 /* type the surrounding context wants     */
    Type  *cur_ret;                /* return type of the function emitted    */
    const char *cur_ns;            /* alias of the module whose function is
                                    * being emitted (multi-file build): a plain
                                    * call inside a module body first resolves
                                    * "<ns>.<name>", then the bare name. NULL
                                    * for the entry / single file. */

    /* Multi-file build: every alias any loaded module uses for each module
     * (alias_list[m] has alias_cnt[m] entries; alias_cnt[0] ignored). Lets a
     * member read (`b.factor`) resolve a module alias to its module's
     * top-level binding. NULL for a single-file build. */
    const char ***alias_list;
    const int *alias_cnt;
    int        nmods;
    StructEnt structs_types[MAX_STRUCT_TYPES];
    int    structs_types_n;

    /* Set while the driver pre-scans / emits the synthetic L_top: the scan
     * then lifts every top-level `let` and for-in variable into `gvars` (an
     * LLVM global) instead of a fresh alloca, and cg_def reuses the
     * pre-scanned table instead of scanning the body a second time. */
    int    scanning_top;
    Asgs  *preset_locals;

    /* Match-arm binds, for the window their arm body's type is inferred in
     * (the infer below reads it as a last resort); empty outside that
     * window, released when the outermost match is done. Mirrors the field
     * of the same name in codegen_internal.h's CG. */
    LTys   arm_scope;

    char   err[1024];
    int    tid;

    LLVMBasicBlockRef *brk; int nbrk, cbrk;   /* enclosing loop exits      */
    LLVMBasicBlockRef *cnt; int ncnt, ccnt;   /* enclosing loop heads      */

    /* While-statement body-scope marks: scan records locals.n before the
     * body's shadow `let`s are registered, emit re-hides them while the
     * condition is generated (the condition sits in the enclosing scope;
     * a same-line shadow would otherwise capture its reads — interp reads
     * the outer value at condition time). Indexed by while-statement
     * occurrence, so nested whiles pair up scan and emit entries. */
    int   *wh_saved; int wh_cap, wh_idx;

    /* Synthetic top-level body emitting `main()`: the process status is the
     * value that call returns, so remember it (exit_val) and hand it to the
     * fallback terminator in cg_function — the top level has no explicit
     * `return`, but here it answers for the program's exit code. */
    int          capture_main;
    LLVMValueRef exit_val;

    /* Native server routes (mirrors codegen_internal.h's CG): while a route
     * handler body is emitted, `in_handler` makes N_RETURN serialise into
     * the FFI response pointers (hout/hlen/hjson = the handler's p1/p2/p3),
     * and `hreq` names the handler's req parameter. `hrid` sequences route
     * ids for the L_route_N symbols. */
    int           in_handler;
    const char   *hreq;
    int           hrid;
    LLVMValueRef  hout, hlen, hjson;
} CG;

typedef struct { LLVMValueRef v; Type *ty; } Val;

/* ---- compile-time limits ---- */
#define AT(g)  LLVMPositionBuilderAtEnd((g)->ab, (g)->cur)

/* A block is done exactly once: LLVM refuses instructions after a terminator,
 * so a `br` out of a block that already returned has to be skipped. */
#define DONE(bb) (LLVMGetBasicBlockTerminator(bb) != NULL)

/* First error wins: the root-cause message must not be overwritten by a
 * downstream one. `cg_print` walks the same `g->err` buffer, so without this
 * guard a genuine "X does not apply to floats" raised in `cg_binary` got
 * clobbered by a generic "print() cannot print this value" once codegen
 * returned a typeless value to it — see §8.1 #1. `g->err` is zeroed by the
 * memset in llvm_codegen_module, so the guard reads cleanly on a fresh pass. */
#define ERR(g, fmt, ...)                                                       \
    do {                                                                       \
        if ((g)->err[0] == '\0')                                               \
            snprintf((g)->err, sizeof (g)->err, fmt, ##__VA_ARGS__);           \
        return NULL;                                                           \
    } while (0)

#define ERRV(g, fmt, ...)                                                      \
    do {                                                                       \
        if ((g)->err[0] == '\0')                                               \
            snprintf((g)->err, sizeof (g)->err, fmt, ##__VA_ARGS__);           \
        return (Val){ NULL, NULL };                                            \
    } while (0)

#define ERRX(g, fmt, ...)                                                      \
    do {                                                                       \
        if ((g)->err[0] == '\0')                                               \
            snprintf((g)->err, sizeof (g)->err, fmt, ##__VA_ARGS__);           \
        return;                                                                \
    } while (0)

#define MATCH_BIND_MAX 32

/* Lume builtins that lower onto a runtime helper (src/rt.c).
 * The int flavour is fn/pty/rty, the float one fn_f/pty_f/rty_f (NULL when
 * the builtin is not overloaded). */
typedef struct {
    const char *name;
    const char *fn_i, *pty_i, *rty_i; int np_i;
    const char *fn_f, *pty_f, *rty_f; int np_f;
} Bi;

static const Bi BUILTINS[] = {
    { "abs",   "lume_bi_abs",  "i64",    "i64",    1,  NULL,            NULL,          NULL,     0 },
    { "min",   "lume_bi_min",  "i64",    "i64",    2,  "lume_bi_minf",  "double",      "double", 2 },
    { "max",   "lume_bi_max",  "i64",    "i64",    2,  "lume_bi_maxf",  "double",      "double", 2 },
    { "sqrt",  NULL,           NULL,     NULL,     0,  "lume_bi_sqrt",  "double",      "double", 1 },
    { "pow",   NULL,           NULL,     NULL,     0,  "lume_bi_pow",   "double",      "double", 2 },
    { "floor", NULL,           NULL,     NULL,     0,  "lume_bi_floor", "double",      "double", 1 },
    { "ceil",  NULL,           NULL,     NULL,     0,  "lume_bi_ceil",  "double",      "double", 1 },
    { "round", NULL,           NULL,     NULL,     0,  "lume_bi_round", "double",      "double", 1 },
    /* Statistical distribution builtins (P4, 2026-10-20), float-only. Kept
     * in lockstep with codegen_call.c's BUILTINS. */
    { "normcdf",   NULL, NULL, NULL, 0, "lume_bi_normcdf",   "double", "double", 1 },
    { "norminv",   NULL, NULL, NULL, 0, "lume_bi_norminv",   "double", "double", 1 },
    { "erf",       NULL, NULL, NULL, 0, "lume_bi_erf",       "double", "double", 1 },
    { "gamma",     NULL, NULL, NULL, 0, "lume_bi_gamma",     "double", "double", 1 },
    { "lgamma",    NULL, NULL, NULL, 0, "lume_bi_lgamma",    "double", "double", 1 },
    { "igamma",    NULL, NULL, NULL, 0, "lume_bi_igamma",    "double", "double", 2 },
    { "ibeta",     NULL, NULL, NULL, 0, "lume_bi_ibeta",     "double", "double", 3 },
    { "tdist_cdf", NULL, NULL, NULL, 0, "lume_bi_tdist_cdf", "double", "double", 2 },
    { "chisq_cdf", NULL, NULL, NULL, 0, "lume_bi_chisq_cdf", "double", "double", 2 },
};

/* ---- static prototypes (one definition per TU; declared here) ---- */
static void *xmalloc(size_t n);
static void *xrealloc(void *p, size_t n);
static char *xstrdup(const char *s);
static void asgs_free(Asgs *a);
static void asg_truncate(Asgs *a, int n);
static void asg_push(Asgs *a, const char *name, Type *ty, LLVMValueRef slot,
                     size_t line);
/* Like asg_push but marks the binding as a capture (slot materialized from
 * the closure context in cg_def); used for phase-2 captured names. */
static void asg_push_cap(Asgs *a, const char *name, Type *ty,
                         LLVMValueRef slot, size_t line);
/* Like asg_push but marks the binding as a body local that shadows a
 * module global (scan's N_ASSIGN arm); the entry block seeds it from the
 * global so the first read matches the interpreter's env lookup. */
static void asg_push_shadow(Asgs *a, const char *name, Type *ty,
                            LLVMValueRef slot, size_t line);
static Asg *asg_find(Asgs *a, const char *name, size_t use_line);
static Asg *gvar_find(Asgs *a, const char *name);
static void sig_push(Sigs *s, const char *name, Type *ret, Type **params, int arity);
static Sig *sig_find(Sigs *s, const char *name);
static void sdef_push(SDefs *d, const char *name, char **names, Type **types, int count);
static SDef *sdef_find(SDefs *d, const char *name);
static void sigs_free(Sigs *s);
static void sdefs_free(SDefs *d);
static void push_bb(LLVMBasicBlockRef **v, int *n, int *cap, LLVMBasicBlockRef bb);
static LLVMTypeRef struct_ty(CG *g, const char *name);
static LLVMTypeRef ty_of(CG *g, Type *t);
static LLVMTypeRef ptr_of(CG *g, Type *t);
static LLVMTypeRef ty_from_spelling(CG *g, const char *s);
static LLVMValueRef cg_string_val(CG *g, const char *text, size_t len);
static Val val_make(Type *ty, LLVMValueRef v);
static const char *node_type_name(NodeType t);
static const char *bad(CG *g, Node *n, const char *what);
static Val cg_literal(CG *g, Node *n);
static Val cg_var(CG *g, Node *n);
static Val cg_unary(CG *g, Node *n);
static LLVMValueRef cg_val_eq(CG *g, Val a, Val b, bool ne, size_t line);
static LLVMValueRef cg_struct_eq(CG *g, Val a, Val b, bool ne, size_t line);
static Val cg_binary(CG *g, Node *n);
static Val cg_len_of(CG *g, const char *fn, LLVMValueRef obj);
static int struct_field_idx(CG *g, const char *sname, const char *fname);
static Type *struct_field_type(CG *g, const char *sname, int idx);
static LLVMValueRef gep_field(CG *g, Type *sty, LLVMValueRef ptrv, int idx);
static void struct_addr(CG *g, Val *o);
static Val cg_member(CG *g, Node *n);
static const char *map_get_fn(Type *ty);
static const char *list_push_fn(Type *ty);
static const char *map_put_fn(Type *ty);
/* from typecheck.c — spells a Type for diagnostics; this TU does not include
 * typecheck_internal.h, so the declaration lives here. */
const char *ty_str(Type *t);
static Val rt_call(CG *g, Type *rty, const char *fn, int n,
                   LLVMTypeRef *ats, LLVMValueRef *vs);
static Type *infer_list_elem(CG *g, Node *lit);
static Type *list_type(void);
static Val cg_list_lit(CG *g, Node *n);
static Val cg_map_lit(CG *g, Node *n);
static Val cg_map_or_struct_lit(CG *g, Node *n);
static Val cg_struct_update(CG *g, Node *n);
static Val cg_assign_expr(CG *g, Node *n);
static Type *resolve_struct_lit(CG *g, Node *n);
static Val cg_struct_lit(CG *g, Node *n);
static LLVMValueRef rt_decl(CG *g, const char *name, LLVMTypeRef ret,
                            LLVMTypeRef *params, int n);
static void cg_route_handler(CG *g, Node *fn, int rid);
static void cg_handler_ret(CG *g, Node *n);
static Val cg_handler_req_field(CG *g, Node *n);
static Val cg_print(CG *g, Node *n, Val a);
static Val emit_struct_to_map(CG *g, LLVMValueRef sv, Type *st);
static const Bi *builtin_find(const char *name);
static Val cg_funclit(CG *g, Node *n);
static LLVMValueRef cg_clo_invoke(CG *g, Node *lambda, LLVMValueRef clo,
                                  LLVMValueRef *args, int argc);
static Val cg_clo_call(CG *g, Node *n, Node *lambda);
static Node *hof_lambda(CG *g, Node *a);
static Val cg_builtin(CG *g, Node *n);
static int struct_is_addr(LLVMValueRef v);
static Val cg_call(CG *g, Node *n);
static Val cg_expr(CG *g, Node *n);
static Val cg_index(CG *g, Node *n);
static Val cg_ternary(CG *g, Node *n);
static Val cg_ctor(CG *g, Node *n);
static Val cg_match(CG *g, Node *n);
static void cg_if_let(CG *g, Node *n);
static Type *member_field_type(CG *g, Node *n);
static void arm_scope_push(CG *g, const char *name, Type *ty);
static void arm_scope_clear(CG *g);
static Type *infer_node_type(CG *g, Node *n);
static Type *for_in_elem_type(CG *g, Node *n);
/* from llvm_cg_scan.inc: the one value shape a struct's fields share, or
 * NULL when they disagree. The get() scan case and emit_get_call
 * (llvm_cg_call.inc) both answer with it, so it is forward-declared here
 * for the call sites that precede the .inc (static: this header is only
 * ever included by llvm_codegen.c, the one translation unit that defines
 * it). */
static Type *struct_common_shape(Type *t);
static LLVMValueRef gvar_make(CG *g, const char *name, Type *ty);
static void scan_stmt(CG *g, Node *n);
static void scan_block(CG *g, Node *blk);
static Val coerce(CG *g, Type *to, Val v, size_t line);
static void br_to(CG *g, LLVMBasicBlockRef target);
static Val cg_assign_mem(CG *g, Node *n);
static void cg_while(CG *g, Node *n);
static const char *list_at_fn(Type *ty);
static void cg_for_in(CG *g, Node *n);
static void cg_for(CG *g, Node *n);
static void cg_if(CG *g, Node *n);
static void cg_stmt(CG *g, Node *n);
static void cg_block(CG *g, Node *blk);
static void cg_def(CG *g, const char *sym, char **names, Type **params,
                   int arity, Type *ret, Node *body, int has_cap,
                   Node *capfn, size_t line);
static void cg_function(CG *g, Node *fn);
static void cg_closure(CG *g, Node *fn);
void llvm_codegen_init_targets(void);
static void collect_lambdas(Node *n, Node ***out, int *cnt, int *cap);
LLVMModuleRef llvm_codegen_module(const char *mod_name, Node *prog,
                                  LLVMContextRef *out_ctx, char *err, size_t err_size);

#endif /* LUME_LLVM_CODEGEN_INTERNAL_H */
