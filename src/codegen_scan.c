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

/* forward: same tu, defined below this point. */
static Type *for_in_elem_type(CG *g, Node *n);

/* forward: same tu, defined below this point. */
static void scan_stmt(CG *g, Node *n);

/* The AST records an explicit `: T` annotation on `let` nodes; a type inferred
 * from the initializer is never stored on the node. Without this the entry
 * block would have nothing to `alloca` — and the alloca pass bails out, which
 * silently truncates the rest of the function body. */

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
    case N_VAR: {
        Asg *a = asg_find(&g->locals, n->as.var.name, n->line);
        if (a) return a->ty;
        /* A top-level binding lifts to a module global (SPEC 8.1 #10); its
         * type was fixed by the driver's pre-scan. */
        a = gvar_find(&g->gvars, n->as.var.name);
        if (a) return a->ty;
        /* The signature pass runs before any slot is allocated, so a name it
         * already typed is the only other source. */
        return lt_find(&g->scope, n->as.var.name);
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
        if (!n->as.call.callee || n->as.call.callee->type != N_VAR) return NULL;
        const char *name = n->as.call.callee->as.var.name;

        /* Closure-valued variable call: `f(3)` where f : func. The variable's
         * type carries a back-pointer to the lambda node. */
        {
            Type *ct = infer_node_type(g, n->as.call.callee);
            if (ct && ct->kind == TY_FUNC && ct->func &&
                ct->func->type == N_FUNC_LIT)
                return ct->func->as.funclit.ret;
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
            return type_list(any_type());
        }
        if (strcmp(name, "filter") == 0) {
            Type *lt = infer_node_type(g, n->as.call.args[1]);
            if (lt && lt->kind == TY_LIST) return lt;
            return type_list(any_type());
        }
        if (strcmp(name, "reduce") == 0)
            return infer_node_type(g, n->as.call.args[2]);

        /* Native server handler: int() lowers to atoi -> int. */
        if (g->in_handler && strcmp(name, "int") == 0)
            return type_prim(TY_INT);
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
    case N_MAP_LIT: return type_anon_struct();
    case N_MEMBER:  return member_field_type(g, n);
    case N_LIST_LIT: {
        /* The element type of a list literal is read off the literal itself:
         * a value's own type is only ever spelled `i8*`, so without this a
         * `for (x in xs)` would see list<any> — and `any` has no spelling, so
         * the iteration variable would be rejected as untypable. */
        Type *et = infer_list_elem(n);
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

    case N_FOR: {
        /* A for-in variable is a *binding*, not a `let`, so nothing registers
         * it and the entry-block alloca pass would silently skip it — every
         * use of the variable then dies as an unknown variable. It binds in
         * the enclosing scope (the interpreter's for-in assigns through to an
         * existing name and leaves it bound after the loop), so it takes the
         * for statement's own line and no block of its own. At the top level
         * the enclosing scope is the module: the variable is lifted exactly
         * like a `let`, because a function the top level calls after the loop
         * must see the rebound value. */
        if (n->as.fors.is_in) {
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
                asg_push(&g->locals, n->as.assign.name, ty ? ty : gv->ty,
                         NULL, n->line);
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
    case N_WHILE:  scan_stmt(g, n->as.whiles.body); break;
    default: break;
    }
}

void scan_block(CG *g, Node *blk)
{
    if (!blk) return;
    for (int i = 0; i < blk->as.block.count; i++) scan_stmt(g, blk->as.block.stmts[i]);
}
