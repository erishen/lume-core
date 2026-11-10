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
#include <string.h> /* strcmp: sort comparator */
#include <regex.h>  /* str_match: POSIX extended regular expressions */

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
            /* Ints print from their i64. Widening to double first and casting
             * back (as this used to) loses every low bit past 2^53, so a
             * wrapped i64 printed as a different number than it holds. */
            if (IS_INT(args[i]))
                printf("%lld", AS_INT(args[i]));
            else
                printf("%g", AS_NUM(args[i]));
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
        char buf[64];
        /* Same rule as print(): an int keeps its exact i64 spelling, a float
         * goes through %g. Testing "is it integral" on the widened double
         * instead would corrupt large ints. */
        if (IS_INT(v))
            snprintf(buf, sizeof(buf), "%lld", AS_INT(v));
        else
            snprintf(buf, sizeof(buf), "%g", AS_NUM(v));
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

/* An accessor's fallback answer crosses into the cast's own type over the
 * same bridge the direct cast builds: int("12x") parses a leading number,
 * a bool widens (true -> 1), and null / containers have no numeric
 * reading, so they land on zero. The static emitters do this at compile
 * time in acc_default (codegen_call.c), and get()'s bool defaults widened
 * the same way before either existed. Handing the default back verbatim
 * instead — what this file used to do — let int() answer a bool or null,
 * the one shape the compiled backends cannot represent, and the accessor
 * silently disagreed with them on exactly those defaults. */
static Value acc_default_int(Value v)
{
    if (IS_NUM(v)) return val_int((long long)AS_NUM(v));
    if (IS_BOOL(v)) return val_int(AS_BOOL(v) ? 1 : 0);
    if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING)
        return val_int(atoll(obj_string(AS_OBJ(v))));
    return val_int(0);
}

static Value acc_default_float(Value v)
{
    if (IS_NUM(v)) return val_num(AS_NUM(v));
    if (IS_BOOL(v)) return val_num(AS_BOOL(v) ? 1.0 : 0.0);
    if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING)
        return val_num(atof(obj_string(AS_OBJ(v))));
    return val_num(0);
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
             * 而不是静默变 0——query 参数异常值应回落到脚本给的安全值
             * (默认值自身也按 cast 的规则转成 int,见 acc_default_int:
             *  "1" -> 1, true -> 1, null -> 0, 与两个编译后端一致)。 */
            const char *s = obj_string(AS_OBJ(v));
            char *end = NULL;
            long long n = strtoll(s, &end, 10);
            if (end != s && *end == '\0')
                *out = val_int(n);
            else
                *out = acc_default_int(argc >= 3 ? args[2] : val_int(0));
        } else {
            *out = acc_default_int(argc >= 3 ? args[2] : val_int(0));
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
                *out = acc_default_float(argc >= 3 ? args[2] : val_num(0));
        } else {
            *out = acc_default_float(argc >= 3 ? args[2] : val_num(0));
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

/* route params for server{} apps that want explicit captures (nsbp-lume uses
 * React router for pages, so this is only a helper the server runtime can wire
 * into a future DSL route shape). */
static void native_resolve(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2 || !IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_STRING) {
        vm_set_error(vm, "resolve() expects (req, path)");
        return;
    }
    if (argc < 3 || !IS_OBJ(args[2]) || AS_OBJ(args[2])->type != OBJ_MAP) {
        vm_set_error(vm, "resolve() expects (req, path, params)");
        return;
    }
    const char *base = obj_string(AS_OBJ(args[0]));
    const char *path = obj_string(AS_OBJ(args[1]));
    Obj *params = AS_OBJ(args[2]);
    Obj *m = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(m)); /* root while filling */
    map_set(vm, m, "path", make_string_cstr(vm, path));
    map_set(vm, m, "params", val_obj(params));
    if (base && base[0]) {
        /* Keep the same prefix semantics nsbp-lume used on the React side:
         * /about stays /about, /lume/scatter stays /lume/scatter. */
        const char *slash = strchr(path, '/');
        if (slash) {
            size_t plen = (size_t)(slash - path);
            char buf[1024];
            if (plen < sizeof buf) {
                memcpy(buf, path, plen);
                buf[plen] = '\0';
                map_set(vm, m, "matched", make_string_cstr(vm, buf));
            }
        } else {
            map_set(vm, m, "matched", make_string_cstr(vm, path));
        }
    }
    *out = vm_pop(vm);
}

static void native_json(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "json() needs an argument"); return; }
    /* Two directions, JS-style duality: a string argument is parsed (the
     * established semantics), any other value is serialized to a JSON
     * string — the same output stringify() produces. Serializing a string
     * is still spelled stringify(str) (it quotes it); json(str) keeps
     * meaning "parse". */
    if (!(IS_OBJ(args[0]) && AS_OBJ(args[0])->type == OBJ_STRING)) {
        sbuf b = {0};
        json_append_value(vm, &b, args[0]);
        *out = make_string(vm, b.p ? b.p : "", b.len);
        free(b.p); /* make_string copied out; the builder owns nothing */
        return;
    }
    const char *s = obj_string(AS_OBJ(args[0]));
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

/* sort(list) — sort a list in place and return it. Homogeneous lists only:
 * all numbers (ascending by value — all-int compares exactly as i64, any
 * float falls back to double) or all strings (byte/lexicographic via strcmp).
 * A list mixing kinds, or holding containers/maps, is rejected. */
static int g_sort_kind;          /* 0=int, 1=float, 2=string (single-threaded VM) */
static int cmp_sort_val(const void *a, const void *b) {
    const Value *va = (const Value *)a, *vb = (const Value *)b;
    if (g_sort_kind == 2)
        return strcmp(obj_string(AS_OBJ(*va)), obj_string(AS_OBJ(*vb)));
    if (g_sort_kind == 0) {      /* all int: exact i64 compare */
        long long x = AS_INT(*va), y = AS_INT(*vb);
        return (x > y) - (x < y);
    }
    double da = AS_NUM(*va), db = AS_NUM(*vb);   /* contains a float */
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}
static void native_sort(VM *vm, int argc, Value *args, Value *out) {
    if (argc != 1) {
        vm_set_error(vm, "sort() needs exactly one argument (a list)");
        return;
    }
    if (!IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_LIST) {
        vm_set_error(vm, "sort() needs a list");
        return;
    }
    ObjList *L = &AS_OBJ(args[0])->as.list;
    int n = L->count;
    if (n <= 1) { *out = args[0]; return; }   /* empty / single: already sorted */

    int kind = -1;
    for (int i = 0; i < n; i++) {
        Value v = L->items[i];
        int k;
        if (IS_INT(v))       k = 0;
        else if (IS_FLOAT(v)) k = 1;
        else if (IS_OBJ(v) && v.as.o->type == OBJ_STRING) k = 2;
        else {
            vm_set_error(vm, "sort() needs a list of numbers or strings");
            return;
        }
        if (kind == -1) kind = k;
        else if (kind != k) {
            vm_set_error(vm, "sort() needs a homogeneous list "
                             "(all numbers or all strings)");
            return;
        }
    }
    g_sort_kind = kind;
    qsort(L->items, (size_t)n, sizeof(Value), cmp_sort_val);
    *out = args[0];              /* sorted in place, original list returned */
}

/* ================= multipart(body, content_type) — RFC 7578 =================
 * This parser block is kept byte-for-byte in step across three build sites:
 *   lume-core/src/builtins.c   (interpreter)
 *   lume-core/src/rt.c         (both native emitters)
 *   lume/lang/builtins.c       (host mirror)
 * rt.c is a self-contained TU compiled on its own by backend.c, so a shared
 * header would have to ship with every packed build; duplicating this small
 * parser follows the native_str_match / lume_bi_str_match precedent instead.
 * The multipart legs of tests/native_backends.sh diff all three backends on
 * the same fixture, so a copy that drifts turns make test red.
 *
 * Input is (buf, len), not a C string: the interpreter's strings carry an
 * explicit length (read_file() is NUL-safe), so a part body containing NUL
 * survives. The native runtime passes strlen(body) because its
 * LumeSlot.str is a NUL-terminated C string — compiled programs keep the
 * platform's existing binary-string boundary (documented in TODO.md P13).
 */

#define MP_MAX_PARTS     4096
#define MP_BOUNDARY_MAX  200
#define MP_FIELD_MAX     1024

typedef struct { const char *p; size_t n; } MPSlice;
typedef struct {
    MPSlice name, filename, type, body;
} MPPart;

static int mp_space(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int mp_ieq(const char *a, size_t an, const char *b)
{
    size_t i, bn = strlen(b);
    if (an != bn) return 0;
    for (i = 0; i < an; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

static MPSlice mp_trim(MPSlice s)
{
    while (s.n && mp_space((unsigned char)s.p[0])) { s.p++; s.n--; }
    while (s.n && mp_space((unsigned char)s.p[s.n - 1])) s.n--;
    return s;
}

/* Copy a header slice into dst, folding the backslash escapes RFC 7578
 * permits inside quoted parameters. Header values are text, so the result
 * is a NUL-terminated C string. */
static size_t mp_copy(MPSlice s, char *dst, size_t cap)
{
    size_t k = 0, w = 0;
    while (k < s.n && w + 1 < cap) {
        if (s.p[k] == '\\' && k + 1 < s.n) k++;
        dst[w++] = s.p[k++];
    }
    dst[w] = '\0';
    return w;
}

/* memmem() is a GNU extension; MinGW/apple libc do not agree on it. */
static const char *mp_memmem(const char *h, size_t n, const char *nd, size_t m)
{
    const char *end, *p;
    if (!m || n < m) return NULL;
    end = h + n - m;
    p = h;
    while (p <= end) {
        p = (const char *)memchr(p, nd[0], (size_t)(end - p) + 1);
        if (!p) return NULL;
        if (memcmp(p, nd, m) == 0) return p;
        p++;
    }
    return NULL;
}

/* Slice of the line at *pos without its EOL, then advance past the EOL.
 * Accepts CRLF (RFC 2046) and bare LF (some clients). */
static int mp_line(const char *buf, size_t len, size_t *pos, MPSlice *out)
{
    size_t s, e;
    if (*pos >= len) return 0;
    s = *pos;
    e = s;
    while (e < len && buf[e] != '\n') e++;
    out->p = buf + s;
    out->n = e - s;
    if (out->n && buf[e - 1] == '\r') out->n--;
    *pos = (e < len) ? e + 1 : e;
    return 1;
}

/* Next occurrence of "--boundary" that starts a line, at or after `from`. */
static const char *mp_next_delim(const char *buf, size_t len,
                                 const char *mark, size_t marklen, size_t from)
{
    size_t i = from;
    while (i + marklen <= len) {
        const char *p = mp_memmem(buf + i, len - i, mark, marklen);
        size_t off;
        if (!p) return NULL;
        off = (size_t)(p - buf);
        if (off == 0 || buf[off - 1] == '\n') return p;
        i = off + 1;
    }
    return NULL;
}

/* Pull `boundary=` out of a Content-Type header. Quoted values and
 * whitespace around the token are accepted (browsers emit both). */
static int mp_boundary(MPSlice ct, char *out, size_t cap, const char **err)
{
    size_t semi = 0;
    MPSlice main;
    main.p = ct.p;
    main.n = 0;
    while (semi < ct.n && ct.p[semi] != ';') semi++;
    main.n = semi;
    main = mp_trim(main);
    if (!mp_ieq(main.p, main.n, "multipart/form-data")) {
        *err = "multipart(): content_type is not multipart/form-data";
        return 0;
    }
    while (semi < ct.n) {
        size_t ks, ke, i = semi + 1;        /* step over ';' */
        MPSlice v;
        int quoted;
        while (i < ct.n && mp_space((unsigned char)ct.p[i])) i++;
        ks = i;
        while (i < ct.n && ct.p[i] != '=' && ct.p[i] != ';') i++;
        ke = i;
        while (ke > ks && mp_space((unsigned char)ct.p[ke - 1])) ke--;
        if (i >= ct.n || ct.p[i] != '=') {
            while (i < ct.n && ct.p[i] != ';') i++;
            semi = i;
            continue;
        }
        i++;
        while (i < ct.n && mp_space((unsigned char)ct.p[i])) i++;
        v.p = ct.p + i;
        v.n = 0;
        quoted = (i < ct.n && ct.p[i] == '"');
        if (quoted) {
            i++;
            v.p = ct.p + i;
            while (i < ct.n && ct.p[i] != '"') {
                if (ct.p[i] == '\\' && i + 1 < ct.n) i++;
                i++;
            }
            v.n = (size_t)(ct.p + i - v.p);
            if (i < ct.n) i++;
        } else {
            while (i < ct.n && ct.p[i] != ';') i++;
            v.n = (size_t)(ct.p + i - v.p);
            v = mp_trim(v);
        }
        if (mp_ieq(ct.p + ks, ke - ks, "boundary")) {
            size_t w = mp_copy(v, out, cap);
            if (!w) { *err = "multipart(): empty boundary"; return 0; }
            if (w > MP_BOUNDARY_MAX) {
                *err = "multipart(): boundary too long";
                return 0;
            }
            return 1;
        }
        while (i < ct.n && ct.p[i] != ';') i++;
        semi = i;
    }
    *err = "multipart(): missing boundary in content_type";
    return 0;
}

/* Read a `name="..."` / `filename="..."` parameter from a
 * Content-Disposition header value. The returned slice still points into
 * the body and may carry backslash escapes; mp_copy() folds them. */
static MPSlice mp_param(MPSlice hv, const char *key)
{
    size_t i = 0;
    MPSlice empty;
    empty.p = NULL;
    empty.n = 0;
    while (i < hv.n && hv.p[i] != ';') i++;   /* disposition type token */
    while (i < hv.n) {
        size_t ks, ke;
        MPSlice v;
        int quoted;
        i++;                                   /* step over ';' */
        while (i < hv.n && mp_space((unsigned char)hv.p[i])) i++;
        ks = i;
        while (i < hv.n && hv.p[i] != '=' && hv.p[i] != ';') i++;
        ke = i;
        while (ke > ks && mp_space((unsigned char)hv.p[ke - 1])) ke--;
        if (i >= hv.n || hv.p[i] != '=') {
            while (i < hv.n && hv.p[i] != ';') i++;
            continue;
        }
        i++;
        while (i < hv.n && mp_space((unsigned char)hv.p[i])) i++;
        v.p = hv.p + i;
        v.n = 0;
        quoted = (i < hv.n && hv.p[i] == '"');
        if (quoted) {
            i++;
            v.p = hv.p + i;
            while (i < hv.n && hv.p[i] != '"') {
                if (hv.p[i] == '\\' && i + 1 < hv.n) i++;
                i++;
            }
            v.n = (size_t)(hv.p + i - v.p);
            if (i < hv.n) i++;
        } else {
            while (i < hv.n && hv.p[i] != ';') i++;
            v.n = (size_t)(hv.p + i - v.p);
            v = mp_trim(v);
        }
        if (mp_ieq(hv.p + ks, ke - ks, key)) return v;
        while (i < hv.n && hv.p[i] != ';') i++;
    }
    return empty;
}

/* Returns a malloc'd array of part descriptors (slices into buf); the
 * caller frees it. NULL + *err means malformed input. Header parsing adds
 * no allocations, so an error never leaves half-built VM containers. */
static MPPart *mp_parse(const char *buf, size_t len, MPSlice ct,
                        size_t *count, const char **err)
{
    char bnd[MP_BOUNDARY_MAX + 2];
    char mark[MP_BOUNDARY_MAX + 4];
    size_t marklen, pos, used = 0, cap = 8;
    MPPart *parts;
    const char *d;

    *count = 0;
    *err = NULL;
    if (!mp_boundary(ct, bnd, sizeof bnd, err)) return NULL;
    mark[0] = '-';
    mark[1] = '-';
    marklen = strlen(bnd);
    memcpy(mark + 2, bnd, marklen);
    marklen += 2;

    parts = (MPPart *)malloc(sizeof(MPPart) * cap);
    if (!parts) { *err = "multipart(): out of memory"; return NULL; }
    if (!len) return parts;                 /* empty body -> empty list */

    d = mp_next_delim(buf, len, mark, marklen, 0);
    if (!d) { *err = "multipart(): no boundary delimiter in body"; goto fail; }
    pos = (size_t)(d - buf);               /* the first delimiter line */

    for (;;) {
        MPSlice line;
        MPPart part;
        size_t body_start, body_end;
        const char *next;
        int closing, saw_blank = 0;

        if (!mp_line(buf, len, &pos, &line)) {
            *err = "multipart(): truncated boundary";
            goto fail;
        }
        if (line.n < marklen || memcmp(line.p, mark, marklen) != 0) {
            *err = "multipart(): malformed boundary line";
            goto fail;
        }
        closing = (line.n >= marklen + 2 &&
                   line.p[marklen] == '-' && line.p[marklen + 1] == '-');
        if (closing) break;
        {
            size_t k = marklen;
            while (k < line.n && mp_space((unsigned char)line.p[k])) k++;
            if (k != line.n) {
                *err = "multipart(): malformed boundary line";
                goto fail;
            }
        }

        memset(&part, 0, sizeof part);
        while (mp_line(buf, len, &pos, &line)) {
            size_t colon;
            MPSlice hn, hv;
            if (!line.n) { saw_blank = 1; break; }
            colon = 0;
            while (colon < line.n && line.p[colon] != ':') colon++;
            if (colon == line.n) continue;  /* ignore a header without ':' */
            hn.p = line.p;
            hn.n = colon;
            hv.p = line.p + colon + 1;
            hv.n = line.n - colon - 1;
            hv = mp_trim(hv);
            if (mp_ieq(hn.p, hn.n, "content-disposition")) {
                MPSlice nm = mp_param(hv, "name");
                MPSlice fn = mp_param(hv, "filename");
                if (nm.p) part.name = nm;
                if (fn.p) part.filename = fn;
            } else if (mp_ieq(hn.p, hn.n, "content-type")) {
                part.type = hv;
            }
        }
        if (!saw_blank) {
            *err = "multipart(): unterminated part headers";
            goto fail;
        }

        body_start = pos;
        next = mp_next_delim(buf, len, mark, marklen, body_start);
        if (!next) { *err = "multipart(): missing closing boundary"; goto fail; }
        body_end = (size_t)(next - buf);
        /* The CRLF just before the delimiter belongs to the delimiter, not
         * to the part body (RFC 2046 §5.1.1). Guard the empty-body case:
         * then `next == body_start` and the preceding LF is the blank
         * header line, which must not be subtracted. */
        if (body_end > body_start) {
            if (body_end >= body_start + 2 &&
                buf[body_end - 2] == '\r' && buf[body_end - 1] == '\n')
                body_end -= 2;
            else if (buf[body_end - 1] == '\n')
                body_end -= 1;
        }
        part.body.p = buf + body_start;
        part.body.n = body_end - body_start;

        if (used == cap) {
            MPPart *np;
            if (cap >= MP_MAX_PARTS) {
                *err = "multipart(): too many parts";
                goto fail;
            }
            cap = cap * 2 > MP_MAX_PARTS ? MP_MAX_PARTS : cap * 2;
            np = (MPPart *)realloc(parts, sizeof(MPPart) * cap);
            if (!np) { *err = "multipart(): out of memory"; goto fail; }
            parts = np;
        }
        parts[used++] = part;
        pos = (size_t)(next - buf);        /* consume that delimiter line next */
    }
    *count = used;
    return parts;
fail:
    free(parts);
    return NULL;
}

/* multipart(body, content_type) -> [{name, filename, type, body}].
 * Interpreter half; lume_bi_multipart (rt.c) is the native mirror and the
 * multipart leg of tests/native_backends.sh pins their agreement. */
static void native_multipart(VM *vm, int argc, Value *args, Value *out) {
    const char *body, *ct;
    size_t body_len, ct_len, n = 0, i;
    MPPart *parts;
    MPSlice cts;
    const char *err = NULL;
    Obj *res;

    if (argc != 2) {
        vm_set_error(vm, "multipart() needs (body, content_type)");
        return;
    }
    if (!IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_STRING) {
        vm_set_error(vm, "multipart(): body must be a string");
        return;
    }
    if (!IS_OBJ(args[1]) || AS_OBJ(args[1])->type != OBJ_STRING) {
        vm_set_error(vm, "multipart(): content_type must be a string");
        return;
    }
    body = obj_string(AS_OBJ(args[0]));
    body_len = obj_string_len(AS_OBJ(args[0]));
    ct = obj_string(AS_OBJ(args[1]));
    ct_len = obj_string_len(AS_OBJ(args[1]));
    cts.p = ct;
    cts.n = ct_len;

    /* Parse first: mp_parse allocates nothing in the VM, so a malformed
     * request errors out before any half-built list/map is rooted. */
    parts = mp_parse(body, body_len, cts, &n, &err);
    if (!parts) {
        vm_set_error(vm, "%s", err ? err : "multipart(): parse failed");
        return;
    }

    res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));
    for (i = 0; i < n; i++) {
        char name[MP_FIELD_MAX], filename[MP_FIELD_MAX], type[MP_FIELD_MAX];
        Obj *mo;
        mp_copy(parts[i].name, name, sizeof name);
        mp_copy(parts[i].filename, filename, sizeof filename);
        mp_copy(parts[i].type, type, sizeof type);
        mo = AS_OBJ(make_map(vm));
        vm_push(vm, val_obj(mo));
        map_set(vm, mo, "name", make_string(vm, name, strlen(name)));
        /* Absent optional headers are "" rather than null: the native
         * anon-row getter lume_map_get_s() collapses a null OBJ slot to
         * "", so a null here would make the same program disagree between
         * the interpreter and the two emitters. */
        map_set(vm, mo, "filename", make_string(vm, filename, strlen(filename)));
        map_set(vm, mo, "type", make_string(vm, type, strlen(type)));
        map_set(vm, mo, "body", make_string(vm, parts[i].body.p, parts[i].body.n));
        list_push(vm, res, val_obj(mo));
        vm_pop(vm);
    }
    vm_pop(vm);
    free(parts);
    *out = val_obj(res);
}

/* str_match(pattern, text) / regex(pattern, text) — scan `text` with a POSIX
 * extended regex and return a list of match records, each
 *   { text: matched substring, index: 0-based offset, groups: [capture groups] }
 * No match -> empty list. Invalid pattern -> VM error. A zero-width match ends
 * the scan so a pattern like `a*` cannot loop forever. */
static void native_str_match(VM *vm, int argc, Value *args, Value *out) {
    if (argc != 2) {
        vm_set_error(vm, "str_match() needs (pattern, text)");
        return;
    }
    const char *pat, *txt;
    if (!arg_string(vm, args[0], &pat)) return;
    if (!arg_string(vm, args[1], &txt)) return;

    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED) != 0) {
        vm_set_error(vm, "str_match(): invalid regex pattern");
        return;
    }

    Obj *res = AS_OBJ(make_list(vm));
    vm_push(vm, val_obj(res));          /* root while filling */

    const char *p = txt;
    regmatch_t m[10];                   /* m[0]=whole match, m[1..9]=groups */
    while (regexec(&re, p, 10, m, 0) == 0) {
        int so = m[0].rm_so, eo = m[0].rm_eo;
        if (eo <= so) break;            /* zero-width match: stop the loop */
        size_t start = (size_t)(p - txt) + (size_t)so;
        size_t mlen  = (size_t)eo - (size_t)so;

        Obj *mo = AS_OBJ(make_map(vm));
        vm_push(vm, val_obj(mo));
        map_set(vm, mo, "text",  make_string(vm, txt + start, mlen));
        map_set(vm, mo, "index", val_int((long long)start));

        Obj *gs = AS_OBJ(make_list(vm));
        vm_push(vm, val_obj(gs));
        for (int gi = 1; gi < 10; gi++) {
            if (m[gi].rm_so == -1) break;
            int gso = m[gi].rm_so, geo = m[gi].rm_eo;
            list_push(vm, gs,
                      make_string(vm, p + gso, (size_t)geo - (size_t)gso));
        }
        map_set(vm, mo, "groups", val_obj(gs));
        vm_pop(vm);                     /* gs */
        vm_pop(vm);                     /* mo */

        list_push(vm, res, val_obj(mo));
        p += eo;
        if (*p == '\0') break;
    }
    regfree(&re);
    vm_pop(vm);                         /* res */
    *out = val_obj(res);
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
/* b_sql_query / b_sql_write: SQLite builtins (builtins_sql.c), compiled in
 * when the Makefile probe found libsqlite3; stubs below otherwise. */
Value b_sql_query(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_sql_query); }
Value b_sql_write(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_sql_write); }
Value b_sql_tx_begin(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_sql_tx_begin); }
Value b_sql_tx_exec(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_sql_tx_exec); }
Value b_sql_tx_query(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_sql_tx_query); }
Value b_sql_tx_commit(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_sql_tx_commit); }
Value b_sql_tx_rollback(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_sql_tx_rollback); }
Value b_json(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_json); }
Value b_stringify(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_stringify); }
Value b_now(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_now); }
Value b_env(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_env); }
Value b_argv(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_argv); }
Value b_files(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_files); }
Value b_read_file(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_read_file); }
Value b_read_stdin(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_read_stdin); }
Value b_write_file(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_write_file); }
Value b_mkdir(VM *vm, int argc, Value *args)      { return vm_native(vm, argc, args, native_mkdir); }
Value b_lock_file(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_lock_file); }
Value b_unlock_file(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_unlock_file); }
Value b_strftime(VM *vm, int argc, Value *args)   { return vm_native(vm, argc, args, native_strftime); }
Value b_put(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_put); }
Value b_push(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_push); }
Value b_try(VM *vm, int argc, Value *args)        { return vm_native(vm, argc, args, native_try); }
Value b_sort(VM *vm, int argc, Value *args)       { return vm_native(vm, argc, args, native_sort); }
Value b_str_match(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_str_match); }
Value b_multipart(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_multipart); }
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
Value b_http_stream_open(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_http_stream_open); }
Value b_http_stream_read(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_http_stream_read); }
Value b_http_stream_close(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_http_stream_close); }
/* P18 server-side SSE push: thin wrappers over builtins_http.c. */
Value b_sse_open(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_sse_open); }
Value b_sse_send(VM *vm, int argc, Value *args)  { return vm_native(vm, argc, args, native_sse_send); }
Value b_sse_close(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_sse_close); }

/* When the build opts out of HTTP (LUME_HAS_HTTP=0 — the Windows/mingw port
 * excludes src/builtins_http.c), the native_http_* symbols still have to
 * exist so the always-compiled b_http_* wrappers above link. Those wrappers
 * are unreferenced in that build: interp.c seeds the same names against its
 * own diagnostic stub (no external wrapper symbol needed), which is what
 * produces the clear runtime error. These implementations exist purely to
 * keep the link closed. */
#if !LUME_HAS_HTTP
static void native_http_unavailable(VM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)args; (void)out;
    vm_set_error(vm, "http_* unavailable: this build was compiled without LUME_HAS_HTTP");
}
void native_http_get(VM *vm, int argc, Value *args, Value *out)    { native_http_unavailable(vm, argc, args, out); }
void native_http_post(VM *vm, int argc, Value *args, Value *out)   { native_http_unavailable(vm, argc, args, out); }
void native_http_put(VM *vm, int argc, Value *args, Value *out)    { native_http_unavailable(vm, argc, args, out); }
void native_http_patch(VM *vm, int argc, Value *args, Value *out)  { native_http_unavailable(vm, argc, args, out); }
void native_http_delete(VM *vm, int argc, Value *args, Value *out) { native_http_unavailable(vm, argc, args, out); }
void native_http_stream_open(VM *vm, int argc, Value *args, Value *out)  { native_http_unavailable(vm, argc, args, out); }
void native_http_stream_read(VM *vm, int argc, Value *args, Value *out)  { native_http_unavailable(vm, argc, args, out); }
void native_http_stream_close(VM *vm, int argc, Value *args, Value *out) { native_http_unavailable(vm, argc, args, out); }
void native_sse_open(VM *vm, int argc, Value *args, Value *out)  { native_http_unavailable(vm, argc, args, out); }
void native_sse_send(VM *vm, int argc, Value *args, Value *out)  { native_http_unavailable(vm, argc, args, out); }
void native_sse_close(VM *vm, int argc, Value *args, Value *out) { native_http_unavailable(vm, argc, args, out); }
#endif

/* Same pattern as the http_* stubs: when the build has no libsqlite3
 * (LUME_HAS_SQLITE=0 — the Makefile probe failed), the two native_sql_*
 * symbols still have to exist so the always-compiled b_sql_* wrappers
 * link. sql_query/sql_write stay registered and type-checkable; calling
 * them fails clearly at runtime instead of crashing on a missing symbol. */
#if !LUME_HAS_SQLITE
static void native_sql_unavailable(VM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)args; (void)out;
    vm_set_error(vm, "sql_* unavailable: this build was compiled without libsqlite3");
}
void native_sql_query(VM *vm, int argc, Value *args, Value *out) { native_sql_unavailable(vm, argc, args, out); }
void native_sql_write(VM *vm, int argc, Value *args, Value *out) { native_sql_unavailable(vm, argc, args, out); }
void native_sql_tx_begin(VM *vm, int argc, Value *args, Value *out) { native_sql_unavailable(vm, argc, args, out); }
void native_sql_tx_exec(VM *vm, int argc, Value *args, Value *out) { native_sql_unavailable(vm, argc, args, out); }
void native_sql_tx_query(VM *vm, int argc, Value *args, Value *out) { native_sql_unavailable(vm, argc, args, out); }
void native_sql_tx_commit(VM *vm, int argc, Value *args, Value *out) { native_sql_unavailable(vm, argc, args, out); }
void native_sql_tx_rollback(VM *vm, int argc, Value *args, Value *out) { native_sql_unavailable(vm, argc, args, out); }
#endif

Value b_default_route(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_default_route); }
Value b_resolve(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_resolve); }

/* ---- P14 gated child processes (builtins_spawn.c) -------------------- */
Value b_spawn(VM *vm, int argc, Value *args)     { return vm_native(vm, argc, args, native_spawn); }
Value b_proc_read(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_proc_read); }
Value b_proc_wait(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_proc_wait); }
Value b_proc_kill(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_proc_kill); }

/* Same shape as the http and sql stubs (do not write their wildcard names
 * inside a block comment: the star-slash closes it early). builtins_spawn.c
 * is POSIX-only and filtered out of the Windows build, but the
 * always-compiled b_spawn wrappers above still need their native symbols.
 * interp.c keeps the names registered and type-checkable through its own
 * diagnostic stub and raises the clear runtime error; these implementations
 * only close the link. */
#ifndef LUME_HAS_SPAWN
#define LUME_HAS_SPAWN 1
#endif
#if !LUME_HAS_SPAWN
static void native_spawn_unavailable(VM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)args; (void)out;
    vm_set_error(vm, "spawn_* unavailable: this build has no POSIX process support");
}
void native_spawn(VM *vm, int argc, Value *args, Value *out)     { native_spawn_unavailable(vm, argc, args, out); }
void native_proc_read(VM *vm, int argc, Value *args, Value *out) { native_spawn_unavailable(vm, argc, args, out); }
void native_proc_wait(VM *vm, int argc, Value *args, Value *out) { native_spawn_unavailable(vm, argc, args, out); }
void native_proc_kill(VM *vm, int argc, Value *args, Value *out) { native_spawn_unavailable(vm, argc, args, out); }
#endif
