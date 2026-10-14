/* codegen_lit.c - literal and struct-construction emitters.
 *
 * Cut out of codegen_expr.c along its own section boundary: the list / map /
 * struct literals, struct update, the enum constructor, and the static-type
 * dispatch tables that pick a runtime helper (rt_arg_type, map_get_fn,
 * list_push_fn, map_put_fn, list_type). The tables are shared with the
 * builtin emitters (codegen_call.c) and the dispatcher (cg_expr), so they
 * are declared in codegen_internal.h rather than private to this file.
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

Val cg_ctor(CG *g, Node *n);
Val cg_list_lit(CG *g, Node *n);
static Val cg_map_lit(CG *g, Node *n);
Val cg_map_or_struct_lit(CG *g, Node *n);
static Val cg_struct_lit(CG *g, Node *n);
const char *list_push_fn(Type *ty);
Type *list_type(void);
const char *map_put_fn(Type *ty);
static Type *resolve_struct_lit(CG *g, Node *n);
const char *rt_arg_type(Type *ty);

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

Type *member_field_type(CG *g, Node *n)
{
    Type *t = infer_node_type(g, n->as.member.obj);
    if (t && t->kind == TY_STRUCT) {
        if (t->name) {
            int idx = struct_field_idx(g, t->name, n->as.member.name);
            if (idx >= 0) return struct_field_type(g, t->name, idx);
        } else {
            /* A runtime map: the type checker required the key to be a
             * declared field, so the result type is exactly the field's
             * value type it recorded on the member node. */
            return n->as.member.type;
        }
    }
    return NULL;
}

/* A map/struct literal is a named struct exactly when its *context* names one
 * — a let annotation, a return type, a parameter, a struct field (g->expect).
 * That is the same rule the type checker applies: typecheck_expr.c's N_MAP_LIT
 * reads `expected` and never scans declared shapes, and infer_node_type
 * (codegen_scan.c) answers "anonymous" for every un-annotated literal. The
 * emitters must give the same answer to the same question — an older version
 * of this function also promoted on a field-name scan, which disagreed with
 * infer_node_type inside one backend: `let rec = { u: 8, v: 9 }` under a
 * declared `type Rec = { u: int, v: int }` stored a %Rec aggregate into the
 * i8* slot the inference had allocated, and the next `rec.u` handed the field
 * value (8) to lume_map_get_i as a pointer and crashed. Tuple literals `(a, b)`
 * are the anonymous-by-default case in its purest form (SPEC §4.13). */
static Type *resolve_struct_lit(CG *g, Node *n)
{
    (void)n;
    Type *e = g->expect;
    if (e && e->kind == TY_STRUCT && e->name && sdef_find(&g->structs, e->name))
        return e;
    return NULL;
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
        Type *fty = sd->types[j];
        /* Pin the expected type to the field's declared type while lowering
         * the field expression, so a *nested* struct literal resolves to the
         * field's type instead of inheriting a leaked outer expectation
         * (e.g. `{ inner: { x: 1 } }` used to resolve the inner literal as an
         * `Outer` and complain that `Outer` is missing `inner`). The libLLVM
         * emitter does the same; the text one did not. */
        Type *prev = g->expect;
        g->expect = fty;
        Val fv = cg_expr(g, n->as.map.vals[order[j]]);
        g->expect = prev;
        if (!llvm_type_of(fty)) { free(cur); free(fv.v); free(order);
            ERRV(g, "line %zu: field '%s' has no codegen type", n->line, sd->names[j]); }
        fv = coerce(g, fty, fv, n->line);
        if (g->err[0]) { free(cur); free(fv.v); free(order); return (Val){ NULL, NULL, 0 }; }

        /* A named-struct field may arrive as an address (a variable or member
         * read — fv.agg == 0) rather than an aggregate value (a nested literal
         * — fv.agg == 1). insertvalue needs the aggregate, so load it first.
         * The serializer recursion already knows how to read a nested named
         * struct, so this is all the constructor needs. */
        char *ins;
        if (fty->kind == TY_STRUCT && fty->name && !fv.agg) {
            const char *stf = llvm_type_of(fty);
            char *ld = emit_instrf(g, fty, "load %s, %s* %s", stf, stf, fv.v);
            if (!ld) { free(cur); free(fv.v); free(order);
                ERRV(g, "line %zu: cannot load nested struct field '%s'",
                     n->line, sd->names[j]); }
            ins = emit_instrf_agg(g, "insertvalue %s %s, %s %s, %d",
                                  st, cur, stf, ld, j);
            free(ld);
        } else {
            ins = emit_instrf_agg(g, "insertvalue %s %s, %s %s, %d",
                                  st, cur, llvm_type_of(fty), fv.v, j);
        }
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

const char *rt_arg_type(Type *ty)
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
    case TY_RESULT:
    case TY_ENUM:   return "i8*";
    /* A null value travels into the runtime as a null i8* -- the container
     * helpers store it under the obj slot, and slot_emit prints "null". */
    case TY_NULL:   return "i8*";
    default:        return "i64";
    }
}

const char *map_get_fn(Type *ty)
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
    /* A null default (`get(m, k, null)`) reads through the obj getter: the
     * value comes back as a plain i8*, null for both a null slot and — as
     * with every obj read — an absent key. */
    case TY_NULL:   return "lume_map_get_obj";
    default:        return NULL;
    }
}

const char *list_push_fn(Type *ty)
{
    if (!ty) return NULL;
    switch (ty->kind) {
    case TY_FLOAT:  return "lume_list_push_f";
    case TY_STRING: return "lume_list_push_s";
    case TY_BOOL:   return "lume_list_push_b";
    case TY_INT:    return "lume_list_push_i";
    /* A nested container travels as the same opaque i8*, so it needs the same
     * argument spelling as the others -- only the slot tag differs, and that
     * is the helper's business. This is what makes `[[1,2],[3]]` buildable. */
    case TY_LIST:
    case TY_STRUCT:
    case TY_RESULT:
    case TY_ENUM:   return "lume_list_push_obj";
    /* A null element rides the obj helper with a null pointer: the slot keeps
     * its obj tag with a null payload, which every reader and printer already
     * treats as null. */
    case TY_NULL:   return "lume_list_push_obj";
    default:        return NULL;
    }
}

const char *map_put_fn(Type *ty)
{
    if (!ty) return NULL;
    switch (ty->kind) {
    case TY_FLOAT:  return "lume_map_put_f";
    case TY_STRING: return "lume_map_put_s";
    case TY_BOOL:   return "lume_map_put_b";
    case TY_INT:    return "lume_map_put_i";
    /* Containers as values, same reasoning as list_push_fn: this is what makes
     * `{ a: { b: 1 } }` buildable. */
    case TY_LIST:
    case TY_STRUCT:
    case TY_RESULT:
    case TY_ENUM:   return "lume_map_put_obj";
    /* Same null rule as list_push_fn: the obj helper stores a null payload,
     * which print/str/eq all render as null. */
    case TY_NULL:   return "lume_map_put_obj";
    default:        return NULL;
    }
}

Type *infer_list_elem(CG *g, Node *lit)
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
        else if (e->type == N_CTOR) {
            /* A bare constructor value (`[Red, Green]`): resolve_ctors
             * stamped the enum's name onto the node, so the element type is
             * statically spelled and the list does not decay to list<any>. */
            t = e->as.ctor.ename ? type_enum(e->as.ctor.ename) : NULL;
        }
        else if (e->type == N_MAP_LIT && e->as.map.is_tuple && g) {
            /* A tuple element `(1, 2)` is a positional anonymous struct: its
             * shape is exactly what infer_node_type computes for it. This is
             * what keeps `ts[0].0` off the list<any> path, where a member read
             * has no static field type to dispatch a getter with. */
            t = infer_node_type(g, e);
        }
        /* Anything else (a call, another list, ...) is a kind this emitter
         * cannot read statically. */
        if (!t) return NULL;
        if (et && et->kind != t->kind) return NULL;
        /* Two anonymous shapes unify only field-for-field: `(1, 2)` and
         * `(3, "x")` are both TY_STRUCT but must not melt into one element
         * type — the checker's heterogeneous rule (ck_list) falls back to
         * any, and so does this. */
        if (et && et->kind == TY_STRUCT && !et->name && !t->name) {
            if (et->count != t->count) return NULL;
            for (int j = 0; j < et->count; j++)
                if (!et->types[j] || !t->types[j] ||
                    strcmp(et->names[j], t->names[j]) != 0 ||
                    et->types[j]->kind != t->types[j]->kind) return NULL;
        }
        et = t;
    }
    return et;   /* `[]` has no element kind; it iterates zero times anyway */
}

Type *list_type(void) { return type_list(infer_list_elem(NULL, NULL)); }

Val cg_list_lit(CG *g, Node *n)
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

Val cg_map_or_struct_lit(CG *g, Node *n)
{
    if (resolve_struct_lit(g, n)) return cg_struct_lit(g, n);
    return cg_map_lit(g, n);
}

Val cg_struct_update(CG *g, Node *n)
{
    Val base = cg_expr(g, n->as.supd.base);
    if (!base.v) return base;

    if (base.ty && base.ty->kind == TY_STRUCT && base.ty->name) {
        Type *ty = base.ty;
        SDef *sd = sdef_find(&g->structs, ty->name);
        if (!sd) {
            free(base.v);
            ERRV(g, "line %zu: unknown type '%s'", n->line, ty->name);
        }
        const char *st = llvm_type_of(ty);
        if (!st) {
            free(base.v);
            ERRV(g, "line %zu: type '%s' has no codegen type", n->line,
                 ty->name);
        }

        /* Normalize to an address (a slot if the base is an aggregate value,
         * a no-op when it already is a reference). */
        struct_addr(g, &base);
        if (!base.v)
            ERRV(g, "line %zu: cannot update a value of type '%s'", n->line,
                 ty->name);

        char *cur = xstrdup("undef");
        for (int j = 0; j < sd->count; j++) {
            int w = -1;
            for (int k = 0; k < n->as.supd.count; k++)
                if (strcmp(n->as.supd.keys[k], sd->names[j]) == 0) { w = k; break; }

            Val fv;
            if (w >= 0) {
                fv = cg_expr(g, n->as.supd.vals[w]);
                if (!fv.v) { free(cur); free(base.v); return fv; }
            } else {
                const char *flt = llvm_type_of(sd->types[j]);
                if (!flt) {
                    free(cur); free(base.v);
                    ERRV(g, "line %zu: field '%s' has no codegen type",
                         n->line, sd->names[j]);
                }
                char *pa = emit_instrf(g, ty,
                                       "getelementptr inbounds %s, %s* %s, i32 0, i32 %d",
                                       st, st, base.v, j);
                if (!pa) {
                    free(cur); free(base.v);
                    ERRV(g, "line %zu: cannot read field '%s'", n->line,
                         sd->names[j]);
                }
                fv = val_take(sd->types[j],
                              emit_instrf(g, sd->types[j], "load %s, %s* %s",
                                          flt, flt, pa));
                free(pa);
                if (!fv.v) {
                    free(cur); free(base.v);
                    ERRV(g, "line %zu: cannot read field '%s'", n->line,
                         sd->names[j]);
                }
                /* A nested struct loads as an aggregate *value* — tag it so a
                 * further coercion/compare treats it as a value, not a pointer
                 * (same rule as cg_struct_eq's field loads). */
                if (flt[0] == '%') fv.agg = 1;
            }

            Type *fty = sd->types[j];
            const char *flt = llvm_type_of(fty);
            if (!flt) {
                free(cur); free(fv.v); free(base.v);
                ERRV(g, "line %zu: field '%s' has no codegen type", n->line,
                     sd->names[j]);
            }
            fv = coerce(g, fty, fv, n->line);
            if (g->err[0]) {
                free(cur); free(fv.v); free(base.v);
                return (Val){ NULL, NULL, 0 };
            }
            char *ins = emit_instrf_agg(g, "insertvalue %s %s, %s %s, %d",
                                        st, cur, flt, fv.v, j);
            free(cur); free(fv.v);
            if (!ins) {
                free(base.v);
                ERRV(g, "line %zu: cannot build a value of type '%s'", n->line,
                     ty->name);
            }
            cur = ins;
        }
        free(base.v);
        Val r = val_make_agg(ty, cur);
        free(cur);
        return r;
    }

    /* Anonymous map: clone the base at runtime, then overwrite the listed
     * keys (the clone shares nested containers, like the interpreter). */
    Val m = rt_call(g, type_anon_struct(), "lume_map_clone", "i8* %s", base.v);
    free(base.v);
    if (!m.v) ERRV(g, "line %zu: cannot copy a map for 'with' update", n->line);
    m.ty = type_anon_struct();

    for (int i = 0; i < n->as.supd.count; i++) {
        Val v = cg_expr(g, n->as.supd.vals[i]);
        if (!v.v) { free(m.v); return v; }
        Val k = val_take(type_prim(TY_STRING),
                         cg_string_val(g, n->as.supd.keys[i],
                                       strlen(n->as.supd.keys[i])));
        const char *fn = map_put_fn(v.ty);
        if (!fn) {
            free(k.v); free(v.v); free(m.v);
            ERRV(g, "line %zu: map values must be int, float, string or a container",
                 n->line);
        }
        if (v.ty && v.ty->kind == TY_BOOL) v = coerce(g, type_prim(TY_INT), v, n->line);
        Val r = rt_call(g, type_prim(TY_INT), fn, "i8* %s, i8* %s, %s %s",
                        m.v, k.v, rt_arg_type(v.ty), v.v);
        free(k.v); free(v.v);
        if (!r.v) {
            free(m.v);
            ERRV(g, "line %zu: cannot store into a map", n->line);
        }
        free(r.v);
    }
    return m;
}

Val cg_ctor(CG *g, Node *n)
{
    Val m = rt_call(g, list_type(), "lume_map_new", "");
    if (!m.v) ERRV(g, "line %zu: cannot allocate an enum value", n->line);
    m.ty = n->as.ctor.ename ? type_enum(n->as.ctor.ename) : type_anon_struct();

    Val k = val_take(type_prim(TY_STRING), cg_string_val(g, "$", 1));
    Val tv = val_take(type_prim(TY_STRING),
                      cg_string_val(g, n->as.ctor.name,
                                    strlen(n->as.ctor.name)));
    Val r = rt_call(g, type_prim(TY_INT), "lume_map_put_s",
                    "i8* %s, i8* %s, %s %s", m.v, k.v,
                    rt_arg_type(type_prim(TY_STRING)), tv.v);
    free(k.v); free(tv.v);
    if (!r.v) { free(m.v); ERRV(g, "line %zu: cannot store the variant tag", n->line); }
    free(r.v);

    for (int i = 0; i < n->as.ctor.argc; i++) {
        Val v = cg_expr(g, n->as.ctor.args[i]);
        k = val_take(type_prim(TY_STRING),
                     cg_string_val(g, n->as.ctor.fields[i],
                                   strlen(n->as.ctor.fields[i])));
        const char *fn = map_put_fn(v.ty);
        if (!fn) {
            free(k.v); free(v.v); free(m.v);
            ERRV(g, "line %zu: enum payload must be int, float, string or bool",
                 n->line);
        }
        if (v.ty && v.ty->kind == TY_BOOL) v = coerce(g, type_prim(TY_INT), v, n->line);
        r = rt_call(g, type_prim(TY_INT), fn, "i8* %s, i8* %s, %s %s",
                    m.v, k.v, rt_arg_type(v.ty), v.v);
        free(k.v); free(v.v);
        if (!r.v) { free(m.v); ERRV(g, "line %zu: cannot store the payload", n->line); }
        free(r.v);
    }
    return m;
}
