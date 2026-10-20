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
 *
 * File layout: the backend-private types (CG, Val, the Asg/Sig/SDef tables),
 * the error macros and every static prototype live in llvm_codegen_internal.h;
 * this file keeps the utilities and the two entry points and then textually
 * includes the section fragments llvm_cg_expr / llvm_cg_lit / llvm_cg_call /
 * llvm_cg_stmt / llvm_cg_scan .inc. Everything stays static and in ONE
 * translation unit on purpose: the text emitter (codegen*.c) defines the
 * same cg_* names as non-static symbols in the same binary, so making these
 * external linkage would collide. The .inc files are not compiled on their
 * own, and a change in any of them rebuilds this object (see the Makefile).
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

#include "llvm_codegen_internal.h"
#include "arena.h"

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

static void asgs_free(Asgs *a)
{
    /* name strings are arena memory now (TODO.md P2 Stage 3): only the
     * malloc'd vector shell dies here. */
    free(a->v);
    memset(a, 0, sizeof *a);
}

/* LLVM twin of the text backend's asg_truncate (codegen.c): scope exits that
 * reset g->locals.n to a saved value. This used to free the discarded
 * entries' xstrdup'd names (the bare count reset leaked them); the names are
 * arena memory now, so the reset is a bare count assignment and the arena
 * reclaims everything at end-of-emission. slot is an LLVMValueRef owned by
 * LLVM's context, never freed here. */
static void asg_truncate(Asgs *a, int n)
{
    if (n < 0) n = 0;
    a->n = n;
}

/* slot == NULL asks for a fresh local slot. */
static void asg_push(Asgs *a, const char *name, Type *ty, LLVMValueRef slot,
                     size_t line)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->v = (Asg *)xrealloc(a->v, (size_t)a->cap * sizeof *a->v);
    }
    a->v[a->n].name = arena_strdup(name);
    a->v[a->n].ty   = ty;
    a->v[a->n].slot = slot;
    a->v[a->n].cap  = 0;
    a->v[a->n].shadow = 0;
    /* Declaration line — see codegen.c's asg_push. The flat pre-scanned
     * table can hold several same-name bindings (loop variable + a later
     * `let` of a different type); lookup is position-aware so a use site
     * only sees declarations at or above its own line, matching the
     * interpreter's sequential set-or-define rebinding. */
    a->v[a->n].line = line;
    a->n++;
}

/* A phase-2 capture binding: same shape, but flagged so cg_def materializes
 * its slot from the closure context (and the alloca pass leaves it alone).
 * The text backend spells this with a %cap<i>-prefixed slot string; here
 * the flag replaces the string convention. */
static void asg_push_cap(Asgs *a, const char *name, Type *ty, LLVMValueRef slot,
                         size_t line)
{
    asg_push(a, name, ty, slot, line);
    a->v[a->n - 1].cap = 1;
}

static void asg_push_shadow(Asgs *a, const char *name, Type *ty, LLVMValueRef slot,
                            size_t line)
{
    asg_push(a, name, ty, slot, line);
    a->v[a->n - 1].shadow = 1;
}

static Asg *asg_find(Asgs *a, const char *name, size_t use_line)
{
    for (int i = a->n - 1; i >= 0; i--)
        if (strcmp(a->v[i].name, name) == 0 && a->v[i].line <= use_line)
            return &a->v[i];
    return NULL;
}

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
    s->v[s->n].pnames = NULL;
    s->v[s->n].arity  = arity;
    s->v[s->n].body   = NULL;
    s->v[s->n].node   = NULL;
    /* The vector grows by realloc, which does not zero the new tail — every
     * field added to Sig must be set HERE (same rule as codegen.c's copy:
     * a named function's write-back must never wander into the lambda
     * branch and spell its types through the funclit union offsets). */
    s->v[s->n].is_lambda = 0;
    s->v[s->n].phmask    = 0;
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
    /* An `any` value (a for-in loop variable over list<any>) is a boxed
     * LumeSlot* travelled as an opaque pointer. */
    case TY_ANY:    return g->i8ptr;
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

static Val val_make(Type *ty, LLVMValueRef v)
{
    Val r;
    r.ty = ty;
    r.v  = v;
    return r;
}

void llvm_codegen_init_targets(void)
{
    static int done = 0;
    if (done) return;
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    done = 1;
}

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
    case N_LET_PAT:    collect_lambdas(n->as.letpat.init, out, cnt, cap); return;
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
static LLVMModuleRef llvm_codegen_module_core(const char *mod_name, Node **mods,
                                              const char **nsnames, int nmods,
                                              LLVMContextRef *out_ctx,
                                              char *err, size_t err_size)
{
    char *msg = NULL;
    if (err && err_size) err[0] = '\0';
    if (!mods || nmods <= 0 || !mods[0] || mods[0]->type != N_PROGRAM) {
        if (err && err_size) snprintf(err, err_size, "internal: not a program node");
        return NULL;
    }
    Node *prog = mods[0];

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

    /* Local functions first, in every file: stamp cnames and rewrite call
     * sites before any signature table is built (idempotent — the driver's
     * inference pass and the checker already ran this; this is the
     * last-resort guard). (Each file's cname counter starts at zero, so a
     * module that nests local functions inside module functions can collide
     * with the entry's — a known boundary, the pinned fixtures have none.) */
    for (int m = 0; m < nmods; m++) {
        Node *mp = mods[m];
        if (!mp || mp->type != N_PROGRAM) continue;
        if (!lume_resolve_localfns(mp, err, err_size)) {
            LLVMDisposeBuilder(cg.ab);
            LLVMDisposeModule(cg.mod);
            LLVMContextDispose(ctx);
            return NULL;
        }
    }

    /* ---- pass 1: signatures and declared shapes, so calls resolve in any order ----
     * Top-level and hoisted local functions alike, locals under their cname.
     * Module functions ride under "<ns>.<name>" for member-call resolution;
     * they are defined below (cg_function) under their own source-name
     * symbols, which is what a member call emits against. */
    for (int m = 0; m < nmods; m++) {
        Node *mp = mods[m];
        if (!mp || mp->type != N_PROGRAM) continue;
        int nfn = lume_collect_funcs(mp, NULL, 0);
        Node **allfns = nfn > 0 ? (Node **)xmalloc(sizeof(Node *) * (size_t)nfn) : NULL;
        if (nfn > 0) lume_collect_funcs(mp, allfns, nfn);
        for (int i = 0; i < nfn; i++) {
            Node *n = allfns[i];
            const char *fname = n->as.func.cname ? n->as.func.cname
                                                 : n->as.func.name;
            char qbuf[512];
            const char *qname = fname;
            if (m > 0 && nsnames && nsnames[m]) {
                snprintf(qbuf, sizeof qbuf, "%s.%s", nsnames[m], fname);
                qname = qbuf;
            }
            sig_push(&cg.sigs, qname,
                     n->as.func.ret, n->as.func.param_types, n->as.func.arity);
            /* The body and the parameter names are what let a call site (and
             * the member-call emitter) recover the function's own symbol name
             * and signature — a Sig without a node cannot name its callee. */
            cg.sigs.v[cg.sigs.n - 1].body   = n->as.func.body;
            cg.sigs.v[cg.sigs.n - 1].pnames = n->as.func.names;
            cg.sigs.v[cg.sigs.n - 1].node   = n;
        }
        free(allfns);
    }
    for (int m = 0; m < nmods; m++) {
        Node *mp = mods[m];
        if (!mp || mp->type != N_PROGRAM) continue;
        Node **mt = mp->as.program.stmts;
        for (int i = 0; i < mp->as.program.count; i++) {
            Node *n = mt[i];
            if (n->type == N_TYPE_DECL)
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
     * takes. Imported module top-levels run first (the loader executes them
     * in dependency order), so a module function body reads its module's
     * tables through the same globals the entry uses. */
    int nfns = 0, ntops = 0;
    for (int m = 0; m < nmods; m++) {
        Node *mp = mods[m];
        if (!mp || mp->type != N_PROGRAM) continue;
        nfns += lume_collect_funcs(mp, NULL, 0);
        for (int i = 0; i < mp->as.program.count; i++) {
            Node *s = mp->as.program.stmts[i];
            if (s->type == N_TYPE_DECL || s->type == N_IMPORT ||
                s->type == N_ENUM_DECL || s->type == N_FUNC_DECL)
                continue;   /* emitted above / resolved by the loader / collected below */
            else ntops++;
        }
    }
    /* Every function gets a define: top-level ones and hoisted locals alike. */
    Node **fns  = nfns  > 0 ? (Node **)xmalloc(sizeof(Node *) * (size_t)nfns)  : NULL;
    if (nfns > 0) {
        int fi = 0;
        for (int m = 0; m < nmods; m++) {
            Node *mp = mods[m];
            if (!mp || mp->type != N_PROGRAM) continue;
            int mn = lume_collect_funcs(mp, NULL, 0);
            if (mn > 0) lume_collect_funcs(mp, fns + fi, mn);
            fi += mn;
        }
    }
    Node **tops = ntops > 0 ? (Node **)xmalloc(sizeof(Node *) * (size_t)ntops) : NULL;
    /* Does the top level actually call `main`? Only then is main() a thing the
     * process can take its status from — a main nobody runs is an ordinary
     * function and the status stays 0. Same question the text backend asks, so
     * the two backends report the same exit code for the same source. */
    int tops_main = 0;
    {
        int m = 0;
        for (int mm = 1; mm < nmods; mm++) {
            Node *mp = mods[mm];
            if (!mp || mp->type != N_PROGRAM) continue;
            Node **mt = mp->as.program.stmts;
            for (int i = 0; i < mp->as.program.count; i++) {
                Node *n = mt[i];
                if (n->type == N_FUNC_DECL)      continue;   /* collected above */
                else if (n->type == N_TYPE_DECL) continue;   /* emitted above */
                else if (n->type == N_ENUM_DECL) continue;   /* no runtime presence */
                else if (n->type == N_IMPORT)    continue;   /* the loader resolved it */
                else tops[m++] = n;
            }
        }
        for (int i = 0; i < ntop; i++) {
            Node *n = top[i];
            if (n->type == N_FUNC_DECL)      continue;   /* collected above */
            else if (n->type == N_TYPE_DECL) continue;   /* emitted above */
            else if (n->type == N_ENUM_DECL) continue;   /* no runtime presence */
            else if (n->type == N_IMPORT)    continue;   /* the loader resolved it */
            else {
                tops[m++] = n;
                Node *e = n->type == N_EXPR_STMT ? n->as.expr_stmt.expr : NULL;
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
        sigs_free(&cg.sigs); sdefs_free(&cg.structs); free(cg.brk); free(cg.cnt);
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

LLVMModuleRef llvm_codegen_module(const char *mod_name, Node *prog,
                                  LLVMContextRef *out_ctx, char *err,
                                  size_t err_size)
{
    Node *mods[1];
    mods[0] = prog;
    return llvm_codegen_module_core(mod_name, mods, NULL, 1,
                                    out_ctx, err, err_size);
}

LLVMModuleRef llvm_codegen_module_multi(const char *mod_name, Node **mods,
                                        const char **nsnames, int nmods,
                                        LLVMContextRef *out_ctx, char *err,
                                        size_t err_size)
{
    return llvm_codegen_module_core(mod_name, mods, nsnames, nmods,
                                    out_ctx, err, err_size);
}

#include "llvm_cg_expr.inc"
#include "llvm_cg_lit.inc"
#include "llvm_cg_call.inc"
#include "llvm_cg_stmt.inc"
#include "llvm_cg_scan.inc"
