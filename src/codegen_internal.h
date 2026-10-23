/* codegen_internal.h — what the codegen*.c tu's share.

 * The text backend used to be one 2782-line file; it is cut along its own
 * section boundaries (see the note at the top of codegen.c). The context, the
 * error macros and the allocator helpers belong to all of them; a function is
 * visible here exactly when some other tu calls it.

 * Not an installed header: it is private to the text backend, and Makefile's
 * INT_HDRS picks it up so every dependent object rebuilds when it changes.
 */

#ifndef LUME_CODEGEN_INTERNAL_H
#define LUME_CODEGEN_INTERNAL_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codegen.h"
#include "lume.h"
#include "irbuf.h"

/* An IR value: a type plus the SSA name holding it, and an `agg` flag for the
 * case where that name is a struct value rather than the address of one. */
typedef struct {
    char *v;
    Type *ty;
    int   agg;
} Val;

/* The checker's ANY singleton. */
Type *any_type(void);

/* ------------------------------------------------------------- alloc --- */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

#define EMIT(g, ...) irbuf_printf((g)->ir, __VA_ARGS__)

/* Report an error into g->err and bail out. Three variants because C has no
 * way to "return whatever this function returns": ERR for pointer results,
 * ERRV for Val, ERRX for void.
 *
 * The *first* error wins: a later one overwriting the earlier text is what hid
 * "cannot infer a type for 'm'" behind "variable 'k' has no codegen type" from
 * an unrelated pass, which cost a round of guessing. */
#define ERR_SET(g, fmt, ...)                                                   \
    do {                                                                       \
        if (!(g)->err[0])                                                      \
            snprintf((g)->err, sizeof (g)->err, fmt, ##__VA_ARGS__);           \
    } while (0)

#define ERR(g, fmt, ...)                                                       \
    do {                                                                       \
        ERR_SET(g, fmt, ##__VA_ARGS__);                                        \
        return NULL;                                                           \
    } while (0)

#define ERRV(g, fmt, ...)                                                      \
    do {                                                                       \
        ERR_SET(g, fmt, ##__VA_ARGS__);                                        \
        return (Val){ NULL, NULL, 0 };                                          \
    } while (0)

#define ERRX(g, fmt, ...)                                                      \
    do {                                                                       \
        ERR_SET(g, fmt, ##__VA_ARGS__);                                        \
        return;                                                                \
    } while (0)

/* ------------------------------------------------------------ codegen ctx --- */

typedef struct { char *name; Type *ty; char *slot; size_t line;
                 int shadow; } Asg;   /* shadows a @lv_ global: seed from
                                       * it in the entry block */
typedef struct { Asg *v; int n, cap; } Asgs;

/* A name whose type the signature pass already knows, and the table it is
 * built in: the emitter has not allocated a slot for any of them yet. */
typedef struct { char *name; Type *ty; } LTyp;
typedef struct { LTyp *v; int n, cap; } LTys;

/* `ret` is NULL for a function with no `return`, and so is a `params[i]` that
 * the source left unannotated — the signature pass fills those in from the
 * call sites / return statements, and only reports at the end if it could
 * not. `body` and `pnames` are what make that possible. */
typedef struct { char *name; Type *ret; Type **params; char **pnames;
                 int arity; struct Node *body; struct Node *node;
                 int is_lambda;
                 /* Bit i set: params[i] is a placeholder pinned from an
                  * any/empty list element type (INT), not a real element
                  * type — a later fixed-point round may upgrade it. */
                 unsigned int phmask; } Sig;
typedef struct { Sig *v; int n, cap; } Sigs;

typedef struct {
    char  *name;
    char **names;
    Type **types;
    int    count;
} SDef;
typedef struct { SDef *v; int n, cap; } SDefs;

typedef struct {
    IrBuf *ir;              /* body: struct types + function definitions   */
    IrBuf  gs;             /* globals: string constants, flushed up front */
    char   err[1024];

    LTys   scope;           /* names the signature pass has typed so far; only
                             * set while that pass runs, then unused */
    LTys   arm_scope;       /* match-arm binds while their arm body's type is
                             * being inferred (both emitters' infer reads it
                             * as a last resort); empty outside that window,
                             * released when the outermost match is done */
    LTys   globals;         /* top-level `let` types, built once by the
                             * signature pass and copied into every body's
                             * scope — a lambda body calling another lambda
                             * (`g = (x) => inc(x)`) resolves the callee
                             * through this, since `inc` is not a local of
                             * the body being walked */

    Node  *prog;            /* the whole program, so the signature pass can
                             * walk the top-level calls as well as the bodies */

    Sigs   sigs;            /* every top-level func, collected before emitting */
    SDefs  structs;         /* every `type X = {...}` declaration            */
    Asgs   locals;          /* variables of the function currently emitted   */
    Asgs   gvars;           /* top-level bindings, lifted to module globals:
                             * a function body that misses in `locals` reads
                             * the name here (SPEC 8.1 #10). Filled by the
                             * driver's pre-scan of the top-level statements,
                             * which is also the scan L_top reuses.            */
    Sig   *cur;             /* signature of the function currently emitted   */
    Type  *expect;          /* type the expression context wants (struct lits) */
    const char *cur_ns;     /* alias of the module whose function is being
                             * emitted (multi-file build): a plain call inside a
                             * module body first resolves "<ns>.<name>", then
                             * the bare name. NULL for the entry / single file. */

    /* Multi-file build: every alias any loaded module uses for each module
     * (alias_list[m] has alias_cnt[m] entries; alias_cnt[0] ignored). Lets a
     * member read (`b.factor`) resolve a module alias to its module's
     * top-level binding. NULL for a single-file build. */
    const char ***alias_list;
    const int *alias_cnt;
    int        nmods;

    /* Set while the driver pre-scans / emits the synthetic L_top: the scan
     * then lifts every top-level `let` and for-in variable into `gvars` (an
     * @lv_ global) instead of a fresh stack slot, and cg_def reuses the
     * pre-scanned table instead of scanning the body a second time. */
    int    scanning_top;
    Asgs  *preset_locals;

    int    lid;             /* label id counter            */
    int    tid;             /* temp/id counter            */
    int    sid;             /* string constant counter    */
    int    nclo;            /* closure mangled-name counter */
    int    mod_idx;         /* module index in the multi-file build: synthetic
                             * closure cnames carry it so __lume_clo_* never
                             * collides across files (0 = entry / single file,
                             * which keeps the historic __lume_clo_N spelling) */

    int   *brk; int nbrk, cbrk;    /* stack of enclosing loop labels */
    int   *cnt; int ncnt, ccnt;

    /* While-statement body-scope marks: scan records locals.n before the
     * body's shadow `let`s are registered, emit re-hides them while the
     * condition is generated (the condition sits in the enclosing scope;
     * a same-line shadow would otherwise capture its reads — interp reads
     * the outer value at condition time). Indexed by while-statement
     * occurrence, so nested whiles pair up scan and emit entries. */
    int   *wh_saved; int wh_cap, wh_idx;

    int    capture_main;           /* emitting the synthetic top-level body:
                                    * remember what `main()` returned */
    char   exit_tmp[48];

    /* Native server: set while compiling a route handler body. `hreq` names
     * the handler's request parameter; member access on it (req.path,
     * req.query_params, ...) is lowered straight onto the SrvReq FFI
     * arguments instead of the type system. `hrid` numbers the handlers. */
    int    in_handler;
    const char *hreq;
    int    hrid;

    /* Handler definitions are buffered here instead of being emitted into the
     * function currently open (L_top): LLVM IR forbids a `define` inside a
     * function body, so route handlers wait until L_top has closed. */
    IrBuf  handlers;
} CG;

/* ------------------------------------------------------------- the emitters --- */

Asg *asg_find(Asgs *a, const char *name, size_t use_line);
Asg *gvar_find(Asgs *a, const char *name);
void asg_push(Asgs *a, const char *name, Type *ty, const char *slot,
              size_t line);
/* Like asg_push but marks the binding as a function-body local that
 * shadows a top-level global (scan's N_ASSIGN arm): the entry block seeds
 * it from the @lv_ global so the body's first read matches the
 * interpreter's env lookup instead of uninitialized stack. */
void asg_push_shadow(Asgs *a, const char *name, Type *ty, const char *slot,
                     size_t line);
void asgs_free(Asgs *a);
void asg_truncate(Asgs *a, int n);
const char *bad(Node *n, const char *what);
Val cg_assign_expr(CG *g, Node *n);
Val cg_assign_mem(CG *g, Node *n);
Val cg_expr(CG *g, Node *n);
/* The statement dispatcher (codegen_stmt.c), called by cg_if_let's
 * then/else branches. */
void cg_stmt(CG *g, Node *n);
void cg_if_let(CG *g, Node *n);
void cg_let_pat(CG *g, Node *n);   /* `let { a, b } = e;` — codegen_expr.c */
char *cg_string_val(CG *g, const char *text, size_t len);
void cg_function(CG *g, Node *fn);
void cg_closure(CG *g, Node *fn);
/* The list element accessor table, shared between the for-in emitter
 * (codegen_stmt.c) and the map/filter/reduce emitters (codegen_expr.c):
 * one dispatch, so a type that iterates is a type the HOFs can also read. */
const char *list_at_fn(Type *ty);
Type *elem_rty(Type *ty);
int codegen_infer_signatures(Node *prog, char *err, size_t err_size);
int codegen_infer_signatures_multi(Node **mods, const char ***alias_list,
                                   const int *alias_cnt, int nmods,
                                   char *err, size_t err_size);
Val coerce(CG *g, Type *to, Val v, size_t line);
void emit_builtin_declares(CG *g);
char *emit_instrf(CG *g, Type *ty, const char *fmt, ...);
Type *infer_list_elem(CG *g, Node *lit);
Type *infer_node_type(CG *g, Node *n);
const char *llvm_ptr_type_of(Type *t);
const char *llvm_type_of(Type *t);
const char *src_type_name(Type *t);
Type *lt_find(LTys *t, const char *name);
Type *member_field_type(CG *g, Node *n);
Val rt_call(CG *g, Type *rty, const char *fn, const char *fmt, ...);
void scan_block(CG *g, Node *blk);
SDef *sdef_find(SDefs *d, const char *name);
void sdef_push(SDefs *d, const char *name, char **names, Type **types, int count);
Sig *sig_find(Sigs *s, const char *name);
void sig_push(Sigs *s, const char *name, Type *ret, Type **params, int arity);
void stack_push_int(int **v, int *n, int *cap, int x);
void struct_addr(CG *g, Val *o);
Val val_make(Type *ty, const char *v);
Val val_take(Type *ty, char *owned);
int struct_field_idx(CG *g, const char *sname, const char *fname);
Type *struct_field_type(CG *g, const char *sname, int idx);


/* Promoted when codegen_expr.c was cut into codegen_{lit,call,match,
 * print}.c: the dispatcher and the builtin/call emitters reach across
 * the new file boundaries, so these lost their `static`. */
Val val_make_agg(Type *ty, const char *v);
char *emit_instrf_agg(CG *g, const char *fmt, ...);
const char *rt_arg_type(Type *ty);
const char *map_get_fn(Type *ty);
const char *list_push_fn(Type *ty);
const char *map_put_fn(Type *ty);
/* from codegen_scan.c: the one value shape a struct's fields share, or
 * NULL when they disagree. The get() scan case and emit_get_call
 * (codegen_call.c) both answer with it, so it is not static. */
Type *struct_common_shape(Type *t);
/* from typecheck.c — spells a Type for diagnostics; this TU does not include
 * typecheck_internal.h, so the declaration lives here. */
const char *ty_str(Type *t);
Type *list_type(void);
Val cg_list_lit(CG *g, Node *n);
Val cg_map_or_struct_lit(CG *g, Node *n);
Val cg_struct_update(CG *g, Node *n);
Val cg_ctor(CG *g, Node *n);
Val cg_funclit(CG *g, Node *n);
Val cg_call(CG *g, Node *n);
Val cg_match(CG *g, Node *n);
Val cg_print(CG *g, Node *n, Val a);
Val emit_struct_to_map(CG *g, const char *sv, Type *st);

#endif /* LUME_CODEGEN_INTERNAL_H */
