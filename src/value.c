#include "lume.h"

/* Object heap (mark-sweep GC), string/map/list/function values, environments,
 * and the JSON <-> Value bridge (tool arguments come in as JSON; tool results
 * and the `json()`/`stringify()` builtins go the other way).
 *
 * GC discipline: every evaluation leaves its result on the VM value stack, so
 * temporaries are always rooted; map_set/env_set re-root their inputs for the
 * duration of their own allocations. The heap is a singly-linked list of Obj
 * blocks; strings keep their bytes inline (Obj + payload). */

/* 返回类型必须是 char * 而不是 const char *:字符串字节内联在 Obj 后面,
 * 那一块是可写的(make_string 就地写 len 个字节再补一个 NUL)。早先写成
 * const 返回,唯一的写入点 make_string 就得靠两处强制转换绕过 —— -Wcast-qual
 * 直接报在上面,而且这种「为了过编译而加的转换」会掩盖真正的 const 违规:
 * 谁哪天真把某个不可变常量塞进 OBJ_STRING,那两处转换不会替他报错。 */
char *obj_string(Obj *o) { return (char *)(o + 1); }
size_t obj_string_len(Obj *o) { return o->as.str.len; }

/* ---------- GC ---------- */

Obj *gc_alloc(VM *vm, ObjType type, size_t extra) {
    size_t total = sizeof(Obj) + extra;
    vm->bytes_allocated += total;
    if (vm->bytes_allocated > vm->gc_threshold) gc_collect(vm);
    Obj *obj = calloc(1, total);
    obj->type = type;
    obj->next = vm->objs;
    vm->objs = obj;
    return obj;
}

char *gc_strdup(VM *vm, const char *s, size_t n) {
    char *copy = malloc(n + 1);
    if (n) memcpy(copy, s, n);
    copy[n] = '\0';
    if (vm->bytes_allocated > vm->gc_threshold) gc_collect(vm);
    return copy;
}

char *gc_cstr(VM *vm, const char *s) { return gc_strdup(vm, s, strlen(s)); }

/* ---------- marking ---------- */

static void mark_value(Value v);
static void mark_obj(Obj *o);
static void obj_release(Obj *o);

static void mark_value(Value v) {
    if (IS_OBJ(v)) mark_obj(AS_OBJ(v));
}

static void mark_obj(Obj *o) {
    if (!o || o->is_marked) return;
    o->is_marked = true;
    switch (o->type) {
        case OBJ_STRING:
        case OBJ_NATIVE:
            break;
        case OBJ_MAP:
            for (int i = 0; i < o->as.map.count; i++)
                mark_value(o->as.map.vals[i]);
            break;
        case OBJ_LIST:
            for (int i = 0; i < o->as.list.count; i++)
                mark_value(o->as.list.items[i]);
            break;
        case OBJ_FUNC:
            if (o->as.fn.closure) mark_obj((Obj *)o->as.fn.closure);
            break;
        case OBJ_ENV: {
            Env *e = (Env *)o;
            if (e->parent) mark_obj((Obj *)e->parent);
            if (e->next_active) mark_obj((Obj *)e->next_active);
            for (int i = 0; i < e->vars.count; i++)
                mark_value(e->vars.vals[i]);
            break;
        }
    }
}

void gc_collect(VM *vm) {
    /* Roots: value stack, global env, the dynamic env chain, bridge tables,
     * the server config map, and a pending `return` value. */
    for (int i = 0; i < vm->stack_count; i++) mark_value(vm->stack[i]);
    if (vm->globals) mark_obj((Obj *)vm->globals);
    for (Env *e = vm->active_envs; e; e = e->next_active) mark_obj((Obj *)e);
    if (vm->server_config) mark_obj(vm->server_config);
    if (vm->argv) mark_obj(vm->argv);       /* CLI script args (see VM.argv) */
    mark_value(vm->default_handler);
    for (int i = 0; i < vm->route_count; i++)
        mark_value(vm->routes[i].handler);
    for (int i = 0; i < vm->tool_count; i++)
        mark_value(vm->tool_records[i].handler);
    mark_value(vm->call_result);

    Obj **link = &vm->objs;
    while (*link) {
        Obj *o = *link;
        if (!o->is_marked) {
            *link = o->next;
            /* The heap's own strings (gc_cstr'd native/fn names, map and env
             * keys, list items) live on malloc rather than inline, so dropping
             * the header alone would strand them. Skipping this on the reclaim
             * path — and only doing it at teardown — leaked everything the
             * collector had swept over the whole run; only the check-only legs
             * exercised a big enough heap to show it under LSan. */
            obj_release(o);
            free(o);
        } else {
            o->is_marked = false;
            link = &o->next;
        }
    }
    vm->bytes_allocated = 0;
    if (vm->gc_threshold < (1u << 30)) vm->gc_threshold *= 2;
}

/* ---------- teardown ---------- */

/* Release whatever an Obj owns beyond itself. Byte payloads of strings are
 * inline (obj_string), and ObjFunc.params / ObjFunc.body point into the AST
 * (node_free's business), so this only covers the heap-side strings and
 * arrays: the gc_strdup'd keys in maps and envs, the key/value vectors, the
 * list vector, and the two gc_cstr'd name fields. */
static void obj_release(Obj *o) {
    switch (o->type) {
    case OBJ_STRING:
        break;              /* payload is inline after the header */
    case OBJ_NATIVE:
        free(o->as.native.name);
        break;
    case OBJ_FUNC:
        free(o->as.fn.name);
        break;              /* params[]/body are the AST's, not ours */
    case OBJ_LIST:
        free(o->as.list.items);
        break;
    case OBJ_MAP:
        for (int i = 0; i < o->as.map.count; i++) free(o->as.map.keys[i]);
        free(o->as.map.keys);
        free(o->as.map.vals);
        break;
    case OBJ_ENV: {
        Env *e = (Env *)o;
        for (int i = 0; i < e->vars.count; i++) free(e->vars.keys[i]);
        free(e->vars.keys);
        free(e->vars.vals);
        break;
    }
    }
}

void vm_free(VM *vm) {
    if (!vm) return; /* vm_free(NULL) is a no-op, so callers need no branch */
    /* Route records own three strdup'd strings each (bridge_define_route). */
    for (int i = 0; i < vm->route_count; i++) {
        free(vm->routes[i].method);
        free(vm->routes[i].path);
        free(vm->routes[i].label);
        vm->routes[i].method = NULL;
        vm->routes[i].path = NULL;
        vm->routes[i].label = NULL;
    }
    vm->route_count = 0;

    vm->globals = NULL;
    vm->active_envs = NULL;
    vm->server_config = NULL;
    vm->default_handler = val_null();
    vm->call_result = val_null();
    vm->stack_count = 0;

    /* The heap: everything still linked is, by definition, unreachable
     * (gc_collect() already reclaimed the rest), so the whole chain goes
     * without a mark pass. */
    Obj *o = vm->objs;
    vm->objs = NULL;
    while (o) {
        Obj *next = o->next;
        obj_release(o);
        free(o);
        o = next;
    }
    free(vm->load_stack);
    vm->load_stack = NULL;
    vm->load_depth = 0;
}

/* ---------- value constructors ---------- */

Value make_string(VM *vm, const char *s, size_t n) {
    Obj *obj = gc_alloc(vm, OBJ_STRING, n + 1);
    obj->as.str.len = n;
    if (n) memcpy(obj_string(obj), s, n);
    obj_string(obj)[n] = '\0';
    return val_obj(obj);
}

Value make_string_cstr(VM *vm, const char *s) {
    return make_string(vm, s, strlen(s));
}

Value make_map(VM *vm) { return val_obj(gc_alloc(vm, OBJ_MAP, sizeof(KVPair))); }
Value make_list(VM *vm) { return val_obj(gc_alloc(vm, OBJ_LIST, sizeof(ObjList))); }

Value make_func(VM *vm, const char *name, char **params, int arity,
                void *body, Env *closure) {
    Obj *o = gc_alloc(vm, OBJ_FUNC, sizeof(ObjFunc));
    o->as.fn.name = gc_cstr(vm, name);
    o->as.fn.params = params;
    o->as.fn.arity = arity;
    o->as.fn.body = body;
    o->as.fn.closure = closure;
    return val_obj(o);
}

Value make_native(VM *vm, const char *name, NativeFn fn) {
    Obj *o = gc_alloc(vm, OBJ_NATIVE, sizeof(ObjNative));
    o->as.native.name = gc_cstr(vm, name);
    o->as.native.fn = fn;
    return val_obj(o);
}

Value make_env(VM *vm, Env *parent) {
    Env *e = (Env *)gc_alloc(vm, OBJ_ENV, sizeof(Env) - sizeof(Obj));
    e->parent = parent;
    e->next_active = NULL;
    return val_obj((Obj *)e);
}

/* ---------- map / env access ---------- */

static int map_find(Obj *map, const char *key) {
    for (int i = 0; i < map->as.map.count; i++)
        if (strcmp(map->as.map.keys[i], key) == 0) return i;
    return -1;
}

Value map_get(VM *vm, Obj *map, const char *key, int *found) {
    (void)vm;
    int i = map_find(map, key);
    if (found) *found = (i >= 0);
    return (i >= 0) ? map->as.map.vals[i] : val_null();
}

/* map_set re-roots both inputs so a GC triggered by the key strdup can never
 * collect the map object or the stored value. */
void map_set(VM *vm, Obj *map, const char *key, Value v) {
    vm_push(vm, val_obj((Obj *)map));
    vm_push(vm, v);
    int i = map_find(map, key);
    if (i >= 0) {
        map->as.map.vals[i] = v;
    } else {
        if (map->as.map.count == map->as.map.cap) {
            map->as.map.cap = map->as.map.cap ? map->as.map.cap * 2 : 8;
            map->as.map.keys = realloc(map->as.map.keys,
                                       sizeof(char *) * (size_t)map->as.map.cap);
            map->as.map.vals = realloc(map->as.map.vals,
                                       sizeof(Value) * (size_t)map->as.map.cap);
        }
        map->as.map.keys[map->as.map.count] = gc_cstr(vm, key);
        map->as.map.vals[map->as.map.count] = v;
        map->as.map.count++;
    }
    vm_pop(vm);
    vm_pop(vm);
}

void env_set(VM *vm, Env *env, const char *name, Value v) {
    vm_push(vm, val_obj((Obj *)env));
    vm_push(vm, v);
    for (int i = 0; i < env->vars.count; i++) {
        if (strcmp(env->vars.keys[i], name) == 0) {
            env->vars.vals[i] = v;
            vm_pop(vm);
            vm_pop(vm);
            return;
        }
    }
    if (env->vars.count == env->vars.cap) {
        env->vars.cap = env->vars.cap ? env->vars.cap * 2 : 8;
        env->vars.keys = realloc(env->vars.keys,
                                 sizeof(char *) * (size_t)env->vars.cap);
        env->vars.vals = realloc(env->vars.vals,
                                 sizeof(Value) * (size_t)env->vars.cap);
    }
    env->vars.keys[env->vars.count] = gc_cstr(vm, name);
    env->vars.vals[env->vars.count] = v;
    env->vars.count++;
    vm_pop(vm);
    vm_pop(vm);
}

Value env_get(Env *env, const char *name, int *found) {
    for (Env *e = env; e; e = e->parent) {
        for (int i = 0; i < e->vars.count; i++) {
            if (strcmp(e->vars.keys[i], name) == 0) {
                if (found) *found = 1;
                return e->vars.vals[i];
            }
        }
    }
    if (found) *found = 0;
    return val_null();
}

/* ---------- truthiness / numbers ---------- */

bool value_truthy(Value v) {
    if (IS_NULL(v)) return false;
    if (IS_BOOL(v)) return AS_BOOL(v);
    if (IS_NUM(v)) return AS_NUM(v) != 0.0;
    if (IS_OBJ(v)) {
        Obj *o = AS_OBJ(v);
        if (o->type == OBJ_STRING) return o->as.str.len > 0;
        if (o->type == OBJ_LIST) return o->as.list.count > 0;
        if (o->type == OBJ_MAP) return o->as.map.count > 0;
        return true;
    }
    return false;
}

static bool value_eq(Value a, Value b) {
    /* int and float are distinct runtime tags but comparable values: the type
     * checker widens int to float (type_compat), so `1 == 1.0` has to stay
     * true. Compare on the widest common representation instead of bailing on
     * the tag mismatch — but keep two ints exact rather than dragging them
     * through double, which is the whole point of the i64 split. */
    if (IS_NUM(a) && IS_NUM(b)) {
        if (IS_INT(a) && IS_INT(b)) return AS_INT(a) == AS_INT(b);
        return AS_NUM(a) == AS_NUM(b);
    }
    if (a.type != b.type) return false;
    switch (a.type) {
        case VAL_NULL: return true;
        case VAL_BOOL: return AS_BOOL(a) == AS_BOOL(b);
        case VAL_INT: return AS_INT(a) == AS_INT(b);
        case VAL_FLOAT: return AS_NUM(a) == AS_NUM(b);
        case VAL_OBJ:
            if (AS_OBJ(a) == AS_OBJ(b)) return true;
            if (AS_OBJ(a)->type == OBJ_STRING && AS_OBJ(b)->type == OBJ_STRING) {
                Obj *x = AS_OBJ(a), *y = AS_OBJ(b);
                return x->as.str.len == y->as.str.len &&
                       memcmp(obj_string(x), obj_string(y), x->as.str.len) == 0;
            }
            return false;
    }
    return false;
}

bool values_equal(Value a, Value b) { return value_eq(a, b); }

/* ---------- JSON encoding (Value -> sbuf) ---------- */

static void json_escape(void *sbufp, const char *s, size_t n) {
    sbuf *b = sbufp;
    sb_chr(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': sb_str(b, "\\\""); break;
            case '\\': sb_str(b, "\\\\"); break;
            case '\n': sb_str(b, "\\n"); break;
            case '\t': sb_str(b, "\\t"); break;
            case '\r': sb_str(b, "\\r"); break;
            case '\b': sb_str(b, "\\b"); break;
            case '\f': sb_str(b, "\\f"); break;
            default:
                if (c < 0x20) {
                    char esc[7];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    sb_str(b, esc);
                } else {
                    sb_chr(b, (char)c);
                }
        }
    }
    sb_chr(b, '"');
}

void json_append_value(VM *vm, void *sbufp, Value v) {
    sbuf *b = sbufp;
    if (IS_NULL(v)) { sb_str(b, "null"); return; }
    if (IS_BOOL(v)) { sb_str(b, AS_BOOL(v) ? "true" : "false"); return; }
    if (IS_NUM(v)) {
        char buf[64];
        /* JSON keeps the exact integer too — an int serialized through a
         * double would round past 2^53, so json() of a large id would not
         * round-trip. */
        if (IS_INT(v))
            snprintf(buf, sizeof(buf), "%lld", AS_INT(v));
        else
            snprintf(buf, sizeof(buf), "%g", AS_NUM(v));
        sb_str(b, buf);
        return;
    }
    Obj *o = AS_OBJ(v);
    switch (o->type) {
        case OBJ_STRING:
            json_escape(b, obj_string(o), obj_string_len(o));
            break;
        case OBJ_MAP:
            sb_chr(b, '{');
            for (int i = 0; i < o->as.map.count; i++) {
                if (i) sb_chr(b, ',');
                json_escape(b, o->as.map.keys[i], strlen(o->as.map.keys[i]));
                sb_chr(b, ':');
                json_append_value(vm, b, o->as.map.vals[i]);
            }
            sb_chr(b, '}');
            break;
        case OBJ_LIST:
            sb_chr(b, '[');
            for (int i = 0; i < o->as.list.count; i++) {
                if (i) sb_chr(b, ',');
                json_append_value(vm, b, o->as.list.items[i]);
            }
            sb_chr(b, ']');
            break;
        default:
            sb_str(b, "null");
            break;
    }
}

/* ---------- JSON decoding (text -> Value) ---------- */

typedef struct {
    const char *p;
    char err[160];
} JsonState;

static void js_skip_ws(JsonState *st) {
    while (*st->p == ' ' || *st->p == '\t' || *st->p == '\n' || *st->p == '\r')
        st->p++;
}

/* Read a JSON string at *p into out. Handles \uXXXX (with surrogate pairs,
 * emitting UTF-8). Returns token position after the closing quote. */
static const char *json_read_string(const char *p, sbuf *out) {
    if (*p != '"') return NULL;
    p++;
    while (*p != '"') {
        if (!*p) return NULL;
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': sb_chr(out, '\n'); p++; break;
                case 't': sb_chr(out, '\t'); p++; break;
                case 'r': sb_chr(out, '\r'); p++; break;
                case 'b': sb_chr(out, '\b'); p++; break;
                case 'f': sb_chr(out, '\f'); p++; break;
                case '/': sb_chr(out, '/'); p++; break;
                case '\\': sb_chr(out, '\\'); p++; break;
                case '"': sb_chr(out, '"'); p++; break;
                case 'u': {
                    unsigned long cp = 0;
                    if (p[1] == 0 || p[2] == 0 || p[3] == 0 || p[4] == 0) return NULL;
                    for (int i = 1; i <= 4; i++) {
                        char c = p[i];
                        cp <<= 4;
                        if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
                        else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
                        else return NULL;
                    }
                    p += 5;
                    /* handle low surrogate after a high surrogate */
                    if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                        unsigned long lo = 0;
                        for (int i = 2; i <= 5; i++) {
                            char c = p[i];
                            lo <<= 4;
                            if (c >= '0' && c <= '9') lo |= (unsigned)(c - '0');
                            else if (c >= 'a' && c <= 'f') lo |= (unsigned)(c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') lo |= (unsigned)(c - 'A' + 10);
                            else return NULL;
                        }
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    /* emit UTF-8 */
                    if (cp < 0x80) sb_chr(out, (char)cp);
                    else if (cp < 0x800) {
                        sb_chr(out, (char)(0xC0 | (cp >> 6)));
                        sb_chr(out, (char)(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        sb_chr(out, (char)(0xE0 | (cp >> 12)));
                        sb_chr(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        sb_chr(out, (char)(0x80 | (cp & 0x3F)));
                    } else {
                        sb_chr(out, (char)(0xF0 | (cp >> 18)));
                        sb_chr(out, (char)(0x80 | ((cp >> 12) & 0x3F)));
                        sb_chr(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        sb_chr(out, (char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default:
                    return NULL;
            }
        } else {
            sb_chr(out, *p);
            p++;
        }
    }
    return p + 1;
}

/* Parse one JSON value. On success pushes it onto the VM stack. */
static void json_value(VM *vm, JsonState *st);

static void json_value(VM *vm, JsonState *st) {
    js_skip_ws(st);
    char c = *st->p;

    if (c == '{') {
        st->p++;
        vm_push(vm, make_map(vm));
        js_skip_ws(st);
        if (*st->p == '}') { st->p++; return; }
        while (true) {
            js_skip_ws(st);
            if (*st->p != '"') { snprintf(st->err, sizeof(st->err), "expected object key"); return; }
            sbuf key = {0};
            const char *after = json_read_string(st->p, &key);
            if (!after) { snprintf(st->err, sizeof(st->err), "bad object key"); free(key.p); return; }
            st->p = after;
            js_skip_ws(st);
            if (*st->p != ':') { snprintf(st->err, sizeof(st->err), "expected ':'"); free(key.p); return; }
            st->p++;
            json_value(vm, st);
            if (st->err[0]) { free(key.p); return; }
            Value v = vm_pop(vm);
            Obj *map = AS_OBJ(vm->stack[vm->stack_count - 1]);
            map_set(vm, map, key.p ? key.p : "", v);
            free(key.p);
            js_skip_ws(st);
            if (*st->p == ',') { st->p++; continue; }
            if (*st->p == '}') { st->p++; return; }
            snprintf(st->err, sizeof(st->err), "expected ',' or '}' in object");
            return;
        }
    }

    if (c == '[') {
        st->p++;
        vm_push(vm, make_list(vm));
        js_skip_ws(st);
        if (*st->p == ']') { st->p++; return; }
        while (true) {
            json_value(vm, st);
            if (st->err[0]) return;
            Value v = vm_pop(vm);
            Obj *list = AS_OBJ(vm->stack[vm->stack_count - 1]);
            if (list->as.list.count == list->as.list.cap) {
                list->as.list.cap = list->as.list.cap ? list->as.list.cap * 2 : 8;
                list->as.list.items = realloc(list->as.list.items,
                                              sizeof(Value) * (size_t)list->as.list.cap);
            }
            list->as.list.items[list->as.list.count++] = v;
            js_skip_ws(st);
            if (*st->p == ',') { st->p++; continue; }
            if (*st->p == ']') { st->p++; return; }
            snprintf(st->err, sizeof(st->err), "expected ',' or ']' in array");
            return;
        }
    }

    if (c == '"') {
        sbuf s = {0};
        const char *after = json_read_string(st->p, &s);
        if (!after) { snprintf(st->err, sizeof(st->err), "bad string"); free(s.p); return; }
        st->p = after;
        vm_push(vm, make_string(vm, s.p ? s.p : "", s.len));
        free(s.p);
        return;
    }

    if (c == '-' || (c >= '0' && c <= '9')) {
        char *end = NULL;
        double d = strtod(st->p, &end);
        if (end == st->p) { snprintf(st->err, sizeof(st->err), "bad number"); return; }
        st->p = end;
        vm_push(vm, val_num(d));
        return;
    }

    if (strncmp(st->p, "true", 4) == 0)  { st->p += 4; vm_push(vm, val_bool(true)); return; }
    if (strncmp(st->p, "false", 5) == 0) { st->p += 5; vm_push(vm, val_bool(false)); return; }
    if (strncmp(st->p, "null", 4) == 0)  { st->p += 4; vm_push(vm, val_null()); return; }

    snprintf(st->err, sizeof(st->err), "unexpected character");
}

/* Parse full JSON text; results in a single value on the stack. */
static void json_parse_value(VM *vm, const char *src, const char **endp,
                             char *errbuf, size_t errbuf_size) {
    JsonState st;
    st.p = src;
    st.err[0] = '\0';
    json_value(vm, &st);
    if (st.err[0]) {
        if (errbuf && errbuf_size)
            snprintf(errbuf, errbuf_size, "%s (at offset %td)", st.err, st.p - src);
        vm_pop(vm); /* discard partial */
        vm_set_error(vm, "json parse error");
        if (endp) *endp = st.p;
        return;
    }
    if (endp) *endp = st.p;
}

/* Public wrapper for the parser/interp: JSON text -> single Value (pushed). */
void json_parse(VM *vm, const char *src, char *errbuf, size_t errbuf_size) {
    json_parse_value(vm, src, NULL, errbuf, errbuf_size);
}