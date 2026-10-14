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
    { "now",   "lume_bi_now",  NULL,     "i8*",    0,  NULL,           NULL,         NULL,     0 },
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
            args[i] = cg_expr(g, n->as.call.args[i]);
            if (args[i].ty && args[i].ty->kind == TY_STRUCT)
                struct_addr(g, &args[i]);
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
    if (!rty) { free(res); return val_make(type_prim(TY_NULL), "void"); }
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

    EMIT(g, "declare i8* @lume_map_new()\n");
    EMIT(g, "declare i64 @lume_map_len(i8*)\n");
    EMIT(g, "declare i64 @lume_map_put_i(i8*, i8*, i64)\n");
    EMIT(g, "declare i64 @lume_map_put_b(i8*, i8*, i64)\n");
    EMIT(g, "declare i64 @lume_map_put_f(i8*, i8*, double)\n");
    EMIT(g, "declare i64 @lume_map_put_s(i8*, i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_map_put_obj(i8*, i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_map_clone(i8*)\n");
    EMIT(g, "declare i64 @lume_map_get_i(i8*, i8*, i64)\n");
    EMIT(g, "declare double @lume_map_get_f(i8*, i8*, double)\n");
    EMIT(g, "declare i8* @lume_map_get_s(i8*, i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_map_get_obj(i8*, i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_map_has(i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_str_eq(i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_value_eq(i8*, i8*)\n");   /* structural, SPEC 1.8 */
    EMIT(g, "declare i64 @lume_any_eq(i8*, i8*)\n");     /* any vs i8*-lowered */
    /* Index syntax (SPEC 6.2): one entry point per result shape, each deciding
     * list-vs-map at run time because the emitter cannot always tell. */
    EMIT(g, "declare i64 @lume_index_int(i8*, i64, i8*)\n");
    EMIT(g, "declare double @lume_index_float(i8*, i64, i8*)\n");
    EMIT(g, "declare i8* @lume_index_ptr(i8*, i64, i8*)\n");
    EMIT(g, "declare i64 @lume_index_failed()\n");
    EMIT(g, "declare void @lume_index_error(i8*)\n");
    EMIT(g, "declare i8* @lume_map_key_at(i8*, i64)\n");
    EMIT(g, "declare i8* @lume_map_keys(i8*)\n");
    EMIT(g, "declare i64 @lume_map_print(i8*)\n");

    /* Closures (SPEC 4.8): the box a lambda literal builds and every call
     * site unwraps. The record layout lives in src/rt.c (LumeClosure). */
    EMIT(g, "; closures (src/rt.c)\n");
    EMIT(g, "declare i8* @lume_closure_make(i8*, i8*)\n");

    /* Native server runtime (src/bridge_native.c): the FFI surface that
     * server{}/route handlers/run() lower onto. SrvReq's layout is a
     * contract with bridge_native.c (four pointers then one i64). */
    EMIT(g, "; native server runtime (src/bridge_native.c)\n");
    EMIT(g, "%%struct.SrvReq = type { i8*, i8*, i8*, i8*, i64 }\n");
    EMIT(g, "declare void @lume_srv_init(i8*, i64)\n");
    EMIT(g, "declare void @lume_srv_route(i8*, i8*, void (%%struct.SrvReq*, i8**, i64*, i32*)*)\n");
    EMIT(g, "declare void @lume_srv_run()\n");
    EMIT(g, "declare i8* @lume_srv_qp(i8*, i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_srv_atoi(i8*)\n");
    EMIT(g, "declare void @lume_srv_json_begin(i8**, i64*)\n");
    EMIT(g, "declare void @lume_srv_json_key(i8**, i64*, i8*)\n");
    EMIT(g, "declare void @lume_srv_json_str(i8**, i64*, i8*)\n");
    EMIT(g, "declare void @lume_srv_json_int(i8**, i64*, i64)\n");
    EMIT(g, "declare void @lume_srv_json_bool(i8**, i64*, i32)\n");
    EMIT(g, "declare void @lume_srv_json_end(i8**, i64*)\n");
}

static Val cg_builtin(CG *g, Node *n)
{
    const char *bname = n->as.call.callee->as.var.name;
    int argc = n->as.call.argc;
    const Bi *b = builtin_find(bname);

    /* now(): current local time as a string (no arguments, so the table
     * path below — which requires argc >= 1 — cannot serve it). */
    if (strcmp(bname, "now") == 0) {
        if (argc != 0)
            ERRV(g, "line %zu: now() takes no arguments", n->line);
        return val_take(type_prim(TY_STRING),
                        emit_instrf(g, type_prim(TY_STRING),
                                    "call i8* @lume_bi_now()"));
    }

    if (b) {
        if (argc < 1)
            ERRV(g, "line %zu: '%s' takes at least one argument", n->line, bname);
        Val *av = (Val *)xmalloc((size_t)argc * sizeof *av);
        for (int i = 0; i < argc; i++) av[i] = cg_expr(g, n->as.call.args[i]);

        int isf = av[0].ty && av[0].ty->kind == TY_FLOAT && b->fn_f;
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

    /* len() — a list answers with its length, a string with its byte length.
     * The helper is picked from the static type, so a list and a string both
     * spell `len(x)` and still reach the right one. */
    if (strcmp(bname, "len") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: len() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || (a.ty->kind != TY_LIST && a.ty->kind != TY_STRING))
            ERRV(g, "line %zu: len() needs a list or a string", n->line);
        Val r = rt_call(g, type_prim(TY_INT),
                        a.ty->kind == TY_LIST ? "lume_list_len" : "lume_bi_len_s",
                        "i8* %s", a.v);
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

    /* get(m, k[, default]) — an absent key yields the default, which is what
     * the interpreter's three-argument form means. */
    if (strcmp(bname, "get") == 0) {
        if (argc < 2 || argc > 3)
            ERRV(g, "line %zu: get() takes two or three arguments", n->line);
        Val m = cg_expr(g, n->as.call.args[0]);
        Val k = cg_expr(g, n->as.call.args[1]);
        Val d = argc == 3 ? cg_expr(g, n->as.call.args[2])
                          : (Val){ NULL, NULL, 0 };
        if (!m.v || !k.v)
            ERRV(g, "line %zu: get() needs a map, a key and a scalar default", n->line);

        /* The flavour follows the *default*, and a key that is absent reads as
         * that default — so an unannotated get(m, k) in a bool context stays
         * an int read rather than silently becoming a string. */
        const char *fn = "lume_map_get_i";
        Type *rty = type_prim(TY_INT);
        if (d.v && d.ty && d.ty->kind == TY_FLOAT)       { fn = "lume_map_get_f"; rty = type_prim(TY_FLOAT); }
        else if (d.v && d.ty && d.ty->kind == TY_STRING) { fn = "lume_map_get_s"; rty = type_prim(TY_STRING); }
        /* A container default (`get(m, k, { x: 1 })`, and the shape a nested
         * read takes once the checker knows a container is there) needs the
         * pointer-returning getter. Without this branch a container came back
         * through lume_map_get_i as the integer in its slot field, which
         * clang then rejected because the call was spelled i64 against an
         * i8* argument. */
        else if (d.v && d.ty && (d.ty->kind == TY_LIST ||
                                  d.ty->kind == TY_STRUCT ||
                                  d.ty->kind == TY_RESULT))
                                      { fn = "lume_map_get_obj"; rty = type_anon_struct(); }
        /* A null default reads through the obj getter too: the result is an
         * i8* that is null both for a null slot and (as with every obj read)
         * for an absent key, and `i8* null` is the argument spelling. */
        else if (d.v && d.ty && d.ty->kind == TY_NULL)
                                      { fn = "lume_map_get_obj"; rty = type_anon_struct(); }

        char dflt[96];
        const char *darg = "i64 0";
        if (d.v) {
            if (d.ty->kind == TY_BOOL) d = coerce(g, type_prim(TY_INT), d, n->line);
            snprintf(dflt, sizeof dflt, "%s %s", rt_arg_type(d.ty), d.v);
            darg = dflt;
            free(d.v);
        }
        Val r = rt_call(g, rty, fn, "i8* %s, i8* %s, %s", m.v, k.v, darg);
        free(m.v); free(k.v);
        if (!r.v) ERRV(g, "line %zu: get() failed", n->line);
        return r;
    }

    /* str() / int() / float(): which helper they need depends on the operand. */
    if (strcmp(bname, "str") == 0 || strcmp(bname, "int") == 0 ||
        strcmp(bname, "float") == 0 || strcmp(bname, "stringify") == 0 ||
        strcmp(bname, "json") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: '%s' takes exactly one argument", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        const char *fn = NULL, *pty = NULL, *rty = NULL;
        Type *rt = NULL;
        bool is_str = strcmp(bname, "str") == 0;
        bool is_json = strcmp(bname, "json") == 0;

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
                if (is_json)
                    ERRV(g, "line %zu: json() string parsing is interpreter-only;"
                            " the native backends cannot type the dynamic result",
                         n->line);
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
            if (!a.ty || a.ty->kind != TY_FLOAT)
                ERRV(g, "line %zu: int() only converts a float here", n->line);
            fn = "@lume_bi_int"; pty = "double"; rty = "i64";
            rt = type_prim(TY_INT);
        } else {
            if (!a.ty || a.ty->kind != TY_INT)
                ERRV(g, "line %zu: float() only converts an int here", n->line);
            fn = "@lume_bi_float"; pty = "i64"; rty = "double";
            rt = type_prim(TY_FLOAT);
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
        return val_make(type_prim(TY_NULL), "void");
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

    if (!n->as.call.callee || n->as.call.callee->type != N_VAR)
        ERRV(g, "line %zu: only direct calls to named functions are supported", n->line);

    /* A closure-valued variable: `f(3)` where f was bound to a lambda. The
     * variable's TY_FUNC type carries the back-pointer to the lambda node
     * the signature pass pinned; a plain named-function reference has no
     * back-pointer and falls through to the sig_find path below. */
    {
        Type *ct = infer_node_type(g, n->as.call.callee);
        if (ct && ct->kind == TY_FUNC && ct->func &&
            ct->func->type == N_FUNC_LIT)
            return cg_clo_call(g, n, ct->func);
    }

    const char *name = n->as.call.callee->as.var.name;
    Sig *s = sig_find(&g->sigs, name);
    if (!s) {
        /* Only after sig_find: a script that shadows a builtin name with its
         * own function must still call the script's. */
        Val bv = cg_builtin(g, n);
        if (bv.v) return bv;
        ERRV(g, "line %zu: call to unknown function '%s'", n->line, name);
    }
    if (n->as.call.argc != s->arity)
        ERRV(g, "line %zu: '%s' expects %d argument(s), got %d",
            n->line, name, s->arity, n->as.call.argc);

    const char *rty = llvm_type_of(s->ret);
    if (s->ret && s->ret->kind == TY_STRUCT &&
        (!s->ret->name || !sdef_find(&g->structs, s->ret->name)))
        ERRV(g, "line %zu: '%s' returns a struct this backend cannot build", n->line, name);

    /* Evaluate the arguments before emitting anything, so a type error cannot
     * leave a half-written call behind. */
    Val        *args = NULL;
    const char **ats = NULL;
    if (n->as.call.argc > 0) {
        args = (Val *)xmalloc((size_t)n->as.call.argc * sizeof *args);
        ats  = (const char **)xmalloc((size_t)n->as.call.argc * sizeof *ats);
        for (int i = 0; i < n->as.call.argc; i++) {
            args[i] = cg_expr(g, n->as.call.args[i]);
            /* Struct arguments travel as pointers: decay an aggregate value
             * (an insertvalue chain, or a call that returns one) into a stack
             * slot. An address is already what the callee wants. */
            if (args[i].ty && args[i].ty->kind == TY_STRUCT)
                struct_addr(g, &args[i]);
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

    if (!rty) return val_make(s->ret, "void");

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
