/* codegen_stmt.c — statements and function bodies.

 * Control flow (while / for / for-in / if) and the function prologue, i.e.
 * the alloca of every local the scanning pass registered. Expressions come
 * from codegen_expr.c.
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
static void cg_block(CG *g, Node *blk);
static void cg_handler_ret(CG *g, Node *n);
static void cg_route_handler(CG *g, Node *fn, int rid);

/* forward: same tu, defined below this point. */
static void cg_for(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static void cg_for_in(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static void cg_if(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static void cg_stmt(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static void cg_while(CG *g, Node *n);

/* forward: same tu, defined below this point. */
Type *elem_rty(Type *ty);

/* forward: same tu, defined below this point. */
const char *list_at_fn(Type *ty);

/* Returns the stored value, because `p.x = e` is a statement *and* an
 * expression: the parser wraps a bare `p.x = e;` in an expr-stmt, and cg_expr
 * has no N_ASSIGN_MEMBER case of its own, so the store has to be spelled from
 * whichever side asks for it. */

Val cg_assign_mem(CG *g, Node *n)
{
    /* The node carries no type of its own — the parser only keeps
     * {obj, name, value} — so the struct spelling comes from the object's own
     * type. Reading n->as.member.type here reads the value node through a
     * Type * and hands garbage to strcmp. */
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
    const char *ft = fty ? llvm_type_of(fty) : NULL;
    if (!ft) ERRV(g, "line %zu: field '%s' has no codegen type",
                  n->line, n->as.assign_mem.name);

    /* The struct spelling needs its `%` sigil: llvm_type_of, not st->name. */
    const char *stn = llvm_type_of(st);
    if (!stn) ERRV(g, "line %zu: unknown field '%s'", n->line, n->as.assign_mem.name);

    /* Two indices: the leading `0` walks the pointer to the struct, the second
     * is the field. Omitting it would turn `1` into an *array* subscript,
     * silently reading offset 16 — the wrong field. */
    char *p = emit_instrf(g, st, "getelementptr inbounds %s, %s* %s, i32 0, i32 %d",
                          stn, stn, obj.v, idx);
    if (!p) { free(obj.v); free(val.v); ERRV(g, "line %zu: cannot access '%s'",
                                             n->line, n->as.assign_mem.name); }
    EMIT(g, "  store %s %s, %s* %s\n", ft, val.v, ft, p);

    free(obj.v); free(p);
    return val;
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

/* Which element accessor reads a list whose variable is typed `ty`.
 * Shared with the map/filter/reduce emitters (declared in codegen_internal.h):
 * one dispatch table, so for-in and the HOFs can never disagree about which
 * element types are readable. */

const char *list_at_fn(Type *ty)
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

/* Which return type that accessor has — it is what the store into the
 * iteration variable's slot has to use. Shared with the HOF emitters, same
 * reason as list_at_fn above. */

Type *elem_rty(Type *ty)
{
    if (!ty) return NULL;
    switch (ty->kind) {
    case TY_FLOAT:  return type_prim(TY_FLOAT);
    case TY_STRING: return type_prim(TY_STRING);
    default:        return type_prim(TY_INT);
    }
}

/* `for (x in xs)` and `for (k in m)`.
 *
 * A loop over a heap object reached through an opaque pointer, so the
 * "iterator" is an index and the object is read through the helper the static
 * element type picked. The iterable is evaluated once before the loop, and the
 * length is captured *once*, before the loop, into a snapshot — that is what
 * the interpreter's for-in does (it reads `list.count` at loop entry). A
 * `push(xs, v)` inside the body therefore does NOT extend the iteration;
 * re-reading the length every iteration used to let the native backends spin
 * forever and OOM (exit 137), which is bug #2 fixed here. */

static void cg_for_in(CG *g, Node *n)
{
    Val it = cg_expr(g, n->as.fors.iterable);
    if (!it.v) ERRX(g, "line %zu: bad iterable", n->line);

    /* A map literal is an anonymous struct, and so is a map value; only a map
     * iterates its keys, so that spelling is the test. */
    bool is_map = it.ty && it.ty->kind == TY_STRUCT && !it.ty->name;

    Asg *a = asg_find(&g->locals, n->as.fors.var, n->line);
    if (!a) { free(it.v); ERRX(g, "line %zu: unknown variable '%s'", n->line, n->as.fors.var); }
    Type *et = a->ty;
    const char *lt = llvm_type_of(et);
    if (!lt) { free(it.v); ERRX(g, "line %zu: variable '%s' has no codegen type",
                                n->line, n->as.fors.var); }
    if (!is_map && !list_at_fn(et)) {
        free(it.v);
        ERRX(g, "line %zu: a list of this element type cannot be iterated", n->line);
    }

    char idx[64];
    snprintf(idx, sizeof idx, "%%li%d", g->tid++);
    /* The index cannot wait for the entry block's alloca pass: the loop is
     * emitted where the source puts it. An alloca is legal anywhere in a
     * function as long as it dominates its uses, and this one is emitted
     * before the loop it serves. */
    EMIT(g, "  %s = alloca i64\n", idx);
    EMIT(g, "  store i64 0, i64* %s\n", idx);

    /* Snapshot the collection length once, before the loop (matches the
     * interpreter's `n_items = list.count` at loop entry). The snapshot is an
     * immutable bound loaded each time the condition is tested; re-reading it
     * every iteration made `push(xs, v)` inside the body grow the bound
     * forever and OOM the native backends. */
    char lenalloc[64];
    snprintf(lenalloc, sizeof lenalloc, "%%li%d", g->tid++);
    EMIT(g, "  %s = alloca i64\n", lenalloc);
    Val len0 = rt_call(g, type_prim(TY_INT), is_map ? "lume_map_len" : "lume_list_len",
                        "i8* %s", it.v);
    if (!len0.v) { free(it.v); ERRX(g, "line %zu: cannot read the collection's length", n->line); }
    EMIT(g, "  store i64 %s, i64* %s\n", len0.v, lenalloc);
    free(len0.v);

    int lc = g->lid, lb = g->lid + 1, li = g->lid + 2, le = g->lid + 3;
    g->lid += 4;

    stack_push_int(&g->brk, &g->nbrk, &g->cbrk, le);
    /* `continue` lands on the *increment* block, not the condition block: a
     * jump back to the condition would skip the index bump and spin forever
     * on the same element. li ends with `br label %lc`, so the re-test
     * happens either way. */
    stack_push_int(&g->cnt, &g->ncnt, &g->ccnt, li);

    Type *boolty = type_prim(TY_BOOL);

    EMIT(g, "  br label %%L%d\n", lc);

    EMIT(g, "L%d:\n", lc);
    /* Load the loop-bound snapshot once per iteration; it is immutable, so a
     * `push(xs, v)` inside the body cannot extend the iteration. */
    char *len = emit_instrf(g, type_prim(TY_INT), "load i64, i64* %s", lenalloc);
    if (!len) { free(it.v); ERRX(g, "line %zu: cannot load the loop length", n->line); }
    char *cur = emit_instrf(g, type_prim(TY_INT), "load i64, i64* %s", idx);
    if (!cur) { free(it.v); free(len); ERRX(g, "line %zu: cannot read the loop index", n->line); }
    char *c = emit_instrf(g, boolty, "icmp slt i64 %s, %s", cur, len);
    free(len);
    if (!c) { free(it.v); free(cur); ERRX(g, "line %zu: bad 'for ... in' condition", n->line); }
    EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n", c, lb, le);
    free(c);

    EMIT(g, "L%d:\n", lb);
    Val e = is_map
        ? rt_call(g, type_prim(TY_STRING), "lume_map_key_at", "i8* %s, i64 %s", it.v, cur)
        : rt_call(g, elem_rty(et), list_at_fn(et), "i8* %s, i64 %s", it.v, cur);
    if (!e.v) { free(it.v); free(cur); ERRX(g, "line %zu: cannot read an element", n->line); }
    /* A bool iteration variable gets its slot narrowed here, so the store and
     * the later `load` both speak i1 and `print(x)` still says true/false. */
    if (et->kind == TY_BOOL) {
        char *b = emit_instrf(g, boolty, "icmp ne i64 %s, 0", e.v);
        free(e.v);
        if (!b) { free(it.v); free(cur); ERRX(g, "line %zu: cannot narrow a bool", n->line); }
        e.v = b;
    }
    EMIT(g, "  store %s %s, %s* %s\n", lt, e.v, lt, a->slot);
    free(e.v);

    cg_stmt(g, n->as.fors.body);
    EMIT(g, "  br label %%L%d\n", li);

    EMIT(g, "L%d:\n", li);
    char *nxt = emit_instrf(g, type_prim(TY_INT), "add i64 %s, 1", cur);
    if (nxt) {
        EMIT(g, "  store i64 %s, i64* %s\n", nxt, idx);
        free(nxt);
    }
    EMIT(g, "  br label %%L%d\n", lc);

    EMIT(g, "L%d:\n", le);
    g->nbrk--; g->ncnt--;
    free(it.v); free(cur);   /* `idx` is a stack buffer, not owned */
}

static void cg_for(CG *g, Node *n)
{
    if (n->as.fors.is_in) { cg_for_in(g, n); return; }

    if (n->as.fors.init) cg_stmt(g, n->as.fors.init);

    int lc = g->lid, lb = g->lid + 1, li = g->lid + 2, le = g->lid + 3;
    g->lid += 4;

    stack_push_int(&g->brk, &g->nbrk, &g->cbrk, le);
    /* The same rule as the `for ... in` loop: `continue` must reach the
     * increment block, otherwise `i = i + 1` never runs and the loop spins
     * on the same element forever. */
    stack_push_int(&g->cnt, &g->ncnt, &g->ccnt, li);

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
        if (g->err[0]) { free(v.v); break; }  /* coerce refused the conversion */
        const char *lt = llvm_type_of(ty);
        if (!lt) ERRX(g, "line %zu: cannot store '%s' into a typed local",
                     n->line, n->as.let.name);
        Asg *a = asg_find(&g->locals, n->as.let.name, n->line);
        if (!a) { free(v.v); ERRX(g, "line %zu: unknown variable '%s'", n->line, n->as.let.name); }
        EMIT(g, "  store %s %s, %s* %s\n", lt, v.v, lt, a->slot);
        free(v.v);
        break;
    }

    case N_ASSIGN:
        free(cg_assign_expr(g, n).v);
        break;

    case N_ASSIGN_MEMBER:
        (void)cg_assign_mem(g, n);
        break;

    case N_RETURN:
        if (g->in_handler) { cg_handler_ret(g, n); break; }
        {
        if (!n->as.ret.expr) { EMIT(g, "  ret void\n"); break; }
        /* `return { .. }` needs the function's declared return type. */
        Type *prev = g->expect;
        g->expect = g->cur ? g->cur->ret : NULL;
        Val v = cg_expr(g, n->as.ret.expr);
        g->expect = prev;
        const char *rty = g->cur ? llvm_type_of(g->cur->ret) : NULL;
        if (!rty) rty = llvm_type_of(v.ty);
        if (!rty) ERRX(g, "line %zu: cannot return this value", n->line);
        /* An int in a double slot is not IR (`ret double 1` reads as an
         * integer constant, `ret double %t0` names an i64), so it is widened
         * here. coerce materialises the sitofp as its own instruction: writing
         * `ret double sitofp i64 %t0 to double` is rejected outright — LLVM
         * dropped constexpr operands. */
        {
            Type *want = g->cur ? g->cur->ret : NULL;
            if (want && v.ty && v.ty->kind == TY_INT && want->kind == TY_FLOAT)
                v = coerce(g, want, v, n->line);
            if (g->err[0]) { free(v.v); break; }
        }
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

    case N_SERVER: {
        /* server { host = ".."; port = N; } -> lume_srv_init */
        const char *host = "127.0.0.1";
        long long port = 8082;
        char hostbuf[256];
        for (int i = 0; i < n->as.server.count; i++) {
            Node *a = n->as.server.assigns[i];
            if (!a || a->type != N_ASSIGN) continue;
            Node *v = a->as.assign.value;
            if (strcmp(a->as.assign.name, "host") == 0 && v &&
                v->type == N_LITERAL && v->as.lit.kind == LIT_STR) {
                const char *t = v->as.lit.text;
                size_t l = (size_t)v->as.lit.len;
                if (l >= 2 && t[0] == '"' && t[l - 1] == '"') { t++; l -= 2; }
                if (l >= sizeof hostbuf) l = sizeof hostbuf - 1;
                memcpy(hostbuf, t, l);
                hostbuf[l] = '\0';
                host = hostbuf;
            } else if (strcmp(a->as.assign.name, "port") == 0 && v &&
                       v->type == N_LITERAL && v->as.lit.kind == LIT_NUM &&
                       !v->as.lit.is_float) {
                port = v->as.lit.inum;
            }
        }
        char *hc = cg_string_val(g, host, strlen(host));
        EMIT(g, "  call void @lume_srv_init(i8* %s, i64 %lld)\n", hc, port);
        free(hc);
        break;
    }

    case N_ROUTE: {
        if (n->as.route.alias)
            ERRX(g, "line %zu: verbs alias routes are not supported by the "
                    "native backend yet", n->line);
        Node *h = n->as.route.handler;
        if (!h || h->type != N_FUNC_LIT)
            ERRX(g, "line %zu: native server: route handler must be a "
                    "function literal", n->line);
        int rid = g->hrid++;
        cg_route_handler(g, h, rid);
        char *mc = cg_string_val(g, n->as.route.method,
                                 strlen(n->as.route.method));
        char *pc = cg_string_val(g, n->as.route.path,
                                 strlen(n->as.route.path));
        EMIT(g, "  call void @lume_srv_route(i8* %s, i8* %s, void "
                "(%%struct.SrvReq*, i8**, i64*, i32*)* @L_route_%d)\n",
             mc, pc, rid);
        free(mc); free(pc);
        break;
    }

    default:
        ERRX(g, "%s", bad(g, n, "this construct"));
    }
}

static void cg_block(CG *g, Node *blk)
{
    if (!blk) return;
    for (int i = 0; i < blk->as.block.count; i++) cg_stmt(g, blk->as.block.stmts[i]);
}

/* --- native server: route handler FFI compilation --------------------------
 *
 * A handler `(req) => { ... }` compiles to
 *
 *   define void @L_route_N(%%struct.SrvReq* %p0, i8** %p1, i64* %p2, i32* %p3)
 *
 * %p0 is the request: req.* member access lowers onto it in codegen_expr.c.
 * %p1/%p2 receive the response buffer and its length; %p3 receives the
 * content-type flag (0 = text/html, 1 = application/json). The handler body
 * runs with g->in_handler set, so N_RETURN serialises instead of returning a
 * value. */

static void cg_route_handler(CG *g, Node *fn, int rid)
{
    if (fn->as.funclit.arity != 1)
        ERRX(g, "line %zu: native server: a route handler must take exactly "
                "one (req) argument", fn->line);

    Sig sig;
    memset(&sig, 0, sizeof sig);
    char fname[48];
    snprintf(fname, sizeof fname, "route_%d", rid);
    sig.name = xstrdup(fname);
    sig.ret = NULL;
    g->cur = &sig;

    Asgs saved = g->locals;
    memset(&g->locals, 0, sizeof g->locals);

    g->in_handler = 1;
    g->hreq = fn->as.funclit.names ? fn->as.funclit.names[0] : "req";
    scan_block(g, fn->as.funclit.body);

    /* Emit into the handlers buffer: a `define` is only legal at module
     * top level, so handler bodies wait until L_top has closed. */
    IrBuf *saved_ir = g->ir;
    g->ir = &g->handlers;

    EMIT(g, "\n; --- route handler %s ---\n", fname);
    EMIT(g, "define void @L_%s(%%struct.SrvReq* %%p0, i8** %%p1, i64* %%p2, "
            "i32* %%p3) {\n", fname);
    EMIT(g, "entry:\n");
    for (int i = 0; i < g->locals.n; i++) {
        Asg *a = &g->locals.v[i];
        if (strncmp(a->slot, "%lv_", 4) != 0) continue;
        const char *lt = llvm_type_of(a->ty);
        if (!lt)
            ERRX(g, "line %zu: variable '%s' has no codegen type",
                 fn->line, a->name);
        EMIT(g, "  %s = alloca %s\n", a->slot, lt);
    }

    if (fn->as.funclit.body) cg_block(g, fn->as.funclit.body);
    g->in_handler = 0;
    g->hreq = NULL;

    EMIT(g, "  ret void\n}\n");

    g->ir = saved_ir;
    asgs_free(&g->locals);
    g->locals = saved;
    free(sig.name);
    g->cur = NULL;
}

/* return in a native handler: string -> plain text, map literal -> JSON. */
static void cg_handler_ret(CG *g, Node *n)
{
    Node *e = n->as.ret.expr;
    if (!e) { EMIT(g, "  ret void\n"); return; }

    if (e->type == N_MAP_LIT) {
        EMIT(g, "  call void @lume_srv_json_begin(i8** %%p1, i64* %%p2)\n");
        for (int i = 0; i < e->as.map.count; i++) {
            char *kc = cg_string_val(g, e->as.map.keys[i],
                                     strlen(e->as.map.keys[i]));
            EMIT(g, "  call void @lume_srv_json_key(i8** %%p1, i64* %%p2, "
                    "i8* %s)\n", kc);
            free(kc);

            Node *v = e->as.map.vals[i];
            if (v->type == N_LITERAL && v->as.lit.kind == LIT_STR) {
                const char *vt = v->as.lit.text;
                size_t vl = (size_t)v->as.lit.len;
                if (vl >= 2 && vt[0] == '"' && vt[vl - 1] == '"') { vt++; vl -= 2; }
                char *vc = cg_string_val(g, vt, vl);
                EMIT(g, "  call void @lume_srv_json_str(i8** %%p1, i64* "
                        "%%p2, i8* %s)\n", vc);
                free(vc);
            } else if (v->type == N_LITERAL &&
                       (v->as.lit.kind == LIT_TRUE ||
                        v->as.lit.kind == LIT_FALSE)) {
                EMIT(g, "  call void @lume_srv_json_bool(i8** %%p1, i64* "
                        "%%p2, %s)\n",
                     v->as.lit.kind == LIT_TRUE ? "1" : "0");
            } else if (v->type == N_LITERAL && v->as.lit.kind == LIT_NUM &&
                       !v->as.lit.is_float) {
                EMIT(g, "  call void @lume_srv_json_int(i8** %%p1, i64* "
                        "%%p2, i64 %lld)\n", v->as.lit.inum);
            } else {
                /* generic value: evaluate, then dispatch on its type */
                Val vv = cg_expr(g, v);
                if (!vv.v) { free(vv.v);
                    ERRX(g, "line %zu: cannot serialise map value", v->line); }
                if (vv.ty && vv.ty->kind == TY_INT) {
                    EMIT(g, "  call void @lume_srv_json_int(i8** %%p1, i64* "
                            "%%p2, i64 %s)\n", vv.v);
                } else if (vv.ty && vv.ty->kind == TY_STRING) {
                    EMIT(g, "  call void @lume_srv_json_str(i8** %%p1, i64* "
                            "%%p2, i8* %s)\n", vv.v);
                } else if (vv.ty && vv.ty->kind == TY_BOOL) {
                    char *b = emit_instrf(g, type_prim(TY_INT),
                                          "zext i1 %s to i32", vv.v);
                    EMIT(g, "  call void @lume_srv_json_bool(i8** %%p1, i64* "
                            "%%p2, i32 %s)\n", b);
                    free(b);
                } else {
                    free(vv.v);
                    ERRX(g, "line %zu: native server: map value must be int, "
                            "string or bool", v->line);
                }
                free(vv.v);
            }
        }
        EMIT(g, "  call void @lume_srv_json_end(i8** %%p1, i64* %%p2)\n");
        EMIT(g, "  store i32 1, i32* %%p3\n");
        EMIT(g, "  ret void\n");
        return;
    }

    /* String-valued expression -> plain text. The length is unknown at
     * compile time (concatenations, str(), ...), so out_len = -1 asks the
     * runtime to strlen the buffer. */
    {
        Val sv = cg_expr(g, e);
        if (!sv.v) { free(sv.v);
            ERRX(g, "line %zu: native server: cannot evaluate handler return",
                 n->line); }
        if (sv.ty && sv.ty->kind == TY_STRING) {
            EMIT(g, "  store i8* %s, i8** %%p1\n", sv.v);
            EMIT(g, "  store i64 -1, i64* %%p2\n");
            EMIT(g, "  store i32 0, i32* %%p3\n");
            free(sv.v);
            EMIT(g, "  ret void\n");
            return;
        }
        free(sv.v);
    }

    ERRX(g, "line %zu: native server: handler must return a string or a map "
            "literal", n->line);
}

/* The one shape every text-backend `define` takes. `sym` is the LLVM symbol
 * (L_<name> for a named function, the mangled __lume_clo_N for a closure);
 * `has_cap` prepends the closure-context parameter a lambda's define carries
 * and a direct call site passes i8* null (or the loaded .cap) into.
 *
 * Everything else — parameter slots, the entry-block alloca pass, the
 * terminator fallback — is byte-for-byte what cg_function always did, because
 * a closure body is a function body and there is no second emitter to keep in
 * step. */

static void cg_def(CG *g, const char *sym, const char *title,
                   char **names, Type **params, int arity, Type *ret,
                   Node *body, int has_cap, size_t line)
{
    Sig sig;
    memset(&sig, 0, sizeof sig);
    sig.ret = ret;
    g->cur = &sig;
    sig.name = xstrdup(title);

    Asgs saved = g->locals;
    memset(&g->locals, 0, sizeof g->locals);

    /* A define is its own function: a `break` inside it can never target a
     * loop in whoever is being emitted around it (no loop can span a define
     * boundary in this backend, so the stacks are always empty here anyway —
     * the reset is what keeps that invariant load-bearing instead of lucky).
     * With an empty stack the N_BREAK emitter reports "'break' outside a
     * loop" instead of branching into another function. */
    int *saved_brk = g->brk; int snbrk = g->nbrk, scbrk = g->cbrk;
    int *saved_cnt = g->cnt; int sncnt = g->ncnt, sccnt = g->ccnt;
    g->brk = g->cnt = NULL; g->nbrk = g->ncnt = g->cbrk = g->ccnt = 0;

    /* parameters first, so their slots exist before the body scan. A struct
     * parameter is *already* a pointer (`%pN`), so it keeps the parameter and
     * skips the alloca/store dance below. */
    if (params) {
        for (int i = 0; i < arity; i++) {
            char pslot[48];
            snprintf(pslot, sizeof pslot, "%%p%d", i);
            asg_push(&g->locals, names[i], params[i],
                     params[i] && params[i]->kind == TY_STRUCT ? pslot : NULL,
                     0);
        }
    }
    scan_block(g, body);

    const char *rty = llvm_type_of(ret);

    EMIT(g, "\n; --- %s ---\n", title);
    EMIT(g, "define ");
    if (rty) EMIT(g, "%s ", rty); else EMIT(g, "void ");
    EMIT(g, "@%s(", sym);
    if (has_cap) EMIT(g, "i8* %%cap");
    for (int i = 0; i < arity; i++) {
        const char *pt = params && params[i] ? llvm_ptr_type_of(params[i]) : NULL;
        if (!pt) ERRX(g, "line %zu: parameter '%s' has no codegen type",
                      line, names ? names[i] : "?");
        EMIT(g, "%s%s %%p%d", (has_cap || i) ? ", " : "", pt, i);
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
        if (!lt) ERRX(g, "line %zu: variable '%s' has no codegen type", line, a->name);
        EMIT(g, "  %s = alloca %s\n", a->slot, lt);
    }
    if (params) {
        for (int i = 0; i < arity; i++) {
            if (params[i] && params[i]->kind == TY_STRUCT) continue;
            const char *pt = llvm_type_of(params[i]);
            EMIT(g, "  store %s %%p%d, %s* %s\n", pt, i, pt,
                 asg_find(&g->locals, names[i], 0)->slot);
        }
    }

    if (body) cg_block(g, body);

    /* A basic block must end with a terminator. A struct-returning function
     * produces its aggregate in the body, so `ret %Rect 0` would not even
     * parse — such a body is expected to have returned already.
     *
     * Note the fallback is emitted unconditionally: it may look like dead code
     * right after a `return`, but it is what closes blocks the body opened and
     * never filled — `if (c) { return 1; }` with no else leaves an empty
     * `Lend` block that has no terminator of its own. Do not "simplify" the
     * trailing terminator away by sniffing the tail of the buffer. */
    /* The synthetic top body is the one that forwards `main()`'s result — see
     * capture_main below. */
    if (g->exit_tmp[0])  EMIT(g, "  ret %s %s\n", rty ? rty : "i64", g->exit_tmp);
    else if (!rty)       EMIT(g, "  ret void\n");
    else if (rty[0] == '%') EMIT(g, "  unreachable\n");
    /* A floating-point literal needs a decimal point: `ret double 0` is not
     * an integer constant and clang rejects it. Only `double` is affected —
     * every other scalar type the emitter can spell is an integer.
     *
     * Same story for a pointer: `ret i8* 0` is not a pointer constant, LLVM
     * spells the null pointer `null`. A Result-returning function is the first
     * thing to reach this line with a pointer result type (every `if` on the
     * way out of the body can leave it falling through), and the illegal IR
     * surfaced as `integer constant must have integer type` from clang. */
    else if (strncmp(rty, "double", 6) == 0) EMIT(g, "  ret double 0.0\n");
    else if (rty[strlen(rty) - 1] == '*') EMIT(g, "  ret %s null\n", rty);
    else                 EMIT(g, "  ret %s 0\n", rty);

    EMIT(g, "}\n");

    free(g->brk); free(g->cnt);
    g->brk = saved_brk; g->nbrk = snbrk; g->cbrk = scbrk;
    g->cnt = saved_cnt; g->ncnt = sncnt; g->ccnt = sccnt;

    asgs_free(&g->locals);
    g->locals = saved;
    free(sig.name);
    g->cur = NULL;
}

void cg_function(CG *g, Node *fn)
{
    char sym[256];
    snprintf(sym, sizeof sym, "L_%s", fn->as.func.name);
    cg_def(g, sym, fn->as.func.name, fn->as.func.names,
           fn->as.func.param_types, fn->as.func.arity, fn->as.func.ret,
           fn->as.func.body, 0, fn->line);
}

/* A closure's `define`: the lambda node the signature pass named, emitted with
 * the leading capture parameter its call sites always pass. The body runs
 * through the same cg_def as a named function — the capture context is i8*
 * null in this phase (no free-variable capture yet), so nothing in the body
 * can reach it, but the parameter is part of the calling convention the call
 * sites and the fn-pointer bitcasts are spelled against. */

void cg_closure(CG *g, Node *fn)
{
    if (!fn->as.funclit.cname)
        ERRX(g, "line %zu: internal: closure was never given a mangled name",
             fn->line);
    cg_def(g, fn->as.funclit.cname, fn->as.funclit.cname,
           fn->as.funclit.names, fn->as.funclit.param_types,
           fn->as.funclit.arity, fn->as.funclit.ret,
           fn->as.funclit.body, 1, fn->line);
}
