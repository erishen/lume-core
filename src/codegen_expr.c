/* codegen_expr.c — every expression form the text backend emits.

 * A `Val` is a (type, value-string) pair; nothing here runs. The helpers at
 * the top (val_make / emit_instrf / rt_call) are what the rest of the file and
 * the statement emitters share.
 *
 * Type inference for the sub-expressions lives in codegen_scan.c, and the
 * whole-program signature pass in codegen_sig.c; both are asked for through
 * codegen_internal.h.
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

/* ---------------------------------------------------------------- builtins --- */

/* Lume builtins that lower straight onto a runtime helper (src/rt.c).
 *
 * fn_i/pty_i/rty_i is the integer flavour, fn_f/pty_f/rty_f the float one
 * (NULL when the builtin is not overloaded). The parameter types are listed
 * separately because they are not always the operand types: `int(x)` takes a
 * double and returns an i64, so reusing the operand type there would emit a
 * call whose argument type no callee would accept. */
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
/* forward: same tu, defined below this point. */
static const Bi *builtin_find(const char *name);

/* forward: same tu, defined below this point. */
static Val cg_binary(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static Val cg_builtin(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static Val cg_call(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static Val cg_list_lit(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static Val cg_literal(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static Val cg_map_lit(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static Val cg_map_or_struct_lit(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static Val cg_member(CG *g, Node *n);
static Val cg_index(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static Val cg_print(CG *g, Node *n, Val a);

/* forward: same tu, defined below this point. */
char *cg_string_val(CG *g, const char *text, size_t len);

/* forward: same tu, defined below this point. */
static Val cg_struct_lit(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static Val cg_unary(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static Val cg_var(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static char *emit_instrf_agg(CG *g, const char *fmt, ...);

/* forward: same tu, defined below this point. */
static const char *list_push_fn(Type *ty);

/* forward: same tu, defined below this point. */
static Type *list_type(void);

/* forward: same tu, defined below this point. */
static const char *map_put_fn(Type *ty);

/* forward: same tu, defined below this point. */
static const char *node_type_name(NodeType t);

/* forward: same tu, defined below this point. */
static Type *resolve_struct_lit(CG *g, Node *n, int fatal);

/* forward: same tu, defined below this point. */
static const char *rt_arg_type(Type *ty);

/* forward: same tu, defined below this point. */
Val val_make(Type *ty, const char *v);

/* forward: same tu, defined below this point. */
static Val val_make_agg(Type *ty, const char *v);

/* forward: same tu, defined below this point. */
Val val_take(Type *ty, char *owned);

/* Emit `@.strN` holding text (plus a trailing NUL) and return an i8* value
 * that points at it, via getelementptr — no bitcast needed, and the GEP is
 * folded away by the optimizer. */

char *cg_string_val(CG *g, const char *text, size_t len)
{
    int id = g->sid++;

    /* Escape into a scratch buffer first: the `[N x i8]` size depends on the
     * *escaped* length (a source newline becomes 4 escaped bytes), and the
     * header has to be written before its own payload. */
    IrBuf esc;
    irbuf_init(&esc);
    size_t esclen = irbuf_emit_ir_string(&esc, text, len);
    if (esclen > len * 4)
        ERR(g, "internal: string escape grew too much (%zu > %zu)", esclen, len * 4);

    /* The global belongs to the module-globals section, never to the function
     * body that first mentions it. Text goes through irbuf_puts because it
     * holds `%` (format strings) verbatim and must not hit printf's fmt. */
    char hdr[160];
    snprintf(hdr, sizeof hdr,
             "@.str%d = private unnamed_addr constant [%zu x i8] c\"", id, esclen + 1);
    irbuf_puts(&g->gs, hdr);
    irbuf_puts(&g->gs, esc.data);
    irbuf_puts(&g->gs, "\\00\"\n");
    irbuf_free(&esc);

    char name[48];
    snprintf(name, sizeof name, "%%s%d", id);
    EMIT(g, "  %s = getelementptr inbounds [%zu x i8], [%zu x i8]* @.str%d, i64 0, i64 0\n",
         name, esclen + 1, esclen + 1, id);
    return xstrdup(name);
}

Val val_make(Type *ty, const char *v)
{
    Val r;
    r.ty  = ty;
    r.v   = xstrdup(v);
    r.agg = 0;
    return r;
}

/* Same as val_make, but takes ownership of the string instead of copying it.
 * Every call site below passes a fresh emit_instrf()/cg_string_val() result,
 * so "copy it again" meant an extra allocation nothing ever freed — the copy
 * reached the instruction stream and the original was dropped on the floor. */

Val val_take(Type *ty, char *owned)
{
    Val r;
    r.ty  = ty;
    r.v   = owned;
    r.agg = 0;
    return r;
}

/* An aggregate value of struct type: see Val.agg. */

static Val val_make_agg(Type *ty, const char *v)
{
    Val r = val_make(ty, v);
    r.agg = 1;
    return r;
}

/* Make sure *o* is addressable. A struct value has no address of its own, so
 * it gets a stack slot; an address is left alone — storing it into a slot
 * again would build a `%Point**` and the field would be read off the pointer's
 * bytes. Every field access goes through here, otherwise `f().x` emits
 * `getelementptr inbounds %Point, %Point* %call-result`, which clang rejects
 * with "'%c76' defined with type '%Point = type { i64, i64 }' but expected
 * 'ptr'". */

void struct_addr(CG *g, Val *o)
{
    if (!o->agg || !o->v) return;
    const char *st = llvm_type_of(o->ty);
    if (!st) return;

    char slot[64];
    snprintf(slot, sizeof slot, "%%sa%d", g->tid++);
    EMIT(g, "  %s = alloca %s\n", slot, st);
    EMIT(g, "  store %s %s, %s* %s\n", st, o->v, st, slot);

    free(o->v);
    o->v   = xstrdup(slot);
    o->agg = 0;
}

/* `%tN = <llvm type> <body>`; returns the new name. NULL if type unsupported. */

char *emit_instrf(CG *g, Type *ty, const char *fmt, ...)
{
    const char *lt = llvm_type_of(ty);
    if (!lt) return NULL;

    char name[48];
    snprintf(name, sizeof name, "%%t%d", g->tid++);

    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);

    /* Instruction form is `%r = <opcode> <type> <operands>` — the LLVM type
     * appears exactly once, so `body` carries it and we must not repeat it. */
    EMIT(g, "  %s = %s\n", name, body);
    return xstrdup(name);
}

/* A runtime helper call (src/rt.c). `fmt` is the *argument list* — the caller
 * has no business spelling the callee, and the type of the result comes from
 * `rty`, so the two halves of "call <ret> @fn(<args>)" are both assembled here
 * and stay in step with src/rt.c's headers in one place.
 *
 * `rty` for a list or a map is `i8*`, which is what the runtime hands back: a
 * heap object reached through an opaque pointer. */

Val rt_call(CG *g, Type *rty, const char *fn, const char *fmt, ...)
{
    const char *lt = llvm_type_of(rty);
    if (!lt) return (Val){ NULL, NULL, 0 };

    char args[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(args, sizeof args, fmt, ap);
    va_end(ap);

    char body[640];
    snprintf(body, sizeof body, "call %s @%s(%s)", lt, fn, args);

    char *r = emit_instrf(g, rty, "%s", body);
    if (!r) return (Val){ NULL, NULL, 0 };
    return val_take(rty, r);
}

/* Same as emit_instrf, but for aggregate ops (insertvalue/insertelement): there
 * the operand list starts with a comma, so the type sits *after* the opcode. */

static char *emit_instrf_agg(CG *g, const char *fmt, ...)
{
    char name[48];
    snprintf(name, sizeof name, "%%t%d", g->tid++);

    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);

    EMIT(g, "  %s = %s\n", name, body);
    return xstrdup(name);
}

/* The vendored frontend exports no node-name helper, so keep a local table —
 * it turns "this expression is not supported" into something actionable. */

static const char *node_type_name(NodeType t)
{
    switch (t) {
    case N_PROGRAM: return "program";           case N_BLOCK:  return "block";
    case N_LET:     return "let";               case N_IF:     return "if";
    case N_WHILE:   return "while";             case N_FOR:    return "for";
    case N_BREAK:   return "break";             case N_CONTINUE: return "continue";
    case N_RETURN:  return "return";            case N_EXPR_STMT: return "expression statement";
    case N_SERVER:  return "server";            case N_ROUTE:  return "route";
    case N_TOOL:    return "tool";              case N_VERBS:  return "verbs";
    case N_FUNC_DECL: return "func";            case N_TYPE_DECL: return "type";
    case N_VAR:     return "variable";          case N_ASSIGN: return "assign";
    case N_ASSIGN_MEMBER: return "member assign";              case N_LITERAL: return "literal";
    case N_MAP_LIT: return "struct literal";    case N_LIST_LIT: return "list literal";
    case N_FUNC_LIT: return "closure";          case N_CALL:   return "call";
    case N_MEMBER:  return "field access";      case N_UNARY:  return "unary";
    case N_BINARY:  return "binary";            case N_IMPORT: return "import";
    }
    return "node";
}

const char *bad(CG *g, Node *n, const char *what)
{
    snprintf(g->err, sizeof g->err,
             "line %zu: %s (%s) is not supported by the native backend yet",
             n ? n->line : 0, node_type_name(n->type), what);
    return g->err;
}

static Val cg_literal(CG *g, Node *n)
{
    LitKind k = n->as.lit.kind;

    if (k == LIT_TRUE || k == LIT_FALSE)
        return val_make(type_prim(TY_BOOL), k == LIT_TRUE ? "true" : "false");

    if (k == LIT_STR) {
        /* The lexer hands over the token *with* its quotes; the IR constant
         * must hold the payload only, or strings print as "hi" including the
         * quote characters. */
        const char *t = n->as.lit.text;
        size_t      len = (size_t)n->as.lit.len;
        if (len >= 2 && t[0] == '"' && t[len - 1] == '"') { t++; len -= 2; }
        return val_take(type_prim(TY_STRING), cg_string_val(g, t, len));
    }

    if (k == LIT_NUM) {
        char buf[64];
        if (n->as.lit.is_float) {
            snprintf(buf, sizeof buf, "%.17g", n->as.lit.num);
            /* `16.0` comes out of %g as `16`, and LLVM reads that as an integer
             * constant — `double 16` then fails to parse. A decimal point or an
             * exponent is what makes the value unambiguously a double. */
            if (!strpbrk(buf, ".eE")) strcat(buf, ".0");
            return val_make(type_prim(TY_FLOAT), buf);
        }
        /* The i64 value comes straight from the literal, not from a round trip
         * through double: casting a >2^53 int back to long long has already
         * lost the low bits by this point. */
        snprintf(buf, sizeof buf, "%lld", n->as.lit.inum);
        return val_make(type_prim(TY_INT), buf);
    }

    if (k == LIT_NULL)
        /* `null` lowers to an opaque i8* null pointer in the native backend;
         * the runtime helper prints it as "null" (matching the interpreter). */
        return val_make(type_prim(TY_NULL), "null");

    ERRV(g, "line %zu: 'null' literals are not supported by the native backend yet", n->line);
}

static Val cg_var(CG *g, Node *n)
{
    Asg *a = asg_find(&g->locals, n->as.var.name, n->line);
    /* Not a local: a top-level binding, read through its @lv_ module global.
     * The load spelling is identical (the slot string just starts with '@'),
     * and the semantics are the interpreter's call-time env lookup — the
     * value read is whatever the top level last stored, not a snapshot. */
    if (!a) a = gvar_find(&g->gvars, n->as.var.name);
    if (!a) ERRV(g, "line %zu: unknown variable '%s'", n->line, n->as.var.name);

    const char *lt = llvm_type_of(a->ty);
    if (!lt) ERRV(g, "line %zu: variable '%s' has no codegen type", n->line, n->as.var.name);

    /* A *named* struct is always *referenced*: the slot is its address, and
     * loading it would hand a callee an aggregate where it expects a pointer.
     * An *anonymous* struct is a runtime map: a heap object behind an opaque
     * i8*, so the slot only holds the pointer and must be loaded -- passing
     * %lv_m straight through makes the runtime read a LumeMap out of the
     * stack slot (garbage len, then a wild deref). */
    if (a->ty->kind == TY_STRUCT && a->ty->name)
        return val_make(a->ty, a->slot);

    char *r = emit_instrf(g, a->ty, "load %s, %s* %s", lt, lt, a->slot);
    if (!r) ERRV(g, "line %zu: cannot load '%s'", n->line, n->as.var.name);
    return val_take(a->ty, r);
}

/* --- closures (SPEC 4.8, native) --------------------------------------------
 *
 * A lambda is a value: the mangled `define` the signature pass named is
 * bitcast to i8* and boxed by lume_closure_make into a { i8* fn, i8* cap }
 * record. `cap` is null in this phase — free-variable capture stays
 * interpreter-only — but the record shape and the leading context parameter
 * of every closure define are the contract every call site is spelled
 * against, so capture later widens without changing the ABI. */

/* The fn-pointer type a closure's define is spelled against:
 * `<ret> (i8* cap, <param>...)*`, read off the lambda node the signature
 * pass already typed. */

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

/* A lambda in expression position builds its boxed closure value. */

static Val cg_funclit(CG *g, Node *n)
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

/* Call through a closure value. `clo_v` is the register holding the boxed
 * record; `lambda` supplies the fn pointer's signature; the arguments are
 * already evaluated, coerced and struct-decayed. Borrows everything — the
 * caller frees `args` and owns the returned register string. */

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

/* `f(3)` where f is a variable bound to a lambda. The named-function table
 * has no entry for the variable — the lambda lives under its mangled name —
 * so cg_call routes here before it ever consults sig_find. */

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

/* The lambda behind a HOF's first argument: an inline literal, or a variable
 * bound to one. Same recovery rule the signature pass used to pin the
 * lambda's parameters, so the emitter and the pass can never disagree about
 * which lambda a call site drives. */

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

static Val cg_unary(CG *g, Node *n)
{
    Val o = cg_expr(g, n->as.unary.operand);

    if (n->as.unary.op == OP_NOT) {
        char *r = emit_instrf(g, type_prim(TY_BOOL), "xor i1 %s, true", o.v);
        free(o.v);
        if (!r) ERRV(g, "line %zu: cannot negate this value", n->line);
        return val_take(type_prim(TY_BOOL), r);
    }

    if (!o.ty || (o.ty->kind != TY_INT && o.ty->kind != TY_FLOAT))
        ERRV(g, "line %zu: unary '-' needs a numeric operand", n->line);

    char *r;
    if (o.ty->kind == TY_FLOAT)
        r = emit_instrf(g, o.ty, "fneg double %s", o.v);
    else
        r = emit_instrf(g, o.ty, "sub i64 0, %s", o.v);
    free(o.v);
    if (!r) ERRV(g, "line %zu: cannot negate this value", n->line);
    return val_take(o.ty, r);
}

static Val cg_binary(CG *g, Node *n)
{
    Val a = cg_expr(g, n->as.binary.left);
    Type *boolty = type_prim(TY_BOOL);

    /* `and`/`or` short-circuit, so the right operand must not be emitted yet —
     * `x != 0 and 10 / x > 1` has to skip the division entirely. The result is
     * parked in an alloca instead of a phi, which is what this emitter does
     * for every other local too. */
    if (n->as.binary.op == OP_AND || n->as.binary.op == OP_OR) {
        if (!a.v) ERRV(g, "line %zu: bad logical operands", n->line);
        bool is_and = n->as.binary.op == OP_AND;

        /* Three labels, each written exactly once. AND can only answer `true`
         * from the right operand, OR can only answer `true` from the left one,
         * so the short path stores a constant instead of merely swapping the
         * branch target: `true or 1/0` has to read `true`, never "the value of
         * 1/0", and the right operand is not even emitted on that path.
         * Note the join is a label of its own -- reusing the short-path label
         * for both the fallthrough and the branch back would emit the same
         * block twice and clang rejects it as a terminator in the middle. */
        int lj = g->lid, ls = g->lid + 1, ll = g->lid + 2;
        g->lid += 3;

        char res[64];
        snprintf(res, sizeof res, "%%lb%d", g->tid++);
        /* Same latitude as the for-in index: an alloca anywhere in the
         * function is legal, it only has to dominate the loads of it. */
        EMIT(g, "  %s = alloca i1\n", res);
        EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n",
             a.v, is_and ? ll : ls, is_and ? ls : ll);

        /* short path: the left operand already decides the answer */
        EMIT(g, "L%d:\n", ls);
        EMIT(g, "  store i1 %s, i1* %s\n", is_and ? "false" : "true", res);
        EMIT(g, "  br label %%L%d\n", lj);

        /* long path: evaluate the right operand */
        EMIT(g, "L%d:\n", ll);
        Val b = cg_expr(g, n->as.binary.right);
        if (!b.v) { free(a.v); ERRV(g, "line %zu: bad logical operands", n->line); }
        EMIT(g, "  store i1 %s, i1* %s\n", b.v, res);
        free(b.v);
        EMIT(g, "  br label %%L%d\n", lj);

        EMIT(g, "L%d:\n", lj);
        char *r = emit_instrf(g, boolty, "load i1, i1* %s", res);
        free(a.v);
        if (!r) ERRV(g, "line %zu: bad logical operands", n->line);
        return val_take(boolty, r);
    }

    Val b = cg_expr(g, n->as.binary.right);

    /* Promote both operands to the arithmetic type *before* emitting anything:
     * `x / 2` with x: float must not become `fdiv double %x, 2`, because that
     * bare `2` is an i64 constant and the instruction would be rejected. */
    if (a.ty && b.ty && (a.ty->kind == TY_FLOAT || b.ty->kind == TY_FLOAT)) {
        a = coerce(g, type_prim(TY_FLOAT), a, n->line);
        b = coerce(g, type_prim(TY_FLOAT), b, n->line);
    }

    if (!a.v || !b.v) ERRV(g, "line %zu: bad operands", n->line);

    /* string + string is the only operator strings get, and it has to be a
     * runtime call: IR has no aggregate instruction to concatenate with. */
    if (n->as.binary.op == OP_ADD &&
        a.ty && b.ty && a.ty->kind == TY_STRING && b.ty->kind == TY_STRING) {
        char *r = emit_instrf(g, type_prim(TY_STRING),
                              "call i8* @lume_bi_cat(i8* %s, i8* %s)", a.v, b.v);
        free(a.v); free(b.v);
        if (!r) ERRV(g, "line %zu: string concatenation failed", n->line);
        return val_take(type_prim(TY_STRING), r);
    }

    if (n->as.binary.op >= OP_EQ && n->as.binary.op <= OP_GE) {
        size_t k = (size_t)(n->as.binary.op - OP_EQ);
        const char *icmp[] = {"eq", "ne", "slt", "sle", "sgt", "sge"};
        const char *fcmp[] = {"oeq", "one", "olt", "ole", "ogt", "oge"};
        char *r;
        if (a.ty && a.ty->kind == TY_FLOAT)
            r = emit_instrf(g, boolty, "fcmp %s double %s, %s", fcmp[k], a.v, b.v);
        else
            r = emit_instrf(g, boolty, "icmp %s i64 %s, %s", icmp[k], a.v, b.v);
        free(a.v); free(b.v);
        if (!r) ERRV(g, "line %zu: bad comparison", n->line);
        return val_take(boolty, r);
    }

    /* A string against a number fell through to `add i64 %ptr`, which clang
     * rejects with a line number *inside the generated .ll*. Say where in the
     * source instead: only `+` takes strings, and only against another string. */
    bool astr    = a.ty && a.ty->kind == TY_STRING;
    bool bstr    = b.ty && b.ty->kind == TY_STRING;
    bool numeric = (a.ty && (a.ty->kind == TY_INT || a.ty->kind == TY_FLOAT)) ||
                   (b.ty && (b.ty->kind == TY_INT || b.ty->kind == TY_FLOAT));
    if ((astr || bstr) && numeric)
        ERRV(g, "line %zu: mixing a string with a number is not supported by the "
                "native backend yet", n->line);

    bool is_float = a.ty && a.ty->kind == TY_FLOAT;

    /* `/` is floating-point division even on two integers: the interpreter
     * answers `7 / 2` with 3.5, and an `sdiv` would answer 3. The native
     * backends used to emit the integer division, which is a wrong answer
     * rather than an error, so the operands are widened here instead. */
    bool div_int = !is_float && n->as.binary.op == OP_DIV;
    Type *rty = type_prim((is_float || div_int) ? TY_FLOAT : TY_INT);
    const char *i64 = "i64";
    char *r = NULL;

    if (is_float) {
        switch (n->as.binary.op) {
        case OP_ADD: r = emit_instrf(g, rty, "fadd double %s, %s", a.v, b.v); break;
        case OP_SUB: r = emit_instrf(g, rty, "fsub double %s, %s", a.v, b.v); break;
        case OP_MUL: r = emit_instrf(g, rty, "fmul double %s, %s", a.v, b.v); break;
        case OP_DIV: r = emit_instrf(g, rty, "fdiv double %s, %s", a.v, b.v); break;
        case OP_MOD: ERRV(g, "line %zu: '%%' does not apply to floats", n->line);
        default:     ERRV(g, "line %zu: bad operator", n->line);
        }
    } else {
        switch (n->as.binary.op) {
        case OP_ADD: r = emit_instrf(g, rty, "add %s %s, %s", i64, a.v, b.v); break;
        case OP_SUB: r = emit_instrf(g, rty, "sub %s %s, %s", i64, a.v, b.v); break;
        case OP_MUL: r = emit_instrf(g, rty, "mul %s %s, %s", i64, a.v, b.v); break;
        case OP_DIV: {
            char *fa = emit_instrf(g, rty, "sitofp i64 %s to double", a.v);
            char *fb = emit_instrf(g, rty, "sitofp i64 %s to double", b.v);
            if (!fa || !fb) { free(fa); free(fb); ERRV(g, "line %zu: bad division operands", n->line); }
            r = emit_instrf(g, rty, "fdiv double %s, %s", fa, fb);
            free(fa); free(fb);
            break;
        }
        case OP_MOD: r = emit_instrf(g, rty, "srem %s %s, %s", i64, a.v, b.v); break;
        default:     ERRV(g, "line %zu: bad operator", n->line);
        }
    }
    free(a.v); free(b.v);
    if (!r) ERRV(g, "line %zu: bad arithmetic operands", n->line);
    return val_take(rty, r);
}

int struct_field_idx(CG *g, const char *sname, const char *fname)
{
    SDef *sd = sdef_find(&g->structs, sname);
    if (!sd) return -1;
    for (int i = 0; i < sd->count; i++)
        if (strcmp(sd->names[i], fname) == 0) return i;
    return -1;
}

Type *struct_field_type(CG *g, const char *sname, int idx)
{
    SDef *sd = sdef_find(&g->structs, sname);
    if (sd && idx >= 0 && idx < sd->count) return sd->types[idx];
    return NULL;
}

/* The static type of `expr.field` with nothing emitted — the scanning pass
 * needs it before the loop body is emitted, which is how `for (v in bag.xs)`
 * learns that the field is a list and what the variable `v` has to be. The
 * emitter below shares this instead of re-deriving the type. */

Type *member_field_type(CG *g, Node *n)
{
    Type *t = infer_node_type(g, n->as.member.obj);
    if (t && t->kind == TY_STRUCT && t->name) {
        int idx = struct_field_idx(g, t->name, n->as.member.name);
        if (idx >= 0) return struct_field_type(g, t->name, idx);
    }
    return NULL;
}

/* Lower `req.field` inside a native server handler onto the SrvReq FFI
 * struct (%p0 of the handler's fixed signature). query_params is special:
 * it is not a SrvReq field — cg_call's get() lowering consumes the sentinel
 * Val below. */
static Val cg_handler_req_field(CG *g, Node *n)
{
    const char *f = n->as.member.name;
    if (strcmp(f, "query_params") == 0)
        return val_make(type_prim(TY_STRING), "@__qp__");

    int idx;
    if (strcmp(f, "method") == 0)      idx = 0;
    else if (strcmp(f, "path") == 0)   idx = 1;
    else if (strcmp(f, "query") == 0)  idx = 2;
    else if (strcmp(f, "body") == 0)   idx = 3;
    else if (strcmp(f, "body_len") == 0) idx = 4;
    else ERRV(g, "line %zu: unknown req field '%s' in a native server handler",
              n->line, f);

    Type *st = type_prim(TY_STRING);
    if (idx == 4) {
        char *gt = emit_instrf(g, st,
            "getelementptr %%struct.SrvReq, %%struct.SrvReq* %%p0, i32 0, i32 4");
        char *ld = emit_instrf(g, type_prim(TY_INT),
            "load i64, i64* %s", gt);
        free(gt);
        return val_take(type_prim(TY_INT), ld);
    }
    char *gt = emit_instrf(g, st,
        "getelementptr %%struct.SrvReq, %%struct.SrvReq* %%p0, i32 0, i32 %d", idx);
    char *ld = emit_instrf(g, st, "load i8*, i8** %s", gt);
    free(gt);
    return val_take(st, ld);
}

/* `m["k"]` / `l[0]`.
 *
 * Index first, then container, matching the interpreter's order so a key with a
 * side effect runs before the container is read.
 *
 * The read goes through the unified lume_index_* entry points rather than a
 * per-shape accessor chosen here, because the emitter often cannot tell a list
 * from a map: `m["p"][1]` hands over an opaque i8* and a runtime map's value
 * type is not written down anywhere. The container knows its own kind, so that
 * branch lives in the runtime and this stays a straight-line lowering.
 *
 * Strictness -- a missing key or an out-of-range index stopping the program --
 * has to be a call the emitted program makes. ERRV() cannot do it: it returns
 * from cg_index() and turns g->err into a compile error, which would reject
 * every index expression at build time, including the ones that would have
 * succeeded. The runtime reports and exits non-zero, like every other fatal
 * condition in a compiled program. */
static Val cg_index(CG *g, Node *n)
{
    Val ix  = cg_expr(g, n->as.index.index);
    if (!ix.v) ERRV(g, "line %zu: bad index expression", n->line);
    Val obj = cg_expr(g, n->as.index.obj);
    if (!obj.v) { free(ix.v); ERRV(g, "line %zu: bad index operand", n->line); }

    /* A list knows its element type; a runtime map does not, so the result is
     * the same opaque type a map literal produces and lume_index_ptr decides. */
    int known_map  = obj.ty && obj.ty->kind == TY_STRUCT && !obj.ty->name;
    int known_list = obj.ty && obj.ty->kind == TY_LIST;
    if (!known_map && !known_list) {
        Type *k = obj.ty;
        /* An opaque i8* is what a chained read leaves behind. Let it through --
         * the runtime asks the container which kind it is. */
        int opaque = k && (k->kind == TY_STRUCT || k->kind == TY_LIST);
        if (!opaque) {
            free(ix.v); free(obj.v);
            ERRV(g, "line %zu: cannot index into a value of type '%s'", n->line,
                 k ? src_type_name(k) : "unknown");
        }
    }

    Type *rty = known_list ? (obj.ty->elem ? obj.ty->elem : any_type())
                           : type_anon_struct();
    const char *pty = rt_arg_type(rty);

    const char *fn = strcmp(pty, "double") == 0 ? "lume_index_float"
                  : strcmp(pty, "i8*")   == 0 ? "lume_index_ptr"
                                               : "lume_index_int";
    /* A list index is an i64 and a map key a char*, so both are passed and the
     * runtime uses the one its container needs (see lume_index_int). The index
     * expression was type-checked as an int for a list and a string for a map,
     * so each is already in the right shape here. */
    /* Both key shapes are passed -- an i64 index and a char* key -- and
     * lume_index_* uses whichever its container needs. When the container kind
     * is not statically known (a chained read leaves an opaque i8*) the index
     * expression was type-checked as `any`, so it is passed as an i64: the
     * runtime will not use it, and a pointer-typed constant would not even
     * assemble. `known_list` is the only case where the index is known to be an
     * int, and it is also the only case where the element type can be spelled. */
    /* Both key shapes always go over: an i64 index and a char* key, and
     * lume_index_* uses whichever its container turns out to need. Nothing
     * decides that here, and it cannot:
     *
     *   - a *list* index is statically an int, but
     *   - a *map* read is not statically a map. `{ p: [1, 2] }` has a map value
     *     whose type is nowhere written down, so infer_node_type() answers
     *     "an anonymous struct" -- the map shape -- for `n["p"]` even when what
     *     came back is a list. Committing to the map branch there is what made
     *     `n["p"][1]` emit `i8* 1` and fail to assemble.
     *
     * rt_arg_type says how the index expression itself travels, which is the
     * one thing that *is* knowable here: a string goes in the key slot, anything
     * else in the index slot. */
    const char *ishape = rt_arg_type(ix.ty);
    int ix_is_str = strcmp(ishape, "i8*") == 0;
    char args[224];
    snprintf(args, sizeof args, "i8* %s, %s %s, i8* %s", obj.v,
             ix_is_str ? "i64" : ishape, ix_is_str ? "0" : ix.v,
             ix_is_str ? ix.v : "null");

    Val r = rt_call(g, rty, fn, "%s", args);
    free(ix.v); free(obj.v);
    if (!r.v) ERRV(g, "line %zu: index read failed", n->line);

    /* Strictness, at run time. Every flavour sets the flag when the read did not
     * happen -- including the pointer flavour, where a NULL is either an absent
     * element or a slot holding something the type checker would have rejected,
     * and both are errors rather than values. */
    char flag[48], test[48];
    snprintf(flag, sizeof flag, "%%c%d", g->tid++);
    snprintf(test, sizeof test, "%%c%d", g->tid++);
    EMIT(g, "  %s = call i64 @lume_index_failed()\n", flag);
    EMIT(g, "  %s = icmp ne i64 %s, 0\n", test, flag);
    int lbad = g->lid++, lok = g->lid++;
    EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n", test, lbad, lok);
    EMIT(g, "L%d:\n", lbad);
    /* Which message: a list says "index out of range", a map names the key. The
     * container is still live here, so the runtime can tell -- lume_index_error
     * takes the flag into account through lume_is_map() below. */
    char *what = known_map ? cg_string_val(g, "no such key in this map", 23)
                          : cg_string_val(g, "index out of range", 19);
    EMIT(g, "  call void @lume_index_error(i8* %s)\n", what);
    free(what);
    EMIT(g, "  unreachable\n");
    EMIT(g, "L%d:\n", lok);
    return r;
}

static Val cg_member(CG *g, Node *n)
{
    /* Native server handler: `req.*` lowers onto the FFI arguments instead
     * of the type system (req never enters g->locals). */
    if (g->in_handler && n->as.member.obj &&
        n->as.member.obj->type == N_VAR &&
        strcmp(n->as.member.obj->as.var.name, g->hreq) == 0)
        return cg_handler_req_field(g, n);

    Val obj = cg_expr(g, n->as.member.obj);
    if (!obj.v) ERRV(g, "line %zu: bad struct operand", n->line);

    /* `xs.len` / `xs.length` on a list (or a string) is not a field — it is a
     * runtime length. Checked before the struct path, whose "no such field"
     * message would send you looking for a field that is not declared anywhere. */
    int is_len = strcmp(n->as.member.name, "len") == 0 ||
                 strcmp(n->as.member.name, "length") == 0;
    /* An anonymous struct is a runtime map, which also has no declared field
     * behind `.len` — the same shape as the list case above. */
    int is_map = obj.ty && obj.ty->kind == TY_STRUCT && !obj.ty->name;
    if (is_len && obj.ty && (obj.ty->kind == TY_LIST ||
                             obj.ty->kind == TY_STRING || is_map)) {
        const char *fn = obj.ty->kind == TY_LIST ? "lume_list_len"
                       : is_map               ? "lume_map_len"
                                             : "lume_bi_len_s";
        Val r = rt_call(g, type_prim(TY_INT), fn, "i8* %s", obj.v);
        free(obj.v);
        if (!r.v) { free(r.v); ERRV(g, "line %zu: cannot read '%s'",
                                    n->line, n->as.member.name); }
        return r;
    }

    struct_addr(g, &obj);

    /* The type checker does not always stamp the struct type onto the member
     * node, so fall back to the type of the object expression — that is what
     * makes `r.w` work on a parameter of a struct type. */
    Type *st = n->as.member.type;
    if (!st || st->kind != TY_STRUCT || !st->name) st = obj.ty;
    if (!st || st->kind != TY_STRUCT || !st->name) {
        free(obj.v);
        ERRV(g, "line %zu: field access on a non-struct value", n->line);
    }

    int idx = struct_field_idx(g, st->name, n->as.member.name);
    if (idx < 0) {
        free(obj.v);
        ERRV(g, "line %zu: unknown field '%s' on '%s'", n->line, n->as.member.name, st->name);
    }

    /* llvm_type_of, not st->name: the spelling needs its `%` sigil. */
    const char *stn = llvm_type_of(st);
    if (!stn) { free(obj.v); ERRV(g, "line %zu: cannot access '%s'", n->line, n->as.member.name); }

    /* Two indices: the leading `0` walks the pointer to the struct, the second
     * is the field. Omitting it would turn `1` into an *array* subscript,
     * silently reading offset 16 — the wrong field. */
    char *p = emit_instrf(g, st, "getelementptr inbounds %s, %s* %s, i32 0, i32 %d",
                          stn, stn, obj.v, idx);
    if (!p) { free(obj.v); ERRV(g, "line %zu: cannot access '%s'", n->line, n->as.member.name); }

    Type *fty = struct_field_type(g, st->name, idx);
    if (!fty || !llvm_type_of(fty)) {
        free(obj.v); free(p);
        ERRV(g, "line %zu: field '%s' has no codegen type", n->line, n->as.member.name);
    }

    char *r = emit_instrf(g, fty, "load %s, %s* %s", llvm_type_of(fty), llvm_type_of(fty), p);
    free(obj.v); free(p);
    if (!r) ERRV(g, "line %zu: cannot read '%s'", n->line, n->as.member.name);
    return val_take(fty, r);
}

/* Assignment that shows up where an expression is expected. It emits the store
 * and then hands the assigned value back, so `for (k = 0; ...)` and friends
 * need no special case at the statement level. */

Val cg_assign_expr(CG *g, Node *n)
{
    Val v = cg_expr(g, n->as.assign.value);

    Asg *a = asg_find(&g->locals, n->as.assign.name, n->line);
    if (!a) { free(v.v); ERRV(g, "line %zu: assignment to unknown variable '%s'",
                              n->line, n->as.assign.name); }

    v = coerce(g, a->ty, v, n->line);
    if (g->err[0]) return (Val){ NULL, NULL, 0 };
    const char *lt = llvm_type_of(a->ty);
    if (!lt) { free(v.v); ERRV(g, "line %zu: variable '%s' has no codegen type",
                               n->line, n->as.assign.name); }

    EMIT(g, "  store %s %s, %s* %s\n", lt, v.v, lt, a->slot);
    return v;
}

/* A struct literal carries no type of its own — the parser makes it a map
 * literal — so it is resolved from the context (a `let` annotation, a function
 * return type), and failing that from the declared type whose field set
 * matches the literal exactly.
 *
 * `fatal=0` answers "is this a struct literal at all?" for the ordinary map
 * case (a `{ "k": v }` literal that matches no declared type is a runtime
 * map, not an error). */

static Type *resolve_struct_lit(CG *g, Node *n, int fatal)
{
    Type *e = g->expect;
    if (e && e->kind == TY_STRUCT && e->name && sdef_find(&g->structs, e->name))
        return e;

    Type *hit = NULL;
    for (int i = 0; i < g->structs.n; i++) {
        SDef *sd = &g->structs.v[i];
        if (sd->count != n->as.map.count) continue;
        int all = 1;
        for (int j = 0; j < sd->count; j++) {
            int seen = 0;
            for (int k = 0; k < n->as.map.count; k++)
                if (strcmp(n->as.map.keys[k], sd->names[j]) == 0) { seen = 1; break; }
            if (!seen) { all = 0; break; }
        }
        if (!all) continue;
        if (hit) {
            if (!fatal) return NULL;
            ERR(g, "line %zu: ambiguous struct literal; annotate the type explicitly",
                n->line);
        }
        hit = type_struct(sd->name);
    }
    if (!hit) {
        if (!fatal) return NULL;
        ERR(g, "line %zu: cannot resolve this struct literal to a declared type", n->line);
    }
    return hit;
}

static Val cg_struct_lit(CG *g, Node *n)
{
    Type *ty = resolve_struct_lit(g, n, 1);
    SDef *sd = sdef_find(&g->structs, ty->name);
    if (!sd) ERRV(g, "line %zu: unknown type '%s'", n->line, ty->name);
    if (sd->count != n->as.map.count)
        ERRV(g, "line %zu: struct literal for '%s' has %d field(s), declared %d",
             n->line, ty->name, n->as.map.count, sd->count);

    const char *st = llvm_type_of(ty);

    /* Fill the declared field order, not the source order, so a literal
     * written out of order still lands in the right slot. */
    int   *order = (int *)xmalloc((size_t)sd->count * sizeof *order);
    for (int j = 0; j < sd->count; j++) {
        int idx = -1;
        for (int i = 0; i < n->as.map.count; i++)
            if (strcmp(n->as.map.keys[i], sd->names[j]) == 0) { idx = i; break; }
        if (idx < 0) { free(order); ERRV(g, "line %zu: '%s' is missing field '%s'",
                                          n->line, ty->name, sd->names[j]); }
        order[j] = idx;
    }

    char *cur = xstrdup("undef");
    for (int j = 0; j < sd->count; j++) {
        Val fv = cg_expr(g, n->as.map.vals[order[j]]);
        Type *fty = sd->types[j];
        if (!llvm_type_of(fty)) { free(cur); free(fv.v); free(order);
            ERRV(g, "line %zu: field '%s' has no codegen type", n->line, sd->names[j]); }
        fv = coerce(g, fty, fv, n->line);
        if (g->err[0]) { free(cur); free(fv.v); free(order); return (Val){ NULL, NULL, 0 }; }

        char *ins = emit_instrf_agg(g, "insertvalue %s %s, %s %s, %d",
                                    st, cur, llvm_type_of(fty), fv.v, j);
        free(cur); free(fv.v);
        if (!ins) {
            free(order);
            ERRV(g, "line %zu: cannot build a value of type '%s'", n->line, ty->name);
        }
        cur = ins;
    }
    free(order);
    /* cur is the last emit_instrf_agg() result, and val_make_agg() copies it.
     * Both ends have to be released here: nothing after this point holds cur,
     * and every intermediate value along the way was already freed above. */
    Val r = val_make_agg(ty, cur);
    free(cur);
    return r;
}

/* How a value is spelled once it is handed to a runtime helper: a float goes
 * as double, a string as i8* (a constant, too — IR will not accept a bare
 * `[2 x i8]*`), and every scalar as i64.
 *
 * One helper instead of a `? "double" : "i64"` at every call site, which is
 * how a string argument reached `@lume_list_push_s(i8*, i64)` — invalid IR for
 * the one element type that is not a scalar. */

static const char *rt_arg_type(Type *ty)
{
    if (!ty) return "i64";
    switch (ty->kind) {
    case TY_FLOAT:  return "double";
    case TY_STRING: return "i8*";
    /* A nested container travels as the same opaque pointer a list or a map
     * itself does. Without this the call site spelled the argument i64 while
     * the helper is declared i8*, and clang rejected the IR with "%t1 defined
     * with type 'ptr' but expected 'i64'" -- the container was being built
     * correctly and then mis-typed on the way into the runtime. */
    case TY_LIST:
    case TY_STRUCT:
    case TY_RESULT: return "i8*";
    default:        return "i64";
    }
}

/* Which getter reads a map value of static type `ty` -- the same choice
 * map_put_fn makes on the way in, read back for `?` unwrapping. */
static const char *map_get_fn(Type *ty)
{
    if (!ty) return NULL;
    switch (ty->kind) {
    case TY_FLOAT:  return "lume_map_get_f";
    case TY_STRING: return "lume_map_get_s";
    case TY_INT:
    case TY_BOOL:   return "lume_map_get_i";
    case TY_LIST:
    case TY_STRUCT:
    case TY_RESULT: return "lume_map_get_obj";
    default:        return NULL;
    }
}


/* Whichpush helper an element of static type `ty` needs. The checker keeps a
 * list homogeneous, so one answer serves every element. */

static const char *list_push_fn(Type *ty)
{
    if (!ty) return NULL;
    switch (ty->kind) {
    case TY_FLOAT:  return "lume_list_push_f";
    case TY_STRING: return "lume_list_push_s";
    case TY_INT:
    case TY_BOOL:   return "lume_list_push_i";
    /* A nested container travels as the same opaque i8*, so it needs the same
     * argument spelling as the others -- only the slot tag differs, and that
     * is the helper's business. This is what makes `[[1,2],[3]]` buildable. */
    case TY_LIST:
    case TY_STRUCT:
    case TY_RESULT: return "lume_list_push_obj";
    default:        return NULL;
    }
}

/* Which map helper stores a value of static type `ty`. */

static const char *map_put_fn(Type *ty)
{
    if (!ty) return NULL;
    switch (ty->kind) {
    case TY_FLOAT:  return "lume_map_put_f";
    case TY_STRING: return "lume_map_put_s";
    case TY_INT:
    case TY_BOOL:   return "lume_map_put_i";
    /* Containers as values, same reasoning as list_push_fn: this is what makes
     * `{ a: { b: 1 } }` buildable. */
    case TY_LIST:
    case TY_STRUCT:
    case TY_RESULT: return "lume_map_put_obj";
    default:        return NULL;
    }
}

/* The element type of a list literal, read off its own elements. Only ever
 * asked about a list *literal*: a value is just an `i8*`, and what a for-in has
 * to know is which reader to call, which is a static question.
 *
 * Everything has to agree — a heterogeneous literal such as `[1.5, 2]` yields
 * `list<any>`, and iterating that natively would read one slot through the
 * wrong accessor and print garbage, so it is reported as unknown instead. */

Type *infer_list_elem(Node *lit)
{
    if (!lit || lit->type != N_LIST_LIT) return NULL;

    Type *et = NULL;
    for (int i = 0; i < lit->as.list.count; i++) {
        Node *e = lit->as.list.items[i];
        Type *t = NULL;

        if (e->type == N_LITERAL) {
            switch (e->as.lit.kind) {
            case LIT_NUM:   t = type_prim(e->as.lit.is_float ? TY_FLOAT : TY_INT); break;
            case LIT_STR:   t = type_prim(TY_STRING); break;
            case LIT_TRUE:
            case LIT_FALSE: t = type_prim(TY_BOOL); break;
            default: break;
            }
        }
        /* Anything else (a call, another list, ...) is a kind this emitter
         * cannot read statically. */
        if (!t) return NULL;
        if (et && et->kind != t->kind) return NULL;
        et = t;
    }
    return et;   /* `[]` has no element kind; it iterates zero times anyway */
}

/* A list value's own type: only the TY_LIST kind matters to the backend
 * (llvm_type_of spells every list as i8*), so with no literal in hand the
 * element is ANY. */

static Type *list_type(void) { return type_list(infer_list_elem(NULL)); }

/* `[a, b, c]` — a heap list, built by pushing every element in source order
 * so iteration and the interpreter's ordering agree. */

static Val cg_list_lit(CG *g, Node *n)
{
    /* The format is the *argument list*: a zero-argument helper takes an empty
     * one, not a spelled-out "i8* @lume_list_new()". */
    Val l = rt_call(g, list_type(), "lume_list_new", "");
    if (!l.v) ERRV(g, "line %zu: cannot allocate a list", n->line);

    for (int i = 0; i < n->as.list.count; i++) {
        Val e = cg_expr(g, n->as.list.items[i]);
        const char *fn = list_push_fn(e.ty);
        if (!fn) { free(e.v); free(l.v); ERRV(g, "line %zu: list elements must be int, float, string or a container",
                                              n->line); }
        if (e.ty && e.ty->kind == TY_BOOL) e = coerce(g, type_prim(TY_INT), e, n->line);
        Val r = rt_call(g, type_prim(TY_INT), fn, "i8* %s, %s %s", l.v,
                        rt_arg_type(e.ty), e.v);
        free(e.v);
        if (!r.v) { free(l.v); ERRV(g, "line %zu: cannot append to a list", n->line); }
        free(r.v);
    }
    return l;
}

/* A map literal that is not a struct literal: a heap map keyed by its source
 * keys. Value helpers are chosen the same way as for a list. */

static Val cg_map_lit(CG *g, Node *n)
{
    Val m = rt_call(g, list_type(), "lume_map_new", "");
    if (!m.v) ERRV(g, "line %zu: cannot allocate a map", n->line);
    m.ty = type_anon_struct();

    for (int i = 0; i < n->as.map.count; i++) {
        Val v = cg_expr(g, n->as.map.vals[i]);
        Val k = val_take(type_prim(TY_STRING),
                         cg_string_val(g, n->as.map.keys[i], strlen(n->as.map.keys[i])));
        const char *fn = map_put_fn(v.ty);
        if (!fn) { free(k.v); free(v.v); ERRV(g, "line %zu: map values must be int, float, string or a container",
                                              n->line); }
        if (v.ty && v.ty->kind == TY_BOOL) v = coerce(g, type_prim(TY_INT), v, n->line);
        Val r = rt_call(g, type_prim(TY_INT), fn, "i8* %s, i8* %s, %s %s", m.v, k.v,
                        rt_arg_type(v.ty), v.v);
        free(k.v); free(v.v);
        if (!r.v) { free(m.v); ERRV(g, "line %zu: cannot store into a map", n->line); }
        free(r.v);
    }
    /* The map is a heap object like a list; the type is only what makes the
     * value's provenance (and any later dispatch) readable. */
    return m;
}

/* A map literal that no declared type claimed is a runtime map; one that did
 * is a struct value (see cg_struct_lit). */

static Val cg_map_or_struct_lit(CG *g, Node *n)
{
    if (resolve_struct_lit(g, n, 0)) return cg_struct_lit(g, n);
    return cg_map_lit(g, n);
}

static Val cg_print(CG *g, Node *n, Val a)
{
    /* print() lowers to a non-variadic runtime helper (src/rt.c), never to
     * printf. On macOS/arm64 the callee's va_start reads an argument save area
     * that the *caller* must build, which hand-written IR cannot express:
     * leaving the value in x1 prints whatever the stack happens to hold.
     * A fixed printf prototype does not help either.
     *
     * The helpers take i64 for int/bool and double for float, so the i1 has to
     * be widened first — the helper would otherwise read the wrong register. */
    const char *fn;      /* bare helper name; rt.c defines it */
    const char *pty;     /* the helper's *parameter* type */
    char *arg = a.v;
    bool   widen = false;

    /* `null` needs no argument and prints the bare word "null" — it cannot go
     * through the switch below, which always emits a typed argument. `arg` is
     * owned by this call (see the `free(arg)` at the bottom), so free it here
     * too to avoid leaking the literal's IR buffer. */
    if (a.ty && a.ty->kind == TY_NULL) {
        char name[48];
        snprintf(name, sizeof name, "%%c%d", g->tid++);
        EMIT(g, "  %s = call i64 @lume_print_null()\n", name);
        free(arg);
        return val_make(type_prim(TY_INT), name);
    }

    /* One switch decides both: which helper prints this value and which type
     * that helper takes — they are not always the same (a bool value is i1,
     * lume_print_bool takes i64; a list value is i8*, lume_list_print takes
     * i8*). Getting the parameter wrong is a type mismatch in the call. */
    switch (a.ty ? a.ty->kind : TY_ANY) {
    case TY_INT:    fn = "lume_print_i64";    pty = "i64";   break;
    case TY_FLOAT:  fn = "lume_print_double"; pty = "double"; break;
    case TY_BOOL:   fn = "lume_print_bool";   pty = "i64";   widen = true; break;
    case TY_STRING: fn = "lume_print_str";    pty = "i8*";   break;
    case TY_LIST:   fn = "lume_list_print";   pty = "i8*";   break;
    /* An unnamed struct is what the checker gives a map literal, so `print(m)`
     * goes to the map helper; a *named* struct is still a struct value. */
    case TY_STRUCT:
        if (!a.ty->name) { fn = "lume_map_print"; pty = "i8*"; break; }
        free(arg);
        ERRV(g, "line %zu: print() cannot print a struct value", n->line);
    /* A Result *is* a map ({ ok: ... } / { err: ... }), so it prints through
     * the same runtime helper. Without this it fell to the default arm and the
     * interpreter could print a Result that the emitters refused to build. */
    case TY_RESULT: fn = "lume_map_print"; pty = "i8*"; break;
    default:
        ERRV(g, "line %zu: print() cannot print this value", n->line);
    }

    if (widen) {
        char *z = emit_instrf(g, type_prim(TY_INT), "zext i1 %s to i64", a.v);
        if (z) { free(a.v); arg = z; }
    }

    char name[48];
    snprintf(name, sizeof name, "%%c%d", g->tid++);
    EMIT(g, "  %s = call i64 @%s(%s %s)\n", name, fn, pty, arg);

    free(arg);
    /* name is a stack buffer and val_make() copies it: wrapping it in an
     * xstrdup of its own stranded one block per print() call, since nothing
     * ever held the outer copy. */
    return val_make(type_prim(TY_INT), name);
}

static const Bi *builtin_find(const char *name)
{
    for (size_t i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i++)
        if (strcmp(BUILTINS[i].name, name) == 0) return &BUILTINS[i];
    return NULL;
}

/* Declarations for every builtin helper, so the IR shows the full surface the
 * backend can call. Unused declarations are legal and cost nothing after
 * linking, and having the list in one place keeps cg_call free of typing. */

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
    EMIT(g, "declare i64 @lume_list_push_f(i8*, double)\n");
    EMIT(g, "declare i64 @lume_list_push_s(i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_list_push_obj(i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_list_at_i(i8*, i64)\n");
    EMIT(g, "declare double @lume_list_at_f(i8*, i64)\n");
    EMIT(g, "declare i8* @lume_list_at_s(i8*, i64)\n");
    EMIT(g, "declare i8* @lume_list_at_obj(i8*, i64)\n");
    EMIT(g, "declare i64 @lume_list_print(i8*)\n");

    EMIT(g, "declare i8* @lume_map_new()\n");
    EMIT(g, "declare i64 @lume_map_len(i8*)\n");
    EMIT(g, "declare i64 @lume_map_put_i(i8*, i8*, i64)\n");
    EMIT(g, "declare i64 @lume_map_put_f(i8*, i8*, double)\n");
    EMIT(g, "declare i64 @lume_map_put_s(i8*, i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_map_put_obj(i8*, i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_map_get_i(i8*, i8*, i64)\n");
    EMIT(g, "declare double @lume_map_get_f(i8*, i8*, double)\n");
    EMIT(g, "declare i8* @lume_map_get_s(i8*, i8*, i8*)\n");
    EMIT(g, "declare i8* @lume_map_get_obj(i8*, i8*, i8*)\n");
    EMIT(g, "declare i64 @lume_map_has(i8*, i8*)\n");
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

/* Lower a builtin call onto a runtime helper. Returns a value with .v set when
 * the name is a builtin this backend knows, an empty Val when it is something
 * else (so the caller can fall through to the "unknown function" error).
 * Returning Val rather than a flag keeps the ERRV macro usable below — it is
 * defined with a `return` of the result type baked in. */

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
        strcmp(bname, "float") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: '%s' takes exactly one argument", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        const char *fn = NULL, *pty = NULL, *rty = NULL;
        Type *rt = NULL;

        if (strcmp(bname, "str") == 0) {
            switch (a.ty ? a.ty->kind : TY_ANY) {
            case TY_INT:    fn = "@lume_bi_str_i64";    pty = "i64";    break;
            case TY_FLOAT:  fn = "@lume_bi_str_double"; pty = "double"; break;
            case TY_BOOL:   fn = "@lume_bi_str_bool";   pty = "i64";    break;
            case TY_STRING: return a;   /* str of a string is itself */
            default:
                ERRV(g, "line %zu: str() cannot convert this value", n->line);
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

static Val cg_call(CG *g, Node *n)
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

Val cg_expr(CG *g, Node *n)
{
    if (!n) ERRV(g, "internal: null expression");

    switch (n->type) {
    case N_LITERAL: return cg_literal(g, n);
    case N_LIST_LIT: return cg_list_lit(g, n);
    case N_MAP_LIT: return cg_map_or_struct_lit(g, n);
    /* An assignment in expression position — a C-style `for` header wraps it
     * in an expression statement, and the parser hands those to expressions. */
    case N_ASSIGN:  return cg_assign_expr(g, n);
    case N_ASSIGN_MEMBER: return cg_assign_mem(g, n);
    case N_EXPR_STMT: return cg_expr(g, n->as.expr_stmt.expr);
    case N_VAR:     return cg_var(g, n);
    case N_FUNC_LIT: return cg_funclit(g, n);
    case N_UNARY:   return cg_unary(g, n);
    case N_BINARY:  return cg_binary(g, n);
    case N_MEMBER:  return cg_member(g, n);
    case N_INDEX:   return cg_index(g, n);
    case N_CALL:    return cg_call(g, n);
    default:        ERRV(g, "%s", bad(g, n, "this expression"));
    }
}

/* Convert a value to the declared type of a local. Needed because an
 * initializer is not always written in the target type: `let x: float = 9`
 * stores an i64 into a double slot, which is invalid IR if left alone. */

Val coerce(CG *g, Type *to, Val v, size_t line)
{
    if (!to || !v.ty || to == v.ty) return v;

    /* `null` is an opaque i8* null pointer natively, and the type checker lets
     * it stand in for any type ("all types nullable"). Pointer-shaped targets
     * stay legal; a scalar target has no null, and the IR text below has no
     * edge that could spell one -- so reject it with the same message the
     * libLLVM backend uses, rather than letting the two backends disagree
     * about whether `let x: int = null` compiles. */
    if (v.ty && v.ty->kind == TY_NULL && to && to->kind != TY_STRING &&
        to->kind != TY_LIST && to->kind != TY_STRUCT && to->kind != TY_ANY) {
        free(v.v);
        ERRV(g, "line %zu: cannot use 'null' as a %s value in the native backend",
             line, src_type_name(to));
    }

    const char *lt = llvm_type_of(v.ty);
    const char *tt = llvm_type_of(to);
    if (!lt || !tt || strcmp(lt, tt) == 0) return v;

    char *r = NULL;
    if (strcmp(lt, "i1") == 0 && strcmp(tt, "i64") == 0)
        r = emit_instrf(g, to, "zext i1 %s to i64", v.v);
    else if (strcmp(lt, "i64") == 0 && strcmp(tt, "i1") == 0)
        r = emit_instrf(g, to, "icmp ne i64 %s, 0", v.v);
    else if (strcmp(lt, "i64") == 0 && strcmp(tt, "double") == 0)
        r = emit_instrf(g, to, "sitofp i64 %s to double", v.v);
    else if (strcmp(lt, "double") == 0 && strcmp(tt, "i64") == 0)
        r = emit_instrf(g, to, "fptosi double %s to i64", v.v);

    if (!r) { free(v.v); ERRV(g, "line %zu: cannot convert %s to %s", line, lt, tt); }
    free(v.v);
    return val_take(to, r);
}
