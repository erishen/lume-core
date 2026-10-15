/* codegen_match.c - pattern-matching emitters.
 *
 * Cut out of codegen_expr.c: the variant-tag reader, the arm-slot and
 * pattern-compare helpers, cg_match and cg_if_let.
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

Val cg_match(CG *g, Node *n);

#define MATCH_BIND_MAX 32

static char *cg_variant_tag(CG *g, Val s, size_t line)
{
    Val k = val_take(type_prim(TY_STRING), cg_string_val(g, "$", 1));
    char *tag = emit_instrf(g, type_prim(TY_STRING),
                            "call i8* @lume_map_get_s(i8* %s, i8* %s, i8* null)",
                            s.v, k.v);
    free(k.v);
    if (!tag) ERR(g, "line %zu: cannot read the variant tag", line);
    return tag;
}

static void cg_store_bind(CG *g, Val s, MatchPat *pat, int kb,
                          const char *slot, size_t line)
{
    if (!pat->vfields)
        ERRX(g, "line %zu: match binding '%s' has no codegen type",
             line, pat->binds[kb]);
    Type *bt = pat->vfield_types ? pat->vfield_types[kb] : NULL;
    const char *blt = bt ? llvm_type_of(bt) : NULL;
    if (!blt)
        ERRX(g, "line %zu: match binding '%s' has no codegen type",
             line, pat->binds[kb]);
    Val fk = val_take(type_prim(TY_STRING),
                      cg_string_val(g, pat->vfields[kb],
                                    strlen(pat->vfields[kb])));
    const char *gfn = map_get_fn(bt);
    if (!gfn) {
        free(fk.v);
        ERRX(g, "line %zu: match binding '%s' payload must be int, "
                "float, string or bool", line, pat->binds[kb]);
    }
    const char *pty = rt_arg_type(bt);
    const char *darg = strcmp(pty, "i8*") == 0   ? "i8* null"
                     : strcmp(pty, "double") == 0 ? "double 0.0"
                                                  : "i64 0";
    char *pv = emit_instrf(g, bt, "call %s @%s(i8* %s, i8* %s, %s)",
                           pty, gfn, s.v, fk.v, darg);
    free(fk.v);
    if (!pv) ERRX(g, "line %zu: cannot read the payload", line);
    /* bool reads come back i64 (lume_map_get_i); the slot — and every later
     * cg_var of the bind — spells i1, so narrow here. */
    if (bt->kind == TY_BOOL) {
        char *b1 = emit_instrf(g, bt, "icmp ne i64 %s, 0", pv);
        free(pv);
        if (!b1) ERRX(g, "line %zu: cannot read the payload", line);
        pv = b1;
    }
    EMIT(g, "  store %s %s, %s* %s\n", blt, pv, blt, slot);
    free(pv);
}

static int cg_pat_cmp(CG *g, const char *tag, MatchPat *pat, int lhit,
                      int lmiss, size_t line)
{
    if (strcmp(pat->vname, "_") == 0) {
        EMIT(g, "  br label %%L%d\n", lhit);
        return -1;
    }
    Val at = val_take(type_prim(TY_STRING),
                      cg_string_val(g, pat->vname, strlen(pat->vname)));
    char *eq64 = emit_instrf(g, type_prim(TY_INT),
                             "call i64 @lume_str_eq(i8* %s, i8* %s)",
                             tag, at.v);
    free(at.v);
    if (!eq64) {
        ERR_SET(g, "line %zu: cannot compare the variant tag", line);
        return -1;
    }
    char *eq = emit_instrf(g, type_prim(TY_BOOL), "icmp ne i64 %s, 0", eq64);
    free(eq64);
    if (!eq) {
        ERR_SET(g, "line %zu: cannot compare the variant tag", line);
        return -1;
    }
    EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n", eq, lhit, lmiss);
    free(eq);
    return lmiss;
}

static void cg_arm_slots(CG *g, MatchPat *p0, char slots[][64], size_t line)
{
    for (int kb = 0; kb < p0->bindc; kb++) {
        Type *bt = p0->vfield_types ? p0->vfield_types[kb] : NULL;
        const char *blt = bt ? llvm_type_of(bt) : NULL;
        if (!blt)
            ERRX(g, "line %zu: match binding '%s' has no codegen type",
                 line, p0->binds[kb]);
        snprintf(slots[kb], 64, "%%lv_%s_%d", p0->binds[kb], g->tid++);
        EMIT(g, "  %s = alloca %s\n", slots[kb], blt);
    }
}

Val cg_match(CG *g, Node *n)
{
    Type *rty = infer_node_type(g, n);
    if (!rty || rty->kind == TY_ANY)
        ERRV(g, "line %zu: cannot infer the match result type", n->line);
    const char *st = llvm_type_of(rty);
    if (!st) ERRV(g, "line %zu: unsupported match result type", n->line);

    Val s = cg_expr(g, n->as.matchn.scrutinee);
    if (!s.v) ERRV(g, "line %zu: bad match scrutinee", n->line);

    char *tag = cg_variant_tag(g, s, n->line);
    if (!tag) { free(s.v); ERRV(g, "line %zu: cannot read the variant tag", n->line); }

    char res[64];
    snprintf(res, sizeof res, "%%tv%d", g->tid++);
    EMIT(g, "  %s = alloca %s\n", res, st);

    int lj = g->lid++;
    int prev_entry = -1;   /* label the previous pattern's false edge needs */
    for (int i = 0; i < n->as.matchn.armc; i++) {
        MatchArm *arm = &n->as.matchn.arms[i];
        bool last_arm = i == n->as.matchn.armc - 1;
        int larm = g->lid++, lnext = g->lid++;
        int lbody = arm->guard ? g->lid++ : -1;

        MatchPat *p0 = &arm->pats[0];
        if (p0->bindc > MATCH_BIND_MAX) {
            free(tag); free(s.v);
            ERRV(g, "line %zu: too many match bindings (max %d)",
                 arm->line, MATCH_BIND_MAX);
        }
        char slots[MATCH_BIND_MAX][64];

        for (int pi = 0; pi < arm->patc; pi++) {
            MatchPat *pat = &arm->pats[pi];
            bool last_pat = pi == arm->patc - 1;
            if (prev_entry >= 0) { EMIT(g, "L%d:\n", prev_entry); prev_entry = -1; }
            if (pi == 0) {
                /* The arm's bind slots go at the top of the arm's entry
                 * block — the block every pattern block of this arm is
                 * entered from, so they dominate the arm block's loads.
                 * Allocating them any later (e.g. at the top of the arm
                 * loop) drops an alloca after the previous arm's `br`,
                 * which is not IR: clang's parser stops at the next label
                 * with "expected instruction opcode". One set serves all
                 * alternatives: the checker unified their names/types. */
                cg_arm_slots(g, p0, slots, arm->line);
                if (g->err[0]) { free(tag); free(s.v); return (Val){ NULL, NULL, 0 }; }
            }
            int lpb = g->lid++;
            if (last_arm && last_pat) {
                /* catches whatever is left — no compare needed */
                EMIT(g, "  br label %%L%d\n", lpb);
            } else {
                /* a miss on the last pattern of a non-last arm falls to the
                 * next arm; otherwise to this arm's next pattern */
                int lmiss = last_pat ? lnext : g->lid++;
                prev_entry = cg_pat_cmp(g, tag, pat, lpb, lmiss, arm->line);
                if (g->err[0]) { free(tag); free(s.v); return (Val){ NULL, NULL, 0 }; }
            }
            EMIT(g, "L%d:\n", lpb);
            for (int kb = 0; kb < pat->bindc; kb++)
                cg_store_bind(g, s, pat, kb, slots[kb], arm->line);
            if (g->err[0]) { free(tag); free(s.v); return (Val){ NULL, NULL, 0 }; }
            EMIT(g, "  br label %%L%d\n", larm);
        }

        EMIT(g, "L%d:\n", larm);
        int saved_n = g->locals.n;
        for (int kb = 0; kb < p0->bindc; kb++)
            asg_push(&g->locals, p0->binds[kb],
                     p0->vfield_types ? p0->vfield_types[kb] : NULL,
                     slots[kb], arm->line);
        if (arm->guard) {
            Val gv = cg_expr(g, arm->guard);
            if (!gv.v) { free(tag); free(s.v); ERRV(g, "line %zu: bad match guard", arm->line); }
            EMIT(g, "  br i1 %s, label %%L%d, label %%L%d\n", gv.v, lbody, lnext);
            free(gv.v);
            EMIT(g, "L%d:\n", lbody);
        }
        Val bv = cg_expr(g, arm->body);
        if (!bv.v) { free(tag); free(s.v); ERRV(g, "line %zu: bad match arm body", arm->line); }
        bv = coerce(g, rty, bv, arm->line);
        EMIT(g, "  store %s %s, %s* %s\n", st, bv.v, st, res);
        free(bv.v);
        asg_truncate(&g->locals, saved_n); /* truncate this arm's binds */
        EMIT(g, "  br label %%L%d\n", lj);
        if (last_arm && arm->guard) {
            /* a guarded last arm falling through is a forged map */
            EMIT(g, "L%d:\n", lnext);
            EMIT(g, "  unreachable\n");
        } else {
            prev_entry = lnext;  /* next arm's entry; also the guard's miss */
        }
    }

    EMIT(g, "L%d:\n", lj);
    free(s.v);
    free(tag);
    char *r = emit_instrf(g, rty, "load %s, %s* %s", st, st, res);
    if (!r) ERRV(g, "line %zu: bad match join", n->line);
    return val_take(rty, r);
}

void cg_if_let(CG *g, Node *n)
{
    Val v = cg_expr(g, n->as.iflet.value);
    if (!v.v) ERRX(g, "line %zu: bad if-let value", n->line);
    char *tag = cg_variant_tag(g, v, n->line);
    if (!tag) { free(v.v); ERRX(g, "line %zu: cannot read the variant tag", n->line); }

    int lt = g->lid++, lel = g->lid++, lend = g->lid++;
    MatchPat *p0 = &n->as.iflet.pats[0];
    if (p0->bindc > MATCH_BIND_MAX) {
        free(tag); free(v.v);
        ERRX(g, "line %zu: too many if-let bindings (max %d)",
             n->line, MATCH_BIND_MAX);
    }
    char slots[MATCH_BIND_MAX][64];
    cg_arm_slots(g, p0, slots, n->line);
    if (g->err[0]) { free(tag); free(v.v); return; }

    for (int pi = 0; pi < n->as.iflet.patc; pi++) {
        MatchPat *pat = &n->as.iflet.pats[pi];
        bool last_pat = pi == n->as.iflet.patc - 1;
        int lpb = g->lid++;
        int lmiss = last_pat ? lel : g->lid++;
        (void)cg_pat_cmp(g, tag, pat, lpb, lmiss, n->line);
        if (g->err[0]) { free(tag); free(v.v); return; }
        EMIT(g, "L%d:\n", lpb);
        for (int kb = 0; kb < pat->bindc; kb++)
            cg_store_bind(g, v, pat, kb, slots[kb], n->line);
        if (g->err[0]) { free(tag); free(v.v); return; }
        EMIT(g, "  br label %%L%d\n", lt);
        if (!last_pat) EMIT(g, "L%d:\n", lmiss);
    }

    EMIT(g, "L%d:\n", lt);
    int saved_n = g->locals.n;
    for (int kb = 0; kb < p0->bindc; kb++)
        asg_push(&g->locals, p0->binds[kb],
                 p0->vfield_types ? p0->vfield_types[kb] : NULL,
                 slots[kb], n->line);
    cg_stmt(g, n->as.iflet.then);
    asg_truncate(&g->locals, saved_n);
    EMIT(g, "  br label %%L%d\n", lend);

    EMIT(g, "L%d:\n", lel);
    if (n->as.iflet.els) cg_stmt(g, n->as.iflet.els);
    EMIT(g, "  br label %%L%d\n", lend);

    EMIT(g, "L%d:\n", lend);
    free(tag);
    free(v.v);
}
