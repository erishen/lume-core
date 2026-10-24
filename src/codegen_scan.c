/* codegen_scan.c — what a name is, before anything is emitted.

 * Two jobs that have to happen before the IR text:
 *
 *  - infer_node_type(): the static type of an expression, asked for long
 *    after cg_expr() would have wanted it (a member access needs the field
 *    type of its object).
 *  - scan_block(): register the variables a block introduces, so the entry
 *    block can allocate all of them before any statement is emitted — the IR
 *    buffer is appended in source order, so an alloca cannot appear late.
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
static Type *for_in_elem_type(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static void scan_stmt(CG *g, Node *n);

/* Match-arm binds, for the window their arm body's type is inferred in.
 * Same shape as the signature pass's lt_push: a repeated name overwrites
 * (no new strdup), distinct names are strdup'd once and freed by
 * arm_scope_clear when the outermost match is done. */
static void arm_scope_push(CG *g, const char *name, Type *ty)
{
    if (!name || !ty) return;
    for (int i = 0; i < g->arm_scope.n; i++)
        if (strcmp(g->arm_scope.v[i].name, name) == 0) {
            g->arm_scope.v[i].ty = ty;
            return;
        }
    if (g->arm_scope.n == g->arm_scope.cap) {
        g->arm_scope.cap = g->arm_scope.cap ? g->arm_scope.cap * 2 : 8;
        g->arm_scope.v = (LTyp *)xrealloc(g->arm_scope.v,
                                          (size_t)g->arm_scope.cap * sizeof *g->arm_scope.v);
    }
    g->arm_scope.v[g->arm_scope.n].name = arena_strdup(name);
    g->arm_scope.v[g->arm_scope.n].ty   = ty;
    g->arm_scope.n++;
}

static void arm_scope_clear(CG *g)
{
    /* names are arena memory now (TODO.md P2 Stage 3): only the malloc'd
     * vector shell dies here. */
    free(g->arm_scope.v);
    g->arm_scope = (LTys){0};
}

/* Reset the arm-bind scope back to `n` entries. This used to free the
 * discarded entries' xstrdup'd names (the bare count reset leaked them;
 * LeakSanitizer caught it from arm_scope_push); the names are arena memory
 * now, so the reset is a bare count assignment and the arena reclaims
 * everything at end-of-emission. Same shape as the emitters' asg_truncate:
 * nested matches truncate to their own saved value, which stays correct when
 * nothing is freed. */
static void arm_scope_truncate(CG *g, int n)
{
    if (n < 0) n = 0;
    g->arm_scope.n = n;
}

/* The AST records an explicit `: T` annotation on `let` nodes; a type inferred
 * from the initializer is never stored on the node. Without this the entry
 * block would have nothing to `alloca` — and the alloca pass bails out, which
 * silently truncates the rest of the function body. */

/* Do two inferred types describe the same runtime shape? Used by the get()
 * branch: a variable key over a statically-shaped map can only carry one
 * return type when every value shares one shape. Named structs compare by
 * name; anonymous ones field-for-field (each is a fresh Type instance, so
 * pointer equality would never fire). */
static int value_shape_eq(Type *a, Type *b)
{
    if (a == b) return 1;
    if (!a || !b || a->kind != b->kind) return 0;
    if (a->kind == TY_STRUCT) {
        if (a->name && b->name) return strcmp(a->name, b->name) == 0;
        if (a->count != b->count) return 0;
        for (int i = 0; i < a->count; i++) {
            if (!a->names[i] || !b->names[i]) return 0;
            if (strcmp(a->names[i], b->names[i]) != 0) return 0;
        }
        return 1;
    }
    if (a->kind == TY_LIST) return value_shape_eq(a->elem, b->elem);
    return 1;                     /* every remaining kind is a primitive */
}

/* The one value shape every field of this struct type shares, or NULL when
 * they disagree (or it declares nothing at all — a str_match row, a
 * json-parsed row). Two callers need the same answer and must not compute
 * it twice: the get() case below promises `any` exactly when this is NULL,
 * and emit_get_call (codegen_call.c) reads the same promise to pick
 * between a typed map getter and the dynamic slot read. */
Type *struct_common_shape(Type *t)
{
    if (!t) return NULL;
    Type *first = NULL;
    for (int i = 0; i < t->count; i++) {
        if (!t->types[i] || t->types[i]->kind == TY_ANY) continue;
        if (!first) { first = t->types[i]; continue; }
        if (!value_shape_eq(first, t->types[i])) return NULL;
    }
    return first;
}

Type *infer_node_type(CG *g, Node *n)
{
    if (!n) return NULL;

    switch (n->type) {
    case N_LITERAL:
        switch (n->as.lit.kind) {
        case LIT_NUM:   return type_prim(n->as.lit.is_float ? TY_FLOAT : TY_INT);
        case LIT_STR:   return type_prim(TY_STRING);
        case LIT_TRUE:
        case LIT_FALSE: return type_prim(TY_BOOL);
        case LIT_NULL:  return type_prim(TY_NULL);
        default:        return NULL;
        }
    case N_LET_EXPR:
        /* `let x = e1 in e2` — the expression's type is the body's (the
         * annotation wins when spelled). Prefer the checker's stamp: a
         * scan-time descent into the body would resolve the binding's name
         * against the outer scope (the emit-time slot does not exist yet),
         * so an outer same-name binding of another type would poison the
         * inferred type. */
        if (n->as.letx.annot) return n->as.letx.annot;
        if (n->as.letx.ty) return n->as.letx.ty;
        return infer_node_type(g, n->as.letx.body);
    case N_VAR: {
        Asg *a = asg_find(&g->locals, n->as.var.name, n->line);
        if (a) return a->ty;
        /* A top-level binding lifts to a module global (SPEC 8.1 #10); its
         * type was fixed by the driver's pre-scan. */
        a = gvar_find(&g->gvars, n->as.var.name);
        if (a) return a->ty;
        /* The signature pass runs before any slot is allocated, so a name it
         * already typed is the only other source. */
        Type *st = lt_find(&g->scope, n->as.var.name);
        if (st) return st;
        /* A match arm whose body is being type-inferred: its binds. */
        return lt_find(&g->arm_scope, n->as.var.name);
    }
    case N_FUNC_LIT: {
        /* A function literal is a closure value. Thread the node through
         * `Type.func` so a later call through a variable bound to it can
         * recover the lambda's own parameter / return types. */
        Type *ft = type_func(n->as.funclit.arity, n->as.funclit.param_types,
                             n->as.funclit.ret);
        ft->func = n;
        return ft;
    }
    case N_CALL: {
        /* `let b = f(...)` takes the return type of f — struct returns included,
         * which is how `let b = scale(a, 5)` gets its %Rect slot. */
        Node *callee = n->as.call.callee;
        if (!callee) return NULL;
        if (callee->type == N_MEMBER && callee->as.member.obj &&
            callee->as.member.obj->type == N_VAR) {
            /* ns member call: policy.decide(...) — the module signature was
             * injected under "<ns>.<name>" by codegen_infer_signatures_multi. */
            char qname[512];
            snprintf(qname, sizeof qname, "%s.%s",
                     callee->as.member.obj->as.var.name, callee->as.member.name);
            Sig *cs = sig_find(&g->sigs, qname);
            if (cs && cs->ret) {
                for (int i = 0; i < n->as.call.argc; i++)
                    infer_node_type(g, n->as.call.args[i]);
                return cs->ret;
            }
            return NULL;
        }
        if (callee->type != N_VAR) return NULL;
        const char *name = callee->as.var.name;

        /* Closure-valued variable call: `f(3)` where f : func. The variable's
         * type carries a back-pointer to the lambda node. The Sig's return
         * type is read live: `funclit.ret` only receives the resolved value
         * after the inference fixed point exits, so a parameter typed from
         * this call (`twice(g(n))` where twice is a local function) would
         * never fill inside the loop if the AST slot were the source. The
         * member-call branch above already reads the Sig the same way. */
        {
            Type *ct = infer_node_type(g, n->as.call.callee);
            if (ct && ct->kind == TY_FUNC && ct->func &&
                ct->func->type == N_FUNC_LIT) {
                Sig *ls = ct->func->as.funclit.cname
                    ? sig_find(&g->sigs, ct->func->as.funclit.cname) : NULL;
                if (ls && ls->ret) return ls->ret;
                return ct->func->as.funclit.ret;
            }
        }

        /* Higher-order builtins that consume a closure: map/filter/reduce.
         * Their result type is shaped by the closure's own types. */
        if (strcmp(name, "map") == 0) {
            Node *clo = n->as.call.argc > 0 ? n->as.call.args[0] : NULL;
            Node *lambda = NULL;
            if (clo && clo->type == N_FUNC_LIT) lambda = clo;
            else if (clo && clo->type == N_VAR) {
                Type *ct = infer_node_type(g, clo);
                if (ct && ct->kind == TY_FUNC && ct->func &&
                    ct->func->type == N_FUNC_LIT)
                    lambda = ct->func;
            }
            if (lambda && lambda->as.funclit.ret)
                return type_list(lambda->as.funclit.ret);
            /* The signature pass pins a lambda's return in its Sig long
             * before the AST write-back at the end of the fixed point, so a
             * top-level `let d = map(...)` typed mid-loop would otherwise be
             * list(any) and every later reduce/map element read would degrade
             * to int — reading a double list by its i64 slots. Read the Sig
             * here as the live source. */
            if (lambda && lambda->as.funclit.cname) {
                for (int i = 0; i < g->sigs.n; i++)
                    if (g->sigs.v[i].node == lambda && g->sigs.v[i].ret)
                        return type_list(g->sigs.v[i].ret);
            }
            return type_list(any_type());
        }
        if (strcmp(name, "filter") == 0) {
            Type *lt = infer_node_type(g, n->as.call.args[1]);
            if (lt && lt->kind == TY_LIST) return lt;
            return type_list(any_type());
        }
        if (strcmp(name, "reduce") == 0)
            return infer_node_type(g, n->as.call.args[2]);

        /* len() is always an int, whoever it measures. */
        if (strcmp(name, "len") == 0 && n->as.call.argc == 1)
            return type_prim(TY_INT);

        /* env() reads a host environment variable: a string, or null when
         * unset / masked (SPEC §6.2). The native backends need this static
         * type before they can emit the getenv call. */
        if (strcmp(name, "env") == 0 && n->as.call.argc == 1)
            return type_prim(TY_STRING);

        /* argv() is the script's trailing CLI arguments as a string list.
         * The native backends need this static type before they can emit the
         * lume_bi_argv call (see codegen_call.c). */
        if (strcmp(name, "argv") == 0 && n->as.call.argc == 0)
            return type_list(type_prim(TY_STRING));

        /* now() is epoch seconds. int()/str()/float()/bool() and the
         * string tools answer their own kind whatever the argument's
         * static type — and the argument itself may not be inferable
         * (int((now() + 28800) % 86400 / 3600) in abac's policy: the
         * arithmetic sits on a builtin call). read_file() yields a
         * string, or null when the file is absent. The cast/string-tool
         * cases live with sort() below. */
        if (strcmp(name, "now") == 0 && n->as.call.argc == 0)
            return type_prim(TY_INT);
        if (strcmp(name, "read_file") == 0 && n->as.call.argc == 1)
            return type_prim(TY_STRING);
        if (strcmp(name, "write_file") == 0 && n->as.call.argc == 2)
            return type_prim(TY_INT);
        if (strcmp(name, "lock_file") == 0 && n->as.call.argc == 2)
            return type_prim(TY_INT);
        if (strcmp(name, "unlock_file") == 0 && n->as.call.argc == 0)
            return type_prim(TY_INT);

        /* Native server handler: int() / get(req.query_params, ...) lower to
         * lume_srv_atoi / lume_srv_qp. These must precede the generic
         * get(m, "k") branch below, which would otherwise swallow
         * get(req.query_params, "k", "d") — its receiver is a string (the
         * req.query_params member type), not a struct, so it returns NULL. */
        if (g->in_handler && strcmp(name, "int") == 0)
            return type_prim(TY_INT);
        if (g->in_handler && strcmp(name, "get") == 0)
            return type_prim(TY_STRING);

        /* try(fn) — { ok, err } where ok is the fn's return type and err is
         * null. Carrying the members here is what lets `r.ok` / `r.err` pick
         * the right getter (member_field_type reads this shape), exactly like
         * a map literal does. */
        if (strcmp(name, "try") == 0 && n->as.call.argc == 1) {
            Node *fn = n->as.call.args[0];
            Type *ft = NULL;
            if (fn->type == N_FUNC_LIT)
                ft = fn->as.funclit.ret;
            else if (fn->type == N_VAR) {
                Type *ct = infer_node_type(g, fn);
                if (ct && ct->kind == TY_FUNC) ft = ct->ret;
            }
            Type *at = type_anon_struct();
            type_add_member(at, "ok", ft ? ft : any_type());
            type_add_member(at, "err", type_prim(TY_NULL));
            return at;
        }

        /* range(...) builds a list of numbers: all-int arguments iterate as
         * ints, one float argument makes the whole range float (the helper's
         * whole-value rule keeps every element readable either way). An arg
         * without a static numeric type leaves the call untyped — the usual
         * explicit-annotation diagnostic explains. */
        if (strcmp(name, "range") == 0 &&
            n->as.call.argc >= 1 && n->as.call.argc <= 3) {
            Type *et = type_prim(TY_INT);
            for (int i = 0; i < n->as.call.argc; i++) {
                Type *at = infer_node_type(g, n->as.call.args[i]);
                if (!at || (at->kind != TY_INT && at->kind != TY_FLOAT))
                    return NULL;
                if (at->kind == TY_FLOAT) et = type_prim(TY_FLOAT);
            }
            return type_list(et);
        }

        /* sort(l) returns the same list it sorted, so the result keeps the
         * argument's element type. A non-list argument falls through to
         * NULL — the explicit-annotation path then explains. Kept in step
         * with llvm_cg_scan.inc. */
        if (strcmp(name, "sort") == 0 && n->as.call.argc == 1) {
            Type *lt = infer_node_type(g, n->as.call.args[0]);
            if (lt && lt->kind == TY_LIST) return lt;
            return NULL;
        }

        /* String tools (builtins_str.c interpreter parity) and the scalar
         * casts: the answer kind does not depend on the argument, so
         * `let parts = split(s, ",")` infers without an annotation the
         * way the interpreter's dynamic value already does. The accessor
         * forms str(m,"k") / int(m,"k") / float(m,"k") / bool(m,"k")
         * answer their own kind too (the field is resolved first, then
         * converted — builtins.c value_from_map). Kept in step with
         * llvm_cg_scan.inc. */
        if (n->as.call.argc >= 1) {
            if ((strcmp(name, "upper") == 0 || strcmp(name, "lower") == 0 ||
                 strcmp(name, "capitalize") == 0 || strcmp(name, "trim") == 0 ||
                 strcmp(name, "str") == 0 || strcmp(name, "string") == 0))
                return type_prim(TY_STRING);
            if (strcmp(name, "int") == 0) return type_prim(TY_INT);
            if (strcmp(name, "float") == 0) return type_prim(TY_FLOAT);
            if (strcmp(name, "bool") == 0) return type_prim(TY_BOOL);
            if (strcmp(name, "join") == 0 && n->as.call.argc >= 2)
                return type_prim(TY_STRING);
            if (strcmp(name, "replace") == 0 && n->as.call.argc == 3)
                return type_prim(TY_STRING);
            if (strcmp(name, "substr") == 0 &&
                n->as.call.argc >= 2 && n->as.call.argc <= 3)
                return type_prim(TY_STRING);
            if (strcmp(name, "split") == 0 && n->as.call.argc == 2)
                return type_list(type_prim(TY_STRING));
            if ((strcmp(name, "str_match") == 0 || strcmp(name, "regex") == 0) &&
                n->as.call.argc == 2)
                /* rows are boxed OBJ slots whose shape only exists at
                 * runtime — type them list(any), the same spelling a json()
                 * array parses to, so get(len/get/slot reads) infer and
                 * emit through the any-slot path on the native backends. */
                return type_list(any_type());
            if (strcmp(name, "contains") == 0 && n->as.call.argc == 2)
                return type_prim(TY_BOOL);
            if ((strcmp(name, "exp") == 0 || strcmp(name, "log") == 0 ||
                 strcmp(name, "ln") == 0))
                return type_prim(TY_FLOAT);
            /* Statistical CDFs / special functions: the interpreter and both
             * native emitters already know them (typecheck arity table,
             * codegen_call.c Bi table, rt.c lume_bi_* helpers); this is the
             * inference half — without it `let p = tdist_cdf(...)` needs an
             * explicit `: float` annotation (the P4 counter-example). */
            if ((strcmp(name, "normcdf") == 0 || strcmp(name, "norminv") == 0 ||
                 strcmp(name, "erf") == 0 ||
                 strcmp(name, "gamma") == 0 || strcmp(name, "lgamma") == 0 ||
                 strcmp(name, "igamma") == 0 || strcmp(name, "ibeta") == 0 ||
                 strcmp(name, "tdist_cdf") == 0 || strcmp(name, "chisq_cdf") == 0 ||
                 strcmp(name, "rand") == 0 || strcmp(name, "rand_range") == 0 ||
                 strcmp(name, "rand_normal") == 0 || strcmp(name, "rand_exponential") == 0 ||
                 strcmp(name, "rand_cauchy") == 0 || strcmp(name, "rand_gamma") == 0 ||
                 strcmp(name, "rand_beta") == 0 || strcmp(name, "rand_chisq") == 0 ||
                 strcmp(name, "rand_student_t") == 0 ||
                 strcmp(name, "rand_weibull") == 0 || strcmp(name, "rand_logistic") == 0 ||
                 strcmp(name, "rand_pareto") == 0 || strcmp(name, "rand_laplace") == 0 ||
                 strcmp(name, "rand_lognormal") == 0 || strcmp(name, "rand_gumbel") == 0 ||
                 strcmp(name, "rand_rayleigh") == 0 || strcmp(name, "rand_triangular") == 0 ||
                 strcmp(name, "rand_erlang") == 0 || strcmp(name, "rand_von_mises") == 0 ||
                 strcmp(name, "bs_call") == 0 || strcmp(name, "bs_put") == 0 ||
                 strcmp(name, "bs_delta_call") == 0 || strcmp(name, "bs_delta_put") == 0 ||
                 strcmp(name, "bs_gamma") == 0 || strcmp(name, "bs_vega") == 0 ||
                 strcmp(name, "z_test") == 0 || strcmp(name, "z_pvalue") == 0 ||
                 strcmp(name, "tstat") == 0 || strcmp(name, "ttest_p") == 0 ||
                 strcmp(name, "chisq_stat") == 0 || strcmp(name, "chisq_p") == 0))
                return type_prim(TY_FLOAT);
            /* Monte Carlo RNG integer flavours (2026-10-24): seed() returns
             * the seed it was given, rand_int() a bounded draw; the
             * distribution family (2026-10-25): rand_bool() answers 0/1,
             * rand_poisson() a count; inverse-transform (2026-10-26):
             * rand_geometric() a failure count; mixed (2026-10-27):
             * rand_binomial() a success count; advanced (2026-10-28):
             * rand_zipf() a Zipf draw, rand_negative_binomial() a failure
             * count. */
            if (strcmp(name, "seed") == 0 || strcmp(name, "rand_int") == 0 ||
                strcmp(name, "rand_bool") == 0 || strcmp(name, "rand_poisson") == 0 ||
                strcmp(name, "rand_geometric") == 0 || strcmp(name, "rand_binomial") == 0 ||
                strcmp(name, "rand_zipf") == 0 || strcmp(name, "rand_negative_binomial") == 0)
                return type_prim(TY_INT);
            /* Quant builtins (2026-10-24): the scalar statistics answer a
             * float; the rolling helpers and ewma build fresh float lists;
             * linreg() a struct map. Kept in step with llvm_cg_scan.inc. */
            if ((strcmp(name, "mean") == 0 || strcmp(name, "variance") == 0 ||
                 strcmp(name, "stddev") == 0 || strcmp(name, "median") == 0 ||
                 strcmp(name, "percentile") == 0 || strcmp(name, "covariance") == 0 ||
                 strcmp(name, "correlation") == 0 || strcmp(name, "max_drawdown") == 0 ||
                 strcmp(name, "skew") == 0 || strcmp(name, "kurtosis") == 0))
                return type_prim(TY_FLOAT);
            if (strcmp(name, "pct_change") == 0 || strcmp(name, "rolling_mean") == 0 ||
                strcmp(name, "rolling_stddev") == 0 || strcmp(name, "ewma") == 0 ||
                strcmp(name, "shuffle") == 0 ||
                strcmp(name, "sim_gbm") == 0 || strcmp(name, "sim_bridge") == 0 ||
                strcmp(name, "sim_ou") == 0 || strcmp(name, "sim_merton") == 0 ||
                strcmp(name, "rand_dirichlet") == 0 || strcmp(name, "rand_binormal") == 0 ||
                strcmp(name, "rand_gauss_copula") == 0 || strcmp(name, "rand_t_copula") == 0)
                return type_list(type_prim(TY_FLOAT));
            /* rand_multinomial (2026-10-28) builds a fresh int list: the
             * success counts per category. */
            if (strcmp(name, "rand_multinomial") == 0)
                return type_list(type_prim(TY_INT));
            if (strcmp(name, "linreg") == 0)
                return type_anon_struct();
        }

        /* json()/stringify() serialize to a JSON string. json(str) means
         * parse: the result is a tagged-any tree (a boxed LumeSlot*, see
         * src/rt.c), readable through get/len/str/for-in. Kept in step
         * with llvm_cg_scan.inc's copy. */
        if ((strcmp(name, "stringify") == 0 || strcmp(name, "json") == 0) &&
            n->as.call.argc == 1) {
            Type *at = infer_node_type(g, n->as.call.args[0]);
            if (strcmp(name, "json") == 0 && at && at->kind == TY_STRING)
                return any_type();
            return type_prim(TY_STRING);
        }

        /* get(m, "k"): the value type the key's static shape declares, so
         * `let x = get(m, "n")` infers without an annotation the way the
         * interpreter's dynamic read already does. A key that is not a
         * literal, or that the shape does not declare, falls through to
         * NULL — the explicit-annotation path then applies. */
        if (strcmp(name, "get") == 0 && n->as.call.argc >= 2) {
            Type *mt = infer_node_type(g, n->as.call.args[0]);
            /* A tagged-any receiver (a json() parse) has no static shape:
             * the result follows the default, or is `any` without one.
             * Kept in step with llvm_cg_scan.inc. */
            if (mt && mt->kind == TY_ANY) {
                if (n->as.call.argc == 3)
                    return infer_node_type(g, n->as.call.args[2]);
                return any_type();
            }
            if (mt && mt->kind == TY_LIST) {
                /* get(list, idx): a list read yields its element type. An
                 * `any` element (a str_match / json row) stays any — the
                 * runtime read is a boxed slot. Kept in step with
                 * llvm_cg_scan.inc. */
                Type *et = mt->elem;
                if (et && et->kind != TY_ANY &&
                    et->kind != TY_STRUCT && et->kind != TY_LIST)
                    return et;
                return any_type();
            }
            Node *kn = n->as.call.args[1];
            if (kn->type == N_LITERAL && kn->as.lit.kind == LIT_STR) {
                if (mt && mt->kind == TY_STRUCT) {
                    /* A string literal's text keeps its quotes; keys do not. */
                    const char *kt = kn->as.lit.text;
                    size_t kl = (size_t)kn->as.lit.len;
                    if (kl >= 2 && kt[0] == '"' && kt[kl - 1] == '"') { kt++; kl -= 2; }
                    if (mt->name) {
                        char kb[128];
                        if (kl < sizeof kb) {
                            memcpy(kb, kt, kl);
                            kb[kl] = '\0';
                            int idx = struct_field_idx(g, mt->name, kb);
                            if (idx >= 0) {
                                Type *ft = struct_field_type(g, mt->name, idx);
                                if (ft && ft->kind != TY_ANY) return ft;
                            }
                        }
                    } else {
                        for (int i = 0; i < mt->count; i++)
                            if (strlen(mt->names[i]) == kl &&
                                strncmp(mt->names[i], kt, kl) == 0) {
                                Type *ft = mt->types[i];
                                if (ft->kind != TY_ANY) return ft;
                            }
                    }
                }
            }
            /* A variable key over a statically-shaped map: the read can hold
             * any value the map holds, so it types as the map's value type
             * when every value shares one shape (get(subjects, user, null)
             * in abac's policy), and as `any` when they disagree. */
            if (mt && mt->kind == TY_STRUCT) {
                Type *cs = struct_common_shape(mt);
                return cs ? cs : any_type();
            }
            return NULL;
        }

        Sig *s = sig_find(&g->sigs, name);
        if (!s) return NULL;
        /* `f()?` is not a Result -- it is the `ok` payload, so the slot has to
         * be typed from that. Reading the callee's declared return type instead
         * alloca'd an i8* for an int payload, and the store/load pair disagreed
         * on the type: `store i64 %c10, i64* %lv_v` into an `i8*` slot, which
         * clang assembles and the program then segfaults on. */
        if (n->as.call.propagate) return s->ret ? s->ret->elem : NULL;
        return s->ret;
    }
    case N_INDEX: {
        /* `m["k"]` / `l[0]`. A list yields its element type; a runtime map holds
         * whatever was put in, so the result is the same opaque type a map
         * literal produces -- there is nothing written down to be more precise
         * about. That is what makes a chained `m["p"][1]` an opaque i8*, which
         * cg_index() hands to the runtime to resolve. */
        Type *ot = infer_node_type(g, n->as.index.obj);
        if (ot && ot->kind == TY_LIST)
            return ot->elem && ot->elem->kind != TY_ANY ? ot->elem : any_type();
        return type_anon_struct();
    }
    case N_MAP_LIT: {
        /* Keep the literal's key -> value-type shape, mirroring the checker's
         * copy: get(m, "k") dispatches its runtime getter on the value type,
         * and a map literal is the only place that type is recoverable. A
         * value whose own type cannot be inferred lands as `any`. */
        Type *at = type_anon_struct();
        for (int i = 0; i < n->as.map.count; i++) {
            Type *vt = infer_node_type(g, n->as.map.vals[i]);
            type_add_member(at, n->as.map.keys[i], vt ? vt : any_type());
        }
        return at;
    }
    case N_STRUCT_UPDATE: {
        /* `{ base with k: v, ... }` — the result type mirrors the base's
         * shape for slot typing: a named struct keeps its declared schema, a
         * map becomes a fresh anonymous struct (like N_MAP_LIT). The typecheck
         * pass already validated the overrides, so only the slot type matters
         * here. */
        Type *bt = infer_node_type(g, n->as.supd.base);
        if (bt && bt->kind == TY_STRUCT && bt->name)
            return bt;
        return type_anon_struct();
    }
    case N_MEMBER:  return member_field_type(g, n);
    case N_LIST_LIT: {
        /* The element type of a list literal is read off the literal itself:
         * a value's own type is only ever spelled `i8*`, so without this a
         * `for (x in xs)` would see list<any> — and `any` has no spelling, so
         * the iteration variable would be rejected as untypable. */
        Type *et = infer_list_elem(g, n);
        return type_list(et ? et : any_type());
    }
    case N_BINARY: {
        /* `/` is float division even on two integers (the interpreter agrees),
         * so it widens both operands — the emitter does the same. Comparisons
         * and the two logical operators are the only bool producers here. */
        Op op = n->as.binary.op;
        switch (op) {
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE:
        case OP_GT: case OP_GE: case OP_AND: case OP_OR:
            return type_prim(TY_BOOL);
        /* `/` widens to float even on two integers — the emitter converts both
         * operands with sitofp and divides as double. */
        case OP_DIV: return type_prim(TY_FLOAT);
        case OP_MOD: {
            Type *l = infer_node_type(g, n->as.binary.left);
            return l ? l : infer_node_type(g, n->as.binary.right);
        }
        default: {
            Type *l = infer_node_type(g, n->as.binary.left);
            Type *r = infer_node_type(g, n->as.binary.right);
            if (l && r && (l->kind == TY_FLOAT || r->kind == TY_FLOAT))
                return type_prim(TY_FLOAT);
            if (l && r && (l->kind == TY_STRING || r->kind == TY_STRING))
                return type_prim(TY_STRING);
            return (l ? l : r);
        }
        }
    }
    case N_UNARY:
        /* `!x` is bool; `-x` keeps its operand's type. */
        return n->as.unary.op == OP_NOT ? type_prim(TY_BOOL)
                                        : infer_node_type(g, n->as.unary.operand);
    case N_TERNARY: {
        /* 与 typecheck_expr.c 同一套统一规则：同型→它，int/float 混合→
         * float，任一 any/未知→any。文本与 libLLVM 两后端都拿它给结果
         * alloca 定型，所以这里必须能答出来。 */
        Type *tt = infer_node_type(g, n->as.ternary.then);
        Type *et = infer_node_type(g, n->as.ternary.els);
        if (!tt || !et || tt->kind == TY_ANY || et->kind == TY_ANY)
            return any_type();
        if (tt->kind == et->kind)
            return tt;
        bool t_num = tt->kind == TY_INT || tt->kind == TY_FLOAT;
        bool e_num = et->kind == TY_INT || et->kind == TY_FLOAT;
        if (t_num && e_num)
            return type_prim(TY_FLOAT);
        return NULL;
    }
    case N_CTOR:
        /* 构造值的类型 = 它的 enum（resolve_ctors 盖了 ename）。 TY_ENUM
         * 的 IR 拼写是 i8*（值即标签 map），见 codegen_types.c。 */
        return n->as.ctor.ename ? type_enum(n->as.ctor.ename) : NULL;
    case N_MATCH: {
        /* 臂体按三元的统一规则收拢——scrutinee 的类型不影响结果槽。臂内
         * 绑定只在它自己的臂里可见：推断窗口内压进 arm_scope，臂与臂之间
         * 截断回 saved（嵌套 match 因此仍能看见外层臂的绑定），最外层
         * match 收尾时统一释放。 */
        int saved = g->arm_scope.n;
        Type *result = NULL;
        for (int i = 0; i < n->as.matchn.armc; i++) {
            MatchArm *arm = &n->as.matchn.arms[i];
            arm_scope_truncate(g, saved);   /* drop the previous arm's binds */
            /* the arm's single binding set: the checker guarantees every
             * alternative binds the same names with the same types, so the
             * first pattern's stamps describe them all */
            MatchPat *p0 = &arm->pats[0];
            for (int b = 0; b < p0->bindc; b++)
                arm_scope_push(g, p0->binds[b],
                               p0->vfield_types ? p0->vfield_types[b] : NULL);
            Type *bt = infer_node_type(g, arm->body);
            /* An arm whose type cannot be read statically (a builtin call
             * like str(n), say) is skipped, not fatal: the arm's real type
             * exists at emit time, and one concrete arm among them still
             * fixes the result slot. Only an explicit `any` arm (a value
             * that IS untyped) widens the whole match. */
            if (!bt) continue;
            if (bt->kind == TY_ANY) { result = any_type(); break; }
            if (!result) { result = bt; continue; }
            if (result->kind == bt->kind) continue;
            bool r_num = result->kind == TY_INT || result->kind == TY_FLOAT;
            bool b_num = bt->kind == TY_INT || bt->kind == TY_FLOAT;
            if (r_num && b_num) { result = type_prim(TY_FLOAT); continue; }
            result = NULL;
            break;
        }
        arm_scope_truncate(g, saved);
        if (saved == 0) arm_scope_clear(g);
        return result ? result : any_type();
    }
    case N_ASSIGN:
        /* An assignment expression yields the *target*, which is what `for
         * (i = 0; ...)` and `a = b` want to carry. */
        return infer_node_type(g, n->as.assign.value);
    case N_ASSIGN_MEMBER:
        return infer_node_type(g, n->as.assign_mem.value);
    default:
        return NULL;
    }
}

/* The type a `for (x in it)` variable binds to.
 *
 * The checker is satisfied with `any` here (it only checks that the body uses
 * x consistently), but the native backend has to spell a type for the entry
 * block's alloca, and `any` has none. So the iterable is read directly:
 * a list yields its element type, an anonymous struct literal is a map and the
 * variable is the string key. */

static Type *for_in_elem_type(CG *g, Node *n)
{
    Type *t = infer_node_type(g, n->as.fors.iterable);

    /* ERR, not ERRX: this one returns a Type * and ERRX's bare `return;` does
     * not compile there; ERR's `return NULL;` is the honest failure value. */
    if (t && t->kind == TY_LIST) {
        if (!t->elem)
            ERR(g, "line %zu: cannot infer what a list of this type holds to "
                   "iterate it (annotate the list)", n->line);
        return t->elem;
    }
    if (t && t->kind == TY_STRUCT && !t->name) return type_prim(TY_STRING);

    ERR(g, "line %zu: the native backend can only iterate a list or a map in "
            "a 'for ... in' loop", n->line);
    return NULL;
}

/* The @lv_ global slot a top-level binding is lifted into (SPEC 8.1 #10).
 * Numbered like asg_push's %lv_ slots so two same-name bindings (a for-in
 * variable rebound by a later `let`) never share storage. */

static void gvar_slot(char *buf, size_t cap, const char *name)
{
    static int s_uid = 0;
    snprintf(buf, cap, "@lv_%s_%d", name, s_uid++);
}

static void scan_stmt(CG *g, Node *n)
{
    if (!n) return;

    switch (n->type) {
    case N_LET: {
        Type *ty = n->as.let.annot ? n->as.let.annot : infer_node_type(g, n->as.let.init);
        if (!ty)
            ERRX(g, "line %zu: cannot infer a type for '%s' (add an explicit annotation)",
                 n->line, n->as.let.name);
        /* `let f: fn(int) => int = (x) => x * 2;` (SPEC §4.8): the annotated
         * TY_FUNC carries no back-pointer of its own, but `f(3)` recovers the
         * lambda through Type.func — thread the literal through so the call
         * emitter can pin the parameter types. Same wiring as
         * infer_node_type's N_FUNC_LIT case. */
        if (ty->kind == TY_FUNC && !ty->func && n->as.let.init &&
            n->as.let.init->type == N_FUNC_LIT)
            ty->func = n->as.let.init;
        if (g->scanning_top) {
            /* A top-level binding is module storage: the same @lv_ global is
             * registered as a gvar (what function bodies resolve through)
             * and as L_top's own local (what the top level's stores hit), so
             * a mutation the top level makes after a function is defined is
             * visible when that function runs — the interpreter's env
             * semantics, spelled as one shared slot. */
            char slot[128];
            gvar_slot(slot, sizeof slot, n->as.let.name);
            asg_push(&g->gvars, n->as.let.name, ty, slot, n->line);
            asg_push(&g->locals, n->as.let.name, ty, slot, n->line);
            break;
        }
        asg_push(&g->locals, n->as.let.name, ty, NULL, n->line);
        break;
    }

    case N_LET_PAT: {
        /* One binding per pattern name, registered exactly like a `let`:
         * a top-level name becomes module storage (the @lv_ global plus
         * L_top's own local), a function-local name a plain local. The
         * types were stamped by the checker; a NULL stamp means the map's
         * shape was unknown, which has no codegen type — the wall `m.x`
         * hits on a dynamic map, reported here because a binding needs a
         * slot. The init is an expression and needs no scanning of its
         * own (lambdas inside it are pre-registered elsewhere). */
        for (int i = 0; i < n->as.letpat.count; i++) {
            Type *ty = n->as.letpat.types[i];
            const char *nm = n->as.letpat.names[i];
            if (!ty)
                ERRX(g, "line %zu: cannot infer a type for '%s' (the "
                        "destructured map has no known shape)",
                     n->line, nm);
            if (g->scanning_top) {
                char slot[128];
                gvar_slot(slot, sizeof slot, nm);
                asg_push(&g->gvars, nm, ty, slot, n->line);
                asg_push(&g->locals, nm, ty, slot, n->line);
            } else {
                asg_push(&g->locals, nm, ty, NULL, n->line);
            }
        }
        break;
    }

    case N_FOR: {
        /* A for-in variable is a *binding*, not a `let`, so nothing registers
         * it and the entry-block alloca pass would silently skip it — every
         * use of the variable then dies as an unknown variable. It binds in
         * the enclosing scope (the interpreter's for-in assigns through to an
         * existing name and leaves it bound after the loop), so it takes the
         * for statement's own line and no block of its own. At the top level
         * the enclosing scope is the module: the variable is lifted exactly
         * like a `let`, because a function the top level calls after the loop
         * must see the rebound value. */        if (n->as.fors.is_in) {
            Type *et = for_in_elem_type(g, n);
            if (g->scanning_top) {
                char slot[128];
                gvar_slot(slot, sizeof slot, n->as.fors.var);
                asg_push(&g->gvars, n->as.fors.var, et, slot, n->line);
                asg_push(&g->locals, n->as.fors.var, et, slot, n->line);
            } else {
                asg_push(&g->locals, n->as.fors.var, et, NULL, n->line);
            }
            scan_stmt(g, n->as.fors.body);
            break;
        }
        Node *init = n->as.fors.init;
        if (init && init->type == N_LET) {
            Type *ty = init->as.let.annot ? init->as.let.annot
                                          : infer_node_type(g, init->as.let.init);
            if (!ty)
                ERRX(g, "line %zu: cannot infer a type for '%s' (add an explicit annotation)",
                     init->line, init->as.let.name);
            /* A C-style for's init `let` stays a plain local even at the top
             * level: the interpreter binds it for the loop, not for the
             * program, and no function can name it. */
            asg_push(&g->locals, init->as.let.name, ty, NULL, init->line);
        } else if (init) {
            /* `for (i = 0; ...)` assigns rather than declares — the target
             * needs its local registered all the same. */
            scan_stmt(g, init);
        }
        if (n->as.fors.incr &&
            (n->as.fors.incr->type == N_ASSIGN ||
             n->as.fors.incr->type == N_EXPR_STMT))
            scan_stmt(g, n->as.fors.incr);
        scan_stmt(g, n->as.fors.body);
        break;
    }

    /* NOTE: match arms deliberately do NOT register their payload binds here.
     * match is an *expression*, so it appears where scan_stmt never walks
     * (let inits, returns, call arguments); the emitter registers each arm's
     * binds at emit time and truncates g->locals afterwards, which also keeps
     * two arms that bind the same name from aliasing one slot. */

    /* An assignment to a name that is not a local *is* meaningful inside a
     * function: the interpreter's assignment does not reach through to a
     * top-level binding — it defines a fresh function-local cell (a `g = 2`
     * in a body leaves the top-level `g` untouched, and a read after the
     * assignment sees the new local). The fresh cell needs a slot like any
     * other local, registered at the assignment's own line so reads before
     * it still resolve to the top-level global and reads after it see the
     * local — the same position-aware rule every binding follows. The type
     * is the assigned value's; a second assignment to the same name can
     * register another entry of a different type, and lookup sorts it out. */
    case N_ASSIGN: {
        if (!g->scanning_top &&
            !asg_find(&g->locals, n->as.assign.name, n->line)) {
            Asg *gv = gvar_find(&g->gvars, n->as.assign.name);
            if (gv) {
                Type *ty = infer_node_type(g, n->as.assign.value);
                /* shadow-marked: the entry block seeds this local from the
                 * @lv_ global, matching the interpreter's call-time env
                 * lookup (first read sees the global's value, not stack
                 * garbage — §8.1 #10's write-side half). */
                asg_push_shadow(&g->locals, n->as.assign.name,
                                ty ? ty : gv->ty, NULL, n->line);
            }
        }
        break;
    }

    /* A bare `x = e;` statement wraps its assignment in an expr-stmt; the
     * registration above only fires if the walk actually reaches the
     * N_ASSIGN, so unwrap the common shapes here. (A C-style for's init and
     * increment are bare N_ASSIGNs handled by their own case.) */
    case N_EXPR_STMT: {
        Node *e = n->as.expr_stmt.expr;
        if (e && (e->type == N_ASSIGN || e->type == N_ASSIGN_MEMBER))
            scan_stmt(g, e);
        break;
    }

    /* A nested block is the body of if/while/for and any bare { } — its `let`s
     * have to be registered too, or every use of such a local dies in codegen
     * ("bad operands" on a read, "unknown variable" on an assignment).
     * N_BLOCK walks one level and scan_block() recurses into its statements. */
    case N_BLOCK:  scan_block(g, n); break;
    case N_IF:     scan_stmt(g, n->as.ifs.then); scan_stmt(g, n->as.ifs.els); break;
    /* if-let binds are NOT registered here (same rule as match arms): the
     * emitter registers them around the then-branch at emit time. The
     * branches still have to be walked — their lets/assignments need slots. */
    case N_IF_LET: scan_stmt(g, n->as.iflet.then); scan_stmt(g, n->as.iflet.els); break;
    /* The condition is scanned BEFORE the body: it sits in the enclosing
     * scope, so its variable reads must bind to the outer slot — the body's
     * same-named `let` (a shadow) registers later and would otherwise
     * capture the condition's read (a read of an uninitialized shadow slot
     * on the native backends, where interp read the outer value). Emit
     * also re-hides the body's binds while generating the condition
     * (cg_while / wh_saved). Kept in step with llvm_cg_scan.inc. */
    case N_WHILE:
        if (g->wh_idx == g->wh_cap) {
            g->wh_cap = g->wh_cap ? g->wh_cap * 2 : 16;
            g->wh_saved = (int *)xrealloc(g->wh_saved,
                                          (size_t)g->wh_cap * sizeof *g->wh_saved);
        }
        g->wh_saved[g->wh_idx++] = g->locals.n;   /* before the body's lets */
        scan_stmt(g, n->as.whiles.cond);
        scan_stmt(g, n->as.whiles.body); break;
    default: break;
    }
}

void scan_block(CG *g, Node *blk)
{
    if (!blk) return;
    for (int i = 0; i < blk->as.block.count; i++) scan_stmt(g, blk->as.block.stmts[i]);
}