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

typedef struct { char *name; Type *ty; char *slot; } Asg;
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
                 int is_lambda; } Sig;
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

    Node  *prog;            /* the whole program, so the signature pass can
                             * walk the top-level calls as well as the bodies */

    Sigs   sigs;            /* every top-level func, collected before emitting */
    SDefs  structs;         /* every `type X = {...}` declaration            */
    Asgs   locals;          /* variables of the function currently emitted   */
    Sig   *cur;             /* signature of the function currently emitted   */
    Type  *expect;          /* type the expression context wants (struct lits) */

    int    lid;             /* label id counter            */
    int    tid;             /* temp/id counter            */
    int    sid;             /* string constant counter    */
    int    nclo;            /* closure mangled-name counter */

    int   *brk; int nbrk, cbrk;    /* stack of enclosing loop labels */
    int   *cnt; int ncnt, ccnt;

    int    capture_main;           /* emitting the synthetic top-level body:
                                    * remember what `main()` returned */
    char   exit_tmp[32];

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

Asg *asg_find(Asgs *a, const char *name);
void asg_push(Asgs *a, const char *name, Type *ty, const char *slot);
void asgs_free(Asgs *a);
const char *bad(CG *g, Node *n, const char *what);
Val cg_assign_expr(CG *g, Node *n);
Val cg_assign_mem(CG *g, Node *n);
Val cg_expr(CG *g, Node *n);
char *cg_string_val(CG *g, const char *text, size_t len);
void cg_function(CG *g, Node *fn);
void cg_closure(CG *g, Node *fn);
/* The list element accessor table, shared between the for-in emitter
 * (codegen_stmt.c) and the map/filter/reduce emitters (codegen_expr.c):
 * one dispatch, so a type that iterates is a type the HOFs can also read. */
const char *list_at_fn(Type *ty);
Type *elem_rty(Type *ty);
int codegen_infer_signatures(Node *prog, char *err, size_t err_size);
Val coerce(CG *g, Type *to, Val v, size_t line);
void emit_builtin_declares(CG *g);
char *emit_instrf(CG *g, Type *ty, const char *fmt, ...);
Type *infer_list_elem(Node *lit);
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

#endif /* LUME_CODEGEN_INTERNAL_H */
