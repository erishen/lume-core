/* ============================================================================
 * interp.c — Table of Contents
 *
 * Tree-walk interpreter (the "Runtime"). See the prose block just below for
 * the value-stack / longjmp / bridge contract. Navigation anchors are the
 * dashed section banners already scattered through the file:
 *
 *  1. VM plumbing .......................... L44
 *  2. string literal unescaping ............ L73
 *  3. function call engine ................. L87
 *  4. expressions (eval_expr dispatch) ..... L280
 *  5. expression cases (one fn each) ....... L580
 *  6. statements (exec_statement dispatch) . L903
 *  7. statement cases (one fn each) ........ L1125
 *  8. VM setup & top-level execution ....... L1365
 * ========================================================================== */

#include "lume.h"
#include "builtins.h"
#include <math.h>
#include <stdarg.h>

/* Tree-walk interpreter (the "Runtime"). Every expression evaluation leaves
 * exactly ONE value on the VM's value stack, so partial results are always
 * GC roots. User-function calls push a jmp_buf; `return` longjmps to it.
 * The bridge (bridge.c) reuses the same call machinery for HTTP routes and
 * agent tool callbacks. */

/* Statements and expressions are dispatched by a single big switch; the heavy
 * cases live in their own functions so the dispatcher stays readable. */
static void exec_statement(VM *vm, Node *n, Env *env);
static void exec_block_walk(VM *vm, Node *block, Env *env);
static void exec_statement_for(VM *vm, Node *n, Env *env);
static void exec_statement_route(VM *vm, Node *n, Env *env);
static void exec_statement_verbs(VM *vm, Node *n, Env *env);
static void exec_statement_tool(VM *vm, Node *n, Env *env);
static void eval_expr(VM *vm, Node *n, Env *env);
static void eval_expr_literal(VM *vm, Node *n);
static void eval_expr_member(VM *vm, Node *n, Env *env);
static void eval_expr_index(VM *vm, Node *n, Env *env);
static void eval_expr_binary(VM *vm, Node *n, Env *env);
static void eval_expr_call(VM *vm, Node *n, Env *env);

/* ---------- VM plumbing ---------- */

void vm_set_error(VM *vm, const char *fmt, ...) {
    if (vm->error) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->error_msg, sizeof(vm->error_msg), fmt, ap);
    va_end(ap);
    vm->error = true;
}

void vm_push(VM *vm, Value v) {
    if (vm->stack_count >= AL_STACK_MAX) {
        vm_set_error(vm, "value stack overflow (max %d)", AL_STACK_MAX);
        return;
    }
    vm->stack[vm->stack_count++] = v;
}

Value vm_pop(VM *vm) {
    if (vm->stack_count <= 0) return val_null(); /* defensive underflow */
    return vm->stack[--vm->stack_count];
}

Value vm_peek(VM *vm, int depth) {
    if (vm->stack_count - 1 - depth < 0) return val_null();
    return vm->stack[vm->stack_count - 1 - depth];
}

/* ---------- string literal unescaping ---------- */

/* Thin wrapper over the shared table (value.c): s spans the raw lexeme
 * INCLUDING the surrounding quotes, so the body is s[1..n-1). Kept as a
 * function because every call site reads the quote-inclusive span straight off
 * the token. */
static char *unescape_literal(const char *s, int n, int *out_len) {
    size_t w = 0;
    char *buf = lume_unescape(s + 1, (size_t)(n - 2), &w);
    *out_len = (int)w;
    return buf;
}

#include "builtins.h"
/* ---------- function call engine ---------- */

/* Bind one function declaration into `env`, under the name its call sites
 * spell: the synthetic cname for a hoisted local function, the source name
 * for a top-level one. Shared by the statement executor and the frame hoist
 * below. */
static void bind_func_decl(VM *vm, Env *env, Node *n)
{
    const char *fname = n->as.func.cname ? n->as.func.cname
                                         : n->as.func.name;
    Value f = make_func(vm, fname, n->as.func.names,
                        n->as.func.arity, n->as.func.body, env);
    env_set(vm, env, fname, f);
}

/* Pre-bind every local function declared inside `body` into the frame env
 * before the body runs. The native backends resolve a local through a
 * signature table built before any code executes, so a call that textually
 * precedes the declaration works there; hoisting at frame setup gives the
 * interpreter the same reach, recursion included. The walk mirrors the
 * resolver's scan: statement level, control-flow blocks and lambda bodies —
 * but never a nested function's own body, whose locals bind when that
 * function's own frame is set up (through this same hoist). */
static void hoist_local_fns(VM *vm, Env *env, Node *n)
{
    if (!n || vm->error) return;
    switch (n->type) {
    case N_FUNC_DECL:
        bind_func_decl(vm, env, n);
        return;
    case N_FUNC_LIT:
        hoist_local_fns(vm, env, n->as.funclit.body);
        return;
    case N_PROGRAM: case N_BLOCK:
        for (int i = 0; i < n->as.block.count; i++)
            hoist_local_fns(vm, env, n->as.block.stmts[i]);
        return;
    case N_LET:        hoist_local_fns(vm, env, n->as.let.init); return;
    case N_LET_PAT:    hoist_local_fns(vm, env, n->as.letpat.init); return;
    case N_IF:
        hoist_local_fns(vm, env, n->as.ifs.cond);
        hoist_local_fns(vm, env, n->as.ifs.then);
        hoist_local_fns(vm, env, n->as.ifs.els);
        return;
    case N_WHILE:
        hoist_local_fns(vm, env, n->as.whiles.cond);
        hoist_local_fns(vm, env, n->as.whiles.body);
        return;
    case N_FOR:
        hoist_local_fns(vm, env, n->as.fors.init);
        hoist_local_fns(vm, env, n->as.fors.cond);
        hoist_local_fns(vm, env, n->as.fors.incr);
        hoist_local_fns(vm, env, n->as.fors.iterable);
        hoist_local_fns(vm, env, n->as.fors.body);
        return;
    case N_RETURN:     hoist_local_fns(vm, env, n->as.ret.expr); return;
    case N_EXPR_STMT:  hoist_local_fns(vm, env, n->as.expr_stmt.expr); return;
    case N_ASSIGN:     hoist_local_fns(vm, env, n->as.assign.value); return;
    case N_ASSIGN_MEMBER:
        hoist_local_fns(vm, env, n->as.assign_mem.obj);
        hoist_local_fns(vm, env, n->as.assign_mem.value);
        return;
    case N_CALL:
        hoist_local_fns(vm, env, n->as.call.callee);
        for (int i = 0; i < n->as.call.argc; i++)
            hoist_local_fns(vm, env, n->as.call.args[i]);
        return;
    case N_MEMBER:     hoist_local_fns(vm, env, n->as.member.obj); return;
    case N_INDEX:
        hoist_local_fns(vm, env, n->as.index.obj);
        hoist_local_fns(vm, env, n->as.index.index);
        return;
    case N_UNARY:      hoist_local_fns(vm, env, n->as.unary.operand); return;
    case N_BINARY:
        hoist_local_fns(vm, env, n->as.binary.left);
        hoist_local_fns(vm, env, n->as.binary.right);
        return;
    case N_TERNARY:
        hoist_local_fns(vm, env, n->as.ternary.cond);
        hoist_local_fns(vm, env, n->as.ternary.then);
        hoist_local_fns(vm, env, n->as.ternary.els);
        return;
    case N_MATCH:
        hoist_local_fns(vm, env, n->as.matchn.scrutinee);
        for (int i = 0; i < n->as.matchn.armc; i++) {
            hoist_local_fns(vm, env, n->as.matchn.arms[i].guard);
            hoist_local_fns(vm, env, n->as.matchn.arms[i].body);
        }
        return;
    case N_LET_EXPR:
        hoist_local_fns(vm, env, n->as.letx.init);
        hoist_local_fns(vm, env, n->as.letx.body);
        return;
    default: return;
    }
}

/* Contract: the caller has already pushed the callee and its args onto the
 * value stack (callee first, then the args for a total of argc+1 slots).
 * call_function replaces that region with a single result value. */
void call_function(VM *vm, Value callee, int argc) {
    if (vm->error) return;

    /* Loop flags are lexical to the caller; a call must not leak a
     * break/continue set inside the callee (a `return` longjmps past the
     * loop's save/restore, so the flag would otherwise stay set). */
    bool saved_brk = vm->loop_break, saved_cont = vm->loop_continue;

    int slot = vm->stack_count - argc - 1; /* callee slot */

    Obj *f = IS_OBJ(callee) ? AS_OBJ(callee) : NULL;
    if (!f || (f->type != OBJ_NATIVE && f->type != OBJ_FUNC)) {
        vm_set_error(vm, "calling a non-function value");
        vm->stack_count = slot;
        vm_push(vm, val_null());
        vm->loop_break = saved_brk;
        vm->loop_continue = saved_cont;
        return;
    }

    if (f->type == OBJ_NATIVE) {
        Value result = f->as.native.fn(vm, argc, &vm->stack[slot + 1]);
        vm->stack_count = slot;
        vm_push(vm, vm->error ? val_null() : result);
        vm->loop_break = saved_brk;
        vm->loop_continue = saved_cont;
        return;
    }

    /* user function */
    if (f->as.fn.arity != argc) {
        vm_set_error(vm, "function '%s' expects %d arguments, got %d",
                     f->as.fn.name, f->as.fn.arity, argc);
        vm->stack_count = slot;
        vm_push(vm, val_null());
        vm->loop_break = saved_brk;
        vm->loop_continue = saved_cont;
        return;
    }

    Value envv = make_env(vm, f->as.fn.closure);
    vm_push(vm, envv); /* root the fresh frame until activated */
    Env *frame = (Env *)AS_OBJ(envv);
    frame->is_boundary = true; /* assignment resolves no further out */

    for (int i = 0; i < argc; i++)
        env_set(vm, frame, f->as.fn.params[i], vm->stack[slot + 1 + i]);
    if (vm->error) goto user_done;

    /* Locals of this body bind before the body runs, matching the native
     * backends' pre-built signature tables (forward calls and recursion). */
    hoist_local_fns(vm, frame, (Node *)f->as.fn.body);
    if (vm->error) goto user_done;

    frame->next_active = vm->active_envs;
    vm->active_envs = frame;
    vm->call_result = val_null();

    int index = vm->jump_depth;
    if (index >= AL_FRAME_MAX) {
        vm_set_error(vm, "call depth exceeded (%d)", AL_FRAME_MAX);
        goto user_done;
    }
    if (setjmp(vm->jump_bufs[index]) == 0) {
        vm->jump_depth++;
        exec_block_walk(vm, (Node *)f->as.fn.body, frame);
    }
    /* both the normal and the longjmp paths pop the frame */
    vm->jump_depth--;

user_done:
    /* deactivate the frame in both normal and jump paths */
    vm->active_envs = frame->next_active;
    vm->stack_count = slot;
    vm_pop(vm); /* drop the env root */
    vm->stack_count = slot;
    vm_push(vm, vm->error ? val_null() : vm->call_result);
    vm->loop_break = saved_brk;
    vm->loop_continue = saved_cont;
}

static void exec_block_walk(VM *vm, Node *block, Env *env) {
    if (!block) return;
    if (block->type == N_BLOCK) {
        for (int i = 0; i < block->as.block.count && !vm->error; i++) {
            exec_statement(vm, block->as.block.stmts[i], env);
            if (vm->loop_break || vm->loop_continue) break;
        }
        return;
    }
    exec_statement(vm, block, env);
}

/* ---------- expressions ---------- */

/* Zero value of a statically-known member type. Used when a typed map member
 * (a tool handler's `arg.<field>`) is absent, so callers get a usable default
 * rather than a runtime error. `any`/aggregate types fall back to null. */
static Value vm_zero_value(VM *vm, Type *t) {
    switch (t->kind) {
        case TY_INT:
        case TY_FLOAT:
            return val_num(0.0);
        case TY_STRING:
            return make_string_cstr(vm, "");
        case TY_BOOL:
            return val_bool(false);
        default:
            return val_null();
    }
}

static void eval_expr(VM *vm, Node *n, Env *env) {
    if (vm->error) return;
    switch (n->type) {
        case N_LITERAL:
            eval_expr_literal(vm, n);
            return;
        case N_VAR: {
            int found = 0;
            Value v = env_get(env, n->as.var.name, &found);
            if (!found) {
                vm_set_error(vm, "line %zu: undefined variable '%s'", n->line, n->as.var.name);
                return;
            }
            vm_push(vm, v);
            return;
        }
        case N_ASSIGN: {
            eval_expr(vm, n->as.assign.value, env);
            if (vm->error) return;
            Value v = vm_peek(vm, 0); /* keep rooted on the stack */
            /* `=` reuses the nearest visible binding inside the current
             * function/module frame — the typechecker models it that way
             * ("外层已有同名时复用") and the native backends store straight
             * into that variable's slot. The walk stops at the first
             * boundary env (function frame / module), which is what keeps
             * "assigning a top-level name inside a function defines a
             * local" intact. Without the walk, an assignment inside a
             * match arm or if-let body (both run in a child env) would
             * silently shadow the outer variable instead of updating it,
             * diverging from the other two backends. */
            for (Env *e = env; e; e = e->parent) {
                for (int i = 0; i < e->vars.count; i++) {
                    if (strcmp(e->vars.keys[i], n->as.assign.name) == 0) {
                        env_set(vm, e, n->as.assign.name, v);
                        return; /* result: assigned value, already on stack */
                    }
                }
                if (e->is_boundary) break;
            }
            env_set(vm, env, n->as.assign.name, v);
            return; /* result: assigned value, already on stack */
        }
        case N_ASSIGN_MEMBER: {
            eval_expr(vm, n->as.assign_mem.obj, env);
            if (vm->error) return;
            eval_expr(vm, n->as.assign_mem.value, env);
            if (vm->error) return;
            Value val = vm_peek(vm, 0);
            Value objv = vm_peek(vm, 1);
            if (!IS_OBJ(objv) || AS_OBJ(objv)->type != OBJ_MAP) {
                vm_set_error(vm, "line %zu: cannot assign member on non-map", n->line);
                return;
            }
            map_set(vm, AS_OBJ(objv), n->as.assign_mem.name, val);
            vm_pop(vm); /* val */
            vm_pop(vm); /* obj */
            vm_push(vm, val);
            return;
        }
        case N_MAP_LIT: {
            Obj *m = AS_OBJ(make_map(vm));
            vm_push(vm, val_obj(m)); /* root during construction */
            for (int i = 0; i < n->as.map.count; i++) {
                eval_expr(vm, n->as.map.vals[i], env);
                if (vm->error) return;
                Value v = vm_pop(vm);
                map_set(vm, m, n->as.map.keys[i], v);
            }
            return; /* map stays on stack */
        }
        case N_STRUCT_UPDATE: {
            /* `{ base with k: v, ... }`: copy the base, then overwrite. The
             * base stays on the stack across the copy — map_shallow_copy's
             * single gc_alloc can trigger a collection, and an unrooted base
             * would be swept out from under the copy loop. */
            eval_expr(vm, n->as.supd.base, env);
            if (vm->error) return;
            Value base = vm_peek(vm, 0);
            if (!IS_OBJ(base) || AS_OBJ(base)->type != OBJ_MAP) {
                vm_set_error(vm, "line %zu: 'with' update requires a map or "
                                 "struct base", n->line);
                return;
            }
            Obj *m = map_shallow_copy(vm, AS_OBJ(base));
            vm_pop(vm);              /* drop the base */
            vm_push(vm, val_obj(m)); /* root the copy during the overrides */
            for (int i = 0; i < n->as.supd.count; i++) {
                eval_expr(vm, n->as.supd.vals[i], env);
                if (vm->error) return;
                Value v = vm_pop(vm);
                map_set(vm, m, n->as.supd.keys[i], v);
            }
            return; /* the updated map stays on stack */
        }
        case N_LIST_LIT: {
            Obj *list = AS_OBJ(make_list(vm));
            vm_push(vm, val_obj(list)); /* root during construction */
            for (int i = 0; i < n->as.list.count; i++) {
                eval_expr(vm, n->as.list.items[i], env);
                if (vm->error) return;
                Value v = vm_pop(vm);
                vm_push(vm, v); /* root while the array grows */
                if (list->as.list.count == list->as.list.cap) {
                    list->as.list.cap = list->as.list.cap ? list->as.list.cap * 2 : 8;
                    list->as.list.items = realloc(list->as.list.items,
                                                  sizeof(Value) * (size_t)list->as.list.cap);
                }
                list->as.list.items[list->as.list.count++] = v;
                vm_pop(vm);
            }
            return;
        }
        case N_FUNC_LIT: {
            Value f = make_func(vm, "(anonymous)", n->as.funclit.names,
                                n->as.funclit.arity, n->as.funclit.body, env);
            vm_push(vm, f);
            return;
        }
        case N_MEMBER:
            eval_expr_member(vm, n, env);
            return;
        case N_INDEX:
            eval_expr_index(vm, n, env);
            return;
        case N_BINARY:
            eval_expr_binary(vm, n, env);
            return;
        case N_TERNARY:
            /* 只求值选中分支——`flag ? 1 : 1/0` 在 flag 为真时不得除零。 */
            eval_expr(vm, n->as.ternary.cond, env);
            if (vm->error) return;
            {
                Value c = vm_pop(vm);
                eval_expr(vm, value_truthy(c) ? n->as.ternary.then
                                              : n->as.ternary.els, env);
            }
            return;
        case N_LET_EXPR: {
            /* `let x = e1 in e2` — e1 evaluates once, the binding lands in
             * a child env that lives exactly as long as e2, and e2's value
             * is the expression's. The child env is what makes the binding
             * invisible after the expression and what lets it shadow an
             * outer same-name binding for the body only — the same scope
             * the native backends spell as an emit-time slot push plus a
             * truncate. Assignments inside the body still reach through:
             * the env-chain walk in N_ASSIGN stops at the first boundary,
             * and this child is not one. */
            eval_expr(vm, n->as.letx.init, env);
            if (vm->error) return;
            Value v = vm_pop(vm);
            /* Keep the named struct type on the object when a map literal
             * is bound to it (same stamp a statement let applies, so `.len`
             * reads the declared field here too). */
            Type *at = n->as.letx.annot;
            if (at && at->kind == TY_STRUCT && at->name &&
                IS_OBJ(v) && AS_OBJ(v)->type == OBJ_MAP)
                AS_OBJ(v)->sname = at->name;
            Value envv = make_env(vm, env);
            Env *benv = (Env *)AS_OBJ(envv);
            env_set(vm, benv, n->as.letx.name, v);
            benv->next_active = vm->active_envs;
            vm->active_envs = benv;
            eval_expr(vm, n->as.letx.body, benv);
            vm->active_envs = benv->next_active;
            return; /* e2's value is already on the stack */
        }
        case N_CTOR: {
            /* 构造值 = 带 "$" 标签的 map：`{ "$": V, f1: a1, ... }`。字段名
             * 由 parser.c 的 resolve_ctors 盖戳（借用 enum decl），这里无需
             * 查任何表。 */
            Obj *m = AS_OBJ(make_map(vm));
            vm_push(vm, val_obj(m)); /* root during construction */
            Value tag = make_string_cstr(vm, n->as.ctor.name);
            map_set(vm, m, "$", tag);
            for (int i = 0; i < n->as.ctor.argc; i++) {
                eval_expr(vm, n->as.ctor.args[i], env);
                if (vm->error) return;
                Value v = vm_pop(vm);
                map_set(vm, m, n->as.ctor.fields[i], v);
            }
            return; /* map stays on stack */
        }
        case N_MATCH: {
            /* 标签分发：读 "$"，逐臂逐模式比对（或通配），臂内绑定进子作用域。
             * typecheck 已保证穷尽；未命中的运行时形状（伪造 map）报错。
             * GC 纪律：scrutinee pop 后重新压根——make_env/env_set 可能触发
             * 回收，而 sv 在臂环境建好前不被任何根引用（ctor 直传值时它只在
             * 栈上）。根在求值臂体前一刻才弹，臂体结果于是干净地留在栈顶。 */
            eval_expr(vm, n->as.matchn.scrutinee, env);
            if (vm->error) return;
            Value sv = vm_pop(vm);
            vm_push(vm, sv); /* re-root the scrutinee across the arm setup */
            if (!IS_OBJ(sv) || AS_OBJ(sv)->type != OBJ_MAP) {
                vm_set_error(vm, "line %zu: match needs an enum value",
                             n->line);
                return;
            }
            int found = 0;
            Value tagv = map_get(vm, AS_OBJ(sv), "$", &found);
            if (!found || !IS_OBJ(tagv) ||
                AS_OBJ(tagv)->type != OBJ_STRING) {
                vm_set_error(vm,
                        "line %zu: match needs an enum value (a map with a "
                        "'$' variant tag)", n->line);
                return;
            }
            const char *tag = obj_string(AS_OBJ(tagv));
            for (int i = 0; i < n->as.matchn.armc; i++) {
                MatchArm *arm = &n->as.matchn.arms[i];
                for (int pi = 0; pi < arm->patc; pi++) {
                    MatchPat *pat = &arm->pats[pi];
                    bool hit = strcmp(pat->vname, "_") == 0 ||
                               strcmp(pat->vname, tag) == 0;
                    if (!hit) continue;
                    Value envv = make_env(vm, env);
                    vm_push(vm, envv); /* root the fresh frame until activated */
                    Env *arm_env = (Env *)AS_OBJ(envv);
                    for (int k = 0; k < pat->bindc; k++) {
                        int f2 = 0;
                        Value pv = map_get(vm, AS_OBJ(sv), pat->vfields[k], &f2);
                        env_set(vm, arm_env, pat->binds[k],
                                f2 ? pv : val_null());
                    }
                    /* Same dynamic-root dance as a call frame: hand the arm env
                     * to vm->active_envs (the GC marks that chain), drop the
                     * explicit stack root, and only then evaluate — the value
                     * the body pushes must be the thing this eval leaves on
                     * top of the stack. */
                    arm_env->next_active = vm->active_envs;
                    vm->active_envs = arm_env;
                    vm_pop(vm); /* drop the env root; active_envs holds it */
                    if (arm->guard) {
                        /* The guard runs in the arm env — it may read the
                         * binds. False falls through to the next pattern. */
                        eval_expr(vm, arm->guard, arm_env);
                        if (vm->error) {
                            vm->active_envs = arm_env->next_active;
                            return;
                        }
                        bool ok = value_truthy(vm_pop(vm));
                        if (!ok) {
                            vm->active_envs = arm_env->next_active;
                            break; /* next pattern of this arm */
                        }
                    }
                    vm_pop(vm); /* drop the scrutinee root; body is next */
                    eval_expr(vm, arm->body, arm_env);
                    vm->active_envs = arm_env->next_active;
                    return;
                }
            }
            vm_set_error(vm, "line %zu: no match arm for variant '%s'",
                         n->line, tag);
            return;
        }
        case N_CALL:
            eval_expr_call(vm, n, env);
            return;
        case N_UNARY: {
            eval_expr(vm, n->as.unary.operand, env);
            if (vm->error) return;
            Value v = vm_pop(vm);
            if (n->as.unary.op == OP_NOT) {
                vm_push(vm, val_bool(!value_truthy(v)));
            } else { /* OP_NEG */
                if (!IS_NUM(v)) { vm_set_error(vm, "cannot negate a non-number"); return; }
                /* Keep int an int. val_num(-AS_NUM(v)) widened every negation
                 * to float, so `let a = -7; a % 3` was rejected with "'%' does
                 * not apply to floats" — the type checker calls `-7` an int, so
                 * the runtime disagreed with it. Negate in i64 for int. */
                if (IS_INT(v)) vm_push(vm, val_int(-AS_INT(v)));
                else          vm_push(vm, val_float(-AS_NUM(v)));
            }
            return;
        }
        default:
            vm_set_error(vm, "line %zu: internal error (expr node %d)", n->line, (int)n->type);
            return;
    }
}

/* ---------- expression cases, one function each ---------- */

static void eval_expr_literal(VM *vm, Node *n) {
    switch (n->as.lit.kind) {
        case LIT_NUM:
            /* Integers keep their i64 (docs/SPEC.md); only float literals take
             * the double slot. Pushing both through val_num() is what used to
             * round every int past 2^53. */
            if (n->as.lit.is_float) vm_push(vm, val_float(n->as.lit.num));
            else                   vm_push(vm, val_int(n->as.lit.inum));
            return;
        case LIT_TRUE: vm_push(vm, val_bool(true)); return;
        case LIT_FALSE: vm_push(vm, val_bool(false)); return;
        case LIT_NULL: vm_push(vm, val_null()); return;
        case LIT_STR: {
            int len = 0;
            char *txt = unescape_literal(n->as.lit.text, n->as.lit.len, &len);
            vm_push(vm, make_string(vm, txt, (size_t)len));
            free(txt);
            return;
        }
    }
    return;
}

/* `m["k"]` / `l[0]`. The index expression is evaluated first and left on the
 * stack, then the container, so this reads like the two-argument `get()` it
 * replaces -- but strictly: a missing key or an out-of-range index is an
 * error, where `get()` returns null and `el()` takes a default. The strictness
 * is the point of having the syntax (SPEC 6.2).
 *
 * A negative index counts from the end, so `l[-1]` is the last element. That
 * is resolved here, once, rather than in every accessor the two native backends
 * end up calling -- they normalise the same way. */
static void eval_expr_index(VM *vm, Node *n, Env *env) {
    eval_expr(vm, n->as.index.index, env);
    if (vm->error) return;
    eval_expr(vm, n->as.index.obj, env);
    if (vm->error) return;

    Value objv = vm_pop(vm);      /* container */
    Value ixv  = vm_pop(vm);      /* index */
    Value out  = val_null();

    if (!IS_OBJ(objv)) {
        vm_set_error(vm, "cannot index into a non-container value");
        return;
    }
    Obj *o = AS_OBJ(objv);

    if (o->type == OBJ_MAP) {
        if (!IS_OBJ(ixv) || AS_OBJ(ixv)->type != OBJ_STRING) {
            vm_set_error(vm, "a map key must be a string");
            return;
        }
        const char *key = obj_string(AS_OBJ(ixv));
        int found = 0;
        Value v = map_get(vm, o, key, &found);
        if (!found) {
            /* One shared message across backends: the native emitters report
             * "index read failed" (codegen_expr.c), so try() captures an
             * identical string everywhere. The interpreter used to name the
             * key here; the detail stays in the map for diagnostics. */
            vm_set_error(vm, "index read failed");
            return;
        }
        out = v;
    } else if (o->type == OBJ_LIST) {
        if (!IS_NUM(ixv)) {
            vm_set_error(vm, "a list index must be an int");
            return;
        }
        long i = (long)AS_NUM(ixv);
        if (i < 0) i += o->as.list.count;   /* -1 is the last element */
        if (i < 0 || i >= o->as.list.count) {
            vm_set_error(vm, "index read failed");
            return;
        }
        out = o->as.list.items[i];
    } else {
        vm_set_error(vm, "cannot index into this value");
        return;
    }
    vm_push(vm, out);
}

static void eval_expr_member(VM *vm, Node *n, Env *env) {
    eval_expr(vm, n->as.member.obj, env);
    if (vm->error) return;
    Value objv = vm_peek(vm, 0);
    if (IS_OBJ(objv)) {
        Obj *o = AS_OBJ(objv);
        if (o->type == OBJ_MAP) {
            /* An anonymous map answers `.len` / `.length` with its key count.
             * The list and the string below do the same, and both native
             * backends already route `.len` to lume_map_len — the interpreter
             * must not be the one leg that refuses it. */
            if (!o->sname &&
                (strcmp(n->as.member.name, "len") == 0 ||
                 strcmp(n->as.member.name, "length") == 0)) {
                vm_pop(vm);
                vm_push(vm, val_int((long long)o->as.map.count));
                return;
            }
            /* A map carrying a static struct name (sname, set when a literal
             * is bound to a named type) is not a bare bag of keys: `.len` is
             * the field the user declared, which is what both native backends
             * read. The lookup below answers it, and a struct with no such
             * field fails the way the compiler fails — it must not quietly
             * hand back a member count. */
            int found = 0;
            Value v = map_get(vm, o, n->as.member.name, &found);
            if (!found) {
                /* Member type is known statically (tool handler args
                 * typed by the schema): a missing key reads as the
                 * type's zero value instead of failing. */
                if (n->as.member.type) {
                    Value z = vm_zero_value(vm, n->as.member.type);
                    vm_pop(vm);
                    vm_push(vm, z);
                    return;
                }
                vm_set_error(vm, "line %zu: map has no field '%s'", n->line, n->as.member.name);
                return;
            }
            vm_pop(vm);
            vm_push(vm, v);
            return;
        }
        if (o->type == OBJ_STRING &&
            (strcmp(n->as.member.name, "len") == 0 ||
             strcmp(n->as.member.name, "length") == 0)) {
            vm_pop(vm);
            vm_push(vm, val_int((long long)o->as.str.len));
            return;
        }
        if (o->type == OBJ_LIST &&
            (strcmp(n->as.member.name, "len") == 0 ||
             strcmp(n->as.member.name, "length") == 0)) {
            vm_pop(vm);
            vm_push(vm, val_int(o->as.list.count));
            return;
        }
        if (o->type == OBJ_ENV) {
            /* `import "x" as ns` — read a module export */
            Env *e = (Env *)o;
            int found = 0;
            Value v = env_get(e, n->as.member.name, &found);
            if (!found) {
                vm_set_error(vm, "line %zu: module has no export '%s'",
                             n->line, n->as.member.name);
                return;
            }
            vm_pop(vm);
            vm_push(vm, v);
            return;
        }
    }
    vm_set_error(vm, "line %zu: cannot read field '%s' on this value",
                 n->line, n->as.member.name);
    return;
}

static void eval_expr_binary(VM *vm, Node *n, Env *env) {
    Op op = n->as.binary.op;
    if (op == OP_AND || op == OP_OR) {
        /* short-circuit: never evaluate the right operand unless the
         * left decides the result (b != 0 and a / b > 1 must not
         * divide by zero when b == 0) */
        eval_expr(vm, n->as.binary.left, env);
        if (vm->error) return;
        bool lt = value_truthy(vm_pop(vm));
        if ((op == OP_AND && !lt) || (op == OP_OR && lt)) {
            vm_push(vm, val_bool(lt));
            return;
        }
        eval_expr(vm, n->as.binary.right, env);
        if (vm->error) return;
        bool rt = value_truthy(vm_pop(vm));
        vm_push(vm, val_bool(op == OP_AND ? (lt && rt) : (lt || rt)));
        return;
    }
    eval_expr(vm, n->as.binary.left, env);
    if (vm->error) return;
    eval_expr(vm, n->as.binary.right, env);
    if (vm->error) return;
    Value r = vm_pop(vm);
    Value l = vm_pop(vm);

    if (n->as.binary.op == OP_EQ || n->as.binary.op == OP_NE) {
        bool eq = values_equal(l, r);
        vm_push(vm, val_bool(n->as.binary.op == OP_EQ ? eq : !eq));
        return;
    }

    /* string concatenation with + */
    if (n->as.binary.op == OP_ADD &&
        IS_OBJ(l) && AS_OBJ(l)->type == OBJ_STRING &&
        IS_OBJ(r) && AS_OBJ(r)->type == OBJ_STRING) {
        Obj *a = AS_OBJ(l), *b = AS_OBJ(r);
        size_t total = a->as.str.len + b->as.str.len;
        char *buf = malloc(total + 1);
        /* malloc is unchecked two lines below this; on a NULL return the
         * terminator write would land on NULL. */
        if (!buf) { vm_set_error(vm, "out of memory"); return; }
        if (total) {
            memcpy(buf, obj_string(a), a->as.str.len);
            memcpy(buf + a->as.str.len, obj_string(b), b->as.str.len);
        }
        buf[total] = '\0';
        Value s = make_string(vm, buf, total);
        free(buf);
        vm_push(vm, s);
        return;
    }

    if (!IS_NUM(l) || !IS_NUM(r)) {
        vm_set_error(vm, "line %zu: operator needs numbers", n->line);
        return;
    }
    /* int op int is i64 arithmetic, wrapping like the native backends'
     * unadorned `add i64` (docs/SPEC.md §integers). Everything else promotes
     * to double. This must not be reordered into a single double path: doing
     * so is what made `m + 1` saturate in the interpreter while the native
     * backends wrapped, i.e. the same program meant two different numbers.
     * `/` and `%` promote even for two ints, because `/` is documented as
     * floating-point division (7 / 2 == 3.5) and codegen widens to match. */
    if (IS_INT(l) && IS_INT(r) &&
        n->as.binary.op != OP_DIV && n->as.binary.op != OP_MOD) {
        long long a = AS_INT(l), b = AS_INT(r);
        switch (n->as.binary.op) {
            case OP_ADD: vm_push(vm, val_int((long long)((unsigned long long)a + (unsigned long long)b))); return;
            case OP_SUB: vm_push(vm, val_int((long long)((unsigned long long)a - (unsigned long long)b))); return;
            case OP_MUL: vm_push(vm, val_int((long long)((unsigned long long)a * (unsigned long long)b))); return;
            case OP_LT:  vm_push(vm, val_bool(a <  b)); return;
            case OP_LE:  vm_push(vm, val_bool(a <= b)); return;
            case OP_GT:  vm_push(vm, val_bool(a >  b)); return;
            case OP_GE:  vm_push(vm, val_bool(a >= b)); return;
            default: vm_push(vm, val_null()); return;
        }
    }
    double a = AS_NUM(l), b = AS_NUM(r);
    switch (n->as.binary.op) {
        case OP_ADD: vm_push(vm, val_float(a + b)); return;
        case OP_SUB: vm_push(vm, val_float(a - b)); return;
        case OP_MUL: vm_push(vm, val_float(a * b)); return;
        case OP_DIV:
            if (b == 0) { vm_set_error(vm, "division by zero"); return; }
            vm_push(vm, val_float(a / b));
            return;
        case OP_MOD:
            if (b == 0) { vm_set_error(vm, "modulo by zero"); return; }
            /* Integer modulo stays integer (and keeps C's sign convention:
             * the result takes the dividend's sign) so that `n % 2` on a large
             * int is not routed through fmod's double. Computed from AS_INT,
             * not the widened a/b — a % b on doubles would be UB for values
             * that are not exactly representable.
             *
             * Both emitters reject `%` on floats outright
             * ("'%' does not apply to floats", codegen_expr.c / llvm_codegen.c),
             * so the interpreter must refuse it too rather than answer with
             * fmod: the three have to agree byte-for-byte (docs/SPEC.md §6). */
            if (IS_INT(l) && IS_INT(r)) {
                long long ia = AS_INT(l), ib = AS_INT(r);
                vm_push(vm, val_int(ia % ib));
            } else {
                vm_set_error(vm, "line %zu: '%%' does not apply to floats",
                             n->line);
                return;
            }
            return;
        case OP_LT: vm_push(vm, val_bool(a < b)); return;
        case OP_LE: vm_push(vm, val_bool(a <= b)); return;
        case OP_GT: vm_push(vm, val_bool(a > b)); return;
        case OP_GE: vm_push(vm, val_bool(a >= b)); return;
        default: vm_push(vm, val_null()); return;
    }
}

static void eval_expr_call(VM *vm, Node *n, Env *env) {
    eval_expr(vm, n->as.call.callee, env);
    if (vm->error) return;
    for (int i = 0; i < n->as.call.argc; i++) {
        eval_expr(vm, n->as.call.args[i], env);
        if (vm->error) return;
    }
    /* stack now: [callee, a0..an-1] */
    Value callee = vm_peek(vm, n->as.call.argc);
    if (!IS_OBJ(callee) ||
        (AS_OBJ(callee)->type != OBJ_FUNC && AS_OBJ(callee)->type != OBJ_NATIVE)) {
        vm_set_error(vm, "line %zu: trying to call a non-function", n->line);
        return;
    }
    call_function(vm, callee, n->as.call.argc);
    if (vm->error) return;
    if (!n->as.call.propagate) return;
    /* `?` — unwrap a Result: `{ err: e }` propagates up like a
     * return; `{ ok: v }` becomes the expression's value. */
    Value r = vm_peek(vm, 0);
    if (IS_OBJ(r) && AS_OBJ(r)->type == OBJ_MAP) {
        Obj *res = AS_OBJ(r);
        int found = 0;
        map_get(vm, res, "err", &found);
        if (found) {
            if (vm->jump_depth <= 0) {
                vm_set_error(vm,
                    "line %zu: '?' with an error outside a function",
                    n->line);
                return;
            }
            vm->call_result = r;
            longjmp(vm->jump_bufs[vm->jump_depth - 1], 1);
        }
        Value o = map_get(vm, res, "ok", &found);
        if (found) {
            vm_pop(vm);
            vm_push(vm, o);
        }
        /* neither key: not a Result — no-op (loose callee) */
    }
    return;
}

/* ---------- statements ---------- */

static void exec_statement(VM *vm, Node *n, Env *env) {
    if (vm->error) return;
    switch (n->type) {
        case N_BLOCK:
            for (int i = 0; i < n->as.block.count && !vm->error; i++) {
                exec_statement(vm, n->as.block.stmts[i], env);
                if (vm->loop_break || vm->loop_continue) break;
            }
            return;
        case N_LET: {
            eval_expr(vm, n->as.let.init, env);
            if (vm->error) return;
            Value v = vm_pop(vm);
            /* Keep the named struct type on the object when a map literal is
             * bound to it: `.len` then reads the declared field (which is
             * what the compiler emits) instead of answering with the key
             * count, so the interpreter and the native backends agree. */
            Type *at = n->as.let.annot;
            if (at && at->kind == TY_STRUCT && at->name &&
                IS_OBJ(v) && AS_OBJ(v)->type == OBJ_MAP)
                AS_OBJ(v)->sname = at->name;
            env_set(vm, env, n->as.let.name, v);
            if (n->is_export && vm->export_env)
                env_set(vm, vm->export_env, n->as.let.name, v);
            return;
        }
        case N_LET_PAT: {
            /* `let { a, b } = e;` — one evaluation of e, then every name
             * bound to e's like-named key in the CURRENT env (if-let's
             * binds live in a child env; this is a let, so it stays).
             * A missing key reads as the stamped type's zero value when
             * the checker knew the shape — the same fallback m.x uses,
             * and the same value the native getters' zero default gives —
             * and fails on a dynamic map, where nothing is known. */
            eval_expr(vm, n->as.letpat.init, env);
            if (vm->error) return;
            Value v = vm_pop(vm);
            if (!IS_OBJ(v) || AS_OBJ(v)->type != OBJ_MAP) {
                vm_set_error(vm, "line %zu: destructuring needs a map value",
                             n->line);
                return;
            }
            Obj *m = AS_OBJ(v);
            /* Root the map across the loop: env_set (and a zero value's
             * allocation) can collect, and every bound value either lives
             * inside m or is kept on the stack around its own env_set. */
            vm_push(vm, v);
            for (int i = 0; i < n->as.letpat.count; i++) {
                int found = 0;
                Value fv = map_get(vm, m, n->as.letpat.fields[i], &found);
                if (!found) {
                    Type *vt = n->as.letpat.types[i];
                    if (!vt) {
                        vm_set_error(vm, "line %zu: map has no field '%s'",
                                     n->line, n->as.letpat.fields[i]);
                        return;
                    }
                    fv = vm_zero_value(vm, vt);
                    vm_push(vm, fv);   /* root the fresh zero value */
                }
                env_set(vm, env, n->as.letpat.names[i], fv);
                if (n->is_export && vm->export_env)
                    env_set(vm, vm->export_env, n->as.letpat.names[i], fv);
                if (!found) vm_pop(vm);
            }
            vm_pop(vm);
            return;
        }
        case N_IF: {
            eval_expr(vm, n->as.ifs.cond, env);
            if (vm->error) return;
            if (value_truthy(vm_pop(vm))) exec_statement(vm, n->as.ifs.then, env);
            else if (n->as.ifs.els) exec_statement(vm, n->as.ifs.els, env);
            return;
        }
        case N_IF_LET: {
            /* `if let P [| P] = value { then } [else ...]` — a match with one
             * arm and no exhaustiveness requirement. Binds live in a child
             * env around the then-branch only (same scoping as a match arm);
             * the value stays rooted on the stack while the env is built. */
            eval_expr(vm, n->as.iflet.value, env);
            if (vm->error) return;
            Value vv = vm_pop(vm);
            vm_push(vm, vv);
            for (int pi = 0; pi < n->as.iflet.patc; pi++) {
                MatchPat *pat = &n->as.iflet.pats[pi];
                const char *tag = NULL;
                if (IS_OBJ(vv) && AS_OBJ(vv)->type == OBJ_MAP) {
                    int found = 0;
                    Value tagv = map_get(vm, AS_OBJ(vv), "$", &found);
                    if (found && IS_OBJ(tagv) &&
                        AS_OBJ(tagv)->type == OBJ_STRING)
                        tag = obj_string(AS_OBJ(tagv));
                }
                bool hit = tag && (strcmp(pat->vname, "_") == 0 ||
                                   strcmp(pat->vname, tag) == 0);
                if (!hit) continue;
                Value envv = make_env(vm, env);
                Env *benv = (Env *)AS_OBJ(envv);
                for (int k = 0; k < pat->bindc; k++) {
                    int f2 = 0;
                    Value pv = map_get(vm, AS_OBJ(vv), pat->vfields[k], &f2);
                    env_set(vm, benv, pat->binds[k], f2 ? pv : val_null());
                }
                benv->next_active = vm->active_envs;
                vm->active_envs = benv;
                vm_pop(vm); /* drop the value root; the binds hold their own */
                exec_statement(vm, n->as.iflet.then, benv);
                vm->active_envs = benv->next_active;
                return;
            }
            vm_pop(vm); /* no pattern matched: drop the value root */
            if (n->as.iflet.els) exec_statement(vm, n->as.iflet.els, env);
            return;
        }
        case N_WHILE: {
            bool saved_brk = vm->loop_break, saved_cont = vm->loop_continue;
            vm->loop_break = vm->loop_continue = false;
            while (!vm->error) {
                eval_expr(vm, n->as.whiles.cond, env);
                if (vm->error) break;
                if (!value_truthy(vm_pop(vm))) break;
                exec_statement(vm, n->as.whiles.body, env);
                if (vm->error) break;
                if (vm->loop_break) { vm->loop_break = false; break; }
                if (vm->loop_continue) vm->loop_continue = false;
            }
            vm->loop_break = saved_brk;
            vm->loop_continue = saved_cont;
            return;
        }
        case N_FOR:
            exec_statement_for(vm, n, env);
            return;
        case N_BREAK:
            vm->loop_break = true;
            return;
        case N_CONTINUE:
            vm->loop_continue = true;
            return;
        case N_EXPR_STMT:
            eval_expr(vm, n->as.expr_stmt.expr, env);
            if (!vm->error) vm_pop(vm);
            return;
        case N_RETURN: {
            if (n->as.ret.expr) {
                eval_expr(vm, n->as.ret.expr, env);
                if (vm->error) return;
                vm->call_result = vm_pop(vm);
            } else {
                vm->call_result = val_null();
            }
            if (vm->jump_depth <= 0) {
                vm_set_error(vm, "line %zu: return outside a function", n->line);
                return;
            }
            longjmp(vm->jump_bufs[vm->jump_depth - 1], 1);
            break; /* unreachable */
        }
        case N_FUNC_DECL: {
            /* A local function was hoisted by lume_resolve_localfns: its call
             * sites inside the enclosing function spell the synthetic cname,
             * so the binding must land under the same name (and the frame
             * hoist has usually bound it already — this re-set is harmless).
             * A local is never exported, so the export path only fires for
             * top-level declarations, whose cname is NULL. */
            const char *fname = n->as.func.cname ? n->as.func.cname
                                                 : n->as.func.name;
            Value f = make_func(vm, fname, n->as.func.names,
                                n->as.func.arity, n->as.func.body, env);
            env_set(vm, env, fname, f);
            if (n->is_export && vm->export_env)
                env_set(vm, vm->export_env, fname, f);
            return;
        }
        case N_TYPE_DECL:
        case N_ENUM_DECL:
            /* type/enum declarations are checked statically, then vanish */
            return;
        case N_SERVER: {
            if (!vm->server_config)
                vm->server_config = AS_OBJ(make_map(vm));
            Obj *cfg = vm->server_config;
            for (int i = 0; i < n->as.server.count; i++) {
                Node *a = n->as.server.assigns[i];
                eval_expr(vm, a->as.assign.value, env);
                if (vm->error) return;
                Value v = vm_pop(vm);
                map_set(vm, cfg, a->as.assign.name, v);
            }
            return;
        }
        case N_ROUTE:
            exec_statement_route(vm, n, env);
            return;
        case N_VERBS:
            exec_statement_verbs(vm, n, env);
            return;
        case N_TOOL:
            exec_statement_tool(vm, n, env);
            return;
        case N_IMPORT: {
            /* `import "x.lume" as ns;` — the loader has already executed this
             * module's dependencies and rewritten the path to the canonical
             * form; bind ns to the dependency's export table. */
            Module *m = loader_find(vm, n->as.imp.path);
            if (!m || !m->exports) {
                vm_set_error(vm, "line %zu: module '%s' not loaded",
                             n->line, n->as.imp.path);
                return;
            }
            env_set(vm, env, n->as.imp.ns, val_obj((Obj *)m->exports));
            return;
        }
        default:
            vm_set_error(vm, "line %zu: internal error (stmt node %d)", n->line, (int)n->type);
            return;
    }
}

/* ---------- statement cases, one function each ---------- */

static void exec_statement_for(VM *vm, Node *n, Env *env) {
    bool saved_brk = vm->loop_break, saved_cont = vm->loop_continue;
    vm->loop_break = vm->loop_continue = false;
    if (n->as.fors.is_in) {
        /* for (x in xs) — iterate a list's items or a map's keys.
         * The iterable value stays ON the VM stack for the whole loop:
         * the stack is a GC root and the iterable is the loop's only
         * reference to the list/map object. The old code popped it
         * right after eval, so any allocation inside the body that
         * tripped a collection freed the object under the C-level
         * pointer — use-after-free on the next round (the inline-temp
         * shape behind TODO chinese/server.lume's crashes; a `let`
         * binding is safe because env keeps the object rooted).
         * Body statements are stack-balanced, so the slot stays valid. */
        eval_expr(vm, n->as.fors.iterable, env);
        if (vm->error) goto done;
        int it_slot = vm->stack_count - 1;   /* rooted under the body */
        Value it = vm->stack[it_slot];
        if (!IS_OBJ(it)) {
            vm_pop(vm);
            vm_set_error(vm, "line %zu: for-in expects a list or map", n->line);
            goto done;
        }
        Obj *o = AS_OBJ(it);
        int n_items = -1;
        if (o->type == OBJ_LIST) n_items = o->as.list.count;
        else if (o->type == OBJ_MAP) n_items = o->as.map.count;
        if (n_items < 0) {
            vm_pop(vm);
            vm_set_error(vm, "line %zu: for-in expects a list or map", n->line);
            goto done;
        }
        for (int i = 0; i < n_items && !vm->error; i++) {
            /* Defensive: bodies must be balanced, so stack[it_slot] is
             * the iterable. If a body ever leaks stack depth (an
             * interpreter bug), drop the surplus rather than reading a
             * shifted slot as the iterable. */
            if (vm->stack_count > it_slot + 1) vm->stack_count = it_slot + 1;
            Value item = (o->type == OBJ_LIST)
                             ? o->as.list.items[i]
                             : make_string_cstr(vm, o->as.map.keys[i]);
            env_set(vm, env, n->as.fors.var, item);
            exec_statement(vm, n->as.fors.body, env);
            if (vm->error) break;
            if (vm->loop_break) { vm->loop_break = false; break; }
            if (vm->loop_continue) vm->loop_continue = false;
        }
        vm_pop(vm);   /* drop the rooted iterable */
    } else {
        /* for (init; cond; incr) — C-style, all three optional */
        if (n->as.fors.init) {
            exec_statement(vm, n->as.fors.init, env);
            if (vm->error) goto done;
        }
        while (!vm->error) {
            if (n->as.fors.cond) {
                eval_expr(vm, n->as.fors.cond, env);
                if (vm->error) goto done;
                if (!value_truthy(vm_pop(vm))) break;
            }
            exec_statement(vm, n->as.fors.body, env);
            if (vm->error) goto done;
            if (vm->loop_break) { vm->loop_break = false; break; }
            if (vm->loop_continue) vm->loop_continue = false;
            if (n->as.fors.incr) {
                exec_statement(vm, n->as.fors.incr, env);
                if (vm->error) goto done;
            }
        }
    }
done:
    vm->loop_break = saved_brk;
    vm->loop_continue = saved_cont;
}

static void exec_statement_route(VM *vm, Node *n, Env *env) {
    Value handler;
    if (n->as.route.handler) {
        eval_expr(vm, n->as.route.handler, env);
        if (vm->error) return;
        handler = vm_pop(vm);
    } else {
        handler = vm->default_handler; /* handler-less route */
    }
    if (n->as.route.alias) {
        /* `group "path", handler;` -> one route per method in the
         * group, all sharing the same handler. A list group registers
         * bare methods; a map group (method -> label) also attaches a
         * label that the shim exposes as req.label. */
        int found = 0;
        Value group = env_get(env, n->as.route.alias, &found);
        if (!found || !IS_OBJ(group) ||
            (AS_OBJ(group)->type != OBJ_LIST &&
             AS_OBJ(group)->type != OBJ_MAP)) {
            vm_set_error(vm, "line %zu: unknown verb group '%s' "
                         "(declare it with `verbs %s = [...];` first)",
                         n->line, n->as.route.alias, n->as.route.alias);
            return;
        }
        Obj *g = AS_OBJ(group);
        if (g->type == OBJ_LIST) {
            for (int i = 0; i < g->as.list.count; i++) {
                Value mv = g->as.list.items[i];
                if (!IS_OBJ(mv) || AS_OBJ(mv)->type != OBJ_STRING) {
                    vm_set_error(vm, "line %zu: verb group '%s' must contain "
                                 "only method strings",
                                 n->line, n->as.route.alias);
                    return;
                }
                const char *m = obj_string(AS_OBJ(mv));
                int rc = bridge_define_route(vm, m, n->as.route.path, handler,
                                             NULL);
                if (rc != 0) {
                    if (rc == -2)
                        vm_set_error(vm, "route table full (%d routes, max %d): "
                                     "cannot register %s %s",
                                     vm->route_count, MAX_AL_ROUTES, m,
                                     n->as.route.path);
                    else
                        vm_set_error(vm, "failed to register route %s %s", m,
                                     n->as.route.path);
                    return;
                }
            }
        } else {
            for (int i = 0; i < g->as.map.count; i++) {
                const char *m = g->as.map.keys[i];
                Value lv = g->as.map.vals[i];
                const char *label = NULL;
                if (IS_OBJ(lv) && AS_OBJ(lv)->type == OBJ_STRING) {
                    label = obj_string(AS_OBJ(lv));
                } else if (!IS_NULL(lv)) {
                    vm_set_error(vm, "line %zu: verb group '%s' label for "
                                 "%s must be a string",
                                 n->line, n->as.route.alias, m);
                    return;
                }
                int rc = bridge_define_route(vm, m, n->as.route.path, handler,
                                             label);
                if (rc != 0) {
                    if (rc == -2)
                        vm_set_error(vm, "route table full (%d routes, max %d): "
                                     "cannot register %s %s",
                                     vm->route_count, MAX_AL_ROUTES, m,
                                     n->as.route.path);
                    else
                        vm_set_error(vm, "failed to register route %s %s", m,
                                     n->as.route.path);
                    return;
                }
            }
        }
        return;
    }
    int rc = bridge_define_route(vm, n->as.route.method, n->as.route.path,
                                 handler, NULL);
    if (rc != 0) {
        if (rc == -2)
            vm_set_error(vm, "route table full (%d routes, max %d): cannot "
                         "register %s %s", vm->route_count, MAX_AL_ROUTES,
                         n->as.route.method, n->as.route.path);
        else
            vm_set_error(vm, "failed to register route %s %s",
                         n->as.route.method, n->as.route.path);
    }
}

static void exec_statement_verbs(VM *vm, Node *n, Env *env) {
    eval_expr(vm, n->as.verbs.methods, env);
    if (vm->error) return;
    Value methods = vm_pop(vm);
    if (!IS_OBJ(methods) ||
        (AS_OBJ(methods)->type != OBJ_LIST &&
         AS_OBJ(methods)->type != OBJ_MAP)) {
        vm_set_error(vm, "line %zu: `verbs %s` needs a list of methods or "
                     "a map of method -> label", n->line, n->as.verbs.name);
        return;
    }
    Obj *g = AS_OBJ(methods);
    if (g->type == OBJ_LIST) {
        for (int i = 0; i < g->as.list.count; i++) {
            Value mv = g->as.list.items[i];
            if (!IS_OBJ(mv) || AS_OBJ(mv)->type != OBJ_STRING) {
                vm_set_error(vm, "line %zu: `verbs %s` items must be strings",
                             n->line, n->as.verbs.name);
                return;
            }
        }
    } else {
        for (int i = 0; i < g->as.map.count; i++) {
            Value lv = g->as.map.vals[i];
            if (!IS_NULL(lv) &&
                !(IS_OBJ(lv) && AS_OBJ(lv)->type == OBJ_STRING)) {
                vm_set_error(vm, "line %zu: `verbs %s` label for %s must be "
                             "a string", n->line, n->as.verbs.name,
                             g->as.map.keys[i]);
                return;
            }
        }
    }
    env_set(vm, env, n->as.verbs.name, methods);
}

static void exec_statement_tool(VM *vm, Node *n, Env *env) {
    eval_expr(vm, n->as.tool.params, env);
    if (vm->error) return;
    Value params = vm_pop(vm);
    sbuf b = {0};
    if (IS_OBJ(params) && AS_OBJ(params)->type == OBJ_MAP) {
        /* { a: int } — a bare type keyword evaluates to the seeded
         * native/function; rewrite such values to their name string so
         * the schema serializes as { a: "int" }. */
        vm_push(vm, params);
        Obj *m = AS_OBJ(params);
        for (int i = 0; i < m->as.map.count; i++) {
            Value v = m->as.map.vals[i];
            if (IS_OBJ(v) && (AS_OBJ(v)->type == OBJ_NATIVE ||
                              AS_OBJ(v)->type == OBJ_FUNC)) {
                const char *nm = (AS_OBJ(v)->type == OBJ_NATIVE)
                                     ? AS_OBJ(v)->as.native.name
                                     : AS_OBJ(v)->as.fn.name;
                m->as.map.vals[i] = make_string_cstr(vm, nm ? nm : "");
            }
        }
        json_append_value(vm, &b, params);
        vm_pop(vm);
    } else {
        json_append_value(vm, &b, params);
    }
    eval_expr(vm, n->as.tool.handler, env);
    if (vm->error) { free(b.p); return; }
    Value handler = vm_pop(vm);
    if (bridge_define_tool(vm, n->as.tool.name, n->as.tool.desc,
                           b.p ? b.p : "", handler) != 0)
        vm_set_error(vm, "failed to register tool '%s'", n->as.tool.name);
    free(b.p); /* bridge_define_tool only snprintf'd a copy into ToolRec */
}

/* ---------- VM setup & top-level execution ---------- */

/* Built-in verb groups, so `write "/items", h;` needs no declaration.
 * `write` maps the write methods to conventional labels (surfaced as
 * `req.label`); `read` is a plain GET/HEAD method list. A user statement
 * `verbs write = ...;` / `verbs read = ...;` simply shadows either one. */
static void seed_verb_groups(VM *vm) {
    static const char *const w_methods[] = {"POST", "PUT", "PATCH", "DELETE"};
    static const char *const w_labels[]  = {"created", "replaced",
                                            "patched", "deleted"};
    static const char *const r_methods[] = {"GET", "HEAD"};

    Obj *w = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(w)); /* root while filling */
    for (int i = 0; i < 4; i++)
        map_set(vm, w, w_methods[i], make_string_cstr(vm, w_labels[i]));
    env_set(vm, vm->globals, "write", vm_peek(vm, 0));
    vm_pop(vm);

    Obj *r = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(r)); /* root while filling */
    for (int i = 0; i < 2; i++) {
        Value s = make_string_cstr(vm, r_methods[i]);
        if (r->as.list.count == r->as.list.cap) {
            r->as.list.cap = r->as.list.cap ? r->as.list.cap * 2 : 8;
            r->as.list.items = realloc(r->as.list.items,
                                       sizeof(Value) * (size_t)r->as.list.cap);
        }
        r->as.list.items[r->as.list.count++] = s;
    }
    env_set(vm, vm->globals, "read", vm_peek(vm, 0));
    vm_pop(vm);
}

/* http_get() 的实现在 builtins_http.c, 而它并不在所有下游里。下游换用
 * 本文件(而不是自己 pin 一份)时, 注册这一行会留下一个未定义的 b_http_get,
 * 链接直接失败。做成开关而不是硬注册, 下游就能用 -DLUME_HAS_HTTP=0 显式
 * 声明「我没有这个内建」—— 这样 host 的 lang/interp.c 才有脱离 host_owned
 * 的可能。默认开, 本树照旧。 */
#ifndef LUME_HAS_HTTP
#define LUME_HAS_HTTP 1
#endif

void bridge_seed_builtins(VM *vm) {
    struct { const char *name; NativeFn fn; } built[] = {
        {"run", b_run},
        {"print", b_print},
        {"str", b_str},
        {"int", b_int},
        {"float", b_float},
        {"bool", b_bool},
        {"string", b_string},
        {"replace", b_replace},
        {"upper", b_upper},
        {"lower", b_lower},
        {"capitalize", b_capitalize},
        {"trim", b_trim},
        {"contains", b_contains},
        {"split", b_split},
        {"join", b_join},
        {"substr", b_substr},
        {"crypt_sha512", b_crypt_sha512},
        {"sha256", b_sha256},
        {"hmac_sha256", b_hmac_sha256},
        {"base64_encode", b_base64_encode},
        {"base64_decode", b_base64_decode},
        {"ed25519_pubkey", b_ed25519_pubkey},
        {"ed25519_sign", b_ed25519_sign},
        {"ed25519_verify", b_ed25519_verify},
        {"ed25519_keypair", b_ed25519_keypair},
        {"len", b_len},
        {"keys", b_keys},
        {"get", b_get},
        {"range", b_range},
        {"map", b_map},
        {"filter", b_filter},
        {"reduce", b_reduce},
        /* SQLite (2026-10-12, builtins_sql.c): guarded SELECT / write with
         * ? binding; stubs fail clearly on builds without libsqlite3.
         * Transactions (2026-10-20, P6): first-class handles, GC-finalized
         * to rollback + close. */
        {"sql_query", b_sql_query},
        {"sql_write", b_sql_write},
        {"sql_tx_begin", b_sql_tx_begin},
        {"sql_tx_exec", b_sql_tx_exec},
        {"sql_tx_query", b_sql_tx_query},
        {"sql_tx_commit", b_sql_tx_commit},
        {"sql_tx_rollback", b_sql_tx_rollback},
        {"json", b_json},
        {"stringify", b_stringify},
        {"now", b_now},
        {"env", b_env},
        {"argv", b_argv},
        {"files", b_files},
        {"read_file", b_read_file},
        {"read_stdin", b_read_stdin},
        {"write_file", b_write_file},
        {"mkdir", b_mkdir},
        {"lock_file", b_lock_file},
        {"unlock_file", b_unlock_file},
        {"strftime", b_strftime},
        {"put", b_put},
        {"push", b_push}, /* DSL 层列表追加 (2026-09-27) */
        {"try", b_try}, /* 捕获 VM error -> {ok}/{err} (2026-09-27) */
        {"sort", b_sort}, /* in-place list sort, homogeneous numbers/strings (2026-10-11) */
        {"str_match", b_str_match}, /* POSIX ERE findall -> [{text,index,groups}] (2026-10-11) */
        {"regex", b_str_match}, /* alias of str_match (2026-10-11) */
        /* 量化 builtins (2026-10-24, builtins_math.c): 描述统计 /
         * 相关回归 / 时间序列,与原生后端同一套公式。 */
        {"mean", b_mean},
        {"variance", b_variance},
        {"stddev", b_stddev},
        {"median", b_median},
        {"percentile", b_percentile},
        {"covariance", b_covariance},
        {"correlation", b_correlation},
        {"linreg", b_linreg},
        {"pct_change", b_pct_change},
        {"max_drawdown", b_max_drawdown},
        {"skew", b_skew},
        {"kurtosis", b_kurtosis},
        {"rolling_mean", b_rolling_mean},
        {"rolling_stddev", b_rolling_stddev},
        {"ewma", b_ewma},
        /* 出站 HTTP (2026-10-04, builtins_http.c): raw socket, no libcurl.
         * 私有地址默认拒绝,--no-net / LUME_NO_NET=1 整体关掉。 */
#if LUME_HAS_HTTP
        {"http_get", b_http_get},
        {"http_post", b_http_post},
        {"http_put", b_http_put},
        {"http_patch", b_http_patch},
        {"http_delete", b_http_delete},
#endif
        /* 数值内建 (2026-09-28, builtins_math.c) */
        {"abs", b_abs},
        {"sqrt", b_sqrt},
        {"exp", b_exp},
        {"log", b_log},
        {"ln", b_ln},
        {"pow", b_pow},
        {"floor", b_floor},
        {"ceil", b_ceil},
        {"round", b_round},
        /* 三角函数 / 双曲 / 反三角 (信息计算科学扩展, 2026-10-31) */
        {"sin", b_sin},
        {"cos", b_cos},
        {"tan", b_tan},
        {"asin", b_asin},
        {"acos", b_acos},
        {"atan", b_atan},
        {"atan2", b_atan2},
        {"sinh", b_sinh},
        {"cosh", b_cosh},
        {"tanh", b_tanh},
        {"asinh", b_asinh},
        {"acosh", b_acosh},
        {"atanh", b_atanh},
        /* 统计分布内建 (2026-10-20, builtins_math.c + rt.c): normcdf/erf/
         * gamma/lgamma/igamma/ibeta/tdist_cdf/chisq_cdf 三后端一致。 */
        {"normcdf", b_normcdf},
        {"norminv", b_norminv},
        /* Monte Carlo RNG (2026-10-24, builtins_math.c): deterministic
         * PCG32, mirrors rt.c byte-for-byte. */
        {"seed", b_seed},
        {"rand", b_rand},
        {"rand_range", b_rand_range},
        {"rand_int", b_rand_int},
        {"rand_normal", b_rand_normal},
        {"rand_bool", b_rand_bool},
        {"rand_exponential", b_rand_exponential},
        {"rand_poisson", b_rand_poisson},
        {"shuffle", b_shuffle},
        {"rand_cauchy", b_rand_cauchy},
        {"rand_gamma", b_rand_gamma},
        {"rand_beta", b_rand_beta},
        {"rand_chisq", b_rand_chisq},
        {"rand_student_t", b_rand_student_t},
        {"rand_geometric", b_rand_geometric},
        {"rand_weibull", b_rand_weibull},
        {"rand_logistic", b_rand_logistic},
        {"rand_pareto", b_rand_pareto},
        {"rand_laplace", b_rand_laplace},
        {"rand_binomial", b_rand_binomial},
        {"rand_lognormal", b_rand_lognormal},
        {"rand_gumbel", b_rand_gumbel},
        {"rand_rayleigh", b_rand_rayleigh},
        {"rand_triangular", b_rand_triangular},
        {"rand_zipf", b_rand_zipf},
        {"rand_negative_binomial", b_rand_negative_binomial},
        {"rand_erlang", b_rand_erlang},
        {"rand_von_mises", b_rand_von_mises},
        {"rand_multinomial", b_rand_multinomial},
        {"sim_gbm", b_sim_gbm},
        {"sim_bridge", b_sim_bridge},
        {"sim_ou", b_sim_ou},
        {"sim_merton", b_sim_merton},
        {"sim_portfolio", b_sim_portfolio},
        {"rsi", b_rsi},
        {"macd", b_macd},
        {"bollinger", b_bollinger},
        {"rolling_max", b_rolling_max},
        {"rolling_min", b_rolling_min},
        {"kdj", b_kdj},
        {"williams_r", b_williams_r},
        {"atr", b_atr},
        {"obv", b_obv},
        {"alpha_beta", b_alpha_beta},
        {"information_ratio", b_information_ratio},
        {"calmar_ratio", b_calmar_ratio},
        {"autocorrelation", b_autocorrelation},
        {"treynor_ratio", b_treynor_ratio},
        {"omega_ratio", b_omega_ratio},
        {"rank", b_rank},
        {"spearman", b_spearman},
        {"ic", b_ic},
        {"annualized_return", b_annualized_return},
        {"annualized_volatility", b_annualized_volatility},
        {"sharpe_annualized", b_sharpe_annualized},
        {"cumulative_return", b_cumulative_return},
        {"semivariance", b_semivariance},
        {"downside_deviation", b_downside_deviation},
        {"param_var", b_param_var},
        {"kelly_fraction", b_kelly_fraction},
        {"profit_factor", b_profit_factor},
        {"win_rate", b_win_rate},
        {"avg_win", b_avg_win},
        {"avg_loss", b_avg_loss},
        {"max_consecutive_losses", b_max_consecutive_losses},
        {"bs_call", b_bs_call},
        {"bs_put", b_bs_put},
        {"bs_delta_call", b_bs_delta_call},
        {"bs_delta_put", b_bs_delta_put},
        {"bs_gamma", b_bs_gamma},
        {"bs_vega", b_bs_vega},
        {"z_test", b_z_test},
        {"z_pvalue", b_z_pvalue},
        {"tstat", b_tstat},
        {"ttest_p", b_ttest_p},
        {"chisq_stat", b_chisq_stat},
        {"chisq_p", b_chisq_p},
        {"f_stat", b_f_stat},
        {"f_pvalue", b_f_pvalue},
        {"rand_dirichlet", b_rand_dirichlet},
        {"rand_binormal", b_rand_binormal},
        {"rand_gauss_copula", b_rand_gauss_copula},
        {"rand_t_copula", b_rand_t_copula},
        {"bootstrap_mean_ci", b_bootstrap_mean_ci},
        {"bootstrap_median_ci", b_bootstrap_median_ci},
        {"bootstrap_stddev_ci", b_bootstrap_stddev_ci},
        {"bootstrap_se_mean", b_bootstrap_se_mean},
        {"value_at_risk", b_value_at_risk},
        {"expected_shortfall", b_expected_shortfall},
        {"sharpe_ratio", b_sharpe_ratio},
        {"sortino_ratio", b_sortino_ratio},
        {"rolling_sharpe", b_rolling_sharpe},
        {"rolling_sortino", b_rolling_sortino},
        {"rolling_var", b_rolling_var},
        {"rolling_cvar", b_rolling_cvar},
        {"erf", b_erf},
        {"gamma", b_gamma},
        {"lgamma", b_lgamma},
        {"igamma", b_igamma},
        {"ibeta", b_ibeta},
        {"tdist_cdf", b_tdist_cdf},
        {"chisq_cdf", b_chisq_cdf},
        {"min", b_min},
        {"max", b_max},
        {"tools", b_tools},
        {"skills", b_skills},
        {"mcps", b_mcps},
        {"discovery_endpoints", b_discovery_endpoints},
        {"catalog", b_catalog},
        {"el", b_el},
        {"render", b_render},
        {"html", b_html},
    };
    for (size_t i = 0; i < sizeof(built) / sizeof(built[0]); i++)
        env_set(vm, vm->globals, built[i].name,
                make_native(vm, built[i].name, built[i].fn));
    /* 数值常量（非函数；用户 let 同名可局部遮蔽，与内建一致） */
    env_set(vm, vm->globals, "pi", val_num(3.141592653589793));
    env_set(vm, vm->globals, "e", val_num(2.718281828459045));
    seed_verb_groups(vm);
    vm->default_handler = make_native(vm, "__default_route", b_default_route);
}

void vm_init(VM *vm) {
    memset(vm, 0, sizeof(*vm));
    vm->gc_threshold = 2 * 1024 * 1024;
    vm->globals = (Env *)AS_OBJ(make_env(vm, NULL));
    bridge_seed_builtins(vm);
}

/* Is this statement an *expression statement* that calls `main`? Only such a
 * statement is a program's status: the two native backends read main()'s value
 * off the same kind of call when they build their C entry point, so the
 * interpreter records it from the same place. */
static bool is_top_level_main_call(Node *n) {
    Node *e = n && n->type == N_EXPR_STMT ? n->as.expr_stmt.expr : NULL;
    if (!e || e->type != N_CALL || !e->as.call.callee ||
        e->as.call.callee->type != N_VAR)
        return false;
    const char *nm = e->as.call.callee->as.var.name;
    return nm && strcmp(nm, "main") == 0;
}

/* Evaluate a top-level `main()` and keep its value as the process status —
 * the statement itself would only pop it. TRUE when the statement was one, so
 * the caller can skip the ordinary statement path. */
static bool exec_top_main(VM *vm, Node *n, Env *env) {
    if (!is_top_level_main_call(n)) return false;
    eval_expr(vm, n->as.expr_stmt.expr, env);
    if (vm->error) return true;
    Value v = vm_pop(vm);
    vm->main_status = IS_NUM(v) ? (int)AS_NUM(v) : 0;
    return true;
}

void exec_program(VM *vm, Node *prog) {
    if (!prog || prog->type != N_PROGRAM) {
        vm_set_error(vm, "no program to run");
        return;
    }
    for (int i = 0; i < prog->as.program.count && !vm->error; i++) {
        Node *n = prog->as.program.stmts[i];
        if (exec_top_main(vm, n, vm->globals)) continue;
        exec_statement(vm, n, vm->globals);
    }
}

/* Execute one module's top level (loader.c calls this in dependency order,
 * exactly once per module). The module's own Env is temporarily the globals
 * (route/tool/server registrations and builtins all key off vm->globals), and
 * exported bindings also land in the module's export table. Restores the
 * caller's globals afterwards; the entry module's env stays as vm->globals. */
void exec_module_top(VM *vm, Module *m) {
    Env *saved_globals = vm->globals;
    Env *saved_exports = vm->export_env;
    vm->globals = m->env;
    vm->export_env = m->exports;
    for (int i = 0; i < m->prog->as.program.count && !vm->error; i++) {
        Node *n = m->prog->as.program.stmts[i];
        /* Only the entry module's main() is the process status; an imported
         * module that happens to call one is an ordinary call like any
         * other, and its value belongs to nobody. */
        if (m->is_entry && exec_top_main(vm, n, m->env)) continue;
        exec_statement(vm, n, m->env);
    }
    vm->export_env = saved_exports;
    vm->globals = saved_globals;
}
