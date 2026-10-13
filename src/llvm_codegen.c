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

typedef struct { char *name; Type *ty; LLVMValueRef slot; size_t line; } Asg;
typedef struct { Asg *v; int n, cap; } Asgs;

/* A name whose type is already known but which has no slot yet — the
 * match-arm binds live here only while their arm body's type is inferred. */
typedef struct { char *name; Type *ty; } LTyp;
typedef struct { LTyp *v; int n, cap; } LTys;

/* Shared with the signature pass (codegen_sig.c): linear lookup by name. */
Type *lt_find(LTys *t, const char *name);

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
    Asgs   gvars;                  /* top-level bindings lifted to module
                                    * globals (SPEC 8.1 #10): a function body
                                    * that misses in `locals` reads here      */
    Sigs   sigs;                   /* every top-level func                   */
    SDefs  structs;                /* every `type X = {...}`                 */
    Type  *expect;                 /* type the surrounding context wants     */
    Type  *cur_ret;                /* return type of the function emitted    */
    StructEnt structs_types[MAX_STRUCT_TYPES];
    int    structs_types_n;

    /* Set while the driver pre-scans / emits the synthetic L_top: the scan
     * then lifts every top-level `let` and for-in variable into `gvars` (an
     * LLVM global) instead of a fresh alloca, and cg_def reuses the
     * pre-scanned table instead of scanning the body a second time. */
    int    scanning_top;
    Asgs  *preset_locals;

    /* Match-arm binds, for the window their arm body's type is inferred in
     * (the infer below reads it as a last resort); empty outside that
     * window, released when the outermost match is done. Mirrors the field
     * of the same name in codegen_internal.h's CG. */
    LTys   arm_scope;

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
static void asg_push(Asgs *a, const char *name, Type *ty, LLVMValueRef slot,
                     size_t line)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->v = (Asg *)xrealloc(a->v, (size_t)a->cap * sizeof *a->v);
    }
    a->v[a->n].name = xstrdup(name);
    a->v[a->n].ty   = ty;
    a->v[a->n].slot = slot;
    /* Declaration line — see codegen.c's asg_push. The flat pre-scanned
     * table can hold several same-name bindings (loop variable + a later
     * `let` of a different type); lookup is position-aware so a use site
     * only sees declarations at or above its own line, matching the
     * interpreter's sequential set-or-define rebinding. */
    a->v[a->n].line = line;
    a->n++;
}

static Asg *asg_find(Asgs *a, const char *name, size_t use_line)
{
    for (int i = a->n - 1; i >= 0; i--)
        if (strcmp(a->v[i].name, name) == 0 && a->v[i].line <= use_line)
            return &a->v[i];
    return NULL;
}

/* The top-level table has no line filter — see codegen.c's gvar_find: the
 * typechecker has already proved every reaching use names a binding declared
 * before the function's own text, and the semantics of reading one are the
 * interpreter's call-time env lookup. */

static Asg *gvar_find(Asgs *a, const char *name)
{
    for (int i = a->n - 1; i >= 0; i--)
        if (strcmp(a->v[i].name, name) == 0)
            return &a->v[i];
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
    case TY_ENUM:
        /* An enum value is the interpreter's `{ "$": variant, ...payload }`
         * map — the same heap map, the same opaque pointer. Kept in step
         * with llvm_type_of() in codegen_types.c (docs/SPEC.md §enum). */
        return g->i8ptr;
    case TY_RESULT:
        /* Result is `{ ok: ... }` / `{ err: ... }`, and the interpreter treats
         * it as an ordinary map (interp.c's `propagate` branch looks the two
         * keys up at runtime) -- so natively it is the same heap map and
         * travels as the same opaque pointer. Kept in step with
         * llvm_type_of() in codegen_types.c, which the text emitter uses. */
        return g->i8ptr;
    case TY_FUNC:   return g->i8ptr;   /* closure record pointer (LumeClosure*) */
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
static const char *list_at_fn(Type *ty);   /* element accessor, defined with for-in */
static LLVMValueRef rt_decl(CG *g, const char *name, LLVMTypeRef ret,
                            LLVMTypeRef *params, int n);   /* declared-once helper */
static int struct_is_addr(LLVMValueRef v); /* defined with cg_builtin's tail */
static Val cg_assign_mem(CG *g, Node *n);   /* defined with cg_stmt, used by cg_expr */
static Val cg_index(CG *g, Node *n);        /* m["k"] / l[0] -- defined after cg_expr */
static Val cg_expr(CG *g, Node *n);
static void cg_stmt(CG *g, Node *n);
static void cg_if_let(CG *g, Node *n);      /* defined with cg_match */
static void cg_block(CG *g, Node *blk);

/* Structural equality (SPEC 1.8), mutually recursive — a struct field that
 * is itself a struct recurses back through cg_val_eq. Mirrors
 * codegen_expr.c's pair branch for branch; see the long comment there. The
 * struct helpers they lean on are defined further down (with cg_member). */
static void struct_addr(CG *g, Val *o);
static int struct_field_idx(CG *g, const char *sname, const char *fname);
static Type *struct_field_type(CG *g, const char *sname, int idx);
static LLVMValueRef gep_field(CG *g, Type *sty, LLVMValueRef ptrv, int idx);
static LLVMValueRef cg_val_eq(CG *g, Val a, Val b, bool ne, size_t line);
static LLVMValueRef cg_struct_eq(CG *g, Val a, Val b, bool ne, size_t line);

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
    case N_IF_LET:  return "if-let";
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
    case N_BINARY:  return "binary";            case N_TERNARY: return "ternary";
    case N_IMPORT: return "import";
    case N_INDEX:   return "index";
    case N_ENUM_DECL: return "enum";
    case N_MATCH:   return "match";             case N_CTOR: return "constructor";
    case N_STRUCT_UPDATE: return "struct update";
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
        /* Language-level escapes resolved through the shared table:
         * LLVMConstStringInContext takes the bytes verbatim, so without this
         * pass `"a\nb"` emitted four characters while the interpreter printed
         * a newline (mirrors codegen_expr.c's cg_literal). */
        size_t ulen = 0;
        char *u = lume_unescape(t, len, &ulen);
        LLVMValueRef s = cg_string_val(g, u, ulen);
        free(u);
        return val_make(type_prim(TY_STRING), s);
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
    Asg *a = asg_find(&g->locals, n->as.var.name, n->line);
    /* Not a local: a top-level binding, read through its module global (the
     * slot value is the global itself — a valid pointer operand for the load,
     * same as an alloca). Mirrors codegen_expr.c's fallback. */
    if (!a) a = gvar_find(&g->gvars, n->as.var.name);
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

/* == / != ask what two values *contain*, never whether two pointers happen to
 * match — the libLLVM mirror of codegen_expr.c's cg_val_eq, kept in step with
 * it branch for branch (the long comment there carries the semantics). Both
 * operands are consumed; NULL is an error, with the reason in g->err.
 *
 * A named struct is the one shape that never reaches rt.c: it lowers to a
 * typed aggregate, not the i8* heap map the runtime walks, so a struct
 * against a struct goes to cg_struct_eq for a field-by-field compare. */
static LLVMValueRef cg_val_eq(CG *g, Val a, Val b, bool ne, size_t line)
{
    AT(g);
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

    if (a_named && b_named)
        return cg_struct_eq(g, a, b, ne, line);

    if (a_named != b_named)
        /* A typed aggregate against an i8* heap map: no runtime bridge
         * between the two representations exists, and the interpreter would
         * answer by comparing contents. */
        ERR(g, "line %zu: '%s' between %s and %s is not supported "
                "by the native backend yet",
             line, ne ? "!=" : "==", src_type_name(a.ty), src_type_name(b.ty));

    const char *rt_fn = NULL;
    if (ak == TY_STRING && bk == TY_STRING)
        rt_fn = "lume_str_eq";
    else if (a_ctr && b_ctr)
        rt_fn = "lume_value_eq";   /* structural, SPEC 1.8 */
    else if ((ak == TY_ANY && b_ptr) || (bk == TY_ANY && a_ptr))
        rt_fn = "lume_any_eq";     /* runtime probes the kind */
    if (rt_fn) {
        LLVMTypeRef at2[2] = { g->i8ptr, g->i8ptr };
        LLVMValueRef eq64 = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i64, at2, 2, 0),
                rt_decl(g, rt_fn, g->i64, at2, 2),
                (LLVMValueRef[]){ a.v, b.v }, 2, "eq");
        LLVMValueRef b1 = LLVMBuildICmp(g->ab, LLVMIntNE, eq64,
                                        LLVMConstInt(g->i64, 0, 0), "isb");
        if (ne)
            b1 = LLVMBuildXor(g->ab, b1, LLVMConstInt(g->i1, 1, 0), "not");
        return b1;
    }
    /* bool == bool, null against another i8*-lowered value, and func identity
     * all compare as their own IR types. */
    bool self_typed =
        (ak == TY_BOOL && bk == TY_BOOL) ||
        ((ak == TY_NULL || bk == TY_NULL) && a_ptr && b_ptr) ||
        (ak == TY_FUNC && bk == TY_FUNC);
    if (self_typed || ((ak == TY_INT || ak == TY_FLOAT) &&
                       (bk == TY_INT || bk == TY_FLOAT))) {
        /* Numeric: cg_binary's promotion already widened a mixed int/float
         * pair to double, so a float here means both sides are float. */
        unsigned pred[2] = { 0, 0 };
        if (ak == TY_FLOAT) {
            pred[0] = LLVMRealOEQ; pred[1] = LLVMRealONE;
            return LLVMBuildFCmp(g->ab, (LLVMRealPredicate)pred[ne ? 1 : 0],
                                 a.v, b.v, "c");
        }
        pred[0] = LLVMIntEQ; pred[1] = LLVMIntNE;
        return LLVMBuildICmp(g->ab, (LLVMIntPredicate)pred[ne ? 1 : 0],
                             a.v, b.v, "c");
    }
    ERR(g, "line %zu: '%s' between %s and %s is not supported "
            "by the native backend yet",
         line, ne ? "!=" : "==", src_type_name(a.ty), src_type_name(b.ty));
}

/* Two named structs compare field by field (SPEC 1.8). The interpreter
 * answers the same question by comparing the key sets of the two maps, and
 * field names are the keys — so a field-set mismatch is a constant answer,
 * not an error, and the two sides need not even be the same declared type:
 * what matters is that the fields line up by name.
 *
 * Both sides get a slot (struct_addr), each field is loaded through a
 * StructGEP and compared recursively — a field that is itself a list, a map
 * or a nested struct recurses back into cg_val_eq. The result is the AND
 * over the fields, xored for `!=`; two field-less structs are equal. */
static LLVMValueRef cg_struct_eq(CG *g, Val a, Val b, bool ne, size_t line)
{
    AT(g);
    SDef *sa = sdef_find(&g->structs, a.ty->name);
    SDef *sb = sdef_find(&g->structs, b.ty->name);

    /* bj[i] is sb's index for sa's field i, or -1 when the name is absent. */
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
    if (!bj)
        return ne ? LLVMConstInt(g->i1, 1, 0) : LLVMConstInt(g->i1, 0, 0);

    struct_addr(g, &a);
    struct_addr(g, &b);
    if (!a.v || !b.v) {
        free(bj);
        ERR(g, "line %zu: cannot compare these struct values", line);
    }

    LLVMValueRef acc = LLVMConstInt(g->i1, 1, 0);
    for (int i = 0; i < sa->count; i++) {
        int j = bj[i];
        Type *fty = sa->types[i];
        Type *gty = sb->types[j];
        if (!ty_of(g, fty) || !ty_of(g, gty)) {
            free(bj);
            ERR(g, "line %zu: field '%s' has no codegen type", line, sa->names[i]);
        }
        LLVMValueRef pa = gep_field(g, a.ty, a.v, i);
        LLVMValueRef pb = gep_field(g, b.ty, b.v, j);
        if (!pa || !pb) {
            free(bj);
            ERR(g, "line %zu: cannot compare field '%s'", line, sa->names[i]);
        }
        Val fa = val_make(fty, LLVMBuildLoad2(g->ab, ty_of(g, fty), pa, "f"));
        Val fb = val_make(gty, LLVMBuildLoad2(g->ab, ty_of(g, gty), pb, "f"));
        if (!fa.v || !fb.v) {
            free(bj);
            ERR(g, "line %zu: cannot read field '%s'", line, sa->names[i]);
        }
        LLVMValueRef fe = cg_val_eq(g, fa, fb, false, line);
        if (!fe) { free(bj); return NULL; }
        acc = LLVMBuildAnd(g->ab, acc, fe, "and");
        if (!acc) {
            free(bj);
            ERR(g, "line %zu: cannot compare these struct values", line);
        }
    }
    free(bj);
    if (ne)
        acc = LLVMBuildXor(g->ab, acc, LLVMConstInt(g->i1, 1, 0), "not");
    return acc;
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
        bool is_eq = n->as.binary.op == OP_EQ || n->as.binary.op == OP_NE;
        static const unsigned iop[] = { LLVMIntEQ, LLVMIntNE, LLVMIntSLT,
                                        LLVMIntSLE, LLVMIntSGT, LLVMIntSGE };
        if (is_eq) {
            /* == / != need content or structure — never pointer luck: the
             * dispatch lives in cg_val_eq (mirrors codegen_expr.c's
             * restructured block; see the comment there for why strings stay
             * a known-broken corner on the relational spellings, which never
             * reach it). */
            LLVMValueRef r = cg_val_eq(g, a, b,
                                       n->as.binary.op == OP_NE, n->line);
            if (!r) ERRV(g, "line %zu: bad comparison", n->line);
            return val_make(type_prim(TY_BOOL), r);
        }
        if (a.ty && a.ty->kind == TY_FLOAT) {
            static const unsigned op[] = { LLVMRealOEQ, LLVMRealONE, LLVMRealOLT,
                                           LLVMRealOLE, LLVMRealOGT, LLVMRealOGE };
            return val_make(type_prim(TY_BOOL),
                            LLVMBuildFCmp(g->ab, (LLVMRealPredicate)op[k], a.v, b.v, "c"));
        }
        return val_make(type_prim(TY_BOOL),
                        LLVMBuildICmp(g->ab, (LLVMIntPredicate)iop[k], a.v, b.v, "c"));
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
    case TY_RESULT:
    case TY_ENUM:   return "lume_map_get_obj";
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
    case TY_RESULT:
    case TY_ENUM:   return "lume_list_push_obj";
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
    case TY_RESULT:
    case TY_ENUM:   return "lume_map_put_obj";
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
        else if (e->type == N_CTOR) {
            /* A bare constructor value (`[Red, Green]`): resolve_ctors
             * stamped the enum's name onto the node, so the element type is
             * statically spelled. Same branch as codegen_expr.c's shared
             * copy of this function. */
            t = e->as.ctor.ename ? type_enum(e->as.ctor.ename) : NULL;
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

/* `{ base with k: v, ... }` — the functional update (SPEC 1.8's sibling).
 * Two shapes by the base's static type, mirroring codegen_expr.c: a named
 * struct is a typed aggregate rebuilt field by field (untouched fields load
 * from the base through gep_field, overridden ones are evaluated and
 * inserted); an anonymous map is cloned by lume_map_clone and then
 * overwritten key by key. */
static Val cg_struct_update(CG *g, Node *n)
{
    AT(g);
    Val base = cg_expr(g, n->as.supd.base);
    if (!base.v) return base;

    if (base.ty && base.ty->kind == TY_STRUCT && base.ty->name) {
        Type *ty = base.ty;
        SDef *sd = sdef_find(&g->structs, ty->name);
        if (!sd) ERRV(g, "line %zu: unknown type '%s'", n->line, ty->name);
        LLVMTypeRef sty = ty_of(g, ty);
        if (!sty) ERRV(g, "line %zu: type '%s' has no codegen type", n->line,
                       ty->name);

        /* Normalize to an address (a slot for an aggregate value, a no-op
         * for a reference). */
        struct_addr(g, &base);
        if (!base.v)
            ERRV(g, "line %zu: cannot update a value of type '%s'", n->line,
                 ty->name);

        LLVMValueRef cur = LLVMGetUndef(sty);
        for (int j = 0; j < sd->count; j++) {
            int w = -1;
            for (int k = 0; k < n->as.supd.count; k++)
                if (strcmp(n->as.supd.keys[k], sd->names[j]) == 0) { w = k; break; }

            Val fv;
            if (w >= 0) {
                Type *prev = g->expect;
                g->expect = sd->types[j];
                fv = cg_expr(g, n->as.supd.vals[w]);
                g->expect = prev;
                if (!fv.v) return fv;
            } else {
                LLVMTypeRef fty = ty_of(g, sd->types[j]);
                if (!fty)
                    ERRV(g, "line %zu: field '%s' has no codegen type",
                         n->line, sd->names[j]);
                LLVMValueRef fp = gep_field(g, ty, base.v, j);
                if (!fp)
                    ERRV(g, "line %zu: cannot read field '%s'", n->line,
                         sd->names[j]);
                fv = val_make(sd->types[j],
                              LLVMBuildLoad2(g->ab, fty, fp, "f"));
            }

            if (fv.ty && sd->types[j] && fv.ty != sd->types[j])
                fv = coerce(g, sd->types[j], fv, n->line);
            if (g->err[0]) return (Val){ NULL, NULL };
            cur = LLVMBuildInsertValue(g->ab, cur, fv.v, (unsigned)j, "s");
        }
        return val_make(ty, cur);
    }

    /* Anonymous map: clone the base at runtime, then overwrite the listed
     * keys (nested containers stay shared, like the interpreter). */
    LLVMTypeRef newp[1] = { g->i8ptr };
    LLVMValueRef nv[1] = { base.v };
    LLVMValueRef m = LLVMBuildCall2(g->ab,
                                    LLVMFunctionType(g->i8ptr, newp, 1, 0),
                                    rt_decl(g, "lume_map_clone", g->i8ptr,
                                            newp, 1),
                                    nv, 1, "mc");
    if (!m) ERRV(g, "line %zu: cannot copy a map for 'with' update", n->line);

    for (int i = 0; i < n->as.supd.count; i++) {
        Val v = cg_expr(g, n->as.supd.vals[i]);
        if (!v.v) return v;
        Val k = val_make(type_prim(TY_STRING),
                         cg_string_val(g, n->as.supd.keys[i],
                                       strlen(n->as.supd.keys[i])));
        const char *fn = map_put_fn(v.ty);
        if (!fn) ERRV(g, "line %zu: map values must be int, float, string or a container",
                      n->line);
        if (v.ty && v.ty->kind == TY_BOOL) v = coerce(g, type_prim(TY_INT), v, n->line);
        LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, ty_of(g, v.ty) };
        LLVMValueRef vs[3] = { m, k.v, v.v };
        Val r = rt_call(g, type_prim(TY_INT), fn, 3, ats, vs);
        if (!r.v) ERRV(g, "line %zu: cannot store into a map", n->line);
    }
    return val_make(type_anon_struct(), m);
}

/* --------------------------------------------------------------- assign ----- */

static Val cg_assign_expr(CG *g, Node *n)
{
    AT(g);
    Val v = cg_expr(g, n->as.assign.value);

    Asg *a = asg_find(&g->locals, n->as.assign.name, n->line);
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
    case TY_ENUM:
        /* An enum value is the same heap map with a "$" tag. */
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
/* --- closures (SPEC 4.8, native) --------------------------------------------
 *
 * A lambda is a value: the mangled `define` the signature pass named is boxed
 * by lume_closure_make into a { i8* fn, i8* cap } record. `cap` is null in
 * this phase — free-variable capture stays interpreter-only — but the record
 * shape and the leading context parameter of every closure define are the
 * contract every call site is built against, and they are the same ABI the
 * text emitter spells by hand. Under opaque pointers no bitcasts are needed:
 * a function value is a pointer like any other. */

/* Forward-declare (or fetch) the closure's define and build the boxed value.
 * The body itself is attached later, by cg_closure, through the same
 * get-or-insert dance a forward call to a named function uses. */

static Val cg_funclit(CG *g, Node *n)
{
    AT(g);
    const char *cname = n->as.funclit.cname;
    if (!cname)
        ERRV(g, "line %zu: internal: lambda was never named by the signature "
                "pass", n->line);

    int arity = n->as.funclit.arity;
    LLVMTypeRef *pts = (LLVMTypeRef *)xmalloc(sizeof(LLVMTypeRef) * (size_t)(arity + 1));
    pts[0] = g->i8ptr;                       /* the capture context */
    for (int i = 0; i < arity; i++) {
        pts[i + 1] = n->as.funclit.param_types
            ? ptr_of(g, n->as.funclit.param_types[i]) : NULL;
        if (!pts[i + 1]) { free(pts); ERRV(g, "line %zu: this lambda's parameter "
                                           "has no codegen type", n->line); }
    }
    LLVMTypeRef rty = n->as.funclit.ret ? ty_of(g, n->as.funclit.ret) : g->void_ty;
    LLVMTypeRef fty = LLVMFunctionType(rty, pts, (unsigned)(arity + 1), 0);
    free(pts);
    if (!fty) ERRV(g, "line %zu: cannot spell this lambda's type", n->line);
    if (!LLVMGetNamedFunction(g->mod, cname) &&
        !LLVMAddFunction(g->mod, cname, fty))
        ERRV(g, "line %zu: cannot declare the closure", n->line);
    LLVMValueRef fnv = LLVMGetNamedFunction(g->mod, cname);
    if (!fnv) ERRV(g, "line %zu: internal: closure symbol vanished", n->line);

    LLVMTypeRef p2[2] = { g->i8ptr, g->i8ptr };
    LLVMValueRef vs[2] = { fnv, LLVMConstNull(g->i8ptr) };
    LLVMValueRef r = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, p2, 2, 0),
                                    rt_decl(g, "lume_closure_make", g->i8ptr, p2, 2),
                                    vs, 2, "clo");
    if (!r) ERRV(g, "line %zu: cannot build this closure", n->line);

    /* The same TY_FUNC-with-back-pointer infer_node_type builds, so a call
     * through the boxed value finds the lambda and its pinned signature. */
    Type *ft = type_func(arity, n->as.funclit.param_types, n->as.funclit.ret);
    ft->func = n;
    return val_make(ft, r);
}

/* Call through a closure value. `clo` holds the boxed record; `lambda`
 * supplies the fn pointer's signature; the arguments are already evaluated,
 * coerced and struct-decayed. Borrows everything; returns the call's value. */

static LLVMValueRef cg_clo_invoke(CG *g, Node *lambda, LLVMValueRef clo,
                                  LLVMValueRef *args, int argc)
{
    AT(g);
    LLVMTypeRef rec_elems[2] = { g->i8ptr, g->i8ptr };
    LLVMTypeRef rec = LLVMStructTypeInContext(g->ctx, rec_elems, 2, 0);
    LLVMValueRef fnp = LLVMBuildStructGEP2(g->ab, rec, clo, 0, "fnp");
    LLVMValueRef capp = LLVMBuildStructGEP2(g->ab, rec, clo, 1, "capp");
    if (!fnp || !capp) return NULL;
    LLVMValueRef fnv = LLVMBuildLoad2(g->ab, g->i8ptr, fnp, "fn");
    LLVMValueRef capv = LLVMBuildLoad2(g->ab, g->i8ptr, capp, "cap");
    if (!fnv || !capv) return NULL;

    int arity = lambda->as.funclit.arity;
    LLVMTypeRef *pts = (LLVMTypeRef *)xmalloc(sizeof(LLVMTypeRef) * (size_t)(arity + 1));
    pts[0] = g->i8ptr;
    for (int i = 0; i < arity; i++) {
        pts[i + 1] = lambda->as.funclit.param_types
            ? ptr_of(g, lambda->as.funclit.param_types[i]) : NULL;
        if (!pts[i + 1]) { free(pts); return NULL; }
    }
    LLVMTypeRef rty = lambda->as.funclit.ret
        ? ty_of(g, lambda->as.funclit.ret) : g->void_ty;
    LLVMTypeRef fty = LLVMFunctionType(rty ? rty : g->void_ty,
                                       pts, (unsigned)(arity + 1), 0);
    free(pts);
    if (!fty) return NULL;

    LLVMValueRef *all = (LLVMValueRef *)xmalloc(sizeof(LLVMValueRef) * (size_t)(argc + 1));
    all[0] = capv;
    for (int i = 0; i < argc; i++) all[i + 1] = args[i];
    LLVMValueRef r = LLVMBuildCall2(g->ab, fty, fnv, all,
                                    (unsigned)(argc + 1), lambda->as.funclit.ret ? "c" : "");
    free(all);
    return r;
}

/* `f(3)` where f is a variable bound to a lambda. cg_call routes here before
 * it consults the named-function table — the lambda lives under its mangled
 * name. */

static Val cg_clo_call(CG *g, Node *n, Node *lambda)
{
    AT(g);
    if (n->as.call.argc != lambda->as.funclit.arity)
        ERRV(g, "line %zu: this closure expects %d argument(s), got %d",
             n->line, lambda->as.funclit.arity, n->as.call.argc);

    Val clo = cg_expr(g, n->as.call.callee);
    if (!clo.v) ERRV(g, "line %zu: cannot evaluate the closure", n->line);

    LLVMValueRef *avs = NULL;
    if (n->as.call.argc > 0) {
        avs = (LLVMValueRef *)xmalloc(
            sizeof(LLVMValueRef) * (size_t)n->as.call.argc);
        for (int i = 0; i < n->as.call.argc; i++) {
            Val a = cg_expr(g, n->as.call.args[i]);
            if (a.ty && a.ty->kind == TY_STRUCT && !struct_is_addr(a.v)) {
                LLVMValueRef slot = LLVMBuildAlloca(g->ab, ty_of(g, a.ty), "sa");
                LLVMBuildStore(g->ab, a.v, slot);
                a.v = slot;
            }
            Type *pt = lambda->as.funclit.param_types
                ? lambda->as.funclit.param_types[i] : NULL;
            if (pt) a = coerce(g, pt, a, n->line);
            if (!a.v) { free(avs); ERRV(g, "line %zu: bad argument %d for the "
                                        "closure", n->line, i + 1); }
            avs[i] = a.v;
        }
    }

    LLVMValueRef r = cg_clo_invoke(g, lambda, clo.v, avs, n->as.call.argc);
    free(avs);
    if (!r) ERRV(g, "line %zu: cannot call through this closure", n->line);
    Type *ret = lambda->as.funclit.ret;
    return val_make(ret, r);
}

/* The lambda behind a HOF's first argument: an inline literal, or a variable
 * bound to one. Same recovery rule the signature pass used to pin the
 * lambda's parameters. */

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
        strcmp(bname, "float") == 0 || strcmp(bname, "stringify") == 0 ||
        strcmp(bname, "json") == 0) {
        if (argc != 1)
            ERRV(g, "line %zu: '%s' takes exactly one argument", n->line, bname);
        Val a = cg_expr(g, n->as.call.args[0]);
        LLVMTypeRef ft = NULL, pty = NULL, rty = NULL;
        Type *rt = NULL;
        bool is_str = strcmp(bname, "str") == 0;
        bool is_json = strcmp(bname, "json") == 0;

        if (is_str || strcmp(bname, "stringify") == 0 || is_json) {
            switch (a.ty ? a.ty->kind : TY_ANY) {
            case TY_INT:    ft = LLVMFunctionType(g->i8ptr, &g->i64, 1, 0);  pty = g->i64;    break;
            case TY_FLOAT:  ft = LLVMFunctionType(g->i8ptr, &g->dbl, 1, 0);  pty = g->dbl;    break;
            case TY_BOOL:   ft = LLVMFunctionType(g->i8ptr, &g->i64, 1, 0);  pty = g->i64;    break;
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
                ft = LLVMFunctionType(g->i8ptr, &g->i8ptr, 1, 0); pty = g->i8ptr;
                break;
            case TY_LIST:
                /* Containers serialize to the same JSON the interpreter's
                 * print() produces; a.v is the i8* container pointer. */
                ft = LLVMFunctionType(g->i8ptr, &g->i8ptr, 1, 0);
                pty = g->i8ptr;
                break;
            case TY_STRUCT:
                /* An anonymous struct is a map (a LumeMap* i8*); a named one
                 * is a raw GEP struct with no runtime header, which the
                 * serializer cannot walk — the same representation gap that
                 * keeps print() off named structs. */
                if (a.ty->name)
                    ERRV(g, "line %zu: %s() cannot convert a named struct value",
                         n->line, bname);
                ft = LLVMFunctionType(g->i8ptr, &g->i8ptr, 1, 0);
                pty = g->i8ptr;
                break;
            default:
                ERRV(g, "line %zu: %s() cannot convert this value",
                     n->line, bname);
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
        if (a.ty && (a.ty->kind == TY_LIST ||
                     (a.ty->kind == TY_STRUCT && !a.ty->name)))
            name = "lume_rt_to_json_obj";
        else if (a.ty && a.ty->kind == TY_STRING)
            /* only stringify reaches here with a string (str returns it as-is,
             * json(str) is rejected above): the quoted-string flavour. */
            name = "lume_rt_quote_str";
        else if (strcmp(bname, "int") != 0 && strcmp(bname, "float") != 0)
            /* str / stringify / json share the scalar stringifying flavours
             * (int/float take their own conversion helpers below). */
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

    /* map(fn, list) / filter(fn, list) / reduce(fn, list, init) — SPEC 4.8's
     * higher-order builtins, looped inline. The lambda's parameters were
     * pinned by the shared signature pass (param[0] from the list element,
     * reduce's param[0] from the init), so the loop reads elements through
     * the same accessor table for-in uses and calls the closure with the
     * exact signature its define was built against. Mirrors the text
     * emitter's copy branch for branch. */

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
        Type *et = lambda->as.funclit.param_types
            ? lambda->as.funclit.param_types[0] : NULL;
        const char *atfn = list_at_fn(et);
        if (!atfn)
            ERRV(g, "line %zu: a list of this element type cannot be mapped",
                 n->line);

        Val fnv = cg_expr(g, n->as.call.args[0]);
        Val src = cg_expr(g, n->as.call.args[1]);
        if (!fnv.v || !src.v || !src.ty || src.ty->kind != TY_LIST)
            ERRV(g, "line %zu: map() needs a list as its second argument",
                 n->line);

        Val dst = rt_call(g, list_type(), "lume_list_new", 0, NULL, NULL);
        if (!dst.v) ERRV(g, "line %zu: cannot allocate a list", n->line);

        /* index + snapshot length, the for-in shape: the bound is captured
         * once, so a push() the lambda makes cannot extend the iteration. */
        LLVMValueRef idx = LLVMBuildAlloca(g->ab, g->i64, "li");
        LLVMBuildStore(g->ab, LLVMConstInt(g->i64, 0, 0), idx);
        LLVMValueRef len_slot = LLVMBuildAlloca(g->ab, g->i64, "len");
        {
            LLVMTypeRef p1[1] = { g->i8ptr };
            LLVMValueRef v1[1] = { src.v };
            LLVMValueRef len0 = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i64, p1, 1, 0),
                rt_decl(g, "lume_list_len", g->i64, p1, 1), v1, 1, "len0");
            LLVMBuildStore(g->ab, len0, len_slot);
        }

        LLVMBasicBlockRef lc = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "map.cond");
        LLVMBasicBlockRef lb = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "map.body");
        LLVMBasicBlockRef li = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "map.incr");
        LLVMBasicBlockRef le = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "map.end");

        if (!DONE(g->cur)) LLVMBuildBr(g->ab, lc);
        g->cur = lc; AT(g);
        LLVMValueRef len = LLVMBuildLoad2(g->ab, g->i64, len_slot, "len");
        LLVMValueRef cur = LLVMBuildLoad2(g->ab, g->i64, idx, "cur");
        LLVMValueRef cond = LLVMBuildICmp(g->ab, LLVMIntSLT, cur, len, "c");
        LLVMBuildCondBr(g->ab, cond, lb, le);

        g->cur = lb; AT(g);
        Type *rt = et->kind == TY_BOOL ? type_prim(TY_INT) : et;
        LLVMTypeRef p2[2] = { g->i8ptr, g->i64 };
        LLVMValueRef v2[2] = { src.v, cur };
        Val e = rt_call(g, rt, atfn, 2, p2, v2);
        if (!e.v) ERRV(g, "line %zu: cannot read an element", n->line);
        Type *p0 = lambda->as.funclit.param_types
            ? lambda->as.funclit.param_types[0] : NULL;
        if (p0) e = coerce(g, p0, e, n->line);
        if (!e.v) ERRV(g, "line %zu: cannot coerce the map argument", n->line);
        LLVMValueRef rv = cg_clo_invoke(g, lambda, fnv.v, &e.v, 1);
        if (!rv) ERRV(g, "line %zu: cannot call the mapping closure", n->line);
        Val item = val_make(ret, rv);
        if (item.ty && item.ty->kind == TY_BOOL)
            item = coerce(g, type_prim(TY_INT), item, n->line);
        if (!item.v) ERRV(g, "line %zu: cannot coerce the mapped value", n->line);
        {
            LLVMTypeRef pa[2] = { g->i8ptr, ty_of(g, item.ty) };
            LLVMValueRef va[2] = { dst.v, item.v };
            Val pr = rt_call(g, type_prim(TY_INT), pushfn, 2, pa, va);
            if (!pr.v) ERRV(g, "line %zu: cannot append to a list", n->line);
        }
        LLVMBuildBr(g->ab, li);

        g->cur = li; AT(g);
        LLVMValueRef nxt = LLVMBuildAdd(g->ab, cur, LLVMConstInt(g->i64, 1, 0), "nxt");
        LLVMBuildStore(g->ab, nxt, idx);
        LLVMBuildBr(g->ab, lc);

        g->cur = le; AT(g);
        return val_make(type_list(ret), dst.v);
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

        Val fnv = cg_expr(g, n->as.call.args[0]);
        Val src = cg_expr(g, n->as.call.args[1]);
        if (!fnv.v || !src.v || !src.ty || src.ty->kind != TY_LIST)
            ERRV(g, "line %zu: filter() needs a list as its second argument",
                 n->line);

        Val dst = rt_call(g, list_type(), "lume_list_new", 0, NULL, NULL);
        if (!dst.v) ERRV(g, "line %zu: cannot allocate a list", n->line);

        LLVMValueRef idx = LLVMBuildAlloca(g->ab, g->i64, "li");
        LLVMBuildStore(g->ab, LLVMConstInt(g->i64, 0, 0), idx);
        LLVMValueRef len_slot = LLVMBuildAlloca(g->ab, g->i64, "len");
        {
            LLVMTypeRef p1[1] = { g->i8ptr };
            LLVMValueRef v1[1] = { src.v };
            LLVMValueRef len0 = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i64, p1, 1, 0),
                rt_decl(g, "lume_list_len", g->i64, p1, 1), v1, 1, "len0");
            LLVMBuildStore(g->ab, len0, len_slot);
        }

        LLVMBasicBlockRef lc = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "flt.cond");
        LLVMBasicBlockRef lb = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "flt.body");
        LLVMBasicBlockRef lp = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "flt.push");
        LLVMBasicBlockRef li = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "flt.incr");
        LLVMBasicBlockRef le = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "flt.end");

        if (!DONE(g->cur)) LLVMBuildBr(g->ab, lc);
        g->cur = lc; AT(g);
        LLVMValueRef len = LLVMBuildLoad2(g->ab, g->i64, len_slot, "len");
        LLVMValueRef cur = LLVMBuildLoad2(g->ab, g->i64, idx, "cur");
        LLVMValueRef cond = LLVMBuildICmp(g->ab, LLVMIntSLT, cur, len, "c");
        LLVMBuildCondBr(g->ab, cond, lb, le);

        g->cur = lb; AT(g);
        Type *rt = et->kind == TY_BOOL ? type_prim(TY_INT) : et;
        LLVMTypeRef p2[2] = { g->i8ptr, g->i64 };
        LLVMValueRef v2[2] = { src.v, cur };
        Val e = rt_call(g, rt, atfn, 2, p2, v2);
        if (!e.v) ERRV(g, "line %zu: cannot read an element", n->line);
        /* The element travels twice: the raw runtime value into the result
         * list when the test passes, and a copy — narrowed to the
         * parameter's spelling — into the closure. coerce() replaces the
         * value it converts, so the closure argument is built from a
         * separate Val and the raw e stays owned for the push below. */
        Val arg = e;
        Type *p0 = lambda->as.funclit.param_types
            ? lambda->as.funclit.param_types[0] : NULL;
        if (p0) arg = coerce(g, p0, arg, n->line);
        if (!arg.v) ERRV(g, "line %zu: cannot coerce the filter argument", n->line);
        LLVMValueRef rv = cg_clo_invoke(g, lambda, fnv.v, &arg.v, 1);
        if (!rv) ERRV(g, "line %zu: cannot call the filter closure", n->line);

        LLVMValueRef test;
        if (ret->kind == TY_BOOL)      test = rv;
        else if (ret->kind == TY_FLOAT) test = LLVMBuildFCmp(g->ab, LLVMRealUNE,
                                                             rv, LLVMConstReal(g->dbl, 0.0), "t");
        else                            test = LLVMBuildICmp(g->ab, LLVMIntNE,
                                                             rv, LLVMConstInt(g->i64, 0, 0), "t");
        if (!test) ERRV(g, "line %zu: cannot test the filter result", n->line);
        LLVMBuildCondBr(g->ab, test, lp, li);

        g->cur = lp; AT(g);
        {
            LLVMTypeRef pa[2] = { g->i8ptr, ty_of(g, rt) };
            LLVMValueRef va[2] = { dst.v, e.v };
            Val pr = rt_call(g, type_prim(TY_INT), list_push_fn(et), 2, pa, va);
            if (!pr.v) ERRV(g, "line %zu: cannot append to a list", n->line);
        }
        LLVMBuildBr(g->ab, li);

        g->cur = li; AT(g);
        LLVMValueRef nxt = LLVMBuildAdd(g->ab, cur, LLVMConstInt(g->i64, 1, 0), "nxt");
        LLVMBuildStore(g->ab, nxt, idx);
        LLVMBuildBr(g->ab, lc);

        g->cur = le; AT(g);
        return val_make(type_list(et), dst.v);
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

        Val fnv = cg_expr(g, n->as.call.args[0]);
        Val src = cg_expr(g, n->as.call.args[1]);
        Val acc0 = cg_expr(g, n->as.call.args[2]);
        if (!fnv.v || !src.v || !src.ty || src.ty->kind != TY_LIST ||
            !acc0.v || !acc0.ty || acc0.ty->kind == TY_STRUCT)
            ERRV(g, "line %zu: reduce() needs a list and a scalar or list "
                    "accumulator", n->line);
        LLVMTypeRef acc_ty = ty_of(g, acc0.ty);
        if (!acc_ty) ERRV(g, "line %zu: the accumulator has no codegen type",
                          n->line);

        /* The accumulator lives in a slot: every iteration loads it, calls
         * the closure with (acc, item) and stores the result back. */
        LLVMValueRef acc_slot = LLVMBuildAlloca(g->ab, acc_ty, "acc");
        {
            Type *p0 = lambda->as.funclit.param_types
                ? lambda->as.funclit.param_types[0] : NULL;
            Val acc_init = p0 ? coerce(g, p0, acc0, n->line) : acc0;
            if (!acc_init.v)
                ERRV(g, "line %zu: cannot seed the accumulator", n->line);
            LLVMBuildStore(g->ab, acc_init.v, acc_slot);
        }

        LLVMValueRef idx = LLVMBuildAlloca(g->ab, g->i64, "li");
        LLVMBuildStore(g->ab, LLVMConstInt(g->i64, 0, 0), idx);
        LLVMValueRef len_slot = LLVMBuildAlloca(g->ab, g->i64, "len");
        {
            LLVMTypeRef p1[1] = { g->i8ptr };
            LLVMValueRef v1[1] = { src.v };
            LLVMValueRef len0 = LLVMBuildCall2(g->ab,
                LLVMFunctionType(g->i64, p1, 1, 0),
                rt_decl(g, "lume_list_len", g->i64, p1, 1), v1, 1, "len0");
            LLVMBuildStore(g->ab, len0, len_slot);
        }

        LLVMBasicBlockRef lc = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "rd.cond");
        LLVMBasicBlockRef lb = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "rd.body");
        LLVMBasicBlockRef li = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "rd.incr");
        LLVMBasicBlockRef le = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "rd.end");

        if (!DONE(g->cur)) LLVMBuildBr(g->ab, lc);
        g->cur = lc; AT(g);
        LLVMValueRef len = LLVMBuildLoad2(g->ab, g->i64, len_slot, "len");
        LLVMValueRef cur = LLVMBuildLoad2(g->ab, g->i64, idx, "cur");
        LLVMValueRef cond = LLVMBuildICmp(g->ab, LLVMIntSLT, cur, len, "c");
        LLVMBuildCondBr(g->ab, cond, lb, le);

        g->cur = lb; AT(g);
        Type *rt = et->kind == TY_BOOL ? type_prim(TY_INT) : et;
        LLVMTypeRef p2[2] = { g->i8ptr, g->i64 };
        LLVMValueRef v2[2] = { src.v, cur };
        Val e = rt_call(g, rt, atfn, 2, p2, v2);
        LLVMValueRef accv = LLVMBuildLoad2(g->ab, acc_ty, acc_slot, "acc");
        if (!e.v || !accv)
            ERRV(g, "line %zu: cannot read an element or the accumulator", n->line);
        Val argv[2] = { { accv, acc0.ty }, e };
        Type *p0 = lambda->as.funclit.param_types
            ? lambda->as.funclit.param_types[0] : NULL;
        Type *p1 = lambda->as.funclit.param_types && lambda->as.funclit.arity > 1
            ? lambda->as.funclit.param_types[1] : NULL;
        if (p0) argv[0] = coerce(g, p0, argv[0], n->line);
        if (p1) argv[1] = coerce(g, p1, argv[1], n->line);
        if (!argv[0].v || !argv[1].v)
            ERRV(g, "line %zu: cannot coerce the reduce arguments", n->line);
        /* cg_clo_invoke takes a bare LLVMValueRef array — the coerced Vals
         * must be unwrapped first (a Val* reinterpreted would hand the call
         * each Val's Type* as the argument value). */
        LLVMValueRef cargs[2] = { argv[0].v, argv[1].v };
        LLVMValueRef rv = cg_clo_invoke(g, lambda, fnv.v, cargs, 2);
        if (!rv) ERRV(g, "line %zu: cannot call the reducing closure", n->line);
        Val acc_next = val_make(acc0.ty, rv);
        acc_next = coerce(g, acc0.ty, acc_next, n->line);
        if (!acc_next.v)
            ERRV(g, "line %zu: the reducing closure's result does not fit "
                    "the accumulator", n->line);
        LLVMBuildStore(g->ab, acc_next.v, acc_slot);
        LLVMBuildBr(g->ab, li);

        g->cur = li; AT(g);
        LLVMValueRef nxt = LLVMBuildAdd(g->ab, cur, LLVMConstInt(g->i64, 1, 0), "nxt");
        LLVMBuildStore(g->ab, nxt, idx);
        LLVMBuildBr(g->ab, lc);

        g->cur = le; AT(g);
        LLVMValueRef res = LLVMBuildLoad2(g->ab, acc_ty, acc_slot, "res");
        if (!res) ERRV(g, "line %zu: cannot read the accumulator", n->line);
        return val_make(acc0.ty, res);
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

static Val cg_ternary(CG *g, Node *n);
static Val cg_ctor(CG *g, Node *n);
static Val cg_match(CG *g, Node *n);

static Val cg_expr(CG *g, Node *n)
{
    if (!n) ERRV(g, "internal: null expression");

    switch (n->type) {
    case N_LITERAL:  return cg_literal(g, n);
    case N_LIST_LIT: return cg_list_lit(g, n);
    case N_MAP_LIT:  return cg_map_or_struct_lit(g, n);
    case N_STRUCT_UPDATE: return cg_struct_update(g, n);
    case N_ASSIGN:   return cg_assign_expr(g, n);
    case N_ASSIGN_MEMBER: return cg_assign_mem(g, n);
    case N_EXPR_STMT: return cg_expr(g, n->as.expr_stmt.expr);
    case N_VAR:      return cg_var(g, n);
    case N_FUNC_LIT: return cg_funclit(g, n);
    case N_UNARY:    return cg_unary(g, n);
    case N_BINARY:   return cg_binary(g, n);
    case N_TERNARY:  return cg_ternary(g, n);
    case N_MATCH:    return cg_match(g, n);
    case N_CTOR:     return cg_ctor(g, n);
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
    case TY_ENUM:   /* an enum value is the same opaque i8* a container is */
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

/* `cond ? then : els`。形状与 cg_binary 的 and/or 短路一致：结果停在
 * alloca 而不是 phi，三个块各写一次。槽类型取 infer_node_type 的统一
 * 结果（typecheck 已保证可统一），分支值 store 前 coerce（int→float）。
 * 只求值选中分支。 */
static Val cg_ternary(CG *g, Node *n)
{
    Type *rty = infer_node_type(g, n);
    if (!rty || rty->kind == TY_ANY)
        ERRV(g, "line %zu: cannot infer ternary result type", n->line);
    LLVMTypeRef slot;
    switch (rty->kind) {
    case TY_FLOAT:             slot = g->dbl;   break;
    case TY_BOOL:              slot = g->i1;    break; /* and/or 同款 i1 槽 */
    case TY_INT:               slot = g->i64;   break;
    default:                   slot = g->i8ptr; break; /* string/list/map/result/closure */
    }

    Val c = cg_expr(g, n->as.ternary.cond);
    if (!c.v) ERRV(g, "line %zu: bad ternary condition", n->line);

    LLVMValueRef res = LLVMBuildAlloca(g->ab, slot, "tv");
    LLVMBasicBlockRef bt = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "then");
    LLVMBasicBlockRef be = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "else");
    LLVMBasicBlockRef lj = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "join");

    if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, c.v, bt, be);

    g->cur = bt; AT(g);
    Val tv = cg_expr(g, n->as.ternary.then);
    if (!tv.v) ERRV(g, "line %zu: bad ternary branch", n->line);
    tv = coerce(g, rty, tv, n->line);
    LLVMBuildStore(g->ab, tv.v, res);
    if (!DONE(g->cur)) LLVMBuildBr(g->ab, lj);

    g->cur = be; AT(g);
    Val ev = cg_expr(g, n->as.ternary.els);
    if (!ev.v) ERRV(g, "line %zu: bad ternary branch", n->line);
    ev = coerce(g, rty, ev, n->line);
    LLVMBuildStore(g->ab, ev.v, res);
    if (!DONE(g->cur)) LLVMBuildBr(g->ab, lj);

    g->cur = lj; AT(g);
    return val_make(rty, LLVMBuildLoad2(g->ab, slot, res, "t"));
}

/* An enum constructor: the interpreter's `{ "$": V, ...payload }` map, built
 * with the same runtime helpers a map literal uses. Kept in step with
 * cg_ctor() in codegen_expr.c (docs/SPEC.md §enum). */
static Val cg_ctor(CG *g, Node *n)
{
    AT(g);
    LLVMTypeRef newp[1] = { g->i8ptr };
    LLVMValueRef nv[1] = { NULL };
    LLVMValueRef m = LLVMBuildCall2(g->ab, LLVMFunctionType(g->i8ptr, NULL, 0, 0),
                                    rt_decl(g, "lume_map_new", g->i8ptr, newp, 0),
                                    nv, 0, "m");
    if (!m) ERRV(g, "line %zu: cannot allocate an enum value", n->line);

    Val k = val_make(type_prim(TY_STRING), cg_string_val(g, "$", 1));
    Val tv = val_make(type_prim(TY_STRING),
                      cg_string_val(g, n->as.ctor.name, strlen(n->as.ctor.name)));
    LLVMTypeRef ats[3] = { g->i8ptr, g->i8ptr, g->i8ptr };
    LLVMValueRef vs[3] = { m, k.v, tv.v };
    Val r = rt_call(g, type_prim(TY_INT), "lume_map_put_s", 3, ats, vs);
    if (!r.v) ERRV(g, "line %zu: cannot store the variant tag", n->line);

    for (int i = 0; i < n->as.ctor.argc; i++) {
        Val v = cg_expr(g, n->as.ctor.args[i]);
        Val fk = val_make(type_prim(TY_STRING),
                          cg_string_val(g, n->as.ctor.fields[i],
                                        strlen(n->as.ctor.fields[i])));
        const char *fn = map_put_fn(v.ty);
        if (!fn)
            ERRV(g, "line %zu: enum payload must be int, float, string or bool",
                 n->line);
        if (v.ty && v.ty->kind == TY_BOOL) v = coerce(g, type_prim(TY_INT), v, n->line);
        LLVMTypeRef pats[3] = { g->i8ptr, g->i8ptr, ty_of(g, v.ty) };
        LLVMValueRef pvs[3] = { m, fk.v, v.v };
        Val rr = rt_call(g, type_prim(TY_INT), fn, 3, pats, pvs);
        if (!rr.v) ERRV(g, "line %zu: cannot store the payload", n->line);
    }
    return val_make(n->as.ctor.ename ? type_enum(n->as.ctor.ename)
                                     : type_anon_struct(), m);
}

/* `match (s) { P [| P] [if guard] => body, ... }` — the libLLVM mirror of
 * cg_match() in codegen_expr.c. Same lowering: read "$" once, lume_str_eq
 * per pattern (a plain `==` would icmp two pointers), the last pattern of
 * the last arm unconditional (the checker only accepts exhaustive matches),
 * each pattern block storing its own variant's field reads into the arm's
 * shared bind slots (allocated where every pattern block of the arm is
 * dominated from), guards branching to the next arm on false, and
 * g->locals truncated after each arm. */
#define MATCH_BIND_MAX 32

static Val cg_match(CG *g, Node *n)
{
    Type *rty = infer_node_type(g, n);
    if (!rty || rty->kind == TY_ANY)
        ERRV(g, "line %zu: cannot infer the match result type", n->line);
    LLVMTypeRef slot;
    switch (rty->kind) {
    case TY_FLOAT:             slot = g->dbl;   break;
    case TY_BOOL:              slot = g->i1;    break;
    case TY_INT:               slot = g->i64;   break;
    default:                   slot = g->i8ptr; break;
    }

    AT(g);
    Val s = cg_expr(g, n->as.matchn.scrutinee);
    if (!s.v) ERRV(g, "line %zu: bad match scrutinee", n->line);

    LLVMValueRef dollark = cg_string_val(g, "$", 1);
    LLVMTypeRef gats[3] = { g->i8ptr, g->i8ptr, g->i8ptr };
    LLVMValueRef tag = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->i8ptr, gats, 3, 0),
            rt_decl(g, "lume_map_get_s", g->i8ptr, gats, 3),
            (LLVMValueRef[]){ s.v, dollark, LLVMConstNull(g->i8ptr) }, 3, "tag");

    LLVMValueRef res = LLVMBuildAlloca(g->ab, slot, "mv");
    LLVMBasicBlockRef lj = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "mjoin");

    for (int i = 0; i < n->as.matchn.armc; i++) {
        MatchArm *arm = &n->as.matchn.arms[i];
        bool last_arm = i == n->as.matchn.armc - 1;
        MatchPat *p0 = &arm->pats[0];
        if (p0->bindc > MATCH_BIND_MAX)
            ERRV(g, "line %zu: too many match bindings (max %d)",
                 arm->line, MATCH_BIND_MAX);

        /* the arm's bind slots, allocated in the block every pattern block
         * of this arm is entered from (so they dominate the arm block's
         * loads); one set serves all alternatives */
        LLVMValueRef bslots[MATCH_BIND_MAX];
        for (int kb = 0; kb < p0->bindc; kb++) {
            Type *bt = p0->vfield_types ? p0->vfield_types[kb] : NULL;
            LLVMTypeRef blt = bt ? ty_of(g, bt) : NULL;
            if (!blt)
                ERRV(g, "line %zu: match binding '%s' has no codegen type",
                     arm->line, p0->binds[kb]);
            bslots[kb] = LLVMBuildAlloca(g->ab, blt, p0->binds[kb]);
        }

        LLVMBasicBlockRef larm = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "marm");
        LLVMBasicBlockRef lnext =
            (last_arm && !arm->guard) ? NULL
            : LLVMAppendBasicBlockInContext(g->ctx, g->fn, "mnext");
        LLVMBasicBlockRef lbody = arm->guard
            ? LLVMAppendBasicBlockInContext(g->ctx, g->fn, "mguard") : NULL;

        for (int pi = 0; pi < arm->patc; pi++) {
            MatchPat *pat = &arm->pats[pi];
            bool last_pat = pi == arm->patc - 1;
            bool catch_all = last_arm && last_pat;
            LLVMBasicBlockRef lpb =
                LLVMAppendBasicBlockInContext(g->ctx, g->fn, "mpat");
            LLVMBasicBlockRef lmiss = catch_all ? NULL
                                   : last_pat ? lnext
                                   : LLVMAppendBasicBlockInContext(g->ctx, g->fn, "mmiss");
            if (catch_all || strcmp(pat->vname, "_") == 0) {
                if (!DONE(g->cur)) LLVMBuildBr(g->ab, lpb);
            } else {
                LLVMValueRef patk = cg_string_val(g, pat->vname,
                                                  strlen(pat->vname));
                LLVMValueRef eq64 = LLVMBuildCall2(g->ab,
                        LLVMFunctionType(g->i64, gats, 2, 0),
                        rt_decl(g, "lume_str_eq", g->i64,
                                (LLVMTypeRef[]){ g->i8ptr, g->i8ptr }, 2),
                        (LLVMValueRef[]){ tag, patk }, 2, "eq");
                LLVMValueRef eq = LLVMBuildICmp(g->ab, LLVMIntNE, eq64,
                                                LLVMConstInt(g->i64, 0, 0), "isv");
                if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, eq, lpb, lmiss);
            }

            g->cur = lpb; AT(g);
            for (int kb = 0; kb < pat->bindc; kb++) {
                Type *bt = pat->vfield_types ? pat->vfield_types[kb] : NULL;
                const char *gfn = map_get_fn(bt);
                if (!gfn)
                    ERRV(g, "line %zu: match binding '%s' payload must be int, "
                            "float, string or bool", arm->line, pat->binds[kb]);
                /* The rt helper's own spelling rules the call:
                 * lume_map_get_i (the bool reader) returns and takes i64,
                 * while the slot spells i1. Read as i64, narrow after. */
                LLVMTypeRef call_ty = ty_of(g, bt);
                if (bt->kind == TY_BOOL) call_ty = g->i64;
                LLVMValueRef fk = cg_string_val(g, pat->vfields[kb],
                                                strlen(pat->vfields[kb]));
                LLVMTypeRef pf[3] = { g->i8ptr, g->i8ptr, call_ty };
                LLVMValueRef dflt = bt->kind == TY_FLOAT
                                      ? LLVMConstReal(ty_of(g, bt), 0.0)
                                    : call_ty == g->i8ptr
                                      ? LLVMConstNull(g->i8ptr)
                                      : LLVMConstInt(call_ty, 0, 0);
                LLVMValueRef pv = LLVMBuildCall2(g->ab,
                        LLVMFunctionType(call_ty, pf, 3, 0),
                        rt_decl(g, gfn, call_ty, pf, 3),
                        (LLVMValueRef[]){ s.v, fk, dflt }, 3, "pv");
                if (bt->kind == TY_BOOL)
                    pv = LLVMBuildICmp(g->ab, LLVMIntNE, pv,
                                       LLVMConstInt(g->i64, 0, 0), "pb");
                LLVMBuildStore(g->ab, pv, bslots[kb]);
            }
            if (!DONE(g->cur)) LLVMBuildBr(g->ab, larm);
            if (lmiss) { g->cur = lmiss; AT(g); }
        }

        g->cur = larm; AT(g);
        int saved_n = g->locals.n;
        for (int kb = 0; kb < p0->bindc; kb++)
            asg_push(&g->locals, p0->binds[kb],
                     p0->vfield_types ? p0->vfield_types[kb] : NULL,
                     bslots[kb], arm->line);
        if (arm->guard) {
            Val gv = cg_expr(g, arm->guard);
            if (!gv.v) ERRV(g, "line %zu: bad match guard", arm->line);
            if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, gv.v, lbody, lnext);
            g->cur = lbody; AT(g);
        }
        Val bv = cg_expr(g, arm->body);
        if (!bv.v) ERRV(g, "line %zu: bad match arm body", arm->line);
        bv = coerce(g, rty, bv, arm->line);
        LLVMBuildStore(g->ab, bv.v, res);
        g->locals.n = saved_n; /* truncate this arm's binds */
        if (!DONE(g->cur)) LLVMBuildBr(g->ab, lj);
        if (last_arm && arm->guard) {
            /* a guarded last arm falling through is a forged map */
            g->cur = lnext; AT(g);
            LLVMBuildUnreachable(g->ab);
        } else if (lnext) {
            g->cur = lnext; AT(g);
        }
    }

    g->cur = lj; AT(g);
    return val_make(rty, LLVMBuildLoad2(g->ab, slot, res, "mv"));
}

/* `if let P [| P] = value { then } [else ...] }` — the libLLVM mirror of
 * cg_if_let() in codegen_expr.c: one arm's worth of pattern blocks, the
 * first hit binds and runs the then-branch, a miss runs the else-branch.
 * Binds are block-scoped (registered around the then-branch, truncated
 * after). No guards in this form. */
static void cg_if_let(CG *g, Node *n)
{
    AT(g);
    Val v = cg_expr(g, n->as.iflet.value);
    if (!v.v) ERRX(g, "line %zu: bad if-let value", n->line);

    LLVMValueRef dollark = cg_string_val(g, "$", 1);
    LLVMTypeRef gats[3] = { g->i8ptr, g->i8ptr, g->i8ptr };
    LLVMValueRef tag = LLVMBuildCall2(g->ab,
            LLVMFunctionType(g->i8ptr, gats, 3, 0),
            rt_decl(g, "lume_map_get_s", g->i8ptr, gats, 3),
            (LLVMValueRef[]){ v.v, dollark, LLVMConstNull(g->i8ptr) }, 3, "tag");

    MatchPat *p0 = &n->as.iflet.pats[0];
    if (p0->bindc > MATCH_BIND_MAX)
        ERRX(g, "line %zu: too many if-let bindings (max %d)",
             n->line, MATCH_BIND_MAX);
    LLVMValueRef bslots[MATCH_BIND_MAX];
    for (int kb = 0; kb < p0->bindc; kb++) {
        Type *bt = p0->vfield_types ? p0->vfield_types[kb] : NULL;
        LLVMTypeRef blt = bt ? ty_of(g, bt) : NULL;
        if (!blt)
            ERRX(g, "line %zu: if-let binding '%s' has no codegen type",
                 n->line, p0->binds[kb]);
        bslots[kb] = LLVMBuildAlloca(g->ab, blt, p0->binds[kb]);
    }

    LLVMBasicBlockRef lt = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "ilt");
    LLVMBasicBlockRef lel = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "ielse");
    LLVMBasicBlockRef lend = LLVMAppendBasicBlockInContext(g->ctx, g->fn, "iend");

    for (int pi = 0; pi < n->as.iflet.patc; pi++) {
        MatchPat *pat = &n->as.iflet.pats[pi];
        bool last_pat = pi == n->as.iflet.patc - 1;
        LLVMBasicBlockRef lpb =
            LLVMAppendBasicBlockInContext(g->ctx, g->fn, "ipat");
        LLVMBasicBlockRef lmiss = last_pat ? lel
            : LLVMAppendBasicBlockInContext(g->ctx, g->fn, "imiss");
        if (strcmp(pat->vname, "_") == 0) {
            if (!DONE(g->cur)) LLVMBuildBr(g->ab, lpb);
        } else {
            LLVMValueRef patk = cg_string_val(g, pat->vname,
                                              strlen(pat->vname));
            LLVMValueRef eq64 = LLVMBuildCall2(g->ab,
                    LLVMFunctionType(g->i64, gats, 2, 0),
                    rt_decl(g, "lume_str_eq", g->i64,
                            (LLVMTypeRef[]){ g->i8ptr, g->i8ptr }, 2),
                    (LLVMValueRef[]){ tag, patk }, 2, "eq");
            LLVMValueRef eq = LLVMBuildICmp(g->ab, LLVMIntNE, eq64,
                                            LLVMConstInt(g->i64, 0, 0), "isv");
            if (!DONE(g->cur)) LLVMBuildCondBr(g->ab, eq, lpb, lmiss);
        }

        g->cur = lpb; AT(g);
        for (int kb = 0; kb < pat->bindc; kb++) {
            Type *bt = pat->vfield_types ? pat->vfield_types[kb] : NULL;
            const char *gfn = map_get_fn(bt);
            if (!gfn)
                ERRX(g, "line %zu: if-let binding '%s' payload must be int, "
                        "float, string or bool", n->line, pat->binds[kb]);
            LLVMTypeRef call_ty = ty_of(g, bt);
            if (bt->kind == TY_BOOL) call_ty = g->i64;
            LLVMValueRef fk = cg_string_val(g, pat->vfields[kb],
                                            strlen(pat->vfields[kb]));
            LLVMTypeRef pf[3] = { g->i8ptr, g->i8ptr, call_ty };
            LLVMValueRef dflt = bt->kind == TY_FLOAT
                                  ? LLVMConstReal(ty_of(g, bt), 0.0)
                                : call_ty == g->i8ptr
                                  ? LLVMConstNull(g->i8ptr)
                                  : LLVMConstInt(call_ty, 0, 0);
            LLVMValueRef pv = LLVMBuildCall2(g->ab,
                    LLVMFunctionType(call_ty, pf, 3, 0),
                    rt_decl(g, gfn, call_ty, pf, 3),
                    (LLVMValueRef[]){ v.v, fk, dflt }, 3, "pv");
            if (bt->kind == TY_BOOL)
                pv = LLVMBuildICmp(g->ab, LLVMIntNE, pv,
                                   LLVMConstInt(g->i64, 0, 0), "pb");
            LLVMBuildStore(g->ab, pv, bslots[kb]);
        }
        if (!DONE(g->cur)) LLVMBuildBr(g->ab, lt);
        if (lmiss != lel) { g->cur = lmiss; AT(g); }
    }

    g->cur = lt; AT(g);
    int saved_n = g->locals.n;
    for (int kb = 0; kb < p0->bindc; kb++)
        asg_push(&g->locals, p0->binds[kb],
                 p0->vfield_types ? p0->vfield_types[kb] : NULL,
                 bslots[kb], n->line);
    cg_stmt(g, n->as.iflet.then);
    g->locals.n = saved_n;
    if (!DONE(g->cur)) LLVMBuildBr(g->ab, lend);

    g->cur = lel; AT(g);
    if (n->as.iflet.els) cg_stmt(g, n->as.iflet.els);
    if (!DONE(g->cur)) LLVMBuildBr(g->ab, lend);

    g->cur = lend; AT(g);
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

/* Match-arm binds, for the window their arm body's type is inferred in.
 * Mirrors codegen_scan.c's copy: a repeated name overwrites (no new
 * strdup), distinct names are strdup'd once and freed by arm_scope_clear
 * when the outermost match is done. */
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
    g->arm_scope.v[g->arm_scope.n].name = xstrdup(name);
    g->arm_scope.v[g->arm_scope.n].ty   = ty;
    g->arm_scope.n++;
}

static void arm_scope_clear(CG *g)
{
    for (int i = 0; i < g->arm_scope.n; i++) free(g->arm_scope.v[i].name);
    free(g->arm_scope.v);
    g->arm_scope = (LTys){0};
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
        Asg *a = asg_find(&g->locals, n->as.var.name, n->line);
        if (a) return a->ty;
        /* A top-level binding lifts to a module global (SPEC 8.1 #10). */
        a = gvar_find(&g->gvars, n->as.var.name);
        if (a) return a->ty;
        /* A match arm whose body is being type-inferred: its binds. */
        return lt_find(&g->arm_scope, n->as.var.name);
    }
    case N_FUNC_LIT: {
        /* A closure value; thread the node through Type.func (mirrors
         * codegen_scan.c). */
        Type *ft = type_func(n->as.funclit.arity, n->as.funclit.param_types,
                             n->as.funclit.ret);
        ft->func = n;
        return ft;
    }
    case N_CALL: {
        if (!n->as.call.callee || n->as.call.callee->type != N_VAR) return NULL;
        const char *name = n->as.call.callee->as.var.name;

        /* Closure-valued variable call: `f(3)` where f : func. */
        {
            Type *ct = infer_node_type(g, n->as.call.callee);
            if (ct && ct->kind == TY_FUNC && ct->func &&
                ct->func->type == N_FUNC_LIT)
                return ct->func->as.funclit.ret;
        }

        /* Higher-order builtins that consume a closure. */
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
            return type_list(any_type());
        }
        if (strcmp(name, "filter") == 0) {
            Type *lt = infer_node_type(g, n->as.call.args[1]);
            if (lt && lt->kind == TY_LIST) return lt;
            return type_list(any_type());
        }
        if (strcmp(name, "reduce") == 0)
            return infer_node_type(g, n->as.call.args[2]);

        /* json()/stringify() serialize to a JSON string. json(str) means
         * parse, whose result type is dynamic — NULL here keeps the slot
         * opaque and lets the emit-time diagnostic do the explaining. */
        if ((strcmp(name, "stringify") == 0 || strcmp(name, "json") == 0) &&
            n->as.call.argc == 1) {
            Type *at = infer_node_type(g, n->as.call.args[0]);
            if (strcmp(name, "json") == 0 && at && at->kind == TY_STRING)
                return NULL;
            return type_prim(TY_STRING);
        }

        Sig *s = sig_find(&g->sigs, name);
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
    case N_STRUCT_UPDATE: {
        /* Kept in step with codegen_scan.c's copy: a `let` whose init is a
         * `with` update must infer a slot type. Named struct keeps its schema,
         * map becomes a fresh anonymous struct. */
        Type *bt = infer_node_type(g, n->as.supd.base);
        if (bt && bt->kind == TY_STRUCT && bt->name)
            return bt;
        return type_anon_struct();
    }
    case N_MEMBER:   return member_field_type(g, n);
    case N_ASSIGN:
        /* An assignment expression yields the *target*, which is what `for
         * (i = 0; ...)` and `a = b` want to carry. Mirrors codegen_scan.c;
         * without it the LLVM emitter answers "cannot infer a type" where
         * the text emitter (and the interpreter) accept the assignment. */
        return infer_node_type(g, n->as.assign.value);
    case N_ASSIGN_MEMBER:
        return infer_node_type(g, n->as.assign_mem.value);
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
    case N_TERNARY: {
        /* 与 codegen_scan.c / typecheck_expr.c 同一套统一规则：同型→它，
         * int/float 混合→float，任一 any/未知→any。cg_ternary 拿它给
         * 结果 alloca 定型。 */
        Type *tt = infer_node_type(g, n->as.ternary.then);
        Type *et = infer_node_type(g, n->as.ternary.els);
        if (!tt || !et || tt->kind == TY_ANY || et->kind == TY_ANY)
            return any_type();
        if (tt->kind == et->kind)
            return tt;
        bool tf = tt->kind == TY_INT || tt->kind == TY_FLOAT;
        bool ef = et->kind == TY_INT || et->kind == TY_FLOAT;
        if (tf && ef)
            return type_prim(TY_FLOAT);
        return NULL;
    }
    case N_CTOR:
        /* 构造值的类型 = 它的 enum（resolve_ctors 盖了 ename）。 */
        return n->as.ctor.ename ? type_enum(n->as.ctor.ename) : NULL;
    case N_MATCH: {
        /* 臂体按三元的统一规则收拢——与 codegen_scan.c 同一套。臂内绑定
         * 只在它自己的臂里可见（arm_scope 推断窗口，嵌套 match 恢复外层
         * 视图，最外层收尾释放）。 */
        int saved = g->arm_scope.n;
        Type *result = NULL;
        for (int i = 0; i < n->as.matchn.armc; i++) {
            MatchArm *arm = &n->as.matchn.arms[i];
            g->arm_scope.n = saved;
            /* the arm's single binding set: the checker guarantees every
             * alternative binds the same names with the same types */
            MatchPat *p0 = &arm->pats[0];
            for (int b = 0; b < p0->bindc; b++)
                arm_scope_push(g, p0->binds[b],
                               p0->vfield_types ? p0->vfield_types[b] : NULL);
            Type *bt = infer_node_type(g, arm->body);
            /* Same rule as codegen_scan.c: an arm with no statically
             * readable type is skipped (its real type exists at emit time);
             * only an explicit `any` arm widens the match. */
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
        g->arm_scope.n = saved;
        if (saved == 0) arm_scope_clear(g);
        return result ? result : any_type();
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

/* The module-global name a top-level binding is lifted into (SPEC 8.1 #10).
 * Numbered like the %lv_ locals so two same-name bindings never share
 * storage. Creates the LLVM global zero-initialized (LLVMConstNull covers
 * every spellable type: i1/i64/double/ptr and a named struct body). */

static LLVMValueRef gvar_make(CG *g, const char *name, Type *ty)
{
    static int s_uid = 0;
    char buf[128];
    snprintf(buf, sizeof buf, "lv_%s_%d", name, s_uid++);
    LLVMTypeRef lt = ty_of(g, ty);
    if (!lt) return NULL;
    LLVMValueRef gv = LLVMAddGlobal(g->mod, lt, buf);
    LLVMSetInitializer(gv, LLVMConstNull(lt));
    return gv;
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
        if (g->scanning_top) {
            /* Module storage, registered twice (mirrors codegen_scan.c):
             * gvars for what function bodies resolve through, L_top's own
             * locals for what the top level's stores hit. */
            LLVMValueRef slot = gvar_make(g, n->as.let.name, ty);
            if (!slot)
                ERRX(g, "line %zu: top-level variable '%s' has no codegen type",
                     n->line, n->as.let.name);
            asg_push(&g->gvars, n->as.let.name, ty, slot, n->line);
            asg_push(&g->locals, n->as.let.name, ty, slot, n->line);
            break;
        }
        asg_push(&g->locals, n->as.let.name, ty, NULL, n->line);
        break;
    }
    case N_FOR: {
        /* `for (x in xs)` binds the loop variable — it is not a `let`, so
         * without registering it here the body's use of it dies in codegen.
         * It binds in the enclosing scope (mirrors codegen_scan.c). At the
         * top level that scope is the module: a function the top level calls
         * after the loop must see the rebound value. */
        if (n->as.fors.is_in) {
            Type *et = for_in_elem_type(g, n);
            if (g->scanning_top) {
                LLVMValueRef slot = gvar_make(g, n->as.fors.var, et);
                if (!slot)
                    ERRX(g, "line %zu: variable '%s' has no codegen type",
                         n->line, n->as.fors.var);
                asg_push(&g->gvars, n->as.fors.var, et, slot, n->line);
                asg_push(&g->locals, n->as.fors.var, et, slot, n->line);
            } else {
                asg_push(&g->locals, n->as.fors.var, for_in_elem_type(g, n), NULL,
                         n->line);
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
            /* A C-style for's init `let` stays a plain local (mirrors
             * codegen_scan.c: the interpreter binds it for the loop). */
            asg_push(&g->locals, init->as.let.name, ty, NULL, init->line);
        } else if (init) {
            scan_stmt(g, init);
        }
        if (n->as.fors.incr &&
            (n->as.fors.incr->type == N_ASSIGN ||
             n->as.fors.incr->type == N_EXPR_STMT))
            scan_stmt(g, n->as.fors.incr);
        scan_stmt(g, n->as.fors.body);
        break;
    }
    /* An assignment to a name that is not a local defines a fresh
     * function-local cell (the interpreter never assigns through to a
     * top-level binding) — register it so the alloca pass sees it. Mirrors
     * codegen_scan.c's N_ASSIGN case. */
    case N_ASSIGN: {
        if (!g->scanning_top &&
            !asg_find(&g->locals, n->as.assign.name, n->line)) {
            Asg *gv = gvar_find(&g->gvars, n->as.assign.name);
            if (gv) {
                Type *ty = infer_node_type(g, n->as.assign.value);
                asg_push(&g->locals, n->as.assign.name, ty ? ty : gv->ty,
                         NULL, n->line);
            }
        }
        break;
    }
    /* A bare `x = e;` wraps its assignment in an expr-stmt: unwrap so the
     * registration above can fire (mirrors codegen_scan.c). */
    case N_EXPR_STMT: {
        Node *e = n->as.expr_stmt.expr;
        if (e && (e->type == N_ASSIGN || e->type == N_ASSIGN_MEMBER))
            scan_stmt(g, e);
        break;
    }
    /* A nested block is the body of if/while/for and any bare { } — its `let`s
     * have to be registered too, or every use of such a local dies in codegen
     * ("bad operands" on a read, "unknown variable" on an assignment). */
    case N_BLOCK:  scan_block(g, n); break;
    case N_IF:     scan_stmt(g, n->as.ifs.then); scan_stmt(g, n->as.ifs.els); break;
    /* if-let binds are registered at emit time (like match arms); the
     * branches still need walking for their lets/assignments. */
    case N_IF_LET: scan_stmt(g, n->as.iflet.then); scan_stmt(g, n->as.iflet.els); break;
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
        to->kind != TY_STRUCT && to->kind != TY_ANY && to->kind != TY_ENUM)
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
    /* Same branch as codegen_stmt.c's shared copy: an enum value is the
     * opaque i8* at_obj hands back, and the loop variable keeps its enum
     * type so print/match dispatch stay correct. */
    case TY_ENUM:   return "lume_list_at_obj";
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

    Asg *a = asg_find(&g->locals, n->as.fors.var, n->line);
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

        Asg *a = asg_find(&g->locals, n->as.let.name, n->line);
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
    case N_IF_LET:  cg_if_let(g, n); break;
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

/* The one shape every function definition takes. `sym` is the LLVM symbol
 * (L_<name> for a named function, the mangled __lume_clo_N for a closure);
 * `has_cap` prepends the closure-context parameter a lambda's define carries
 * and every call site passes the loaded .cap into (null in this phase). */

static void cg_def(CG *g, const char *sym, char **names, Type **params,
                   int arity, Type *ret, Node *body, int has_cap, size_t line)
{
    int np = arity + (has_cap ? 1 : 0);
    LLVMTypeRef *pts = NULL;
    if (np > 0) {
        pts = (LLVMTypeRef *)xmalloc(sizeof(LLVMTypeRef) * (size_t)np);
        if (has_cap) pts[0] = g->i8ptr;
        for (int i = 0; i < arity; i++) {
            pts[i + (has_cap ? 1 : 0)] = params ? ptr_of(g, params[i]) : NULL;
            if (!pts[i + (has_cap ? 1 : 0)])
                ERRX(g, "line %zu: parameter '%s' has no codegen type",
                     line, names ? names[i] : "?");
        }
    }

    LLVMTypeRef rty = ty_of(g, ret);
    LLVMTypeRef fty = LLVMFunctionType(rty ? rty : g->void_ty,
                                       np > 0 ? pts : NULL, (unsigned)np, 0);

    /* LLVMAddFunction always creates and then *uniques* the name (L_g.1, ...)
     * — the get-or-insert semantics live in the C++ helper a call goes
     * through, not in this API. A forward call (or the boxing call a lambda
     * literal emits) already declared this symbol, so it is reused instead;
     * otherwise the body would land on a copy. */
    LLVMValueRef prev = LLVMGetNamedFunction(g->mod, sym);
    g->fn = prev ? prev : LLVMAddFunction(g->mod, sym, fty);

    /* Locals move aside: a variable of the caller must not be visible here.
     * The loop-label stacks too: a `break` inside this body can never target
     * a loop outside it, and with the stacks cleared the emitter reports
     * "'break' outside a loop" instead of branching into another function.
     * L_top is the one body that arrives with its locals already scanned
     * (the driver's pre-scan, which lifted the top-level bindings into
     * module globals) — reuse that table instead of scanning a second time. */
    Asgs saved = g->locals;
    int preset = g->scanning_top && g->preset_locals;
    if (preset)
        g->locals = *g->preset_locals;
    else
        memset(&g->locals, 0, sizeof g->locals);
    LLVMBasicBlockRef *saved_brk = g->brk; int snbrk = g->nbrk, scbrk = g->cbrk;
    LLVMBasicBlockRef *saved_cnt = g->cnt; int sncnt = g->ncnt, sccnt = g->ccnt;
    g->brk = g->cnt = NULL; g->nbrk = g->ncnt = g->cbrk = g->ccnt = 0;
    Type *saved_ret = g->cur_ret;
    g->cur_ret = ret;

    /* Parameters first, so their slots exist before the body is scanned. A
     * struct parameter is *already* a pointer, so it keeps the parameter value
     * as its slot. With a capture context the closure's own parameters start
     * at LLVM param index 1. */
    LLVMValueRef *pv = NULL;
    if (arity > 0) {
        pv = (LLVMValueRef *)xmalloc(sizeof(LLVMValueRef) * (size_t)np);
        /* LLVMGetParams fills one entry per function parameter, the capture
         * context first when has_cap — so the readback below indexes
         * pv[i + 1] for a closure's own parameters. */
        LLVMGetParams(g->fn, pv);
        for (int i = 0; i < arity; i++)
            asg_push(&g->locals, names[i], params[i],
                     (params[i] && params[i]->kind == TY_STRUCT)
                         ? pv[i + (has_cap ? 1 : 0)] : NULL,
                     0);
    }

    if (!preset) scan_block(g, body);

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
                 line, a->name);
        a->slot = LLVMBuildAlloca(g->ab, ty_of(g, a->ty), "lv");
    }
    if (pv) {
        for (int i = 0; i < arity; i++) {
            if (params[i] && params[i]->kind == TY_STRUCT) continue;
            LLVMBuildStore(g->ab, pv[i + (has_cap ? 1 : 0)],
                           asg_find(&g->locals, names[i], 0)->slot);
        }
    }

    if (body) cg_block(g, body);

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

    if (preset) {
        /* The pre-scanned table stays owned by the driver; only the shallow
         * copy here goes away. */
        memset(&g->locals, 0, sizeof g->locals);
    } else {
        asgs_free(&g->locals);
    }
    g->locals = saved;
    /* The body ran with its own break/continue stacks (cg_def NULLs them on
     * the way in); they were grown by push_bb inside this function, so free
     * them before the outer pointers come back -- restoring over them used
     * to drop the blocks and CI's asan leg caught exactly that (a top-level
     * for-in makes L_top itself a body that grows both stacks). */
    free(g->brk); free(g->cnt);
    g->brk = saved_brk; g->nbrk = snbrk; g->cbrk = scbrk;
    g->cnt = saved_cnt; g->ncnt = sncnt; g->ccnt = sccnt;
    g->cur_ret = saved_ret;
    g->fn = NULL;
    /* The two scratch vectors: LLVMFunctionType and LLVMGetParams only read
     * them, the IR owns the values, so nothing survives past this point. */
    free(pts);
    free(pv);
}

static void cg_function(CG *g, Node *fn)
{
    char sym[160];
    snprintf(sym, sizeof sym, "L_%s", fn->as.func.name ? fn->as.func.name : "?");
    cg_def(g, sym, fn->as.func.names, fn->as.func.param_types,
           fn->as.func.arity, fn->as.func.ret, fn->as.func.body, 0, fn->line);
}

/* A closure's `define`: the lambda node the signature pass named, emitted
 * with the leading capture parameter its call sites always pass. The body
 * runs through the same cg_def as a named function — the capture context is
 * null in this phase (no free-variable capture yet), but the parameter is
 * part of the calling convention the boxing call and the fn-pointer
 * invocations are built against. */

static void cg_closure(CG *g, Node *fn)
{
    if (!fn->as.funclit.cname)
        ERRX(g, "line %zu: internal: closure was never given a mangled name",
             fn->line);
    cg_def(g, fn->as.funclit.cname, fn->as.funclit.names,
           fn->as.funclit.param_types, fn->as.funclit.arity,
           fn->as.funclit.ret, fn->as.funclit.body, 1, fn->line);
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

/* Collect every lambda the signature pass named, in one pass over the tree —
 * the same walk the text emitter's driver runs (codegen.c), kept here because
 * each backend owns its module assembly. Only nodes with a mangled name are
 * collected: a route handler is a function literal too, but it goes through
 * the FFI path and never gets a cname. The walk descends into lambda bodies
 * as well, so a lambda nested in a lambda is found from the outer one. */

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
    case N_IF:
        collect_lambdas(n->as.ifs.cond, out, cnt, cap);
        collect_lambdas(n->as.ifs.then, out, cnt, cap);
        collect_lambdas(n->as.ifs.els,  out, cnt, cap);
        return;
    case N_IF_LET:
        collect_lambdas(n->as.iflet.value, out, cnt, cap);
        collect_lambdas(n->as.iflet.then, out, cnt, cap);
        collect_lambdas(n->as.iflet.els,  out, cnt, cap);
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
    case N_MATCH:
        collect_lambdas(n->as.matchn.scrutinee, out, cnt, cap);
        for (int i = 0; i < n->as.matchn.armc; i++)
            collect_lambdas(n->as.matchn.arms[i].body, out, cnt, cap);
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
        /* An enum has no runtime presence of its own: every variant value is
         * built at its construction site (a tagged map), the constructor
         * rewrite happened in the parser, and the type rules live in the
         * checker. Nothing to emit, nothing to skip silently. Same branch as
         * codegen.c's copy of this walk. */
        else if (n->type == N_ENUM_DECL)
            continue;
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
        else if (top[i]->type == N_TYPE_DECL || top[i]->type == N_IMPORT ||
                 top[i]->type == N_ENUM_DECL) continue;
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
            else if (top[i]->type == N_ENUM_DECL) continue;   /* no runtime presence */
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

    /* ---- pass 1.7: lift the top-level bindings into module globals ----
     * Mirrors codegen.c: one global per top-level `let` / for-in variable,
     * scanned once so L_top's stores and every function body's loads name
     * the same storage (the interpreter's env semantics, spelled as one
     * shared slot). The scanned table is handed to cg_def through
     * preset_locals, so L_top is never scanned twice. */
    Asgs top_locals;
    memset(&top_locals, 0, sizeof top_locals);
    if (ntops > 0) {
        Node body;
        memset(&body, 0, sizeof body);
        body.type = N_BLOCK;
        body.as.block.stmts = tops;
        body.as.block.count = ntops;

        cg.scanning_top = 1;
        scan_block(&cg, &body);
        cg.scanning_top = 0;
        top_locals = cg.locals;      /* steal the scanned table */
        memset(&cg.locals, 0, sizeof cg.locals);
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
        /* The table pass 1.7 scanned IS L_top's locals: its entries carry the
         * module globals, so every store the top level makes lands in the
         * same global a function body reads. */
        cg.preset_locals = &top_locals;
        cg.scanning_top = 1;
        cg_function(&cg, top_fn);
        cg.scanning_top = 0;
        cg.preset_locals = NULL;
        asgs_free(&top_locals);
        cg.capture_main = 0;
        cg.exit_val = NULL;
        free(top_fn);
    }
    free(fns); free(tops);

    /* Closures: every lambda the signature pass named gets its own define.
     * The bodies are attached here, after the named functions, through the
     * same get-or-insert path a forward call used when the lambda literal was
     * boxed — so the symbol cg_funclit declared and called is the one that
     * gains a body. */
    {
        Node **clos = NULL;
        int nclos = 0, capc = 0;
        collect_lambdas(prog, &clos, &nclos, &capc);
        for (int i = 0; i < nclos; i++) cg_closure(&cg, clos[i]);
        free(clos);
    }

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
        asgs_free(&cg.gvars);
        free(msg);
        return NULL;
    }

    LLVMDisposeBuilder(cg.ab);
    sigs_free(&cg.sigs); sdefs_free(&cg.structs);
    asgs_free(&cg.gvars);
    free(cg.brk); free(cg.cnt);
    /* The module outlives this function, so the context cannot be disposed
     * here — hand it to the caller instead. It is disposed after the module,
     * which owns nothing of its own but every type and value it mentions. */
    if (out_ctx) *out_ctx = ctx;
    return cg.mod;
}
