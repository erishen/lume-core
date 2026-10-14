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

static Val cg_binary(CG *g, Node *n);
static char *cg_val_eq(CG *g, Val a, Val b, bool ne, size_t line);
static char *cg_struct_eq(CG *g, Val a, Val b, bool ne, size_t line);
static Val cg_literal(CG *g, Node *n);
static Val cg_member(CG *g, Node *n);
static Val cg_index(CG *g, Node *n);
char *cg_string_val(CG *g, const char *text, size_t len);
static Val cg_unary(CG *g, Node *n);
static Val cg_var(CG *g, Node *n);
char *emit_instrf_agg(CG *g, const char *fmt, ...);
static const char *node_type_name(NodeType t);
Val val_make(Type *ty, const char *v);
Val val_make_agg(Type *ty, const char *v);
Val val_take(Type *ty, char *owned);

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

Val val_take(Type *ty, char *owned)
{
    Val r;
    r.ty  = ty;
    r.v   = owned;
    r.agg = 0;
    return r;
}

Val val_make_agg(Type *ty, const char *v)
{
    Val r = val_make(ty, v);
    r.agg = 1;
    return r;
}

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

char *emit_instrf_agg(CG *g, const char *fmt, ...)
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
    case N_ENUM_DECL: return "enum";
    case N_VAR:     return "variable";          case N_ASSIGN: return "assign";
    case N_ASSIGN_MEMBER: return "member assign";              case N_LITERAL: return "literal";
    case N_MAP_LIT: return "struct literal";    case N_LIST_LIT: return "list literal";
    case N_FUNC_LIT: return "closure";          case N_CALL:   return "call";
    case N_MEMBER:  return "field access";      case N_UNARY:  return "unary";
    case N_BINARY:  return "binary";            case N_TERNARY: return "ternary";
    case N_MATCH:   return "match";             case N_CTOR:   return "constructor";
    case N_INDEX:   return "index";             case N_IMPORT: return "import";
    case N_IF_LET: return "if-let";
    case N_STRUCT_UPDATE: return "struct update";
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
        /* Language-level escapes are resolved here, once, through the shared
         * table (value.c). irbuf_emit_ir_string only escapes for the IR's own
         * syntax, so without this pass `"a\nb"` emitted four characters while
         * the interpreter printed a newline — a three-way split only the
         * interpreter-only builtins-test could have caught. */
        size_t ulen = 0;
        char *u = lume_unescape(t, len, &ulen);
        char *name = cg_string_val(g, u, ulen);
        free(u);
        return val_take(type_prim(TY_STRING), name);
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

static char *cg_val_eq(CG *g, Val a, Val b, bool ne, size_t line)
{
    Type *boolty = type_prim(TY_BOOL);
    TypeKind ak = a.ty ? a.ty->kind : TY_ANY;
    TypeKind bk = b.ty ? b.ty->kind : TY_ANY;
    bool a_ctr = ak == TY_LIST || ak == TY_STRUCT ||
                 ak == TY_ENUM || ak == TY_RESULT;
    bool b_ctr = bk == TY_LIST || bk == TY_STRUCT ||
                 bk == TY_ENUM || bk == TY_RESULT;
    bool a_ptr = a_ctr || ak == TY_STRING || ak == TY_FUNC ||
                 ak == TY_NULL || ak == TY_ANY;
    bool b_ptr = b_ctr || bk == TY_STRING || bk == TY_FUNC ||
                 bk == TY_NULL || bk == TY_ANY;
    bool a_named = ak == TY_STRUCT && a.ty->name &&
                   sdef_find(&g->structs, a.ty->name) != NULL;
    bool b_named = bk == TY_STRUCT && b.ty->name &&
                   sdef_find(&g->structs, b.ty->name) != NULL;
    /* lume_str_eq/lume_value_eq/lume_any_eq answer 1 for "equal", so ==
     * tests the call against 0 with `ne` and != with `eq`. The direct
     * operand comparisons (bool, null, func, numerics) keep the predicate
     * the operator spells — flipping those would answer `null == null`
     * with "the pointers differ". */
    const char *cmp = ne ? "eq" : "ne";
    const char *idc = ne ? "ne" : "eq";
    char *r = NULL;

    if (a_named && b_named)
        return cg_struct_eq(g, a, b, ne, line);

    if (a_named != b_named) {
        /* A typed aggregate against an i8* heap map (a map literal that
         * matched no declared type, or a map-typed value): no runtime bridge
         * between the two representations exists, and the interpreter would
         * answer by comparing contents. Refuse here rather than emit an icmp
         * whose operands clang rejects with a line number inside the .ll. */
        free(a.v); free(b.v);
        ERR(g, "line %zu: '%s' between %s and %s is not supported "
               "by the native backend yet",
            line, ne ? "!=" : "==", src_type_name(a.ty), src_type_name(b.ty));
    }

    if (ak == TY_STRING && bk == TY_STRING) {
        /* String equality is a byte comparison — the interpreter's
         * values_equal walks the bytes. An icmp here would compare two i8*
         * pointers, which agrees by pointer-luck at best and answers `false`
         * for the same text in two literals. Only ==/!= carry string
         * semantics; the relational spellings keep the pointer order they
         * always had (matched by the libLLVM backend, which does the same). */
        char *eq64 = emit_instrf(g, type_prim(TY_INT),
                                 "call i64 @lume_str_eq(i8* %s, i8* %s)",
                                 a.v, b.v);
        if (!eq64) { free(a.v); free(b.v); return NULL; }
        r = emit_instrf(g, boolty, "icmp %s i64 %s, 0", cmp, eq64);
        free(eq64);
    }
    else if (ak == TY_BOOL && bk == TY_BOOL) {
        /* bool == bool was spelled `icmp eq i64` on two i1 values, which
         * clang rejects outright ("constant expression type mismatch: got
         * type 'i1' but expected 'i64'"). Compare as i1 — the libLLVM
         * backend's builder never had the problem, so this is also a
         * text-vs-llvm parity fix. */
        r = emit_instrf(g, boolty, "icmp %s i1 %s, %s", idc, a.v, b.v);
    }
    else if (a_ctr && b_ctr) {
        /* Structural equality (SPEC 1.8): rt.c walks the slots — list length
         * plus element-wise recursion, map key sets plus values, order
         * independent. The icmp that used to live here compared two i8*
         * pointers, so `[1,2] == [1,2]` answered false on every backend. */
        char *eq64 = emit_instrf(g, type_prim(TY_INT),
                                 "call i64 @lume_value_eq(i8* %s, i8* %s)",
                                 a.v, b.v);
        if (!eq64) { free(a.v); free(b.v); return NULL; }
        r = emit_instrf(g, boolty, "icmp %s i64 %s, 0", cmp, eq64);
        free(eq64);
    }
    else if ((ak == TY_ANY && b_ptr) || (bk == TY_ANY && a_ptr)) {
        /* An any against an i8*-lowered value: the runtime probes the kind —
         * two containers structurally, a container against a non-container
         * never equal, otherwise bytes (which is what the interpreter's
         * values_equal answers for the same pair). `get(m, k) == null` lands
         * here too and used to emit an icmp with a ptr operand, which clang
         * rejects. */
        char *eq64 = emit_instrf(g, type_prim(TY_INT),
                                 "call i64 @lume_any_eq(i8* %s, i8* %s)",
                                 a.v, b.v);
        if (!eq64) { free(a.v); free(b.v); return NULL; }
        r = emit_instrf(g, boolty, "icmp %s i64 %s, 0", cmp, eq64);
        free(eq64);
    }
    else if ((ak == TY_NULL || bk == TY_NULL) && a_ptr && b_ptr) {
        /* null against another i8*-lowered value is pointer identity:
         * `x == null` for a string or container asks "is it the null
         * pointer", which is exactly this — and the only form the native side
         * can express, since it refuses to build a null scalar at all. */
        r = emit_instrf(g, boolty, "icmp %s i8* %s, %s", idc, a.v, b.v);
    }
    else if (ak == TY_FUNC && bk == TY_FUNC) {
        /* Function values compare by identity — the interpreter's
         * values_equal answers true only for the same object. */
        r = emit_instrf(g, boolty, "icmp %s i8* %s, %s", idc, a.v, b.v);
    }
    else if ((ak == TY_INT || ak == TY_FLOAT) && (bk == TY_INT || bk == TY_FLOAT)) {
        /* Numeric: cg_binary's arithmetic promotion already widened a mixed
         * int/float pair to double, so a float here means both sides are
         * float and the fallback icmp would have been an fcmp anyway. */
        if (ak == TY_FLOAT)
            r = emit_instrf(g, boolty, "fcmp %s double %s, %s",
                            ne ? "one" : "oeq", a.v, b.v);
        else
            r = emit_instrf(g, boolty, "icmp %s i64 %s, %s", idc, a.v, b.v);
    }
    else {
        /* Whatever is left is a shape the checker's any-wildcard let through
         * but no lowering exists for (any against a scalar, null against a
         * scalar). Refuse here instead of emitting an icmp whose operands
         * clang rejects with a line number inside the generated .ll. */
        free(a.v); free(b.v);
        ERR(g, "line %zu: '%s' between %s and %s is not supported "
               "by the native backend yet",
            line, ne ? "!=" : "==", src_type_name(a.ty), src_type_name(b.ty));
    }

    free(a.v); free(b.v);
    return r;
}

static char *cg_struct_eq(CG *g, Val a, Val b, bool ne, size_t line)
{
    Type *boolty = type_prim(TY_BOOL);
    SDef *sa = sdef_find(&g->structs, a.ty->name);
    SDef *sb = sdef_find(&g->structs, b.ty->name);

    /* bj[i] is sb's index for sa's field i, or -1 when the name is absent.
     * A field-set mismatch is a well-formed question with the answer "not
     * equal" — the interpreter says the same about two maps with different
     * key sets. */
    int *bj = NULL;
    if (sa && sb && sa->count == sb->count) {
        bj = (int *)xmalloc((size_t)sa->count * sizeof *bj);
        for (int i = 0; i < sa->count; i++) {
            bj[i] = -1;
            for (int j = 0; j < sb->count; j++)
                if (strcmp(sb->names[j], sa->names[i]) == 0) { bj[i] = j; break; }
        }
        for (int i = 0; i < sa->count; i++)
            if (bj[i] < 0) { free(bj); bj = NULL; break; }
    }
    if (!bj) {
        free(a.v); free(b.v);
        return xstrdup(ne ? "true" : "false");
    }

    struct_addr(g, &a);
    struct_addr(g, &b);
    const char *at = llvm_type_of(a.ty);
    const char *bt = llvm_type_of(b.ty);
    if (!at || !bt || !a.v || !b.v) {
        free(bj); free(a.v); free(b.v);
        ERR(g, "line %zu: cannot compare these struct values", line);
    }

    char *acc = xstrdup("true");
    for (int i = 0; i < sa->count; i++) {
        int j = bj[i];
        Type *fty = sa->types[i];
        Type *gty = sb->types[j];
        const char *flt = llvm_type_of(fty);
        const char *glt = llvm_type_of(gty);
        if (!flt || !glt) {
            free(acc); free(bj); free(a.v); free(b.v);
            ERR(g, "line %zu: field '%s' has no codegen type", line, sa->names[i]);
        }

        /* Two indices: the leading 0 walks the pointer to the struct, the
         * second is the field — omitting it turns the subscript into an
         * array index (cg_member carries the same warning). */
        char *pa = emit_instrf(g, a.ty,
                               "getelementptr inbounds %s, %s* %s, i32 0, i32 %d",
                               at, at, a.v, i);
        char *pb = emit_instrf(g, b.ty,
                               "getelementptr inbounds %s, %s* %s, i32 0, i32 %d",
                               bt, bt, b.v, j);
        if (!pa || !pb) {
            free(pa); free(pb); free(acc); free(bj); free(a.v); free(b.v);
            ERR(g, "line %zu: cannot compare field '%s'", line, sa->names[i]);
        }

        Val fa = val_take(fty, emit_instrf(g, fty, "load %s, %s* %s",
                                           flt, flt, pa));
        Val fb = val_take(gty, emit_instrf(g, gty, "load %s, %s* %s",
                                           glt, glt, pb));
        free(pa); free(pb);
        /* A nested struct arrives as an aggregate *value* — tag it so
         * struct_addr knows to give it a slot before the recursive compare
         * GEPs into it (cg_var hands out references instead: agg already 0
         * and v already an address, which struct_addr leaves alone). */
        if (flt[0] == '%') fa.agg = 1;
        if (glt[0] == '%') fb.agg = 1;
        if (!fa.v || !fb.v) {
            free(fa.v); free(fb.v); free(acc); free(bj); free(a.v); free(b.v);
            ERR(g, "line %zu: cannot read field '%s'", line, sa->names[i]);
        }

        char *fe = cg_val_eq(g, fa, fb, false, line);
        if (!fe) {
            free(acc); free(bj); free(a.v); free(b.v);
            return NULL;   /* g->err already set by cg_val_eq */
        }
        char *nx = emit_instrf(g, boolty, "and i1 %s, %s", acc, fe);
        free(acc); free(fe);
        if (!nx) {
            free(bj); free(a.v); free(b.v);
            ERR(g, "line %zu: cannot compare these struct values", line);
        }
        acc = nx;
    }
    free(bj); free(a.v); free(b.v);

    if (!ne) return acc;
    char *out = emit_instrf(g, boolty, "xor i1 %s, true", acc);
    free(acc);
    if (!out) ERR(g, "line %zu: cannot negate struct comparison", line);
    return out;
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
        bool is_eq = n->as.binary.op == OP_EQ || n->as.binary.op == OP_NE;
        char *r = NULL;

        if (is_eq) {
            /* == / != need content or structure — never pointer luck: the
             * dispatch lives in cg_val_eq, which consumes both operand names
             * (freed on every path, including this one), so a.v/b.v are
             * NULLed here to keep the tail of this block from freeing them a
             * second time. The relational spellings never reach it — they
             * keep the numeric/pointer paths they always had (the checker
             * refuses them for containers, and strings are a known-broken
             * corner on all three backends). */
            r = cg_val_eq(g, a, b, n->as.binary.op == OP_NE, n->line);
            a.v = NULL;
            b.v = NULL;
            if (!r) ERRV(g, "line %zu: bad comparison", n->line);
            return val_take(boolty, r);
        }
        if (!r) {
            if (a.ty && a.ty->kind == TY_FLOAT)
                r = emit_instrf(g, boolty, "fcmp %s double %s, %s", fcmp[k], a.v, b.v);
            else
                r = emit_instrf(g, boolty, "icmp %s i64 %s, %s", icmp[k], a.v, b.v);
        }
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

static Val cg_ternary(CG *g, Node *n)
{
    Type *rty = infer_node_type(g, n);
    if (!rty || rty->kind == TY_ANY)
        ERRV(g, "line %zu: cannot infer ternary result type", n->line);
    const char *st = llvm_type_of(rty);
    if (!st) ERRV(g, "line %zu: unsupported ternary result type", n->line);

    Val c = cg_expr(g, n->as.ternary.cond);
    if (!c.v) ERRV(g, "line %zu: bad ternary condition", n->line);

    int lt = g->lid, lf = g->lid + 1, lj = g->lid + 2;
    g->lid += 3;

    char res[64];
    snprintf(res, sizeof res, "%%tv%d", g->tid++);
    /* Same latitude as the and/or slot: an alloca anywhere in the function
     * is legal, it only has to dominate the loads of it. */
    EMIT(g, "  %s = alloca %s\n", res, st);
    EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n", c.v, lt, lf);
    free(c.v);

    EMIT(g, "L%d:\n", lt);
    Val tv = cg_expr(g, n->as.ternary.then);
    if (!tv.v) ERRV(g, "line %zu: bad ternary branch", n->line);
    tv = coerce(g, rty, tv, n->line);
    EMIT(g, "  store %s %s, %s* %s\n", st, tv.v, st, res);
    free(tv.v);
    EMIT(g, "  br label %%L%d\n", lj);

    EMIT(g, "L%d:\n", lf);
    Val ev = cg_expr(g, n->as.ternary.els);
    if (!ev.v) ERRV(g, "line %zu: bad ternary branch", n->line);
    ev = coerce(g, rty, ev, n->line);
    EMIT(g, "  store %s %s, %s* %s\n", st, ev.v, st, res);
    free(ev.v);
    EMIT(g, "  br label %%L%d\n", lj);

    EMIT(g, "L%d:\n", lj);
    char *r = emit_instrf(g, rty, "load %s, %s* %s", st, st, res);
    if (!r) ERRV(g, "line %zu: bad ternary join", n->line);
    return val_take(rty, r);
}

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

    /* `m.x` on a runtime map: the type checker (typecheck_expr.c) already
     * required `x` to be a *declared* key, so the value is always present at
     * run time. Lower it onto the same lume_map_get_* the `?` unwrap and the
     * index read use, dispatched by the field's value type. This matches the
     * interpreter, which does a plain map_get and only falls back to a zero
     * value on a missing key (which a type-checked program cannot produce). */
    if (is_map) {
        Type *vt = n->as.member.type;
        const char *gfn = map_get_fn(vt);
        if (!gfn) {
            free(obj.v);
            ERRV(g, "line %zu: map field '%s' has no codegen type",
                 n->line, n->as.member.name);
        }
        const char *pty = rt_arg_type(vt);
        const char *darg = strcmp(pty, "i8*") == 0   ? "i8* null"
                         : strcmp(pty, "double") == 0 ? "double 0.0"
                                                      : "i64 0";
        Val key = val_take(type_prim(TY_STRING),
                           cg_string_val(g, n->as.member.name,
                                         strlen(n->as.member.name)));
        char *res = emit_instrf(g, vt,
                                "call %s @%s(i8* %s, i8* %s, %s)",
                                pty, gfn, obj.v, key.v, darg);
        free(obj.v); free(key.v);
        if (!res) ERRV(g, "line %zu: cannot read map field '%s'",
                       n->line, n->as.member.name);
        /* A bool travels out of lume_map_get_i as an i64 (0/1) but the
         * backend represents bool as i1; narrow it so downstream coerce and
         * print see a proper bool. */
        if (vt->kind == TY_BOOL) {
            char *b = emit_instrf(g, vt, "trunc i64 %s to i1", res);
            free(res);
            if (!b) ERRV(g, "line %zu: cannot read map field '%s'",
                         n->line, n->as.member.name);
            res = b;
        }
        return val_take(vt, res);
    }

    struct_addr(g, &obj);

    /* Look the field up through the *object's* type, not the member node's
     * own type: the member node carries the field's *result* type (so
     * `o1.inner` stamps `Inner`), which would send the lookup to the wrong
     * struct. The object is always the owner of the field. */
    Type *st = obj.ty;
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

    /* A struct field is returned by address, so a chain like `o1.inner.x`
     * keeps working: the next member access GEPs this address, and print()/
     * stringify() read it through the same path as a struct variable (which
     * is also an address). Scalar fields are loaded into a value as before. */
    if (fty->kind == TY_STRUCT && fty->name) {
        Val rv = val_make(fty, p);
        free(obj.v); free(p);
        return rv;
    }
    char *r = emit_instrf(g, fty, "load %s, %s* %s", llvm_type_of(fty), llvm_type_of(fty), p);
    free(obj.v); free(p);
    if (!r) ERRV(g, "line %zu: cannot read '%s'", n->line, n->as.member.name);
    return val_take(fty, r);
}

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

Val cg_expr(CG *g, Node *n)
{
    if (!n) ERRV(g, "internal: null expression");

    switch (n->type) {
    case N_LITERAL: return cg_literal(g, n);
    case N_LIST_LIT: return cg_list_lit(g, n);
    case N_MAP_LIT: return cg_map_or_struct_lit(g, n);
    case N_STRUCT_UPDATE: return cg_struct_update(g, n);
    /* An assignment in expression position — a C-style `for` header wraps it
     * in an expression statement, and the parser hands those to expressions. */
    case N_ASSIGN:  return cg_assign_expr(g, n);
    case N_ASSIGN_MEMBER: return cg_assign_mem(g, n);
    case N_EXPR_STMT: return cg_expr(g, n->as.expr_stmt.expr);
    case N_VAR:     return cg_var(g, n);
    case N_FUNC_LIT: return cg_funclit(g, n);
    case N_UNARY:   return cg_unary(g, n);
    case N_BINARY:  return cg_binary(g, n);
    case N_TERNARY: return cg_ternary(g, n);
    case N_MATCH:   return cg_match(g, n);
    case N_CTOR:    return cg_ctor(g, n);
    case N_MEMBER:  return cg_member(g, n);
    case N_INDEX:   return cg_index(g, n);
    case N_CALL:    return cg_call(g, n);
    default:        ERRV(g, "%s", bad(g, n, "this expression"));
    }
}

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
        to->kind != TY_LIST && to->kind != TY_STRUCT && to->kind != TY_ANY &&
        to->kind != TY_ENUM) {
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
