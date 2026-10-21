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
#include "arena.h"

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
    /* name/slot strings are arena memory now (TODO.md P2 Stage 3): only the
     * malloc'd vector shell dies here. */
    free(a->v);
    memset(a, 0, sizeof *a);
}

/* Drop the top of an Asgs table back to `n` entries. The native backends
 * scope local bindings — match-arm binds, if-let binds, let-expression
 * bindings — by pushing onto g->locals and resetting g->locals.n to a saved
 * value afterwards. This used to free the discarded entries' xstrdup'd
 * name/slot strings (the bare count reset leaked them; LeakSanitizer caught
 * it from asg_push); with the strings in the parse arena the reset is a bare
 * count assignment again, and the arena reclaims everything at
 * end-of-emission. Nested scopes each capture their own saved_n and truncate
 * to it, which stays correct when nothing is freed. */
void asg_truncate(Asgs *a, int n)
{
    if (n < 0) n = 0;
    a->n = n;
}

/* `slot` is the LLVM name holding the variable's address; passing NULL asks
 * for the ordinary `%lv_<name>` local slot. */

void asg_push(Asgs *a, const char *name, Type *ty, const char *slot,
              size_t line)
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

    a->v[a->n].name = arena_strdup(name);
    a->v[a->n].ty   = ty;
    a->v[a->n].slot = arena_strdup(default_slot);
    /* The declaration's source line. The locals table is pre-scanned flat
     * (the entry block needs every slot before any statement is emitted),
     * so a name can appear several times — a loop variable and a later
     * same-name `let` of a different type, say. Lookup is therefore
     * position-aware: a use site may only see declarations at or above its
     * own line. That is what makes the native backends agree with the
     * interpreter, whose set-or-define env rebinds a name sequentially:
     * the visible binding at a use is the latest one declared before it. */
    a->v[a->n].line = line;
    a->v[a->n].shadow = 0;
    a->n++;
}

void asg_push_shadow(Asgs *a, const char *name, Type *ty, const char *slot,
                     size_t line)
{
    asg_push(a, name, ty, slot, line);
    a->v[a->n - 1].shadow = 1;
}

Asg *asg_find(Asgs *a, const char *name, size_t use_line)
{
    for (int i = a->n - 1; i >= 0; i--)
        if (strcmp(a->v[i].name, name) == 0 && a->v[i].line <= use_line)
            return &a->v[i];
    return NULL;
}

/* The top-level table has no line filter: a function body may only name a
 * top-level binding the typechecker already proved is declared before the
 * function's own text (a forward reference dies as "undefined variable" in
 * the checker), so every use that reaches codegen is of a binding that
 * exists. What the use *sees* is the single module global — which is exactly
 * the interpreter's call-time env lookup (a function reads whatever the top
 * level last stored, not a snapshot taken at definition time). */

Asg *gvar_find(Asgs *a, const char *name)
{
    for (int i = a->n - 1; i >= 0; i--)
        if (strcmp(a->v[i].name, name) == 0)
            return &a->v[i];
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
    /* The vector grows by realloc, which does not zero the new tail — every
     * field added to Sig must be set HERE, or a named function's write-back
     * can wander into the lambda branch and spell its types through the
     * funclit union offsets (where funclit.ret sits exactly on
     * func.param_types). Found as a use-after-free that only reproduced
     * under some binaries: the garbage byte decided which branch ran. */
    s->v[s->n].is_lambda = 0;
    s->v[s->n].phmask    = 0;
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
    asgs_free(&cg->gvars);
    /* Route handlers are buffered until every function has closed (see
     * codegen_emit_ir), and codegen_emit_ir frees them once it has appended
     * them to the module. Releasing them here as well is what makes the
     * empty case safe: a program with no route handler never enters that
     * `if`, so the append-and-free never runs, and the buffer leaked on every
     * single compile -- 256 bytes that LeakSanitizer reported as
     * `Direct leak ... irbuf_init src/irbuf.c:14 <- codegen_emit_ir`, and
     * which failed `make asan` on Linux. Members freed twice are not a
     * concern: irbuf_free zeroes the buffer it frees, and the memset below
     * covers the rest. */
    irbuf_free(&cg->handlers);
    free(cg->brk);
    free(cg->cnt);
    memset(&cg->sigs, 0, sizeof cg->sigs);
    memset(&cg->structs, 0, sizeof cg->structs);
    memset(&cg->gs, 0, sizeof cg->gs);
    memset(&cg->handlers, 0, sizeof cg->handlers);
    cg->brk = cg->cnt = NULL;
    cg->nbrk = cg->ncnt = cg->cbrk = cg->ccnt = 0;
}

/* Collect every lambda the signature pass named, in one pass over the tree.
 *
 * The pass registers each N_FUNC_LIT it can reach from a call site and stamps
 * its mangled name onto the node (funclit.cname); this walk afterwards is what
 * turns that registry into `define`s. Only nodes with a cname are collected —
 * a route handler is a function literal too, but it is emitted through the
 * FFI path and never gets a cname, so the gate keeps the two apart. The walk
 * descends into lambda bodies as well, so a lambda nested in a lambda is
 * found from the outer one's own entry. */

static void collect_lambdas(Node *n, Node ***out, int *cnt, int *cap)
{
    if (!n) return;
    switch (n->type) {
    case N_FUNC_LIT:
        if (n->as.funclit.cname) {
            if (*cnt == *cap) {
                *cap = *cap ? *cap * 2 : 8;
                *out = (Node **)xrealloc(*out, (size_t)*cap * sizeof **out);
            }
            (*out)[(*cnt)++] = n;
        }
        collect_lambdas(n->as.funclit.body, out, cnt, cap);
        return;
    case N_PROGRAM: case N_BLOCK:
        for (int i = 0; i < n->as.block.count; i++)
            collect_lambdas(n->as.block.stmts[i], out, cnt, cap);
        return;
    case N_FUNC_DECL:
        collect_lambdas(n->as.func.body, out, cnt, cap);
        return;
    case N_LET:        collect_lambdas(n->as.let.init, out, cnt, cap); return;
    case N_LET_PAT:    collect_lambdas(n->as.letpat.init, out, cnt, cap); return;
    case N_IF:
        collect_lambdas(n->as.ifs.cond, out, cnt, cap);
        collect_lambdas(n->as.ifs.then, out, cnt, cap);
        collect_lambdas(n->as.ifs.els,  out, cnt, cap);
        return;
    case N_WHILE:
        collect_lambdas(n->as.whiles.cond, out, cnt, cap);
        collect_lambdas(n->as.whiles.body, out, cnt, cap);
        return;
    case N_FOR:
        collect_lambdas(n->as.fors.init,     out, cnt, cap);
        collect_lambdas(n->as.fors.cond,     out, cnt, cap);
        collect_lambdas(n->as.fors.incr,     out, cnt, cap);
        collect_lambdas(n->as.fors.iterable, out, cnt, cap);
        collect_lambdas(n->as.fors.body,     out, cnt, cap);
        return;
    case N_RETURN:     collect_lambdas(n->as.ret.expr, out, cnt, cap); return;
    case N_EXPR_STMT:  collect_lambdas(n->as.expr_stmt.expr, out, cnt, cap); return;
    case N_ASSIGN:     collect_lambdas(n->as.assign.value, out, cnt, cap); return;
    case N_ASSIGN_MEMBER:
        collect_lambdas(n->as.assign_mem.obj,   out, cnt, cap);
        collect_lambdas(n->as.assign_mem.value, out, cnt, cap);
        return;
    case N_CALL:
        collect_lambdas(n->as.call.callee, out, cnt, cap);
        for (int i = 0; i < n->as.call.argc; i++)
            collect_lambdas(n->as.call.args[i], out, cnt, cap);
        return;
    case N_MEMBER:     collect_lambdas(n->as.member.obj, out, cnt, cap); return;
    case N_INDEX:
        collect_lambdas(n->as.index.obj,   out, cnt, cap);
        collect_lambdas(n->as.index.index, out, cnt, cap);
        return;
    case N_UNARY:      collect_lambdas(n->as.unary.operand, out, cnt, cap); return;
    case N_BINARY:
        collect_lambdas(n->as.binary.left,  out, cnt, cap);
        collect_lambdas(n->as.binary.right, out, cnt, cap);
        return;
    case N_TERNARY:
        collect_lambdas(n->as.ternary.cond, out, cnt, cap);
        collect_lambdas(n->as.ternary.then, out, cnt, cap);
        collect_lambdas(n->as.ternary.els,  out, cnt, cap);
        return;
    case N_MATCH:
        collect_lambdas(n->as.matchn.scrutinee, out, cnt, cap);
        for (int i = 0; i < n->as.matchn.armc; i++) {
            collect_lambdas(n->as.matchn.arms[i].guard, out, cnt, cap);
            collect_lambdas(n->as.matchn.arms[i].body, out, cnt, cap);
        }
        return;
    case N_IF_LET:
        collect_lambdas(n->as.iflet.value, out, cnt, cap);
        collect_lambdas(n->as.iflet.then, out, cnt, cap);
        collect_lambdas(n->as.iflet.els, out, cnt, cap);
        return;
    case N_CTOR:
        for (int i = 0; i < n->as.ctor.argc; i++)
            collect_lambdas(n->as.ctor.args[i], out, cnt, cap);
        return;
    case N_LIST_LIT:
        for (int i = 0; i < n->as.list.count; i++)
            collect_lambdas(n->as.list.items[i], out, cnt, cap);
        return;
    case N_MAP_LIT:
        for (int i = 0; i < n->as.map.count; i++)
            collect_lambdas(n->as.map.vals[i], out, cnt, cap);
        return;
    case N_SERVER:
        for (int i = 0; i < n->as.server.count; i++)
            collect_lambdas(n->as.server.assigns[i], out, cnt, cap);
        return;
    case N_ROUTE:      collect_lambdas(n->as.route.handler, out, cnt, cap); return;
    case N_TOOL:
        collect_lambdas(n->as.tool.params,  out, cnt, cap);
        collect_lambdas(n->as.tool.handler, out, cnt, cap);
        return;
    case N_VERBS:      collect_lambdas(n->as.verbs.methods, out, cnt, cap); return;
    default: return;   /* literals, imports, type decls, break/continue */
    }
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
    irbuf_init(&cg.handlers);

    Node **top  = prog->as.program.stmts;
    int    ntop = prog->as.program.count;

    /* Local functions first: stamp cnames and rewrite call sites before any
     * signature table is built (idempotent — the checker already ran this;
     * this is the guard for paths that compile without checking). */
    if (!lume_resolve_localfns(prog, 0, err, err_size)) {
        cg_free(&cg);
        return NULL;
    }

    /* ---- pass 1: signatures and structs, so calls can be resolved in any order ---- */
    /* Local functions ride the same table under their cname (stamped and
     * call-rewritten by lume_resolve_localfns above), so a call inside the
     * enclosing function resolves and the emit loop below defines them. */
    {
        int nfn = lume_collect_funcs(prog, NULL, 0);
        Node **allfns = nfn > 0 ? xmalloc(sizeof(Node *) * (size_t)nfn) : NULL;
        if (nfn > 0) lume_collect_funcs(prog, allfns, nfn);
        for (int i = 0; i < nfn; i++) {
            Node *n = allfns[i];
            const char *fname = n->as.func.cname ? n->as.func.cname
                                                 : n->as.func.name;
            sig_push(&cg.sigs, fname, n->as.func.ret,
                     n->as.func.param_types, n->as.func.arity);
            /* The body and the parameter names are what let the pass below
             * infer an unannotated return type, and match arguments to the
             * parameters whose type is still unknown. */
            cg.sigs.v[cg.sigs.n - 1].body   = n->as.func.body;
            cg.sigs.v[cg.sigs.n - 1].pnames = n->as.func.names;
            cg.sigs.v[cg.sigs.n - 1].node   = n;
        }
        free(allfns);
    }
    for (int i = 0; i < ntop; i++) {
        Node *n = top[i];
        if (n->type == N_TYPE_DECL)
            sdef_push(&cg.structs, n->as.type_decl.name, n->as.type_decl.field_names,
                      n->as.type_decl.field_types, n->as.type_decl.count);
        /* An enum has no runtime presence of its own: every variant value is
         * built at its construction site (a tagged map), the constructor
         * rewrite happened in the parser, and the type rules live in the
         * checker. Nothing to emit, nothing to skip silently. */
        else if (n->type == N_ENUM_DECL)
            continue;
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

    /* ---- pass 1.7: lift the top-level bindings into module globals ----
     *
     * A function body may read a top-level `let` (the interpreter resolves it
     * through the global env at call time), but the emitter compiles each
     * function in isolation, so the name has to resolve to storage that every
     * define can reach: one `@lv_<name>` global per top-level binding. The
     * pre-scan walks the top-level statements once — the same scan_stmt a
     * function body gets — and every top-level `let` / for-in variable is
     * registered twice: into `gvars` (the cross-function table, @lv_ slot)
     * and into what will become L_top's own locals table, so L_top's stores
     * land in the very same global a function reads. A top-level mutation
     * made after a function is defined is therefore visible when the function
     * runs, exactly as in the interpreter. The scanned table is handed to
     * cg_def through preset_locals further down, so L_top is never scanned
     * twice. */
    int nfns = 0, ntops = 0;
    /* Every function gets a define: top-level ones and hoisted locals alike.
     * The collector returns the full count first so one allocation fits. */
    nfns = lume_collect_funcs(prog, NULL, 0);
    Node **fns  = nfns > 0 ? xmalloc(sizeof(Node *) * (size_t)nfns) : NULL;
    if (nfns > 0) lume_collect_funcs(prog, fns, nfns);
    Node **tops = ntop > 0 ? xmalloc(sizeof(Node *) * ntop) : NULL;
    /* Does the top level actually call `main`? Only then is `main` a thing the
     * process can take its status from — a `main` nobody runs is an ordinary
     * function and the status stays 0. */
    int tops_main = 0;
    for (int i = 0; i < ntop; i++) {
        Node *n = top[i];
        if (n->type == N_FUNC_DECL)      continue;    /* collected above */
        else if (n->type == N_TYPE_DECL) continue;   /* already emitted */
        else if (n->type == N_ENUM_DECL) continue;   /* no runtime presence */
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

    Asgs top_locals;
    memset(&top_locals, 0, sizeof top_locals);
    if (ntops > 0) {
        Node body;
        memset(&body, 0, sizeof body);
        body.type = N_BLOCK;
        body.as.block.stmts = tops;
        body.as.block.count = ntops;

        cg.scanning_top = 1;
        scan_block(&cg, &body);      /* fills cg.locals AND cg.gvars */
        cg.scanning_top = 0;
        top_locals = cg.locals;      /* steal the scanned table */
        memset(&cg.locals, 0, sizeof cg.locals);
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
    EMIT(&cg, "declare i64 @lume_print_null()\n");
    EMIT(&cg, "declare i64 @lume_print_slot(i8*)\n");
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

    /* ---- module globals: one @lv_ slot per top-level binding (SPEC 8.1 #10)
     * All of them zero-start; L_top stores the real initializer in source
     * order before any use can run (the typechecker rejects a use of a name
     * before its declaration, and a function is only ever *called* from
     * statements that follow its initialization). A named struct is stored
     * directly in the global — the same by-address model its locals use; a
     * runtime map is a heap pointer, so its global is the i8* itself, which
     * is also what llvm_type_of spells for it. */
    for (int i = 0; i < cg.gvars.n; i++) {
        Asg *gv = &cg.gvars.v[i];
        const char *lt = llvm_type_of(gv->ty);
        if (!lt)
            ERR(&cg, "line %zu: top-level variable '%s' has no codegen type",
                gv->line, gv->name);
        EMIT(&cg, "%s = global %s zeroinitializer\n", gv->slot, lt);
    }
    if (cg.gvars.n)
        EMIT(&cg, "\n");

    /* ---- pass 2: one LLVM function per Lume function ----
     *
     * Top-level statements (a `let`, a bare `print`, ...) live outside every
     * function, but lume runs them in source order, so they are collected
     * here and emitted as one synthetic `L_top` body through exactly the same
     * path a real function takes. Without this they would silently vanish and
     * the link would fail on a missing `_main` — an error that points at the
     * linker instead of the construct actually unsupported. The collection
     * itself happened in pass 1.7 above (the pre-scan needs the same split);
     * what is left here is emission. */

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
        /* The table pass 1.7 scanned IS L_top's locals: the entries carry the
         * @lv_ slots, so every store the top level makes lands in the same
         * global a function body reads. cg_def reuses it instead of scanning
         * the body a second time. */
        cg.preset_locals = &top_locals;
        cg.scanning_top = 1;
        cg_function(&cg, top_fn);
        cg.scanning_top = 0;
        cg.preset_locals = NULL;
        asgs_free(&top_locals);
        cg.capture_main = 0;
        cg.exit_tmp[0] = '\0';
    }
    /* Route handlers were buffered (they cannot be defined inside L_top);
     * now that every function has closed, append them to the module. */
    if (cg.handlers.len) {
        irbuf_puts(&ir, cg.handlers.data);
        irbuf_free(&cg.handlers);
    }
    /* Closures: every lambda the signature pass named gets its own define.
     * They are emitted here, at module scope after the named functions, so
     * nothing inside them can sit inside another body — a `define` is only
     * legal at the top level of a module. */
    {
        Node **clos = NULL;
        int nclos = 0, capc = 0;
        collect_lambdas(prog, &clos, &nclos, &capc);
        for (int i = 0; i < nclos; i++) cg_closure(&cg, clos[i]);
        free(clos);
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
    /* argv() support: the entry receives the process arguments and stashes
     * them (lume_cli_init in src/rt.c) before any top-level statement runs. */
    EMIT(&cg, "declare void @lume_cli_init(i32, i8**)\n");
    EMIT(&cg, "define i32 @main(i32 %%argc, i8** %%argv) {\nentry:\n");
    EMIT(&cg, "  call void @lume_cli_init(i32 %%argc, i8** %%argv)\n");
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

/* Multi-file variant of codegen_emit_ir: imported module ASTs join the
 * emission, so a cross-module member call (policy.decide(...)) can link
 * against the module function and a module function body can read its
 * module's top-level bindings (attribute tables, audit path) through the
 * same @lv_ globals the entry uses.
 *
 * mods[0] is the entry program; mods[1..] are its imports in dependency
 * order, alias_list[m] every alias any loaded module uses for mods[m] — the
 * entry's `as` names plus module-side imports (alias_list[0] ignored).
 * Module functions are collected under "<ns>.<name>" for each referencing
 * alias (member-call resolution) but defined under their own source-name
 * symbols (cg_function's @L_<fname>), which is what the member call emits
 * against.
 * Module type decls join the struct table under their plain names (a
 * `d.decision` member access has no namespace to qualify with), and module
 * top-level statements run first, inside L_top, before the entry's own — the
 * native mirror of the loader executing imported module top levels in
 * dependency order. */
char *codegen_emit_ir_multi(const char *mod_name, Node **mods,
                            const char ***alias_list, const int *alias_cnt,
                            int nmods, char *err, size_t err_size)
{
    if (err && err_size) err[0] = '\0';
    if (!mods || nmods <= 0 || !mods[0] || mods[0]->type != N_PROGRAM)
        return NULL;

    Node *prog = mods[0];
    CG cg;
    memset(&cg, 0, sizeof cg);
    cg.alias_list = alias_list;
    cg.alias_cnt  = alias_cnt;
    cg.nmods      = nmods;

    IrBuf ir;
    irbuf_init(&ir);
    cg.ir = &ir;
    irbuf_init(&cg.gs);
    irbuf_init(&cg.handlers);

    Node **top  = prog->as.program.stmts;
    int    ntop = prog->as.program.count;

    /* Local functions first, in every file: stamp cnames and rewrite call
     * sites before any signature table is built. (Each file's cname counter
     * starts at zero, so a module that nests local functions inside module
     * functions can collide with the entry's — a known boundary, the module
     * functions in the pinned fixtures have none.) */
    for (int m = 0; m < nmods; m++) {
        Node *mp = mods[m];
        if (!mp || mp->type != N_PROGRAM) continue;
        if (!lume_resolve_localfns(mp, m, err, err_size)) {
            cg_free(&cg);
            return NULL;
        }
    }

    /* ---- pass 1: signatures and structs, so calls can be resolved in any order ---- */
    /* Entry functions ride under their plain name/cname; module functions are
     * registered under "<ns>.<name>" for member-call resolution, but defined
     * (below, via cg_function) under their own source-name symbol. */
    for (int m = 0; m < nmods; m++) {
        Node *mp = mods[m];
        if (!mp || mp->type != N_PROGRAM) continue;
        int nfn = lume_collect_funcs(mp, NULL, 0);
        Node **allfns = nfn > 0 ? xmalloc(sizeof(Node *) * (size_t)nfn) : NULL;
        if (nfn > 0) lume_collect_funcs(mp, allfns, nfn);
        for (int i = 0; i < nfn; i++) {
            Node *n = allfns[i];
            const char *fname = n->as.func.cname ? n->as.func.cname
                                                 : n->as.func.name;
            char qbuf[512];
            const char *qname = fname;
            if (m > 0 && alias_list && alias_cnt && alias_cnt[m] > 0) {
                /* One signature per alias any loaded module uses for this
                 * module: the entry's `as` name and every module-side import
                 * alias, so a member call resolves under each referencing
                 * side's alias (module chains: A imports B, A calls b.fn). */
                for (int k = 0; k < alias_cnt[m]; k++) {
                    snprintf(qbuf, sizeof qbuf, "%s.%s", alias_list[m][k],
                             fname);
                    sig_push(&cg.sigs, qbuf, n->as.func.ret,
                             n->as.func.param_types, n->as.func.arity);
                    cg.sigs.v[cg.sigs.n - 1].body   = n->as.func.body;
                    cg.sigs.v[cg.sigs.n - 1].pnames = n->as.func.names;
                    cg.sigs.v[cg.sigs.n - 1].node   = n;
                }
                continue;
            }
            sig_push(&cg.sigs, qname, n->as.func.ret,
                     n->as.func.param_types, n->as.func.arity);
            cg.sigs.v[cg.sigs.n - 1].body   = n->as.func.body;
            cg.sigs.v[cg.sigs.n - 1].pnames = n->as.func.names;
            cg.sigs.v[cg.sigs.n - 1].node   = n;
        }
        free(allfns);
    }
    for (int m = 0; m < nmods; m++) {
        Node *mp = mods[m];
        if (!mp || mp->type != N_PROGRAM) continue;
        Node **mt = mp->as.program.stmts;
        for (int i = 0; i < mp->as.program.count; i++) {
            Node *n = mt[i];
            if (n->type == N_TYPE_DECL)
                sdef_push(&cg.structs, n->as.type_decl.name, n->as.type_decl.field_names,
                          n->as.type_decl.field_types, n->as.type_decl.count);
            /* An enum has no runtime presence of its own (see the single-file
             * pass). */
            else if (n->type == N_ENUM_DECL)
                continue;
        }
    }

    /* ---- pass 1.5: infer the parameters and return types the source left out ----
     * The multi-file pass so an imported function's unannotated parameters are
     * back-filled from the entry's call sites; idempotent, and shared with the
     * libLLVM backend (the driver runs it before picking an emitter). */
    if (nmods > 1) {
        if (codegen_infer_signatures_multi(mods, alias_list, alias_cnt, nmods,
                                           cg.err, sizeof cg.err) != 0) {
            if (err && err_size) snprintf(err, err_size, "%s", cg.err);
            cg_free(&cg);
            return NULL;
        }
    } else if (codegen_infer_signatures(prog, cg.err, sizeof cg.err) != 0) {
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

    /* ---- pass 1.7: lift the top-level bindings into module globals ----
     * (see the single-file pass for the rationale). Imported module top-level
     * statements run first, inside L_top, so a module function body reads its
     * module's tables through @lv_ exactly as the interpreter resolves them
     * through the module env. */
    int nfns = 0, ntops = 0;
    for (int m = 0; m < nmods; m++) {
        Node *mp = mods[m];
        if (!mp || mp->type != N_PROGRAM) continue;
        nfns += lume_collect_funcs(mp, NULL, 0);
    }
    Node **fns  = nfns > 0 ? xmalloc(sizeof(Node *) * (size_t)nfns) : NULL;
    int *fnmod = nfns > 0 ? xmalloc(sizeof(int) * (size_t)nfns) : NULL;
    if (nfns > 0) {
        int fi = 0;
        for (int m = 0; m < nmods; m++) {
            Node *mp = mods[m];
            if (!mp || mp->type != N_PROGRAM) continue;
            int mn = lume_collect_funcs(mp, NULL, 0);
            if (mn > 0) lume_collect_funcs(mp, fns + fi, mn);
            for (int k = 0; k < mn; k++) fnmod[fi + k] = m;
            fi += mn;
        }
    }
    /* Top-level statements from every file, modules first: they all execute
     * in one L_top, which is what the entry point runs. */
    int topcap = ntop;
    for (int m = 1; m < nmods; m++) {
        Node *mp = mods[m];
        if (mp && mp->type == N_PROGRAM) topcap += mp->as.program.count;
    }
    Node **tops = topcap > 0 ? xmalloc(sizeof(Node *) * (size_t)topcap) : NULL;
    /* Does the top level actually call `main`? Only then is `main` a thing the
     * process can take its status from — a `main` nobody runs is an ordinary
     * function and the status stays 0. */
    int tops_main = 0;
    for (int m = 1; m < nmods; m++) {
        Node *mp = mods[m];
        if (!mp || mp->type != N_PROGRAM) continue;
        Node **mt = mp->as.program.stmts;
        for (int i = 0; i < mp->as.program.count; i++) {
            Node *n = mt[i];
            if (n->type == N_FUNC_DECL)      continue;    /* collected above */
            else if (n->type == N_TYPE_DECL) continue;   /* already emitted */
            else if (n->type == N_ENUM_DECL) continue;   /* no runtime presence */
            else if (n->type == N_IMPORT)    continue;   /* resolved by the loader */
            else tops[ntops++] = n;
        }
    }
    for (int i = 0; i < ntop; i++) {
        Node *n = top[i];
        if (n->type == N_FUNC_DECL)      continue;    /* collected above */
        else if (n->type == N_TYPE_DECL) continue;   /* already emitted */
        else if (n->type == N_ENUM_DECL) continue;   /* no runtime presence */
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

    Asgs top_locals;
    memset(&top_locals, 0, sizeof top_locals);
    if (ntops > 0) {
        Node body;
        memset(&body, 0, sizeof body);
        body.type = N_BLOCK;
        body.as.block.stmts = tops;
        body.as.block.count = ntops;

        cg.scanning_top = 1;
        scan_block(&cg, &body);      /* fills cg.locals AND cg.gvars */
        cg.scanning_top = 0;
        top_locals = cg.locals;      /* steal the scanned table */
        memset(&cg.locals, 0, sizeof cg.locals);
    }

    /* ---- header ---- */
    EMIT(&cg, "; ModuleID = \"%s\"\n", mod_name ? mod_name : "lume");
    EMIT(&cg, "; generated by lume (native backend: Lume -> LLVM IR text)\n");
    EMIT(&cg, "\n");
#ifdef TARGET_TRIPLE
    EMIT(&cg, "target triple = \"%s\"\n", TARGET_TRIPLE);
#endif
    EMIT(&cg, "\n");
    EMIT(&cg, "declare i64 @lume_print_i64(i64)\n");
    EMIT(&cg, "declare i64 @lume_print_double(double)\n");
    EMIT(&cg, "declare i64 @lume_print_bool(i64)\n");
    EMIT(&cg, "declare i64 @lume_print_str(i8*)\n");
    EMIT(&cg, "declare i64 @lume_print_null()\n");
    EMIT(&cg, "declare i64 @lume_print_slot(i8*)\n");
    emit_builtin_declares(&cg);

    for (int i = 0; i < cg.structs.n; i++) {
        SDef *sd = &cg.structs.v[i];
        EMIT(&cg, "%%%s = type {", sd->name);
        for (int j = 0; j < sd->count; j++) {
            const char *ft = sd->types[j] ? llvm_type_of(sd->types[j]) : NULL;
            if (!ft)
                ERR(&cg, "field '%s' of struct '%s' has no codegen type",
                    sd->names[j], sd->name);
            EMIT(&cg, "%s%s", j ? ", " : "", ft);
        }
        EMIT(&cg, " }\n");
    }
    EMIT(&cg, "\n");

    /* ---- module globals: one @lv_ slot per top-level binding (SPEC 8.1 #10) */
    for (int i = 0; i < cg.gvars.n; i++) {
        Asg *gv = &cg.gvars.v[i];
        const char *lt = llvm_type_of(gv->ty);
        if (!lt)
            ERR(&cg, "line %zu: top-level variable '%s' has no codegen type",
                gv->line, gv->name);
        EMIT(&cg, "%s = global %s zeroinitializer\n", gv->slot, lt);
    }
    if (cg.gvars.n)
        EMIT(&cg, "\n");

    /* ---- pass 2: one LLVM function per Lume function ---- */
    for (int i = 0; i < nfns; i++) {
        /* A plain call inside a module function resolves "<ns>.<name>" first
         * (the multi-file registration key), so same-module functions and the
         * module's own local functions link even though the body spells bare
         * names. Entry functions keep cur_ns NULL (bare-key lookup only). */
        cg.cur_ns = (fnmod[i] > 0 && alias_list && alias_cnt &&
                     alias_cnt[fnmod[i]] > 0)
                        ? alias_list[fnmod[i]][0] : NULL;
        cg_function(&cg, fns[i]);
    }
    cg.cur_ns = NULL;
    free(fnmod);

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
        top_fn->as.func.name = "top";
        top_fn->as.func.body = &body;
        if (tops_main) {
            top_fn->as.func.ret = type_prim(TY_INT);
            cg.capture_main = 1;
        }
        cg.preset_locals = &top_locals;
        cg.scanning_top = 1;
        cg_function(&cg, top_fn);
        cg.scanning_top = 0;
        cg.preset_locals = NULL;
        asgs_free(&top_locals);
        cg.capture_main = 0;
        cg.exit_tmp[0] = '\0';
    }
    /* Route handlers were buffered; now that every function has closed,
     * append them to the module. */
    if (cg.handlers.len) {
        irbuf_puts(&ir, cg.handlers.data);
        irbuf_free(&cg.handlers);
    }
    /* Closures: every lambda the signature pass named gets its own define.
     * All module files are walked (not just the entry), and every file's
     * lambda carries its module index in the cname — the fixed form of the
     * former "module lambdas are a known boundary" note. */
    {
        Node **clos = NULL;
        int nclos = 0, capc = 0;
        for (int m = 0; m < nmods; m++) {
            Node *mp = mods[m];
            if (mp && mp->type == N_PROGRAM)
                collect_lambdas(mp, &clos, &nclos, &capc);
        }
        for (int i = 0; i < nclos; i++) cg_closure(&cg, clos[i]);
        free(clos);
    }
    free(fns);
    free(tops);

    if (cg.err[0]) {
        if (err && err_size) snprintf(err, err_size, "%s", cg.err);
        cg_free(&cg);
        return NULL;
    }

    if (cg.gs.data)
        irbuf_puts(&ir, cg.gs.data);
    if (cg.gs.len)
        EMIT(&cg, "\n");

    /* ---- entry point: wrap the program's top level as the C entry ---- */
    EMIT(&cg, "\n; --- C entry point ---\n");
    EMIT(&cg, "declare void @lume_cli_init(i32, i8**)\n");
    EMIT(&cg, "define i32 @main(i32 %%argc, i8** %%argv) {\nentry:\n");
    EMIT(&cg, "  call void @lume_cli_init(i32 %%argc, i8** %%argv)\n");
    if (top_fn) {
        if (tops_main) {
            EMIT(&cg, "  %%r = call i64 @L_top()\n");
            EMIT(&cg, "  %%t = trunc i64 %%r to i32\n");
            EMIT(&cg, "  ret i32 %%t\n");
        } else
            EMIT(&cg, "  call void @L_top()\n");
    }
    if (!top_fn || !tops_main) EMIT(&cg, "  ret i32 0\n");
    EMIT(&cg, "}\n");

    free(top_fn);
    top_fn = NULL;

    /* ---- cleanup ---- */
    cg_free(&cg);

    return ir.data;
}
