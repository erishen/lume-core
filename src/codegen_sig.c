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

/* Types already known for the names one function body can see. The pass below
 * has to type an argument like `f(n)` before the body is emitted, and `n` may
 * be one of that function's own parameters (whose type is the thing being
 * worked out) or a local declared with an annotated `let`. */

static void lt_push(LTys *t, const char *name, Type *ty)
{
    if (!name || !ty) return;
    for (int i = 0; i < t->n; i++)              /* a later binding wins */
        if (strcmp(t->v[i].name, name) == 0) { t->v[i].ty = ty; return; }
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->v = (LTyp *)xrealloc(t->v, (size_t)t->cap * sizeof *t->v);
    }
    t->v[t->n].name = xstrdup(name);
    t->v[t->n].ty   = ty;
    t->n++;
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
    lambda->as.funclit.cname = xstrdup(buf);

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

/* Types are allocated per use — there is no interning — so pointer equality
 * would call two `int`s different. Structural, and shallow enough that two
 * differing shapes can only disagree on the kind. */

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
    case TY_LIST:   { char e[32]; type_desc(t->elem, e, sizeof e);
                      snprintf(buf, n, "%s[]", e); break; }
    case TY_STRUCT: snprintf(buf, n, "%s", t->name ? t->name : "struct"); break;
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
                            if (init_t && !ls->params[0]) { ls->params[0] = init_t; changed++; }
                            if (list_t && list_t->kind == TY_LIST && list_t->elem &&
                                !ls->params[1]) { ls->params[1] = list_t->elem; changed++; }
                        } else {
                            Type *list_t = argc >= 2
                                ? infer_arg_type(g, n->as.call.args[1], t) : NULL;
                            if (list_t && list_t->kind == TY_LIST && list_t->elem &&
                                !ls->params[0]) { ls->params[0] = list_t->elem; changed++; }
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
         * assigned last. If the new lambda's shape differs, the call is
         * silently wrong, so reassignment is refused on the native paths
         * (the interpreter, which is dynamically typed, still accepts it). */
        {
            Type *vt = lt_find(t, n->as.assign.name);
            if (vt && vt->kind == TY_FUNC && n->as.assign.value &&
                n->as.assign.value->type == N_FUNC_LIT) {
                ERR_SET(g, "line %zu: reassigning a closure variable is not "
                           "supported on the native backends — the variable "
                           "keeps the first lambda's signature", n->line);
                return changed;
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
    case N_MEMBER:      return walk_sigs(g, n->as.member.obj, t, s);
    case N_LET:         return walk_sigs(g, n->as.let.init, t, s);
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

    for (int i = 0; i < s->arity; i++)
        if (s->params[i] && s->pnames && s->pnames[i])
            lt_push(&t, s->pnames[i], s->params[i]);
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
    /* Top-level statements are walked too: an entry-point call like
     * `half(5)` sits outside every function body, but it is exactly the shape
     * that pins a parameter's type. */
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
            walk_sigs(g, st, &t, NULL);
        }
        g->scope = (LTys){0};
        lt_free(&t);
    }

    for (int round = 0; round <= g->sigs.n + 1; round++) {
        int changed = 0;
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

    CG g;
    memset(&g, 0, sizeof g);
    /* No IrBuf is attached: the pass only types nodes, it never emits. */
    g.prog = prog;

    Node **top  = prog->as.program.stmts;
    int    ntop = prog->as.program.count;
    for (int i = 0; i < ntop; i++) {
        Node *n = top[i];
        if (!n || n->type != N_FUNC_DECL) continue;
        sig_push(&g.sigs, n->as.func.name, n->as.func.ret,
                 n->as.func.param_types, n->as.func.arity);
        Sig *s = &g.sigs.v[g.sigs.n - 1];
        s->body   = n->as.func.body;
        s->pnames = n->as.func.names;
        s->node   = n;
    }
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
