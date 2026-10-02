/* codegen.c — Lume AST → LLVM IR text.
 *
 * Design notes (wider rationale in README.md):
 *
 *  - IR is emitted as *text*; clang/cc performs the native step, so this
 *    project never links libLLVM.
 *  - Locals are `alloca` + load/store instead of SSA with phi nodes, which
 *    keeps the emitter trivial. `clang -O2` runs mem2reg and turns the whole
 *    thing into proper SSA afterwards.
 *  - IR is appended to one buffer in source order, so every alloca must land
 *    in the entry block: each function body is scanned once for variable
 *    names before the entry block is emitted.
 *  - Anything outside the supported subset is rejected with an explicit
 *    error; we never emit broken IR.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lume.h"
#include "codegen.h"
#include "irbuf.h"

/* ----------------------------------------------------------------- utils --- */

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("lume-llvm: out of memory\n", stderr); exit(70); }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fputs("lume-llvm: out of memory\n", stderr); exit(70); }
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

#define EMIT(g, ...) irbuf_printf((g)->ir, __VA_ARGS__)

/* ------------------------------------------------------------ codegen ctx --- */

typedef struct { char *name; Type *ty; char *slot; } Asg;
typedef struct { Asg *v; int n, cap; } Asgs;

typedef struct { char *name; Type *ret; Type **params; int arity; } Sig;
typedef struct { Sig *v; int n, cap; } Sigs;

typedef struct {
    char  *name;
    char **names;
    Type **types;
    int    count;
} SDef;
typedef struct { SDef *v; int n, cap; } SDefs;

typedef struct {
    IrBuf *ir;              /* body: struct types + function definitions   */
    IrBuf  gs;             /* globals: string constants, flushed up front */
    char   err[1024];

    Sigs   sigs;            /* every top-level func, collected before emitting */
    SDefs  structs;         /* every `type X = {...}` declaration            */
    Asgs   locals;          /* variables of the function currently emitted   */
    Sig   *cur;             /* signature of the function currently emitted   */
    Type  *expect;          /* type the expression context wants (struct lits) */

    int    lid;             /* label id counter            */
    int    tid;             /* temp/id counter            */
    int    sid;             /* string constant counter    */

    int   *brk; int nbrk, cbrk;    /* stack of enclosing loop labels */
    int   *cnt; int ncnt, ccnt;
} CG;

/* Report an error into g->err and bail out. Three variants because C has no
 * way to "return whatever this function returns": ERR for char * results,
 * ERRV for Val, ERRX for void. */
#define ERR(g, fmt, ...)                                                       \
    do {                                                                       \
        snprintf((g)->err, sizeof (g)->err, fmt, ##__VA_ARGS__);               \
        return NULL;                                                           \
    } while (0)

#define ERRV(g, fmt, ...)                                                      \
    do {                                                                       \
        snprintf((g)->err, sizeof (g)->err, fmt, ##__VA_ARGS__);               \
        return (Val){ NULL, NULL };                                            \
    } while (0)

#define ERRX(g, fmt, ...)                                                      \
    do {                                                                       \
        snprintf((g)->err, sizeof (g)->err, fmt, ##__VA_ARGS__);               \
        return;                                                                \
    } while (0)

static void asgs_free(Asgs *a)
{
    for (int i = 0; i < a->n; i++) { free(a->v[i].name); free(a->v[i].slot); }
    free(a->v);
    memset(a, 0, sizeof *a);
}

/* `slot` is the LLVM name holding the variable's address; passing NULL asks
 * for the ordinary `%lv_<name>` local slot. */
static void asg_push(Asgs *a, const char *name, Type *ty, const char *slot)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->v = (Asg *)xrealloc(a->v, (size_t)a->cap * sizeof *a->v);
    }
    char default_slot[128];
    if (slot)
        snprintf(default_slot, sizeof default_slot, "%s", slot);
    else
        snprintf(default_slot, sizeof default_slot, "%%lv_%s", name);

    a->v[a->n].name = xstrdup(name);
    a->v[a->n].ty   = ty;
    a->v[a->n].slot = xstrdup(default_slot);
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

static void stack_push_int(int **v, int *n, int *cap, int x)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *v = (int *)xrealloc(*v, (size_t)*cap * sizeof(int));
    }
    (*v)[(*n)++] = x;
}

/* ----------------------------------------------------------- type mapping --- */

/* LLVM spellings of named struct types carry the `%` sigil. They are interned
 * rather than written into a scratch buffer, because llvm_type_of is called
 * twice inside a single format string (`getelementptr inbounds %s, %s* %s`),
 * and a shared scratch buffer would be overwritten before the format ran. */
#define MAX_STRUCT_SPELLINGS 64
typedef struct { char name[160]; char val[160]; char ptr[160]; } StructSpell;
static StructSpell struct_spell[MAX_STRUCT_SPELLINGS];
static int  struct_spell_n;

/* Two spellings per struct: `%Rect` for the aggregate itself (alloca/store) and
 * `%Rect*` for a reference to one (parameters, arguments, variable reads). */
static const char *intern_struct_spelling(const char *name, int as_ptr)
{
    for (int i = 0; i < struct_spell_n; i++)
        if (strcmp(struct_spell[i].name, name) == 0)
            return as_ptr ? struct_spell[i].ptr : struct_spell[i].val;

    if (struct_spell_n >= MAX_STRUCT_SPELLINGS) return NULL;
    StructSpell *s = &struct_spell[struct_spell_n++];
    snprintf(s->name, sizeof s->name, "%s", name);
    snprintf(s->val,  sizeof s->val,  "%%%s",  name);
    snprintf(s->ptr,  sizeof s->ptr,  "%%%s*", name);
    return as_ptr ? s->ptr : s->val;
}

static const char *llvm_type_of(Type *t);

/* How a reference to `t` is spelled: same as llvm_type_of, except that a struct
 * is always referenced through a pointer — that is how struct parameters and
 * arguments are declared. */
static const char *llvm_ptr_type_of(Type *t)
{
    if (t && t->kind == TY_STRUCT) return intern_struct_spelling(t->name, 1);
    return llvm_type_of(t);
}

/* lume Type -> LLVM type spelling. NULL means "not supported by this backend". */
static const char *llvm_type_of(Type *t)
{
    if (!t) return NULL;
    switch (t->kind) {
    case TY_INT:    return "i64";
    case TY_FLOAT:  return "double";
    case TY_BOOL:   return "i1";
    case TY_STRING: return "i8*";
    case TY_STRUCT:
        if (!t->name) return NULL;   /* anonymous structs: not supported */
        return intern_struct_spelling(t->name, 0);
    default:        return NULL;
    }
}

/* ---------------------------------------------------------- string consts --- */

/* Emit `@.strN` holding text (plus a trailing NUL) and return an i8* value
 * that points at it, via getelementptr — no bitcast needed, and the GEP is
 * folded away by the optimizer. */
static char *cg_string_val(CG *g, const char *text, size_t len)
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

/* ------------------------------------------------------------- expressions -- */

typedef struct { char *v; Type *ty; } Val;

/* forward: struct literals convert their fields through it */
static Val coerce(CG *g, Type *to, Val v, size_t line);

static Val val_make(Type *ty, const char *v)
{
    Val r;
    r.ty = ty;
    r.v  = xstrdup(v);
    return r;
}

/* `%tN = <llvm type> <body>`; returns the new name. NULL if type unsupported. */
static char *emit_instrf(CG *g, Type *ty, const char *fmt, ...)
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

static Val cg_expr(CG *g, Node *n);
static void cg_stmt(CG *g, Node *n);
static void cg_block(CG *g, Node *blk);

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
        return val_make(type_prim(TY_STRING), cg_string_val(g, t, len));
    }

    if (k == LIT_NUM) {
        char buf[64];
        if (n->as.lit.is_float) {
            snprintf(buf, sizeof buf, "%.17g", n->as.lit.num);
            return val_make(type_prim(TY_FLOAT), buf);
        }
        snprintf(buf, sizeof buf, "%lld", (long long)n->as.lit.num);
        return val_make(type_prim(TY_INT), buf);
    }

    ERRV(g, "line %zu: 'null' literals are not supported by the native backend yet", n->line);
}

/* ------------------------------------------------------------------- var --- */

static Val cg_var(CG *g, Node *n)
{
    Asg *a = asg_find(&g->locals, n->as.var.name);
    if (!a) ERRV(g, "line %zu: unknown variable '%s'", n->line, n->as.var.name);

    const char *lt = llvm_type_of(a->ty);
    if (!lt) ERRV(g, "line %zu: variable '%s' has no codegen type", n->line, n->as.var.name);

    /* A struct is always *referenced*: the slot is its address, and loading it
     * would hand a callee an aggregate where it expects a pointer. */
    if (a->ty->kind == TY_STRUCT) return val_make(a->ty, xstrdup(a->slot));

    char *r = emit_instrf(g, a->ty, "load %s, %s* %s", lt, lt, a->slot);
    if (!r) ERRV(g, "line %zu: cannot load '%s'", n->line, n->as.var.name);
    return val_make(a->ty, r);
}

/* --------------------------------------------------------------- unary ------ */

static Val cg_unary(CG *g, Node *n)
{
    Val o = cg_expr(g, n->as.unary.operand);

    if (n->as.unary.op == OP_NOT) {
        char *r = emit_instrf(g, type_prim(TY_BOOL), "xor i1 %s, true", o.v);
        free(o.v);
        if (!r) ERRV(g, "line %zu: cannot negate this value", n->line);
        return val_make(type_prim(TY_BOOL), r);
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
    return val_make(o.ty, r);
}

/* --------------------------------------------------------------- binary ----- */

static Val cg_binary(CG *g, Node *n)
{
    Val a = cg_expr(g, n->as.binary.left);
    Val b = cg_expr(g, n->as.binary.right);
    Type *boolty = type_prim(TY_BOOL);

    /* Promote both operands to the arithmetic type *before* emitting anything:
     * `x / 2` with x: float must not become `fdiv double %x, 2`, because that
     * bare `2` is an i64 constant and the instruction would be rejected. */
    if (a.ty && b.ty && (a.ty->kind == TY_FLOAT || b.ty->kind == TY_FLOAT)) {
        a = coerce(g, type_prim(TY_FLOAT), a, n->line);
        b = coerce(g, type_prim(TY_FLOAT), b, n->line);
    }

    if (n->as.binary.op == OP_AND || n->as.binary.op == OP_OR) {
        if (!a.v || !b.v) ERRV(g, "line %zu: bad logical operands", n->line);
        /* No short-circuit: the subset here is parsed into eager operands, so
         * `and`/`or` lower to plain i1 bit ops (the checker already rejects
         * non-bool operands). */
        const char *op = n->as.binary.op == OP_AND ? "and" : "or";
        char *r = emit_instrf(g, boolty, "%s i1 %s, %s", op, a.v, b.v);
        free(a.v); free(b.v);
        if (!r) ERRV(g, "line %zu: bad logical operands", n->line);
        return val_make(boolty, r);
    }

    if (!a.v || !b.v) ERRV(g, "line %zu: bad operands", n->line);

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
        return val_make(boolty, r);
    }

    bool is_float = a.ty && a.ty->kind == TY_FLOAT;
    if (is_float && (a.ty->kind == TY_STRING || b.ty->kind == TY_STRING))
        ERRV(g, "line %zu: string arithmetic is not supported by the native backend yet", n->line);

    Type *rty = type_prim(is_float ? TY_FLOAT : TY_INT);
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
        case OP_DIV: r = emit_instrf(g, rty, "sdiv %s %s, %s", i64, a.v, b.v); break;
        case OP_MOD: r = emit_instrf(g, rty, "srem %s %s, %s", i64, a.v, b.v); break;
        default:     ERRV(g, "line %zu: bad operator", n->line);
        }
    }
    free(a.v); free(b.v);
    if (!r) ERRV(g, "line %zu: bad arithmetic operands", n->line);
    return val_make(rty, r);
}

/* ---------------------------------------------------------------- member ---- */

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

static Val cg_member(CG *g, Node *n)
{
    Val obj = cg_expr(g, n->as.member.obj);
    if (!obj.v) ERRV(g, "line %zu: bad struct operand", n->line);

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
    return val_make(fty, r);
}

/* --------------------------------------------------------------- assign ----- */

/* Assignment that shows up where an expression is expected. It emits the store
 * and then hands the assigned value back, so `for (k = 0; ...)` and friends
 * need no special case at the statement level. */
static Val cg_assign_expr(CG *g, Node *n)
{
    Val v = cg_expr(g, n->as.assign.value);

    Asg *a = asg_find(&g->locals, n->as.assign.name);
    if (!a) { free(v.v); ERRV(g, "line %zu: assignment to unknown variable '%s'",
                              n->line, n->as.assign.name); }

    v = coerce(g, a->ty, v, n->line);
    const char *lt = llvm_type_of(a->ty);
    if (!lt) { free(v.v); ERRV(g, "line %zu: variable '%s' has no codegen type",
                               n->line, n->as.assign.name); }

    EMIT(g, "  store %s %s, %s* %%lv_%s\n", lt, v.v, lt, n->as.assign.name);
    return v;
}

/* -------------------------------------------------------------- struct lit -- */

/* Struct literals arrive as map literals `{ w: 3, h: 4 }`; the type checker
 * already resolved them against the declared struct, so `ty` carries the name.
 * Built with a chain of insertvalue over undef — no alloca, so a struct
 * literal inside a loop cannot grow the stack. */
static Val cg_struct_lit(CG *g, Node *n);

/* A struct literal carries no type of its own — the parser makes it a map
 * literal — so it is resolved from the context (a `let` annotation, a function
 * return type), and failing that from the declared type whose field set
 * matches the literal exactly. */
static Type *resolve_struct_lit(CG *g, Node *n)
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
    if (!hit)
        ERR(g, "line %zu: cannot resolve this struct literal to a declared type", n->line);
    return hit;
}

static Val cg_struct_lit(CG *g, Node *n)
{
    Type *ty = resolve_struct_lit(g, n);
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
    return val_make(ty, cur);
}

/* ------------------------------------------------------------------ call ---- */

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
    const char *fn;
    char *arg = a.v;
    bool   widen = false;

    switch (a.ty ? a.ty->kind : TY_ANY) {
    case TY_INT:    fn = "@lume_print_i64";    break;
    case TY_FLOAT:  fn = "@lume_print_double"; break;
    case TY_BOOL:   fn = "@lume_print_bool";   widen = true;  break;
    case TY_STRING: fn = "@lume_print_str";    break;
    default:
        ERRV(g, "line %zu: print() cannot print this value", n->line);
    }

    if (widen) {
        char *z = emit_instrf(g, type_prim(TY_INT), "zext i1 %s to i64", a.v);
        if (z) { free(a.v); arg = z; }
    }

    /* The helper's *parameter* type is not always the value's type: strings are
     * i8* and bools are i1, but lume_print_str takes i8* and lume_print_bool
     * takes i64. Getting this wrong yields a type mismatch in the call. */
    const char *pty;
    switch (a.ty ? a.ty->kind : TY_ANY) {
    case TY_FLOAT:  pty = "double"; break;
    case TY_STRING: pty = "i8*";    break;
    default:        pty = "i64";    break;
    }

    char name[48];
    snprintf(name, sizeof name, "%%c%d", g->tid++);
    EMIT(g, "  %s = call %s %s(%s %s)\n", name, pty, fn, pty, arg);

    free(arg);
    return val_make(type_prim(TY_INT), xstrdup(name));
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

    if (!n->as.call.callee || n->as.call.callee->type != N_VAR)
        ERRV(g, "line %zu: only direct calls to named functions are supported", n->line);

    const char *name = n->as.call.callee->as.var.name;
    Sig *s = sig_find(&g->sigs, name);
    if (!s) ERRV(g, "line %zu: call to unknown function '%s'", n->line, name);
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
             * (an insertvalue chain) into a stack slot first. */
            if (args[i].ty && args[i].ty->kind == TY_STRUCT) {
                const char *st = llvm_type_of(args[i].ty);
                if (!st) ERRV(g, "line %zu: bad argument type for '%s'", n->line, name);
                if (args[i].v[0] != '%' || strncmp(args[i].v, "%lv_", 4) == 0) {
                    /* already a pointer value */
                } else {
                    char slot[64];
                    snprintf(slot, sizeof slot, "%%sa%d", g->tid++);
                    EMIT(g, "  %s = alloca %s\n", slot, st);
                    EMIT(g, "  store %s %s, %s* %s\n", st, args[i].v, st, slot);
                    free(args[i].v);
                    args[i].v = xstrdup(slot);
                }
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

    if (!rty) return val_make(s->ret, xstrdup("void"));
    return val_make(s->ret, xstrdup(name_res));
}

static Val cg_expr(CG *g, Node *n)
{
    if (!n) ERRV(g, "internal: null expression");

    switch (n->type) {
    case N_LITERAL: return cg_literal(g, n);
    case N_MAP_LIT: return cg_struct_lit(g, n);
    /* An assignment in expression position — a C-style `for` header wraps it
     * in an expression statement, and the parser hands those to expressions. */
    case N_ASSIGN:  return cg_assign_expr(g, n);
    case N_EXPR_STMT: return cg_expr(g, n->as.expr_stmt.expr);
    case N_VAR:     return cg_var(g, n);
    case N_UNARY:   return cg_unary(g, n);
    case N_BINARY:  return cg_binary(g, n);
    case N_MEMBER:  return cg_member(g, n);
    case N_CALL:    return cg_call(g, n);
    default:        ERRV(g, "%s", bad(g, n, "this expression"));
    }
}

/* -------------------------------------------------------------- scanning ---- */

/* The AST records an explicit `: T` annotation on `let` nodes; a type inferred
 * from the initializer is never stored on the node. Without this the entry
 * block would have nothing to `alloca` — and the alloca pass bails out, which
 * silently truncates the rest of the function body. */
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
        default:        return NULL;    /* null: no codegen type */
        }
    case N_VAR: {
        Asg *a = asg_find(&g->locals, n->as.var.name);
        return a ? a->ty : NULL;
    }
    case N_CALL: {
        /* `let b = f(...)` takes the return type of f — struct returns included,
         * which is how `let b = scale(a, 5)` gets its %Rect slot. */
        if (!n->as.call.callee || n->as.call.callee->type != N_VAR) return NULL;
        Sig *s = sig_find(&g->sigs, n->as.call.callee->as.var.name);
        return s ? s->ret : NULL;
    }
    default:
        return NULL;
    }
}

/* Convert a value to the declared type of a local. Needed because an
 * initializer is not always written in the target type: `let x: float = 9`
 * stores an i64 into a double slot, which is invalid IR if left alone. */
static Val coerce(CG *g, Type *to, Val v, size_t line)
{
    if (!to || !v.ty || to == v.ty) return v;

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
    return val_make(to, r);
}

/* Collect the variables a block introduces, so the entry block can allocate
 * all of them before any statement is emitted. */
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

    case N_IF:    scan_stmt(g, n->as.ifs.then); scan_stmt(g, n->as.ifs.els); break;
    case N_WHILE: scan_stmt(g, n->as.whiles.body); break;
    default: break;
    }
}

static void scan_block(CG *g, Node *blk)
{
    if (!blk) return;
    for (int i = 0; i < blk->as.block.count; i++) scan_stmt(g, blk->as.block.stmts[i]);
}

/* ------------------------------------------------------------- statements --- */

static void cg_assign_mem(CG *g, Node *n)
{
    Type *st = n->as.member.type;
    int idx = st && st->name ? struct_field_idx(g, st->name, n->as.member.name) : -1;
    if (idx < 0) ERRX(g, "line %zu: unknown field '%s'", n->line, n->as.member.name);

    Val obj = cg_expr(g, n->as.assign_mem.obj);
    Val val = cg_expr(g, n->as.assign_mem.value);

    Type *fty = struct_field_type(g, st->name, idx);
    const char *ft = fty ? llvm_type_of(fty) : NULL;
    if (!ft) ERRX(g, "line %zu: field '%s' has no codegen type", n->line, n->as.member.name);

    /* The struct spelling needs its `%` sigil: llvm_type_of, not st->name. */
    const char *stn = st && st->name ? llvm_type_of(st) : NULL;
    if (!stn) { free(obj.v); free(val.v); ERRX(g, "line %zu: unknown field '%s'",
                                               n->line, n->as.member.name); }

    /* Two indices: the leading `0` walks the pointer to the struct, the second
     * is the field. Omitting it would turn `1` into an *array* subscript,
     * silently reading offset 16 — the wrong field. */
    char *p = emit_instrf(g, st, "getelementptr inbounds %s, %s* %s, i32 0, i32 %d",
                          stn, stn, obj.v, idx);
    if (!p) { free(obj.v); free(val.v); ERRX(g, "line %zu: cannot access '%s'", n->line, n->as.member.name); }
    EMIT(g, "  store %s %s, %s* %s\n", ft, val.v, ft, p);

    free(obj.v); free(val.v); free(p);
}

static void cg_while(CG *g, Node *n)
{
    int lc = g->lid, lb = g->lid + 1, le = g->lid + 2;
    g->lid += 3;

    stack_push_int(&g->brk, &g->nbrk, &g->cbrk, le);
    stack_push_int(&g->cnt, &g->ncnt, &g->ccnt, lc);

    EMIT(g, "  br label %%L%d\n", lc);

    EMIT(g, "L%d:\n", lc);
    Val c = cg_expr(g, n->as.whiles.cond);
    EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n", c.v, lb, le);
    free(c.v);

    EMIT(g, "L%d:\n", lb);
    cg_stmt(g, n->as.whiles.body);
    EMIT(g, "  br label %%L%d\n", lc);

    EMIT(g, "L%d:\n", le);
    g->nbrk--; g->ncnt--;
}

static void cg_for(CG *g, Node *n)
{
    if (n->as.fors.is_in)
        ERRX(g, "line %zu: 'for ... in' is not supported by the native backend yet", n->line);

    if (n->as.fors.init) cg_stmt(g, n->as.fors.init);

    int lc = g->lid, lb = g->lid + 1, li = g->lid + 2, le = g->lid + 3;
    g->lid += 4;

    stack_push_int(&g->brk, &g->nbrk, &g->cbrk, le);
    stack_push_int(&g->cnt, &g->ncnt, &g->ccnt, lc);

    EMIT(g, "  br label %%L%d\n", lc);

    EMIT(g, "L%d:\n", lc);
    if (n->as.fors.cond) {
        Val c = cg_expr(g, n->as.fors.cond);
        EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n", c.v, lb, le);
        free(c.v);
    } else {
        EMIT(g, "  br label %%L%d\n", lb);
    }

    EMIT(g, "L%d:\n", lb);
    cg_stmt(g, n->as.fors.body);
    EMIT(g, "  br label %%L%d\n", li);

    EMIT(g, "L%d:\n", li);
    if (n->as.fors.incr) {
        /* The increment is a *statement*, not an expression: `k = k + 1` is an
         * assignment node wrapped in an expression statement, so emitting it
         * through cg_expr would bail out on the wrapper. */
        if (n->as.fors.incr->type == N_ASSIGN || n->as.fors.incr->type == N_EXPR_STMT)
            cg_stmt(g, n->as.fors.incr);
        else {
            Val v = cg_expr(g, n->as.fors.incr);
            free(v.v);
        }
    }
    EMIT(g, "  br label %%L%d\n", lc);

    EMIT(g, "L%d:\n", le);
    g->nbrk--; g->ncnt--;
}

static void cg_if(CG *g, Node *n)
{
    int lt = g->lid, lel = g->lid + 1, lend = g->lid + 2;
    g->lid += 3;

    Val c = cg_expr(g, n->as.ifs.cond);
    EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n", c.v, lt, lel);
    free(c.v);

    EMIT(g, "L%d:\n", lt);
    cg_stmt(g, n->as.ifs.then);
    EMIT(g, "  br label %%L%d\n", lend);

    EMIT(g, "L%d:\n", lel);
    if (n->as.ifs.els) cg_stmt(g, n->as.ifs.els);
    EMIT(g, "  br label %%L%d\n", lend);

    EMIT(g, "L%d:\n", lend);
}

static void cg_stmt(CG *g, Node *n)
{
    if (!n) return;

    switch (n->type) {
    case N_LET: {
        /* Hand the annotation to struct literals before evaluating them. */
        Type *prev = g->expect;
        g->expect = n->as.let.annot;
        Val v = cg_expr(g, n->as.let.init);
        g->expect = prev;

        /* Prefer the annotation so the stored value matches the slot that was
         * allocated for it (and gets converted when the two types differ). */
        Type *ty = n->as.let.annot ? n->as.let.annot : v.ty;
        v = coerce(g, ty, v, n->line);
        const char *lt = llvm_type_of(ty);
        if (!lt) ERRX(g, "line %zu: cannot store '%s' into a typed local",
                     n->line, n->as.let.name);
        EMIT(g, "  store %s %s, %s* %%lv_%s\n", lt, v.v, lt, n->as.let.name);
        free(v.v);
        break;
    }

    case N_ASSIGN:
        free(cg_assign_expr(g, n).v);
        break;

    case N_ASSIGN_MEMBER:
        cg_assign_mem(g, n);
        break;

    case N_RETURN: {
        if (!n->as.ret.expr) { EMIT(g, "  ret void\n"); break; }
        /* `return { .. }` needs the function's declared return type. */
        Type *prev = g->expect;
        g->expect = g->cur ? g->cur->ret : NULL;
        Val v = cg_expr(g, n->as.ret.expr);
        g->expect = prev;
        const char *rty = g->cur ? llvm_type_of(g->cur->ret) : NULL;
        if (!rty) rty = llvm_type_of(v.ty);
        if (!rty) ERRX(g, "line %zu: cannot return this value", n->line);
        EMIT(g, "  ret %s %s\n", rty, v.v);
        free(v.v);
        break;
    }

    case N_IF:      cg_if(g, n); break;
    case N_WHILE:   cg_while(g, n); break;
    case N_FOR:     cg_for(g, n); break;

    case N_BREAK:
        if (g->nbrk == 0) ERRX(g, "line %zu: 'break' outside a loop", n->line);
        EMIT(g, "  br label %%L%d\n", g->brk[g->nbrk - 1]);
        break;

    case N_CONTINUE:
        if (g->ncnt == 0) ERRX(g, "line %zu: 'continue' outside a loop", n->line);
        EMIT(g, "  br label %%L%d\n", g->cnt[g->ncnt - 1]);
        break;

    case N_EXPR_STMT: {
        Val v = cg_expr(g, n->as.expr_stmt.expr);
        free(v.v);
        break;
    }

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
    Sig    sig;
    memset(&sig, 0, sizeof sig);
    sig.ret = fn->as.func.ret;
    g->cur = &sig;
    sig.name = xstrdup(fn->as.func.name);

    Asgs saved = g->locals;
    memset(&g->locals, 0, sizeof g->locals);

    /* parameters first, so their slots exist before the body scan. A struct
     * parameter is *already* a pointer (`%pN`), so it keeps the parameter and
     * skips the alloca/store dance below. */
    if (params) {
        for (int i = 0; i < arity; i++) {
            char pslot[48];
            snprintf(pslot, sizeof pslot, "%%p%d", i);
            asg_push(&g->locals, fn->as.func.names[i], params[i],
                     params[i] && params[i]->kind == TY_STRUCT ? pslot : NULL);
        }
    }
    scan_block(g, fn->as.func.body);

    const char *rty = llvm_type_of(sig.ret);

    EMIT(g, "\n; --- func %s ---\n", fn->as.func.name);
    EMIT(g, "define ");
    if (rty) EMIT(g, "%s ", rty); else EMIT(g, "void ");
    EMIT(g, "@L_%s(", fn->as.func.name);
    for (int i = 0; i < arity; i++) {
        const char *pt = params && params[i] ? llvm_ptr_type_of(params[i]) : NULL;
        if (!pt) ERRX(g, "line %zu: parameter '%s' has no codegen type",
                     fn->line, fn->as.func.names ? fn->as.func.names[i] : "?");
        EMIT(g, "%s %%p%d%s", pt, i, (i + 1 < arity) ? ", " : "");
    }
    EMIT(g, ") {\n");

    /* entry: one alloca per local, then seed the parameters */
    EMIT(g, "entry:\n");
    for (int i = 0; i < g->locals.n; i++) {
        Asg *a = &g->locals.v[i];
        /* A struct *parameter* already is its slot (it arrived as a pointer);
         * a struct local still needs an alloca. Locals always live in %lv_. */
        if (strncmp(a->slot, "%lv_", 4) != 0) continue;
        const char *lt = llvm_type_of(a->ty);
        if (!lt) ERRX(g, "line %zu: variable '%s' has no codegen type", fn->line, a->name);
        EMIT(g, "  %s = alloca %s\n", a->slot, lt);
    }
    if (params) {
        for (int i = 0; i < arity; i++) {
            if (params[i] && params[i]->kind == TY_STRUCT) continue;
            const char *pt = llvm_type_of(params[i]);
            EMIT(g, "  store %s %%p%d, %s* %s\n", pt, i, pt,
                 asg_find(&g->locals, fn->as.func.names[i])->slot);
        }
    }

    if (fn->as.func.body) cg_block(g, fn->as.func.body);

    /* A basic block must end with a terminator. A struct-returning function
     * produces its aggregate in the body, so `ret %Rect 0` would not even
     * parse — such a body is expected to have returned already.
     *
     * Note the fallback is emitted unconditionally: it may look like dead code
     * right after a `return`, but it is what closes blocks the body opened and
     * never filled — `if (c) { return 1; }` with no else leaves an empty
     * `Lend` block that has no terminator of its own. Do not "simplify" the
     * trailing terminator away by sniffing the tail of the buffer. */
    if (!rty)            EMIT(g, "  ret void\n");
    else if (rty[0] == '%') EMIT(g, "  unreachable\n");
    else                 EMIT(g, "  ret %s 0\n", rty);

    EMIT(g, "}\n");

    asgs_free(&g->locals);
    g->locals = saved;
    free(sig.name);
    g->cur = NULL;
}

/* ------------------------------------------------------------------- entry -- */

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

    Node **top  = prog->as.program.stmts;
    int    ntop = prog->as.program.count;

    /* ---- pass 1: signatures and structs, so calls can be resolved in any order ---- */
    for (int i = 0; i < ntop; i++) {
        Node *n = top[i];
        if (n->type == N_FUNC_DECL)
            sig_push(&cg.sigs, n->as.func.name, n->as.func.ret,
                     n->as.func.param_types, n->as.func.arity);
        else if (n->type == N_TYPE_DECL)
            sdef_push(&cg.structs, n->as.type_decl.name, n->as.type_decl.field_names,
                      n->as.type_decl.field_types, n->as.type_decl.count);
    }

    /* ---- header ---- */
    EMIT(&cg, "; ModuleID = \"%s\"\n", mod_name ? mod_name : "lume");
    EMIT(&cg, "; generated by lume-llvm (Lume -> LLVM IR text -> native)\n");
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

    /* ---- pass 2: one LLVM function per Lume function ---- */
    for (int i = 0; i < ntop; i++)
        if (top[i]->type == N_FUNC_DECL) cg_function(&cg, top[i]);

    /* Anything that failed above leaves a truncated module behind, so refuse
     * to hand out IR the caller would happily write to disk — the driver then
     * reports the real cause instead of a syntax error from clang. */
    if (cg.err[0]) {
        if (err && err_size) snprintf(err, err_size, "%s", cg.err);
        free(cg.gs.data); free(cg.sigs.v); free(cg.structs.v);
        free(cg.brk); free(cg.cnt);
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

    /* ---- entry point: wrap a `main` function as the C entry ---- */
    Sig *m = sig_find(&cg.sigs, "main");
    if (m) {
        if (m->arity != 0)
            ERR(&cg, "main() must take no arguments for the native backend");
        const char *rty = llvm_type_of(m->ret);
        if (!rty) ERR(&cg, "main() must return int for the native backend");
        EMIT(&cg, "\n; --- C entry point ---\n");
        EMIT(&cg, "define i32 @main() {\nentry:\n");
        if (strcmp(rty, "double") == 0)
            EMIT(&cg, "  %%r = call double @L_main()\n  %%z = fptosi double %%r to i32\n  ret i32 %%z\n");
        else
            EMIT(&cg, "  %%r = call %s @L_main()\n  %%z = trunc %s %%r to i32\n  ret i32 %%z\n",
                 rty, rty);
        EMIT(&cg, "}\n");
    }

    /* ---- cleanup ---- */
    free(cg.gs.data);
    free(cg.sigs.v);
    free(cg.structs.v);
    free(cg.brk);
    free(cg.cnt);

    return ir.data;
}
