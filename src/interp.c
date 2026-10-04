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

static char *unescape_literal(const char *s, int n, int *out_len) {
    /* s spans the raw lexeme INCLUDING the surrounding quotes. */
    char *buf = malloc((size_t)n + 1);
    int w = 0;
    for (int i = 1; i < n - 1; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < n - 1) {
            i++;
            switch (s[i]) {
                case 'n': buf[w++] = '\n'; break;
                case 't': buf[w++] = '\t'; break;
                case 'r': buf[w++] = '\r'; break;
                case '0': buf[w++] = '\0'; break;
                case '\\': buf[w++] = '\\'; break;
                case '"': buf[w++] = '"'; break;
                default: buf[w++] = s[i]; break;
            }
        } else {
            buf[w++] = c;
        }
    }
    buf[w] = '\0';
    *out_len = w;
    return buf;
}

#include "builtins.h"
/* ---------- function call engine ---------- */

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

    for (int i = 0; i < argc; i++)
        env_set(vm, frame, f->as.fn.params[i], vm->stack[slot + 1 + i]);
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
        case N_BINARY:
            eval_expr_binary(vm, n, env);
            return;
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
                vm_push(vm, val_num(-AS_NUM(v)));
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
        case LIT_NUM:  vm_push(vm, val_num(n->as.lit.num)); return;
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

static void eval_expr_member(VM *vm, Node *n, Env *env) {
    eval_expr(vm, n->as.member.obj, env);
    if (vm->error) return;
    Value objv = vm_peek(vm, 0);
    if (IS_OBJ(objv)) {
        Obj *o = AS_OBJ(objv);
        if (o->type == OBJ_MAP) {
            /* A map answers `.len` / `.length` with its key count. The list and
             * the string below do the same, and both native backends already
             * route `.len` to lume_map_len — the interpreter must not be the
             * one leg that refuses it. */
            if (strcmp(n->as.member.name, "len") == 0 ||
                strcmp(n->as.member.name, "length") == 0) {
                vm_pop(vm);
                vm_push(vm, val_int((long long)o->as.map.count));
                return;
            }
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
    double a = AS_NUM(l), b = AS_NUM(r);
    switch (n->as.binary.op) {
        case OP_ADD: vm_push(vm, val_num(a + b)); return;
        case OP_SUB: vm_push(vm, val_num(a - b)); return;
        case OP_MUL: vm_push(vm, val_num(a * b)); return;
        case OP_DIV:
            if (b == 0) { vm_set_error(vm, "division by zero"); return; }
            vm_push(vm, val_num(a / b));
            return;
        case OP_MOD:
            if (b == 0) { vm_set_error(vm, "modulo by zero"); return; }
            vm_push(vm, val_num(fmod(a, b)));
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
            env_set(vm, env, n->as.let.name, v);
            if (n->is_export && vm->export_env)
                env_set(vm, vm->export_env, n->as.let.name, v);
            return;
        }
        case N_IF: {
            eval_expr(vm, n->as.ifs.cond, env);
            if (vm->error) return;
            if (value_truthy(vm_pop(vm))) exec_statement(vm, n->as.ifs.then, env);
            else if (n->as.ifs.els) exec_statement(vm, n->as.ifs.els, env);
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
            Value f = make_func(vm, n->as.func.name, n->as.func.names,
                                n->as.func.arity, n->as.func.body, env);
            env_set(vm, env, n->as.func.name, f);
            if (n->is_export && vm->export_env)
                env_set(vm, vm->export_env, n->as.func.name, f);
            return;
        }
        case N_TYPE_DECL:
            /* type declarations are checked statically, then vanish */
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
        /* for (x in xs) — iterate a list's items or a map's keys */
        eval_expr(vm, n->as.fors.iterable, env);
        if (vm->error) goto done;
        Value it = vm_pop(vm);
        if (!IS_OBJ(it)) {
            vm_set_error(vm, "line %zu: for-in expects a list or map", n->line);
            goto done;
        }
        Obj *o = AS_OBJ(it);
        int n_items = -1;
        if (o->type == OBJ_LIST) n_items = o->as.list.count;
        else if (o->type == OBJ_MAP) n_items = o->as.map.count;
        if (n_items < 0) {
            vm_set_error(vm, "line %zu: for-in expects a list or map", n->line);
            goto done;
        }
        for (int i = 0; i < n_items && !vm->error; i++) {
            Value item = (o->type == OBJ_LIST)
                             ? o->as.list.items[i]
                             : make_string_cstr(vm, o->as.map.keys[i]);
            env_set(vm, env, n->as.fors.var, item);
            exec_statement(vm, n->as.fors.body, env);
            if (vm->error) break;
            if (vm->loop_break) { vm->loop_break = false; break; }
            if (vm->loop_continue) vm->loop_continue = false;
        }
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
                if (bridge_define_route(vm, m, n->as.route.path, handler,
                                        NULL) != 0) {
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
                if (bridge_define_route(vm, m, n->as.route.path, handler,
                                        label) != 0) {
                    vm_set_error(vm, "failed to register route %s %s", m,
                                 n->as.route.path);
                    return;
                }
            }
        }
        return;
    }
    if (bridge_define_route(vm, n->as.route.method, n->as.route.path, handler,
                            NULL) != 0)
        vm_set_error(vm, "failed to register route %s %s",
                     n->as.route.method, n->as.route.path);
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
        {"crypt_sha512", b_crypt_sha512},
        {"len", b_len},
        {"keys", b_keys},
        {"get", b_get},
        {"range", b_range},
        {"map", b_map},
        {"filter", b_filter},
        {"reduce", b_reduce},
        /* sql_query / sql_write: host build only. */
        {"json", b_json},
        {"stringify", b_stringify},
        {"now", b_now},
        {"env", b_env},
        {"files", b_files},
        {"read_file", b_read_file},
        {"write_file", b_write_file},
        {"mkdir", b_mkdir},
        {"lock_file", b_lock_file},
        {"unlock_file", b_unlock_file},
        {"strftime", b_strftime},
        {"put", b_put},
        {"push", b_push}, /* DSL 层列表追加 (2026-09-27) */
        {"try", b_try}, /* 捕获 VM error -> {ok}/{err} (2026-09-27) */
        /* 出站 HTTP (2026-10-04, builtins_http.c): raw socket, no libcurl.
         * 私有地址默认拒绝,--no-net / LUME_NO_NET=1 整体关掉。 */
        {"http_get", b_http_get},
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
