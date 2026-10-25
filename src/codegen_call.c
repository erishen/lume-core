/* codegen_call.c - call, closure and builtin emitters.
 *
 * Cut out of codegen_expr.c: the Bi builtin table, the closure literal and
 * its invoke/call helpers, the builtin declarations, the builtin dispatch
 * (cg_builtin) and the general call emitter (cg_call, which also routes
 * print() onto cg_print in codegen_print.c).
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

typedef struct {
    const char *name;
    const char *fn_i, *pty_i, *rty_i;
    int np_i;                    /* arity of the int flavour */
    const char *fn_f, *pty_f, *rty_f;
    int np_f;                    /* arity of the float flavour */
} Bi;


/* pty_* is a *single* parameter type: every helper takes homogeneous
 * arguments, so the arity plus this one string is enough to spell the whole
 * signature, and the call site emits the type once and the values after. */
static const Bi BUILTINS[] = {
    { "abs",   "lume_bi_abs",  "i64",    "i64",    1,  NULL,           NULL,         NULL,     0 },
    { "min",   "lume_bi_min",  "i64",    "i64",    2,  "lume_bi_minf", "double",     "double", 2 },
    { "max",   "lume_bi_max",  "i64",    "i64",    2,  "lume_bi_maxf", "double",     "double", 2 },
    { "sqrt",  NULL,           NULL,     NULL,     0,  "lume_bi_sqrt", "double",     "double", 1 },
    { "pow",   NULL,           NULL,     NULL,     0,  "lume_bi_pow",  "double",     "double", 2 },
    { "floor", NULL,           NULL,     NULL,     0,  "lume_bi_floor","double",     "double", 1 },
    { "ceil",  NULL,           NULL,     NULL,     0,  "lume_bi_ceil", "double",     "double", 1 },
    { "round", NULL,           NULL,     NULL,     0,  "lume_bi_round","double",     "double", 1 },
    /* Statistical distribution builtins (P4, 2026-10-20): float-only, so the
     * fn_f column carries them; emit_builtin_declares spells the signature. */
    { "normcdf",   NULL, NULL, NULL, 0, "lume_bi_normcdf",   "double", "double", 1 },
    { "norminv",   NULL, NULL, NULL, 0, "lume_bi_norminv",   "double", "double", 1 },
    { "erf",       NULL, NULL, NULL, 0, "lume_bi_erf",       "double", "double", 1 },
    { "gamma",     NULL, NULL, NULL, 0, "lume_bi_gamma",     "double", "double", 1 },
    { "lgamma",    NULL, NULL, NULL, 0, "lume_bi_lgamma",    "double", "double", 1 },
    { "igamma",    NULL, NULL, NULL, 0, "lume_bi_igamma",    "double", "double", 2 },
    { "ibeta",     NULL, NULL, NULL, 0, "lume_bi_ibeta",     "double", "double", 3 },
    { "tdist_cdf", NULL, NULL, NULL, 0, "lume_bi_tdist_cdf", "double", "double", 2 },
    { "chisq_cdf", NULL, NULL, NULL, 0, "lume_bi_chisq_cdf", "double", "double", 2 },
    { "z_pvalue",  NULL, NULL, NULL, 0, "lume_bi_z_pvalue",  "double", "double", 1 },
    { "ttest_p",   NULL, NULL, NULL, 0, "lume_bi_ttest_p",   "double", "double", 2 },
    { "chisq_p",   NULL, NULL, NULL, 0, "lume_bi_chisq_p",   "double", "double", 2 },
    { "now",   "lume_bi_now",  NULL,     "i64",    0,  NULL,           NULL,         NULL,     0 },
    { "argv",  "lume_bi_argv", NULL,     "i8*",    0,  NULL,           NULL,         NULL,     0 },
};

static const Bi *builtin_find(const char *name);
static Val cg_builtin(CG *g, Node *n);
Val cg_call(CG *g, Node *n);

static void clo_fn_type(Node *lambda, char *buf, size_t n)
{
    const char *rty = lambda->as.funclit.ret
        ? llvm_type_of(lambda->as.funclit.ret) : "void";
    size_t off = (size_t)snprintf(buf, n, "%s (i8*", rty);
    for (int i = 0; lambda->as.funclit.param_types &&
                    i < lambda->as.funclit.arity && off < n; i++) {
        const char *pt = llvm_ptr_type_of(lambda->as.funclit.param_types[i]);
        off += (size_t)snprintf(buf + off, n - off, ", %s", pt ? pt : "i8*");
    }
    if (off < n) snprintf(buf + off, n - off, ")*");
}

Val cg_funclit(CG *g, Node *n)
{
    const char *cname = n->as.funclit.cname;
    if (!cname)
        ERRV(g, "line %zu: internal: lambda was never named by the signature "
                "pass", n->line);
    char ftype[256];
    clo_fn_type(n, ftype, sizeof ftype);

    /* The same TY_FUNC-with-back-pointer infer_node_type builds, so a call
     * through the boxed value (or through a variable bound to it) finds the
     * lambda and its pinned signature again. */
    Type *ft = type_func(n->as.funclit.arity, n->as.funclit.param_types,
                         n->as.funclit.ret);
    ft->func = n;
    /* Phase-2 free-variable capture: build the capture array — one i8* per
     * captured name, holding the address of the outer variable's slot — and
     * hand it to lume_closure_make as the record's second word. The closure's
     * own define reads those addresses back through its `%cap` parameter, so
     * a later write to the outer variable is visible inside (shared env,
     * SPEC §4.7). A no-capture closure keeps passing null. */
    if (n->as.funclit.cap_count > 0) {
        char arrsym[48];
        snprintf(arrsym, sizeof arrsym, "%%caparr%d", g->tid++);
        EMIT(g, "  %s = alloca [%d x i8*]\n", arrsym,
             n->as.funclit.cap_count);
        for (int i = 0; i < n->as.funclit.cap_count; i++) {
            const char *nm = n->as.funclit.cap_names[i];
            Asg *a = asg_find(&g->locals, nm, n->line);
            if (!a) ERRV(g, "line %zu: captured variable '%s' has no codegen "
                            "slot", n->line, nm);
            const char *lt = llvm_type_of(a->ty);
            if (!lt) ERRV(g, "line %zu: captured variable '%s' has no codegen "
                             "type", n->line, nm);
            if (strncmp(a->slot, "%cap", 4) == 0)
                ERRV(g, "line %zu: capturing a variable already captured by "
                        "the enclosing closure is not supported", n->line);
            char el[48];
            snprintf(el, sizeof el, "%%capel%d", g->tid++);
            EMIT(g, "  %s = getelementptr [%d x i8*], [%d x i8*]* %s, "
                    "i32 0, i32 %d\n",
                 el, n->as.funclit.cap_count, n->as.funclit.cap_count,
                 arrsym, i);
            /* The slot is the alloca itself, so its address is the slot; a
             * captured string/container slot is a pointer slot, and storing
             * THAT address shares the outer cell too. The bitcast is a real
             * instruction, not a constant expression — %lv_* is an SSA value,
             * which LLVM rejects inside a constant. */
            char *bc = emit_instrf(g, type_prim(TY_NULL),
                                   "bitcast %s* %s to i8*", lt, a->slot);
            if (!bc) ERRV(g, "line %zu: cannot capture '%s'", n->line, nm);
            EMIT(g, "  store i8* %s, i8** %s\n", bc, el);
            free(bc);
        }
        char *arr = emit_instrf(g, type_prim(TY_NULL),
            "bitcast [%d x i8*]* %s to i8*", n->as.funclit.cap_count, arrsym);
        char *r = emit_instrf(g, ft,
            "call i8* @lume_closure_make(i8* bitcast (%s @%s to i8*), i8* %s)",
            ftype, cname, arr);
        free(arr);
        if (!r) ERRV(g, "line %zu: cannot build this closure", n->line);
        return val_take(ft, r);
    }
    char *r = emit_instrf(g, ft,
        "call i8* @lume_closure_make(i8* bitcast (%s @%s to i8*), i8* null)",
        ftype, cname);
    if (!r) ERRV(g, "line %zu: cannot build this closure", n->line);
    return val_take(ft, r);
}

static char *cg_clo_invoke(CG *g, Node *lambda, const char *clo_v,
                           Val *args, int argc)
{
    char ftype[256];
    clo_fn_type(lambda, ftype, sizeof ftype);
    const char *rty = lambda->as.funclit.ret
        ? llvm_type_of(lambda->as.funclit.ret) : NULL;

    /* Split the { i8* fn, i8* cap } record. The struct type is spelled
     * inline: it is a private ABI between the boxing call, this load pair,
     * and LumeClosure in src/rt.c. */
    char *rec = emit_instrf(g, type_prim(TY_NULL),
                            "bitcast i8* %s to { i8*, i8* }*", clo_v);
    if (!rec) return NULL;
    char *fnp = emit_instrf(g, type_prim(TY_NULL),
        "getelementptr inbounds { i8*, i8* }, { i8*, i8* }* %s, i32 0, i32 0",
        rec);
    char *capp = emit_instrf(g, type_prim(TY_NULL),
        "getelementptr inbounds { i8*, i8* }, { i8*, i8* }* %s, i32 0, i32 1",
        rec);
    free(rec);
    if (!fnp || !capp) { free(fnp); free(capp); return NULL; }
    char *fnv = emit_instrf(g, type_prim(TY_NULL), "load i8*, i8** %s", fnp);
    char *capv = emit_instrf(g, type_prim(TY_NULL), "load i8*, i8** %s", capp);
    free(fnp); free(capp);
    if (!fnv || !capv) { free(fnv); free(capv); return NULL; }
    char *fnbc = emit_instrf(g, type_prim(TY_NULL),
                             "bitcast i8* %s to %s", fnv, ftype);
    free(fnv);
    if (!fnbc) { free(capv); return NULL; }

    char res[48];
    snprintf(res, sizeof res, "%%t%d", g->tid++);
    if (rty) EMIT(g, "  %s = call %s %s(i8* %s", res, rty, fnbc, capv);
    else     EMIT(g, "  call void %s(i8* %s", fnbc, capv);
    free(fnbc);
    for (int i = 0; i < argc; i++) {
        const char *pt = args[i].ty ? llvm_ptr_type_of(args[i].ty) : "i8*";
        EMIT(g, ", %s %s", pt ? pt : "i8*", args[i].v);
    }
    EMIT(g, ")\n");
    free(capv);
    return xstrdup(res);
}

static Val cg_clo_call(CG *g, Node *n, Node *lambda)
{
    if (n->as.call.argc != lambda->as.funclit.arity)
        ERRV(g, "line %zu: this closure expects %d argument(s), got %d",
             n->line, lambda->as.funclit.arity, n->as.call.argc);

    Val clo = cg_expr(g, n->as.call.callee);
    if (!clo.v) ERRV(g, "line %zu: cannot evaluate the closure", n->line);

    Val *args = NULL;
    if (n->as.call.argc > 0) {
        args = (Val *)xmalloc((size_t)n->as.call.argc * sizeof *args);
        for (int i = 0; i < n->as.call.argc; i++) {
            /* Same expect pinning as a named call: the checker threads the
             * parameter type into the argument (ck_expr(arg, param)), so a
             * struct/tuple literal argument resolves to the parameter's
             * declared type here too. */
            Type *prev = g->expect;
            g->expect = lambda->as.funclit.param_types
                ? lambda->as.funclit.param_types[i] : NULL;
            args[i] = cg_expr(g, n->as.call.args[i]);
            g->expect = prev;
            if (args[i].ty && args[i].ty->kind == TY_STRUCT) {
                /* Same representation guard as the named-call path: an
                 * untyped map value must not be passed to a named struct
                 * parameter of a closure either. */
                Type *pt0 = lambda->as.funclit.param_types
                    ? lambda->as.funclit.param_types[i] : NULL;
                if (!args[i].ty->name && pt0 && pt0->kind == TY_STRUCT && pt0->name)
                    ERRV(g, "line %zu: this closure expects a '%s' struct, but "
                            "argument %d is an untyped map value — bind it to "
                            "the struct type first (`let v: %s = ...`)",
                         n->line, pt0->name, i + 1, pt0->name);
                struct_addr(g, &args[i]);
            }
            /* Pin the value to the parameter's spelling, the way a named
             * call's coerce does: int/float/bool conversions, and the no-op
             * everything else is. */
            Type *pt = lambda->as.funclit.param_types
                ? lambda->as.funclit.param_types[i] : NULL;
            if (pt && args[i].v) args[i] = coerce(g, pt, args[i], n->line);
            if (!args[i].v) {
                for (int k = 0; k < i; k++) free(args[k].v);
                free(args); free(clo.v);
                ERRV(g, "line %zu: bad argument %d for the closure",
                     n->line, i + 1);
            }
        }
    }

    char *res = cg_clo_invoke(g, lambda, clo.v, args, n->as.call.argc);
    free(clo.v);
    for (int i = 0; i < n->as.call.argc; i++) free(args[i].v);
    free(args);
    if (!res) ERRV(g, "line %zu: cannot call through this closure", n->line);

    Type *ret = lambda->as.funclit.ret;
    const char *rty = ret ? llvm_type_of(ret) : NULL;
    if (!rty) {
        /* A closure whose body yields no value is null when its result is
         * used as one — the interpreter pushes/prints null. Spelling the
         * type name as the value token ("void") emitted `i8* void`, which
         * is not IR; a null i8* is (TY_NULL lowers to i8*). */
        free(res);
        return val_make(type_prim(TY_NULL), "null");
    }
    Val v = val_make(ret, res);
    free(res);
    if (rty[0] == '%') v.agg = 1;
    return v;
}

static Node *hof_lambda(CG *g, Node *a)
{
    if (a && a->type == N_FUNC_LIT) return a;
    if (a && a->type == N_VAR) {
        Type *ct = infer_node_type(g, a);
        if (ct && ct->kind == TY_FUNC && ct->func &&
            ct->func->type == N_FUNC_LIT)
            return ct->func;
    }
    return NULL;
}

static const Bi *builtin_find(const char *name)
{
    for (size_t i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i++)
        if (strcmp(BUILTINS[i].name, name) == 0) return &BUILTINS[i];
    return NULL;
}

/* The value type a literal key maps to in m's static shape — the key set a
 * map literal carries, or a named struct's declared fields — or NULL when the
 * key is not a string literal or the shape does not mention it. get(m, "k")
 * dispatches its runtime getter on this: the value sitting at a key the shape
 * declares is always that type, whatever a default would have been. */
static Type *map_key_shape(CG *g, Val *m, Node *knode)
{
    if (!knode || knode->type != N_LITERAL ||
        knode->as.lit.kind != LIT_STR || !m->ty || m->ty->kind != TY_STRUCT)
        return NULL;
    /* A string literal's text keeps its quotes; map keys do not carry any. */
    const char *kt = knode->as.lit.text;
    size_t kl = (size_t)knode->as.lit.len;
    if (kl >= 2 && kt[0] == '"' && kt[kl - 1] == '"') { kt++; kl -= 2; }
    Type *mt = m->ty;
    if (mt->name) {
        /* A declared struct's shape lives in the SDef table, not on the Type. */
        char kb[128];
        if (kl >= sizeof kb) return NULL;
        memcpy(kb, kt, kl);
        kb[kl] = '\0';
        int idx = struct_field_idx(g, mt->name, kb);
        return idx >= 0 ? struct_field_type(g, mt->name, idx) : NULL;
    }
    for (int i = 0; i < mt->count; i++)
        if (strlen(mt->names[i]) == kl && strncmp(mt->names[i], kt, kl) == 0)
            return mt->types[i];
    return NULL;
}

/* Which lume_map_get_* flavour reads a value of type t, and the static type
 * the read result carries. Returns 0 for a type no getter covers. */
static int get_flavour(Type *t, const char **fn, Type **rty)
{
    if (!t) return 0;
    switch (t->kind) {
    case TY_STRING: *fn = "lume_map_get_s";   *rty = type_prim(TY_STRING);  return 1;
    case TY_FLOAT:  *fn = "lume_map_get_f";   *rty = type_prim(TY_FLOAT);   return 1;
    case TY_INT:
    case TY_BOOL:   *fn = "lume_map_get_i";   *rty = type_prim(TY_INT);     return 1;
    case TY_LIST:
        /* A list-typed default reads a real LumeList* back, so the answer
         * is indexable — not the anon struct used for maps. The element
         * type is unknowable at compile time (the map does not say), so it
         * reads as a list of any; indexing it goes through the boxed slot
         * path, and the interpreter's dynamic get() agrees. */
        *fn = "lume_map_get_obj";
        *rty = type_list(type_prim(TY_ANY));
        return 1;
    case TY_STRUCT:
    case TY_RESULT:
    case TY_NULL:   *fn = "lume_map_get_obj"; *rty = type_anon_struct();    return 1;
    default: return 0;
    }
}

void emit_builtin_declares(CG *g)
{
    EMIT(g, "; runtime helpers (src/rt.c)\n");
    for (size_t i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i++) {
        const Bi *b = &BUILTINS[i];
        if (b->fn_i) {
            EMIT(g, "declare %s @%s(", b->rty_i, b->fn_i);
            for (int k = 0; k < b->np_i; k++)
                EMIT(g, "%s%s", k ? ", " : "", b->pty_i);
            EMIT(g, ")\n");
        }
        if (b->fn_f) {
            EMIT(g, "declare %s @%s(", b->rty_f, b->fn_f);
            for (int k = 0; k < b->np_f; k++)
                EMIT(g, "%s%s", k ? ", " : "", b->pty_f);
            EMIT(g, ")\n");
        }
    }
    /* Conversion and concatenation: what they take depends on the operand kind,
     * so they cannot sit in the table above. */
    EMIT(g, "declare i8* @lume_bi_str_i64(i64)\n");
    EMIT(g, "declare i8* @lume_bi_str_double(double)\n");
    EMIT(g, "declare i8* @lume_bi_str_bool(i64)\n");
    EMIT(g, "declare i8* @lume_bi_cat(i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_rt_to_json_obj(i8*)\n");
    EMIT(g, "declare i8* @lume_rt_quote_str(i8*)\n");
    EMIT(g, "declare i64 @lume_bi_int(double)\n");
    EMIT(g, "declare double @lume_bi_float(i64)\n");
    EMIT(g, "declare i64 @lume_bi_len_s(i8*)\n");

    /* String tools (interpreter parity, builtins_str.c / builtins.c): string
     * parameters keep these out of the numeric table above, so they are
     * declared here and dispatched by name in cg_builtin. split/str_match
     * hand back a list (i8* to a LumeList), join/replace/substr a string. */
    EMIT(g, "declare i8* @lume_bi_upper(i8*)\n");
    EMIT(g, "declare i8* @lume_bi_lower(i8*)\n");
    EMIT(g, "declare i8* @lume_bi_capitalize(i8*)\n");
    EMIT(g, "declare i8* @lume_bi_trim(i8*)\n");
    EMIT(g, "declare i64 @lume_bi_contains(i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_bi_split(i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_bi_join(i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_bi_replace(i8*, i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_bi_substr(i8*, i64, i64)\n");
    EMIT(g, "declare i8* @lume_bi_str_match(i8*, i8*)\n");
    EMIT(g, "declare double @lume_bi_exp(double)\n");
    /* Monte Carlo RNG (2026-10-24, rt.c): deterministic PCG32 behind the
     * default seed; rand() takes no arguments, so it cannot ride the
     * numeric table path — declared here, dispatched in cg_builtin. */
    EMIT(g, "declare i64 @lume_bi_seed(i64)\n");
    EMIT(g, "declare double @lume_bi_rand()\n");
    EMIT(g, "declare double @lume_bi_rand_range(double, double)\n");
    EMIT(g, "declare i64 @lume_bi_rand_int(i64, i64)\n");
    EMIT(g, "declare double @lume_bi_rand_normal(double, double)\n");
    EMIT(g, "declare i64 @lume_bi_rand_bool(double)\n");
    EMIT(g, "declare double @lume_bi_rand_exponential(double)\n");
    EMIT(g, "declare i64 @lume_bi_rand_poisson(double)\n");
    EMIT(g, "declare i8* @lume_bi_shuffle(i8*)\n");
    EMIT(g, "declare double @lume_bi_rand_cauchy(double, double)\n");
    EMIT(g, "declare double @lume_bi_rand_gamma(double, double)\n");
    EMIT(g, "declare double @lume_bi_rand_beta(double, double)\n");
    EMIT(g, "declare double @lume_bi_rand_chisq(double)\n");
    EMIT(g, "declare double @lume_bi_rand_student_t(double)\n");
    EMIT(g, "declare i64 @lume_bi_rand_geometric(double)\n");
    EMIT(g, "declare double @lume_bi_rand_weibull(double, double)\n");
    EMIT(g, "declare double @lume_bi_rand_logistic(double, double)\n");
    EMIT(g, "declare double @lume_bi_rand_pareto(double, double)\n");
    EMIT(g, "declare double @lume_bi_rand_laplace(double, double)\n");
    EMIT(g, "declare i64 @lume_bi_rand_binomial(i64, double)\n");
    EMIT(g, "declare i64 @lume_bi_rand_zipf(i64, double)\n");
    EMIT(g, "declare i64 @lume_bi_rand_negative_binomial(i64, double)\n");
    EMIT(g, "declare double @lume_bi_rand_erlang(i64, double)\n");
    EMIT(g, "declare double @lume_bi_rand_von_mises(double, double)\n");
    EMIT(g, "declare i8* @lume_bi_rand_multinomial(i64, i8*)\n");
    EMIT(g, "declare i8* @lume_bi_sim_gbm(double, double, double, double, i64)\n");
    EMIT(g, "declare i8* @lume_bi_rand_dirichlet(i8*)\n");
    EMIT(g, "declare i8* @lume_bi_rand_binormal(double, double, double, double, double, i64)\n");
    EMIT(g, "declare i8* @lume_bi_rand_gauss_copula(double, i64)\n");
    EMIT(g, "declare i8* @lume_bi_rand_t_copula(double, double, i64)\n");
    EMIT(g, "declare i8* @lume_bi_bootstrap_mean_ci(i8*, double, double)\n");
    EMIT(g, "declare i8* @lume_bi_bootstrap_median_ci(i8*, double, double)\n");
    EMIT(g, "declare i8* @lume_bi_bootstrap_stddev_ci(i8*, double, double)\n");
    EMIT(g, "declare double @lume_bi_bootstrap_se_mean(i8*, double)\n");
    EMIT(g, "declare double @lume_bi_value_at_risk(i8*, double)\n");
    EMIT(g, "declare double @lume_bi_expected_shortfall(i8*, double)\n");
    EMIT(g, "declare double @lume_bi_sharpe_ratio(i8*, double)\n");
    EMIT(g, "declare double @lume_bi_sortino_ratio(i8*, double)\n");
    EMIT(g, "declare i8* @lume_bi_sim_bridge(double, double, double, double, i64)\n");
    EMIT(g, "declare i8* @lume_bi_sim_ou(double, double, double, double, double, i64)\n");
    EMIT(g, "declare i8* @lume_bi_sim_merton(double, double, double, double, double, double, double, i64)\n");
    EMIT(g, "declare i8* @lume_bi_sim_portfolio(i8*, i8*)\n");
    EMIT(g, "declare double @lume_bi_bs_call(double, double, double, double, double)\n");
    EMIT(g, "declare double @lume_bi_bs_put(double, double, double, double, double)\n");
    EMIT(g, "declare double @lume_bi_bs_delta_call(double, double, double, double, double)\n");
    EMIT(g, "declare double @lume_bi_bs_delta_put(double, double, double, double, double)\n");
    EMIT(g, "declare double @lume_bi_bs_gamma(double, double, double, double, double)\n");
    EMIT(g, "declare double @lume_bi_bs_vega(double, double, double, double, double)\n");
    EMIT(g, "declare double @lume_bi_z_test(i8*, double, double)\n");
    EMIT(g, "declare double @lume_bi_tstat(i8*, double)\n");
    EMIT(g, "declare double @lume_bi_chisq_stat(i8*, i8*)\n");
    EMIT(g, "declare double @lume_bi_rand_lognormal(double, double)\n");
    EMIT(g, "declare double @lume_bi_rand_gumbel(double, double)\n");
    EMIT(g, "declare double @lume_bi_rand_rayleigh(double)\n");
    EMIT(g, "declare double @lume_bi_rand_triangular(double, double)\n");
    EMIT(g, "declare double @lume_bi_log(double)\n");
    EMIT(g, "declare i64 @lume_bi_parse_i64(i8*)\n");
    EMIT(g, "declare double @lume_bi_parse_double(i8*)\n");
    EMIT(g, "declare i64 @lume_bi_bool_s(i8*)\n");
    EMIT(g, "declare i64 @lume_bi_bool_i(i64)\n");
    EMIT(g, "declare i64 @lume_bi_bool_f(double)\n");
    EMIT(g, "declare i64 @lume_bi_bool_obj(i8*)\n");
    EMIT(g, "declare i64 @lume_bi_bool_slot(i8*)\n");

    /* Lists and maps are heap objects the same shape the interpreter hands
     * out, so they travel as opaque pointers. */
    /* One declaration per helper src/rt.c defines. A missing line here does not
     * fail loudly at emit time — it surfaces as "use of undefined value
     * '@lume_list_push_i'" from clang, which names the helper but not why it
     * is missing, so keep this list and rt.c in lockstep. */
    EMIT(g, "; list / map runtime (src/rt.c)\n");
    EMIT(g, "declare i8* @lume_list_new()\n");
    EMIT(g, "declare i64 @lume_list_len(i8*)\n");
    EMIT(g, "declare i64 @lume_list_push_i(i8*, i64)\n");
    EMIT(g, "declare i64 @lume_list_push_b(i8*, i64)\n");
    EMIT(g, "declare i64 @lume_list_push_f(i8*, double)\n");
    EMIT(g, "declare i64 @lume_list_push_s(i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_list_push_obj(i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_list_at_i(i8*, i64)\n");
    EMIT(g, "declare double @lume_list_at_f(i8*, i64)\n");
    EMIT(g, "declare i8* @lume_list_at_s(i8*, i64)\n");
    EMIT(g, "declare i8* @lume_list_at_obj(i8*, i64)\n");
    EMIT(g, "declare i8* @lume_list_at_slot(i8*, i64)\n");
    EMIT(g, "declare i64 @lume_list_print(i8*)\n");
    EMIT(g, "declare void @lume_list_sort(i8*)\n");
    /* Quant builtins (2026-10-24): list-argument statistics / time-series
     * helpers. They live outside the numeric BUILTINS table (sort()'s
     * pattern — a list argument cannot travel through the i64/double
     * columns), so their signatures are declared here. */
    EMIT(g, "declare double @lume_bi_mean(i8*)\n");
    EMIT(g, "declare double @lume_bi_variance(i8*)\n");
    EMIT(g, "declare double @lume_bi_stddev(i8*)\n");
    EMIT(g, "declare double @lume_bi_median(i8*)\n");
    EMIT(g, "declare double @lume_bi_percentile(i8*, double)\n");
    EMIT(g, "declare double @lume_bi_covariance(i8*, i8*)\n");
    EMIT(g, "declare double @lume_bi_correlation(i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_bi_linreg(i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_bi_alpha_beta(i8*, i8*, double)\n");
    EMIT(g, "declare double @lume_bi_information_ratio(i8*, i8*)\n");
    EMIT(g, "declare double @lume_bi_calmar_ratio(i8*)\n");
    EMIT(g, "declare double @lume_bi_autocorrelation(i8*, i64)\n");
    EMIT(g, "declare double @lume_bi_treynor_ratio(i8*, i8*, double)\n");
    EMIT(g, "declare double @lume_bi_omega_ratio(i8*, double)\n");
    EMIT(g, "declare i8* @lume_bi_rank(i8*)\n");
    EMIT(g, "declare double @lume_bi_spearman(i8*, i8*)\n");
    EMIT(g, "declare double @lume_bi_ic(i8*, i8*, i64)\n");
    EMIT(g, "declare double @lume_bi_annualized_return(i8*)\n");
    EMIT(g, "declare double @lume_bi_annualized_volatility(i8*)\n");
    EMIT(g, "declare double @lume_bi_sharpe_annualized(i8*, double)\n");
    EMIT(g, "declare double @lume_bi_cumulative_return(i8*)\n");
    EMIT(g, "declare double @lume_bi_semivariance(i8*)\n");
    EMIT(g, "declare double @lume_bi_downside_deviation(i8*, double)\n");
    EMIT(g, "declare double @lume_bi_param_var(i8*, double)\n");
    EMIT(g, "declare double @lume_bi_kelly_fraction(i8*)\n");
    EMIT(g, "declare i8* @lume_bi_pct_change(i8*)\n");
    EMIT(g, "declare double @lume_bi_max_drawdown(i8*)\n");
    EMIT(g, "declare double @lume_bi_skew(i8*)\n");
    EMIT(g, "declare double @lume_bi_kurtosis(i8*)\n");
    EMIT(g, "declare i8* @lume_bi_rolling_mean(i8*, i64)\n");
    EMIT(g, "declare i8* @lume_bi_rolling_stddev(i8*, i64)\n");
    EMIT(g, "declare i8* @lume_bi_rsi(i8*, i64)\n");
    EMIT(g, "declare i8* @lume_bi_macd(i8*, i64, i64, i64)\n");
    EMIT(g, "declare i8* @lume_bi_bollinger(i8*, i64, double)\n");
    EMIT(g, "declare i8* @lume_bi_rolling_sharpe(i8*, i64, double)\n");
    EMIT(g, "declare i8* @lume_bi_rolling_sortino(i8*, i64, double)\n");
    EMIT(g, "declare i8* @lume_bi_rolling_var(i8*, i64, double)\n");
    EMIT(g, "declare i8* @lume_bi_rolling_cvar(i8*, i64, double)\n");
    EMIT(g, "declare i8* @lume_bi_ewma(i8*, double)\n");
    EMIT(g, "declare i64 @lume_bi_ftoi(double)\n");

    EMIT(g, "declare i8* @lume_map_new()\n");
    EMIT(g, "declare i64 @lume_map_len(i8*)\n");
    EMIT(g, "declare i64 @lume_map_put_i(i8*, i8*, i64)\n");
    EMIT(g, "declare i64 @lume_map_put_b(i8*, i8*, i64)\n");
    EMIT(g, "declare i64 @lume_map_put_f(i8*, i8*, double)\n");
    EMIT(g, "declare i64 @lume_map_put_s(i8*, i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_map_put_obj(i8*, i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_map_put_slot(i8*, i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_map_clone(i8*)\n");    EMIT(g, "declare i64 @lume_map_get_i(i8*, i8*, i64)\n");
    EMIT(g, "declare double @lume_map_get_f(i8*, i8*, double)\n");
    EMIT(g, "declare i8* @lume_map_get_s(i8*, i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_map_get_obj(i8*, i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_map_get_any(i8*, i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_map_get_slot(i8*, i8*, i8*)\n");
    /* Cast accessors (int/float/str on a map + key): the interpreter's
     * value_from_map table, answering in the cast's own type whatever
     * the field holds — see emit_acc_call below. */
    EMIT(g, "declare i64 @lume_map_acc_i(i8*, i8*, i64)\n");
    EMIT(g, "declare double @lume_map_acc_f(i8*, i8*, double)\n");
    EMIT(g, "declare i8* @lume_map_acc_s(i8*, i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_map_acc_b(i8*, i8*, i64)\n");
    EMIT(g, "declare i64 @lume_map_has(i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_str_eq(i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_value_eq(i8*, i8*)\n");   /* structural, SPEC 1.8 */
    EMIT(g, "declare i64 @lume_any_eq(i8*, i8*)\n");     /* any vs i8*-lowered */
    /* Index syntax (SPEC 6.2): one entry point per result shape, each deciding
     * list-vs-map at run time because the emitter cannot always tell. */
    EMIT(g, "declare i64 @lume_index_int(i8*, i64, i8*)\n");
    EMIT(g, "declare double @lume_index_float(i8*, i64, i8*)\n");
    EMIT(g, "declare i8* @lume_index_ptr(i8*, i64, i8*)\n");
    EMIT(g, "declare i8* @lume_index_slot(i8*, i64, i8*)\n");
    EMIT(g, "declare i64 @lume_index_failed()\n");
    EMIT(g, "declare void @lume_index_error(i8*)\n");
    EMIT(g, "declare i8* @lume_map_key_at(i8*, i64)\n");
    EMIT(g, "declare i8* @lume_map_keys(i8*)\n");
    EMIT(g, "declare i8* @lume_range(double, double, double)\n");
    EMIT(g, "declare i64 @lume_map_print(i8*)\n");
    /* Tagged-any values (json(), for-in over a list<any>): these unpack a
     * boxed LumeSlot* (src/rt.c) before the typed read or serialization. */
    EMIT(g, "declare i8* @lume_json_parse(i8*, i8*, i64)\n");
    EMIT(g, "declare i8* @lume_slot_str(i8*)\n");
    EMIT(g, "declare i8* @lume_slot_str_scalar(i8*)\n");
    EMIT(g, "declare i64 @lume_slot_get_i(i8*, i8*, i64)\n");
    EMIT(g, "declare double @lume_slot_get_f(i8*, i8*, double)\n");
    EMIT(g, "declare i8* @lume_slot_get_s(i8*, i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_slot_get_obj(i8*, i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_slot_get_any(i8*, i8*, i8*)\n");
    /* The same cast accessors on a tagged-any receiver (json(), for-in). */
    EMIT(g, "declare i64 @lume_slot_acc_i(i8*, i8*, i64)\n");
    EMIT(g, "declare double @lume_slot_acc_f(i8*, i8*, double)\n");
    EMIT(g, "declare i8* @lume_slot_acc_s(i8*, i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_slot_acc_b(i8*, i8*, i64)\n");
    EMIT(g, "declare i64 @lume_slot_len(i8*)\n");
    EMIT(g, "declare i8* @lume_slot_at(i8*, i64)\n");
    EMIT(g, "declare i8* @lume_slot_at_key(i8*, i8*)\n");

    /* Closures (SPEC 4.8): the box a lambda literal builds and every call
     * site unwraps. The record layout lives in src/rt.c (LumeClosure). */
    EMIT(g, "; closures (src/rt.c)\n");
    EMIT(g, "declare i8* @lume_closure_make(i8*, i8*)\n");

    /* try(fn) (SPEC 6.3): the runtime records the first recoverable error
     * inside try() instead of exiting, and hands it back to the wrapper. */
    EMIT(g, "declare void @lume_error_begin()\n");
    EMIT(g, "declare i8* @lume_error_end()\n");
    EMIT(g, "declare void @lume_error_set(i8*)\n");

    /* Native server runtime (src/bridge_native.c): the FFI surface that
     * server{}/route handlers/run() lower onto. SrvReq's layout is a
     * contract with bridge_native.c (four pointers then one i64). */
    EMIT(g, "; native server runtime (src/bridge_native.c)\n");
    EMIT(g, "declare i8* @lume_bi_env(i8*)\n");
    /* File builtins (SPEC §6.2): read_file returns a string-or-null,
     * write_file/lock_file are booleans, unlock_file releases the lock.
     * Declared here because they take string parameters and therefore sit
     * outside the numeric builtin table. */
    EMIT(g, "declare i8* @lume_bi_read_file(i8*)\n");
    EMIT(g, "declare i64 @lume_bi_write_file(i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_bi_lock_file(i8*, i64)\n");
    EMIT(g, "declare i64 @lume_bi_unlock_file()\n");
    EMIT(g, "%%struct.SrvReq = type { i8*, i8*, i8*, i8*, i64, i8* }\n");
    EMIT(g, "declare void @lume_srv_init(i8*, i64)\n");
    EMIT(g, "declare void @lume_srv_route(i8*, i8*, void (%%struct.SrvReq*, i8**, i64*, i32*)*)\n");
    EMIT(g, "declare void @lume_srv_default_handler(%%struct.SrvReq*, i8**, i64*, i32*)\n");
    EMIT(g, "declare void @lume_srv_run()\n");
    EMIT(g, "declare i8* @lume_srv_qp(i8*, i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_srv_atoi(i8*)\n");
    EMIT(g, "declare void @lume_srv_json_begin(i8**, i64*)\n");
    EMIT(g, "declare void @lume_srv_json_key(i8**, i64*, i8*)\n");
    EMIT(g, "declare void @lume_srv_json_str(i8**, i64*, i8*)\n");
    EMIT(g, "declare void @lume_srv_json_int(i8**, i64*, i64)\n");
    EMIT(g, "declare void @lume_srv_json_bool(i8**, i64*, i32)\n");
    EMIT(g, "declare void @lume_srv_json_end(i8**, i64*)\n");

    /* Native SQLite runtime (src/bridge_native.c, compiled only when the
     * build probed libsqlite3): sql_query returns a row-list (i8* to a
     * LumeList) or null on error; sql_write returns the affected count
     * (-1 on error); tx handles are opaque i8* (heap LumeSqlTx). */
#if LUME_HAS_SQLITE
    EMIT(g, "; native sqlite runtime (src/bridge_native.c)\n");
    EMIT(g, "declare i8* @lume_bi_sql_query(i8*, i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_bi_sql_write(i8*, i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_bi_sql_tx_begin(i8*)\n");
    EMIT(g, "declare i8* @lume_bi_sql_tx_query(i8*, i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_bi_sql_tx_exec(i8*, i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_bi_sql_tx_commit(i8*)\n");
    EMIT(g, "declare i64 @lume_bi_sql_tx_rollback(i8*)\n");
#endif
}

/* get(m, k[, default]) emission — the shared core of the get() branch in
 * cg_builtin and of the str()/int()/float()/stringify()/json() accessor
 * shorthand: the interpreter spells str(m, k[, d]) as str(get(m, k[, d]))
 * (builtins.c value_from_map), so the casts desugar to a get() read here
 * too. dnode == NULL is the two-argument form. Kept in step with the llvm
 * emitter's copy in llvm_cg_call.inc. */
static Val emit_get_call(CG *g, Node *mnode, Node *knode, Node *dnode,
                         size_t line)
{
    Val m = cg_expr(g, mnode);
    Val k = cg_expr(g, knode);
    Val d = dnode ? cg_expr(g, dnode) : (Val){ NULL, NULL, 0 };
    if (!m.v || !k.v)
        ERRV(g, "line %zu: get() needs a map, a key and a scalar default", line);

    /* The key's value shape, resolved while the receiver still carries its
     * declared field list: boxing a named struct into a map below keeps the
     * field names only at runtime, so the shape has to be read off the SDef
     * first. NULL for an any/list receiver (map_key_shape needs a struct). */
    Type *shape = map_key_shape(g, &m, knode);

    /* A named struct is a raw GEP struct with no runtime map header, so the
     * lume_map_get_* getters would read its first field as the header — a
     * type confusion that segfaulted both native backends on get(p, "f")
     * while the interpreter answered correctly. Box it into a real map
     * first, the conversion print()/str() already use for named structs. */
    if (m.ty && m.ty->name) {
        struct_addr(g, &m);
        Val mm = emit_struct_to_map(g, m.v, m.ty);
        if (!mm.v) {
            free(m.v); free(k.v); free(d.v);
            ERRV(g, "line %zu: get() cannot read struct '%s'",
                 line, m.ty->name);
        }
        free(m.v);
        m.v = mm.v;
        m.ty = mm.ty;
    }

    /* The flavour follows the *shape* of the key when the map's static
     * type declares it: the value at that key is always the declared
     * type. A default of a different type could only ever fire for an
     * absent key, so it is refused here rather than silently reading the
     * slot through the wrong getter. Without a shape the flavour follows
     * the *default* — so an unannotated get(m, k) in a bool context stays
     * an int read rather than silently becoming a string — and the int
     * read when neither says (an absent key then reads as the zero of
     * that flavour, spelled below). */
    const char *fn = "lume_map_get_i";
    Type *rty = type_prim(TY_INT);
    if (m.ty && m.ty->kind == TY_ANY &&
        knode && knode->type == N_LITERAL && knode->as.lit.kind == LIT_NUM &&
        !knode->as.lit.is_float) {
        /* An any receiver with an integer key is a list-element read —
         * get(j, 0) on a json array, a str_match list. The receiver is a
         * boxed LumeSlot*; lume_slot_at takes (slot, i64) and returns a
         * boxed slot, so the read is always `any`. A default would need
         * the absent-key fallback the slot helper does not offer, so the
         * three-argument form is refused rather than silently misread.
         * Kept in step with the llvm emitter's copy. */
        if (d.v)
            ERRV(g, "line %zu: get() on an any list with a default is not "
                    "supported on the native backends", line);
        Val r = rt_call(g, type_prim(TY_ANY), "lume_slot_at",
                        "i8* %s, i64 %s", m.v, k.v);
        free(m.v); free(k.v);
        if (!r.v) ERRV(g, "line %zu: get() failed", line);
        return r;
    }
    if (m.ty && m.ty->kind == TY_ANY) {
        /* An any-typed map operand: the value is a boxed LumeSlot* whose
         * tag only exists at runtime, so the read must unpack the slot
         * before the lookup (lume_slot_get_* in src/rt.c). There is no
         * static shape to consult, so the flavour follows the default's
         * static type; without a concrete default the raw slot is read
         * (kept in step with the llvm emitter's copy). */
        if (d.v && d.ty && d.ty->kind != TY_ANY) {
            const char *sfn; Type *srty;
            if (!get_flavour(d.ty, &sfn, &srty))
                ERRV(g, "line %zu: get() cannot read a %s off an any value",
                     line, ty_str(d.ty));
            if (strcmp(sfn, "lume_map_get_i") == 0)
                fn = "lume_slot_get_i";
            else if (strcmp(sfn, "lume_map_get_f") == 0)
                fn = "lume_slot_get_f";
            else if (strcmp(sfn, "lume_map_get_s") == 0)
                fn = "lume_slot_get_s";
            else if (strcmp(sfn, "lume_map_get_obj") == 0)
                fn = "lume_slot_get_obj";
            else
                ERRV(g, "line %zu: get() cannot read a %s off an any value",
                     line, ty_str(d.ty));
            /* The result keeps the default's own type where codegen has one:
             * a [] default reads a json array back as a list, a {} default an
             * anonymous map, a string default a string. BOOL keeps the
             * get_flavour mapping (an int read), matching get() on a typed
             * map, so `get(j, "ok", false)` prints "1" the same way
             * `get(m, "ok", false)` does on a literal map. */
            rty = d.ty && d.ty->kind != TY_BOOL ? d.ty : srty;
        } else {
            /* No default (or an any default): read the cell back boxed
             * as its own tag — the interpreter's dynamic read. Kept in
             * step with the llvm emitter's copy. */
            fn = "lume_slot_get_any";
            rty = type_prim(TY_ANY);
        }
    } else if (m.ty && m.ty->kind == TY_LIST) {
        /* get(list, i[, default]): an index read — the interpreter's
         * dynamic get() on a list. The element type drives the accessor;
         * a default of a different scalar kind is refused like the map
         * path. */
        Type *et = m.ty->elem;
        /* A bare `list` annotation carries no element type: the read is
         * the dynamic slot path (lume_list_at_slot / any). The default
         * check below still sees the raw et (NULL skips it, as before). */
        const char *atfn = et ? list_at_fn(et) : "lume_list_at_slot";
        if (!atfn)
            ERRV(g, "line %zu: get() cannot index a list of %s",
                 line, et ? ty_str(et) : "unknown");
        if (d.v && d.ty && d.ty->kind != TY_ANY &&
            !(et && d.ty->kind == et->kind) &&
            !(d.ty->kind == TY_BOOL && et && et->kind == TY_INT))
            ERRV(g, "line %zu: get() default is %s but the list holds %s",
                 line, ty_str(d.ty), et ? ty_str(et) : "unknown");
        const char *art = et && et->kind == TY_STRING ? "i8*" :
                          et && et->kind == TY_FLOAT ? "double" :
                          (!et || et->kind == TY_ANY) ? "i8*" : "i64";
        Type *rty = et && et->kind != TY_ANY ? et : any_type();
        char *call = emit_instrf(g, rty,
                                 "call %s @%s(i8* %s, i64 %s)",
                                 art, atfn, m.v, k.v);
        free(m.v); free(k.v); free(d.v);
        return val_take(rty, call);
    } else {
        if (shape && shape->kind != TY_ANY) {
            const char *sfn; Type *srty;
            if (!get_flavour(shape, &sfn, &srty))
                ERRV(g, "line %zu: get() cannot read a value of type %s",
                     line, ty_str(shape));
            if (d.v && d.ty && d.ty->kind != TY_ANY && d.ty->kind != shape->kind &&
                !(d.ty->kind == TY_BOOL && shape->kind == TY_INT) &&
                !(d.ty->kind == TY_INT && shape->kind == TY_BOOL))
                ERRV(g, "line %zu: get() default is %s but key %s holds %s",
                     line, ty_str(d.ty), knode->as.lit.text, ty_str(shape));
            fn = sfn; rty = srty;
        } else if (m.ty && m.ty->kind == TY_STRUCT && !m.ty->name &&
                   struct_common_shape(m.ty) == NULL && !d.v) {
            /* Undeclared struct receiver (a list-of-maps literal element,
             * a str_match row, a json-parsed row) with no default: no static
             * field the typed getters could read and no default to infer the
             * flavour from, so this is the interpreter's dynamic read. Hand
             * back a boxed slot (any) -- lume_map_get_slot -- the same boxed
             * representation for-in and get-on-any use. When a default IS
             * present the next branch picks the typed getter by the default's
             * type, which is the correct (and verifier-clean) path for a
             * literal struct like `{ "count": 7 }` whose anon type carries no
             * field names natively. */
            fn = "lume_map_get_slot";
            rty = type_prim(TY_ANY);
        } else if (d.v && d.ty && d.ty->kind != TY_ANY) {
            const char *dfn; Type *drty;
            if (get_flavour(d.ty, &dfn, &drty)) { fn = dfn; rty = drty; }
        }
    }

    char dflt[96];
    const char *darg = "i64 0";
    if (d.v) {
        if (d.ty->kind == TY_BOOL) d = coerce(g, type_prim(TY_INT), d, line);
        snprintf(dflt, sizeof dflt, "%s %s", rt_arg_type(d.ty), d.v);
        darg = dflt;
        free(d.v);
    } else if (rty->kind == TY_STRING || rty->kind == TY_NULL ||
               rty->kind == TY_LIST || rty->kind == TY_STRUCT ||
               rty->kind == TY_RESULT) {
        /* The pointer-returning getters take an i8* default; i64 0 would
         * not verify. */
        darg = "i8* null";
    } else if (rty->kind == TY_FLOAT) {
        darg = "double 0.0";
    }
    Val r = rt_call(g, rty, fn, "i8* %s, i8* %s, %s", m.v, k.v, darg);
    free(m.v); free(k.v);
    if (!r.v) ERRV(g, "line %zu: get() failed", line);
    return r;
}

/* Convert an accessor's default expression to the cast's own type, with
 * the same lowerings the direct casts use (int("12x") parses a leading
 * number, float(3) widens, str(3) renders). The interpreter hands the
 * default back verbatim — whatever type it has — but the static
 * accessor answers in the cast's type, so the default has to cross the
 * same bridge the cast would build. A bool default widens numerically
 * (the getters' bool defaults always did); a null default is treated as
 * absent, since null is not representable in a scalar cast type and the
 * two-argument form's absent-key answer is the closest static answer. */
static Val acc_default(CG *g, Type *rty, Val d, size_t line)
{
    if (!d.v) return d;
    if (d.ty && d.ty->kind == TY_NULL) { free(d.v); return (Val){ NULL, NULL, 0 }; }
    /* A bool default travels as the interpreter's bool representation,
     * i64 — widen it before the same-kind shortcut below can pass an
     * i1 through to a helper that takes i64. */
    if (d.ty && d.ty->kind == TY_BOOL && rty->kind == TY_BOOL)
        return coerce(g, type_prim(TY_INT), d, line);
    if (!d.ty || d.ty->kind == rty->kind) return d;

    /* A bool default widens numerically — the widening the getters'
     * bool defaults always used (the interpreter hands the bool itself
     * back; the static accessor answers in the cast's type). For a bool
     * accessor the helper's answer IS the interpreter's bool
     * representation (i64), so the widening is the whole conversion. */
    if (d.ty->kind == TY_BOOL &&
        (rty->kind == TY_INT || rty->kind == TY_FLOAT || rty->kind == TY_BOOL)) {
        Val i = coerce(g, type_prim(TY_INT), d, line);
        if (!i.v) return i;
        if (rty->kind != TY_FLOAT) return i;
        /* coerce() already freed i.v on its way out (val_take semantics) —
         * freeing it again here is the double free the float leg hit. */
        return coerce(g, type_prim(TY_FLOAT), i, line);
    }

    const char *fn = NULL, *pty = NULL, *rs = NULL;
    switch (rty->kind) {
    case TY_INT:
        if (d.ty->kind == TY_STRING)      { fn = "@lume_bi_parse_i64";  pty = "i8*";   rs = "i64"; }
        else if (d.ty->kind == TY_FLOAT)  { fn = "@lume_bi_int";        pty = "double"; rs = "i64"; }
        break;
    case TY_FLOAT:
        if (d.ty->kind == TY_STRING)      { fn = "@lume_bi_parse_double"; pty = "i8*"; rs = "double"; }
        else if (d.ty->kind == TY_INT)    { fn = "@lume_bi_float";      pty = "i64";   rs = "double"; }
        break;
    case TY_STRING:
        if (d.ty->kind == TY_INT)         { fn = "@lume_bi_str_i64";    pty = "i64";   rs = "i8*"; }
        else if (d.ty->kind == TY_FLOAT)  { fn = "@lume_bi_str_double"; pty = "double"; rs = "i8*"; }
        else if (d.ty->kind == TY_BOOL)   { fn = "@lume_bi_str_bool";   pty = "i64";   rs = "i8*"; }
        else if (d.ty->kind == TY_ANY)    { fn = "@lume_slot_str_scalar"; pty = "i8*"; rs = "i8*"; }
        else if (d.ty->kind == TY_LIST ||
                 (d.ty->kind == TY_STRUCT && !d.ty->name))
                                          { fn = "@lume_rt_to_json_obj"; pty = "i8*"; rs = "i8*"; }
        break;
    case TY_BOOL:
        /* A bool accessor's default crosses by its own truthiness — the
         * same helpers the direct bool() cast dispatches to (value.c
         * value_truthy), answering in the interpreter's bool
         * representation (i64). */
        if (d.ty->kind == TY_INT)         { fn = "@lume_bi_bool_i";    pty = "i64";    rs = "i64"; }
        else if (d.ty->kind == TY_STRING) { fn = "@lume_bi_bool_s";    pty = "i8*";    rs = "i64"; }
        else if (d.ty->kind == TY_FLOAT)  { fn = "@lume_bi_bool_f";    pty = "double"; rs = "i64"; }
        else if (d.ty->kind == TY_ANY)    { fn = "@lume_bi_bool_slot"; pty = "i8*";    rs = "i64"; }
        else if (d.ty->kind == TY_LIST ||
                 (d.ty->kind == TY_STRUCT && !d.ty->name))
                                          { fn = "@lume_bi_bool_obj";  pty = "i8*";    rs = "i64"; }
        break;
    default: break;
    }
    if (!fn) {
        free(d.v);
        ERRV(g, "line %zu: accessor default cannot convert to %s",
             line, ty_str(rty));
    }

    /* The runtime's bool builtins take the interpreter's bool
     * representation (i64), so an i1 default widens before the call. */
    if (d.ty->kind == TY_BOOL && strcmp(pty, "i64") == 0) {
        char *w = emit_instrf(g, type_prim(TY_BOOL), "zext i1 %s to i64", d.v);
        free(d.v);
        if (!w) return (Val){ NULL, NULL, 0 };
        d.v = w;
    }
    char tmp[48];
    snprintf(tmp, sizeof tmp, "%%c%d", g->tid++);
    EMIT(g, "  %s = call %s %s(%s %s)\n", tmp, rs, fn, pty, d.v);
    free(d.v);
    return val_make(rty, tmp);
}

/* str/int/float/bool accessor emission: int(m, "k"[, d]) and friends —
 * the interpreter's value_from_map + native_int/native_float/
 * native_str/native_bool (src/builtins.c), where the answer is ALWAYS
 * the cast's own type no matter what the field holds or what the
 * default is. That is why this exists next to emit_get_call instead of
 * routing through it: there, a default whose type differs from the
 * key's declared shape is refused (a get() default of the wrong type
 * would read the slot through the wrong getter), but for an accessor a
 * mismatched default is the normal case — `int(acc, "n", 0)` reading
 * the string "42" is the point of the sugar. The runtime helpers
 * lume_map_acc_* / lume_slot_acc_* (src/rt.c) carry the interpreter's
 * whole table, so neither the field's static type nor the default's
 * type picks the flavour here. Kept in step with the llvm emitter's
 * copy. */
static Val emit_acc_call(CG *g, Node *mnode, Node *knode, Node *dnode,
                         bool is_str, bool is_float, bool is_bool,
                         bool is_any, size_t line)
{
    Val m = cg_expr(g, mnode);
    Val k = cg_expr(g, knode);
    Val d = dnode ? cg_expr(g, dnode) : (Val){ NULL, NULL, 0 };
    if (!m.v || !k.v)
        ERRV(g, "line %zu: accessor needs a map and a key", line);

    /* A named struct is a raw GEP struct with no runtime map header, so
     * the helper would read its first field as the header — the same
     * boxing get() does before its getters run. An any receiver is
     * already the boxed slot the slot helpers unpack. */
    if (!is_any && m.ty && m.ty->name) {
        struct_addr(g, &m);
        Val mm = emit_struct_to_map(g, m.v, m.ty);
        if (!mm.v) {
            free(m.v); free(k.v); free(d.v);
            ERRV(g, "line %zu: accessor cannot read struct '%s'",
                 line, m.ty->name);
        }
        free(m.v);
        m.v = mm.v;
        m.ty = mm.ty;
    }

    /* The helpers answer the cast's own type; bool answers the
     * interpreter's bool representation (i64), narrowed to i1 below
     * like the direct bool() cast does. */
    Type *rty = is_str ? type_prim(TY_STRING)
             : is_float ? type_prim(TY_FLOAT)
                        : type_prim(TY_INT);
    const char *fn = is_str ? (is_any ? "lume_slot_acc_s" : "lume_map_acc_s")
                   : is_float ? (is_any ? "lume_slot_acc_f" : "lume_map_acc_f")
                   : is_bool ? (is_any ? "lume_slot_acc_b" : "lume_map_acc_b")
                             : (is_any ? "lume_slot_acc_i" : "lume_map_acc_i");

    if (d.v) {
        d = acc_default(g, is_bool ? type_prim(TY_BOOL) : rty, d, line);
        if (g->err[0]) {
            free(m.v); free(k.v);
            return (Val){ NULL, NULL, 0 };
        }
    }

    char dflt[96];
    const char *darg;
    if (d.v) {
        snprintf(dflt, sizeof dflt, "%s %s", rt_arg_type(rty), d.v);
        darg = dflt;
        free(d.v);
    } else if (rty->kind == TY_STRING) {
        /* The two-argument form's absent-key answer is the interpreter's
         * str_of_value(null): the runtime renders that word for a NULL
         * default. */
        darg = "i8* null";
    } else if (rty->kind == TY_FLOAT) {
        darg = "double 0.0";
    } else {
        /* int, float and bool all answer the flavour's zero (the
         * interpreter's val_num(0) / value_truthy(null)). */
        darg = "i64 0";
    }
    Val r = rt_call(g, rty, fn, "i8* %s, i8* %s, %s", m.v, k.v, darg);
    free(m.v); free(k.v);
    if (!r.v) ERRV(g, "line %zu: accessor failed", line);
    if (is_bool) {
        char *bv = emit_instrf(g, type_prim(TY_BOOL), "trunc i64 %s to i1", r.v);
        free(r.v);
        if (!bv) ERRV(g, "line %zu: accessor failed", line);
        return val_take(type_prim(TY_BOOL), bv);
    }
    return r;
}

static Val cg_builtin(CG *g, Node *n)
{
    const char *bname = n->as.call.callee->as.var.name;
    int argc = n->as.call.argc;
    const Bi *b = builtin_find(bname);

    /* now(): epoch seconds (SPEC §6.2, matching the interpreter's now()) —
     * no arguments, so the table path below (which requires argc >= 1)
     * cannot serve it. */
    if (strcmp(bname, "now") == 0) {
        if (argc != 0)
            ERRV(g, "line %zu: now() takes no arguments", n->line);
        return val_take(type_prim(TY_INT),
                        emit_instrf(g, type_prim(TY_INT),
                                    "call i64 @lume_bi_now()"));
    }

    /* argv(): the script's trailing CLI arguments as a string list. Also zero
     * arguments, so the table path — which requires argc >= 1 — cannot serve
     * it; the list the runtime hands back is stashed by lume_cli_init in the
     * generated entry point. */
    if (strcmp(bname, "argv") == 0) {
        if (argc != 0)
            ERRV(g, "line %zu: argv() takes no arguments", n->line);
        return val_take(list_type(),
                        emit_instrf(g, list_type(),
                                    "call i8* @lume_bi_argv()"));
    }

    /* env(name): a host environment variable as a string; unset or masked
     * (credential) names read as null — same promise as the interpreter's
     * native_env (builtins_fs.c), kept by lume_bi_env (bridge_native.c). */
    if (strcmp(bname, "env") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: env() takes exactly one argument", n->line);
        Val key = cg_expr(g, n->as.call.args[0]);
        if (!key.v) return (Val){ NULL, NULL, 0 };
        Val r = val_take(type_prim(TY_STRING),
                         emit_instrf(g, type_prim(TY_STRING),
                                     "call i8* @lume_bi_env(i8* %s)",
                                     key.v));
        free(key.v);
        return r;
    }

    /* Monte Carlo RNG (2026-10-24, rt.c lume_bi_*): rand() takes no
     * arguments; the others take numbers — an int passes straight through,
     * a float widens/truncates through lume_bi_float / lume_bi_ftoi (the
     * same conversions the interpreter applies to its double arguments). */
    if (strcmp(bname, "seed") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: seed() takes exactly one argument", n->line);
        Val s = cg_expr(g, n->as.call.args[0]);
        if (!s.v || !s.ty || (s.ty->kind != TY_INT && s.ty->kind != TY_FLOAT)) {
            free(s.v);
            ERRV(g, "line %zu: seed() needs a number", n->line);
        }
        if (s.ty->kind == TY_FLOAT) {
            char *si = emit_instrf(g, type_prim(TY_INT),
                                   "call i64 @lume_bi_ftoi(double %s)", s.v);
            free(s.v);
            s.v = si;
        }
        char *t = emit_instrf(g, type_prim(TY_INT),
                              "call i64 @lume_bi_seed(i64 %s)", s.v);
        free(s.v);
        return val_make(type_prim(TY_INT), t);
    }
    if (strcmp(bname, "rand") == 0) {
        if (argc != 0)
            ERRV(g, "line %zu: rand() takes no arguments", n->line);
        return val_make(type_prim(TY_FLOAT),
                        emit_instrf(g, type_prim(TY_FLOAT),
                                    "call double @lume_bi_rand()"));
    }
    if (strcmp(bname, "rand_range") == 0 || strcmp(bname, "rand_normal") == 0 ||
        strcmp(bname, "rand_cauchy") == 0 || strcmp(bname, "rand_gamma") == 0 ||
        strcmp(bname, "rand_beta") == 0 || strcmp(bname, "rand_weibull") == 0 ||
        strcmp(bname, "rand_logistic") == 0 || strcmp(bname, "rand_pareto") == 0 ||
        strcmp(bname, "rand_laplace") == 0 || strcmp(bname, "rand_lognormal") == 0 ||
        strcmp(bname, "rand_gumbel") == 0 || strcmp(bname, "rand_triangular") == 0 ||
        strcmp(bname, "rand_von_mises") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: %s() takes two numbers", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !a.ty || (a.ty->kind != TY_INT && a.ty->kind != TY_FLOAT) ||
            !b.v || !b.ty || (b.ty->kind != TY_INT && b.ty->kind != TY_FLOAT)) {
            free(a.v); free(b.v);
            ERRV(g, "line %zu: %s() needs two numbers", n->line, bname);
        }
        if (a.ty->kind == TY_INT) {
            char *w = emit_instrf(g, type_prim(TY_FLOAT),
                                  "call double @lume_bi_float(i64 %s)", a.v);
            free(a.v);
            a.v = w;
        }
        if (b.ty->kind == TY_INT) {
            char *w = emit_instrf(g, type_prim(TY_FLOAT),
                                  "call double @lume_bi_float(i64 %s)", b.v);
            free(b.v);
            b.v = w;
        }
        const char *fn;
        if (strcmp(bname, "rand_range") == 0) fn = "lume_bi_rand_range";
        else if (strcmp(bname, "rand_normal") == 0) fn = "lume_bi_rand_normal";
        else if (strcmp(bname, "rand_cauchy") == 0) fn = "lume_bi_rand_cauchy";
        else if (strcmp(bname, "rand_gamma") == 0) fn = "lume_bi_rand_gamma";
        else if (strcmp(bname, "rand_beta") == 0) fn = "lume_bi_rand_beta";
        else if (strcmp(bname, "rand_weibull") == 0) fn = "lume_bi_rand_weibull";
        else if (strcmp(bname, "rand_logistic") == 0) fn = "lume_bi_rand_logistic";
        else if (strcmp(bname, "rand_pareto") == 0) fn = "lume_bi_rand_pareto";
        else if (strcmp(bname, "rand_laplace") == 0) fn = "lume_bi_rand_laplace";
        else if (strcmp(bname, "rand_lognormal") == 0) fn = "lume_bi_rand_lognormal";
        else if (strcmp(bname, "rand_gumbel") == 0) fn = "lume_bi_rand_gumbel";
        else if (strcmp(bname, "rand_von_mises") == 0) fn = "lume_bi_rand_von_mises";
        else fn = "lume_bi_rand_triangular";
        char *t = emit_instrf(g, type_prim(TY_FLOAT),
                              "call double @%s(double %s, double %s)", fn, a.v, b.v);
        free(a.v); free(b.v);
        return val_make(type_prim(TY_FLOAT), t);
    }
    if (strcmp(bname, "rand_int") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: rand_int() takes (lo, hi)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !a.ty || (a.ty->kind != TY_INT && a.ty->kind != TY_FLOAT) ||
            !b.v || !b.ty || (b.ty->kind != TY_INT && b.ty->kind != TY_FLOAT)) {
            free(a.v); free(b.v);
            ERRV(g, "line %zu: rand_int() needs two numbers", n->line);
        }
        if (a.ty->kind == TY_FLOAT) {
            char *w = emit_instrf(g, type_prim(TY_INT),
                                  "call i64 @lume_bi_ftoi(double %s)", a.v);
            free(a.v);
            a.v = w;
        }
        if (b.ty->kind == TY_FLOAT) {
            char *w = emit_instrf(g, type_prim(TY_INT),
                                  "call i64 @lume_bi_ftoi(double %s)", b.v);
            free(b.v);
            b.v = w;
        }
        char *t = emit_instrf(g, type_prim(TY_INT),
                              "call i64 @lume_bi_rand_int(i64 %s, i64 %s)", a.v, b.v);
        free(a.v); free(b.v);
        return val_make(type_prim(TY_INT), t);
    }

    /* rand_binomial(n, p) (2026-10-27): mixed arity — n is an i64 count
     * (float truncates through lume_bi_ftoi), p a double probability (int
     * widens through lume_bi_float), matching the interpreter's casts.
     * rand_zipf / rand_negative_binomial / rand_erlang (2026-10-28) share
     * the same (i64, double) signature — n truncates, the shape/rate/
     * probability widens — erlang answering a double, the other three an
     * i64. */
    if (strcmp(bname, "rand_binomial") == 0 || strcmp(bname, "rand_zipf") == 0 ||
        strcmp(bname, "rand_negative_binomial") == 0 || strcmp(bname, "rand_erlang") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: %s() takes (n, p)", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !a.ty || (a.ty->kind != TY_INT && a.ty->kind != TY_FLOAT) ||
            !b.v || !b.ty || (b.ty->kind != TY_INT && b.ty->kind != TY_FLOAT)) {
            free(a.v); free(b.v);
            ERRV(g, "line %zu: %s() needs (n, p)", n->line, bname);
        }
        if (a.ty->kind == TY_FLOAT) {
            char *w = emit_instrf(g, type_prim(TY_INT),
                                  "call i64 @lume_bi_ftoi(double %s)", a.v);
            free(a.v);
            a.v = w;
        }
        if (b.ty->kind == TY_INT) {
            char *w = emit_instrf(g, type_prim(TY_FLOAT),
                                  "call double @lume_bi_float(i64 %s)", b.v);
            free(b.v);
            b.v = w;
        }
        const char *fn;
        Type *rt;
        if (strcmp(bname, "rand_binomial") == 0) { fn = "lume_bi_rand_binomial"; rt = type_prim(TY_INT); }
        else if (strcmp(bname, "rand_zipf") == 0) { fn = "lume_bi_rand_zipf"; rt = type_prim(TY_INT); }
        else if (strcmp(bname, "rand_negative_binomial") == 0) { fn = "lume_bi_rand_negative_binomial"; rt = type_prim(TY_INT); }
        else { fn = "lume_bi_rand_erlang"; rt = type_prim(TY_FLOAT); }
        char *t = emit_instrf(g, rt, "call %s @%s(i64 %s, double %s)",
                              rt->kind == TY_FLOAT ? "double" : "i64",
                              fn, a.v, b.v);
        free(a.v); free(b.v);
        return val_make(rt, t);
    }

    /* Distribution family (2026-10-25): rand_bool(p) / rand_exponential(rate)
     * / rand_poisson(lambda) take one number — an int widens through
     * lume_bi_float exactly as the interpreter treats its double argument;
     * shuffle(list) returns a fresh list (Fisher-Yates on the same PCG32
     * stream). */
    if (strcmp(bname, "rand_bool") == 0 || strcmp(bname, "rand_exponential") == 0 ||
        strcmp(bname, "rand_poisson") == 0 || strcmp(bname, "rand_chisq") == 0 ||
        strcmp(bname, "rand_student_t") == 0 || strcmp(bname, "rand_geometric") == 0 ||
        strcmp(bname, "rand_rayleigh") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: %s() takes one number", n->line, bname);
        Val p = cg_expr(g, n->as.call.args[0]);
        if (!p.v || !p.ty || (p.ty->kind != TY_INT && p.ty->kind != TY_FLOAT)) {
            free(p.v);
            ERRV(g, "line %zu: %s() needs a number", n->line, bname);
        }
        if (p.ty->kind == TY_INT) {
            char *w = emit_instrf(g, type_prim(TY_FLOAT),
                                  "call double @lume_bi_float(i64 %s)", p.v);
            free(p.v);
            p.v = w;
        }
        int is_float = strcmp(bname, "rand_exponential") == 0 ||
                       strcmp(bname, "rand_chisq") == 0 || strcmp(bname, "rand_student_t") == 0 ||
                       strcmp(bname, "rand_rayleigh") == 0;
        const char *fn;
        if (strcmp(bname, "rand_bool") == 0) fn = "lume_bi_rand_bool";
        else if (strcmp(bname, "rand_exponential") == 0) fn = "lume_bi_rand_exponential";
        else if (strcmp(bname, "rand_poisson") == 0) fn = "lume_bi_rand_poisson";
        else if (strcmp(bname, "rand_chisq") == 0) fn = "lume_bi_rand_chisq";
        else if (strcmp(bname, "rand_student_t") == 0) fn = "lume_bi_rand_student_t";
        else if (strcmp(bname, "rand_geometric") == 0) fn = "lume_bi_rand_geometric";
        else fn = "lume_bi_rand_rayleigh";
        Type *rt = is_float ? type_prim(TY_FLOAT) : type_prim(TY_INT);
        const char *pty = is_float ? "double" : "i64";
        char *t = emit_instrf(g, rt, "call %s @%s(double %s)", pty, fn, p.v);
        free(p.v);
        return val_make(rt, t);
    }
    if (strcmp(bname, "shuffle") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: shuffle() takes one list", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: shuffle() needs a list", n->line);
        char *t = emit_instrf(g, type_list(type_prim(TY_FLOAT)),
                              "call i8* @lume_bi_shuffle(i8* %s)", a.v);
        free(a.v);
        return val_make(type_list(type_prim(TY_FLOAT)), t);
    }
    /* rand_multinomial(n, probs) (2026-10-28): n weighted draws against a
     * float list of category weights; answers a fresh int list. n
     * truncates through lume_bi_ftoi, probs passes as i8*. */
    if (strcmp(bname, "rand_multinomial") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: rand_multinomial() takes (n, probs)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !a.ty || (a.ty->kind != TY_INT && a.ty->kind != TY_FLOAT) ||
            !b.v || !b.ty || b.ty->kind != TY_LIST) {
            free(a.v); free(b.v);
            ERRV(g, "line %zu: rand_multinomial() needs (n, list)", n->line);
        }
        if (a.ty->kind == TY_FLOAT) {
            char *w = emit_instrf(g, type_prim(TY_INT),
                                  "call i64 @lume_bi_ftoi(double %s)", a.v);
            free(a.v);
            a.v = w;
        }
        char *t = emit_instrf(g, type_list(type_prim(TY_INT)),
                              "call i8* @lume_bi_rand_multinomial(i64 %s, i8* %s)", a.v, b.v);
        free(a.v); free(b.v);
        return val_make(type_list(type_prim(TY_INT)), t);
    }

    /* Financial path simulators (2026-10-28): all-scalar inputs answering
     * a fresh float list — the sim_* family. The last argument is the step
     * count (float truncates through lume_bi_ftoi, like rand_int), every
     * other argument is a double (int widens through lume_bi_float). The
     * interpreter's native_* validate the same way. */
    if (strcmp(bname, "sim_gbm") == 0 || strcmp(bname, "sim_bridge") == 0 ||
        strcmp(bname, "sim_ou") == 0 || strcmp(bname, "sim_merton") == 0) {
        int want = (strcmp(bname, "sim_gbm") == 0 || strcmp(bname, "sim_bridge") == 0) ? 5 :
                   (strcmp(bname, "sim_ou") == 0 ? 6 : 8);
        if (argc != want)
            ERRV(g, "line %zu: %s() takes %d arguments", n->line, bname, want);
        Val *vs = (Val *)calloc((size_t)argc, sizeof(Val));
        for (int i = 0; i < argc; i++) {
            vs[i] = cg_expr(g, n->as.call.args[i]);
            if (!vs[i].v || !vs[i].ty ||
                (vs[i].ty->kind != TY_INT && vs[i].ty->kind != TY_FLOAT)) {
                for (int j = 0; j <= i; j++) free(vs[j].v);
                free(vs);
                ERRV(g, "line %zu: %s() needs numbers", n->line, bname);
            }
        }
        char *nv = vs[argc - 1].v;
        if (vs[argc - 1].ty->kind == TY_FLOAT) {
            char *w = emit_instrf(g, type_prim(TY_INT),
                                  "call i64 @lume_bi_ftoi(double %s)", nv);
            free(nv);
            nv = w;
        }
        const char *fn;
        if (strcmp(bname, "sim_gbm") == 0) fn = "lume_bi_sim_gbm";
        else if (strcmp(bname, "sim_bridge") == 0) fn = "lume_bi_sim_bridge";
        else if (strcmp(bname, "sim_ou") == 0) fn = "lume_bi_sim_ou";
        else fn = "lume_bi_sim_merton";
        char *buf = NULL;
        size_t bl = 0;
        for (int i = 0; i < argc - 1; i++) {
            char *a = vs[i].v;
            if (vs[i].ty->kind == TY_INT) {
                char *w = emit_instrf(g, type_prim(TY_FLOAT),
                                      "call double @lume_bi_float(i64 %s)", a);
                free(a);
                a = w;
            }
            if (i == 0) {
                size_t need = snprintf(NULL, 0, "call i8* @%s(double %s", fn, a) + 1;
                buf = (char *)malloc(need);
                bl = (size_t)snprintf(buf, need, "call i8* @%s(double %s", fn, a);
            } else {
                size_t need = bl + snprintf(NULL, 0, ", double %s", a) + 1;
                buf = (char *)realloc(buf, need);
                bl += (size_t)snprintf(buf + bl, need - bl, ", double %s", a);
            }
            free(a);
        }
        size_t need = bl + snprintf(NULL, 0, ", i64 %s)", nv) + 1;
        buf = (char *)realloc(buf, need);
        snprintf(buf + bl, need - bl, ", i64 %s)", nv);
        free(nv);
        free(vs);
        char *t = emit_instrf(g, type_list(type_prim(TY_FLOAT)), "%s", buf);
        free(buf);
        return val_make(type_list(type_prim(TY_FLOAT)), t);
    }

    /* sim_portfolio(paths, weights) (2026-11-01): two list arguments — a
     * list of equal-length price paths and one finite weight per path —
     * answering the portfolio-return series as a fresh float list. */
    if (strcmp(bname, "sim_portfolio") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: sim_portfolio() takes (paths, weights)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST) {
            free(a.v);
            ERRV(g, "line %zu: sim_portfolio() needs a list of paths", n->line);
        }
        Val w = cg_expr(g, n->as.call.args[1]);
        if (!w.v || !w.ty || w.ty->kind != TY_LIST) {
            free(a.v); free(w.v);
            ERRV(g, "line %zu: sim_portfolio() needs a list of weights", n->line);
        }
        char *t = emit_instrf(g, type_list(type_prim(TY_FLOAT)),
                              "call i8* @lume_bi_sim_portfolio(i8* %s, i8* %s)",
                              a.v, w.v);
        free(a.v); free(w.v);
        return val_make(type_list(type_prim(TY_FLOAT)), t);
    }

    /* rand_dirichlet(alphas) (2026-10-30): a fresh normalized probability
     * vector from iid Gamma(alpha_i, 1) samples; one list argument. */
    if (strcmp(bname, "rand_dirichlet") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: rand_dirichlet() takes one list", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST) {
            free(a.v);
            ERRV(g, "line %zu: rand_dirichlet() needs a list", n->line);
        }
        char *t = emit_instrf(g, type_list(type_prim(TY_FLOAT)),
                              "call i8* @lume_bi_rand_dirichlet(i8* %s)", a.v);
        free(a.v);
        return val_make(type_list(type_prim(TY_FLOAT)), t);
    }

    /* Multivariate / copula samplers (2026-10-30): all-scalar inputs
     * answering a fresh float list. rand_binormal(mu1, mu2, sd1, sd2, rho,
     * n) takes six arguments, rand_gauss_copula(rho, n) two and
     * rand_t_copula(rho, df, n) three. The last argument is the sample
     * count (float truncates through lume_bi_ftoi), every other argument a
     * double (int widens through lume_bi_float). */
    if (strcmp(bname, "rand_binormal") == 0 || strcmp(bname, "rand_gauss_copula") == 0 ||
        strcmp(bname, "rand_t_copula") == 0) {
        int want = (strcmp(bname, "rand_binormal") == 0) ? 6 :
                   (strcmp(bname, "rand_gauss_copula") == 0 ? 2 : 3);
        if (argc != want)
            ERRV(g, "line %zu: %s() takes %d arguments", n->line, bname, want);
        Val *vs = (Val *)calloc((size_t)argc, sizeof(Val));
        for (int i = 0; i < argc; i++) {
            vs[i] = cg_expr(g, n->as.call.args[i]);
            if (!vs[i].v || !vs[i].ty ||
                (vs[i].ty->kind != TY_INT && vs[i].ty->kind != TY_FLOAT)) {
                for (int j = 0; j <= i; j++) free(vs[j].v);
                free(vs);
                ERRV(g, "line %zu: %s() needs numbers", n->line, bname);
            }
        }
        char *nv = vs[argc - 1].v;
        if (vs[argc - 1].ty->kind == TY_FLOAT) {
            char *w = emit_instrf(g, type_prim(TY_INT),
                                  "call i64 @lume_bi_ftoi(double %s)", nv);
            free(nv);
            nv = w;
        }
        const char *fn;
        if (strcmp(bname, "rand_binormal") == 0) fn = "lume_bi_rand_binormal";
        else if (strcmp(bname, "rand_gauss_copula") == 0) fn = "lume_bi_rand_gauss_copula";
        else fn = "lume_bi_rand_t_copula";
        char *buf = NULL;
        size_t bl = 0;
        for (int i = 0; i < argc - 1; i++) {
            char *a = vs[i].v;
            if (vs[i].ty->kind == TY_INT) {
                char *w = emit_instrf(g, type_prim(TY_FLOAT),
                                      "call double @lume_bi_float(i64 %s)", a);
                free(a);
                a = w;
            }
            if (i == 0) {
                size_t need = snprintf(NULL, 0, "call i8* @%s(double %s", fn, a) + 1;
                buf = (char *)malloc(need);
                bl = (size_t)snprintf(buf, need, "call i8* @%s(double %s", fn, a);
            } else {
                size_t need = bl + snprintf(NULL, 0, ", double %s", a) + 1;
                buf = (char *)realloc(buf, need);
                bl += (size_t)snprintf(buf + bl, need - bl, ", double %s", a);
            }
            free(a);
        }
        size_t need = bl + snprintf(NULL, 0, ", i64 %s)", nv) + 1;
        buf = (char *)realloc(buf, need);
        snprintf(buf + bl, need - bl, ", i64 %s)", nv);
        free(nv);
        free(vs);
        char *t = emit_instrf(g, type_list(type_prim(TY_FLOAT)), "%s", buf);
        free(buf);
        return val_make(type_list(type_prim(TY_FLOAT)), t);
    }

    /* Bootstrap family (2026-10-30): list + scalars. The three CI builtins
     * take (list, n_iter, alpha) — the list passes as i8*, the scalars widen
     * through lume_bi_float — and answer a fresh two-element float list;
     * bootstrap_se_mean takes (list, n_iter) and answers a double. */
    if (strcmp(bname, "bootstrap_mean_ci") == 0 ||
        strcmp(bname, "bootstrap_median_ci") == 0 ||
        strcmp(bname, "bootstrap_stddev_ci") == 0) {
        if (argc != 3)
            ERRV(g, "line %zu: %s() takes (list, n_iter, alpha)", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST) {
            free(a.v);
            ERRV(g, "line %zu: %s() needs a list", n->line, bname);
        }
        const char *fn = strcmp(bname, "bootstrap_mean_ci") == 0
                             ? "lume_bi_bootstrap_mean_ci"
                         : strcmp(bname, "bootstrap_median_ci") == 0
                             ? "lume_bi_bootstrap_median_ci"
                             : "lume_bi_bootstrap_stddev_ci";
        Val scal[2];
        for (int i = 0; i < 2; i++) {
            scal[i] = cg_expr(g, n->as.call.args[i + 1]);
            if (!scal[i].v || !scal[i].ty ||
                (scal[i].ty->kind != TY_INT && scal[i].ty->kind != TY_FLOAT)) {
                free(a.v); free(scal[0].v);
                if (i == 1) free(scal[1].v);
                ERRV(g, "line %zu: %s() needs numbers for n_iter/alpha", n->line, bname);
            }
            if (scal[i].ty->kind == TY_INT) {
                char *w = emit_instrf(g, type_prim(TY_FLOAT),
                                      "call double @lume_bi_float(i64 %s)", scal[i].v);
                free(scal[i].v);
                scal[i].v = w;
            }
        }
        char *t = emit_instrf(g, type_list(type_prim(TY_FLOAT)),
                              "call i8* @%s(i8* %s, double %s, double %s)",
                              fn, a.v, scal[0].v, scal[1].v);
        free(a.v); free(scal[0].v); free(scal[1].v);
        return val_make(type_list(type_prim(TY_FLOAT)), t);
    }

    if (strcmp(bname, "bootstrap_se_mean") == 0 || strcmp(bname, "value_at_risk") == 0 ||
        strcmp(bname, "expected_shortfall") == 0 || strcmp(bname, "sharpe_ratio") == 0 ||
        strcmp(bname, "sortino_ratio") == 0 || strcmp(bname, "sharpe_annualized") == 0 ||
        strcmp(bname, "downside_deviation") == 0 || strcmp(bname, "param_var") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: %s() takes (list, number)", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST) {
            free(a.v);
            ERRV(g, "line %zu: %s() needs a list", n->line, bname);
        }
        Val ni = cg_expr(g, n->as.call.args[1]);
        if (!ni.v || !ni.ty || (ni.ty->kind != TY_INT && ni.ty->kind != TY_FLOAT)) {
            free(a.v); free(ni.v);
            ERRV(g, "line %zu: %s() needs a number for its second argument", n->line, bname);
        }
        if (ni.ty->kind == TY_INT) {
            char *w = emit_instrf(g, type_prim(TY_FLOAT),
                                  "call double @lume_bi_float(i64 %s)", ni.v);
            free(ni.v);
            ni.v = w;
        }
        const char *fn = "lume_bi_bootstrap_se_mean";
        if (strcmp(bname, "value_at_risk") == 0) fn = "lume_bi_value_at_risk";
        else if (strcmp(bname, "expected_shortfall") == 0) fn = "lume_bi_expected_shortfall";
        else if (strcmp(bname, "sharpe_ratio") == 0) fn = "lume_bi_sharpe_ratio";
        else if (strcmp(bname, "sortino_ratio") == 0) fn = "lume_bi_sortino_ratio";
        else if (strcmp(bname, "sharpe_annualized") == 0) fn = "lume_bi_sharpe_annualized";
        else if (strcmp(bname, "downside_deviation") == 0) fn = "lume_bi_downside_deviation";
        else if (strcmp(bname, "param_var") == 0) fn = "lume_bi_param_var";
        char *t = emit_instrf(g, type_prim(TY_FLOAT),
                              "call double @%s(i8* %s, double %s)",
                              fn, a.v, ni.v);
        free(a.v); free(ni.v);
        return val_make(type_prim(TY_FLOAT), t);
    }

    /* Black-Scholes pricing family (2026-10-28): five doubles in, one
     * double out — prices, deltas, gamma and vega. */
    if (strcmp(bname, "bs_call") == 0 || strcmp(bname, "bs_put") == 0 ||
        strcmp(bname, "bs_delta_call") == 0 || strcmp(bname, "bs_delta_put") == 0 ||
        strcmp(bname, "bs_gamma") == 0 || strcmp(bname, "bs_vega") == 0) {
        if (argc != 5)
            ERRV(g, "line %zu: %s() takes (S, K, r, sigma, T)", n->line, bname);
        Val a0 = cg_expr(g, n->as.call.args[0]);
        Val a1 = cg_expr(g, n->as.call.args[1]);
        Val a2 = cg_expr(g, n->as.call.args[2]);
        Val a3 = cg_expr(g, n->as.call.args[3]);
        Val a4 = cg_expr(g, n->as.call.args[4]);
        if (!a0.v || !a0.ty || (a0.ty->kind != TY_INT && a0.ty->kind != TY_FLOAT) ||
            !a1.v || !a1.ty || (a1.ty->kind != TY_INT && a1.ty->kind != TY_FLOAT) ||
            !a2.v || !a2.ty || (a2.ty->kind != TY_INT && a2.ty->kind != TY_FLOAT) ||
            !a3.v || !a3.ty || (a3.ty->kind != TY_INT && a3.ty->kind != TY_FLOAT) ||
            !a4.v || !a4.ty || (a4.ty->kind != TY_INT && a4.ty->kind != TY_FLOAT)) {
            free(a0.v); free(a1.v); free(a2.v); free(a3.v); free(a4.v);
            ERRV(g, "line %zu: %s() needs five numbers", n->line, bname);
        }
        Val vv[5] = { a0, a1, a2, a3, a4 };
        for (int i = 0; i < 5; i++) {
            if (vv[i].ty->kind == TY_INT) {
                char *w = emit_instrf(g, type_prim(TY_FLOAT),
                                      "call double @lume_bi_float(i64 %s)", vv[i].v);
                free(vv[i].v);
                vv[i].v = w;
            }
        }
        const char *fn;
        if (strcmp(bname, "bs_call") == 0) fn = "lume_bi_bs_call";
        else if (strcmp(bname, "bs_put") == 0) fn = "lume_bi_bs_put";
        else if (strcmp(bname, "bs_delta_call") == 0) fn = "lume_bi_bs_delta_call";
        else if (strcmp(bname, "bs_delta_put") == 0) fn = "lume_bi_bs_delta_put";
        else if (strcmp(bname, "bs_gamma") == 0) fn = "lume_bi_bs_gamma";
        else fn = "lume_bi_bs_vega";
        char *t = emit_instrf(g, type_prim(TY_FLOAT),
                              "call double @%s(double %s, double %s, double %s, double %s, double %s)",
                              fn, vv[0].v, vv[1].v, vv[2].v, vv[3].v, vv[4].v);
        for (int i = 0; i < 5; i++) free(vv[i].v);
        return val_make(type_prim(TY_FLOAT), t);
    }

    /* File builtins (SPEC §6.2): read_file returns the file as a string or
     * null; write_file/lock_file are booleans; unlock_file releases the
     * process-wide lock. String parameters keep these out of the numeric
     * table path below, so they get their own branches — the same shape the
     * interpreter's native_read_file/native_lock_file have (builtins_fs.c). */
    if (strcmp(bname, "read_file") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: read_file() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v) return (Val){ NULL, NULL, 0 };
        Val r = val_take(type_prim(TY_STRING),
                         emit_instrf(g, type_prim(TY_STRING),
                                     "call i8* @lume_bi_read_file(i8* %s)",
                                     a.v));
        free(a.v);
        return r;
    }
    if (strcmp(bname, "write_file") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: write_file() takes exactly two arguments", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !b.v) { free(a.v); free(b.v); return (Val){ NULL, NULL, 0 }; }
        Val r = val_take(type_prim(TY_INT),
                         emit_instrf(g, type_prim(TY_INT),
                                     "call i64 @lume_bi_write_file(i8* %s, i8* %s)",
                                     a.v, b.v));
        free(a.v); free(b.v);
        return r;
    }
    if (strcmp(bname, "lock_file") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: lock_file() takes a path and a wait (ms)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val m = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !m.v) { free(a.v); free(m.v); return (Val){ NULL, NULL, 0 }; }
        Val r = val_take(type_prim(TY_INT),
                         emit_instrf(g, type_prim(TY_INT),
                                     "call i64 @lume_bi_lock_file(i8* %s, i64 %s)",
                                     a.v, m.v));
        free(a.v); free(m.v);
        return r;
    }
    if (strcmp(bname, "unlock_file") == 0) {
        if (argc != 0)
            ERRV(g, "line %zu: unlock_file() takes no arguments", n->line);
        return val_take(type_prim(TY_INT),
                        emit_instrf(g, type_prim(TY_INT),
                                    "call i64 @lume_bi_unlock_file()"));
    }

#if LUME_HAS_SQLITE
    /* SQLite (SPEC 6.4, P6 three-backend parity): sql_query / sql_write /
     * sql_tx_* lower onto the native runtime in bridge_native.c, sharing
     * the interpreter's multi-shape argument rules: sql_query(sql[, params])
     * or sql_query(path, sql[, params]). The two-argument form is
     * disambiguated by arg 1's static type — string means path, a list
     * means params. tx_begin returns an opaque i8* handle (heap LumeSqlTx)
     * typed as a string so if (tx) null-checks work like read_file. */
    if (strcmp(bname, "sql_query") == 0 || strcmp(bname, "sql_write") == 0) {
        int isq = strcmp(bname, "sql_query") == 0;
        const char *fn = isq ? "lume_bi_sql_query" : "lume_bi_sql_write";
        if (argc < 1 || argc > 3)
            ERRV(g, "line %zu: %s() takes 1 to 3 arguments", n->line, bname);
        Val *av = (Val *)xmalloc((size_t)argc * sizeof *av);
        for (int i = 0; i < argc; i++) av[i] = cg_expr(g, n->as.call.args[i]);
        int bad = 0;
        for (int i = 0; i < argc; i++) if (!av[i].v) bad = 1;
        if (bad) {
            for (int i = 0; i < argc; i++) free(av[i].v);
            free(av);
            return (Val){ NULL, NULL, 0 };
        }
        const char *db = "null", *sql, *params = "null";
        if (argc == 1) {
            sql = av[0].v;
        } else if (argc == 2) {
            if (av[1].ty && av[1].ty->kind == TY_STRING) {
                db = av[0].v;
                sql = av[1].v;
            } else {
                sql = av[0].v;
                params = av[1].v;
            }
        } else {
            db = av[0].v;
            sql = av[1].v;
            params = av[2].v;
        }
        Type *rt = isq ? list_type() : type_prim(TY_INT);
        Val r = val_take(rt,
                         emit_instrf(g, rt,
                                     "call %s @%s(i8* %s, i8* %s, i8* %s)",
                                     isq ? "i8*" : "i64", fn,
                                     db, sql, params));
        for (int i = 0; i < argc; i++) free(av[i].v);
        free(av);
        return r;
    }
    if (strcmp(bname, "sql_tx_begin") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: sql_tx_begin() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v) return (Val){ NULL, NULL, 0 };
        Val r = val_take(type_prim(TY_STRING),
                         emit_instrf(g, type_prim(TY_STRING),
                                     "call i8* @lume_bi_sql_tx_begin(i8* %s)",
                                     a.v));
        free(a.v);
        return r;
    }
    if (strcmp(bname, "sql_tx_exec") == 0) {
        if (argc < 2 || argc > 3)
            ERRV(g, "line %zu: sql_tx_exec() takes a handle, SQL and optional params",
                 n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        Val c = argc == 3 ? cg_expr(g, n->as.call.args[2])
                          : (Val){ NULL, NULL, 0 };
        if (!a.v || !b.v || (argc == 3 && !c.v)) {
            free(a.v); free(b.v); free(c.v);
            return (Val){ NULL, NULL, 0 };
        }
        Val r = val_take(type_prim(TY_INT),
                         emit_instrf(g, type_prim(TY_INT),
                                     "call i64 @lume_bi_sql_tx_exec(i8* %s, i8* %s, i8* %s)",
                                     a.v, b.v, argc == 3 ? c.v : "null"));
        free(a.v); free(b.v); free(c.v);
        return r;
    }
    if (strcmp(bname, "sql_tx_query") == 0) {
        if (argc < 2 || argc > 3)
            ERRV(g, "line %zu: sql_tx_query() takes a handle, SQL and optional params",
                 n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        Val c = argc == 3 ? cg_expr(g, n->as.call.args[2])
                          : (Val){ NULL, NULL, 0 };
        if (!a.v || !b.v || (argc == 3 && !c.v)) {
            free(a.v); free(b.v); free(c.v);
            return (Val){ NULL, NULL, 0 };
        }
        Type *rt = list_type();
        Val r = val_take(rt,
                         emit_instrf(g, rt,
                                     "call i8* @lume_bi_sql_tx_query(i8* %s, i8* %s, i8* %s)",
                                     a.v, b.v, argc == 3 ? c.v : "null"));
        free(a.v); free(b.v); free(c.v);
        return r;
    }
    if (strcmp(bname, "sql_tx_commit") == 0 || strcmp(bname, "sql_tx_rollback") == 0) {
        int isc = strcmp(bname, "sql_tx_commit") == 0;
        if (argc != 1)
            ERRV(g, "line %zu: %s() takes exactly one argument", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v) return (Val){ NULL, NULL, 0 };
        /* The interpreter's sql_tx_commit/rollback return bool (print shows
         * "true"/"false"), so the emitted Val must be typed TY_BOOL and its
         * IR value must be an i1 — print()'s bool arm widens i1 to i64 for
         * lume_print_bool, and every other bool consumer assumes i1. The
         * runtime helper returns i64, so truncate after the call. */
        char *cv = emit_instrf(g, type_prim(TY_INT),
                               "call i64 @lume_bi_sql_tx_%s(i8* %s)",
                               isc ? "commit" : "rollback", a.v);
        if (!cv) { free(a.v); return (Val){ NULL, NULL, 0 }; }
        char *bv = emit_instrf(g, type_prim(TY_BOOL),
                               "trunc i64 %s to i1", cv);
        free(a.v); free(cv);
        if (!bv) return (Val){ NULL, NULL, 0 };
        return val_take(type_prim(TY_BOOL), bv);
    }
#endif /* LUME_HAS_SQLITE */

    if (b) {
        if (argc < 1)
            ERRV(g, "line %zu: '%s' takes at least one argument", n->line, bname);
        Val *av = (Val *)xmalloc((size_t)argc * sizeof *av);
        for (int i = 0; i < argc; i++) av[i] = cg_expr(g, n->as.call.args[i]);

        int isf = av[0].ty && b->fn_f &&
                  (av[0].ty->kind == TY_FLOAT ||
                   (b->fn_i == NULL && av[0].ty->kind == TY_INT));
        /* A float-only builtin (sqrt/exp/floor/...) has no int flavour, and
         * the interpreter's arg_num accepts either kind (native_sqrt tests
         * IS_NUM), so an int operand takes the float one — refusing
         * `sqrt(4)` while the interpreter answers 2 was a three-way split.
         * Only INT promotes: a string still falls through to the
         * "does not apply" error below rather than reaching coerce(). */
        const char *fn  = isf ? b->fn_f  : b->fn_i;
        const char *pty = isf ? b->pty_f : b->pty_i;
        const char *rty = isf ? b->rty_f : b->rty_i;
        if (!fn || !pty || !rty)
            ERRV(g, "line %zu: '%s' does not apply to this operand type", n->line, bname);

        /* The helper's parameter type is not always the operand's: `min` has an
         * int and a float flavour and the operand decides which one is used. */
        Type *want = isf ? type_prim(TY_FLOAT) : type_prim(TY_INT);
        for (int i = 0; i < argc; i++)
            if (av[i].ty && av[i].ty->kind != want->kind)
                av[i] = coerce(g, want, av[i], n->line);
        if (g->err[0]) return (Val){ NULL, NULL, 0 };

        char tmp[48];
        snprintf(tmp, sizeof tmp, "%%c%d", g->tid++);
        /* Every argument carries its type, constants included — IR will not
         * accept a bare `8` in a call, same as for a user function. */
        EMIT(g, "  %s = call %s @%s(", tmp, rty, fn);
        for (int i = 0; i < argc; i++)
            EMIT(g, "%s%s %s", i ? ", " : "", pty, av[i].v);
        EMIT(g, ")\n");

        for (int i = 0; i < argc; i++) free(av[i].v);
        free(av);
        return val_make(want, tmp);
    }

    /* map(fn, list) / filter(fn, list) / reduce(fn, list, init) — SPEC 4.8's
     * higher-order builtins, looped inline. The lambda's parameters were
     * pinned by the shared signature pass (param[0] from the list element,
     * reduce's param[0] from the init), so the loop reads elements through
     * the same accessor table for-in uses and calls the closure with the
     * exact spelling its define was emitted against. */

    if (strcmp(bname, "map") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: map() takes exactly two arguments", n->line);
        Node *lambda = hof_lambda(g, n->as.call.args[0]);
        if (!lambda)
            ERRV(g, "line %zu: map() needs a lambda as its first argument",
                 n->line);
        Type *ret = lambda->as.funclit.ret;
        const char *pushfn = list_push_fn(ret);
        if (!ret || !pushfn)
            ERRV(g, "line %zu: map() needs a lambda that returns a value "
                    "this backend can put in a list", n->line);

        Val fn = cg_expr(g, n->as.call.args[0]);
        Val src = cg_expr(g, n->as.call.args[1]);
        Type *et = lambda->as.funclit.param_types
            ? lambda->as.funclit.param_types[0] : NULL;
        const char *atfn = list_at_fn(et);
        if (!fn.v || !src.v || !src.ty || src.ty->kind != TY_LIST) {
            free(fn.v); free(src.v);
            ERRV(g, "line %zu: map() needs a list as its second argument",
                 n->line);
        }
        if (!atfn) {
            free(fn.v); free(src.v);
            ERRV(g, "line %zu: a list of this element type cannot be mapped",
                 n->line);
        }

        Val dst = rt_call(g, list_type(), "lume_list_new", "");
        if (!dst.v) { free(fn.v); free(src.v); ERRV(g, "line %zu: cannot allocate a list", n->line); }

        char idx[64], lenalloc[64];
        snprintf(idx, sizeof idx, "%%li%d", g->tid++);
        snprintf(lenalloc, sizeof lenalloc, "%%li%d", g->tid++);
        EMIT(g, "  %s = alloca i64\n", idx);
        EMIT(g, "  store i64 0, i64* %s\n", idx);
        EMIT(g, "  %s = alloca i64\n", lenalloc);
        Val len0 = rt_call(g, type_prim(TY_INT), "lume_list_len", "i8* %s", src.v);
        if (!len0.v) {
            free(fn.v); free(src.v); free(dst.v);
            ERRV(g, "line %zu: cannot read the list's length", n->line);
        }
        EMIT(g, "  store i64 %s, i64* %s\n", len0.v, lenalloc);
        free(len0.v);

        int lc = g->lid++, lb = g->lid++, li = g->lid++, le = g->lid++;
        EMIT(g, "  br label %%L%d\n", lc);
        EMIT(g, "L%d:\n", lc);
        char *len = emit_instrf(g, type_prim(TY_INT), "load i64, i64* %s", lenalloc);
        char *cur = emit_instrf(g, type_prim(TY_INT), "load i64, i64* %s", idx);
        char *cond = len && cur
            ? emit_instrf(g, type_prim(TY_BOOL), "icmp slt i64 %s, %s", cur, len)
            : NULL;
        free(len);
        if (!cond) { free(cur); free(fn.v); free(src.v); free(dst.v);
                     ERRV(g, "line %zu: cannot spell the map loop", n->line); }
        EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n", cond, lb, le);
        free(cond);

        EMIT(g, "L%d:\n", lb);
        Val e = rt_call(g, elem_rty(et), atfn, "i8* %s, i64 %s", src.v, cur);
        if (!e.v) { free(cur); free(fn.v); free(src.v); free(dst.v);
                    ERRV(g, "line %zu: cannot read an element", n->line); }
        Type *p0 = lambda->as.funclit.param_types
            ? lambda->as.funclit.param_types[0] : NULL;
        if (p0) e = coerce(g, p0, e, n->line);
        if (!e.v) { free(cur); free(fn.v); free(src.v); free(dst.v);
                    ERRV(g, "line %zu: cannot coerce the map argument", n->line); }
        Val argv[1] = { e };
        char *rv = cg_clo_invoke(g, lambda, fn.v, argv, 1);
        free(e.v);
        if (!rv) { free(cur); free(fn.v); free(src.v); free(dst.v);
                   ERRV(g, "line %zu: cannot call the mapping closure", n->line); }
        Val item = val_make(ret, rv);
        free(rv);
        if (item.ty && item.ty->kind == TY_BOOL)
            item = coerce(g, type_prim(TY_INT), item, n->line);
        Val pr = rt_call(g, type_prim(TY_INT), pushfn, "i8* %s, %s %s",
                         dst.v, rt_arg_type(item.ty), item.v);
        free(item.v);
        if (!pr.v) { free(cur); free(fn.v); free(src.v); free(dst.v);
                     ERRV(g, "line %zu: cannot append to a list", n->line); }
        free(pr.v);

        EMIT(g, "  br label %%L%d\n", li);
        EMIT(g, "L%d:\n", li);
        char *nxt = emit_instrf(g, type_prim(TY_INT), "add i64 %s, 1", cur);
        free(cur);
        if (nxt) {
            EMIT(g, "  store i64 %s, i64* %s\n", nxt, idx);
            free(nxt);
        }
        EMIT(g, "  br label %%L%d\n", lc);
        EMIT(g, "L%d:\n", le);

        free(fn.v); free(src.v);
        { Val r = dst; r.ty = type_list(ret); return r; }
    }

    if (strcmp(bname, "filter") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: filter() takes exactly two arguments", n->line);
        Node *lambda = hof_lambda(g, n->as.call.args[0]);
        if (!lambda)
            ERRV(g, "line %zu: filter() needs a lambda as its first argument",
                 n->line);
        Type *ret = lambda->as.funclit.ret;
        if (!ret || (ret->kind != TY_BOOL && ret->kind != TY_INT &&
                     ret->kind != TY_FLOAT))
            ERRV(g, "line %zu: filter() needs a lambda that returns a bool "
                    "(the native backend tests the truth of a bool, an int "
                    "or a float)", n->line);
        Type *et = lambda->as.funclit.param_types
            ? lambda->as.funclit.param_types[0] : NULL;
        const char *atfn = list_at_fn(et);
        if (!atfn)
            ERRV(g, "line %zu: a list of this element type cannot be filtered",
                 n->line);

        Val fn = cg_expr(g, n->as.call.args[0]);
        Val src = cg_expr(g, n->as.call.args[1]);
        if (!fn.v || !src.v || !src.ty || src.ty->kind != TY_LIST) {
            free(fn.v); free(src.v);
            ERRV(g, "line %zu: filter() needs a list as its second argument",
                 n->line);
        }

        Val dst = rt_call(g, list_type(), "lume_list_new", "");
        if (!dst.v) { free(fn.v); free(src.v); ERRV(g, "line %zu: cannot allocate a list", n->line); }

        char idx[64], lenalloc[64];
        snprintf(idx, sizeof idx, "%%li%d", g->tid++);
        snprintf(lenalloc, sizeof lenalloc, "%%li%d", g->tid++);
        EMIT(g, "  %s = alloca i64\n", idx);
        EMIT(g, "  store i64 0, i64* %s\n", idx);
        EMIT(g, "  %s = alloca i64\n", lenalloc);
        Val len0 = rt_call(g, type_prim(TY_INT), "lume_list_len", "i8* %s", src.v);
        if (!len0.v) {
            free(fn.v); free(src.v); free(dst.v);
            ERRV(g, "line %zu: cannot read the list's length", n->line);
        }
        EMIT(g, "  store i64 %s, i64* %s\n", len0.v, lenalloc);
        free(len0.v);

        int lc = g->lid++, lb = g->lid++, li = g->lid++, le = g->lid++;
        EMIT(g, "  br label %%L%d\n", lc);
        EMIT(g, "L%d:\n", lc);
        char *len = emit_instrf(g, type_prim(TY_INT), "load i64, i64* %s", lenalloc);
        char *cur = emit_instrf(g, type_prim(TY_INT), "load i64, i64* %s", idx);
        char *cond = len && cur
            ? emit_instrf(g, type_prim(TY_BOOL), "icmp slt i64 %s, %s", cur, len)
            : NULL;
        free(len);
        if (!cond) { free(cur); free(fn.v); free(src.v); free(dst.v);
                     ERRV(g, "line %zu: cannot spell the filter loop", n->line); }
        EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n", cond, lb, le);
        free(cond);

        EMIT(g, "L%d:\n", lb);
        Val e = rt_call(g, elem_rty(et), atfn, "i8* %s, i64 %s", src.v, cur);
        if (!e.v) { free(cur); free(fn.v); free(src.v); free(dst.v);
                    ERRV(g, "line %zu: cannot read an element", n->line); }
        /* The element travels twice: once into the closure (narrowed to the
         * parameter's spelling) and once — the raw runtime value as read —
         * into the result list when the test passes. The closure argument is
         * a *copy* of the register name precisely because coerce() consumes
         * the string it converts: the raw spelling has to survive for the
         * push below. */
        Val arg = { xstrdup(e.v), e.ty, 0 };
        Type *p0 = lambda->as.funclit.param_types
            ? lambda->as.funclit.param_types[0] : NULL;
        if (p0) arg = coerce(g, p0, arg, n->line);
        if (!arg.v) {
            free(e.v); free(cur); free(fn.v); free(src.v); free(dst.v);
            ERRV(g, "line %zu: cannot coerce the filter argument", n->line);
        }
        Val argv[1] = { arg };
        char *rv = cg_clo_invoke(g, lambda, fn.v, argv, 1);
        free(arg.v);
        if (!rv) { free(e.v); free(cur); free(fn.v); free(src.v); free(dst.v);
                   ERRV(g, "line %zu: cannot call the filter closure", n->line); }

        /* truthy test on the lambda's return, spelled by its kind. For a
         * bool return the register IS the test, so `test` takes ownership
         * of rv and the free below must not run — aliasing it and freeing
         * it was a use-after-free ASan caught on the first filter(). */
        char *test = NULL;
        if (ret->kind == TY_BOOL)
            test = rv;                       /* already i1; takes ownership */
        else if (ret->kind == TY_FLOAT)
            test = emit_instrf(g, type_prim(TY_BOOL),
                               "fcmp une double %s, 0.0", rv);
        else
            test = emit_instrf(g, type_prim(TY_BOOL),
                               "icmp ne i64 %s, 0", rv);
        if (ret->kind != TY_BOOL) free(rv);
        if (!test) { free(e.v); free(cur); free(fn.v); free(src.v); free(dst.v);
                     ERRV(g, "line %zu: cannot test the filter result", n->line); }

        int lpush = g->lid++, lnext = g->lid++;
        EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n", test, lpush, lnext);
        free(test);
        EMIT(g, "L%d:\n", lpush);
        Val pr = rt_call(g, type_prim(TY_INT), list_push_fn(et),
                         "i8* %s, %s %s", dst.v, rt_arg_type(e.ty), e.v);
        free(e.v);
        if (!pr.v) { free(cur); free(fn.v); free(src.v); free(dst.v);
                     ERRV(g, "line %zu: cannot append to a list", n->line); }
        free(pr.v);
        EMIT(g, "  br label %%L%d\n", lnext);
        EMIT(g, "L%d:\n", lnext);

        EMIT(g, "  br label %%L%d\n", li);
        EMIT(g, "L%d:\n", li);
        char *nxt = emit_instrf(g, type_prim(TY_INT), "add i64 %s, 1", cur);
        free(cur);
        if (nxt) {
            EMIT(g, "  store i64 %s, i64* %s\n", nxt, idx);
            free(nxt);
        }
        EMIT(g, "  br label %%L%d\n", lc);
        EMIT(g, "L%d:\n", le);

        free(fn.v); free(src.v);
        { Val r = dst; r.ty = type_list(et); return r; }
    }

    if (strcmp(bname, "reduce") == 0) {
        if (argc != 3)
            ERRV(g, "line %zu: reduce() takes exactly three arguments", n->line);
        Node *lambda = hof_lambda(g, n->as.call.args[0]);
        if (!lambda)
            ERRV(g, "line %zu: reduce() needs a lambda as its first argument",
                 n->line);
        if (lambda->as.funclit.arity != 2)
            ERRV(g, "line %zu: reduce()'s lambda takes (accumulator, item)",
                 n->line);
        Type *et = lambda->as.funclit.param_types && lambda->as.funclit.arity > 1
            ? lambda->as.funclit.param_types[1] : NULL;
        const char *atfn = list_at_fn(et);
        if (!atfn)
            ERRV(g, "line %zu: a list of this element type cannot be reduced",
                 n->line);

        Val fn = cg_expr(g, n->as.call.args[0]);
        Val src = cg_expr(g, n->as.call.args[1]);
        Val acc0 = cg_expr(g, n->as.call.args[2]);
        if (!fn.v || !src.v || !src.ty || src.ty->kind != TY_LIST || !acc0.v ||
            !acc0.ty || acc0.ty->kind == TY_STRUCT) {
            free(fn.v); free(src.v); free(acc0.v);
            ERRV(g, "line %zu: reduce() needs a list and a scalar or list "
                    "accumulator", n->line);
        }
        const char *at_acc = llvm_type_of(acc0.ty);
        if (!at_acc) { free(fn.v); free(src.v); free(acc0.v);
                       ERRV(g, "line %zu: the accumulator has no codegen type",
                            n->line); }

        char accslot[64];
        snprintf(accslot, sizeof accslot, "%%la%d", g->tid++);
        EMIT(g, "  %s = alloca %s\n", accslot, at_acc);
        Val acc_init = acc0.ty && lambda->as.funclit.param_types &&
                       lambda->as.funclit.param_types[0]
            ? coerce(g, lambda->as.funclit.param_types[0], acc0, n->line)
            : acc0;
        if (!acc_init.v) { free(fn.v); free(src.v);
                           ERRV(g, "line %zu: cannot seed the accumulator",
                                n->line); }
        EMIT(g, "  store %s %s, %s* %s\n", at_acc, acc_init.v, at_acc, accslot);
        free(acc_init.v);

        char idx[64], lenalloc[64];
        snprintf(idx, sizeof idx, "%%li%d", g->tid++);
        snprintf(lenalloc, sizeof lenalloc, "%%li%d", g->tid++);
        EMIT(g, "  %s = alloca i64\n", idx);
        EMIT(g, "  store i64 0, i64* %s\n", idx);
        EMIT(g, "  %s = alloca i64\n", lenalloc);
        Val len0 = rt_call(g, type_prim(TY_INT), "lume_list_len", "i8* %s", src.v);
        if (!len0.v) { free(fn.v); free(src.v);
                       ERRV(g, "line %zu: cannot read the list's length", n->line); }
        EMIT(g, "  store i64 %s, i64* %s\n", len0.v, lenalloc);
        free(len0.v);

        int lc = g->lid++, lb = g->lid++, li = g->lid++, le = g->lid++;
        EMIT(g, "  br label %%L%d\n", lc);
        EMIT(g, "L%d:\n", lc);
        char *len = emit_instrf(g, type_prim(TY_INT), "load i64, i64* %s", lenalloc);
        char *cur = emit_instrf(g, type_prim(TY_INT), "load i64, i64* %s", idx);
        char *cond = len && cur
            ? emit_instrf(g, type_prim(TY_BOOL), "icmp slt i64 %s, %s", cur, len)
            : NULL;
        free(len);
        if (!cond) { free(cur); free(fn.v); free(src.v);
                     ERRV(g, "line %zu: cannot spell the reduce loop", n->line); }
        EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n", cond, lb, le);
        free(cond);

        EMIT(g, "L%d:\n", lb);
        Val e = rt_call(g, elem_rty(et), atfn, "i8* %s, i64 %s", src.v, cur);
        char *accv = emit_instrf(g, acc0.ty, "load %s, %s* %s", at_acc, at_acc,
                                 accslot);
        if (!e.v || !accv) {
            free(e.v); free(accv); free(cur); free(fn.v); free(src.v);
            ERRV(g, "line %zu: cannot read an element or the accumulator",
                 n->line);
        }
        Val argv[2] = { { accv, acc0.ty, 0 }, e };
        Type *p0 = lambda->as.funclit.param_types
            ? lambda->as.funclit.param_types[0] : NULL;
        Type *p1 = lambda->as.funclit.param_types &&
                   lambda->as.funclit.arity > 1
            ? lambda->as.funclit.param_types[1] : NULL;
        if (p0) argv[0] = coerce(g, p0, argv[0], n->line);
        if (p1) argv[1] = coerce(g, p1, argv[1], n->line);
        if (!argv[0].v || !argv[1].v) {
            free(argv[0].v); free(argv[1].v); free(cur); free(fn.v); free(src.v);
            ERRV(g, "line %zu: cannot coerce the reduce arguments", n->line);
        }
        char *rv = cg_clo_invoke(g, lambda, fn.v, argv, 2);
        free(argv[0].v); free(argv[1].v);
        if (!rv) { free(cur); free(fn.v); free(src.v);
                   ERRV(g, "line %zu: cannot call the reducing closure", n->line); }
        Val acc_next = val_make(acc0.ty, rv);
        free(rv);
        acc_next = coerce(g, acc0.ty, acc_next, n->line);
        if (!acc_next.v) { free(cur); free(fn.v); free(src.v);
                           ERRV(g, "line %zu: the reducing closure's result "
                                "does not fit the accumulator", n->line); }
        EMIT(g, "  store %s %s, %s* %s\n", at_acc, acc_next.v, at_acc, accslot);
        free(acc_next.v);

        EMIT(g, "  br label %%L%d\n", li);
        EMIT(g, "L%d:\n", li);
        char *nxt = emit_instrf(g, type_prim(TY_INT), "add i64 %s, 1", cur);
        free(cur);
        if (nxt) {
            EMIT(g, "  store i64 %s, i64* %s\n", nxt, idx);
            free(nxt);
        }
        EMIT(g, "  br label %%L%d\n", lc);
        EMIT(g, "L%d:\n", le);

        free(fn.v); free(src.v);
        char *res = emit_instrf(g, acc0.ty, "load %s, %s* %s", at_acc, at_acc,
                                accslot);
        if (!res) ERRV(g, "line %zu: cannot read the accumulator", n->line);
        return val_take(acc0.ty, res);
    }

    /* try(fn) — SPEC 6.3: call fn with no arguments and wrap the outcome in
     * { ok, err }, exactly as the interpreter prints. Inside try the runtime
     * records the first recoverable error (a failed index read, SPEC 6.2)
     * instead of exiting; ok then holds a null obj slot and err the message,
     * otherwise ok holds the result and err a null obj slot. The map_put
     * writer overwrites the tag, so the ok slot can be int/float/string on
     * success and obj-null on failure. */
    if (strcmp(bname, "try") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: try() expects (fn)", n->line);
        Node *lam = hof_lambda(g, n->as.call.args[0]);
        if (!lam)
            ERRV(g, "line %zu: try() needs a lambda or a closure variable "
                    "(a named function is not a value on the native backend)",
                n->line);
        Val fnv = cg_expr(g, n->as.call.args[0]);
        if (!fnv.v) ERRV(g, "line %zu: try() needs a function", n->line);

        EMIT(g, "  call void @lume_error_begin()\n");
        char *res = cg_clo_invoke(g, lam, fnv.v, NULL, 0);
        free(fnv.v);
        if (!res) ERRV(g, "line %zu: try() could not call the function", n->line);
        char *emsg = emit_instrf(g, type_prim(TY_STRING),
                                 "call i8* @lume_error_end()");
        if (!emsg) { free(res); ERRV(g, "line %zu: try() failed", n->line); }

        Val m = rt_call(g, type_anon_struct(), "lume_map_new", "");
        if (!m.v) { free(res); free(emsg);
            ERRV(g, "line %zu: try() failed to allocate", n->line); }
        char *kok = cg_string_val(g, "ok", 2);
        char *kerr = cg_string_val(g, "err", 3);

        char test[48];
        snprintf(test, sizeof test, "%%t%d", g->tid++);
        EMIT(g, "  %s = icmp ne i8* %s, null\n", test, emsg);
        int ler = g->lid++, lok = g->lid++, le = g->lid++;
        EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n", test, ler, lok);

        EMIT(g, "L%d:\n", ler);                 /* error fired */
        {
            /* ok becomes a null obj slot ("null"), err a string slot: the
             * json printer renders obj-null as null and a string as a quoted
             * message, exactly like the interpreter's { ok: null, err: msg }. */
            Val r = rt_call(g, type_prim(TY_INT), "lume_map_put_obj",
                            "i8* %s, i8* %s, i8* null", m.v, kok);
            free(r.v);
            r = rt_call(g, type_prim(TY_INT), "lume_map_put_s",
                        "i8* %s, i8* %s, i8* %s", m.v, kerr, emsg);
            free(r.v);
        }
        EMIT(g, "  br label %%L%d\n", le);

        EMIT(g, "L%d:\n", lok);                 /* no error */
        {
            Type *ret = lam->as.funclit.ret;
            if (ret && ret->kind != TY_NULL) {
                if (ret->kind == TY_ANY) {
                    /* any payload (e.g. try(() => json(...))): the slot
                     * helper dispatches by tag — scalar payloads land as
                     * int/float/string/bool, container payloads as obj
                     * pointers — so the map prints like the interpreter's
                     * { ok: <value>, err: ... }. */
                    Val r = rt_call(g, type_prim(TY_INT), "lume_map_put_slot",
                                    "i8* %s, i8* %s, i8* %s", m.v, kok, res);
                    free(r.v);
                } else {
                    const char *pfn = map_put_fn(ret);
                    if (!pfn) { free(res); free(emsg); free(kok); free(kerr);
                        ERRV(g, "line %zu: try() result has no codegen type", n->line); }
                    Val r = rt_call(g, type_prim(TY_INT), pfn, "i8* %s, i8* %s, %s %s",
                                    m.v, kok, rt_arg_type(ret), res);
                    free(r.v);
                }
            } else {
                Val r = rt_call(g, type_prim(TY_INT), "lume_map_put_obj",
                                "i8* %s, i8* %s, i8* null", m.v, kok);
                free(r.v);
            }
            Val r = rt_call(g, type_prim(TY_INT), "lume_map_put_obj",
                            "i8* %s, i8* %s, i8* null", m.v, kerr);
            free(r.v);
        }
        EMIT(g, "  br label %%L%d\n", le);
        EMIT(g, "L%d:\n", le);

        free(res); free(emsg); free(kok); free(kerr);
        return m;
    }

    /* len() — a list answers with its length, a string with its byte length.
     * The helper is picked from the static type, so a list and a string both
     * spell `len(x)` and still reach the right one. */
    if (strcmp(bname, "len") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: len() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty ||
            (a.ty->kind != TY_LIST && a.ty->kind != TY_STRING &&
             a.ty->kind != TY_ANY))
            ERRV(g, "line %zu: len() needs a list, a string or an any value", n->line);
        const char *lfn = a.ty->kind == TY_LIST ? "lume_list_len"
                        : a.ty->kind == TY_ANY  ? "lume_slot_len"
                                                : "lume_bi_len_s";
        Val r = rt_call(g, type_prim(TY_INT), lfn, "i8* %s", a.v);
        free(a.v);
        if (!r.v) ERRV(g, "line %zu: cannot take the length of this value", n->line);
        return r;
    }

    /* push(list, item) / put(map, key, value) — both mutate in place and hand
     * the container back, exactly as the interpreter does. */
    if (strcmp(bname, "push") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: push() takes exactly two arguments", n->line);
        Val l = cg_expr(g, n->as.call.args[0]);
        Val e = cg_expr(g, n->as.call.args[1]);
        const char *fn = list_push_fn(e.ty);
        if (!l.v || !e.v || !l.ty || l.ty->kind != TY_LIST || !fn)
            ERRV(g, "line %zu: push() needs a list and a scalar", n->line);
        if (e.ty->kind == TY_BOOL) e = coerce(g, type_prim(TY_INT), e, n->line);
        Val r = rt_call(g, type_prim(TY_INT), fn, "i8* %s, %s %s", l.v,
                        rt_arg_type(e.ty), e.v);
        free(e.v);
        if (!r.v) { free(l.v); ERRV(g, "line %zu: push() failed", n->line); }
        free(r.v);
        return l;                       /* the list itself, as the interpreter returns */
    }

    if (strcmp(bname, "put") == 0) {
        if (argc != 3)
            ERRV(g, "line %zu: put() takes exactly three arguments", n->line);
        Val m = cg_expr(g, n->as.call.args[0]);
        Val k = cg_expr(g, n->as.call.args[1]);
        Val v = cg_expr(g, n->as.call.args[2]);
        const char *fn = map_put_fn(v.ty);
        if (!m.v || !k.v || !v.v || !fn)
            ERRV(g, "line %zu: put() needs a map, a key and a scalar", n->line);
        if (v.ty->kind == TY_BOOL) v = coerce(g, type_prim(TY_INT), v, n->line);
        Val r = rt_call(g, type_prim(TY_INT), fn, "i8* %s, i8* %s, %s %s", m.v, k.v,
                        rt_arg_type(v.ty), v.v);
        free(k.v); free(v.v);
        if (!r.v) { free(m.v); ERRV(g, "line %zu: put() failed", n->line); }
        free(r.v);
        return m;
    }

    /* keys(m) — the interpreter hands back a list, so the helper builds one. */
    if (strcmp(bname, "keys") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: keys() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind == TY_LIST)
            ERRV(g, "line %zu: keys() needs a map", n->line);
        Val r = rt_call(g, list_type(), "lume_map_keys", "i8* %s", a.v);
        free(a.v);
        if (!r.v) ERRV(g, "line %zu: keys() failed", n->line);
        return r;
    }

    /* range(...) — the helper runs the same double arithmetic the
     * interpreter's native_range() does (whole values become INT slots), so
     * print/for-in agree compiled or interpreted. Numeric args only: a slot
     * holds a statically-typed number, and an `any` has no reader. */
    if (strcmp(bname, "range") == 0) {
        if (argc < 1 || argc > 3)
            ERRV(g, "line %zu: range() takes one to three arguments", n->line);
        Val a = argc >= 2 ? cg_expr(g, n->as.call.args[0])
                          : (Val){ NULL, NULL, 0 };
        Val b = cg_expr(g, n->as.call.args[argc == 1 ? 0 : 1]);
        Val s = argc == 3 ? cg_expr(g, n->as.call.args[2])
                          : (Val){ NULL, NULL, 0 };
        Val *xs[3] = { &a, &b, &s };
        for (int i = 0; i < 3; i++) {
            Val *x = xs[i];
            if (!x->v) continue;                    /* absent start/step    */
            if (!x->ty || (x->ty->kind != TY_INT && x->ty->kind != TY_FLOAT))
                ERRV(g, "line %zu: range() arguments must be numbers", n->line);
            *x = coerce(g, type_prim(TY_FLOAT), *x, n->line);
        }
        Val r = rt_call(g, list_type(), "lume_range",
                        "double %s, double %s, double %s",
                        a.v ? a.v : "0.000000e+00",
                        b.v, s.v ? s.v : "1.000000e+00");
        free(a.v); free(b.v); free(s.v);
        if (!r.v) ERRV(g, "line %zu: range() failed", n->line);
        return r;
    }

    /* get(m, k[, default]) — an absent key yields the default, which is what
     * the interpreter's three-argument form means. The emission itself lives
     * in emit_get_call so the cast accessors below can share it. */
    if (strcmp(bname, "get") == 0) {
        if (argc < 2 || argc > 3)
            ERRV(g, "line %zu: get() takes two or three arguments", n->line);
        return emit_get_call(g, n->as.call.args[0], n->as.call.args[1],
                             argc == 3 ? n->as.call.args[2] : NULL, n->line);
    }

    /* str() / int() / float(): which helper they need depends on the operand.
     * `string` is the interpreter's direct alias for str (b_string forwards to
     * native_str), so it takes the same branch and the same accessor desugar. */
    if (strcmp(bname, "str") == 0 || strcmp(bname, "string") == 0 ||
        strcmp(bname, "int") == 0 ||
        strcmp(bname, "float") == 0 || strcmp(bname, "stringify") == 0 ||
        strcmp(bname, "json") == 0) {
        if (argc < 1)
            ERRV(g, "line %zu: '%s' needs an argument", n->line, bname);
        /* str(m, k[, d]) is the interpreter's accessor shorthand —
         * value_from_map + the cast (src/builtins.c) — but only in the
         * shape value_from_map accepts: a map receiver and a string
         * key. Anything else keeps the interpreter's fallback
         * (serialize args[0]; the extra arguments are ignored there),
         * so the desugar fires only when the static types promise
         * that shape. Only str/int/float take the accessor:
         * stringify/json look at args[0] alone and ignore the rest
         * (native_stringify / native_json). Both receiver shapes go
         * to emit_acc_call — the accessor answers in the cast's own
         * type whatever the field holds, so neither the field's
         * declared shape nor the default's type may pick the flavour;
         * routing through emit_get_call instead would refuse
         * `int(acc, "n", 0)` on a string field, the sugar's primary
         * use. */
        bool is_str = strcmp(bname, "str") == 0 ||
                      strcmp(bname, "string") == 0;
        bool is_json = strcmp(bname, "json") == 0;
        Type *a0t = infer_node_type(g, n->as.call.args[0]);
        Type *a1t = argc >= 2 ? infer_node_type(g, n->as.call.args[1]) : NULL;
        bool accessor = argc >= 2 && a0t && a1t && a1t->kind == TY_STRING &&
                        (a0t->kind == TY_STRUCT || a0t->kind == TY_ANY) &&
                        (is_str || strcmp(bname, "int") == 0 ||
                         strcmp(bname, "float") == 0);
        if (accessor) {
            bool is_flt = strcmp(bname, "float") == 0;
            return emit_acc_call(g, n->as.call.args[0], n->as.call.args[1],
                                 argc == 3 ? n->as.call.args[2] : NULL,
                                 is_str, is_flt, false, a0t->kind == TY_ANY,
                                 n->line);
        }
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v)
            ERRV(g, "line %zu: '%s' failed to lower", n->line, bname);
        const char *fn = NULL, *pty = NULL, *rty = NULL;
        Type *rt = NULL;

        if (is_str || strcmp(bname, "stringify") == 0 || is_json) {
            switch (a.ty ? a.ty->kind : TY_ANY) {
            case TY_INT:    fn = "@lume_bi_str_i64";    pty = "i64";    break;
            case TY_FLOAT:  fn = "@lume_bi_str_double"; pty = "double"; break;
            case TY_BOOL:   fn = "@lume_bi_str_bool";   pty = "i64";    break;
            case TY_STRING:
                /* str of a string is itself; stringify quotes it (the
                 * interpreter's json_append_value does); json(str) means
                 * parse, whose result type is dynamic -- the statically
                 * typed emitters cannot spell it yet. */
                if (is_str) return a;
                if (is_json) {
                    /* json("...") parses into a tagged-any tree: emit a call to
                     * lume_json_parse (src/rt.c), which hands back a boxed
                     * LumeSlot* (TY_ANY lowers to i8*). The error buffer is a
                     * 256-byte stack slot zeroed before the call; a parse
                     * failure returns null and the value serializes as null
                     * (the try() error path is a follow-up). */
                    char eb[32], ep[32], tmp2[48];
                    snprintf(eb, sizeof eb, "%%jeb%d", g->tid);
                    snprintf(ep, sizeof ep, "%%jep%d", g->tid);
                    snprintf(tmp2, sizeof tmp2, "%%c%d", g->tid++);
                    EMIT(g, "  %s = alloca [256 x i8]\n", eb);
                    EMIT(g, "  %s = getelementptr [256 x i8], [256 x i8]* %s, i32 0, i32 0\n", ep, eb);
                    EMIT(g, "  store i8 0, i8* %s\n", ep);
                    EMIT(g, "  %s = call i8* @lume_json_parse(i8* %s, i8* %s, i64 256)\n",
                         tmp2, a.v, ep);
                    /* Parse failure goes through the recoverable-error channel
                     * (rt.c lume_error_set): inside try() it fills { err: ... }
                     * and ok stays a null slot; outside try() it prints and
                     * exits — the interpreter's json() VM error, same shape. */
                    char jslot[32], jtst[48];
                    snprintf(jslot, sizeof jslot, "%%js%d", g->tid);
                    snprintf(jtst, sizeof jtst, "%%t%d", g->tid++);
                    int jler = g->lid++, jlok = g->lid++, jle = g->lid++;
                    EMIT(g, "  %s = alloca i8*\n", jslot);
                    EMIT(g, "  store i8* %s, i8* %s\n", tmp2, jslot);
                    EMIT(g, "  %s = icmp eq i8* %s, null\n", jtst, tmp2);
                    EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n", jtst, jler, jlok);
                    EMIT(g, "L%d:\n", jler);
                    EMIT(g, "  store i8* null, i8* %s\n", jslot);
                    EMIT(g, "  call void @lume_error_set(i8* %s)\n", ep);
                    EMIT(g, "  br label %%L%d\n", jle);
                    EMIT(g, "L%d:\n", jlok);
                    EMIT(g, "  br label %%L%d\n", jle);
                    EMIT(g, "L%d:\n", jle);
                    char *jv = emit_instrf(g, type_prim(TY_ANY),
                                           "load i8*, i8* %s", jslot);
                    if (!jv) { free(a.v);
                               ERRV(g, "line %zu: json() failed to lower", n->line); }
                    free(a.v);
                    return val_take(type_prim(TY_ANY), jv);
                }
                fn = "@lume_rt_quote_str"; pty = "i8*"; break;
            case TY_LIST:
                /* Containers serialize to the same JSON the interpreter's
                 * print() produces; a.v holds the i8* container pointer. */
                fn = "@lume_rt_to_json_obj"; pty = "i8*"; break;
            case TY_STRUCT:
                /* An anonymous struct is a map (a LumeMap* i8*) and serializes
                 * directly. A named one is a raw GEP struct with no runtime
                 * header, so build a map from its fields first (the same path
                 * print() takes) and then serialize that map. */
                if (a.ty->name) {
                    struct_addr(g, &a);
                    Val m = emit_struct_to_map(g, a.v, a.ty);
                    if (!m.v) { free(a.v);
                                ERRV(g, "line %zu: cannot convert struct '%s'",
                                     n->line, a.ty->name); }
                    free(a.v);
                    a.v = m.v;
                    fn = "@lume_rt_to_json_obj"; pty = "i8*"; break;
                }
                fn = "@lume_rt_to_json_obj"; pty = "i8*"; break;
            case TY_ANY:
                /* A tagged-any value serializes by its runtime tag (src/rt.c):
                 * str() of a string slot is the bare string (lume_slot_str_scalar),
                 * stringify() keeps the JSON quotes (lume_slot_str); containers
                 * are JSON either way — the interpreter's str()/stringify()
                 * rules. */
                fn = is_str ? "@lume_slot_str_scalar" : "@lume_slot_str";
                pty = "i8*"; break;
            case TY_NULL:
                /* The serializer's null entry point prints the bare word the
                 * interpreter's str(null)/stringify(null) produce. */
                fn = "@lume_rt_to_json_obj"; pty = "i8*"; break;
            default:
                ERRV(g, "line %zu: %s() cannot convert this value",
                     n->line, bname);
            }
            rty = "i8*"; rt = type_prim(TY_STRING);
        } else if (strcmp(bname, "int") == 0) {
            /* Identity on an int (the interpreter's IS_NUM branch, and the
             * shape int(m, "k") takes on an int field); a float truncates;
             * a string parses a leading number like the interpreter's
             * atoll on the direct cast path ("12x" -> 12, "abc" -> 0). */
            if (a.ty && a.ty->kind == TY_INT) return a;
            if (a.ty && a.ty->kind == TY_STRING) {
                fn = "@lume_bi_parse_i64"; pty = "i8*"; rty = "i64";
                rt = type_prim(TY_INT);
            } else if (a.ty && a.ty->kind == TY_FLOAT) {
                fn = "@lume_bi_int"; pty = "double"; rty = "i64";
                rt = type_prim(TY_INT);
            } else {
                ERRV(g, "line %zu: int() only converts a float or a string here",
                     n->line);
            }
        } else {
            if (a.ty && a.ty->kind == TY_FLOAT) return a;
            if (a.ty && a.ty->kind == TY_STRING) {
                fn = "@lume_bi_parse_double"; pty = "i8*"; rty = "double";
                rt = type_prim(TY_FLOAT);
            } else if (a.ty && a.ty->kind == TY_INT) {
                fn = "@lume_bi_float"; pty = "i64"; rty = "double";
                rt = type_prim(TY_FLOAT);
            } else {
                ERRV(g, "line %zu: float() only converts an int or a string here",
                     n->line);
            }
        }

        char tmp[48];
        snprintf(tmp, sizeof tmp, "%%c%d", g->tid++);
        /* A comparison yields i1 in LLVM, but the runtime's bool builtins take
         * the interpreter's bool representation, which is i64. Widen before the
         * call; without this, str(1 == 1.0) fails to assemble with "%t36 defined
         * with type 'i1' but expected 'i64'" (reachable now that int/float
         * comparison is well-typed instead of rejected). */
        if (pty && strcmp(pty, "i64") == 0 && a.ty && a.ty->kind == TY_BOOL) {
            char *w = emit_instrf(g, type_prim(TY_BOOL), "zext i1 %s to i64", a.v);
            free(a.v);
            a.v = w;
        }
        EMIT(g, "  %s = call %s %s(%s %s)\n", tmp, rty, fn, pty, a.v);
        free(a.v);
        return val_make(rt, tmp);
    }

    /* sort(l): sort the list in place and return it (SPEC collection tools).
     * The runtime helper (src/rt.c lume_list_sort) applies the interpreter's
     * homogeneity rule by scanning the element slots, so any statically typed
     * list compiles and the runtime error shape matches the interpreter; a
     * non-list argument is a compile-time error here. The same list pointer
     * is returned (in place, like the interpreter's sort). */
    if (strcmp(bname, "sort") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: sort() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST) {
            free(a.v);
            ERRV(g, "line %zu: sort() needs a list", n->line);
        }
        EMIT(g, "  call void @lume_list_sort(i8* %s)\n", a.v);
        return a;   /* sorted in place; the same list is returned */
    }

    /* Quant builtins (2026-10-24): descriptive statistics, covariance /
     * correlation / linear regression and the time-series helpers. Like
     * sort() they take lists, so they live outside the numeric BUILTINS
     * table; the helpers (src/rt.c lume_bi_*) mirror the interpreter's
     * native_* contracts in builtins_math.c. */
    if (strcmp(bname, "mean") == 0 || strcmp(bname, "variance") == 0 ||
        strcmp(bname, "stddev") == 0 || strcmp(bname, "median") == 0 ||
        strcmp(bname, "max_drawdown") == 0 || strcmp(bname, "calmar_ratio") == 0 ||
        strcmp(bname, "skew") == 0 ||
        strcmp(bname, "kurtosis") == 0 ||
        strcmp(bname, "annualized_return") == 0 ||
        strcmp(bname, "annualized_volatility") == 0 ||
        strcmp(bname, "cumulative_return") == 0 ||
        strcmp(bname, "semivariance") == 0 ||
        strcmp(bname, "kelly_fraction") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: %s() takes exactly one argument", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: %s() needs a list", n->line, bname);
        const char *fn = strcmp(bname, "mean") == 0 ? "lume_bi_mean"
                       : strcmp(bname, "variance") == 0 ? "lume_bi_variance"
                       : strcmp(bname, "stddev") == 0 ? "lume_bi_stddev"
                       : strcmp(bname, "median") == 0 ? "lume_bi_median"
                       : strcmp(bname, "max_drawdown") == 0 ? "lume_bi_max_drawdown"
                       : strcmp(bname, "calmar_ratio") == 0 ? "lume_bi_calmar_ratio"
                       : strcmp(bname, "skew") == 0 ? "lume_bi_skew"
                       : strcmp(bname, "annualized_return") == 0 ? "lume_bi_annualized_return"
                       : strcmp(bname, "annualized_volatility") == 0 ? "lume_bi_annualized_volatility"
                       : strcmp(bname, "cumulative_return") == 0 ? "lume_bi_cumulative_return"
                       : strcmp(bname, "semivariance") == 0 ? "lume_bi_semivariance"
                       : strcmp(bname, "kelly_fraction") == 0 ? "lume_bi_kelly_fraction"
                       : "lume_bi_kurtosis";
        char *t = emit_instrf(g, type_prim(TY_FLOAT),
                              "call double @%s(i8* %s)", fn, a.v);
        free(a.v);
        return val_make(type_prim(TY_FLOAT), t);
    }

    if (strcmp(bname, "percentile") == 0 || strcmp(bname, "omega_ratio") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: %s() takes (list, scalar)", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: %s() needs a list", n->line, bname);
        Val p = cg_expr(g, n->as.call.args[1]);
        if (!p.v || !p.ty || (p.ty->kind != TY_INT && p.ty->kind != TY_FLOAT)) {
            free(a.v); free(p.v);
            ERRV(g, "line %zu: %s() needs a number for the scalar", n->line, bname);
        }
        if (p.ty->kind == TY_INT) {
            char *w = emit_instrf(g, type_prim(TY_FLOAT),
                                  "call double @lume_bi_float(i64 %s)", p.v);
            free(p.v);
            p.v = w;
        }
        const char *fn = strcmp(bname, "percentile") == 0
                             ? "lume_bi_percentile" : "lume_bi_omega_ratio";
        char *t = emit_instrf(g, type_prim(TY_FLOAT),
                              "call double @%s(i8* %s, double %s)", fn, a.v, p.v);
        free(a.v); free(p.v);
        return val_make(type_prim(TY_FLOAT), t);
    }

    /* Autocorrelation at a lag: a list plus an integer lag — the rolling
     * window pattern with a double return. */
    if (strcmp(bname, "autocorrelation") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: autocorrelation() takes (series, lag)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: autocorrelation() needs a list", n->line);
        Val lag = cg_expr(g, n->as.call.args[1]);
        if (!lag.v || !lag.ty || (lag.ty->kind != TY_INT && lag.ty->kind != TY_FLOAT)) {
            free(a.v); free(lag.v);
            ERRV(g, "line %zu: autocorrelation() needs a lag size", n->line);
        }
        if (lag.ty->kind == TY_FLOAT) {
            char *wi = emit_instrf(g, type_prim(TY_INT),
                                   "call i64 @lume_bi_ftoi(double %s)", lag.v);
            free(lag.v); lag.v = wi;
        }
        char *t = emit_instrf(g, type_prim(TY_FLOAT),
                              "call double @lume_bi_autocorrelation(i8* %s, i64 %s)",
                              a.v, lag.v);
        free(a.v); free(lag.v);
        return val_make(type_prim(TY_FLOAT), t);
    }

    /* Information coefficient: rank correlation between a signal and its
     * forward returns lagged by k periods — (list, list, i64) -> double,
     * the integer lag truncates through lume_bi_ftoi. */
    if (strcmp(bname, "ic") == 0) {
        if (argc != 3)
            ERRV(g, "line %zu: ic() takes (signal, returns, lag)", n->line);
        Val s = cg_expr(g, n->as.call.args[0]);
        Val r = cg_expr(g, n->as.call.args[1]);
        if (!s.v || !s.ty || s.ty->kind != TY_LIST ||
            !r.v || !r.ty || r.ty->kind != TY_LIST) {
            free(s.v); free(r.v);
            ERRV(g, "line %zu: ic() needs two lists", n->line);
        }
        Val lag = cg_expr(g, n->as.call.args[2]);
        if (!lag.v || !lag.ty || (lag.ty->kind != TY_INT && lag.ty->kind != TY_FLOAT)) {
            free(s.v); free(r.v); free(lag.v);
            ERRV(g, "line %zu: ic() needs a lag size", n->line);
        }
        if (lag.ty->kind == TY_FLOAT) {
            char *wi = emit_instrf(g, type_prim(TY_INT),
                                   "call i64 @lume_bi_ftoi(double %s)", lag.v);
            free(lag.v); lag.v = wi;
        }
        char *t = emit_instrf(g, type_prim(TY_FLOAT),
                              "call double @lume_bi_ic(i8* %s, i8* %s, i64 %s)",
                              s.v, r.v, lag.v);
        free(s.v); free(r.v); free(lag.v);
        return val_make(type_prim(TY_FLOAT), t);
    }

    if (strcmp(bname, "covariance") == 0 || strcmp(bname, "correlation") == 0 ||
        strcmp(bname, "linreg") == 0 || strcmp(bname, "information_ratio") == 0 ||
        strcmp(bname, "spearman") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: %s() takes (x, y)", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: %s() needs a list", n->line, bname);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!b.v || !b.ty || b.ty->kind != TY_LIST) {
            free(a.v); free(b.v);
            ERRV(g, "line %zu: %s() needs a list", n->line, bname);
        }
        if (strcmp(bname, "linreg") == 0) {
            char *t = emit_instrf(g, type_anon_struct(),
                                  "call i8* @lume_bi_linreg(i8* %s, i8* %s)",
                                  a.v, b.v);
            free(a.v); free(b.v);
            return val_make(type_anon_struct(), t);
        }
        const char *fn = strcmp(bname, "covariance") == 0 ? "lume_bi_covariance"
                       : strcmp(bname, "information_ratio") == 0 ? "lume_bi_information_ratio"
                       : strcmp(bname, "spearman") == 0 ? "lume_bi_spearman"
                       : "lume_bi_correlation";
        char *t = emit_instrf(g, type_prim(TY_FLOAT),
                              "call double @%s(i8* %s, i8* %s)", fn, a.v, b.v);
        free(a.v); free(b.v);
        return val_make(type_prim(TY_FLOAT), t);
    }

    /* Technical indicators (2026-11-02, #52): macd takes a prices list
     * plus three integer periods and answers an anonymous struct
     * {macd, signal, hist} of float lists; bollinger takes a prices list,
     * an integer window and a float k and answers {mid, upper, lower} —
     * both emitted as an opaque i8* like linreg. */
    if (strcmp(bname, "macd") == 0) {
        if (argc != 4)
            ERRV(g, "line %zu: macd() takes (prices, fast, slow, signal)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: macd() needs a list", n->line);
        Val f = cg_expr(g, n->as.call.args[1]);
        Val s = cg_expr(g, n->as.call.args[2]);
        Val sig = cg_expr(g, n->as.call.args[3]);
        if (!f.v || !f.ty || (f.ty->kind != TY_INT && f.ty->kind != TY_FLOAT) ||
            !s.v || !s.ty || (s.ty->kind != TY_INT && s.ty->kind != TY_FLOAT) ||
            !sig.v || !sig.ty || (sig.ty->kind != TY_INT && sig.ty->kind != TY_FLOAT)) {
            free(a.v); free(f.v); free(s.v); free(sig.v);
            ERRV(g, "line %zu: macd() needs integer periods", n->line);
        }
        if (f.ty->kind == TY_FLOAT) {
            char *wi = emit_instrf(g, type_prim(TY_INT),
                                   "call i64 @lume_bi_ftoi(double %s)", f.v);
            free(f.v); f.v = wi;
        }
        if (s.ty->kind == TY_FLOAT) {
            char *wi = emit_instrf(g, type_prim(TY_INT),
                                   "call i64 @lume_bi_ftoi(double %s)", s.v);
            free(s.v); s.v = wi;
        }
        if (sig.ty->kind == TY_FLOAT) {
            char *wi = emit_instrf(g, type_prim(TY_INT),
                                   "call i64 @lume_bi_ftoi(double %s)", sig.v);
            free(sig.v); sig.v = wi;
        }
        char *t = emit_instrf(g, type_anon_struct(),
                              "call i8* @lume_bi_macd(i8* %s, i64 %s, i64 %s, i64 %s)",
                              a.v, f.v, s.v, sig.v);
        free(a.v); free(f.v); free(s.v); free(sig.v);
        return val_make(type_anon_struct(), t);
    }

    if (strcmp(bname, "bollinger") == 0) {
        if (argc != 3)
            ERRV(g, "line %zu: bollinger() takes (prices, window, k)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: bollinger() needs a list", n->line);
        Val w = cg_expr(g, n->as.call.args[1]);
        if (!w.v || !w.ty || (w.ty->kind != TY_INT && w.ty->kind != TY_FLOAT)) {
            free(a.v); free(w.v);
            ERRV(g, "line %zu: bollinger() needs a window size", n->line);
        }
        if (w.ty->kind == TY_FLOAT) {
            char *wi = emit_instrf(g, type_prim(TY_INT),
                                   "call i64 @lume_bi_ftoi(double %s)", w.v);
            free(w.v); w.v = wi;
        }
        Val k = cg_expr(g, n->as.call.args[2]);
        if (!k.v || !k.ty || (k.ty->kind != TY_INT && k.ty->kind != TY_FLOAT)) {
            free(a.v); free(w.v); free(k.v);
            ERRV(g, "line %zu: bollinger() needs a number for k", n->line);
        }
        if (k.ty->kind == TY_INT) {
            char *kd = emit_instrf(g, type_prim(TY_FLOAT),
                                   "call double @lume_bi_float(i64 %s)", k.v);
            free(k.v); k.v = kd;
        }
        char *t = emit_instrf(g, type_anon_struct(),
                              "call i8* @lume_bi_bollinger(i8* %s, i64 %s, double %s)",
                              a.v, w.v, k.v);
        free(a.v); free(w.v); free(k.v);
        return val_make(type_anon_struct(), t);
    }

    /* Performance attribution (2026-11-02, #53): alpha_beta takes a
     * returns list, a benchmark list and a scalar rf, and answers an
     * anonymous struct {alpha, beta} — emitted as an opaque i8* like
     * linreg / macd. The scalar widens to double, following the #48
     * (list, scalar) group. */
    if (strcmp(bname, "alpha_beta") == 0) {
        if (argc != 3)
            ERRV(g, "line %zu: alpha_beta() takes (returns, benchmark, rf)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST ||
            !b.v || !b.ty || b.ty->kind != TY_LIST) {
            free(a.v); free(b.v);
            ERRV(g, "line %zu: alpha_beta() needs two lists", n->line);
        }
        Val rf = cg_expr(g, n->as.call.args[2]);
        if (!rf.v || !rf.ty || (rf.ty->kind != TY_INT && rf.ty->kind != TY_FLOAT)) {
            free(a.v); free(b.v); free(rf.v);
            ERRV(g, "line %zu: alpha_beta() needs a number for rf", n->line);
        }
        if (rf.ty->kind == TY_INT) {
            char *fd = emit_instrf(g, type_prim(TY_FLOAT),
                                   "call double @lume_bi_float(i64 %s)", rf.v);
            free(rf.v); rf.v = fd;
        }
        char *t = emit_instrf(g, type_anon_struct(),
                              "call i8* @lume_bi_alpha_beta(i8* %s, i8* %s, double %s)",
                              a.v, b.v, rf.v);
        free(a.v); free(b.v); free(rf.v);
        return val_make(type_anon_struct(), t);
    }

    /* Treynor ratio: same (returns, benchmark, rf) shape as alpha_beta
     * but answers a double — the reward per unit of systematic risk. */
    if (strcmp(bname, "treynor_ratio") == 0) {
        if (argc != 3)
            ERRV(g, "line %zu: treynor_ratio() takes (returns, benchmark, rf)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST ||
            !b.v || !b.ty || b.ty->kind != TY_LIST) {
            free(a.v); free(b.v);
            ERRV(g, "line %zu: treynor_ratio() needs two lists", n->line);
        }
        Val rf = cg_expr(g, n->as.call.args[2]);
        if (!rf.v || !rf.ty || (rf.ty->kind != TY_INT && rf.ty->kind != TY_FLOAT)) {
            free(a.v); free(b.v); free(rf.v);
            ERRV(g, "line %zu: treynor_ratio() needs a number for rf", n->line);
        }
        if (rf.ty->kind == TY_INT) {
            char *fd = emit_instrf(g, type_prim(TY_FLOAT),
                                   "call double @lume_bi_float(i64 %s)", rf.v);
            free(rf.v); rf.v = fd;
        }
        char *t = emit_instrf(g, type_prim(TY_FLOAT),
                              "call double @lume_bi_treynor_ratio(i8* %s, i8* %s, double %s)",
                              a.v, b.v, rf.v);
        free(a.v); free(b.v); free(rf.v);
        return val_make(type_prim(TY_FLOAT), t);
    }

    /* Hypothesis tests (2026-10-29): z_test / tstat take a sample list plus
     * scalars, chisq_stat takes observed + expected lists — all answering a
     * float. The pure-scalar p-value helpers (z_pvalue / ttest_p / chisq_p)
     * live in the BUILTINS table; these three need a list argument, so they
     * follow percentile()'s hand-emit pattern. */
    if (strcmp(bname, "z_test") == 0) {
        if (argc != 3)
            ERRV(g, "line %zu: z_test() takes (list, mu, sigma)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: z_test() needs a list", n->line);
        Val m = cg_expr(g, n->as.call.args[1]);
        Val s = cg_expr(g, n->as.call.args[2]);
        if (!m.v || !m.ty || (m.ty->kind != TY_INT && m.ty->kind != TY_FLOAT) ||
            !s.v || !s.ty || (s.ty->kind != TY_INT && s.ty->kind != TY_FLOAT)) {
            free(a.v); free(m.v); free(s.v);
            ERRV(g, "line %zu: z_test() needs numbers for mu and sigma", n->line);
        }
        if (m.ty->kind == TY_INT) {
            char *w = emit_instrf(g, type_prim(TY_FLOAT),
                                  "call double @lume_bi_float(i64 %s)", m.v);
            free(m.v);
            m.v = w;
        }
        if (s.ty->kind == TY_INT) {
            char *w = emit_instrf(g, type_prim(TY_FLOAT),
                                  "call double @lume_bi_float(i64 %s)", s.v);
            free(s.v);
            s.v = w;
        }
        char *t = emit_instrf(g, type_prim(TY_FLOAT),
                              "call double @lume_bi_z_test(i8* %s, double %s, double %s)",
                              a.v, m.v, s.v);
        free(a.v); free(m.v); free(s.v);
        return val_make(type_prim(TY_FLOAT), t);
    }

    if (strcmp(bname, "tstat") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: tstat() takes (list, mu)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: tstat() needs a list", n->line);
        Val m = cg_expr(g, n->as.call.args[1]);
        if (!m.v || !m.ty || (m.ty->kind != TY_INT && m.ty->kind != TY_FLOAT)) {
            free(a.v); free(m.v);
            ERRV(g, "line %zu: tstat() needs a number for mu", n->line);
        }
        if (m.ty->kind == TY_INT) {
            char *w = emit_instrf(g, type_prim(TY_FLOAT),
                                  "call double @lume_bi_float(i64 %s)", m.v);
            free(m.v);
            m.v = w;
        }
        char *t = emit_instrf(g, type_prim(TY_FLOAT),
                              "call double @lume_bi_tstat(i8* %s, double %s)",
                              a.v, m.v);
        free(a.v); free(m.v);
        return val_make(type_prim(TY_FLOAT), t);
    }

    if (strcmp(bname, "chisq_stat") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: chisq_stat() takes (observed, expected)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: chisq_stat() needs a list", n->line);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!b.v || !b.ty || b.ty->kind != TY_LIST) {
            free(a.v); free(b.v);
            ERRV(g, "line %zu: chisq_stat() needs a list", n->line);
        }
        char *t = emit_instrf(g, type_prim(TY_FLOAT),
                              "call double @lume_bi_chisq_stat(i8* %s, i8* %s)",
                              a.v, b.v);
        free(a.v); free(b.v);
        return val_make(type_prim(TY_FLOAT), t);
    }

    if (strcmp(bname, "pct_change") == 0 || strcmp(bname, "rank") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: %s() takes exactly one argument", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: %s() needs a list", n->line, bname);
        const char *fn = strcmp(bname, "pct_change") == 0
                             ? "lume_bi_pct_change" : "lume_bi_rank";
        char *t = emit_instrf(g, type_list(type_prim(TY_FLOAT)),
                              "call i8* @%s(i8* %s)", fn, a.v);
        free(a.v);
        return val_make(type_list(type_prim(TY_FLOAT)), t);
    }

    /* rolling_mean/rolling_stddev/rsi (list, window) and ewma (list, alpha):
     * list plus one scalar, returning a fresh float list. The window is an
     * integer semantic — an int argument passes straight through as i64, a
     * float truncates through lume_bi_ftoi (the same truncation the
     * interpreter applies). */
    if (strcmp(bname, "rolling_mean") == 0 || strcmp(bname, "rolling_stddev") == 0 ||
        strcmp(bname, "rsi") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: %s() takes (list, window)", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: %s() needs a list", n->line, bname);
        Val w = cg_expr(g, n->as.call.args[1]);
        if (!w.v || !w.ty || (w.ty->kind != TY_INT && w.ty->kind != TY_FLOAT)) {
            free(a.v); free(w.v);
            ERRV(g, "line %zu: %s() needs a window size", n->line, bname);
        }
        if (w.ty->kind == TY_FLOAT) {
            char *wi = emit_instrf(g, type_prim(TY_INT),
                                   "call i64 @lume_bi_ftoi(double %s)", w.v);
            free(w.v);
            w.v = wi;
        }
        const char *fn = strcmp(bname, "rolling_mean") == 0
                             ? "lume_bi_rolling_mean" : strcmp(bname, "rsi") == 0
                             ? "lume_bi_rsi" : "lume_bi_rolling_stddev";
        char *t = emit_instrf(g, type_list(type_prim(TY_FLOAT)),
                              "call i8* @%s(i8* %s, i64 %s)", fn, a.v, w.v);
        free(a.v); free(w.v);
        return val_make(type_list(type_prim(TY_FLOAT)), t);
    }

    /* rolling_sharpe/sortino/var/cvar (list, window, scalar): the #48
     * metrics applied to every window of a return series — the
     * backtesting shape. (list, i64, double) -> fresh float list with
     * window-1 leading NANs; the window truncates through lume_bi_ftoi,
     * the scalar widens through lume_bi_float. */
    if (strcmp(bname, "rolling_sharpe") == 0 || strcmp(bname, "rolling_sortino") == 0 ||
        strcmp(bname, "rolling_var") == 0 || strcmp(bname, "rolling_cvar") == 0) {
        if (argc != 3)
            ERRV(g, "line %zu: %s() takes (list, window, scalar)", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: %s() needs a list", n->line, bname);
        Val w = cg_expr(g, n->as.call.args[1]);
        if (!w.v || !w.ty || (w.ty->kind != TY_INT && w.ty->kind != TY_FLOAT)) {
            free(a.v); free(w.v);
            ERRV(g, "line %zu: %s() needs a window size", n->line, bname);
        }
        if (w.ty->kind == TY_FLOAT) {
            char *wi = emit_instrf(g, type_prim(TY_INT),
                                   "call i64 @lume_bi_ftoi(double %s)", w.v);
            free(w.v);
            w.v = wi;
        }
        Val s = cg_expr(g, n->as.call.args[2]);
        if (!s.v || !s.ty || (s.ty->kind != TY_INT && s.ty->kind != TY_FLOAT)) {
            free(a.v); free(w.v); free(s.v);
            ERRV(g, "line %zu: %s() needs a number for the third argument", n->line, bname);
        }
        if (s.ty->kind == TY_INT) {
            char *sf = emit_instrf(g, type_prim(TY_FLOAT),
                                   "call double @lume_bi_float(i64 %s)", s.v);
            free(s.v);
            s.v = sf;
        }
        const char *fn = NULL;
        if (strcmp(bname, "rolling_sharpe") == 0) fn = "lume_bi_rolling_sharpe";
        else if (strcmp(bname, "rolling_sortino") == 0) fn = "lume_bi_rolling_sortino";
        else if (strcmp(bname, "rolling_var") == 0) fn = "lume_bi_rolling_var";
        else fn = "lume_bi_rolling_cvar";
        char *t = emit_instrf(g, type_list(type_prim(TY_FLOAT)),
                              "call i8* @%s(i8* %s, i64 %s, double %s)",
                              fn, a.v, w.v, s.v);
        free(a.v); free(w.v); free(s.v);
        return val_make(type_list(type_prim(TY_FLOAT)), t);
    }

    if (strcmp(bname, "ewma") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: ewma() takes (list, alpha)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_LIST)
            ERRV(g, "line %zu: ewma() needs a list", n->line);
        Val p = cg_expr(g, n->as.call.args[1]);
        if (!p.v || !p.ty || (p.ty->kind != TY_INT && p.ty->kind != TY_FLOAT)) {
            free(a.v); free(p.v);
            ERRV(g, "line %zu: ewma() needs a number for alpha", n->line);
        }
        if (p.ty->kind == TY_INT) {
            char *w = emit_instrf(g, type_prim(TY_FLOAT),
                                  "call double @lume_bi_float(i64 %s)", p.v);
            free(p.v);
            p.v = w;
        }
        char *t = emit_instrf(g, type_list(type_prim(TY_FLOAT)),
                              "call i8* @lume_bi_ewma(i8* %s, double %s)", a.v, p.v);
        free(a.v); free(p.v);
        return val_make(type_list(type_prim(TY_FLOAT)), t);
    }

    /* ---- string tools (interpreter parity, builtins_str.c) ----
     * String parameters keep every one of these out of the numeric
     * BUILTINS table (that path coerces its arguments to i64/double), so
     * each gets its own branch, the same shape read_file/env() use. The
     * helpers live in src/rt.c and mirror the interpreter's native_*
     * contracts: ASCII-only case mapping, byte-based split/substr, a
     * 16 MiB result ceiling on replace/join, and str_match's ERE
     * findall -> [{text, index, groups}]. */
    if (strcmp(bname, "upper") == 0 || strcmp(bname, "lower") == 0 ||
        strcmp(bname, "capitalize") == 0 || strcmp(bname, "trim") == 0) {
        if (argc < 1)
            ERRV(g, "line %zu: %s() needs a string", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind != TY_STRING)
            ERRV(g, "line %zu: %s() needs a string", n->line, bname);
        const char *fn = strcmp(bname, "upper") == 0 ? "lume_bi_upper"
                       : strcmp(bname, "lower") == 0 ? "lume_bi_lower"
                       : strcmp(bname, "capitalize") == 0
                             ? "lume_bi_capitalize" : "lume_bi_trim";
        Val r = val_take(type_prim(TY_STRING),
                         emit_instrf(g, type_prim(TY_STRING),
                                     "call i8* @%s(i8* %s)", fn, a.v));
        free(a.v);
        if (!r.v) ERRV(g, "line %zu: %s() failed", n->line, bname);
        return r;
    }

    if (strcmp(bname, "contains") == 0) {
        if (argc < 2)
            ERRV(g, "line %zu: contains() needs (s, sub)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !b.v || !a.ty || !b.ty ||
            a.ty->kind != TY_STRING || b.ty->kind != TY_STRING)
            ERRV(g, "line %zu: contains() needs two strings", n->line);
        char *iv = emit_instrf(g, type_prim(TY_INT),
                               "call i64 @lume_bi_contains(i8* %s, i8* %s)",
                               a.v, b.v);
        free(a.v); free(b.v);
        if (!iv) ERRV(g, "line %zu: contains() failed", n->line);
        char *bv = emit_instrf(g, type_prim(TY_BOOL), "trunc i64 %s to i1", iv);
        free(iv);
        if (!bv) ERRV(g, "line %zu: contains() failed", n->line);
        return val_take(type_prim(TY_BOOL), bv);
    }

    if (strcmp(bname, "split") == 0) {
        if (argc < 2)
            ERRV(g, "line %zu: split() needs (s, sep)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !b.v || !a.ty || !b.ty ||
            a.ty->kind != TY_STRING || b.ty->kind != TY_STRING)
            ERRV(g, "line %zu: split() needs two strings", n->line);
        /* The result is a list of strings: typing the element kind keeps
         * `for s in split(...)` readable through lume_list_at_s. */
        Val r = val_take(type_list(type_prim(TY_STRING)),
                         emit_instrf(g, type_list(type_prim(TY_STRING)),
                                     "call i8* @lume_bi_split(i8* %s, i8* %s)",
                                     a.v, b.v));
        free(a.v); free(b.v);
        if (!r.v) ERRV(g, "line %zu: split() failed", n->line);
        return r;
    }

    if (strcmp(bname, "join") == 0) {
        if (argc < 2)
            ERRV(g, "line %zu: join() needs (list, sep)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !b.v || !a.ty || !b.ty ||
            a.ty->kind != TY_LIST || b.ty->kind != TY_STRING)
            ERRV(g, "line %zu: join() needs a list and a string", n->line);
        Val r = val_take(type_prim(TY_STRING),
                         emit_instrf(g, type_prim(TY_STRING),
                                     "call i8* @lume_bi_join(i8* %s, i8* %s)",
                                     a.v, b.v));
        free(a.v); free(b.v);
        if (!r.v) ERRV(g, "line %zu: join() failed", n->line);
        return r;
    }

    if (strcmp(bname, "replace") == 0) {
        if (argc < 3)
            ERRV(g, "line %zu: replace() needs (s, from, to)", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        Val c = cg_expr(g, n->as.call.args[2]);
        if (!a.v || !b.v || !c.v || !a.ty || !b.ty || !c.ty ||
            a.ty->kind != TY_STRING || b.ty->kind != TY_STRING ||
            c.ty->kind != TY_STRING)
            ERRV(g, "line %zu: replace() needs three strings", n->line);
        Val r = val_take(type_prim(TY_STRING),
                         emit_instrf(g, type_prim(TY_STRING),
                                     "call i8* @lume_bi_replace(i8* %s, i8* %s, i8* %s)",
                                     a.v, b.v, c.v));
        free(a.v); free(b.v); free(c.v);
        if (!r.v) ERRV(g, "line %zu: replace() failed", n->line);
        return r;
    }

    if (strcmp(bname, "substr") == 0) {
        if (argc < 2 || argc > 3)
            ERRV(g, "line %zu: substr() needs (s, start[, len])", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        Val c = argc == 3 ? cg_expr(g, n->as.call.args[2])
                          : (Val){ NULL, NULL, 0 };
        if (!a.v || !b.v || !a.ty || !b.ty ||
            a.ty->kind != TY_STRING || b.ty->kind != TY_INT ||
            (argc == 3 && (!c.v || !c.ty || c.ty->kind != TY_INT)))
            ERRV(g, "line %zu: substr() needs a string and integer bounds",
                 n->line);
        /* The two-argument form spells len as -1, which the helper reads
         * as "to the end of the string" — the interpreter's default. */
        Val r = val_take(type_prim(TY_STRING),
                         emit_instrf(g, type_prim(TY_STRING),
                                     "call i8* @lume_bi_substr(i8* %s, i64 %s, i64 %s)",
                                     a.v, b.v, argc == 3 ? c.v : "-1"));
        free(a.v); free(b.v); free(c.v);
        if (!r.v) ERRV(g, "line %zu: substr() failed", n->line);
        return r;
    }

    if (strcmp(bname, "str_match") == 0 || strcmp(bname, "regex") == 0) {
        if (argc < 2)
            ERRV(g, "line %zu: %s() needs (pattern, text)", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        Val b = cg_expr(g, n->as.call.args[1]);
        if (!a.v || !b.v || !a.ty || !b.ty ||
            a.ty->kind != TY_STRING || b.ty->kind != TY_STRING)
            ERRV(g, "line %zu: %s() needs two strings", n->line, bname);
        /* A list of match maps, each {text, index, groups}. */
        Val r = val_take(type_list(type_anon_struct()),
                         emit_instrf(g, type_list(type_anon_struct()),
                                     "call i8* @lume_bi_str_match(i8* %s, i8* %s)",
                                     a.v, b.v));
        free(a.v); free(b.v);
        if (!r.v) ERRV(g, "line %zu: %s() failed", n->line, bname);
        return r;
    }

    if (strcmp(bname, "exp") == 0 || strcmp(bname, "log") == 0 ||
        strcmp(bname, "ln") == 0) {
        if (argc < 1)
            ERRV(g, "line %zu: %s() needs a number", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty ||
            (a.ty->kind != TY_INT && a.ty->kind != TY_FLOAT))
            ERRV(g, "line %zu: %s() needs a number", n->line, bname);
        a = coerce(g, type_prim(TY_FLOAT), a, n->line);
        if (!a.v) ERRV(g, "line %zu: %s() failed to lower", n->line, bname);
        /* exp has no domain check; log/ln refuse a non-positive argument
         * through the error channel, like the interpreter's native_log. */
        const char *fn = strcmp(bname, "exp") == 0 ? "lume_bi_exp"
                                                   : "lume_bi_log";
        Val r = val_take(type_prim(TY_FLOAT),
                         emit_instrf(g, type_prim(TY_FLOAT),
                                     "call double @%s(double %s)", fn, a.v));
        free(a.v);
        if (!r.v) ERRV(g, "line %zu: %s() failed", n->line, bname);
        return r;
    }

    /* bool(x) — value_truthy (value.c) by static type: null and "" are
     * false, 0/0.0 false, an empty container false, everything else true.
     * An any value dispatches on its runtime tag (lume_bi_bool_slot), and
     * bool(m, "k"[, d]) is the same accessor desugar the casts take
     * (the interpreter checks value_from_map first, builtins.c) — it
     * answers the truthiness of the found value, or of the default when
     * the key is absent. */
    if (strcmp(bname, "bool") == 0) {
        if (argc < 1)
            ERRV(g, "line %zu: bool() needs an argument", n->line);
        Type *a0t = infer_node_type(g, n->as.call.args[0]);
        Type *a1t = argc >= 2 ? infer_node_type(g, n->as.call.args[1]) : NULL;
        bool accessor = argc >= 2 && a0t && a1t && a1t->kind == TY_STRING &&
                        (a0t->kind == TY_STRUCT || a0t->kind == TY_ANY);
        if (accessor) {
            return emit_acc_call(g, n->as.call.args[0], n->as.call.args[1],
                                 argc == 3 ? n->as.call.args[2] : NULL,
                                 false, false, true, a0t->kind == TY_ANY,
                                 n->line);
        }
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v)
            ERRV(g, "line %zu: bool() failed to lower", n->line);
        if (a.ty && a.ty->kind == TY_BOOL) return a;    /* already i1 */
        if (a.ty && a.ty->kind == TY_NULL)
            return val_make(type_prim(TY_BOOL), "false");
        const char *fn = NULL, *pty = NULL;
        switch (a.ty ? a.ty->kind : TY_ANY) {
        case TY_INT:    fn = "lume_bi_bool_i";    pty = "i64";    break;
        case TY_FLOAT:  fn = "lume_bi_bool_f";    pty = "double"; break;
        case TY_STRING: fn = "lume_bi_bool_s";    pty = "i8*";    break;
        case TY_LIST:   fn = "lume_bi_bool_obj";  pty = "i8*";    break;
        case TY_STRUCT: fn = "lume_bi_bool_obj";  pty = "i8*";    break;
        case TY_ANY:    fn = "lume_bi_bool_slot"; pty = "i8*";    break;
        default:
            ERRV(g, "line %zu: bool() cannot convert this value", n->line);
        }
        if (a.ty && a.ty->name) {
            /* A named struct is a raw GEP record with no map header, so
             * box it first — the length the helper reads is then the
             * field count, the interpreter's map truthiness. */
            struct_addr(g, &a);
            Val m = emit_struct_to_map(g, a.v, a.ty);
            if (!m.v) {
                free(a.v);
                ERRV(g, "line %zu: bool() cannot convert struct '%s'",
                     n->line, a.ty->name);
            }
            free(a.v);
            a.v = m.v;
            a.ty = m.ty;
        }
        char *iv = emit_instrf(g, type_prim(TY_INT), "call i64 @%s(%s %s)",
                               fn, pty, a.v);
        free(a.v);
        if (!iv) ERRV(g, "line %zu: bool() failed", n->line);
        char *bv = emit_instrf(g, type_prim(TY_BOOL), "trunc i64 %s to i1", iv);
        free(iv);
        if (!bv) ERRV(g, "line %zu: bool() failed", n->line);
        return val_take(type_prim(TY_BOOL), bv);
    }

    return (Val){ NULL, NULL, 0 };
}

Val cg_call(CG *g, Node *n)
{
    /* print() is the one builtin we lower directly to printf */
    if (n->as.call.callee && n->as.call.callee->type == N_VAR &&
        strcmp(n->as.call.callee->as.var.name, "print") == 0) {
        if (n->as.call.argc != 1)
            ERRV(g, "line %zu: print() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        return cg_print(g, n, a);
    }

    /* run(): hand control to the native server loop (blocking). */
    if (n->as.call.callee && n->as.call.callee->type == N_VAR &&
        strcmp(n->as.call.callee->as.var.name, "run") == 0) {
        if (n->as.call.argc != 0)
            ERRV(g, "line %zu: run() takes no arguments", n->line);
        EMIT(g, "  call void @lume_srv_run()\n");
        return val_make(type_prim(TY_NULL), "null");
    }

    /* Native server handler: get(req.query_params, key, default?) lowers to
     * lume_srv_qp with the handler's query FFI argument. */
    if (g->in_handler && n->as.call.callee &&
        n->as.call.callee->type == N_VAR &&
        strcmp(n->as.call.callee->as.var.name, "get") == 0) {
        Node *a0 = n->as.call.argc >= 1 ? n->as.call.args[0] : NULL;
        int qp = a0 && a0->type == N_MEMBER && a0->as.member.obj &&
                 a0->as.member.obj->type == N_VAR &&
                 strcmp(a0->as.member.obj->as.var.name, g->hreq) == 0 &&
                 strcmp(a0->as.member.name, "query_params") == 0;
        if (!qp)
            ERRV(g, "line %zu: native server: get() must be called on req.query_params",
                 n->line);
        if (n->as.call.argc < 2 || n->as.call.argc > 3)
            ERRV(g, "line %zu: get() takes (query_params, key, default?)", n->line);
        Node *a1 = n->as.call.args[1];
        if (!(a1->type == N_LITERAL && a1->as.lit.kind == LIT_STR))
            ERRV(g, "line %zu: native server: get() key must be a string literal",
                 n->line);
        const char *kt = a1->as.lit.text;
        size_t kl = (size_t)a1->as.lit.len;
        if (kl >= 2 && kt[0] == '"' && kt[kl - 1] == '"') { kt++; kl -= 2; }
        char *kc = cg_string_val(g, kt, kl);
        char *dc;
        if (n->as.call.argc == 3) {
            Node *a2 = n->as.call.args[2];
            if (!(a2->type == N_LITERAL && a2->as.lit.kind == LIT_STR))
                ERRV(g, "line %zu: native server: get() default must be a string literal",
                     n->line);
            const char *dt = a2->as.lit.text;
            size_t dl = (size_t)a2->as.lit.len;
            if (dl >= 2 && dt[0] == '"' && dt[dl - 1] == '"') { dt++; dl -= 2; }
            dc = cg_string_val(g, dt, dl);
        } else {
            dc = xstrdup("null");      /* i8* null */
        }
        Type *st = type_prim(TY_STRING);
        char *gt = emit_instrf(g, st,
            "getelementptr %%struct.SrvReq, %%struct.SrvReq* %%p0, i32 0, i32 2");
        char *q  = emit_instrf(g, st, "load i8*, i8** %s", gt);
        free(gt);
        char *r = emit_instrf(g, st,
            "call i8* @lume_srv_qp(i8* %s, i8* %s, i8* %s)", q, kc, dc);
        free(q); free(kc); free(dc);
        return val_take(st, r);
    }

    /* Native server handler: int(...) — string/number to i64 via atoi. */
    if (g->in_handler && n->as.call.callee &&
        n->as.call.callee->type == N_VAR &&
        strcmp(n->as.call.callee->as.var.name, "int") == 0) {
        if (n->as.call.argc != 1)
            ERRV(g, "line %zu: int() takes one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v) { free(a.v); ERRV(g, "line %zu: bad int() operand", n->line); }
        if (a.ty && a.ty->kind == TY_INT) return a;
        char *r = emit_instrf(g, type_prim(TY_INT),
            "call i64 @lume_srv_atoi(i8* %s)", a.v);
        free(a.v);
        return val_take(type_prim(TY_INT), r);
    }

    Sig *s = NULL;
    const char *name = NULL;
    Node *callee = n->as.call.callee;
    if (callee && callee->type == N_MEMBER && callee->as.member.obj &&
        callee->as.member.obj->type == N_VAR) {
        /* ns member call: policy.decide(...) — the module signature injected
         * by codegen_infer_signatures_multi resolves under "<ns>.<name>"; the
         * call itself emits against the module function's own symbol
         * (@L_<fname>), which codegen_emit_ir_multi defines under its source
         * name. */
        char qname[512];
        snprintf(qname, sizeof qname, "%s.%s",
                 callee->as.member.obj->as.var.name, callee->as.member.name);
        s = sig_find(&g->sigs, qname);
        if (!s)
            ERRV(g, "line %zu: call to unknown function '%s'", n->line, qname);
        name = s->node ? (s->node->as.func.cname ? s->node->as.func.cname
                                                 : s->node->as.func.name)
                       : qname;
    } else {
        if (!callee || callee->type != N_VAR)
            ERRV(g, "line %zu: only direct calls to named functions are supported", n->line);

        /* A closure-valued variable: `f(3)` where f was bound to a lambda. The
         * variable's TY_FUNC type carries the back-pointer to the lambda node
         * the signature pass pinned; a plain named-function reference has no
         * back-pointer and falls through to the sig_find path below. */
        {
            Type *ct = infer_node_type(g, callee);
            if (ct && ct->kind == TY_FUNC && ct->func &&
                ct->func->type == N_FUNC_LIT)
                return cg_clo_call(g, n, ct->func);
        }

        name = callee->as.var.name;
        s = NULL;
        if (g->cur_ns) {
            /* Inside a module function, a plain call spells the bare name but
             * the multi-file signature table registered it under "<ns>.<name>"
             * (see codegen_emit_ir_multi pass 1); resolve the module key first,
             * then fall back to the bare name (entry functions / builtins). */
            char qname[512];
            snprintf(qname, sizeof qname, "%s.%s", g->cur_ns, name);
            s = sig_find(&g->sigs, qname);
        }
        if (!s) s = sig_find(&g->sigs, name);
        if (!s) {
            /* Only after sig_find: a script that shadows a builtin name with its
             * own function must still call the script's. */
            Val bv = cg_builtin(g, n);
            if (bv.v) return bv;
            ERRV(g, "line %zu: call to unknown function '%s'", n->line, name);
        }
    }
    if (n->as.call.argc != s->arity)
        ERRV(g, "line %zu: '%s' expects %d argument(s), got %d",
            n->line, name, s->arity, n->as.call.argc);

    const char *rty = llvm_type_of(s->ret);
    /* A *named* struct return needs its aggregate defined before the call
     * can be typed; an anonymous one is a heap map (an i8*), which needs
     * nothing — respond() in abac.lume returns a 3-field map literal and the
     * handler serialises it. */
    if (s->ret && s->ret->kind == TY_STRUCT && s->ret->name &&
        !sdef_find(&g->structs, s->ret->name))
        ERRV(g, "line %zu: '%s' returns a struct this backend cannot build", n->line, name);

    /* Evaluate the arguments before emitting anything, so a type error cannot
     * leave a half-written call behind. */
    Val        *args = NULL;
    const char **ats = NULL;
    if (n->as.call.argc > 0) {
        args = (Val *)xmalloc((size_t)n->as.call.argc * sizeof *args);
        ats  = (const char **)xmalloc((size_t)n->as.call.argc * sizeof *ats);
        for (int i = 0; i < n->as.call.argc; i++) {
            /* Pin each argument's expected type to the parameter's, so a
             * struct/tuple literal in argument position resolves to the
             * parameter's declared type — the same rule the type checker
             * applies (ck_expr(arg, callee_t->types[i]) in typecheck_expr.c).
             * Without this an unannotated `(7, 9)` passed to a `T2` parameter
             * would stay an anonymous runtime map while the callee's body
             * reads declared fields off a %T2 aggregate. */
            Type *prev = g->expect;
            g->expect = s->params ? s->params[i] : NULL;
            args[i] = cg_expr(g, n->as.call.args[i]);
            g->expect = prev;
            /* Struct arguments travel as pointers: decay an aggregate value
             * (an insertvalue chain, or a call that returns one) into a stack
             * slot. An address is already what the callee wants. */
            if (args[i].ty && args[i].ty->kind == TY_STRUCT) {
                /* An untyped map value (anonymous struct: an i8* heap map)
                 * cannot be passed to a named struct parameter (a %Point
                 * aggregate): the two representations are incompatible, and
                 * the checker accepts the call on field-compatibility alone.
                 * Reject here with the fix spelled out — binding the value
                 * to the struct type makes the representations line up.
                 * Inline literals never reach this: g->expect pins them to
                 * the parameter type and they are built as aggregates. */
                Type *pt = s->params ? s->params[i] : NULL;
                if (!args[i].ty->name && pt && pt->kind == TY_STRUCT && pt->name)
                    ERRV(g, "line %zu: '%s' expects a '%s' struct, but argument "
                            "%d is an untyped map value — bind it to the struct "
                            "type first (`let v: %s = ...`)", n->line, name,
                         pt->name, i + 1, pt->name);
                struct_addr(g, &args[i]);
            }
            ats[i] = llvm_ptr_type_of(args[i].ty);
            if (!ats[i]) ERRV(g, "line %zu: bad argument type for '%s'", n->line, name);
        }
    }

    char name_res[48];
    snprintf(name_res, sizeof name_res, "%%c%d", g->tid++);

    if (rty) EMIT(g, "  %s = call %s @L_%s(", name_res, rty, name);
    else     EMIT(g, "  call void @L_%s(", name);

    for (int i = 0; i < n->as.call.argc; i++) {
        if (i) EMIT(g, ", ");
        EMIT(g, "%s %s", ats[i], args[i].v);
        free(args[i].v);
    }
    free(args);
    free((void *)ats);
    EMIT(g, ")\n");

    /* A top-level `main()` decides the process status, and the value has to
     * outlive the statement that produced it — hence the capture here rather
     * than a lookup at the end of the body. */
    if (g->capture_main && rty && strcmp(name, "main") == 0) {
        snprintf(g->exit_tmp, sizeof g->exit_tmp, "%s", name_res);
    }

    /* A call to a function with no return type yields null when its result
     * is used as a value (the interpreter's answer); "void" as a value
     * token is not IR — see the same fix in cg_clo_call. */
    if (!rty) return val_make(type_prim(TY_NULL), "null");

    /* `f()?` — the callee handed back a Result; the error has to travel out of
     * *this* function before the payload is read. The interpreter does exactly
     * this (interp.c's propagate branch): look for `err` first, and if it is
     * there return it from the enclosing function without touching `ok`.
     *
     * Both halves matter. Lowering `?` as a plain call would print the whole
     * Result instead of the payload *and* keep running the statements the error
     * was supposed to skip, which is a silent wrong answer rather than a
     * diagnostic.
     *
     * The returned value is the err slot's contents; the enclosing function's
     * own Result return type is what carries it out, and the type checker has
     * already established that this function returns Result.
     */
    if (n->as.call.propagate) {
        Type *payload = s->ret->elem;
        if (!payload || payload->kind == TY_ANY)
            ERRV(g, "line %zu: '?' needs '%s' to return at least one "
                    "{ ok: <value> } whose type is known", n->line, name);
        const char *get_fn = map_get_fn(payload);
        if (!get_fn)
            ERRV(g, "line %zu: '?' cannot unwrap a payload of type '%s'",
                 n->line, src_type_name(payload));

        char has[48], test[48], pay[48];
        snprintf(has,  sizeof has,  "%%c%d", g->tid++);
        snprintf(test, sizeof test, "%%c%d", g->tid++);
        snprintf(pay,  sizeof pay,  "%%c%d", g->tid++);
        int lerr = g->lid++, lok = g->lid++;

        /* if lume_map_has(result, "err") { return result; } -- the err slot's
         * contents are the Result itself here, which is what the enclosing
         * function hands back: `?` moves the failure out, it does not rewrite
         * it. That matches the interpreter, which stores the whole map as
         * vm->call_result and longjmps. */
        /* cg_string_val() returns xstrdup'd memory, so these two are owned and
         * must be freed once emitted. (name_res, next to them, is a stack
         * buffer and must NOT be freed -- mixing the two up is what made an
         * earlier version of this abort with "pointer being freed was not
         * allocated", and then leak 16 bytes under LSan when the free was
         * simply dropped instead.) */
        Val k_err = val_take(type_prim(TY_STRING), cg_string_val(g, "err", 3));
        EMIT(g, "  %s = call i64 @lume_map_has(i8* %s, i8* %s)\n",
             has, name_res, k_err.v);
        free(k_err.v);
        EMIT(g, "  %s = icmp ne i64 %s, 0\n", test, has);
        EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n", test, lerr, lok);
        EMIT(g, "L%d:\n", lerr);
        EMIT(g, "  ret i8* %s\n", name_res);
        EMIT(g, "L%d:\n", lok);

        /* Otherwise the payload, through the accessor its own type picks. An
         * absent `ok` cannot happen on this path -- lume_map_has said the map
         * has no `err` -- but the accessors still take a default, and passing
         * the zero of the right type keeps the call well-typed for all three. */
        Val k_ok = val_take(type_prim(TY_STRING), cg_string_val(g, "ok", 2));
        const char *pty = rt_arg_type(payload);
        const char *darg = strcmp(pty, "i8*") == 0   ? "i8* null"
                         : strcmp(pty, "double") == 0 ? "double 0.0"
                                                      : "i64 0";
        EMIT(g, "  %s = call %s @%s(i8* %s, i8* %s, %s)\n",
             pay, pty, get_fn, name_res, k_ok.v, darg);
        free(k_ok.v);
        return val_make(payload, pay);
    }

    /* A function that returns a struct hands back an aggregate value — the
     * caller's field access has to slot it first, hence agg. */
    return val_make_agg(s->ret, name_res);
}