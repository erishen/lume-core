/* Native builtin functions for Lume — split out of interp.c
 * (2026-09-25 refactor; behavior unchanged). The b_* wrappers are the
 * function pointers bridge_seed_builtins() registers; native_* are the
 * implementations. VM plumbing lives in interp.c.
 *
 * 2026-09-27: 按职责拆为 builtins_sql.c / builtins_fs.c /
 * builtins_catalog.c / builtins_hof.c;本文件保留基础 helper(去掉 static,
 * 见 builtins_internal.h)、通用原生函数与全部 b_* 导出包装。 */

#include "builtins_internal.h"
#include <time.h>   /* native_now: time() */

/* ---------- native builtins ---------- */

bool arg_string(VM *vm, Value v, const char **out) {
    if (!IS_OBJ(v) || AS_OBJ(v)->type != OBJ_STRING) {
        vm_set_error(vm, "expected string argument");
        return false;
    }
    *out = obj_string(AS_OBJ(v));
    return true;
}

static void native_print(VM *vm, int argc, Value *args, Value *out) {
    for (int i = 0; i < argc; i++) {
        if (i) printf(" ");
        if (IS_OBJ(args[i]) && AS_OBJ(args[i])->type == OBJ_STRING) {
            fwrite(obj_string(AS_OBJ(args[i])), 1, obj_string_len(AS_OBJ(args[i])), stdout);
        } else if (IS_NUM(args[i])) {
            double d = AS_NUM(args[i]);
            if (d == (long long)d)
                printf("%lld", (long long)d);
            else
                printf("%g", d);
        } else if (IS_BOOL(args[i]) || IS_NULL(args[i])) {
            printf(IS_BOOL(args[i]) ? (AS_BOOL(args[i]) ? "true" : "false") : "null");
        } else {
            sbuf b = {0};
            json_append_value(vm, &b, args[i]);
            printf("%s", b.p ? b.p : "");
            free(b.p); /* print() only reads it */
        }
    }
    printf("\n");
    fflush(stdout);
    *out = val_null();
}

void str_of_value(VM *vm, Value v, Value *out) {
    sbuf b = {0};
    if (IS_NUM(v)) {
        double d = AS_NUM(v);
        char buf[64];
        if (d == (long long)d)
            snprintf(buf, sizeof(buf), "%lld", (long long)d);
        else
            snprintf(buf, sizeof(buf), "%g", d);
        sb_str(&b, buf);
    } else if (IS_BOOL(v)) {
        sb_str(&b, AS_BOOL(v) ? "true" : "false");
    } else if (IS_NULL(v)) {
        sb_str(&b, "null");
    } else {
        json_append_value(vm, &b, v);
    }
    *out = make_string(vm, b.p ? b.p : "", b.len);
    /* sbuf hands its buffer to nobody: make_string copies out of b.p, so the
     * builder's own block has to go back here. Skipping this leaked 256 bytes
     * per str() call -- 5.1 MB across the smoke suite, i.e. 84% of everything
     * LeakSanitizer saw before this was fixed. b.len stays valid (it feeds
     * make_string above), only the pointer dies. */
    free(b.p);
}

/* When args = (map, string-key [, default]), resolve the field (default when
   missing) into *v and return 1; otherwise return 0. Lets casts double as
   safe accessors: int(m, "a") == int(get(m, "a")). */
int value_from_map(VM *vm, int argc, Value *args, Value *v) {
    if (argc < 2 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_MAP)
        return 0;
    if (!IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_STRING)
        return 0;
    int found = 0;
    Value g = map_get(vm, AS_OBJ(args[0]), obj_string(AS_OBJ(args[1])), &found);
    *v = found ? g : (argc >= 3 ? args[2] : val_null());
    return 1;
}

static void native_str(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "str() needs an argument"); return; }
    Value v;
    if (value_from_map(vm, argc, args, &v)) {
        if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING) { *out = v; return; }
        str_of_value(vm, v, out);
        return;
    }
    if (IS_OBJ(args[0]) && AS_OBJ(args[0])->type == OBJ_STRING) {
        *out = args[0];
        return;
    }
    str_of_value(vm, args[0], out);
}

static void native_int(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "int() needs an argument"); return; }
    Value v;
    if (value_from_map(vm, argc, args, &v)) {
        if (IS_NUM(v)) {
            *out = val_int((long long)AS_NUM(v));
        } else if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING) {
            /* 字符串转 int:非纯数字(如 "abc"、"12x")回落第三参默认值,
             * 而不是静默变 0——query 参数异常值应回落到脚本给的安全值。 */
            const char *s = obj_string(AS_OBJ(v));
            char *end = NULL;
            long long n = strtoll(s, &end, 10);
            if (end != s && *end == '\0')
                *out = val_int(n);
            else
                *out = (argc >= 3 ? args[2] : val_num(0));
        } else {
            *out = (argc >= 3 ? args[2] : val_num(0));
        }
        return;
    }
    if (IS_NUM(args[0])) {
        *out = val_int((long long)AS_NUM(args[0]));
    } else if (IS_OBJ(args[0]) && AS_OBJ(args[0])->type == OBJ_STRING) {
        *out = val_int(atoll(obj_string(AS_OBJ(args[0]))));
    } else {
        *out = val_num(0);
    }
}

static void native_float(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "float() needs an argument"); return; }
    Value v;
    if (value_from_map(vm, argc, args, &v)) {
        if (IS_NUM(v)) {
            *out = val_num(AS_NUM(v));
        } else if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING) {
            const char *s = obj_string(AS_OBJ(v));
            char *end = NULL;
            double d = strtod(s, &end);
            if (end != s && *end == '\0')
                *out = val_num(d);
            else
                *out = (argc >= 3 ? args[2] : val_num(0));
        } else {
            *out = (argc >= 3 ? args[2] : val_num(0));
        }
        return;
    }
    if (IS_NUM(args[0])) {
        *out = val_num(AS_NUM(args[0]));
    } else if (IS_OBJ(args[0]) && AS_OBJ(args[0])->type == OBJ_STRING) {
        *out = val_num(atof(obj_string(AS_OBJ(args[0]))));
    } else {
        *out = val_num(0);
    }
}

static void native_bool(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "bool() needs an argument"); return; }
    Value v;
    if (value_from_map(vm, argc, args, &v))
        *out = val_bool(value_truthy(v));
    else
        *out = val_bool(value_truthy(args[0]));
}

static void native_len(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "len() needs an argument"); return; }
    if (IS_OBJ(args[0])) {
        Obj *o = AS_OBJ(args[0]);
        if (o->type == OBJ_STRING) { *out = val_int((long long)o->as.str.len); return; }
        if (o->type == OBJ_LIST)   { *out = val_int(o->as.list.count); return; }
        if (o->type == OBJ_MAP)    { *out = val_int(o->as.map.count); return; }
    }
    *out = val_num(0);
}

static void native_keys(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_MAP) {
        vm_set_error(vm, "keys() expects a map");
        return;
    }
    Obj *m = AS_OBJ(args[0]);
    Obj *list = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj((Obj *)list)); /* root during construction */
    for (int i = 0; i < m->as.map.count; i++) {
        Value s = make_string_cstr(vm, m->as.map.keys[i]);
        vm_push(vm, s);
        if (list->as.list.count == list->as.list.cap) {
            list->as.list.cap = list->as.list.cap ? list->as.list.cap * 2 : 8;
            list->as.list.items = realloc(list->as.list.items,
                                          sizeof(Value) * (size_t)list->as.list.cap);
        }
        list->as.list.items[list->as.list.count++] = s;
        vm_pop(vm);
    }
    vm_pop(vm);
    *out = val_obj((Obj *)list);
}

static void native_get(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2 || !IS_OBJ(args[0])) {
        vm_set_error(vm, "get() expects a map or list and a key");
        return;
    }
    Obj *o = AS_OBJ(args[0]);
    if (o->type == OBJ_MAP) {
        const char *key = NULL;
        if (!arg_string(vm, args[1], &key)) return;
        int found = 0;
        Value v = map_get(vm, o, key, &found);
        *out = found ? v : (argc >= 3 ? args[2] : val_null());
    } else if (o->type == OBJ_LIST) {
        if (!IS_NUM(args[1])) {
            vm_set_error(vm, "list index must be a number");
            return;
        }
        int i = (int)AS_NUM(args[1]);
        *out = (i >= 0 && i < o->as.list.count) ? o->as.list.items[i]
                                                : (argc >= 3 ? args[2] : val_null());
    } else {
        vm_set_error(vm, "get() expects a map or list and a key");
    }
}

/* Default handler for handler-less routes (`write "/items";`): acknowledge the
 * request as { action: <label>, method: <method>, got: <body> }. `action` is
 * null for a route whose group carries no label. */
static void native_default_route(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_MAP) {
        vm_set_error(vm, "default route handler expects a request map");
        return;
    }
    Obj *req = AS_OBJ(args[0]);
    Obj *m = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(m)); /* root while filling */
    int found = 0;
    map_set(vm, m, "action", map_get(vm, req, "label", &found));
    map_set(vm, m, "method", map_get(vm, req, "method", &found));
    map_set(vm, m, "got", map_get(vm, req, "body", &found));
    *out = vm_pop(vm);
}

static void native_json(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "json() needs a string"); return; }
    const char *s = NULL;
    if (!arg_string(vm, args[0], &s)) return;
    char err[256] = {0};
    json_parse(vm, s, err, sizeof(err));
    if (vm->error) {
        if (err[0]) vm_set_error(vm, "%s", err);
        *out = val_null();
        return;
    }
    *out = vm_pop(vm); /* json_parse pushed the result above the args */
}

/* push(list, item) — DSL 层列表追加(之前 list_push 只在 C 内部可用,
 * 列表只能 map(fn, keys(m)) 现造, 是明显的人体工学缺口)。返回原列表。 */
static void native_push(VM *vm, int argc, Value *args, Value *out) {
    if (argc != 2) {
        vm_set_error(vm, "push(list, item) needs 2 arguments, got %d", argc);
        return;
    }
    if (!IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_LIST) {
        vm_set_error(vm, "push() first argument must be a list");
        return;
    }
    list_push(vm, AS_OBJ(args[0]), args[1]);
    *out = args[0];
}

/* try(func) — 捕获被调函数（func 字面量或 (…) => {…} 箭头函数）内部置起的
 * VM error。返回固定双键结构 { ok: 结果, err: null }（成功）或
 * { ok: null, err: 消息 }（失败）：成员访问 r.ok / r.err 永不因缺键抛
 * sticky error（m.xxx 缺键会报 "map has no field"），r.err == null 即成功。
 * 对未知来源的 map 仍建议用宽容的 get() 读取。json() 解析失败、类型错误等
 * 一律可接住; error 是 sticky 的, 这里按调用点显式清掉。 */
static void native_try(VM *vm, int argc, Value *args, Value *out) {
    if (argc != 1 || !IS_OBJ(args[0]) ||
        (AS_OBJ(args[0])->type != OBJ_FUNC && AS_OBJ(args[0])->type != OBJ_NATIVE)) {
        vm_set_error(vm, "try() needs a function");
        return;
    }
    Value callee = args[0];
    vm_push(vm, callee);
    call_function(vm, callee, 0);
    Value result = vm_pop(vm); /* 出错时 call_function 保证槽位上是 null */

    Obj *m = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj((Obj *)m)); /* root while filling */
    if (vm->error) {
        map_set(vm, m, "ok", val_null());
        map_set(vm, m, "err",
                make_string_cstr(vm, vm->error_msg[0] ? vm->error_msg : "error"));
        vm->error = false;
        vm->error_msg[0] = '\0';
    } else {
        map_set(vm, m, "ok", result);
        map_set(vm, m, "err", val_null());
    }
    *out = vm_pop(vm);
}

static void native_stringify(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "stringify() needs a value"); return; }
    sbuf b = {0};
    json_append_value(vm, &b, args[0]);
    *out = make_string(vm, b.p ? b.p : "", b.len);
    free(b.p); /* copied out above; the builder owns nothing downstream */
}
static void native_now(VM *vm, int argc, Value *args, Value *out) {
    (void)vm; (void)argc; (void)args;
    *out = val_int((long long)time(NULL));
}
Value vm_native(VM *vm, int argc, Value *args, Native2 impl) {
    Value out = val_null();
    impl(vm, argc, args, &out);
    return out;
}

Value b_run(VM *vm, int argc, Value *args) {
    (void)argc; (void)args;
    bridge_run(vm);
    return val_null();
}
Value b_print(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_print); }
Value b_str(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_str); }
Value b_int(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_int); }
Value b_float(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_float); }
Value b_bool(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_bool); }
Value b_string(VM *vm, int argc, Value *args)    { return vm_native(vm, argc, args, native_str); }
Value b_len(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_len); }
Value b_keys(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_keys); }
Value b_get(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_get); }
Value b_range(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_range); }
Value b_map(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_map); }
Value b_filter(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_filter); }
Value b_reduce(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_reduce); }
/* b_sql_query / b_sql_write are host-only (agent-httpd db layer); the sql_*
 * builtins are not registered in this tree. */
Value b_json(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_json); }
Value b_stringify(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_stringify); }
Value b_now(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_now); }
Value b_env(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_env); }
Value b_files(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_files); }
Value b_read_file(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_read_file); }
Value b_write_file(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_write_file); }
Value b_mkdir(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_mkdir); }
Value b_lock_file(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_lock_file); }
Value b_unlock_file(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_unlock_file); }
Value b_strftime(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_strftime); }
Value b_put(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_put); }
Value b_push(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_push); }
Value b_try(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_try); }
Value b_tools(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_tools); }
Value b_skills(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_skills); }
Value b_mcps(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_mcps); }
Value b_discovery_endpoints(VM *vm, int argc, Value *args)
{ return vm_native(vm, argc, args, native_discovery_endpoints); }
Value b_catalog(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_catalog); }
Value b_http_get(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_http_get); }
Value b_http_post(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_http_post); }
Value b_http_put(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_http_put); }
Value b_http_patch(VM *vm, int argc, Value *args){ return vm_native(vm, argc, args, native_http_patch); }
Value b_http_delete(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_http_delete); }
Value b_default_route(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_default_route); }
