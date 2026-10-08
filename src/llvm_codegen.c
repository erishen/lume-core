/* llvm_codegen.c — Lume AST → LLVM IR, built through the libLLVM C API.
 *
 * Design notes (wider rationale in docs/NATIVE.md):
 *
 *  - Unlike codegen.c, IR exists as *values* while it is being built: every
 *    instruction goes through an LLVMBuilder, so operand types, block
 *    terminators and reference validity are enforced by LLVM as we go. That
 *    is the whole point of this backend — the hand-written text emitter has
 *    to enforce all of that itself, and only finds out at the moment clang
 *    parses the buffer.
 *  - Locals are still alloca + load/store. The builder's insertion point
 *    removes the text backend's "hoist every alloca into the entry block"
 *    constraint, but hoisting is kept because it keeps the emitted IR both
 *    readable and shaped like the other backend's output.
 *  - A block is closed exactly once (LLVMGetBasicBlockTerminator decides),
 *    which is what makes `if (c) { return 1; }` legal without the trailing
 *    `unreachable` the text backend has to emit.
 *  - Anything outside the supported subset is rejected with an explicit
 *    error; we never hand back a half-built module.
 */

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>          /* LLVMVerifyModule */
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm/Config/llvm-config.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lume.h"
#include "llvm_codegen.h"

/* The checker's ANY singleton. A list is spelled the same whatever it holds,
 * but a Val still needs a Type and `any` is the honest one. */
Type *any_type(void);

/* Source-level type name, from codegen_types.c. Declared here rather than
 * included: this backend deliberately does not pull in codegen_internal.h,
 * but the coerce() null guard below has to name the type it refused. */
const char *src_type_name(Type *t);

/* ----------------------------------------------------------------- utils --- */

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("lume(llvm): out of memory\n", stderr); exit(70); }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fputs("lume(llvm): out of memory\n", stderr); exit(70); }
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------ codegen ctx --- */

typedef struct { char *name; Type *ty; LLVMValueRef slot; } Asg;
typedef struct { Asg *v; int n, cap; } Asgs;

typedef struct { char *name; Type *ret; Type **params; int arity; } Sig;
typedef struct { Sig *v; int n, cap; } Sigs;

typedef struct { char *name; char **names; Type **types; int count; } SDef;
typedef struct { SDef *v; int n, cap; } SDefs;

#define MAX_STRUCT_TYPES 64
typedef struct { char name[128]; LLVMTypeRef ty; } StructEnt;

typedef struct {
    LLVMContextRef    ctx;
    LLVMModuleRef     mod;
    LLVMBuilderRef    ab;          /* the moving builder                     */
    LLVMValueRef      fn;          /* function whose body is being emitted   */
    LLVMBasicBlockRef cur;         /* block the builder points into          */
    LLVMTypeRef       i1, i64, dbl, i8ptr, void_ty, i32;

    Asgs   locals;                 /* variables of the function emitted now  */
    Sigs   sigs;                   /* every top-level func                   */
    SDefs  structs;                /* every `type X = {...}`                 */
    Type  *expect;                 /* type the surrounding context wants     */
    Type  *cur_ret;                /* return type of the function emitted    */
    StructEnt structs_types[MAX_STRUCT_TYPES];
    int    structs_types_n;

    char   err[1024];
    int    tid;

    LLVMBasicBlockRef *brk; int nbrk, cbrk;   /* enclosing loop exits      */
    LLVMBasicBlockRef *cnt; int ncnt, ccnt;   /* enclosing loop heads      */

    /* Synthetic top-level body emitting `main()`: the process status is the
     * value that call returns, so remember it (exit_val) and hand it to the
     * fallback terminator in cg_function — the top level has no explicit
     * `return`, but here it answers for the program's exit code. */
    int          capture_main;
    LLVMValueRef exit_val;
} CG;

/* Forward: the member type below reads the object's static type, which is
 * computed by infer_node_type further down. */
static Type *infer_node_type(CG *g, Node *n);

/* Move the builder to the block being emitted into. Every emitter starts with
 * this, so a statement that only conditionally emits cannot leave the builder
 * pointing somewhere else for the next one. */
#define AT(g)  LLVMPositionBuilderAtEnd((g)->ab, (g)->cur)

/* A block is done exactly once: LLVM refuses instructions after a terminator,
 * so a `br` out of a block that already returned has to be skipped. */
#define DONE(bb) (LLVMGetBasicBlockTerminator(bb) != NULL)

/* First error wins: the root-cause message must not be overwritten by a
 * downstream one. `cg_print` walks the same `g->err` buffer, so without this
 * guard a genuine "X does not apply to floats" raised in `cg_binary` got
 * clobbered by a generic "print() cannot print this value" once codegen
 * returned a typeless value to it — see §8.1 #1. `g->err` is zeroed by the
 * memset in llvm_codegen_module, so the guard reads cleanly on a fresh pass. */
#define ERR(g, fmt, ...)                                                       \
    do {                                                                       \
        if ((g)->err[0] == '\0')                                               \
            snprintf((g)->err, sizeof (g)->err, fmt, ##__VA_ARGS__);           \
        return NULL;                                                           \
    } while (0)

#define ERRV(g, fmt, ...)                                                      \
    do {                                                                       \
        if ((g)->err[0] == '\0')                                               \
            snprintf((g)->err, sizeof (g)->err, fmt, ##__VA_ARGS__);           \
        return (Val){ NULL, NULL };                                            \
    } while (0)

#define ERRX(g, fmt, ...)                                                      \
    do {                                                                       \
        if ((g)->err[0] == '\0')                                               \
            snprintf((g)->err, sizeof (g)->err, fmt, ##__VA_ARGS__);           \
        return;                                                                \
    } while (0)

static void asgs_free(Asgs *a)
{
    for (int i = 0; i < a->n; i++) free(a->v[i].name);
    free(a->v);
    memset(a, 0, sizeof *a);
}

/* slot == NULL asks for a fresh local slot. */
static void asg_push(Asgs *a, const char *name, Type *ty, LLVMValueRef slot)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->v = (Asg *)xrealloc(a->v, (size_t)a->cap * sizeof *a->v);
    }
    a->v[a->n].name = xstrdup(name);
    a->v[a->n].ty   = ty;
    a->v[a->n].slot = slot;
    a->n++;
}

static Asg *asg_find(Asgs *a, const char *name)
{
    for (int i = a->n - 1; i >= 0; i--)
        if (strcmp(a->v[i].name, name) == 0) return &a->v[i];
    return NULL;
}

static void sig_push(Sigs *s, const char *name, Type *ret, Type **params, int arity)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = (Sig *)xrealloc(s->v, (size_t)s->cap * sizeof *s->v);
    }
    s->v[s->n].name   = xstrdup(name);
    s->v[s->n].ret    = ret;
    s->v[s->n].params = params;
    s->v[s->n].arity  = arity;
    s->n++;
}

static Sig *sig_find(Sigs *s, const char *name)
{
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->v[i].name, name) == 0) return &s->v[i];
    return NULL;
}

static void sdef_push(SDefs *d, const char *name, char **names, Type **types, int count)
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

static SDef *sdef_find(SDefs *d, const char *name)
{
    for (int i = 0; i < d->n; i++)
        if (strcmp(d->v[i].name, name) == 0) return &d->v[i];
    return NULL;
}

/* Both tables xstrdup the name they are handed, and those copies outlive every
 * lookup as soon as codegen is done looking. The shapes they point at (Type *,
 * char **) are the AST's, so only the names are ours to release. */
static void sigs_free(Sigs *s)
{
    for (int i = 0; i < s->n; i++) free(s->v[i].name);
    free(s->v);
    s->v = NULL; s->n = 0; s->cap = 0;
}

static void sdefs_free(SDefs *d)
{
    for (int i = 0; i < d->n; i++) free(d->v[i].name);
    free(d->v);
    d->v = NULL; d->n = 0; d->cap = 0;
}

static void push_bb(LLVMBasicBlockRef **v, int *n, int *cap, LLVMBasicBlockRef bb)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *v = (LLVMBasicBlockRef *)xrealloc(*v, (size_t)*cap * sizeof(**v));
    }
    (*v)[(*n)++] = bb;
}

/* ----------------------------------------------------------- type mapping --- */

/* Named struct types are interned: the same declaration has to hand out the
 * same type object every time, or GEPs and call signatures stop matching. */
static LLVMTypeRef struct_ty(CG *g, const char *name)
{
    for (int i = 0; i < g->structs_types_n; i++)
        if (strcmp(g->structs_types[i].name, name) == 0)
            return g->structs_types[i].ty;

    if (g->structs_types_n >= MAX_STRUCT_TYPES)
        ERR(g, "too many struct types (limit %d)", MAX_STRUCT_TYPES);

    StructEnt *e = &g->structs_types[g->structs_types_n++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->ty = LLVMStructCreateNamed(g->ctx, name);
    return e->ty;
}

static LLVMTypeRef ty_of(CG *g, Type *t)
{
    if (!t) return NULL;
    switch (t->kind) {
    case TY_INT:    return g->i64;
    case TY_FLOAT:  return g->dbl;
    case TY_BOOL:   return g->i1;
    case TY_STRING: return g->i8ptr;
    case TY_NULL:   return g->i8ptr;   /* opaque null pointer; rt.c prints it */
    case TY_LIST:   return g->i8ptr;
    case TY_RESULT:
        /* Result is `{ ok: ... }` / `{ err: ... }`, and the interpreter treats
         * it as an ordinary map (interp.c's `propagate` branch looks the two
         * keys up at runtime) -- so natively it is the same heap map and
         * travels as the same opaque pointer. Kept in step with
         * llvm_type_of() in codegen_types.c, which the text emitter uses. */
        return g->i8ptr;
    case TY_STRUCT:
        /* An anonymous struct is a runtime map (a `{...}` no declared type
         * claimed) and a list is a heap object: both are opaque pointers, not
         * aggregates the IR can spell. */
        if (!t->name) return g->i8ptr;
        return struct_ty(g, t->name);
    default:        return NULL;
    }
}

/* How a reference to `t` travels: a *named* struct is passed as a pointer to
 * its value, a list or a map is itself an opaque pointer to a heap object so
 * it travels as-is, everything else by value. */
static LLVMTypeRef ptr_of(CG *g, Type *t)
{
    LLVMTypeRef ty = ty_of(g, t);
    if (!ty) return NULL;
    if (t->kind == TY_STRUCT && t->name) return LLVMPointerType(ty, 0);
    return ty;
}

static LLVMTypeRef ty_from_spelling(CG *g, const char *s)
{
    if (strcmp(s, "i64") == 0)    return g->i64;
    if (strcmp(s, "double") == 0) return g->dbl;
    if (strcmp(s, "i1") == 0)     return g->i1;
    if (strcmp(s, "i8*") == 0)    return g->i8ptr;
    return NULL;
}

/* ---------------------------------------------------------- string consts --- */

/* Build `@lume.strN` from an explicit length (the lexer's token is not
 * guaranteed to be NUL-terminated) and return an i8* to its first byte. */
static LLVMValueRef cg_string_val(CG *g, const char *text, size_t len)
{
    LLVMTypeRef arr = LLVMArrayType(LLVMInt8TypeInContext(g->ctx), (unsigned)len + 1);
    LLVMValueRef init = LLVMConstStringInContext(g->ctx, text, (unsigned)len, 0);
    LLVMValueRef gv = LLVMAddGlobal(g->mod, arr, "lume.str");
    LLVMSetInitializer(gv, init);
    LLVMSetLinkage(gv, LLVMPrivateLinkage);
    LLVMSetUnnamedAddr(gv, 1);

    LLVMValueRef zero = LLVMConstInt(g->i64, 0, 0);
    LLVMValueRef idx[2] = { zero, zero };
    AT(g);
    /* LLVM 23 dropped the pointer-typed legacy overloads in favour of the
     * explicit element type, so GEPs are spelled ...2 and pass Ty by hand.
     * Here the element type is i8 (the array is [len+1 x i8]). */
    return LLVMBuildInBoundsGEP2(g->ab, LLVMInt8TypeInContext(g->ctx), gv, idx, 2, "s");
}

/* ------------------------------------------------------------- expressions -- */

typedef struct { LLVMValueRef v; Type *ty; } Val;

static Val coerce(CG *g, Type *to, Val v, size_t line);
static Val cg_assign_mem(CG *g, Node *n);   /* defined with cg_stmt, used by cg_expr */
static Val cg_index(CG *g, Node *n);        /* m["k"] / l[0] -- defined after cg_expr */
static Val cg_expr(CG *g, Node *n);
static void cg_stmt(CG *g, Node *n);
static void cg_block(CG *g, Node *blk);

static Val val_make(Type *ty, LLVMValueRef v)
{
    Val r;
    r.ty = ty;
    r.v  = v;
    return r;
}

/* The vendored frontend exports no node-name helper; keep a local table so
 * "not supported yet" errors point at the actual construct. */
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
    case N_INDEX:   return "index";
    }
    return "node";
}

static const char *bad(CG *g, Node *n, const char *what)
{
    snprintf(g->err, sizeof g->err,
             "line %zu: %s (%s) is not supported by the native backend yet",
             n ? n->line : 0, node_type_name(n->type), what);
    return g->err;
}

/* ---------------------------------------------------------------- literal --- */

static Val cg_literal(CG *g, Node *n)
{
    AT(g);
    LitKind k = n->as.lit.kind;

    if (k == LIT_TRUE || k == LIT_FALSE)
        return val_make(type_prim(TY_BOOL),
                        LLVMConstInt(g->i1, k == LIT_TRUE ? 1u : 0u, 0));

    if (k == LIT_STR) {
        /* The lexer hands over the token *with* its quotes. */
        const char *t = n->as.lit.text;
        size_t      len = (size_t)n->as.lit.len;
        if (len >= 2 && t[0] == '"' && t[len - 1] == '"') { t++; len -= 2; }
        return val_make(type_prim(TY_STRING), cg_string_val(g, t, len));
    }

    if (k == LIT_NUM) {
        if (n->as.lit.is_float)
            return val_make(type_prim(TY_FLOAT), LLVMConstReal(g->dbl, n->as.lit.num));
        /* Read the literal's i64 directly; going through lit.num (a double)
         * would round anything past 2^53 before LLVM ever sees it. */
        return val_make(type_prim(TY_INT),
                        LLVMConstInt(g->i64, (unsigned long long)n->as.lit.inum, 0));
    }

    if (k == LIT_NULL)
        /* `null` lowers to an opaque i8* null pointer (LLVMConstNull); the
         * runtime helper prints it as "null", matching the interpreter. */
        return val_make(type_prim(TY_NULL), LLVMConstNull(g->i8ptr));

    ERRV(g, "line %zu: 'null' literals are not supported by the native backend yet", n->line);
}

/* ------------------------------------------------------------------- var --- */

static Val cg_var(CG *g, Node *n)
{
    Asg *a = asg_find(&g->locals, n->as.var.name);
    if (!a) ERRV(g, "line %zu: unknown variable '%s'", n->line, n->as.var.name);
    if (!ty_of(g, a->ty))
        ERRV(g, "line %zu: variable '%s' has no codegen type", n->line, n->as.var.name);

    /* A *named* struct is always *referenced*: the slot holds its address.
     * A list or a runtime map is a heap object behind an opaque pointer, so
     * the slot holds the pointer itself and has to be loaded -- passing the
     * slot would make the runtime read its struct out of the stack frame. */
    if (a->ty->kind == TY_STRUCT && a->ty->name) return val_make(a->ty, a->slot);

    AT(g);
    return val_make(a->ty, LLVMBuildLoad2(g->ab, ty_of(g, a->ty), a->slot, "v"));
}

/* --------------------------------------------------------------- unary ------ */

static Val cg_unary(CG *g, Node *n)
{
    AT(g);
    Val o = cg_expr(g, n->as.unary.operand);
    if (!o.v) ERRV(g, "line %zu: bad unary operand", n->line);

    if (n->as.unary.op == OP_NOT)
        return val_make(type_prim(TY_BOOL), LLVMBuildNot(g->ab, o.v, "n"));

    if (!o.ty || (o.ty->kind != TY_INT && o.ty->kind != TY_FLOAT))
        ERRV(g, "line %zu: unary '-' needs a numeric operand", n->line);

    if (o.ty->kind == TY_FLOAT) return val_make(o.ty, LLVMBuildFNeg(g->ab, o.v, "n"));
    return val_make(o.ty, LLVMBuildNeg(g->ab, o.v, "n"));
}

/* --------------------------------------------------------------- binary ----- */

static Val cg_binary(CG *g, Node *n)
{
    AT(g);
    Val a = cg_expr(g, n->as.binary.left);
    Val b = cg_expr(g, n->as.binary.right);

    /* Promote before emitting anything: `x / 2` with x: double would otherwise
     * hand LLVM an i64 constant where a double operand is required. */
    if (a.ty && b.ty && (a.ty->kind == TY_FLOAT || b.ty->kind == TY_FLOAT)) {
        a = coerce(g, type_prim(TY_FLOAT), a, n->line);
        b = coerce(g, type_prim(TY_FLOAT), b, n->line);
    }
    if (!a.v || !b.v) ERRV(g, "line %zu: bad operands", n->line);

    if (n->as.binary.op == OP_AND || n->as.binary.op == OP_OR) {
        /* Short-circuit, so the right operand is *not* emitted yet: the subset
         * is used for `x != 0 and 10 / x > 1` and an eager lowering would
         * divide by zero. */
        bool is_and = n->as.binary.op == OP_AND;

        /* The result is parked in an alloca rather than in a phi, which is
         * what the rest of this emitter does for every other local. */
        LLVMValueRef a_slot = LLVMBuildAlloca(g->ab, g->i1, "lb");

        /* Three blocks, each writing the result slot exactly once. AND can
         * only answer `true` from the right operand, OR only from the left, so
         * the short path stores a constant: `true or 1/0` has to read `true`,
         * never the value of an expression it never evaluated. */
        LLVMBasicBlockRef ls = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "short");
        LLVMBasicBlockRef ll = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "long");
        LLVMBasicBlockRef lj = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "join");

        if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, a.v, is_and ? ll : ls, is_and ? ls : ll);

        g->cur = ls; AT(g);
        LLVMBuildStore(g->ab, LLVMConstInt(g->i1, is_and ? 0u : 1u, 0), a_slot);
        if (!DONE(g->cur)) LLVMBuildBr(g->ab, lj);

        g->cur = ll; AT(g);
        Val b2 = cg_expr(g, n->as.binary.right);
        if (!b2.v) ERRV(g, "line %zu: bad logical operands", n->line);
        LLVMBuildStore(g->ab, b2.v, a_slot);
        if (!DONE(g->cur)) LLVMBuildBr(g->ab, lj);

        g->cur = lj; AT(g);
        return val_make(type_prim(TY_BOOL), LLVMBuildLoad2(g->ab, g->i1, a_slot, "l"));
    }

    /* string + string is the only operator strings get, and it has to be a
     * runtime call — IR has no aggregate instruction to concatenate with. */
    if (n->as.binary.op == OP_ADD &&
        a.ty && b.ty && a.ty->kind == TY_STRING && b.ty->kind == TY_STRING) {
        LLVMTypeRef at[2] = { g->i8ptr, g->i8ptr };
        LLVMTypeRef ft = LLVMFunctionType(g->i8ptr, at, 2, 0);
        LLVMValueRef fn = LLVMGetNamedFunction(g->mod, "lume_bi_cat");
        if (!fn) fn = LLVMAddFunction(g->mod, "lume_bi_cat", ft);
        LLVMValueRef args[2] = { a.v, b.v };
        return val_make(type_prim(TY_STRING),
                        LLVMBuildCall2(g->ab, ft, fn, args, 2, "cat"));
    }

    if (n->as.binary.op >= OP_EQ && n->as.binary.op <= OP_GE) {
        size_t k = (size_t)(n->as.binary.op - OP_EQ);
        if (a.ty && a.ty->kind == TY_FLOAT) {
            static const unsigned op[] = { LLVMRealOEQ, LLVMRealONE, LLVMRealOLT,
                                           LLVMRealOLE, LLVMRealOGT, LLVMRealOGE };
            return val_make(type_prim(TY_BOOL),
                            LLVMBuildFCmp(g->ab, (LLVMRealPredicate)op[k], a.v, b.v, "c"));
        }
        static const unsigned op[] = { LLVMIntEQ, LLVMIntNE, LLVMIntSLT,
                                       LLVMIntSLE, LLVMIntSGT, LLVMIntSGE };
        return val_make(type_prim(TY_BOOL),
                        LLVMBuildICmp(g->ab, (LLVMIntPredicate)op[k], a.v, b.v, "c"));
    }

    /* Same guard the IR-text emitter puts up (see cg_binary there): a string
     * against a number is rejected with a line number in the source instead of
     * letting a libLLVM assertion with no file name surface from the builder. */
    bool astr    = a.ty && a.ty->kind == TY_STRING;
    bool bstr    = b.ty && b.ty->kind == TY_STRING;
    bool numeric = (a.ty && (a.ty->kind == TY_INT || a.ty->kind == TY_FLOAT)) ||
                   (b.ty && (b.ty->kind == TY_INT || b.ty->kind == TY_FLOAT));
    if ((astr || bstr) && numeric)
        ERRV(g, "line %zu: mixing a string with a number is not supported by the "
                "native backend yet", n->line);

    bool is_float = a.ty && a.ty->kind == TY_FLOAT;

    /* `/` is floating-point division even on two integers: the interpreter
     * answers `7 / 2` with 3.5, and an sdiv answers 3. Both native backends
     * used to emit the integer division — a wrong answer, not an error — so
     * the operands are widened here instead. */
    bool div_int = !is_float && n->as.binary.op == OP_DIV;
    Type *rty = type_prim((is_float || div_int) ? TY_FLOAT : TY_INT);
    LLVMValueRef r;
    if (is_float) {
        switch (n->as.binary.op) {
        case OP_ADD: r = LLVMBuildFAdd(g->ab, a.v, b.v, "a"); break;
        case OP_SUB: r = LLVMBuildFSub(g->ab, a.v, b.v, "a"); break;
        case OP_MUL: r = LLVMBuildFMul(g->ab, a.v, b.v, "a"); break;
        case OP_DIV: r = LLVMBuildFDiv(g->ab, a.v, b.v, "a"); break;
        case OP_MOD: ERRV(g, "line %zu: '%%' does not apply to floats", n->line);
        default:     ERRV(g, "line %zu: bad operator", n->line);
        }
    } else {
        switch (n->as.binary.op) {
        case OP_ADD: r = LLVMBuildAdd(g->ab, a.v, b.v, "a"); break;
        case OP_SUB: r = LLVMBuildSub(g->ab, a.v, b.v, "a"); break;
        case OP_MUL: r = LLVMBuildMul(g->ab, a.v, b.v, "a"); break;
        case OP_DIV: {
            LLVMValueRef fa = LLVMBuildSIToFP(g->ab, a.v, g->dbl, "a");
            LLVMValueRef fb = LLVMBuildSIToFP(g->ab, b.v, g->dbl, "b");
            if (!fa || !fb) ERRV(g, "line %zu: bad division operands", n->line);
            r = LLVMBuildFDiv(g->ab, fa, fb, "a");
            break;
        }
        case OP_MOD: r = LLVMBuildSRem(g->ab, a.v, b.v, "a"); break;
        default:     ERRV(g, "line %zu: bad operator", n->line);
        }
    }
    return val_make(rty, r);
}

/* ---------------------------------------------------------------- member ---- */

/* `.len` / `.length` on a list or a map is a runtime call: both are opaque
 * heap objects, so there is no field to index. */
static LLVMValueRef rt_decl(CG *g, const char *name, LLVMTypeRef ret,
                            LLVMTypeRef *params, int n);   /* defined with cg_print */

static Val cg_len_of(CG *g, const char *fn, LLVMValueRef obj)
{
    AT(g);
    LLVMTypeRef p[1] = { g->i8ptr };
    LLVMValueRef v[1] = { obj };
    LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i64, p, 1, 0),
                                    rt_decl(g, fn, g->i64, p, 1), v, 1, "len");
    return r ? val_make(type_prim(TY_INT), r) : (Val){ NULL, NULL };
}

static int struct_field_idx(CG *g, const char *sname, const char *fname)
{
    SDef *sd = sdef_find(&g->structs, sname);
    if (!sd) return -1;
    for (int i = 0; i < sd->count; i++)
        if (strcmp(sd->names[i], fname) == 0) return i;
    return -1;
}

static Type *struct_field_type(CG *g, const char *sname, int idx)
{
    SDef *sd = sdef_find(&g->structs, sname);
    if (sd && idx >= 0 && idx < sd->count) return sd->types[idx];
    return NULL;
}

/* Address of field `idx` of *ptrv* (a pointer to a struct value).
 * LLVM ≥ 15 keeps pointers opaque, so the pointee cannot be recovered from
 * the pointer: on LLVM 23 LLVMTypeOf(ptrv) is a bare `ptr` and
 * LLVMGetElementType on it yields `half`, which turns the GEP into
 * `getelementptr inbounds nuw half, ptr %0, i32 0, i32 0` — the verifier
 * rejects it ("Invalid indices for GEP pointer type!"). The struct type is
 * therefore spelled in from the frontend's type node, which is authoritative
 * for both the member read (cg_member) and the member write (cg_assign_mem).
 *
 * One index, not the text backend's double index, so the offset-16
 * wrong-field trap it dances around does not exist here. */
static LLVMValueRef gep_field(CG *g, Type *sty, LLVMValueRef ptrv, int idx)
{
    LLVMTypeRef et = ty_of(g, sty);
    if (!et) return NULL;
    return LLVMBuildStructGEP2(g->ab, et, ptrv, (unsigned)idx, "f");
}

/* Address of *o*, slotting an aggregate first. A struct is always referenced
 * in the IR this backend builds — a variable is its slot — so the only values
 * indexed by a field access are addresses; a struct literal or a call that
 * returns one hands back an aggregate that has to land in memory first.
 * Storing an address into a slot here would build a `%Point**` and the field
 * would be read off the pointer's bytes. */
static int struct_is_addr(LLVMValueRef v);   /* defined with the argument path */

static void struct_addr(CG *g, Val *o)
{
    if (!o->ty || o->ty->kind != TY_STRUCT) return;
    if (o->v && struct_is_addr(o->v)) return;      /* already an address */
    LLVMTypeRef st = ty_of(g, o->ty);
    if (!st) return;
    LLVMValueRef slot = LLVMBuildAlloca(g->ab, st, "sa");
    LLVMBuildStore(g->ab, o->v, slot);
    o->v = slot;
}

static Val cg_member(CG *g, Node *n)
{
    AT(g);
    Val obj = cg_expr(g, n->as.member.obj);

    /* `s.len` / `s.length` / `xs.len` / `m.len`, before the struct path: there
     * is no declared field behind any of them. It has to come before struct_addr
     * too — struct_addr turns a map's *heap pointer* into the address of a
     * stack slot holding that pointer, and passing the slot to lume_map_len
     * would read the map out of the caller's frame. */
    if (strcmp(n->as.member.name, "len") == 0 ||
        strcmp(n->as.member.name, "length") == 0) {
        if (obj.ty && obj.ty->kind == TY_LIST)
            return cg_len_of(g, "lume_list_len", obj.v);
        if (obj.ty && obj.ty->kind == TY_STRUCT && !obj.ty->name)
            return cg_len_of(g, "lume_map_len", obj.v);
        /* A string is the runtime's own i8* in a slot, not an object with a
         * header, so its length lives in the runtime's helper. Skipping this
         * branch used to fall through to print() as an unprintable value. */
        if (obj.ty && obj.ty->kind == TY_STRING)
            return cg_len_of(g, "lume_bi_len_s", obj.v);
    }

    struct_addr(g, &obj);

    /* The checker does not always stamp the struct type onto a member node, so
     * fall back to the object's type — that is what makes `r.w` work on a
     * struct parameter. */
    Type *st = n->as.member.type;
    if (!st || st->kind != TY_STRUCT || !st->name) st = obj.ty;
    if (!st || st->kind != TY_STRUCT || !st->name)
        ERRV(g, "line %zu: field access on a non-struct value", n->line);

    int idx = struct_field_idx(g, st->name, n->as.member.name);
    if (idx < 0) ERRV(g, "line %zu: unknown field '%s' on '%s'",
                      n->line, n->as.member.name, st->name);
    if (!ty_of(g, st)) ERRV(g, "line %zu: cannot access '%s'", n->line, n->as.member.name);

    Type *fty = struct_field_type(g, st->name, idx);
    if (!fty || !ty_of(g, fty))
        ERRV(g, "line %zu: field '%s' has no codegen type", n->line, n->as.member.name);

    /* StructGEP walks `%Rect* -> field` in one index, so the text backend's
     * `getelementptr inbounds %Rect, %Rect* %r, i32 0, i32 <field>` double
     * index (and the silent wrong-field bug it avoids) is gone. A struct value
     * was never loaded, so obj.v is its address already. */
    LLVMValueRef p = gep_field(g, st, obj.v, idx);
    if (!p) ERRV(g, "line %zu: cannot take the address of '%s'", n->line, n->as.member.name);
    return val_make(fty, LLVMBuildLoad2(g->ab, ty_of(g, fty), p, "f"));
}

/* --------------------------------------------------------- list / map lit -- */

/* Which getter reads a map value of static type `ty` -- the same choice
 * map_put_fn makes on the way in, read back for `?` unwrapping. Kept in step
 * with codegen_expr.c's copy. */
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

/* Which push helper an element of static type `ty` needs. The checker keeps a
 * list homogeneous, so one answer serves every element. */
static const char *list_push_fn(Type *ty)
{
    if (!ty) return NULL;
    switch (ty->kind) {
    case TY_FLOAT:  return "lume_list_push_f";
    case TY_STRING: return "lume_list_push_s";
    case TY_INT:
    case TY_BOOL:   return "lume_list_push_i";
    /* Containers as elements -- the same branch, and the same reasoning, as in
     * codegen_expr.c's copy of this function. Kept in step deliberately: these
     * two are separate implementations of one table, and a type added to one
     * but not the other compiles on one backend and not the other. */
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
    case TY_LIST:
    case TY_STRUCT:
    case TY_RESULT: return "lume_map_put_obj";
    default:        return NULL;
    }
}

/* `fn(<object>, <args...>)`: the object is always the opaque i8* the runtime
 * hands back, the remaining arguments are spelled by the caller so that a
 * map's key (i8*) and value (i64/double/i8*) can differ, and the result is
 * the helper's own return type. */
static Val rt_call(CG *g, Type *rty, const char *fn, int n,
                   LLVMTypeRef *ats, LLVMValueRef *vs)
{
    AT(g);
    LLVMTypeRef fty = LLVMFunctionType(ty_of(g, rty), ats, (unsigned)n, 0);
    LLVMValueRef r = LLVMBuildCall2(g->ab, fty,
                                    rt_decl(g, fn, ty_of(g, rty), ats, n),
                                    vs, (unsigned)n, "r");
    return r ? val_make(rty, r) : (Val){ NULL, NULL };
}

/* The element type of a list literal, read off its own elements. Only ever
 * asked about a list *literal*: a value is just an i8*, and what a for-in has
 * to know is which reader to call, which is a static question. A heterogeneous
 * literal (`[1.5, 2]`) yields `list<any>`, and iterating that natively would
 * read one slot through the wrong accessor, so it is reported as unknown. */
static Type *infer_list_elem(Node *lit)
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
            case LIT_FALSE: t = type_prim(TY_BOOL);   break;
            default: break;
            }
        }
        if (!t) return NULL;          /* a call, another list, ... is not static */
        if (et && et->kind != t->kind) return NULL;
        et = t;
    }
    return et;   /* `[]` has no element kind; it iterates zero times anyway */
}

static Type *list_type(void) { return type_list(infer_list_elem(NULL)); }

/* `[a, b, c]` — a heap list, built by pushing every element in source order so
 * iteration and the interpreter's ordering agree. */
static Val cg_list_lit(CG *g, Node *n)
{
    AT(g);
    LLVMTypeRef newp[1] = { g->i8ptr };
    LLVMValueRef nv[1] = { NULL };
    LLVMValueRef l = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, NULL, 0, 0),
                                    rt_decl(g, "lume_list_new", g->i8ptr, newp, 0),
                                    nv, 0, "l");
    if (!l) ERRV(g, "line %zu: cannot allocate a list", n->line);

    for (int i = 0; i < n->as.list.count; i++) {
        Val e = cg_expr(g, n->as.list.items[i]);
        const char *fn = list_push_fn(e.ty);
        if (!fn) ERRV(g, "line %zu: list elements must be int, float, string or a container", n->line);
        if (e.ty && e.ty->kind == TY_BOOL) e = coerce(g, type_prim(TY_INT), e, n->line);
        LLVMTypeRef ats[2] = { g->i8ptr, ty_of(g, e.ty) };
        LLVMValueRef vs[2] = { l, e.v };
        Val r = rt_call(g, type_prim(TY_INT), fn, 2, ats, vs);
        if (!r.v) ERRV(g, "line %zu: cannot append to a list", n->line);
    }
    return val_make(list_type(), l);
}

/* A map literal that no declared type claimed is a runtime map, keyed by its
 * source keys in insertion order. */
static Val cg_map_lit(CG *g, Node *n)
{
    AT(g);
    LLVMTypeRef newp[1] = { g->i8ptr };
    LLVMValueRef nv[1] = { NULL };
    LLVMValueRef m = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, NULL, 0, 0),
                                    rt_decl(g, "lume_map_new", g->i8ptr, newp, 0),
                                    nv, 0, "m");
    if (!m) ERRV(g, "line %zu: cannot allocate a map", n->line);

    for (int i = 0; i < n->as.map.count; i++) {
        Val v = cg_expr(g, n->as.map.vals[i]);
        Val k = val_make(type_prim(TY_STRING),
                         cg_string_val(g, n->as.map.keys[i], strlen(n->as.map.keys[i])));
        const char *fn = map_put_fn(v.ty);
        if (!fn) ERRV(g, "line %zu: map values must be int, float, string or a container", n->line);
        if (v.ty && v.ty->kind == TY_BOOL) v = coerce(g, type_prim(TY_INT), v, n->line);
        /* The three flavours disagree on the *type* of the third argument as
         * well as on the helper: the string one takes a i8* key, the two
         * scalar ones an i8* key and a numeric value, so both are spelled. */
        LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, ty_of(g, v.ty) };
        LLVMValueRef vs[3] = { m, k.v, v.v };
        Val r = rt_call(g, type_prim(TY_INT), fn, 3, ats, vs);
        if (!r.v) ERRV(g, "line %zu: cannot store into a map", n->line);
    }
    /* The map is a heap object like a list; the anonymous struct type only
     * marks the value's provenance. */
    return val_make(type_anon_struct(), m);
}

/* Defined with cg_stmt; the map-or-struct decision below needs it. */
static Type *resolve_struct_lit(CG *g, Node *n, int fatal);
static Val cg_struct_lit(CG *g, Node *n);

/* A map literal that no declared type claimed is a runtime map; one that did
 * is a struct value (see cg_struct_lit). */
static Val cg_map_or_struct_lit(CG *g, Node *n)
{
    if (resolve_struct_lit(g, n, 0)) return cg_struct_lit(g, n);
    return cg_map_lit(g, n);
}

/* --------------------------------------------------------------- assign ----- */

static Val cg_assign_expr(CG *g, Node *n)
{
    AT(g);
    Val v = cg_expr(g, n->as.assign.value);

    Asg *a = asg_find(&g->locals, n->as.assign.name);
    if (!a) ERRV(g, "line %zu: assignment to unknown variable '%s'",
                 n->line, n->as.assign.name);

    v = coerce(g, a->ty, v, n->line);
    if (g->err[0]) return (Val){ NULL, NULL };
    if (!ty_of(g, a->ty))
        ERRV(g, "line %zu: variable '%s' has no codegen type",
             n->line, n->as.assign.name);

    LLVMBuildStore(g->ab, v.v, a->slot);
    return v;
}

/* ------------------------------------------------------------ struct lit --- */

static Val cg_struct_lit(CG *g, Node *n);

/* A struct literal carries no type of its own (the parser makes it a map
 * literal), so it is resolved from the context — a `let` annotation or a
 * function return type — and failing that from the declared type whose field
 * set matches the literal exactly. */
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
        if (hit) ERR(g, "line %zu: ambiguous struct literal; annotate the type explicitly",
                     n->line);
        hit = type_struct(sd->name);
    }
    if (!hit) {
        if (fatal)
            ERR(g, "line %zu: cannot resolve this struct literal to a declared type", n->line);
        return NULL;   /* nobody claimed it: a runtime map, not a struct value */
    }
    return hit;
}

static Val cg_struct_lit(CG *g, Node *n)
{
    AT(g);
    Type *ty = resolve_struct_lit(g, n, 1);
    SDef *sd = sdef_find(&g->structs, ty->name);
    if (!sd) ERRV(g, "line %zu: unknown type '%s'", n->line, ty->name);
    if (sd->count != n->as.map.count)
        ERRV(g, "line %zu: struct literal for '%s' has %d field(s), declared %d",
             n->line, ty->name, n->as.map.count, sd->count);

    LLVMTypeRef sty = ty_of(g, ty);
    if (!sty) ERRV(g, "line %zu: type '%s' has no codegen type", n->line, ty->name);

    /* Fill the declared field order, not the source order, so a literal
     * written out of order still lands in the right slot. */
    LLVMValueRef cur = LLVMGetUndef(sty);
    for (int j = 0; j < sd->count; j++) {
        int idx = -1;
        for (int i = 0; i < n->as.map.count; i++)
            if (strcmp(n->as.map.keys[i], sd->names[j]) == 0) { idx = i; break; }
        if (idx < 0) ERRV(g, "line %zu: '%s' is missing field '%s'",
                          n->line, ty->name, sd->names[j]);

        Type *prev = g->expect;
        g->expect = sd->types[j];                 /* nested literals resolve here */
        Val f = cg_expr(g, n->as.map.vals[idx]);
        g->expect = prev;

        if (f.ty && sd->types[j] && f.ty != sd->types[j])
            f = coerce(g, sd->types[j], f, n->line);
        if (g->err[0]) return (Val){ NULL, NULL };
        cur = LLVMBuildInsertValue(g->ab, cur, f.v, (unsigned)j, "s");
    }
    return val_make(ty, cur);
}

/* ------------------------------------------------------------------ call ---- */

/* Declare a runtime helper (src/rt.c) once, then hand it back. */
static LLVMValueRef rt_decl(CG *g, const char *name, LLVMTypeRef ret,
                            LLVMTypeRef *params, int n)
{
    LLVMValueRef fn = LLVMGetNamedFunction(g->mod, name);
    if (fn) return fn;
    return LLVMAddFunction(g->mod, name, LLVMFunctionType(ret, params, (unsigned)n, 0));
}

/* print() lowers to a non-variadic runtime helper, never to printf: the
 * helpers take i64 for int/bool and double for float, so a boolean has to be
 * widened before the call. */
static Val cg_print(CG *g, Node *n, Val a)
{
    AT(g);
    LLVMTypeRef ret = g->i64;

    switch (a.ty ? a.ty->kind : TY_ANY) {
    case TY_INT: {
        LLVMTypeRef p[1] = { g->i64 };
        LLVMValueRef v[1] = { a.v };
        return val_make(type_prim(TY_INT),
                        LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                       rt_decl(g, "lume_print_i64", ret, p, 1), v, 1, "p"));
    }
    case TY_FLOAT: {
        LLVMTypeRef p[1] = { g->dbl };
        LLVMValueRef v[1] = { a.v };
        return val_make(type_prim(TY_INT),
                        LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                       rt_decl(g, "lume_print_double", ret, p, 1), v, 1, "p"));
    }
    case TY_BOOL: {
        LLVMTypeRef p[1] = { g->i64 };
        LLVMValueRef w = LLVMBuildZExt(g->ab, a.v, g->i64, "p");
        LLVMValueRef v[1] = { w };
        return val_make(type_prim(TY_INT),
                        LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                       rt_decl(g, "lume_print_bool", ret, p, 1), v, 1, "p"));
    }
    case TY_STRING: {
        LLVMTypeRef p[1] = { g->i8ptr };
        LLVMValueRef v[1] = { a.v };
        return val_make(type_prim(TY_INT),
                        LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                       rt_decl(g, "lume_print_str", ret, p, 1), v, 1, "p"));
    }
    case TY_LIST: {
        LLVMTypeRef p[1] = { g->i8ptr };
        LLVMValueRef v[1] = { a.v };
        return val_make(type_prim(TY_INT),
                        LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                       rt_decl(g, "lume_list_print", ret, p, 1), v, 1, "p"));
    }
    case TY_STRUCT:
        if (a.ty->name)
            ERRV(g, "line %zu: print() cannot print a struct value", n->line);
        {   /* an anonymous struct is a runtime map */
            LLVMTypeRef p[1] = { g->i8ptr };
            LLVMValueRef v[1] = { a.v };
            return val_make(type_prim(TY_INT),
                            LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                           rt_decl(g, "lume_map_print", ret, p, 1),
                                           v, 1, "p"));
        }
    case TY_RESULT:
        /* A Result *is* a map ({ ok: ... } / { err: ... }), so it prints
         * through the same runtime helper. Kept in step with cg_print() in
         * codegen_expr.c, which the text emitter uses. */
        {
            LLVMTypeRef p[1] = { g->i8ptr };
            LLVMValueRef v[1] = { a.v };
            return val_make(type_prim(TY_INT),
                            LLVMBuildCall2(g->ab, LLVMFunctionType(ret, p, 1, 0),
                                           rt_decl(g, "lume_map_print", ret, p, 1),
                                           v, 1, "p"));
        }
    case TY_NULL:
        /* `null` prints the bare word "null" and takes no argument, so it
         * cannot share the typed-argument call shape the switch otherwise
         * builds. */
        return val_make(type_prim(TY_INT),
                        LLVMBuildCall2(g->ab, LLVMFunctionType(ret, NULL, 0, 0),
                                       rt_decl(g, "lume_print_null", ret, NULL, 0),
                                       NULL, 0, "p"));
    default:
        ERRV(g, "line %zu: print() cannot print this value", n->line);
    }
}

/* Lume builtins that lower onto a runtime helper (src/rt.c).
 * The int flavour is fn/pty/rty, the float one fn_f/pty_f/rty_f (NULL when
 * the builtin is not overloaded). */
typedef struct {
    const char *name;
    const char *fn_i, *pty_i, *rty_i; int np_i;
    const char *fn_f, *pty_f, *rty_f; int np_f;
} Bi;

static const Bi BUILTINS[] = {
    { "abs",   "lume_bi_abs",  "i64",    "i64",    1,  NULL,            NULL,          NULL,     0 },
    { "min",   "lume_bi_min",  "i64",    "i64",    2,  "lume_bi_minf",  "double",      "double", 2 },
    { "max",   "lume_bi_max",  "i64",    "i64",    2,  "lume_bi_maxf",  "double",      "double", 2 },
    { "sqrt",  NULL,           NULL,     NULL,     0,  "lume_bi_sqrt",  "double",      "double", 1 },
    { "pow",   NULL,           NULL,     NULL,     0,  "lume_bi_pow",   "double",      "double", 2 },
    { "floor", NULL,           NULL,     NULL,     0,  "lume_bi_floor", "double",      "double", 1 },
    { "ceil",  NULL,           NULL,     NULL,     0,  "lume_bi_ceil",  "double",      "double", 1 },
    { "round", NULL,           NULL,     NULL,     0,  "lume_bi_round", "double",      "double", 1 },
};

static const Bi *builtin_find(const char *name)
{
    for (size_t i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i++)
        if (strcmp(BUILTINS[i].name, name) == 0) return &BUILTINS[i];
    return NULL;
}

/* Returns a value with .v set when the name is a builtin this backend knows;
 * an empty Val when it is something else, so the caller can report the
 * "unknown function" error instead. */
static Val cg_builtin(CG *g, Node *n)
{
    const char *bname = n->as.call.callee->as.var.name;
    int argc = n->as.call.argc;
    const Bi *b = builtin_find(bname);
    AT(g);

    if (b) {
        if (argc < 1)
            ERRV(g, "line %zu: '%s' takes at least one argument", n->line, bname);

        Val *av = (Val *)xmalloc((size_t)argc * sizeof *av);
        for (int i = 0; i < argc; i++) av[i] = cg_expr(g, n->as.call.args[i]);

        int isf = av[0].ty && av[0].ty->kind == TY_FLOAT && b->fn_f;
        const char *fn  = isf ? b->fn_f  : b->fn_i;
        const char *pty = isf ? b->pty_f : b->pty_i;
        if (!fn || !pty)
            ERRV(g, "line %zu: '%s' does not apply to this operand type", n->line, bname);

        /* The helper's parameter type is not always the operand's: `min` has an
         * int and a float flavour and the operand picks one. */
        Type *want = isf ? type_prim(TY_FLOAT) : type_prim(TY_INT);
        for (int i = 0; i < argc; i++)
            if (av[i].ty && av[i].ty->kind != want->kind)
                av[i] = coerce(g, want, av[i], n->line);
        if (g->err[0]) return (Val){ NULL, NULL };

        LLVMTypeRef pt = ty_from_spelling(g, pty);
        LLVMTypeRef rt = ty_from_spelling(g, isf ? b->rty_f : b->rty_i);
        if (!pt || !rt) ERRV(g, "line %zu: bad helper signature for '%s'", n->line, bname);

        LLVMTypeRef *pts = (LLVMTypeRef *)xmalloc(sizeof(LLVMTypeRef) * (argc > 0 ? argc : 1));
        LLVMValueRef *vv = (LLVMValueRef *)xmalloc(sizeof(LLVMValueRef) * (argc > 0 ? argc : 1));
        for (int i = 0; i < argc; i++) { pts[i] = pt; vv[i] = av[i].v; }
        LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(rt, pts, (unsigned)argc, 0),
                                        rt_decl(g, fn, rt, pts, argc), vv, (unsigned)argc, "b");
        free(pts); free(vv); free(av);
        return val_make(want, r);
    }

    /* str() / int() / float(): which helper they need depends on the operand. */
    if (strcmp(bname, "str") == 0 || strcmp(bname, "int") == 0 ||
        strcmp(bname, "float") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: '%s' takes exactly one argument", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        LLVMTypeRef ft = NULL, pty = NULL, rty = NULL;
        Type *rt = NULL;

        if (strcmp(bname, "str") == 0) {
            switch (a.ty ? a.ty->kind : TY_ANY) {
            case TY_INT:    ft = LLVMFunctionType(g->i8ptr, &g->i64, 1, 0);  pty = g->i64;    break;
            case TY_FLOAT:  ft = LLVMFunctionType(g->i8ptr, &g->dbl, 1, 0);  pty = g->dbl;    break;
            case TY_BOOL:   ft = LLVMFunctionType(g->i8ptr, &g->i64, 1, 0);  pty = g->i64;    break;
            default:
                ERRV(g, "line %zu: str() cannot convert this value", n->line);
            }
            rty = g->i8ptr; rt = type_prim(TY_STRING);
        } else if (strcmp(bname, "int") == 0) {
            if (!a.ty || a.ty->kind != TY_FLOAT)
                ERRV(g, "line %zu: int() only converts a float here", n->line);
            ft = LLVMFunctionType(g->i64, &g->dbl, 1, 0); pty = g->dbl; rty = g->i64;
            rt = type_prim(TY_INT);
        } else {
            if (!a.ty || a.ty->kind != TY_INT)
                ERRV(g, "line %zu: float() only converts an int here", n->line);
            ft = LLVMFunctionType(g->dbl, &g->i64, 1, 0); pty = g->i64; rty = g->dbl;
            rt = type_prim(TY_FLOAT);
        }

        const char *name;
        if (strcmp(bname, "str") == 0)
            name = (a.ty && a.ty->kind == TY_FLOAT) ? "lume_bi_str_double"
                 : (a.ty && a.ty->kind == TY_BOOL)  ? "lume_bi_str_bool"
                                                     : "lume_bi_str_i64";
        else
            name = strcmp(bname, "int") == 0 ? "lume_bi_int" : "lume_bi_float";

        /* A comparison result is i1 in LLVM, but the runtime's bool builtins
         * take i64 (the interpreter's bool representation). Zext before the
         * call — otherwise str(1 == 1.0) fails to verify with an i1 argument. */
        LLVMValueRef v[1] = { a.v };
        if (a.ty && a.ty->kind == TY_BOOL && pty == g->i64)
            v[0] = LLVMBuildZExt(g->ab, a.v, g->i64, "boolarg");
        LLVMValueRef r = LLVMBuildCall2(g->ab, ft, rt_decl(g, name, rty, &pty, 1),
                                        v, 1, "b");
        return val_make(rt, r);
    }

    /* len() — the helper follows the static type, so a list and a string both
     * spelled `len(x)` still reach the right one. */
    if (strcmp(bname, "len") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: len() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || (a.ty->kind != TY_LIST && a.ty->kind != TY_STRING))
            ERRV(g, "line %zu: len() needs a list or a string", n->line);
        LLVMTypeRef p[1] = { g->i8ptr };
        LLVMValueRef v[1] = { a.v };
        LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i64, p, 1, 0),
                                        rt_decl(g, a.ty->kind == TY_LIST
                                                    ? "lume_list_len"
                                                    : "lume_bi_len_s",
                                                g->i64, p, 1), v, 1, "len");
        if (!r) ERRV(g, "line %zu: cannot take the length of this value", n->line);
        return val_make(type_prim(TY_INT), r);
    }

    /* push(list, item) / put(map, key, value) — both mutate in place and hand
     * the container back, which is what the interpreter does. */
    if (strcmp(bname, "push") == 0) {
        if (argc != 2)
            ERRV(g, "line %zu: push() takes exactly two arguments", n->line);
        Val l = cg_expr(g, n->as.call.args[0]);
        Val e = cg_expr(g, n->as.call.args[1]);
        const char *fn = list_push_fn(e.ty);
        if (!l.v || !e.v || !l.ty || l.ty->kind != TY_LIST || !fn)
            ERRV(g, "line %zu: push() needs a list and a scalar", n->line);
        if (e.ty->kind == TY_BOOL) e = coerce(g, type_prim(TY_INT), e, n->line);
        LLVMTypeRef ats[2] = { g->i8ptr, ty_of(g, e.ty) };
        LLVMValueRef vs[2] = { l.v, e.v };
        Val r = rt_call(g, type_prim(TY_INT), fn, 2, ats, vs);
        if (!r.v) ERRV(g, "line %zu: push() failed", n->line);
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
        LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, ty_of(g, v.ty) };
        LLVMValueRef vs[3] = { m.v, k.v, v.v };
        Val r = rt_call(g, type_prim(TY_INT), fn, 3, ats, vs);
        if (!r.v) ERRV(g, "line %zu: put() failed", n->line);
        return m;
    }

    /* keys(m) — the helper builds the list, so the result is a list value. */
    if (strcmp(bname, "keys") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: keys() takes exactly one argument", n->line);
        Val a = cg_expr(g, n->as.call.args[0]);
        if (!a.v || !a.ty || a.ty->kind == TY_LIST)
            ERRV(g, "line %zu: keys() needs a map", n->line);
        LLVMTypeRef p[1] = { g->i8ptr };
        LLVMValueRef v[1] = { a.v };
        LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, p, 1, 0),
                                        rt_decl(g, "lume_map_keys", g->i8ptr, p, 1),
                                        v, 1, "keys");
        if (!r) ERRV(g, "line %zu: keys() failed", n->line);
        return val_make(list_type(), r);
    }

    /* get(m, k[, default]) — an absent key yields the default, so the flavour
     * follows the default and an unannotated get(m, k) in a bool context stays
     * an int read rather than silently becoming a string. */
    if (strcmp(bname, "get") == 0) {
        if (argc < 2 || argc > 3)
            ERRV(g, "line %zu: get() takes two or three arguments", n->line);
        Val m = cg_expr(g, n->as.call.args[0]);
        Val k = cg_expr(g, n->as.call.args[1]);
        Val d = argc == 3 ? cg_expr(g, n->as.call.args[2]) : (Val){ NULL, NULL };
        if (!m.v || !k.v)
            ERRV(g, "line %zu: get() needs a map, a key and a scalar default", n->line);

        const char *fn = "lume_map_get_i";
        Type    *rty = type_prim(TY_INT);
        if (d.v && d.ty && d.ty->kind == TY_FLOAT)       { fn = "lume_map_get_f"; rty = type_prim(TY_FLOAT); }
        else if (d.v && d.ty && d.ty->kind == TY_STRING) { fn = "lume_map_get_s"; rty = type_prim(TY_STRING); }
        /* A container default needs the pointer-returning getter; see the
         * matching branch in cg_builtin() in codegen_expr.c. */
        else if (d.v && d.ty && (d.ty->kind == TY_LIST ||
                                  d.ty->kind == TY_STRUCT ||
                                  d.ty->kind == TY_RESULT))
                                      { fn = "lume_map_get_obj"; rty = type_anon_struct(); }
        if (d.v && d.ty && d.ty->kind == TY_BOOL)
            d = coerce(g, type_prim(TY_INT), d, n->line);

        LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, ty_of(g, rty) };
        LLVMValueRef vs[3] = { m.v, k.v, d.v ? d.v : LLVMConstInt(g->i64, 0, 0) };
        Val r = rt_call(g, rty, fn, d.v ? 3 : 2, ats, vs);
        if (!r.v) ERRV(g, "line %zu: get() failed", n->line);
        return r;
    }

    return (Val){ NULL, NULL };
}

/* Is *v* already an address rather than an aggregate value?
 *
 * A struct travels as a pointer, but which of the two shapes is in hand cannot
 * be read off the frontend type: `cg_var` hands back a variable's address,
 * `cg_struct_lit` hands back an aggregate insertvalue chain, and a call that
 * returns a struct hands back an aggregate. Pointers stopped being typed in
 * LLVM 15, so the frontend type cannot distinguish them either; the value's
 * own LLVM type still can — an aggregate is the struct type, an address is
 * always the bare `ptr`. Passing the wrong shape here is silent: opaque
 * pointers let the verifier accept `%Point**` where `%Point*` is expected and
 * the callee simply loads the pointer's bytes as the first field. */
static int struct_is_addr(LLVMValueRef v)
{
    return v && LLVMTypeOf(v) && LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMPointerTypeKind;
}

static Val cg_call(CG *g, Node *n)
{
    AT(g);

    if (n->as.call.callee && n->as.call.callee->type == N_VAR &&
        strcmp(n->as.call.callee->as.var.name, "print") == 0) {
        if (n->as.call.argc != 1)
            ERRV(g, "line %zu: print() takes exactly one argument", n->line);
        return cg_print(g, n, cg_expr(g, n->as.call.args[0]));
    }

    if (!n->as.call.callee || n->as.call.callee->type != N_VAR)
        ERRV(g, "line %zu: only direct calls to named functions are supported", n->line);

    const char *name = n->as.call.callee->as.var.name;
    Sig *s = sig_find(&g->sigs, name);
    if (!s) {
        /* Only after sig_find: a script that shadows a builtin with its own
         * function must still call the script's. */
        Val bv = cg_builtin(g, n);
        if (bv.v) return bv;
        ERRV(g, "line %zu: call to unknown function '%s'", n->line, name);
    }
    if (n->as.call.argc != s->arity)
        ERRV(g, "line %zu: '%s' expects %d argument(s), got %d",
            n->line, name, s->arity, n->as.call.argc);

    LLVMTypeRef rty = ty_of(g, s->ret);
    if (s->ret && s->ret->kind == TY_STRUCT &&
        (!s->ret->name || !sdef_find(&g->structs, s->ret->name)))
        ERRV(g, "line %zu: '%s' returns a struct this backend cannot build", n->line, name);

    /* A call needs the callee's *symbol* to exist, and bodies are emitted in
     * source order: IR text declares by name the moment it sees a call, but
     * through the C API a forward call (the usual shape — main calling a
     * helper defined below it) has to create it here. cg_function adds the
     * same name and signature later, which returns this symbol and attaches
     * the body to it. */
    {
        char fname[160];
        snprintf(fname, sizeof fname, "L_%s", name);
        if (!LLVMGetNamedFunction(g->mod, fname)) {
            LLVMTypeRef *pts = (LLVMTypeRef *)xmalloc(
                sizeof(LLVMTypeRef) * (s->arity > 0 ? (size_t)s->arity : 1));
            for (int i = 0; i < s->arity; i++)
                pts[i] = ptr_of(g, s->params ? s->params[i] : NULL);
            LLVMTypeRef ft = LLVMFunctionType(rty ? rty : g->void_ty,
                                              s->arity > 0 ? pts : NULL,
                                              (unsigned)s->arity, 0);
            LLVMAddFunction(g->mod, fname, ft);
            free(pts);
        }
    }

    /* Arguments are evaluated first, so a type error cannot leave a half-built
     * call behind. */
    int argc = n->as.call.argc;
    LLVMValueRef *avs = (LLVMValueRef *)xmalloc(sizeof(LLVMValueRef) * (argc > 0 ? argc : 1));
    LLVMTypeRef *ats = (LLVMTypeRef *)xmalloc(sizeof(LLVMTypeRef) * (argc > 0 ? argc : 1));
    for (int i = 0; i < argc; i++) {
        Val a = cg_expr(g, n->as.call.args[i]);
        avs[i] = a.v;
        if (a.ty && a.ty->kind == TY_STRUCT && !struct_is_addr(a.v)) {
            /* An aggregate (an insertvalue chain) has to be materialised into
             * a stack slot before it can travel as a pointer. An address that
             * already is one — a struct variable — is passed straight through:
             * storing it into a slot again would build a `%Point**` and the
             * callee would read the pointer instead of the struct. */
            LLVMValueRef slot = LLVMBuildAlloca(g->ab, ty_of(g, a.ty), "sa");
            LLVMBuildStore(g->ab, a.v, slot);
            a.v    = slot;
            ats[i] = ptr_of(g, a.ty);
        } else {
            ats[i] = ptr_of(g, a.ty);
        }
        avs[i] = a.v;
        if (!ats[i]) ERRV(g, "line %zu: bad argument type for '%s'", n->line, name);
    }

    char fname[160];
    snprintf(fname, sizeof fname, "L_%s", name);
    LLVMValueRef callee = LLVMGetNamedFunction(g->mod, fname);
    if (!callee) ERRV(g, "line %zu: '%s' was never defined", n->line, name);

    /* The call type is rebuilt from the signature rather than read back off the
     * callee: LLVMGetElementType(LLVMTypeOf(callee)) no longer yields the
     * function type on LLVM ≥ 20 (opaque pointers), and a wrong call type is
     * reported by the verifier as "Incorrect number of arguments", which
     * points nowhere near the real mistake. Going through the same helpers
     * cg_function used guarantees the two agree. */
    if (s->arity > 64)
        ERRV(g, "line %zu: '%s' has too many parameters", n->line, name);
    LLVMTypeRef pt[65];
    for (int i = 0; i < s->arity; i++)
        pt[i] = ptr_of(g, s->params ? s->params[i] : NULL);
    LLVMTypeRef ft = LLVMFunctionType(rty ? rty : g->void_ty,
                                      s->arity > 0 ? pt : NULL,
                                      (unsigned)s->arity, 0);

    /* The result, not an undef: handing back an undef placeholder here is what
     * turns `n * fact(n - 1)` into `mul i64 %v, undef` and quietly produces
     * wrong numbers instead of a failed build. A void call still has a value
     * (the call instruction); its type is what tells callers to ignore it. */
    /* A void call must stay anonymous (the verifier rejects a call with a name
     * that returns nothing) and the name slot must not be NULL (this libLLVM
     * segfaults on it), so an empty string is the only safe spelling — see the
     * L_top call in llvm_codegen_module. */
    LLVMValueRef res = LLVMBuildCall2(g->ab, ft, callee, avs,
                                      (unsigned)argc, s->ret ? "c" : "");
    free(ats); free(avs);
    if (!res) ERRV(g, "line %zu: failed to emit the call to '%s'", n->line, name);
    /* Only the captured call survives past cg_function: it is the value the
     * synthetic top body returns, and from there the entry's exit status. */
    if (g->capture_main && rty && strcmp(name, "main") == 0)
        g->exit_val = res;

    /* `f()?` — see the matching block in cg_call() in codegen_expr.c for why
     * this has to branch rather than read `ok` directly. Kept in step with that
     * copy deliberately: the two emitters are separate implementations of one
     * lowering, and a change made to only one of them shows up as a backend
     * that accepts what the other rejects. */
    if (n->as.call.propagate) {
        Type *payload = s->ret ? s->ret->elem : NULL;
        if (!payload || payload->kind == TY_ANY)
            ERRV(g, "line %zu: '?' needs '%s' to return at least one "
                    "{ ok: <value> } whose type is known", n->line, name);
        const char *get_fn = map_get_fn(payload);
        if (!get_fn)
            ERRV(g, "line %zu: '?' cannot unwrap a payload of type '%s'",
                 n->line, src_type_name(payload));

        /* Same three-block shape the short-circuit and/or lowering uses: park
         * the payload in an alloca rather than a phi, which is what the rest of
         * this emitter does for every local. The `err` block returns the Result
         * unchanged -- `?` moves the failure out of this function, it does not
         * rewrite it -- and the join block reads the payload back. */
        LLVMTypeRef pty   = ty_of(g, payload);
        LLVMValueRef slot = LLVMBuildAlloca(g->ab, pty, "prop");

        LLVMBasicBlockRef bb_err = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "prop_err");
        LLVMBasicBlockRef bb_ok  = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "prop_ok");
        LLVMBasicBlockRef bb_end = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "prop_end");

        LLVMValueRef errk = cg_string_val(g, "err", 3);
        LLVMValueRef has  = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i64, (LLVMTypeRef[]){ g->i8ptr, g->i8ptr }, 2, 0),
                rt_decl(g, "lume_map_has", g->i64,
                        (LLVMTypeRef[]){ g->i8ptr, g->i8ptr }, 2),
                (LLVMValueRef[]){ res, errk }, 2, "has");
        LLVMValueRef cond = LLVMBuildICmp(g->ab, LLVMIntNE, has,
                                          LLVMConstInt(g->i64, 0, 0), "iserr");
        if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, cond, bb_err, bb_ok);

        g->cur = bb_err; AT(g);
        if (!DONE(g->cur)) LLVMBuildRet(g->ab, res);
        if (!DONE(g->cur)) LLVMBuildBr(g->ab, bb_end);

        g->cur = bb_ok; AT(g);
        LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, pty };
        LLVMValueRef dflt = payload->kind == TY_FLOAT ? LLVMConstReal(pty, 0.0)
                            : pty == g->i8ptr         ? LLVMConstNull(g->i8ptr)
                                                     : LLVMConstInt(pty, 0, 0);
        LLVMValueRef okk = cg_string_val(g, "ok", 2);
        LLVMValueRef pay = LLVMBuildCall2(g->ab,
                LLVMFunctionType(pty, ats, 3, 0),
                rt_decl(g, get_fn, pty, ats, 3),
                (LLVMValueRef[]){ res, okk, dflt }, 3, "p");
        if (!DONE(g->cur)) LLVMBuildStore(g->ab, pay, slot);
        if (!DONE(g->cur)) LLVMBuildBr(g->ab, bb_end);

        g->cur = bb_end; AT(g);
        return val_make(payload, LLVMBuildLoad2(g->ab, pty, slot, "pv"));
    }

    return val_make(s->ret, res);
}

static Val cg_expr(CG *g, Node *n)
{
    if (!n) ERRV(g, "internal: null expression");

    switch (n->type) {
    case N_LITERAL:  return cg_literal(g, n);
    case N_LIST_LIT: return cg_list_lit(g, n);
    case N_MAP_LIT:  return cg_map_or_struct_lit(g, n);
    case N_ASSIGN:   return cg_assign_expr(g, n);
    case N_ASSIGN_MEMBER: return cg_assign_mem(g, n);
    case N_EXPR_STMT: return cg_expr(g, n->as.expr_stmt.expr);
    case N_VAR:      return cg_var(g, n);
    case N_UNARY:    return cg_unary(g, n);
    case N_BINARY:   return cg_binary(g, n);
    case N_MEMBER:   return cg_member(g, n);
    case N_INDEX:    return cg_index(g, n);
    case N_CALL:     return cg_call(g, n);
    default:         ERRV(g, "%s", bad(g, n, "this expression"));
    }
}

/* `m["k"]` / `l[0]`.
 *
 * Lowered the same way as the text backend: index first, then container
 * (matching the interpreter's evaluation order so a key with a side effect runs
 * first), and the read goes through the unified lume_index_* entry points
 * rather than a per-shape accessor chosen here. The emitter often cannot tell a
 * list from a map -- `m["p"][1]` hands over an opaque i8* and a runtime map's
 * value type is written down nowhere -- so the container's own kind does the
 * dispatch, in rt.c, once.
 *
 * Strictness (a missing key or out-of-range index stopping the program) has to
 * be a call the emitted program makes, exactly as in the text backend: the
 * read sets g_index_failed and lume_index_error() reports and exits, like every
 * other fatal native condition. The three-block shape (bad / ok, the join is
 * implicit -- the read's result is used directly from the pre-branch block)
 * follows the `?` propagation lowering in cg_call. */
static Val cg_index(CG *g, Node *n)
{
    Val ix  = cg_expr(g, n->as.index.index);
    if (!ix.v) ERRV(g, "line %zu: bad index expression", n->line);
    Val obj = cg_expr(g, n->as.index.obj);
    if (!obj.v) ERRV(g, "line %zu: bad index operand", n->line);

    int known_map  = obj.ty && obj.ty->kind == TY_STRUCT && !obj.ty->name;
    int known_list = obj.ty && obj.ty->kind == TY_LIST;
    if (!known_map && !known_list) {
        Type *k = obj.ty;
        int opaque = k && (k->kind == TY_STRUCT || k->kind == TY_LIST);
        if (!opaque)
            ERRV(g, "line %zu: cannot index into a value of type '%s'", n->line,
                 k ? src_type_name(k) : "unknown");
    }

    Type *rty = known_list ? (obj.ty->elem ? obj.ty->elem : any_type())
                           : type_anon_struct();
    LLVMTypeRef ret_ty;
    const char *fn;
    switch (rty->kind) {
    case TY_FLOAT:
        ret_ty = g->dbl;  fn = "lume_index_float";  break;
    case TY_STRING: case TY_STRUCT: case TY_LIST: case TY_NULL: case TY_ANY:
        ret_ty = g->i8ptr; fn = "lume_index_ptr";  break;
    default:
        ret_ty = g->i64;  fn = "lume_index_int";   break;  /* int / bool */
    }

    /* The index is an i64 for a list and an i8* for a map, passed in that order
     * so the runtime uses whichever its container needs. Which slot the index
     * travels in is the one thing knowable here, and it is the same rule the
     * text backend's rt_arg_type() applies: a string / list / struct / result
     * index is a pointer (the key), everything else an integer (the index). An
     * `any` index lowers to i64 on both backends, so a backend that refused it
     * for the wrong reason refuses it the same way as its sibling. */
    int ix_is_str;
    if (ix.ty) {
        switch (ix.ty->kind) {
        case TY_STRING: case TY_LIST: case TY_STRUCT: case TY_RESULT:
            ix_is_str = 1; break;
        default:
            ix_is_str = 0;
        }
    } else ix_is_str = 0;

    LLVMTypeRef iparams[3] = { g->i8ptr, g->i64, g->i8ptr };
    LLVMValueRef iargs[3];
    iargs[0] = obj.v;
    if (ix_is_str) {
        iargs[1] = LLVMConstInt(g->i64, 0, 0);
        iargs[2] = ix.v;
    } else {
        iargs[1] = ix.v;
        iargs[2] = LLVMConstNull(g->i8ptr);
    }

    LLVMValueRef call = LLVMBuildCall2(g->ab,
            LLVMFunctionType(ret_ty, iparams, 3, 0),
            rt_decl(g, fn, ret_ty, iparams, 3),
            iargs, 3, "idx");

    /* Strictness, at run time. lume_index_failed() is non-zero when the read
     * did not happen, and lume_index_error() reports and exits. */
    LLVMBasicBlockRef bb_bad = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "idx_bad");
    LLVMBasicBlockRef bb_ok  = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "idx_ok");

    LLVMValueRef failed = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->i64, NULL, 0, 0),
            rt_decl(g, "lume_index_failed", g->i64, NULL, 0),
            NULL, 0, "idxfail");
    LLVMValueRef cond = LLVMBuildICmp(g->ab, LLVMIntNE, failed,
                                      LLVMConstInt(g->i64, 0, 0), "idxbad");
    if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, cond, bb_bad, bb_ok);

    g->cur = bb_bad; AT(g);
    LLVMValueRef what = cg_string_val(g,
            known_map ? "no such key in this map" : "index out of range",
            known_map ? 23 : 19);
    LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->void_ty, (LLVMTypeRef[]){ g->i8ptr }, 1, 0),
            rt_decl(g, "lume_index_error", g->void_ty, (LLVMTypeRef[]){ g->i8ptr }, 1),
            (LLVMValueRef[]){ what }, 1, "");
    if (!DONE(g->cur)) LLVMBuildUnreachable(g->ab);

    g->cur = bb_ok; AT(g);
    return val_make(rty, call);
}

/* -------------------------------------------------------------- scanning ---- */

/* The static type of `expr.field` without emitting anything. The entry-block
 * alloca pass runs before any statement is emitted, so a `for (v in bag.xs)`
 * has to be able to answer "what type is v" from the field's declared type —
 * otherwise the loop variable is registered untyped and LLVMBuildAlloca is
 * handed a NULL type. */
static Type *member_field_type(CG *g, Node *n)
{
    Type *t = infer_node_type(g, n->as.member.obj);
    if (t && t->kind == TY_STRUCT && t->name) {
        int idx = struct_field_idx(g, t->name, n->as.member.name);
        if (idx >= 0) return struct_field_type(g, t->name, idx);
    }
    return NULL;
}

static Type *infer_node_type(CG *g, Node *n)
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
    case N_VAR: {
        Asg *a = asg_find(&g->locals, n->as.var.name);
        return a ? a->ty : NULL;
    }
    case N_CALL: {
        if (!n->as.call.callee || n->as.call.callee->type != N_VAR) return NULL;
        Sig *s = sig_find(&g->sigs, n->as.call.callee->as.var.name);
        if (!s) return NULL;
        /* `f()?` is the `ok` payload, not a Result -- see the same branch in
         * codegen_scan.c. Reading the declared return type here alloca'd a
         * pointer for an int payload and the emitter then printed the integer
         * through lume_map_print, which segfaulted. */
        if (n->as.call.propagate) return s->ret ? s->ret->elem : NULL;
        return s->ret;
    }
    case N_LIST_LIT: { Type *et = infer_list_elem(n); return type_list(et ? et : any_type()); }
    case N_INDEX: {
        /* Kept in step with codegen_scan.c's copy -- the two backends each carry
         * their own infer_node_type, and a kind handled in one but not the other
         * shows up as a backend that refuses what the other accepts. */
        Type *ot = infer_node_type(g, n->as.index.obj);
        if (ot && ot->kind == TY_LIST)
            return ot->elem && ot->elem->kind != TY_ANY ? ot->elem : any_type();
        return type_anon_struct();
    }
    case N_MAP_LIT:  return type_anon_struct();
    case N_MEMBER:   return member_field_type(g, n);
    case N_UNARY: {
        /* `let neg = -7` / `let b = not flag` must infer a type so the
         * binding's alloca gets an IR type; otherwise the native backend
         * falls back to "cannot infer a type for 'neg'" while the
         * interpreter accepts it (§8.1 #2). */
        Type *ot = infer_node_type(g, n->as.unary.operand);
        if (n->as.unary.op == OP_NOT) return type_prim(TY_BOOL);
        if (!ot) return NULL;
        return type_prim(ot->kind == TY_FLOAT ? TY_FLOAT : TY_INT);
    }
    case N_BINARY: {
        int op = (int)n->as.binary.op;
        if (op == OP_AND || op == OP_OR) return type_prim(TY_BOOL);
        if (op >= OP_EQ && op <= OP_GE)  return type_prim(TY_BOOL);
        Type *lt = infer_node_type(g, n->as.binary.left);
        Type *rt = infer_node_type(g, n->as.binary.right);
        bool lf = lt && lt->kind == TY_FLOAT;
        bool rf = rt && rt->kind == TY_FLOAT;
        /* Catch float % at inference so the libLLVM backend rejects it with
         * the same "'%' does not apply to floats" the text backend and the
         * interpreter use, instead of a downstream "print() cannot print
         * this value" (§8.1 #1). */
        if (op == OP_MOD && (lf || rf))
            ERR(g, "line %zu: '%%' does not apply to floats", n->line);
        if (op == OP_ADD && lt && rt &&
            lt->kind == TY_STRING && rt->kind == TY_STRING)
            return type_prim(TY_STRING);
        if (lf || rf) return type_prim(TY_FLOAT);
        return type_prim(TY_INT);
    }
    default:
        return NULL;
    }
}

/* The type of a `for (x in xs)` loop variable: a list yields its element type,
 * a map yields strings (it iterates its keys), anything else is an error --
 * a value is just an i8* here, so which reader a for-in has to call is a
 * static question and `list<any>` (a heterogeneous literal, say) has none. */
static Type *for_in_elem_type(CG *g, Node *n)
{
    Type *t = infer_node_type(g, n->as.fors.iterable);
    if (t && t->kind == TY_LIST) {
        if (!t->elem)
            ERR(g, "line %zu: cannot infer what a list of this type holds to "
                   "iterate it (annotate the list)", n->line);
        return t->elem;
    }
    /* An anonymous struct literal is a runtime map, which iterates its keys. */
    if (t && t->kind == TY_STRUCT && !t->name) return type_prim(TY_STRING);
    ERR(g, "line %zu: the native backend can only iterate a list or a map in "
           "a 'for ... in' loop", n->line);
    return NULL;   /* the checker is never the one that reads this path */
}

/* Collect the variables a block introduces, so the entry block can allocate
 * all of them before any statement is emitted. */
static void scan_block(CG *g, Node *blk);

static void scan_stmt(CG *g, Node *n)
{
    if (!n) return;

    switch (n->type) {
    case N_LET: {
        Type *ty = n->as.let.annot ? n->as.let.annot : infer_node_type(g, n->as.let.init);
        if (!ty)
            ERRX(g, "line %zu: cannot infer a type for '%s' (add an explicit annotation)",
                 n->line, n->as.let.name);
        asg_push(&g->locals, n->as.let.name, ty, NULL);
        break;
    }
    case N_FOR: {
        /* `for (x in xs)` binds the loop variable — it is not a `let`, so
         * without registering it here the body's use of it dies in codegen. */
        if (n->as.fors.is_in) {
            asg_push(&g->locals, n->as.fors.var, for_in_elem_type(g, n), NULL);
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
            asg_push(&g->locals, init->as.let.name, ty, NULL);
        }
        scan_stmt(g, n->as.fors.body);
        break;
    }
    /* A nested block is the body of if/while/for and any bare { } — its `let`s
     * have to be registered too, or every use of such a local dies in codegen
     * ("bad operands" on a read, "unknown variable" on an assignment). */
    case N_BLOCK:  scan_block(g, n); break;
    case N_IF:     scan_stmt(g, n->as.ifs.then); scan_stmt(g, n->as.ifs.els); break;
    case N_WHILE:  scan_stmt(g, n->as.whiles.body); break;
    default: break;
    }
}

static void scan_block(CG *g, Node *blk)
{
    if (!blk) return;
    for (int i = 0; i < blk->as.block.count; i++) scan_stmt(g, blk->as.block.stmts[i]);
}

/* ------------------------------------------------------------- statements --- */

static Val coerce(CG *g, Type *to, Val v, size_t line)
{
    if (!to || !v.ty || to == v.ty) return v;

    /* `null` is representable natively as an opaque i8* null pointer, and the
     * type checker lets it stand in for any type (`type_compat`: "all types
     * nullable"). But the pointer has no scalar meaning: without this guard
     * the ptrtoint/intcast edges below turned a null into 0 (or false), so
     * `let x: int = null; print(x)` printed a confident, wrong 0 where the
     * interpreter printed `null`. Reject it instead -- silently substituting a
     * value is worse than refusing to compile. Pointer-shaped targets
     * (string/list/struct) stay legal; lume_print_str and friends absorb the
     * null. */
    if (v.ty->kind == TY_NULL && to->kind != TY_STRING && to->kind != TY_LIST &&
        to->kind != TY_STRUCT && to->kind != TY_ANY)
        ERRV(g, "line %zu: cannot use 'null' as a %s value in the native backend",
             line, src_type_name(to));

    LLVMTypeRef tt = ty_of(g, to);
    LLVMTypeRef vt = ty_of(g, v.ty);
    if (!tt || !vt || tt == vt) return v;

    AT(g);
    LLVMValueRef r;
    if (vt == g->i1 && tt == g->i64)       r = LLVMBuildZExt(g->ab, v.v, tt, "co");
    else if (vt == g->i64 && tt == g->i1)  r = LLVMBuildICmp(g->ab, LLVMIntNE, v.v,
                                                              LLVMConstInt(g->i64, 0, 0), "co");
    else if (vt == g->i64 && tt == g->dbl) r = LLVMBuildSIToFP(g->ab, v.v, tt, "co");
    else if (vt == g->dbl && tt == g->i64) r = LLVMBuildFPToSI(g->ab, v.v, tt, "co");
    else if (vt == g->i8ptr && tt == g->i64) r = LLVMBuildPtrToInt(g->ab, v.v, tt, "co");
    else if (vt == g->i64 && tt == g->i8ptr) r = LLVMBuildIntToPtr(g->ab, v.v, tt, "co");
    else r = LLVMBuildIntCast2(g->ab, v.v, tt, 1, "co");
    if (!r) ERRV(g, "line %zu: cannot convert this value", line);
    return val_make(to, r);
}

/* Branch unless the current block already ended. This is what makes
 * `if (c) { return 1; }` legal without a trailing `unreachable`. */
static void br_to(CG *g, LLVMBasicBlockRef target)
{
    if (!DONE(g->cur)) LLVMBuildBr(g->ab, target);
}

/* Returns the stored value: `p.x = e` is both a statement and an expression,
 * and the parser wraps a bare `p.x = e;` in an expr-stmt, so the store has to
 * be reachable from cg_expr too. */
static Val cg_assign_mem(CG *g, Node *n)
{
    AT(g);
    /* The node carries no type: the parser's assign_mem is only {obj, name,
     * value}, so the struct spelling comes from the object's own type.
     * Reading n->as.member.type here would read the value node through a
     * Type * and compare garbage against the struct names. */
    Val obj = cg_expr(g, n->as.assign_mem.obj);
    struct_addr(g, &obj);
    Type *st = obj.ty;
    if (!st || st->kind != TY_STRUCT || !st->name)
        ERRV(g, "line %zu: field '%s' on a non-struct value",
             n->line, n->as.assign_mem.name);
    int idx = struct_field_idx(g, st->name, n->as.assign_mem.name);
    if (idx < 0) ERRV(g, "line %zu: unknown field '%s' on '%s'",
                      n->line, n->as.assign_mem.name, st->name);

    Val val = cg_expr(g, n->as.assign_mem.value);

    Type *fty = struct_field_type(g, st->name, idx);
    if (!fty || !ty_of(g, fty))
        ERRV(g, "line %zu: field '%s' has no codegen type", n->line, n->as.assign_mem.name);

    LLVMValueRef p = gep_field(g, st, obj.v, idx);
    if (!p) ERRV(g, "line %zu: cannot take the address of '%s'", n->line, n->as.assign_mem.name);
    LLVMBuildStore(g->ab, val.v, p);
    return val;
}

static void cg_while(CG *g, Node *n)
{
    AT(g);
    LLVMBasicBlockRef c = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "while.cond");
    LLVMBasicBlockRef b = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "while.body");
    LLVMBasicBlockRef e = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "while.end");

    push_bb(&g->brk, &g->nbrk, &g->cbrk, e);
    push_bb(&g->cnt, &g->ncnt, &g->ccnt, c);

    if (!DONE(g->cur)) LLVMBuildBr(g->ab, c);

    g->cur = c; AT(g);
    LLVMValueRef cond = cg_expr(g, n->as.whiles.cond).v;
    if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, cond, b, e);

    g->cur = b; AT(g);
    cg_stmt(g, n->as.whiles.body);
    br_to(g, c);

    g->cur = e; AT(g);
    g->nbrk--; g->ncnt--;
}

/* Which element accessor reads a list whose element type is `ty`. */
static const char *list_at_fn(Type *ty)
{
    if (!ty) return NULL;
    switch (ty->kind) {
    case TY_FLOAT:  return "lume_list_at_f";
    case TY_STRING: return "lume_list_at_s";
    case TY_INT:
    case TY_BOOL:   return "lume_list_at_i";
    default:        return NULL;
    }
}

/* `for (x in xs)` / `for (k in m)`: the loop variable is an index into a heap
 * object reached through an opaque pointer, read through the accessor the
 * static element type picked. The iterable is evaluated once, but the length
 * is re-read every iteration — that is what the interpreter's for-in does and
 * what makes a `push(xs, v)` inside the body visible to the loop. */
static void cg_for_in(CG *g, Node *n)
{
    AT(g);
    Val it = cg_expr(g, n->as.fors.iterable);
    if (!it.v) ERRX(g, "line %zu: bad iterable", n->line);

    /* Only a map iterates its keys, so that spelling is the test. */
    bool is_map = it.ty && it.ty->kind == TY_STRUCT && !it.ty->name;

    Asg *a = asg_find(&g->locals, n->as.fors.var);
    if (!a) ERRX(g, "line %zu: unknown variable '%s'", n->line, n->as.fors.var);
    Type *et = a->ty;
    const char *fn = is_map ? NULL : list_at_fn(et);
    if (!is_map && !fn)
        ERRX(g, "line %zu: a list of this element type cannot be iterated", n->line);

    LLVMValueRef idx = LLVMBuildAlloca(g->ab, g->i64, "li");
    LLVMBuildStore(g->ab, LLVMConstInt(g->i64, 0, 0), idx);

    /* Snapshot the collection length once, before the loop, to match the
     * interpreter (which captures list.count at loop entry). Re-reading it
     * every iteration made `push(xs, v)` inside the body extend the bound
     * forever and OOM the backend. */
    LLVMValueRef len_slot = LLVMBuildAlloca(g->ab, g->i64, "len");
    LLVMValueRef len0 = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i64, &g->i8ptr, 1, 0),
                                       rt_decl(g, is_map ? "lume_map_len" : "lume_list_len",
                                                g->i64, &g->i8ptr, 1),
                                       &it.v, 1, "len0");
    LLVMBuildStore(g->ab, len0, len_slot);

    LLVMBasicBlockRef c = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.cond");
    LLVMBasicBlockRef b = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.body");
    LLVMBasicBlockRef i = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.incr");
    LLVMBasicBlockRef e = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.end");

    push_bb(&g->brk, &g->nbrk, &g->cbrk, e);
    /* `continue` has to reach the increment block, not the condition: jumping
     * back to `c` would skip the index bump and spin forever on element 0.
     * The incr block ends with a branch to `c`, so the re-test still runs. */
    push_bb(&g->cnt, &g->ncnt, &g->ccnt, i);

    if (!DONE(g->cur)) LLVMBuildBr(g->ab, c);

    g->cur = c; AT(g);
    LLVMValueRef len = LLVMBuildLoad2(g->ab, g->i64, len_slot, "len");
    LLVMValueRef cur = LLVMBuildLoad2(g->ab, g->i64, idx, "cur");
    LLVMValueRef cond = LLVMBuildICmp(g->ab, LLVMIntSLT, cur, len, "c");
    if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, cond, b, e);

    g->cur = b; AT(g);
    LLVMValueRef arg[2] = { it.v, cur };
    /* The accessor is declared with its real signature rather than through
     * rt_call, because the result type differs per element kind. */
    {
        LLVMTypeRef pts[2] = { g->i8ptr, g->i64 };
        Type *rt = et;
        if (is_map) rt = type_prim(TY_STRING);
        else if (rt->kind == TY_BOOL) rt = type_prim(TY_INT);   /* narrowed below */
        LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(ty_of(g, rt), pts, 2, 0),
                                        rt_decl(g, is_map ? "lume_map_key_at" : fn,
                                                ty_of(g, rt), pts, 2),
                                        arg, 2, "e");
        LLVMValueRef store = r;
        if (et->kind == TY_BOOL) store = LLVMBuildICmp(g->ab, LLVMIntNE,
                                                        r, LLVMConstInt(g->i64, 0, 0), "n");
        LLVMBuildStore(g->ab, store, a->slot);
    }
    cg_stmt(g, n->as.fors.body);
    br_to(g, i);

    g->cur = i; AT(g);
    LLVMValueRef nxt = LLVMBuildAdd(g->ab, cur, LLVMConstInt(g->i64, 1, 0), "nxt");
    LLVMBuildStore(g->ab, nxt, idx);
    br_to(g, c);

    g->cur = e; AT(g);
    g->nbrk--; g->ncnt--;
}

static void cg_for(CG *g, Node *n)
{
    AT(g);
    if (n->as.fors.is_in) { cg_for_in(g, n); return; }

    if (n->as.fors.init) cg_stmt(g, n->as.fors.init);

    LLVMBasicBlockRef c = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.cond");
    LLVMBasicBlockRef b = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.body");
    LLVMBasicBlockRef i = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.incr");
    LLVMBasicBlockRef e = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "for.end");

    push_bb(&g->brk, &g->nbrk, &g->cbrk, e);
    /* Same rule as the `for ... in` loop above: `continue` must land on the
     * increment block, or the loop variable never advances. */
    push_bb(&g->cnt, &g->ncnt, &g->ccnt, i);

    if (!DONE(g->cur)) LLVMBuildBr(g->ab, c);

    g->cur = c; AT(g);
    if (n->as.fors.cond) {
        LLVMValueRef cond = cg_expr(g, n->as.fors.cond).v;
        if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, cond, b, e);
    } else if (!DONE(g->cur)) {
        LLVMBuildBr(g->ab, b);
    }

    g->cur = b; AT(g);
    cg_stmt(g, n->as.fors.body);
    br_to(g, i);

    g->cur = i; AT(g);
    if (n->as.fors.incr) {
        /* The increment is a *statement*: `k = k + 1` is an assignment node
         * wrapped in an expression statement. */
        if (n->as.fors.incr->type == N_ASSIGN || n->as.fors.incr->type == N_EXPR_STMT)
            cg_stmt(g, n->as.fors.incr);
        else
            (void)cg_expr(g, n->as.fors.incr);
    }
    br_to(g, c);

    g->cur = e; AT(g);
    g->nbrk--; g->ncnt--;
}

static void cg_if(CG *g, Node *n)
{
    AT(g);
    LLVMBasicBlockRef t = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "if.then");
    LLVMBasicBlockRef f = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "if.else");
    LLVMBasicBlockRef e = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "if.end");

    LLVMValueRef cond = cg_expr(g, n->as.ifs.cond).v;
    if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, cond, t, f);

    g->cur = t; AT(g);
    cg_stmt(g, n->as.ifs.then);
    br_to(g, e);

    g->cur = f; AT(g);
    if (n->as.ifs.els) cg_stmt(g, n->as.ifs.els);
    br_to(g, e);

    g->cur = e; AT(g);
}

static void cg_stmt(CG *g, Node *n)
{
    if (!n) return;

    switch (n->type) {
    case N_LET: {
        AT(g);
        /* Hand the annotation to struct literals before evaluating them. */
        Type *prev = g->expect;
        g->expect = n->as.let.annot;
        Val v = cg_expr(g, n->as.let.init);
        g->expect = prev;

        /* Prefer the annotation so the stored value matches the slot. */
        Type *ty = n->as.let.annot ? n->as.let.annot : v.ty;
        v = coerce(g, ty, v, n->line);
        if (g->err[0]) break;   /* coerce refused (e.g. null into a scalar) */
        if (!ty_of(g, ty))
            ERRX(g, "line %zu: cannot store this into a typed local", n->line);

        Asg *a = asg_find(&g->locals, n->as.let.name);
        if (!a) ERRX(g, "internal: '%s' was not allocated by the scan pass",
                     n->as.let.name);
        if (!a->slot) a->slot = LLVMBuildAlloca(g->ab, ty_of(g, ty), "lv");
        LLVMBuildStore(g->ab, v.v, a->slot);
        break;
    }

    case N_ASSIGN:
        (void)cg_assign_expr(g, n);
        break;

    case N_ASSIGN_MEMBER:
        (void)cg_assign_mem(g, n);
        break;

    case N_RETURN: {
        AT(g);
        if (!n->as.ret.expr) {
            if (!DONE(g->cur)) LLVMBuildRetVoid(g->ab);
            break;
        }
        /* `return { .. }` needs the function's declared return type. */
        Val v = cg_expr(g, n->as.ret.expr);
        LLVMTypeRef rty = ty_of(g, g->cur_ret);
        if (!rty) rty = ty_of(g, v.ty);
        if (!rty) ERRX(g, "line %zu: cannot return this value", n->line);
        if (rty == g->void_ty) { if (!DONE(g->cur)) LLVMBuildRetVoid(g->ab); break; }
        LLVMValueRef val = v.v;
        if (v.ty && g->cur_ret && v.ty != g->cur_ret) {
            val = coerce(g, g->cur_ret, v, n->line).v;
            if (!val) ERRX(g, "line %zu: cannot return this value", n->line);
        }
        if (!DONE(g->cur)) LLVMBuildRet(g->ab, val);
        break;
    }

    case N_IF:      cg_if(g, n); break;
    case N_WHILE:   cg_while(g, n); break;
    case N_FOR:     cg_for(g, n); break;

    case N_BREAK:
        if (g->nbrk == 0) ERRX(g, "line %zu: 'break' outside a loop", n->line);
        br_to(g, g->brk[g->nbrk - 1]);
        break;

    case N_CONTINUE:
        if (g->ncnt == 0) ERRX(g, "line %zu: 'continue' outside a loop", n->line);
        br_to(g, g->cnt[g->ncnt - 1]);
        break;

    case N_EXPR_STMT:
        (void)cg_expr(g, n->as.expr_stmt.expr);
        break;

    case N_BLOCK:
        cg_block(g, n);
        break;

    default:
        ERRX(g, "%s", bad(g, n, "this construct"));
    }
}

static void cg_block(CG *g, Node *blk)
{
    if (!blk) return;
    for (int i = 0; i < blk->as.block.count; i++) cg_stmt(g, blk->as.block.stmts[i]);
}

/* ------------------------------------------------------------- functions ---- */

static void cg_function(CG *g, Node *fn)
{
    Type **params = fn->as.func.param_types;
    int    arity  = fn->as.func.arity;
    const char *lume_name = fn->as.func.name;

    LLVMTypeRef *pts = NULL;
    if (params) {
        pts = (LLVMTypeRef *)xmalloc(sizeof(LLVMTypeRef) * (arity > 0 ? arity : 1));
        for (int i = 0; i < arity; i++) {
            pts[i] = ptr_of(g, params[i]);
            if (!pts[i])
                ERRX(g, "line %zu: parameter '%s' has no codegen type",
                     fn->line, fn->as.func.names ? fn->as.func.names[i] : "?");
        }
    }

    LLVMTypeRef rty = ty_of(g, fn->as.func.ret);
    LLVMTypeRef fty = LLVMFunctionType(rty ? rty : g->void_ty, pts, (unsigned)arity, 0);

    char fname[160];
    snprintf(fname, sizeof fname, "L_%s", lume_name ? lume_name : "?");
    /* LLVMAddFunction always creates and then *uniques* the name (L_g.1, ...)
     * — the get-or-insert semantics live in the C++ helper a call goes
     * through, not in this API. A forward call already declared this symbol,
     * so it is reused instead; otherwise the body would land on a copy. */
    LLVMValueRef prev = LLVMGetNamedFunction(g->mod, fname);
    g->fn = prev ? prev : LLVMAddFunction(g->mod, fname, fty);

    /* Locals move aside: a variable of the caller must not be visible here. */
    Asgs saved = g->locals;
    memset(&g->locals, 0, sizeof g->locals);
    Type *saved_ret = g->cur_ret;
    g->cur_ret = fn->as.func.ret;

    /* Parameters first, so their slots exist before the body is scanned. A
     * struct parameter is *already* a pointer, so it keeps the parameter value
     * as its slot. */
    LLVMValueRef *pv = NULL;
    if (arity > 0) {
        pv = (LLVMValueRef *)xmalloc(sizeof(LLVMValueRef) * (size_t)arity);
        LLVMGetParams(g->fn, pv);
        for (int i = 0; i < arity; i++)
            asg_push(&g->locals, fn->as.func.names[i], params[i],
                     (params[i] && params[i]->kind == TY_STRUCT) ? pv[i] : NULL);
    }

    scan_block(g, fn->as.func.body);

    /* Entry: one alloca per local, then seed the non-struct parameters. */
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "entry");
    g->cur = entry;
    LLVMPositionBuilderAtEnd(g->ab, entry);

    for (int i = 0; i < g->locals.n; i++) {
        Asg *a = &g->locals.v[i];
        if (a->slot) continue;                       /* a struct parameter */
        /* A local whose type the frontend could not infer has no IR type to
         * allocate, and NULL is not something libLLVM survives: this used to
         * SIGSEGV the compiler on `for (x in [])`, where the loop variable is
         * registered with an element type there is none to find. Reporting it
         * is the whole point — llvm_codegen_module turns g->err into a clean
         * exit 1 at the failure cleanup below, which is what --compile-text
         * already did for the same input. */
        if (!ty_of(g, a->ty))
            ERRX(g, "line %zu: variable '%s' has no codegen type "
                    "(its type could not be inferred, e.g. an empty list)",
                 fn->line, a->name);
        a->slot = LLVMBuildAlloca(g->ab, ty_of(g, a->ty), "lv");
    }
    if (pv) {
        for (int i = 0; i < arity; i++) {
            if (params[i] && params[i]->kind == TY_STRUCT) continue;
            LLVMBuildStore(g->ab, pv[i],
                           asg_find(&g->locals, fn->as.func.names[i])->slot);
        }
    }

    if (fn->as.func.body) cg_block(g, fn->as.func.body);

    /* Close the function: any block the body left open still needs exactly one
     * terminator. A struct return has no zeroinitialiser to hand back — such a
     * body is expected to have returned already. */
    if (!DONE(g->cur)) {
        /* The synthetic top body carries whatever main() gave back, not a
         * zero: translating the interpreter's exit code is the whole point. */
        if (g->exit_val)                       LLVMBuildRet(g->ab, g->exit_val);
        else if (!rty || rty == g->void_ty)    LLVMBuildRetVoid(g->ab);
        else                                   LLVMBuildRet(g->ab, LLVMConstNull(rty));
    }

    asgs_free(&g->locals);
    g->locals = saved;
    g->cur_ret = saved_ret;
    g->fn = NULL;
    /* The two scratch vectors: LLVMFunctionType and LLVMGetParams only read
     * them, the IR owns the values, so nothing survives past this point. */
    free(pts);
    free(pv);
}

/* ------------------------------------------------------------------- entry -- */

void llvm_codegen_init_targets(void)
{
    static int done = 0;
    if (done) return;
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    done = 1;
}

/* Build (and hand back) the module. Returns NULL with a message in err. */
LLVMModuleRef llvm_codegen_module(const char *mod_name, Node *prog,
                                  LLVMContextRef *out_ctx, char *err, size_t err_size)
{
    char *msg = NULL;
    if (err && err_size) err[0] = '\0';
    if (!prog || prog->type != N_PROGRAM) {
        if (err && err_size) snprintf(err, err_size, "internal: not a program node");
        return NULL;
    }

    CG cg;
    memset(&cg, 0, sizeof cg);

    LLVMContextRef ctx = LLVMContextCreate();
    cg.ctx     = ctx;
    cg.ab      = LLVMCreateBuilderInContext(ctx);
    cg.i1      = LLVMInt1TypeInContext(ctx);
    cg.i64     = LLVMInt64TypeInContext(ctx);
    cg.dbl     = LLVMDoubleTypeInContext(ctx);
    cg.i8ptr   = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    cg.void_ty = LLVMVoidTypeInContext(ctx);
    cg.i32     = LLVMInt32TypeInContext(ctx);
    cg.mod     = LLVMModuleCreateWithNameInContext(mod_name ? mod_name : "lume", ctx);
    /* The triple comes from libLLVM itself: no TARGET_TRIPLE injection, no
     * -Woverride-module fight, no generic-target ABI surprises. */
    char *triple = LLVMGetDefaultTargetTriple();
    if (triple) { LLVMSetTarget(cg.mod, triple); LLVMDisposeMessage(triple); }

    Node **top  = prog->as.program.stmts;
    int    ntop = prog->as.program.count;

    /* ---- pass 1: signatures and declared shapes, so calls resolve in any order ---- */
    for (int i = 0; i < ntop; i++) {
        Node *n = top[i];
        if (n->type == N_FUNC_DECL) {
            sig_push(&cg.sigs, n->as.func.name, n->as.func.ret,
                     n->as.func.param_types, n->as.func.arity);
        }
        else if (n->type == N_TYPE_DECL)
            sdef_push(&cg.structs, n->as.type_decl.name, n->as.type_decl.field_names,
                      n->as.type_decl.field_types, n->as.type_decl.count);
    }

    /* Struct bodies must exist before anything can name a field. */
    for (int i = 0; i < cg.structs.n; i++) {
        SDef *sd = &cg.structs.v[i];
        LLVMTypeRef *fts = (LLVMTypeRef *)xmalloc(sizeof(LLVMTypeRef) * (sd->count > 0 ? sd->count : 1));
        for (int j = 0; j < sd->count; j++) {
            fts[j] = ty_of(&cg, sd->types[j]);
            if (!fts[j])
                ERR(&cg, "field '%s' of struct '%s' has no codegen type",
                    sd->names[j], sd->name);
        }
        LLVMStructSetBody(struct_ty(&cg, sd->name), fts, (unsigned)sd->count, 0);
        free(fts);
    }

    /* ---- pass 2: one LLVM function per Lume function ----
     * Top-level statements (a bare `let`, a `print`, ...) live outside every
     * function but lume runs them in source order, so they are collected here
     * into one synthetic `top` body through exactly the path a real function
     * takes. */
    int nfns = 0, ntops = 0;
    for (int i = 0; i < ntop; i++) {
        if (top[i]->type == N_FUNC_DECL) nfns++;
        else if (top[i]->type == N_TYPE_DECL || top[i]->type == N_IMPORT) continue;
        else ntops++;
    }
    Node **fns  = nfns  > 0 ? (Node **)xmalloc(sizeof(Node *) * (size_t)nfns)  : NULL;
    Node **tops = ntops > 0 ? (Node **)xmalloc(sizeof(Node *) * (size_t)ntops) : NULL;
    /* Does the top level actually call `main`? Only then is main() a thing the
     * process can take its status from — a main nobody runs is an ordinary
     * function and the status stays 0. Same question the text backend asks, so
     * the two backends report the same exit code for the same source. */
    int tops_main = 0;
    {
        int k = 0, m = 0;
        for (int i = 0; i < ntop; i++) {
            if (top[i]->type == N_FUNC_DECL)      fns[k++] = top[i];
            else if (top[i]->type == N_TYPE_DECL) continue;   /* emitted above */
            else if (top[i]->type == N_IMPORT)    continue;   /* the loader resolved it */
            else {
                tops[m++] = top[i];
                Node *e = top[i]->type == N_EXPR_STMT ? top[i]->as.expr_stmt.expr : NULL;
                if (e && e->type == N_CALL && e->as.call.callee &&
                    e->as.call.callee->type == N_VAR &&
                    strcmp(e->as.call.callee->as.var.name, "main") == 0)
                    tops_main = 1;
            }
        }
    }

    for (int i = 0; i < nfns; i++) {
        cg_function(&cg, fns[i]);
    }

    if (ntops > 0) {
        Node body;
        memset(&body, 0, sizeof body);
        body.type = N_BLOCK;
        body.as.block.stmts = tops;
        body.as.block.count = ntops;

        Node *top_fn = (Node *)xmalloc(sizeof(Node));
        memset(top_fn, 0, sizeof(Node));
        top_fn->type = N_FUNC_DECL;
        top_fn->line = top[0] ? top[0]->line : 0;
        top_fn->as.func.name = "top";       /* cg_function prefixes it with L_ */
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
        cg.exit_val = NULL;
        free(top_fn);
    }
    free(fns); free(tops);

    /* ---- C entry: the top level, and nothing else ----
     * `main` is an ordinary function here, as it is in the interpreter: the
     * entry used to call L_main after L_top too, which ran main twice for any
     * program whose top level called it and ran it where the interpreter runs
     * nothing. A program that wants main() to run calls it from the top level
     * like any other function. The exit status is 0 — main()'s return value
     * is not forwarded, since the entry never calls it — see below, where the
     * top-level call to main() does carry it. */
    {
        LLVMValueRef ltop = LLVMGetNamedFunction(cg.mod, "L_top");

        LLVMValueRef cmain = LLVMGetNamedFunction(cg.mod, "main");
        if (!cmain) cmain = LLVMAddFunction(cg.mod, "main",
                                            LLVMFunctionType(cg.i32, NULL, 0, 0));
        LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx, cmain, "entry");
        LLVMPositionBuilderAtEnd(cg.ab, entry);

        if (ltop && tops_main) {
            /* L_top carries an i64, like every other lume int: the status is
             * the low eight bits of it, which is what a process reports. A
             * call that returns something can be named — the void one below
             * cannot. */
            LLVMTypeRef  top_ty = LLVMFunctionType(cg.i64, NULL, 0, 0);
            LLVMValueRef r = LLVMBuildCall2(cg.ab, top_ty, ltop, NULL, 0, "r");
            LLVMValueRef t = r ? LLVMBuildTrunc(cg.ab, r, cg.i32, "t") : NULL;
            LLVMBuildRet(cg.ab, t ? t : LLVMConstInt(cg.i32, 0, 0));
        } else {
            if (ltop) {
                /* The name has to be an empty string, not NULL, and not a real
                 * name. A real one is rejected by the verifier ("Instruction
                 * has a name, but provides a void value") because a void call
                 * has no value to hold a name; NULL is worse — this libLLVM's
                 * C API turns the name into a Twine and dereferences it, so a
                 * script with any top-level statement segfaults the compiler
                 * here. An empty string is the only spelling that satisfies
                 * both. */
                LLVMTypeRef top_ty = LLVMFunctionType(cg.void_ty, NULL, 0, 0);
                LLVMValueRef no_args[1] = { NULL };
                LLVMBuildCall2(cg.ab, top_ty, ltop, no_args, 0, "");
            }
            LLVMBuildRet(cg.ab, LLVMConstInt(cg.i32, 0, 0));
        }
    }

    /* ---- failure cleanup ---- */
    if (cg.err[0]) {
        msg = xstrdup(cg.err);
    } else {
        /* Let LLVM check what we built before codegen ever sees it: this is
         * the one place a broken module is reported in full. */
        char *vmsg = NULL;
        if (LLVMVerifyModule(cg.mod, LLVMReturnStatusAction, &vmsg)) {
            msg = xstrdup(vmsg ? vmsg : "module verification failed");
            if (vmsg) LLVMDisposeMessage(vmsg);
        }
    }
    if (msg) {
        if (err && err_size) snprintf(err, err_size, "%s", msg);
        LLVMDisposeBuilder(cg.ab);
        LLVMDisposeModule(cg.mod);
        LLVMContextDispose(ctx);
        free(cg.sigs.v); free(cg.structs.v); free(cg.brk); free(cg.cnt);
        free(msg);
        return NULL;
    }

    LLVMDisposeBuilder(cg.ab);
    sigs_free(&cg.sigs); sdefs_free(&cg.structs);
    free(cg.brk); free(cg.cnt);
    /* The module outlives this function, so the context cannot be disposed
     * here — hand it to the caller instead. It is disposed after the module,
     * which owns nothing of its own but every type and value it mentions. */
    if (out_ctx) *out_ctx = ctx;
    return cg.mod;
}
