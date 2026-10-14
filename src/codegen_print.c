/* codegen_print.c - print() lowering and struct serialization.
 *
 * Cut out of codegen_expr.c: cg_print (reached from cg_call's print route)
 * and emit_struct_to_map (also used by the str()/stringify() branch of
 * cg_builtin), which turns a struct value into the runtime map the JSON
 * printer already knows how to render.
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

Val cg_print(CG *g, Node *n, Val a);
Val emit_struct_to_map(CG *g, const char *sv, Type *st);

Val cg_print(CG *g, Node *n, Val a)
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
        /* A named struct is a bare GEP value; build a map from its fields
         * (emit_struct_to_map) and print it the same way an anonymous map
         * prints — matching the interpreter's JSON output. */
        struct_addr(g, &a);
        Val m = emit_struct_to_map(g, a.v, a.ty);
        if (!m.v) { free(a.v); return m; }
        free(a.v);
        fn = "lume_map_print"; pty = "i8*"; arg = m.v;
        break;
    /* A Result *is* a map ({ ok: ... } / { err: ... }), so it prints through
     * the same runtime helper. Without this it fell to the default arm and the
     * interpreter could print a Result that the emitters refused to build. */
    case TY_RESULT: fn = "lume_map_print"; pty = "i8*"; break;
    /* An enum value is the same heap map with a "$" tag, so it prints the
     * same way — matching the interpreter, which prints the raw map. */
    case TY_ENUM:   fn = "lume_map_print"; pty = "i8*"; break;
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

Val emit_struct_to_map(CG *g, const char *sv, Type *st)
{
    Val m = rt_call(g, type_anon_struct(), "lume_map_new", "");
    if (!m.v) return m;

    const char *stn = llvm_type_of(st);
    if (!stn) { free(m.v); ERRV(g, "cannot serialize struct '%s'", st->name); }

    /* A struct type handed in may be a bare name reference (count == 0) —
     * that is what a member access returns, because struct_field_type() gives
     * the codegen struct table's stored field type, which is kept as a
     * reference. The declared field list therefore has to be resolved through
     * the SDef by name; the variable's own type is the resolved one, which is
     * why the nested case already worked. */
    SDef *sd = st->name ? sdef_find(&g->structs, st->name) : NULL;

    for (int i = 0; i < (sd ? sd->count : st->count); i++) {
        Type *fty   = sd ? sd->types[i] : st->types[i];
        const char *fname = sd ? sd->names[i] : st->names[i];

        char *key = cg_string_val(g, fname, strlen(fname));
        if (!key) { free(m.v); return (Val){ NULL, NULL, 0 }; }

        char *pa = emit_instrf(g, st,
                               "getelementptr inbounds %s, %s* %s, i32 0, i32 %d",
                               stn, stn, sv, i);
        if (!pa) { free(key); free(m.v); return (Val){ NULL, NULL, 0 }; }

        if (fty->kind == TY_STRUCT && fty->name) {
            /* Nested named struct: convert it the same way, then store the
             * resulting map as an obj value. */
            Val inner = emit_struct_to_map(g, pa, fty);
            free(pa);
            if (!inner.v) { free(key); free(m.v); return inner; }
            Val r = rt_call(g, type_anon_struct(), "lume_map_put_obj",
                            "i8* %s, i8* %s, i8* %s", m.v, key, inner.v);
            free(inner.v); free(key);
            if (!r.v) { free(m.v); return r; }
            continue;
        }

        const char *flt = llvm_type_of(fty);
        char *fv = emit_instrf(g, fty, "load %s, %s* %s", flt, flt, pa);
        free(pa);
        if (!fv) { free(key); free(m.v); return (Val){ NULL, NULL, 0 }; }

        const char *fn = map_put_fn(fty);
        if (!fn) { free(fv); free(key); free(m.v);
                   ERRV(g, "struct field '%s' has no serializable type", fname); }

        /* The put helper's value parameter type: float is double, int/bool
         * are i64 (bool widened from i1), everything else is i8*. */
        const char *t;
        char *wv = fv;
        if (fty->kind == TY_FLOAT) {
            t = "double";
        } else if (fty->kind == TY_BOOL) {
            wv = emit_instrf(g, type_prim(TY_INT), "zext i1 %s to i64", fv);
            free(fv); fv = wv; t = "i64";
        } else if (fty->kind == TY_INT) {
            t = "i64";
        } else {
            t = "i8*";
        }

        char farg[128];
        snprintf(farg, sizeof farg, "%s %s", t, fv);
        Val r = rt_call(g, type_anon_struct(), fn,
                        "i8* %s, i8* %s, %s", m.v, key, farg);
        free(fv); free(key);
        if (!r.v) { free(m.v); return r; }
    }
    return m;
}
