/* codegen_sig.c — fill in the signatures the source left unannotated.

 * A lume function may leave `ret` and some `params[i]` unwritten; they are
 * inferred from the call sites and the `return` statements, written back onto
 * the AST, and then both native backends emit from the same agreed shape.
 * `codegen_infer_signatures()` is exported (codegen.h); the driver runs it
 * once before picking a backend, and re-runs it as a guard.
 *
 * The LTys table is the scratch list of bindings this pass builds; it exists
 * only while the pass runs, which is why it lives here and not in CG.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"

#include "codegen.h"
#include "lume.h"
#include "irbuf.h"
#include "codegen_internal.h"

/* forward: same tu, defined below this point. */
static void collect_known(CG *g, Node *n, LTys *t);

/* forward: same tu, defined below this point. */
static Type *infer_arg_type(CG *g, Node *a, LTys *t);

/* forward: same tu, defined below this point. */
static void infer_one_sig(CG *g, Sig *s);

/* forward: same tu, defined below this point. */
static void infer_sigs(CG *g);

/* forward: same tu, defined below this point. */
static void lt_free(LTys *t);

/* forward: same tu, defined below this point. */
static void lt_push(LTys *t, const char *name, Type *ty);

/* forward: same tu, defined below this point. */
static void type_desc(Type *t, char *buf, size_t n);

/* forward: same tu, defined below this point. */
static int types_eq(Type *a, Type *b);

/* forward: same tu, defined below this point. */
static int walk_sigs(CG *g, Node *n, LTys *t, Sig *s);

/* forward: same tu, defined below this point. */
static void ensure_lambda_registered(CG *g, Node *lambda);

/* forward: same tu, defined below this point. */
static void preregister_lambdas(CG *g, Node *n, int in_handler);

/* forward: same tu, defined below this point. */
static void lt_copy(LTys *dst, const LTys *src);

/* Grow-on-push node list (shared with the local-function passes below) and
 * the resolver context. Both must be declared before the forward prototypes
 * reference them: a struct tag first seen inside a prototype has prototype
 * scope only, and the later definition would become a different type. */

typedef struct NodeVec { Node **v; int n, cap; } NodeVec;

typedef struct LfCtx {
    int    counter;                 /* synthetic-name source */
    bool   failed;
    char  *err;
    size_t err_size;
} LfCtx;

/* forward: same tu, defined below this point. */
static void lf_walk_fn_body(LfCtx *c, Node *body);

/* forward: same tu, defined below this point. */
static void lf_scan_locals(Node *n, NodeVec *out);

/* forward: same tu, defined below this point. */
static void lf_rename_calls(Node *n, const char *from, const char *to);

/* forward: same tu, defined below this point. */
static void fn_collect_walk(Node *n, NodeVec *out);


/* Types already known for the names one function body can see. The pass below
 * has to type an argument like `f(n)` before the body is emitted, and `n` may
 * be one of that function's own parameters (whose type is the thing being
 * worked out) or a local declared with an annotated `let`. */

static void lt_push(LTys *t, const char *name, Type *ty)
{
    if (!name) return;
    for (int i = 0; i < t->n; i++)              /* a later binding wins */
        if (strcmp(t->v[i].name, name) == 0) {
            /* A NULL type is a placeholder (an untyped lambda parameter that
             * must still shadow a same-named global); never let it clobber a
             * type that is already known. */
            if (ty) t->v[i].ty = ty;
            return;
        }
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->v = (LTyp *)xrealloc(t->v, (size_t)t->cap * sizeof *t->v);
    }
    t->v[t->n].name = xstrdup(name);
    t->v[t->n].ty   = ty;
    t->n++;
}

/* Drop every binding of `name`. Used so an untyped lambda parameter still
 * shadows a same-named top-level global during inference: without this the
 * body's type scan would inherit the global's type for the parameter and
 * freeze the lambda's return at it before the parameter is ever hinted
 * (`let a: float = 1.5;` then `(a, x) => a + x * k` inside a function). */
static void lt_remove(LTys *t, const char *name)
{
    if (!name) return;
    int w = 0;
    for (int i = 0; i < t->n; i++) {
        if (strcmp(t->v[i].name, name) == 0) {
            free(t->v[i].name);
        } else {
            if (w != i) t->v[w] = t->v[i];
            w++;
        }
    }
    t->n = w;
}

static void lt_free(LTys *t)
{
    for (int i = 0; i < t->n; i++) free(t->v[i].name);
    free(t->v);
}

/* A fresh, owning copy: every name is strdup'd again so the source table can
 * outlive the copy (infer_one_sig copies `globals` per body and frees the
 * copy when the body is done). */

static void lt_copy(LTys *dst, const LTys *src)
{
    for (int i = 0; i < src->n; i++)
        lt_push(dst, src->v[i].name, src->v[i].ty);
}

Type *lt_find(LTys *t, const char *name)
{
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->v[i].name, name) == 0) return t->v[i].ty;
    return NULL;
}

/* Every name in scope with a type the backend can spell. An unannotated `let`
 * counts only when its initializer has an inferable type, which is the case
 * this whole section is here for. */
/* Only the statement shapes that can introduce a name are walked: a `let` can
 * appear inside blocks and the loop bodies, never inside an expression, so
 * descending into a literal's union fields would just read garbage pointers. */

static void collect_known(CG *g, Node *n, LTys *t)
{
    if (!n) return;

    if (n->type == N_LET) {
        /* Seed the scope with what collect_known has already gathered before
         * typing this initializer, so an init that references an earlier
         * top-level binding (`let s = sort(scores)`) can see that binding's
         * type. Without this the variable resolves to NULL here, the binding
         * is recorded with a NULL type, and an unannotated function
         * parameter fed the result stays "no known type" even though the
         * scan pass (pass 1.7) types the very same binding fine by walking
         * top-level lets in order with a live scope. Keeps the sig pass'
         * view of top-level bindings in step with the scan. */
        g->scope = *t;
        Type *ty = n->as.let.annot ? n->as.let.annot
                                   : infer_node_type(g, n->as.let.init);
        lt_push(t, n->as.let.name, ty);
        collect_known(g, n->as.let.init, t);
        return;
    }
    if (n->type == N_BLOCK || n->type == N_PROGRAM) {
        for (int i = 0; i < n->as.block.count; i++)
            collect_known(g, n->as.block.stmts[i], t);
    } else if (n->type == N_IF) {
        collect_known(g, n->as.ifs.cond, t);
        collect_known(g, n->as.ifs.then, t);
        collect_known(g, n->as.ifs.els, t);
    } else if (n->type == N_WHILE) {
        collect_known(g, n->as.whiles.cond, t);
        collect_known(g, n->as.whiles.body, t);
    } else if (n->type == N_FOR) {
        collect_known(g, n->as.fors.init, t);
        collect_known(g, n->as.fors.cond, t);
        collect_known(g, n->as.fors.incr, t);
        collect_known(g, n->as.fors.body, t);
        collect_known(g, n->as.fors.iterable, t);
    }
}

/* ---------- phase-2 free-variable capture ----------
 *
 * A lambda's body may read locals of its enclosing function (SPEC §4.7's
 * closure semantics). The interpreter shares one env chain, so a later write
 * to the captured variable is visible inside the closure; the native backend
 * reproduces that by storing the *address* of the outer slot into a capture
 * array on the closure record, and reading the variable back through it.
 *
 * This pass finds, for every named lambda, the outer locals its body reads
 * (name + type) and stores them on funclit.cap_*. It reruns each fixed-point
 * round of signature inference, so a capture type tracks the same iteration
 * that resolves the enclosing function's own returns. */

static void cap_reset(Node *lam)
{
    /* cap_* strings and arrays are arena-owned (TODO.md P2): the fixed-point
     * rerun just drops the counts and starts over. */
    lam->as.funclit.cap_names = NULL;
    lam->as.funclit.cap_types = NULL;
    lam->as.funclit.cap_count = 0;
}

static void cap_add(Node *lam, const char *name, Type *ty)
{
    for (int i = 0; i < lam->as.funclit.cap_count; i++)
        if (strcmp(lam->as.funclit.cap_names[i], name) == 0) return;
    int n = lam->as.funclit.cap_count;
    lam->as.funclit.cap_names =
        arena_grow(lam->as.funclit.cap_names, (size_t)n * sizeof(char *),
                   (size_t)(n + 1) * sizeof(char *));
    lam->as.funclit.cap_types =
        arena_grow(lam->as.funclit.cap_types, (size_t)n * sizeof(Type *),
                   (size_t)(n + 1) * sizeof(Type *));
    lam->as.funclit.cap_names[n] = arena_strdup(name);
    lam->as.funclit.cap_types[n] = ty;
    lam->as.funclit.cap_count = n + 1;
}

/* Walk one tree with the current lambda's own bindings (`self`: parameters
 * plus body locals) and the lexical outer bindings (`env`: globals + the
 * enclosing function's parameters and locals + outer lambda bindings). A free
 * N_VAR -- not a lambda-local, not a top-level global -- is a capture. */
static void cap_walk(CG *g, Node *n, LTys *self, LTys *env, Node *lam)
{
    if (!n) return;
    switch (n->type) {
    case N_VAR:
        if (!lam) return;
        /* Own param/local: a name match shadows regardless of whether the
         * parameter has a type yet (untyped params are pushed as NULL
         * placeholders so a same-named global is never captured). */
        for (int i = 0; i < self->n; i++)
            if (strcmp(self->v[i].name, n->as.var.name) == 0) return;
        /* Lexical outer bindings first: an enclosing function's local named
         * the same as a top-level global shadows it (`func f(){ let n = ...;
         * let g = () => n; }` with a top-level `let n` must capture the LOCAL,
         * not mistake n for the global and skip the capture). */
        {
            Type *ty = lt_find(env, n->as.var.name);
            if (ty) { cap_add(lam, n->as.var.name, ty); return; }
        }
        if (lt_find(&g->globals, n->as.var.name)) return;   /* top-level global */
        return;
    case N_FUNC_LIT: {
        if (!n->as.funclit.cname) return;   /* FFI-only literal: not compiled here */
        cap_reset(n);
        LTys cself = {0}, cenv = {0};
        for (int i = 0; i < n->as.funclit.arity; i++)
            if (n->as.funclit.names[i])
                lt_push(&cself, n->as.funclit.names[i],
                        i < n->as.funclit.arity ? n->as.funclit.param_types[i] : NULL);
        /* The child sees everything this walk sees (self ∪ env), so a lambda
         * nested inside a lambda can capture the outer lambda's own bindings
         * too. The body locals are collected against that full scope. */
        lt_copy(&cenv, env);
        for (int i = 0; i < self->n; i++)
            lt_push(&cenv, self->v[i].name, self->v[i].ty);
        g->scope = cenv;
        collect_known(g, n->as.funclit.body, &cself);
        cap_walk(g, n->as.funclit.body, &cself, &cenv, n);
        lt_free(&cenv);
        lt_free(&cself);
        g->scope = (LTys){0};
        return;
    }
    case N_PROGRAM: case N_BLOCK:
        for (int i = 0; i < n->as.block.count; i++)
            cap_walk(g, n->as.block.stmts[i], self, env, lam);
        return;
    case N_FUNC_DECL:
        cap_walk(g, n->as.func.body, self, env, lam);
        return;
    case N_LET:        cap_walk(g, n->as.let.init, self, env, lam); return;
    case N_LET_PAT:    cap_walk(g, n->as.letpat.init, self, env, lam); return;
    case N_LET_EXPR:
        cap_walk(g, n->as.letx.init, self, env, lam);
        cap_walk(g, n->as.letx.body, self, env, lam);
        return;
    case N_IF:
        cap_walk(g, n->as.ifs.cond, self, env, lam);
        cap_walk(g, n->as.ifs.then, self, env, lam);
        cap_walk(g, n->as.ifs.els,  self, env, lam);
        return;
    case N_IF_LET:
        cap_walk(g, n->as.iflet.then, self, env, lam);
        cap_walk(g, n->as.iflet.els,  self, env, lam);
        return;
    case N_WHILE:
        cap_walk(g, n->as.whiles.cond, self, env, lam);
        cap_walk(g, n->as.whiles.body, self, env, lam);
        return;
    case N_FOR:
        cap_walk(g, n->as.fors.init,     self, env, lam);
        cap_walk(g, n->as.fors.cond,     self, env, lam);
        cap_walk(g, n->as.fors.incr,     self, env, lam);
        cap_walk(g, n->as.fors.iterable, self, env, lam);
        cap_walk(g, n->as.fors.body,     self, env, lam);
        return;
    case N_RETURN:     cap_walk(g, n->as.ret.expr, self, env, lam); return;
    case N_EXPR_STMT:  cap_walk(g, n->as.expr_stmt.expr, self, env, lam); return;
    case N_ASSIGN:
        cap_walk(g, n->as.assign.value, self, env, lam);
        return;
    case N_ASSIGN_MEMBER:
        cap_walk(g, n->as.assign_mem.obj,   self, env, lam);
        cap_walk(g, n->as.assign_mem.value, self, env, lam);
        return;
    case N_CALL:
        cap_walk(g, n->as.call.callee, self, env, lam);
        for (int i = 0; i < n->as.call.argc; i++)
            cap_walk(g, n->as.call.args[i], self, env, lam);
        return;
    case N_MEMBER:     cap_walk(g, n->as.member.obj, self, env, lam); return;
    case N_INDEX:
        cap_walk(g, n->as.index.obj,   self, env, lam);
        cap_walk(g, n->as.index.index, self, env, lam);
        return;
    case N_UNARY:      cap_walk(g, n->as.unary.operand, self, env, lam); return;
    case N_BINARY:
        cap_walk(g, n->as.binary.left,  self, env, lam);
        cap_walk(g, n->as.binary.right, self, env, lam);
        return;
    case N_TERNARY:
        cap_walk(g, n->as.ternary.cond, self, env, lam);
        cap_walk(g, n->as.ternary.then, self, env, lam);
        cap_walk(g, n->as.ternary.els,  self, env, lam);
        return;
    case N_MATCH:
        cap_walk(g, n->as.matchn.scrutinee, self, env, lam);
        for (int i = 0; i < n->as.matchn.armc; i++) {
            cap_walk(g, n->as.matchn.arms[i].guard, self, env, lam);
            cap_walk(g, n->as.matchn.arms[i].body,  self, env, lam);
        }
        return;
    case N_CTOR:
        for (int i = 0; i < n->as.ctor.argc; i++)
            cap_walk(g, n->as.ctor.args[i], self, env, lam);
        return;
    case N_LIST_LIT:
        for (int i = 0; i < n->as.list.count; i++)
            cap_walk(g, n->as.list.items[i], self, env, lam);
        return;
    case N_MAP_LIT:
        for (int i = 0; i < n->as.map.count; i++)
            cap_walk(g, n->as.map.vals[i], self, env, lam);
        return;
    case N_STRUCT_UPDATE:
        cap_walk(g, n->as.supd.base, self, env, lam);
        for (int i = 0; i < n->as.supd.count; i++)
            cap_walk(g, n->as.supd.vals[i], self, env, lam);
        return;
    default: return;
    }
}

/* Run the capture walk over one function: its parameters and body locals are
 * `self` (not captured), everything lexical-outer is the capture source. */
static void capture_func(CG *g, Node *fn)
{
    LTys self = {0}, env = {0}, full = {0};
    for (int i = 0; i < fn->as.func.arity; i++)
        if (fn->as.func.names[i])
            lt_push(&self, fn->as.func.names[i],
                    i < fn->as.func.arity ? fn->as.func.param_types[i] : NULL);
    lt_copy(&env, &g->globals);
    lt_copy(&full, &g->globals);
    for (int i = 0; i < self.n; i++)
        lt_push(&full, self.v[i].name, self.v[i].ty);
    g->scope = full;
    collect_known(g, fn->as.func.body, &self);
    lt_free(&full);
    cap_walk(g, fn->as.func.body, &self, &env, NULL);
    lt_free(&env);
    lt_free(&self);
    g->scope = (LTys){0};
}

/* Refresh every lambda's capture table. Idempotent: it clears and refills, so
 * running it again after inference advanced the enclosing types is safe. */
static void capture_pass(CG *g)
{
    Node *prog = g->prog;
    if (!prog || prog->type != N_PROGRAM) return;
    for (int i = 0; i < prog->as.program.count; i++) {
        Node *n = prog->as.program.stmts[i];
        if (n && n->type == N_FUNC_DECL)
            capture_func(g, n);
    }
}

/* The type of one argument: a bare name first (the body's own bindings), then
 * the expression's own shape. */

static Type *infer_arg_type(CG *g, Node *a, LTys *t)
{
    if (a && a->type == N_VAR) {
        Type *ty = lt_find(t, a->as.var.name);
        if (ty) return ty;
    }
    return infer_node_type(g, a);
}

/* Assign a lambda its synthetic mangled name (once) and register a `Sig` for
 * it so the rest of the pass can pin its parameters and infer its return type
 * on the same fixed point as a named function. The name is written onto the
 * node (`funclit.cname`) so both native backends read the same spelling when
 * they later emit the closure's `define`. Idempotent: a lambda visited twice
 * (e.g. once as a let initializer, once via a call site) keeps its first name. */

static void ensure_lambda_registered(CG *g, Node *lambda)
{
    if (!lambda || lambda->type != N_FUNC_LIT) return;
    if (lambda->as.funclit.cname) return;

    char buf[32];
    snprintf(buf, sizeof buf, "__lume_clo_%d", g->nclo++);
    lambda->as.funclit.cname = arena_strdup(buf);

    sig_push(&g->sigs, buf, lambda->as.funclit.ret,
             lambda->as.funclit.param_types, lambda->as.funclit.arity);
    Sig *s = &g->sigs.v[g->sigs.n - 1];
    s->is_lambda = 1;
    s->body   = lambda->as.funclit.body;
    s->pnames = lambda->as.funclit.names;
    s->node   = lambda;
}

/* Register every lambda in the tree before inference starts.
 *
 * Registration used to happen only where a call site reached a lambda, which
 * misses the shapes that evaluate a lambda without calling it: an assignment
 * right-hand side (`f = (x) => ...`), a lambda body calling another lambda
 * (the callee is not a local of the body being walked, so the closure branch
 * never fires), a let whose variable is used later. Any of those reached the
 * emitter with a NULL cname and died on an "internal:" message. Registering
 * up front also removes a subtler hazard: mid-round registration pushes onto
 * `g->sigs`, whose realloc would dangle the `Sig *s` the walk is carrying.
 *
 * Route/tool/verb handler literals are skipped (`in_handler`): they are
 * emitted through the handler path, and stamping a cname on them would make
 * the emitters' cname-gated collection emit a second `define` for the same
 * literal. Lambdas nested *inside* a handler body are registered — only the
 * handler literal itself is skipped. */

static void preregister_lambdas(CG *g, Node *n, int in_handler)
{
    if (!n) return;
    switch (n->type) {
    case N_FUNC_LIT:
        if (!in_handler)
            ensure_lambda_registered(g, n);
        preregister_lambdas(g, n->as.funclit.body, 0);
        return;
    case N_PROGRAM: case N_BLOCK:
        for (int i = 0; i < n->as.block.count; i++)
            preregister_lambdas(g, n->as.block.stmts[i], in_handler);
        return;
    case N_FUNC_DECL:
        preregister_lambdas(g, n->as.func.body, 0);
        return;
    case N_LET:        preregister_lambdas(g, n->as.let.init, 0); return;
    case N_LET_PAT:    preregister_lambdas(g, n->as.letpat.init, 0); return;
    case N_LET_EXPR:
        preregister_lambdas(g, n->as.letx.init, 0);
        preregister_lambdas(g, n->as.letx.body, 0);
        return;
    case N_IF:
        preregister_lambdas(g, n->as.ifs.cond, 0);
        preregister_lambdas(g, n->as.ifs.then, 0);
        preregister_lambdas(g, n->as.ifs.els,  0);
        return;
    case N_WHILE:
        preregister_lambdas(g, n->as.whiles.cond, 0);
        preregister_lambdas(g, n->as.whiles.body, 0);
        return;
    case N_FOR:
        preregister_lambdas(g, n->as.fors.init,     0);
        preregister_lambdas(g, n->as.fors.cond,     0);
        preregister_lambdas(g, n->as.fors.incr,     0);
        preregister_lambdas(g, n->as.fors.iterable, 0);
        preregister_lambdas(g, n->as.fors.body,     0);
        return;
    case N_RETURN:     preregister_lambdas(g, n->as.ret.expr, 0); return;
    case N_EXPR_STMT:  preregister_lambdas(g, n->as.expr_stmt.expr, 0); return;
    case N_ASSIGN:
        preregister_lambdas(g, n->as.assign.value, 0);
        return;
    case N_ASSIGN_MEMBER:
        preregister_lambdas(g, n->as.assign_mem.obj,   0);
        preregister_lambdas(g, n->as.assign_mem.value, 0);
        return;
    case N_CALL:
        preregister_lambdas(g, n->as.call.callee, 0);
        for (int i = 0; i < n->as.call.argc; i++)
            preregister_lambdas(g, n->as.call.args[i], 0);
        return;
    case N_MEMBER:     preregister_lambdas(g, n->as.member.obj, 0); return;
    case N_INDEX:
        preregister_lambdas(g, n->as.index.obj,   0);
        preregister_lambdas(g, n->as.index.index, 0);
        return;
    case N_UNARY:      preregister_lambdas(g, n->as.unary.operand, 0); return;
    case N_BINARY:
        preregister_lambdas(g, n->as.binary.left,  0);
        preregister_lambdas(g, n->as.binary.right, 0);
        return;
    case N_TERNARY:
        preregister_lambdas(g, n->as.ternary.cond, 0);
        preregister_lambdas(g, n->as.ternary.then, 0);
        preregister_lambdas(g, n->as.ternary.els,  0);
        return;
    case N_MATCH:
        preregister_lambdas(g, n->as.matchn.scrutinee, 0);
        for (int i = 0; i < n->as.matchn.armc; i++)
            preregister_lambdas(g, n->as.matchn.arms[i].body, 0);
        return;
    case N_CTOR:
        for (int i = 0; i < n->as.ctor.argc; i++)
            preregister_lambdas(g, n->as.ctor.args[i], 0);
        return;
    case N_LIST_LIT:
        for (int i = 0; i < n->as.list.count; i++)
            preregister_lambdas(g, n->as.list.items[i], 0);
        return;
    case N_MAP_LIT:
        for (int i = 0; i < n->as.map.count; i++)
            preregister_lambdas(g, n->as.map.vals[i], 0);
        return;
    case N_SERVER:
        for (int i = 0; i < n->as.server.count; i++)
            preregister_lambdas(g, n->as.server.assigns[i], 0);
        return;
    case N_ROUTE:      preregister_lambdas(g, n->as.route.handler, 1); return;
    case N_TOOL:
        preregister_lambdas(g, n->as.tool.params,  0);
        preregister_lambdas(g, n->as.tool.handler, 1);
        return;
    case N_VERBS:      preregister_lambdas(g, n->as.verbs.methods, 1); return;
    default: return;   /* literals, imports, type decls, break/continue */
    }
}

/* ===================== local functions (SPEC §4.14) =====================
 *
 * A `func` declared inside a function body is a local function. It compiles
 * like any module function — locals do not capture — but it must resolve
 * nowhere except inside the function that declares it. The mechanism is a
 * rewrite run before the checker and again before native codegen:
 *
 *   1. every nested N_FUNC_DECL gets a synthetic module-level name (`cname`);
 *   2. every call inside the enclosing function that names the local is
 *      rewritten to that name.
 *
 * After the rewrite the source name exists in no scope at all: calls inside
 * the enclosing function carry the cname (which the checker registers and
 * the emitters define), while a call anywhere else still spells the source
 * name, which resolves to nothing — an undefined variable in all three
 * backends, which is exactly the scoping a local declaration should have.
 * The rename reaches into lambda bodies and nested local functions too, so
 * recursion and calls from inner scopes land on the same define. Two locals
 * in one enclosing function may not share a name: the blanket rename inside
 * that function would send both call sets to whichever cname was assigned
 * last, so it is diagnosed instead.
 */

/* Grow-on-push node list. Pointers only — nothing here owns a Node. (The
 * typedef itself lives above the forward prototypes at the top of the file;
 * see the note there.) */

static void nv_push(NodeVec *s, Node *n)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = (Node **)xrealloc(s->v, (size_t)s->cap * sizeof *s->v);
    }
    s->v[s->n++] = n;
}

/* Every function declaration in the tree, pre-order (a parent always sits
 * before the locals nested inside it). Shared by the checker's registration
 * pass and both native backends' signature/emit collection, so all four see
 * exactly the same function set. */

static void fn_collect_walk(Node *n, NodeVec *out)
{
    if (!n) return;
    switch (n->type) {
    case N_FUNC_DECL:
        nv_push(out, n);
        fn_collect_walk(n->as.func.body, out);
        return;
    case N_FUNC_LIT:
        fn_collect_walk(n->as.funclit.body, out);
        return;
    case N_PROGRAM: case N_BLOCK:
        for (int i = 0; i < n->as.block.count; i++)
            fn_collect_walk(n->as.block.stmts[i], out);
        return;
    case N_LET:        fn_collect_walk(n->as.let.init, out); return;
    case N_LET_PAT:    fn_collect_walk(n->as.letpat.init, out); return;
    case N_LET_EXPR:
        fn_collect_walk(n->as.letx.init, out);
        fn_collect_walk(n->as.letx.body, out);
        return;
    case N_IF:
        fn_collect_walk(n->as.ifs.cond, out);
        fn_collect_walk(n->as.ifs.then, out);
        fn_collect_walk(n->as.ifs.els,  out);
        return;
    case N_WHILE:
        fn_collect_walk(n->as.whiles.cond, out);
        fn_collect_walk(n->as.whiles.body, out);
        return;
    case N_FOR:
        fn_collect_walk(n->as.fors.init,     out);
        fn_collect_walk(n->as.fors.cond,     out);
        fn_collect_walk(n->as.fors.incr,     out);
        fn_collect_walk(n->as.fors.iterable, out);
        fn_collect_walk(n->as.fors.body,     out);
        return;
    case N_RETURN:     fn_collect_walk(n->as.ret.expr, out); return;
    case N_EXPR_STMT:  fn_collect_walk(n->as.expr_stmt.expr, out); return;
    case N_ASSIGN:     fn_collect_walk(n->as.assign.value, out); return;
    case N_ASSIGN_MEMBER:
        fn_collect_walk(n->as.assign_mem.obj,   out);
        fn_collect_walk(n->as.assign_mem.value, out);
        return;
    case N_CALL:
        fn_collect_walk(n->as.call.callee, out);
        for (int i = 0; i < n->as.call.argc; i++)
            fn_collect_walk(n->as.call.args[i], out);
        return;
    case N_MEMBER:     fn_collect_walk(n->as.member.obj, out); return;
    case N_INDEX:
        fn_collect_walk(n->as.index.obj,   out);
        fn_collect_walk(n->as.index.index, out);
        return;
    case N_UNARY:      fn_collect_walk(n->as.unary.operand, out); return;
    case N_BINARY:
        fn_collect_walk(n->as.binary.left,  out);
        fn_collect_walk(n->as.binary.right, out);
        return;
    case N_TERNARY:
        fn_collect_walk(n->as.ternary.cond, out);
        fn_collect_walk(n->as.ternary.then, out);
        fn_collect_walk(n->as.ternary.els,  out);
        return;
    case N_MATCH:
        fn_collect_walk(n->as.matchn.scrutinee, out);
        for (int i = 0; i < n->as.matchn.armc; i++) {
            fn_collect_walk(n->as.matchn.arms[i].guard, out);
            fn_collect_walk(n->as.matchn.arms[i].body,  out);
        }
        return;
    case N_CTOR:
        for (int i = 0; i < n->as.ctor.argc; i++)
            fn_collect_walk(n->as.ctor.args[i], out);
        return;
    case N_LIST_LIT:
        for (int i = 0; i < n->as.list.count; i++)
            fn_collect_walk(n->as.list.items[i], out);
        return;
    case N_MAP_LIT:
        for (int i = 0; i < n->as.map.count; i++)
            fn_collect_walk(n->as.map.vals[i], out);
        return;
    case N_SERVER:
        for (int i = 0; i < n->as.server.count; i++)
            fn_collect_walk(n->as.server.assigns[i]->as.assign.value, out);
        return;
    case N_ROUTE:      fn_collect_walk(n->as.route.handler, out); return;
    case N_TOOL:
        fn_collect_walk(n->as.tool.params,  out);
        fn_collect_walk(n->as.tool.handler, out);
        return;
    case N_VERBS:      fn_collect_walk(n->as.verbs.methods, out); return;
    default: return;   /* literals, imports, type/enum decls, break/continue */
    }
}

int lume_collect_funcs(Node *prog, Node **out, int cap)
{
    if (!prog || prog->type != N_PROGRAM) return 0;
    NodeVec v = {0};
    fn_collect_walk(prog, &v);
    int n = v.n;
    if (out)
        for (int i = 0; i < n && i < cap; i++) out[i] = v.v[i];
    free(v.v);
    return n;
}

/* Rename every call that spells `from` into `to`. The walk is total — it
 * descends into lambda bodies and nested function bodies — because a local
 * is visible throughout its enclosing function once hoisted, and two locals
 * in one enclosing function cannot share a name (diagnosed below), so no
 * inner declaration can shadow the name mid-walk. */

static void lf_rename_calls(Node *n, const char *from, const char *to)
{
    if (!n) return;
    switch (n->type) {
    case N_FUNC_DECL:
        lf_rename_calls(n->as.func.body, from, to);
        return;
    case N_FUNC_LIT:
        lf_rename_calls(n->as.funclit.body, from, to);
        return;
    case N_PROGRAM: case N_BLOCK:
        for (int i = 0; i < n->as.block.count; i++)
            lf_rename_calls(n->as.block.stmts[i], from, to);
        return;
    case N_LET:        lf_rename_calls(n->as.let.init, from, to); return;
    case N_LET_PAT:    lf_rename_calls(n->as.letpat.init, from, to); return;
    case N_LET_EXPR:
        lf_rename_calls(n->as.letx.init, from, to);
        lf_rename_calls(n->as.letx.body, from, to);
        return;
    case N_IF:
        lf_rename_calls(n->as.ifs.cond, from, to);
        lf_rename_calls(n->as.ifs.then, from, to);
        lf_rename_calls(n->as.ifs.els,  from, to);
        return;
    case N_WHILE:
        lf_rename_calls(n->as.whiles.cond, from, to);
        lf_rename_calls(n->as.whiles.body, from, to);
        return;
    case N_FOR:
        lf_rename_calls(n->as.fors.init,     from, to);
        lf_rename_calls(n->as.fors.cond,     from, to);
        lf_rename_calls(n->as.fors.incr,     from, to);
        lf_rename_calls(n->as.fors.iterable, from, to);
        lf_rename_calls(n->as.fors.body,     from, to);
        return;
    case N_RETURN:     lf_rename_calls(n->as.ret.expr, from, to); return;
    case N_EXPR_STMT:  lf_rename_calls(n->as.expr_stmt.expr, from, to); return;
    case N_ASSIGN:     lf_rename_calls(n->as.assign.value, from, to); return;
    case N_ASSIGN_MEMBER:
        lf_rename_calls(n->as.assign_mem.obj,   from, to);
        lf_rename_calls(n->as.assign_mem.value, from, to);
        return;
    case N_CALL: {
        Node *ce = n->as.call.callee;
        if (ce && ce->type == N_VAR && strcmp(ce->as.var.name, from) == 0) {
            /* the source name stays reachable (arena); the new name simply
             * replaces the pointer */
            ce->as.var.name = arena_strdup(to);
        }
        lf_rename_calls(ce, from, to);
        for (int i = 0; i < n->as.call.argc; i++)
            lf_rename_calls(n->as.call.args[i], from, to);
        return;
    }
    case N_MEMBER:     lf_rename_calls(n->as.member.obj, from, to); return;
    case N_INDEX:
        lf_rename_calls(n->as.index.obj,   from, to);
        lf_rename_calls(n->as.index.index, from, to);
        return;
    case N_UNARY:      lf_rename_calls(n->as.unary.operand, from, to); return;
    case N_BINARY:
        lf_rename_calls(n->as.binary.left,  from, to);
        lf_rename_calls(n->as.binary.right, from, to);
        return;
    case N_TERNARY:
        lf_rename_calls(n->as.ternary.cond, from, to);
        lf_rename_calls(n->as.ternary.then, from, to);
        lf_rename_calls(n->as.ternary.els,  from, to);
        return;
    case N_MATCH:
        lf_rename_calls(n->as.matchn.scrutinee, from, to);
        for (int i = 0; i < n->as.matchn.armc; i++) {
            lf_rename_calls(n->as.matchn.arms[i].guard, from, to);
            lf_rename_calls(n->as.matchn.arms[i].body,  from, to);
        }
        return;
    case N_CTOR:
        for (int i = 0; i < n->as.ctor.argc; i++)
            lf_rename_calls(n->as.ctor.args[i], from, to);
        return;
    case N_LIST_LIT:
        for (int i = 0; i < n->as.list.count; i++)
            lf_rename_calls(n->as.list.items[i], from, to);
        return;
    case N_MAP_LIT:
        for (int i = 0; i < n->as.map.count; i++)
            lf_rename_calls(n->as.map.vals[i], from, to);
        return;
    case N_SERVER:
        for (int i = 0; i < n->as.server.count; i++)
            lf_rename_calls(n->as.server.assigns[i]->as.assign.value, from, to);
        return;
    case N_ROUTE:      lf_rename_calls(n->as.route.handler, from, to); return;
    case N_TOOL:
        lf_rename_calls(n->as.tool.params,  from, to);
        lf_rename_calls(n->as.tool.handler, from, to);
        return;
    case N_VERBS:      lf_rename_calls(n->as.verbs.methods, from, to); return;
    default: return;
    }
}

/* The local functions declared directly inside one body: statement level
 * plus any if/while/for/block below it. A nested function's own body is NOT
 * descended into here — its locals are a deeper scope, walked separately by
 * lf_walk_fn_body. Lambda bodies ARE descended into: a `func` inside a
 * lambda body belongs to the enclosing function's scope and the rename
 * reaches lambda bodies too, so the two walks agree. */

static void lf_scan_locals(Node *n, NodeVec *out)
{
    if (!n) return;
    switch (n->type) {
    case N_FUNC_DECL:
        nv_push(out, n);            /* collected, not descended into */
        return;
    case N_FUNC_LIT:
        lf_scan_locals(n->as.funclit.body, out);
        return;
    case N_PROGRAM: case N_BLOCK:
        for (int i = 0; i < n->as.block.count; i++)
            lf_scan_locals(n->as.block.stmts[i], out);
        return;
    case N_LET:        lf_scan_locals(n->as.let.init, out); return;
    case N_LET_PAT:    lf_scan_locals(n->as.letpat.init, out); return;
    case N_LET_EXPR:
        lf_scan_locals(n->as.letx.init, out);
        lf_scan_locals(n->as.letx.body, out);
        return;
    case N_IF:
        lf_scan_locals(n->as.ifs.cond, out);
        lf_scan_locals(n->as.ifs.then, out);
        lf_scan_locals(n->as.ifs.els,  out);
        return;
    case N_WHILE:
        lf_scan_locals(n->as.whiles.cond, out);
        lf_scan_locals(n->as.whiles.body, out);
        return;
    case N_FOR:
        lf_scan_locals(n->as.fors.init,     out);
        lf_scan_locals(n->as.fors.cond,     out);
        lf_scan_locals(n->as.fors.incr,     out);
        lf_scan_locals(n->as.fors.iterable, out);
        lf_scan_locals(n->as.fors.body,     out);
        return;
    case N_RETURN:     lf_scan_locals(n->as.ret.expr, out); return;
    case N_EXPR_STMT:  lf_scan_locals(n->as.expr_stmt.expr, out); return;
    case N_ASSIGN:     lf_scan_locals(n->as.assign.value, out); return;
    case N_ASSIGN_MEMBER:
        lf_scan_locals(n->as.assign_mem.obj,   out);
        lf_scan_locals(n->as.assign_mem.value, out);
        return;
    case N_CALL:
        lf_scan_locals(n->as.call.callee, out);
        for (int i = 0; i < n->as.call.argc; i++)
            lf_scan_locals(n->as.call.args[i], out);
        return;
    case N_MEMBER:     lf_scan_locals(n->as.member.obj, out); return;
    case N_INDEX:
        lf_scan_locals(n->as.index.obj,   out);
        lf_scan_locals(n->as.index.index, out);
        return;
    case N_UNARY:      lf_scan_locals(n->as.unary.operand, out); return;
    case N_BINARY:
        lf_scan_locals(n->as.binary.left,  out);
        lf_scan_locals(n->as.binary.right, out);
        return;
    case N_TERNARY:
        lf_scan_locals(n->as.ternary.cond, out);
        lf_scan_locals(n->as.ternary.then, out);
        lf_scan_locals(n->as.ternary.els,  out);
        return;
    case N_MATCH:
        lf_scan_locals(n->as.matchn.scrutinee, out);
        for (int i = 0; i < n->as.matchn.armc; i++) {
            lf_scan_locals(n->as.matchn.arms[i].guard, out);
            lf_scan_locals(n->as.matchn.arms[i].body,  out);
        }
        return;
    case N_CTOR:
        for (int i = 0; i < n->as.ctor.argc; i++)
            lf_scan_locals(n->as.ctor.args[i], out);
        return;
    case N_LIST_LIT:
        for (int i = 0; i < n->as.list.count; i++)
            lf_scan_locals(n->as.list.items[i], out);
        return;
    case N_MAP_LIT:
        for (int i = 0; i < n->as.map.count; i++)
            lf_scan_locals(n->as.map.vals[i], out);
        return;
    default: return;
    }
}

/* (LfCtx typedef lives above the forward prototypes at the top of the file.) */

static void lf_fail(LfCtx *c, Node *n, const char *fmt, ...)
{
    if (c->failed) return;
    c->failed = true;
    if (!c->err || !c->err_size) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(c->err, c->err_size, "line %zu: %s", n ? n->line : 0, msg);
}

/* Name, stamp and rewrite the locals of one body, then recurse into each
 * local's own body. Idempotent: a body whose locals all carry a cname is
 * skipped wholesale, so the checker's run and the codegen guard's run do no
 * double work. */

static void lf_walk_fn_body(LfCtx *c, Node *body)
{
    if (!body || c->failed) return;

    NodeVec v = {0};
    lf_scan_locals(body, &v);
    if (v.n == 0) { free(v.v); return; }

    bool all_done = true;
    for (int i = 0; i < v.n; i++)
        if (!v.v[i]->as.func.cname) { all_done = false; break; }
    if (all_done) { free(v.v); return; }

    /* Duplicate source names inside one enclosing function are rejected
     * before anything is stamped, so a failed program never leaves a
     * half-renamed tree behind. */
    for (int i = 0; i < v.n && !c->failed; i++)
        for (int j = i + 1; j < v.n && !c->failed; j++)
            if (!v.v[i]->as.func.cname && !v.v[j]->as.func.cname &&
                strcmp(v.v[i]->as.func.name, v.v[j]->as.func.name) == 0)
                lf_fail(c, v.v[j],
                        "local function '%s' is declared twice in the same "
                        "function; local function names must be unique "
                        "within their enclosing function",
                        v.v[j]->as.func.name);

    for (int i = 0; i < v.n && !c->failed; i++) {
        Node *L = v.v[i];
        if (L->as.func.cname) continue;
        char buf[32];
        snprintf(buf, sizeof buf, "__lume_lf_%d", c->counter++);
        L->as.func.cname = arena_strdup(buf);
        lf_rename_calls(body, L->as.func.name, L->as.func.cname);
    }

    for (int i = 0; i < v.n && !c->failed; i++)
        lf_walk_fn_body(c, v.v[i]->as.func.body);

    free(v.v);
}

bool lume_resolve_localfns(Node *prog, char *errbuf, size_t errbuf_size)
{
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    if (!prog || prog->type != N_PROGRAM) return true;

    LfCtx c = {0};
    c.err      = errbuf;
    c.err_size = errbuf_size;

    NodeVec fns = {0};
    fn_collect_walk(prog, &fns);
    for (int i = 0; i < fns.n && !c.failed; i++)
        lf_walk_fn_body(&c, fns.v[i]->as.func.body);
    free(fns.v);

    return !c.failed;
}



static int types_eq(Type *a, Type *b)
{
    if (a == b) return 1;
    if (!a || !b || a->kind != b->kind) return 0;
    if (a->kind == TY_STRUCT) {
        /* A struct literal is anonymous and still fills a named parameter:
         * `sq_dist({ x: 3, y: 4 }, ..)` against `Point` is the normal way to
         * call it, so an unnamed struct matches any named one. */
        if (!a->name || !b->name) return 1;
        return strcmp(a->name, b->name) == 0;
    }
    if (a->kind == TY_ENUM) {
        /* Two enums with different names are different types even though
         * both travel as i8*: `describe(c: Color)` must not silently accept
         * a `Tag` value. Unnamed shells match anything named, as structs
         * do. */
        if (!a->name || !b->name) return 1;
        return strcmp(a->name, b->name) == 0;
    }
    if (a->kind == TY_LIST) return types_eq(a->elem, b->elem);
    return 1;                     /* every remaining kind is a primitive */
}

/* A name for a type, for diagnostics. */

static void type_desc(Type *t, char *buf, size_t n)
{
    switch (t ? t->kind : TY_NULL) {
    case TY_INT:    snprintf(buf, n, "int"); break;
    case TY_FLOAT:  snprintf(buf, n, "float"); break;
    case TY_STRING: snprintf(buf, n, "string"); break;
    case TY_BOOL:   snprintf(buf, n, "bool"); break;
    case TY_LIST:   { type_desc(t->elem, buf, n);
                      size_t len = strlen(buf);
                      if (len + 2 < n) { buf[len] = '['; buf[len + 1] = ']'; buf[len + 2] = '\0'; }
                      break; }
    case TY_STRUCT: snprintf(buf, n, "%s", t->name ? t->name : "struct"); break;
    case TY_ENUM:   snprintf(buf, n, "%s", t->name ? t->name : "enum"); break;
    default:        snprintf(buf, n, "?"); break;
    }
}

/* Walk a function body: type every argument whose parameter is still unknown,
 * and take the return type from the first `return` that has an inferable
 * expression. Returns the number of slots it filled, which is what drives the
 * fixed-point loop below. ERRX on the first thing it cannot name. */

static int walk_sigs(CG *g, Node *n, LTys *t, Sig *s)
{
    if (!n) return 0;
    int changed = 0;

    switch (n->type) {
    case N_CALL: {
        Node *c = n->as.call.callee;
        int argc = n->as.call.argc;

        /* Closure-valued variable call: `f(3)` where f : func. Pin the lambda's
         * parameters from the call arguments (the same fixed-point game as a
         * named function), then keep walking. The lambda node is reached
         * through the variable's TY_FUNC type, which carries a back-pointer to
         * the N_FUNC_LIT it was spelled from. */
        if (c && c->type == N_VAR) {
            Type *ct = infer_node_type(g, c);
            if (ct && ct->kind == TY_FUNC && ct->func &&
                ct->func->type == N_FUNC_LIT) {
                Node *lambda = ct->func;
                ensure_lambda_registered(g, lambda);
                Sig *ls = sig_find(&g->sigs, lambda->as.funclit.cname);
                if (ls) {
                    for (int i = 0; i < argc && i < ls->arity; i++) {
                        Type *at = infer_arg_type(g, n->as.call.args[i], t);
                        if (!at) continue;
                        if (ls->params[i] && !types_eq(ls->params[i], at)) {
                            char want[32], got[32];
                            type_desc(ls->params[i], want, sizeof want);
                            type_desc(at, got, sizeof got);
                            ERR_SET(g, "line %zu: argument %d of the closure is a "
                                       "%s, but the parameter was already resolved "
                                       "as a %s", n->line, i + 1, got, want);
                            return changed;
                        }
                        if (ls->params[i]) continue;
                        ls->params[i] = at;
                        changed++;
                    }
                }
                changed += walk_sigs(g, c, t, s);
                for (int i = 0; i < argc; i++)
                    changed += walk_sigs(g, n->as.call.args[i], t, s);
                return changed;
            }
        }

        /* Higher-order builtins that consume a closure: map/filter/reduce.
         * Pin the lambda's parameter(s) from the list / accumulator argument
         * — `map(f, list)` pins param[0] from the list element type,
         * `reduce(f, list, init)` pins param[0] from the init (acc) type and
         * param[1] from the list element type. */
        if (c && c->type == N_VAR) {
            const char *bn = c->as.var.name;
            if (strcmp(bn, "map") == 0 || strcmp(bn, "filter") == 0 ||
                strcmp(bn, "reduce") == 0) {
                Node *clo = argc > 0 ? n->as.call.args[0] : NULL;
                Node *lambda = NULL;
                if (clo && clo->type == N_FUNC_LIT) lambda = clo;
                else if (clo && clo->type == N_VAR) {
                    Type *ct = infer_node_type(g, clo);
                    if (ct && ct->kind == TY_FUNC && ct->func &&
                        ct->func->type == N_FUNC_LIT)
                        lambda = ct->func;
                }
                if (lambda) {
                    ensure_lambda_registered(g, lambda);
                    Sig *ls = sig_find(&g->sigs, lambda->as.funclit.cname);
                    if (ls) {
                        if (strcmp(bn, "reduce") == 0) {
                            Type *init_t = argc >= 3
                                ? infer_arg_type(g, n->as.call.args[2], t) : NULL;
                            Type *list_t = argc >= 2
                                ? infer_arg_type(g, n->as.call.args[1], t) : NULL;
                            /* An empty/any list never calls the lambda (len ==
                             * 0), so int is a safe spellable element type. */
                            Type *et = (list_t && list_t->kind == TY_LIST)
                                ? (list_t->elem && list_t->elem->kind != TY_ANY
                                   ? list_t->elem : type_prim(TY_INT))
                                : NULL;
                            if (init_t && !ls->params[0]) { ls->params[0] = init_t; changed++; }
                            if (et && !ls->params[1]) { ls->params[1] = et; changed++; }
                        } else {
                            Type *list_t = argc >= 2
                                ? infer_arg_type(g, n->as.call.args[1], t) : NULL;
                            Type *et = (list_t && list_t->kind == TY_LIST)
                                ? (list_t->elem && list_t->elem->kind != TY_ANY
                                   ? list_t->elem : type_prim(TY_INT))
                                : NULL;
                            if (et && !ls->params[0]) { ls->params[0] = et; changed++; }
                        }
                    }
                }
                changed += walk_sigs(g, c, t, s);
                for (int i = 0; i < argc; i++)
                    changed += walk_sigs(g, n->as.call.args[i], t, s);
                return changed;
            }
        }

        Sig *cs = (c && c->type == N_VAR) ? sig_find(&g->sigs, c->as.var.name) : NULL;
        if (cs) {
            /* One argument at a time: filling a parameter's type makes that
             * name known for the *arguments after* it in the same call. */
            for (int i = 0; i < argc && i < cs->arity; i++) {
                /* The argument's own type is always asked for: a parameter
                 * that is already resolved can only be checked against it. */
                Type *at = infer_arg_type(g, n->as.call.args[i], t);
                if (!at) continue;
                /* An `any` argument carries no information: it can neither
                 * contradict a resolved parameter nor fill an empty slot
                 * (filling it would freeze the parameter at "untypable" and
                 * reject the very call that would have resolved it — the
                 * checker itself accepts `any` wherever the resolved type
                 * is accepted). `describe(xs[0])` on a list the scanner
                 * could not type is the everyday case. */
                if (at->kind == TY_ANY) continue;
                Type *seen = cs->params[i];
                if (seen && !types_eq(seen, at)) {
                    /* One name, one signature: a second call contradicting the
                     * inferred type is not an overload. Without this the
                     * emitter would coerce the string to the inferred int and
                     * print pointer garbage at run time, where nothing points
                     * back at the source. */
                    char want[32], got[32];
                    type_desc(seen, want, sizeof want);
                    type_desc(at, got, sizeof got);
                    ERR_SET(g, "line %zu: argument %d of '%s' is a %s, but the "
                               "parameter was already resolved as a %s — one "
                               "function name has one signature",
                            n->line, i + 1, cs->name, got, want);
                    return changed;
                }
                /* Only ever fill a slot that is still empty: writing back an
                 * equal-but-different Type here would hand the emitter the
                 * argument's anon-struct type in place of the parameter's
                 * declared one, and every later `a.x` would look at a struct
                 * with no name and report itself as a non-struct value. */
                if (seen) continue;
                cs->params[i] = at;
                changed++;
            }
        }
        changed += walk_sigs(g, c, t, s);
        for (int i = 0; i < argc; i++) changed += walk_sigs(g, n->as.call.args[i], t, s);
        return changed;
    }
    case N_RETURN:
        /* A bare `return;` emits `ret void`, which would not match a return
         * type the pass already worked out from another `return`. */
        if (s && !n->as.ret.expr && s->ret) {
            ERR_SET(g, "line %zu: 'return' with no value in '%s', which returns "
                       "a value — every 'return' must agree", n->line, s->name);
            return changed;
        }
        if (s && n->as.ret.expr && !s->ret) {
            Type *rt = infer_node_type(g, n->as.ret.expr);
            if (!rt) {
                /* ERR_SET rather than ERRX: this walk returns a count, and a
                 * bare `return;` from the macro would be a mismatch. */
                ERR_SET(g, "line %zu: cannot infer the return type of '%s' "
                           "(annotate it or return a value of a known type)",
                        n->line, s->name);
                return changed;
            }
            s->ret = rt;
            changed++;
        }
        return changed + walk_sigs(g, n->as.ret.expr, t, s);
    case N_ASSIGN:
        /* A closure variable keeps the first lambda it was bound to as its
         * type's back pointer; every call through the variable lowers
         * against THAT signature while the runtime box holds whatever was
         * assigned last. Reassigning with a lambda of the SAME shape is
         * safe — both spell the same signature, so the lowering cannot go
         * wrong — and is now supported. A shape change is still refused:
         * the call would lower against the old signature while the box
         * holds the new lambda, silently wrong. (The interpreter, being
         * dynamically typed, accepts both.)
         *
         * The second lambda has no call site of its own (every call through
         * the variable pins the FIRST lambda), so its parameter types are
         * unknown until this branch propagates the old signature into it —
         * which is exactly what makes its body inferable and the two
         * signatures comparable. A parameter that is still NULL on either
         * side skips the comparison (undecided) and the fixed point walks
         * this statement again. */
        {
            Type *vt = lt_find(t, n->as.assign.name);
            if (vt && vt->kind == TY_FUNC && n->as.assign.value &&
                n->as.assign.value->type == N_FUNC_LIT) {
                Node *old = vt->func;
                Node *new_ = n->as.assign.value;
                if (old && old->type == N_FUNC_LIT &&
                    old->as.funclit.arity == new_->as.funclit.arity) {
                    /* Propagate the old lambda's parameter types into the new
                     * one (funclit slots AND its Sig) so its body can infer a
                     * return type and the comparison below is decidable. */
                    Sig *ns = sig_find(&g->sigs, new_->as.funclit.cname);
                    for (int k = 0; k < old->as.funclit.arity; k++) {
                        if (!new_->as.funclit.param_types[k] &&
                            old->as.funclit.param_types[k]) {
                            new_->as.funclit.param_types[k] =
                                old->as.funclit.param_types[k];
                            if (ns) ns->params[k] =
                                old->as.funclit.param_types[k];
                            changed++;
                        }
                    }
                }
                int same = 1, undecided = 0;
                if (!old || old->type != N_FUNC_LIT ||
                    old->as.funclit.arity != new_->as.funclit.arity) {
                    same = 0;       /* shape differs, or first binding unknown */
                } else {
                    for (int k = 0; k < old->as.funclit.arity && same; k++) {
                        Type *a = old->as.funclit.param_types[k];
                        Type *b = new_->as.funclit.param_types[k];
                        if (!a || !b) { undecided = 1; same = 0; break; }
                        if (!types_eq(a, b)) same = 0;
                    }
                    if (same) {
                        if (!old->as.funclit.ret || !new_->as.funclit.ret) {
                            undecided = 1;
                            same = 0;
                        } else if (!types_eq(old->as.funclit.ret,
                                             new_->as.funclit.ret)) {
                            same = 0;
                        }
                    }
                }
                if (!undecided && !same) {
                    ERR_SET(g, "line %zu: reassigning a closure variable is not "
                               "supported on the native backends when the new "
                               "lambda has a different signature — the variable "
                               "keeps the first lambda's signature", n->line);
                    return changed;
                }
            }
        }
        changed += walk_sigs(g, n->as.assign.value, t, s);
        return changed;
    case N_ASSIGN_MEMBER:
        changed += walk_sigs(g, n->as.assign_mem.obj, t, s);
        return changed + walk_sigs(g, n->as.assign_mem.value, t, s);
    case N_EXPR_STMT:   return walk_sigs(g, n->as.expr_stmt.expr, t, s);
    case N_UNARY:       return walk_sigs(g, n->as.unary.operand, t, s);
    case N_BINARY:
        changed += walk_sigs(g, n->as.binary.left, t, s);
        return changed + walk_sigs(g, n->as.binary.right, t, s);
    case N_TERNARY:
        changed += walk_sigs(g, n->as.ternary.cond, t, s);
        changed += walk_sigs(g, n->as.ternary.then, t, s);
        return changed + walk_sigs(g, n->as.ternary.els, t, s);
    case N_MATCH: {
        changed += walk_sigs(g, n->as.matchn.scrutinee, t, s);
        for (int i = 0; i < n->as.matchn.armc; i++)
            changed += walk_sigs(g, n->as.matchn.arms[i].body, t, s);
        return changed;
    }
    case N_CTOR:
        for (int i = 0; i < n->as.ctor.argc; i++)
            changed += walk_sigs(g, n->as.ctor.args[i], t, s);
        return changed;
    case N_MEMBER:      return walk_sigs(g, n->as.member.obj, t, s);
    case N_LET: {
        /* `let f: fn(int) => int = (x) => x * 2;` (SPEC §4.8): the fn
         * annotation spells the lambda's parameter types. Pin them into the
         * lambda's signature so an unannotated `(x)` parameter is typed even
         * before any call site shapes it. */
        if (n->as.let.annot && n->as.let.annot->kind == TY_FUNC &&
            n->as.let.init && n->as.let.init->type == N_FUNC_LIT) {
            Node *lit = n->as.let.init;
            ensure_lambda_registered(g, lit);
            Sig *ls = sig_find(&g->sigs, lit->as.funclit.cname);
            Type *ft = n->as.let.annot;
            if (ls) {
                for (int i = 0; i < ls->arity && i < ft->count; i++)
                    if (!ls->params[i] && ft->types[i]) {
                        ls->params[i] = ft->types[i];
                        changed++;
                    }
            }
        }
        return changed + walk_sigs(g, n->as.let.init, t, s);
    }
    case N_LET_PAT:     return walk_sigs(g, n->as.letpat.init, t, s);
    case N_LIST_LIT:
        for (int i = 0; i < n->as.list.count; i++)
            changed += walk_sigs(g, n->as.list.items[i], t, s);
        return changed;
    case N_MAP_LIT:
        for (int i = 0; i < n->as.map.count; i++)
            changed += walk_sigs(g, n->as.map.vals[i], t, s);
        return changed;
    case N_IF:
        changed += walk_sigs(g, n->as.ifs.cond, t, s);
        changed += walk_sigs(g, n->as.ifs.then, t, s);
        return changed + walk_sigs(g, n->as.ifs.els, t, s);
    case N_WHILE:
        changed += walk_sigs(g, n->as.whiles.cond, t, s);
        return changed + walk_sigs(g, n->as.whiles.body, t, s);
    case N_FOR:
        changed += walk_sigs(g, n->as.fors.init, t, s);
        changed += walk_sigs(g, n->as.fors.cond, t, s);
        changed += walk_sigs(g, n->as.fors.incr, t, s);
        changed += walk_sigs(g, n->as.fors.iterable, t, s);
        return changed + walk_sigs(g, n->as.fors.body, t, s);
    case N_FUNC_LIT: {
        /* Walk into a nested lambda so its own HOF calls (`map((xs) =>
         * reduce(...), ...)`) get their parameter hints and its body's
         * returns fill ITS signature, not the enclosing function's. The
         * scope is the lambda's: params, phase-2 captures, then body locals. */
        if (!n->as.funclit.cname) return 0;
        Sig *ls = sig_find(&g->sigs, n->as.funclit.cname);
        /* Only walk the body once its parameters are pinned: a body whose
         * return depends on a not-yet-hinted parameter would set the error
         * slot in the N_RETURN walk — ERR_SET is one-shot, the fixed point
         * never retries it. Named functions dodge this because their bodies
         * are only walked by infer_one_sig, after the top-level pass has
         * already hinted the parameters. A lambda written inline as a HOF
         * argument is hinted by its enclosing N_CALL branch before this
         * recursion, so it is ready here. */
        if (ls) {
            int ready = 1;
            for (int i = 0; i < ls->arity; i++)
                if (!ls->params[i]) { ready = 0; break; }
            if (!ready) return 0;
        }
        LTys lt2 = {0};
        lt_copy(&lt2, t);
        /* Parameter types come from the lambda's Sig first: the AST's own
         * param_types are only written back at the end of the fixed point,
         * so mid-walk a just-hinted parameter is still NULL on the node. */
        for (int i = 0; i < n->as.funclit.arity; i++) {
            Type *pt = (ls && ls->params[i]) ? ls->params[i]
                : (i < n->as.funclit.arity ? n->as.funclit.param_types[i] : NULL);
            if (n->as.funclit.names[i]) {
                lt_remove(&lt2, n->as.funclit.names[i]);
                if (pt) lt_push(&lt2, n->as.funclit.names[i], pt);
            }
        }
        for (int i = 0; i < n->as.funclit.cap_count; i++)
            if (n->as.funclit.cap_names[i] && n->as.funclit.cap_types[i])
                lt_push(&lt2, n->as.funclit.cap_names[i],
                        n->as.funclit.cap_types[i]);
        collect_known(g, n->as.funclit.body, &lt2);
        g->scope = lt2;
        int ch = walk_sigs(g, n->as.funclit.body, &lt2, ls ? ls : s);
        g->scope = *t;          /* restore the enclosing scope, not empty */
        lt_free(&lt2);
        return ch;
    }
    case N_BLOCK: case N_PROGRAM:
        for (int i = 0; i < n->as.block.count; i++)
            changed += walk_sigs(g, n->as.block.stmts[i], t, s);
        return changed;
    default: return 0;
    }
}

/* Fill one signature in place. ERRX when a slot could not be filled at all. */

static void infer_one_sig(CG *g, Sig *s)
{
    if (!s->body) return;
    LTys t = {0};

    /* Top-level bindings come first: a lambda body calling another lambda
     * resolves the callee through them (the callee is not a local of the
     * body being walked). Parameters are pushed after and replace any
     * same-named global, so the innermost binding wins, and body locals are
     * collected last for the same reason. */
    lt_copy(&t, &g->globals);

    for (int i = 0; i < s->arity; i++) {
        if (s->pnames && s->pnames[i]) {
            /* A parameter shadows a same-named global even before its type is
             * pinned: leaving the global's type visible would let the body
             * inherit it and freeze the wrong return type (see lt_remove). A
             * NULL type still occupies the name so the body never falls back
             * to the global. */
            lt_remove(&t, s->pnames[i]);
            lt_push(&t, s->pnames[i], s->params[i]);
        }
    }
    /* Phase-2 captures: a lambda body may read outer locals, typed by the
     * capture pass (which reruns each fixed-point round). Push them before
     * the body's own locals, which shadow on a same-name collision. */
    if (s->is_lambda && s->node) {
        Node *fn = s->node;
        for (int i = 0; i < fn->as.funclit.cap_count; i++)
            if (fn->as.funclit.cap_names[i] && fn->as.funclit.cap_types[i])
                lt_push(&t, fn->as.funclit.cap_names[i],
                        fn->as.funclit.cap_types[i]);
    }
    collect_known(g, s->body, &t);
    g->scope = t;                 /* readable by infer_node_type */
    walk_sigs(g, s->body, &t, s);
    g->scope = (LTys){0};
    lt_free(&t);

    /* The "parameter has no known type" check is deliberately NOT here: it
     * would fire on the first fixed-point round, before a call site in another
     * function body has had a chance to pin the parameter. It is deferred to
     * a single post-loop validation in infer_sigs() instead. */
}

/* Unannotated parameters and return types, filled from the call sites and the
 * `return` statements. Iterated to a fixed point because a call can shape the
 * caller: `g(f(n))` gives g's return type a dependency on f's parameter, and f's
 * parameter may only be knowable from an argument g is handed. */

static void infer_sigs(CG *g)
{
    for (int round = 0; round <= g->sigs.n + 1; round++) {
        int changed = 0;
        /* Phase-2 captures refresh every round: a capture type depends on the
         * enclosing function's locals, which this same fixed point is still
         * resolving, so the lambda's captured view must track it. */
        capture_pass(g);

        /* Top-level statements are walked too, every round: an entry-point
         * call like `half(5)` sits outside every function body, but it is
         * exactly the shape that pins a parameter's type. Re-walking each
         * round also lets a top-level closure reassignment propagate the old
         * lambda's signature into the new one the moment the old one is
         * final (the N_ASSIGN branch), after which the new lambda's body
         * infers a return type on a later round — the same fixed-point game
         * a function body plays. */
        if (g->prog && g->prog->type == N_PROGRAM) {
            LTys t = {0};
            /* Seed the top-level scope with the types of top-level `let`s — a
             * closure bound at top level (`let f = (x) => ...; f(3)`) has to be
             * visible so its call site can find the lambda's TY_FUNC type. */
            collect_known(g, g->prog, &t);
            for (int i = 0; i < g->prog->as.program.count; i++) {
                Node *st = g->prog->as.program.stmts[i];
                if (st && st->type == N_EXPR_STMT) st = st->as.expr_stmt.expr;
                g->scope = t;
                changed += walk_sigs(g, st, &t, NULL);
            }
            g->scope = (LTys){0};
            lt_free(&t);
        }

        for (int i = 0; i < g->sigs.n; i++) {
            int before = 0;
            Sig *s = &g->sigs.v[i];
            for (int k = 0; k < s->arity; k++) if (s->params[k]) before++;
            if (s->ret) before++;
            infer_one_sig(g, s);
            int after = 0;
            for (int k = 0; k < s->arity; k++) if (s->params[k]) after++;
            if (s->ret) after++;
            if (after > before) changed = 1;
        }
        if (!changed) break;
    }

    /* Hand the result back to the emitter. The AST is still the owner of
     * `params`/`ret`, so this only fills the slots the source left NULL — the
     * function emitter then sees the same types the callsite pass resolved,
     * and a `%p0`/`define` can be spelled for a parameter nobody annotated.
     *
     * This is written back rather than read from `sigs` in cg_function
     * because a few emitters (the struct-literal `expect` type, the call
     * result) look the type up through the node itself. */
    for (int i = 0; i < g->sigs.n; i++) {
        Sig *s = &g->sigs.v[i];
        if (!s->node) continue;
        if (s->is_lambda) {
            Node *fn = s->node;
            if (!fn->as.funclit.ret) fn->as.funclit.ret = s->ret;
            for (int k = 0; k < s->arity; k++)
                if (!fn->as.funclit.param_types[k])
                    fn->as.funclit.param_types[k] = s->params[k];
        } else {
            if (!s->node->as.func.ret) s->node->as.func.ret = s->ret;
            for (int k = 0; k < s->arity; k++)
                if (!s->node->as.func.param_types[k]) {
                    s->node->as.func.param_types[k] = s->params[k];
                }
        }
    }

    /* Done: anything still unresolved is a genuine error (a parameter no call
     * site typed), not a placeholder a later round would have filled. */
    for (int i = 0; i < g->sigs.n; i++) {
        Sig *s = &g->sigs.v[i];
        for (int k = 0; k < s->arity; k++)
            if (!s->params[k])
                ERRX(g, "line %zu: parameter '%s' of %s has no known type — "
                        "annotate it or call it with a value of a known type",
                     s->body ? s->body->line : 0,
                     s->pnames && s->pnames[k] ? s->pnames[k] : "?",
                     s->is_lambda ? "the lambda" : s->name);
    }
}

/* Run the signature pass over a whole program, without emitting anything.
 *
 * Exported so the *driver* can run it once before it dispatches: both native
 * backends (this one and the libLLVM walker in llvm_codegen.c) read the
 * function signatures straight off the AST, so guessing them separately in two
 * translation units is how the two would drift apart and quietly produce IR
 * that agrees with neither the source nor each other. The pass is idempotent —
 * everything it fills in is a slot the source left NULL, so a second run sees
 * nothing to do. */

int codegen_infer_signatures(Node *prog, char *err, size_t err_size)
{
    if (err && err_size) err[0] = '\0';
    if (!prog || prog->type != N_PROGRAM) return 0;

    /* Local functions first: it stamps cnames and rewrites call sites, and
     * the signature collection below must see those names. Idempotent. */
    if (!lume_resolve_localfns(prog, err, err_size)) return -1;

    CG g;
    memset(&g, 0, sizeof g);
    /* No IrBuf is attached: the pass only types nodes, it never emits. */
    g.prog = prog;

    /* Top-level and local functions alike: a local's return type is inferred
     * from its own returns exactly like a top-level one, under its cname. */
    int nfn = lume_collect_funcs(prog, NULL, 0);
    Node **fns = nfn > 0 ? xmalloc(sizeof(Node *) * (size_t)nfn) : NULL;
    if (nfn > 0) lume_collect_funcs(prog, fns, nfn);
    for (int i = 0; i < nfn; i++) {
        Node *n = fns[i];
        const char *fname = n->as.func.cname ? n->as.func.cname
                                             : n->as.func.name;
        sig_push(&g.sigs, fname, n->as.func.ret,
                 n->as.func.param_types, n->as.func.arity);
        Sig *s = &g.sigs.v[g.sigs.n - 1];
        s->body   = n->as.func.body;
        s->pnames = n->as.func.names;
        s->node   = n;
    }
    free(fns);
    Node **top  = prog->as.program.stmts;
    int    ntop = prog->as.program.count;
    /* Structs too: resolving `a.x` asks which field `x` is, and a field type
     * is only known once the declaration is in the table. */
    for (int i = 0; i < ntop; i++) {
        Node *n = top[i];
        if (n && n->type == N_TYPE_DECL)
            sdef_push(&g.structs, n->as.type_decl.name, n->as.type_decl.field_names,
                      n->as.type_decl.field_types, n->as.type_decl.count);
    }

    /* Every lambda gets its mangled name and its Sig before inference starts
     * (see preregister_lambdas), and the top-level `let` types are collected
     * once so every function body's scope can see them. Both are safe here:
     * nothing holds a `Sig *` or aliases `g.globals` while they grow. */
    preregister_lambdas(&g, prog, 0);
    collect_known(&g, prog, &g.globals);

    infer_sigs(&g);          /* fills g.sigs, writes through to the AST below */

    if (err && err_size && g.err[0])
        snprintf(err, err_size, "%s", g.err);

    for (int i = 0; i < g.sigs.n; i++) free(g.sigs.v[i].name);
    free(g.sigs.v);
    for (int i = 0; i < g.structs.n; i++) free(g.structs.v[i].name);
    free(g.structs.v);
    lt_free(&g.scope);
    lt_free(&g.globals);
    memset(&g.sigs, 0, sizeof g.sigs);
    memset(&g.structs, 0, sizeof g.structs);
    memset(&g.scope, 0, sizeof g.scope);
    memset(&g.globals, 0, sizeof g.globals);

    return g.err[0] ? -1 : 0;
}
