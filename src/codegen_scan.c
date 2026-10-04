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
        default:        return NULL;    /* null: no codegen type */
        }
    case N_VAR: {
        Asg *a = asg_find(&g->locals, n->as.var.name);
        if (a) return a->ty;
        /* The signature pass runs before any slot is allocated, so a name it
         * already typed is the only other source. */
        return lt_find(&g->scope, n->as.var.name);
    }
    case N_CALL: {
        /* `let b = f(...)` takes the return type of f — struct returns included,
         * which is how `let b = scale(a, 5)` gets its %Rect slot. */
        if (!n->as.call.callee || n->as.call.callee->type != N_VAR) return NULL;
        Sig *s = sig_find(&g->sigs, n->as.call.callee->as.var.name);
        return s ? s->ret : NULL;
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
        /* A for-in variable is a *binding*, not a `let`, so nothing registers
         * it and the entry-block alloca pass would silently skip it — every
         * use of the variable then dies as an unknown variable. */
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
