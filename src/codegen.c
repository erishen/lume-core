/* codegen.c — the text backend's entry point.

 * What stays here: the allocator helpers, the CG context (the IR buffers, the
 * collected signatures/structs, the current function's locals) and the shared
 * macros, plus codegen_emit_ir() itself.

 * The emitters that used to be in this one file now live next to it:

 *   codegen_types.c   Type -> LLVM spelling
 *   codegen_expr.c    every expression form
 *   codegen_scan.c    static types of expressions, block scanning
 *   codegen_sig.c     signature inference
 *   codegen_stmt.c    statements, loops, function bodies

 * They share the context through codegen_internal.h, so this file is the
 * entry, not the whole backend.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codegen.h"
#include "lume.h"
#include "irbuf.h"
#include "codegen_internal.h"

/* forward: same tu, defined below this point. */
static void cg_free(CG *cg);

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("lume(native): out of memory\n", stderr); exit(70); }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fputs("lume(native): out of memory\n", stderr); exit(70); }
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

void asgs_free(Asgs *a)
{
    for (int i = 0; i < a->n; i++) { free(a->v[i].name); free(a->v[i].slot); }
    free(a->v);
    memset(a, 0, sizeof *a);
}

/* `slot` is the LLVM name holding the variable's address; passing NULL asks
 * for the ordinary `%lv_<name>` local slot. */

void asg_push(Asgs *a, const char *name, Type *ty, const char *slot)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->v = (Asg *)xrealloc(a->v, (size_t)a->cap * sizeof *a->v);
    }
    char default_slot[128];
    if (slot) {
        snprintf(default_slot, sizeof default_slot, "%s", slot);
    } else {
        /* A local's slot must be unique across the whole program, not just
         * within one function: the text backend emits every local as a
         * top-level LLVM `%lv_<name>`, and LLVM rejects two definitions of
         * the same local. Two `let x` in one function (or a nested `for`
         * reusing a name) used to collide and the module failed to
         * assemble. Stamping a process-wide counter onto the name keeps
         * every slot distinct; the `%lv_` prefix is preserved so the
         * entry-block alloca pass still recognises it as a stack slot. */
        static int s_uid = 0;
        snprintf(default_slot, sizeof default_slot, "%%lv_%s_%d", name, s_uid++);
    }

    a->v[a->n].name = xstrdup(name);
    a->v[a->n].ty   = ty;
    a->v[a->n].slot = xstrdup(default_slot);
    a->n++;
}

Asg *asg_find(Asgs *a, const char *name)
{
    for (int i = a->n - 1; i >= 0; i--)
        if (strcmp(a->v[i].name, name) == 0) return &a->v[i];
    return NULL;
}

void sig_push(Sigs *s, const char *name, Type *ret, Type **params, int arity)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = (Sig *)xrealloc(s->v, (size_t)s->cap * sizeof *s->v);
    }
    s->v[s->n].name   = xstrdup(name);
    s->v[s->n].ret    = ret;
    s->v[s->n].params = params;
    s->v[s->n].pnames = NULL;
    s->v[s->n].arity  = arity;
    s->v[s->n].body   = NULL;
    s->v[s->n].node   = NULL;
    s->n++;
}

Sig *sig_find(Sigs *s, const char *name)
{
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->v[i].name, name) == 0) return &s->v[i];
    return NULL;
}

void sdef_push(SDefs *d, const char *name, char **names, Type **types, int count)
{
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 8;
        d->v = (SDef *)xrealloc(d->v, (size_t)d->cap * sizeof *d->v);
    }
    d->v[d->n].name  = xstrdup(name);
    d->v[d->n].names = names;
    d->v[d->n].types = types;
    d->v[d->n].count = count;
    d->n++;
}

SDef *sdef_find(SDefs *d, const char *name)
{
    for (int i = 0; i < d->n; i++)
        if (strcmp(d->v[i].name, name) == 0) return &d->v[i];
    return NULL;
}

void stack_push_int(int **v, int *n, int *cap, int x)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *v = (int *)xrealloc(*v, (size_t)*cap * sizeof(int));
    }
    (*v)[(*n)++] = x;
}

/* Everything the emit loop owns. Both exits of codegen_emit_ir() run this.
 *
 * The vector shells were freed by hand before; what was easy to miss is that
 * each Sig and SDef owns an xstrdup'd copy of its name — sig_push() and
 * sdef_push() copy the identifier out of the AST, and the AST is the loader's
 * to free, so these copies are the only reference to that string.
 *
 * `ir` is deliberately not touched: the caller takes ir.data and this function
 * returns it. `locals` is not touched either — asgs_free() hands the table
 * back to the enclosing scope (see cg_function). */

static void cg_free(CG *cg)
{
    for (int i = 0; i < cg->sigs.n; i++)
        free(cg->sigs.v[i].name);
    for (int i = 0; i < cg->structs.n; i++)
        free(cg->structs.v[i].name);
    free(cg->sigs.v);
    free(cg->structs.v);
    free(cg->gs.data);
    free(cg->brk);
    free(cg->cnt);
    memset(&cg->sigs, 0, sizeof cg->sigs);
    memset(&cg->structs, 0, sizeof cg->structs);
    memset(&cg->gs, 0, sizeof cg->gs);
    cg->brk = cg->cnt = NULL;
    cg->nbrk = cg->ncnt = cg->cbrk = cg->ccnt = 0;
}

char *codegen_emit_ir(const char *mod_name, Node *prog, char *err, size_t err_size)
{
    if (err && err_size) err[0] = '\0';
    if (!prog || prog->type != N_PROGRAM) return NULL;

    CG cg;
    memset(&cg, 0, sizeof cg);

    IrBuf ir;
    irbuf_init(&ir);
    cg.ir = &ir;
    irbuf_init(&cg.gs);

    Node **top  = prog->as.program.stmts;
    int    ntop = prog->as.program.count;

    /* ---- pass 1: signatures and structs, so calls can be resolved in any order ---- */
    for (int i = 0; i < ntop; i++) {
        Node *n = top[i];
        if (n->type == N_FUNC_DECL) {
            sig_push(&cg.sigs, n->as.func.name, n->as.func.ret,
                     n->as.func.param_types, n->as.func.arity);
            /* The body and the parameter names are what let the pass below
             * infer an unannotated return type, and match arguments to the
             * parameters whose type is still unknown. */
            cg.sigs.v[cg.sigs.n - 1].body   = n->as.func.body;
            cg.sigs.v[cg.sigs.n - 1].pnames = n->as.func.names;
            cg.sigs.v[cg.sigs.n - 1].node   = n;
        }
        else if (n->type == N_TYPE_DECL)
            sdef_push(&cg.structs, n->as.type_decl.name, n->as.type_decl.field_names,
                      n->as.type_decl.field_types, n->as.type_decl.count);
    }

    /* ---- pass 1.5: infer the parameters and return types the source left out ----
     * Idempotent, and shared with the libLLVM backend (the driver runs the same
     * pass before it picks an emitter), so both see one signature per name. */
    codegen_infer_signatures(prog, cg.err, sizeof cg.err);
    if (cg.err[0]) {
        if (err && err_size) snprintf(err, err_size, "%s", cg.err);
        cg_free(&cg);
        return NULL;
    }
    /* The pass rewrote the AST; this table still holds the copies pass 1 took
     * while every one of those slots was NULL, so a call would resolve a
     * return type the function no longer has. */
    for (int i = 0; i < cg.sigs.n; i++) {
        Sig *s = &cg.sigs.v[i];
        if (!s->node) continue;
        s->ret    = s->node->as.func.ret;
        s->params = s->node->as.func.param_types;
    }

    /* ---- header ---- */
    EMIT(&cg, "; ModuleID = \"%s\"\n", mod_name ? mod_name : "lume");
    EMIT(&cg, "; generated by lume (native backend: Lume -> LLVM IR text)\n");
    EMIT(&cg, "\n");
    /* Without a target triple LLVM assumes a generic target and lowers with the
     * wrong ABI, so even `printf("%ld", 42)` prints garbage. The triple comes
     * from the host toolchain at build time (see TARGET_TRIPLE in the Makefile).
     * The datalayout is intentionally omitted: LLVM infers it from the triple. */
#ifdef TARGET_TRIPLE
    EMIT(&cg, "target triple = \"%s\"\n", TARGET_TRIPLE);
#endif
    EMIT(&cg, "\n");
    /* Non-variadic runtime helpers — see src/rt.c for why printf is avoided. */
    EMIT(&cg, "declare i64 @lume_print_i64(i64)\n");
    EMIT(&cg, "declare i64 @lume_print_double(double)\n");
    EMIT(&cg, "declare i64 @lume_print_bool(i64)\n");
    EMIT(&cg, "declare i64 @lume_print_str(i8*)\n");
    emit_builtin_declares(&cg);

    for (int i = 0; i < cg.structs.n; i++) {
        SDef *sd = &cg.structs.v[i];
        EMIT(&cg, "%%%s = type {", sd->name);
        for (int j = 0; j < sd->count; j++) {
            const char *ft = sd->types[j] ? llvm_type_of(sd->types[j]) : NULL;
            if (!ft)
                ERR(&cg, "field '%s' of struct '%s' has no codegen type",
                    sd->names[j], sd->name);
            /* Aggregate field lists are comma-separated: `type { i64, i64 }`. */
            EMIT(&cg, "%s%s", j ? ", " : "", ft);
        }
        EMIT(&cg, " }\n");
    }
    EMIT(&cg, "\n");

    /* ---- pass 2: one LLVM function per Lume function ----
     *
     * Top-level statements (a `let`, a bare `print`, ...) live outside every
     * function, but lume runs them in source order, so they are collected
     * here and emitted as one synthetic `L_top` body through exactly the same
     * path a real function takes. Without this they would silently vanish and
     * the link would fail on a missing `_main` — an error that points at the
     * linker instead of the construct actually unsupported. */
    int nfns = 0, ntops = 0;
    Node **fns  = ntop > 0 ? xmalloc(sizeof(Node *) * ntop) : NULL;
    Node **tops = ntop > 0 ? xmalloc(sizeof(Node *) * ntop) : NULL;
    /* Does the top level actually call `main`? Only then is `main` a thing the
     * process can take its status from — a `main` nobody runs is an ordinary
     * function and the status stays 0. */
    int tops_main = 0;
    for (int i = 0; i < ntop; i++) {
        Node *n = top[i];
        if (n->type == N_FUNC_DECL)      fns[nfns++] = n;
        else if (n->type == N_TYPE_DECL) continue;   /* already emitted */
        else if (n->type == N_IMPORT)    continue;   /* resolved by the loader */
        else {
            tops[ntops++] = n;
            if (n->type == N_EXPR_STMT && n->as.expr_stmt.expr &&
                n->as.expr_stmt.expr->type == N_CALL &&
                n->as.expr_stmt.expr->as.call.callee &&
                n->as.expr_stmt.expr->as.call.callee->type == N_VAR &&
                strcmp(n->as.expr_stmt.expr->as.call.callee->as.var.name,
                       "main") == 0)
                tops_main = 1;
        }
    }

    for (int i = 0; i < nfns; i++) cg_function(&cg, fns[i]);

    /* A synthetic function node: only the fields cg_function reads matter. */
    Node *top_fn = NULL;
    if (ntops > 0) {
        Node body;
        memset(&body, 0, sizeof body);
        body.type = N_BLOCK;
        body.as.block.stmts = tops;
        body.as.block.count = ntops;

        top_fn = xmalloc(sizeof(Node));
        memset(top_fn, 0, sizeof(Node));
        top_fn->type = N_FUNC_DECL;
        top_fn->line = top[0] ? top[0]->line : 0;
        top_fn->as.func.name = "top";   /* cg_function prefixes it with L_ */
        top_fn->as.func.body = &body;
        /* `main()` is the program's status in the interpreter too, so it is
         * the interpreter that decides here: the value is read back by the
         * fallback terminator in cg_function, which is why this is set before
         * the body runs and cleared right after. */
        if (tops_main) {
            top_fn->as.func.ret = type_prim(TY_INT);
            cg.capture_main = 1;
        }
        cg_function(&cg, top_fn);
        cg.capture_main = 0;
        cg.exit_tmp[0] = '\0';
    }
    /* top_fn borrows `tops` as its body, so it goes out with the array; its
     * name is the literal "top" rather than a copy, and `body` is a stack
     * frame, so there is nothing else here to release. The node itself outlives
     * this loop: the entry point below reads it to decide whether the program
     * had top-level statements to run. */
    free(fns);
    free(tops);

    /* Anything that failed above leaves a truncated module behind, so refuse
     * to hand out IR the caller would happily write to disk — the driver then
     * reports the real cause instead of a syntax error from clang. */
    if (cg.err[0]) {
        if (err && err_size) snprintf(err, err_size, "%s", cg.err);
        cg_free(&cg);
        return NULL;
    }

    /* String globals are flushed *here*, not before pass 2: printing code is
     * emitted inside function bodies, so constants created during pass 2 would
     * otherwise land after the flush. Anywhere at module scope is legal IR.
     * irbuf_puts, not EMIT — the text holds `%` (format strings) verbatim. */
    if (cg.gs.data)
        irbuf_puts(&ir, cg.gs.data);
    if (cg.gs.len)
        EMIT(&cg, "\n");

    /* ---- entry point: wrap the program's top level as the C entry ----
     * L_top (the module's own statements) is all the entry runs. `main` is an
     * ordinary function here, exactly as it is in the interpreter: the entry
     * used to call L_main as well, which ran it a second time whenever the
     * top level called it too, and never ran it when the interpreter would
     * have (the interpreter only ever executes top-level statements). A
     * program that wants main() to run calls it from the top level, in source
     * order, like anything else.
     *
     * And L_top — not the entry — is what carries the status: the interpreter
     * reads main()'s value off the same top-level call, so forwarding it from
     * here keeps the two paths reporting the same exit code. */
    EMIT(&cg, "\n; --- C entry point ---\n");
    EMIT(&cg, "define i32 @main() {\nentry:\n");
    if (top_fn) {
        if (tops_main) {
            /* L_top carries an i64, like every other lume int: the status is
             * the low eight bits of it, which is what a process reports. */
            EMIT(&cg, "  %%r = call i64 @L_top()\n");
            EMIT(&cg, "  %%t = trunc i64 %%r to i32\n");
            EMIT(&cg, "  ret i32 %%t\n");
        } else
            EMIT(&cg, "  call void @L_top()\n");
    }
    if (!top_fn || !tops_main) EMIT(&cg, "  ret i32 0\n");
    EMIT(&cg, "}\n");

    /* Only now is top_fn done being read — the entry above is the last field
     * of it any code touches. Releasing it here rather than with `fns`/`tops`
     * is what keeps the emission order the entry point depends on intact. */
    free(top_fn);
    top_fn = NULL;

    /* ---- cleanup ---- */
    cg_free(&cg);

    return ir.data;
}
