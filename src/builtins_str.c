/* 字符串内建——replace 等（2026-09-27 新增，builtins_str.c）。
 * 归属：字符串变换类函数后续都放这里（lower/upper/trim/split/join…）。 */

#include "builtins_internal.h"

/* replace(s, from, to)：字面量全局替换。
 * - from 为空 → 返回原串（不做字符间插入）
 * - 无匹配 → 返回原串
 * - 结果上限 16 MiB（与 read_file 一致）；UTF-8 按字节序列匹配，
 *   中文等非 ASCII 替换照常生效 */
void native_replace(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 3) { vm_set_error(vm, "replace() needs (s, from, to)"); return; }
    const char *s = NULL, *from = NULL, *to = NULL;
    if (!arg_string(vm, args[0], &s)) return;
    if (!arg_string(vm, args[1], &from)) return;
    if (!arg_string(vm, args[2], &to)) return;
    size_t slen = strlen(s), flen = strlen(from);
    if (flen == 0) { *out = make_string_cstr(vm, s); return; }
    sbuf b = {0};
    const char *p = s, *end = s + slen;
    int matched = 0;
    while (p < end) {
        if ((size_t)(end - p) >= flen && memcmp(p, from, flen) == 0) {
            size_t tlen = strlen(to);
            if (b.len + tlen > (16u << 20)) {
                free(b.p);
                vm_set_error(vm, "replace() result too large");
                *out = val_null();
                return;
            }
            sb_mem(&b, to, tlen);
            p += flen;
            matched = 1;
        } else {
            sb_chr(&b, *p);
            p++;
        }
    }
    if (!matched) { free(b.p); *out = make_string_cstr(vm, s); return; }
    if (b.oom || !b.p) { free(b.p); *out = val_null(); return; }
    *out = make_string(vm, b.p, b.len);
    free(b.p);
}

Value b_replace(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_replace); }

/* ---- string tools (2026-10-08, P4a: nsgm codegen needs) ----
 * upper/lower/capitalize/trim are ASCII-only (non-ASCII bytes pass through
 * unchanged, like replace()); split/substr are byte-based. */

/* upper(s) — ASCII uppercase. */
static void native_upper(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "upper() needs a string"); return; }
    const char *s = NULL;
    if (!arg_string(vm, args[0], &s)) return;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) { vm_set_error(vm, "upper() out of memory"); *out = val_null(); return; }
    for (size_t i = 0; i < n; i++) p[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
    p[n] = '\0';
    *out = make_string_cstr(vm, p);
    free(p);
}

/* lower(s) — ASCII lowercase. */
static void native_lower(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "lower() needs a string"); return; }
    const char *s = NULL;
    if (!arg_string(vm, args[0], &s)) return;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) { vm_set_error(vm, "lower() out of memory"); *out = val_null(); return; }
    for (size_t i = 0; i < n; i++) p[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
    p[n] = '\0';
    *out = make_string_cstr(vm, p);
    free(p);
}

/* capitalize(s) — first ASCII char uppercased, rest unchanged. */
static void native_capitalize(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "capitalize() needs a string"); return; }
    const char *s = NULL;
    if (!arg_string(vm, args[0], &s)) return;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) { vm_set_error(vm, "capitalize() out of memory"); *out = val_null(); return; }
    for (size_t i = 0; i < n; i++) p[i] = s[i];
    if (n > 0 && p[0] >= 'a' && p[0] <= 'z') p[0] = (char)(p[0] - 32);
    p[n] = '\0';
    *out = make_string_cstr(vm, p);
    free(p);
}

/* trim(s) — strip ASCII whitespace from both ends. */
static void native_trim(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 1) { vm_set_error(vm, "trim() needs a string"); return; }
    const char *s = NULL;
    if (!arg_string(vm, args[0], &s)) return;
    size_t n = strlen(s), start = 0;
    while (start < n && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' ||
                         s[start] == '\r' || s[start] == '\f' || s[start] == '\v'))
        start++;
    while (n > start && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\n' ||
                         s[n-1] == '\r' || s[n-1] == '\f' || s[n-1] == '\v'))
        n--;
    *out = make_string(vm, s + start, n - start);
}

/* contains(s, sub) — substring test (empty sub → true). */
static void native_contains(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2) { vm_set_error(vm, "contains() needs (s, sub)"); return; }
    const char *s = NULL, *sub = NULL;
    if (!arg_string(vm, args[0], &s)) return;
    if (!arg_string(vm, args[1], &sub)) return;
    *out = val_bool(sub[0] == '\0' || strstr(s, sub) != NULL);
}

/* split(s, sep) — byte-based split into a list. Empty sep yields [s]. */
static void native_split(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2) { vm_set_error(vm, "split() needs (s, sep)"); return; }
    const char *s = NULL, *sep = NULL;
    if (!arg_string(vm, args[0], &s)) return;
    if (!arg_string(vm, args[1], &sep)) return;
    size_t slen = strlen(s), flen = strlen(sep);
    Obj *list = AS_OBJ(make_list(vm));
    if (flen == 0) {
        list_push(vm, list, make_string_cstr(vm, s));
        *out = val_obj(list);
        return;
    }
    const char *p = s, *end = s + slen;
    for (;;) {
        const char *hit = strstr(p, sep);
        if (!hit) { list_push(vm, list, make_string(vm, p, (size_t)(end - p))); break; }
        list_push(vm, list, make_string(vm, p, (size_t)(hit - p)));
        p = hit + flen;
    }
    *out = val_obj(list);
}

/* join(list, sep) — concatenate element strings with sep. Result capped at
 * 16 MiB like replace(). */
static void native_join(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2) { vm_set_error(vm, "join() needs (list, sep)"); return; }
    if (!IS_OBJ(args[0]) || AS_OBJ(args[0])->type != OBJ_LIST) {
        vm_set_error(vm, "join() first argument must be a list");
        return;
    }
    const char *sep = NULL;
    if (!arg_string(vm, args[1], &sep)) return;
    Obj *list = AS_OBJ(args[0]);
    size_t flen = strlen(sep);
    sbuf b = {0};
    for (int i = 0; i < list->as.list.count; i++) {
        if (i && flen) sb_mem(&b, sep, flen);
        Value sv;
        str_of_value(vm, list->as.list.items[i], &sv);
        if (IS_OBJ(sv) && AS_OBJ(sv)->type == OBJ_STRING) {
            const char *ps = obj_string(AS_OBJ(sv));
            size_t psn = strlen(ps);
            if (b.len + psn > (16u << 20)) {
                free(b.p);
                vm_set_error(vm, "join() result too large");
                *out = val_null();
                return;
            }
            sb_mem(&b, ps, psn);
        }
    }
    if (b.oom || !b.p) { free(b.p); *out = val_null(); return; }
    *out = make_string(vm, b.p, b.len);
    free(b.p);
}

/* substr(s, start, len?) — byte-based substring. Negative start counts from
 * the end; len clamps to the string end. */
static void native_substr(VM *vm, int argc, Value *args, Value *out) {
    if (argc < 2) { vm_set_error(vm, "substr() needs (s, start)"); return; }
    const char *s = NULL;
    if (!arg_string(vm, args[0], &s)) return;
    if (!IS_NUM(args[1])) { vm_set_error(vm, "substr() start must be a number"); return; }
    long n = (long)strlen(s);
    long start = (long)AS_NUM(args[1]);
    if (start < 0) start = n + start;
    if (start < 0) start = 0;
    if (start > n) start = n;
    long len = n - start;
    if (argc >= 3) {
        if (!IS_NUM(args[2])) { vm_set_error(vm, "substr() len must be a number"); return; }
        len = (long)AS_NUM(args[2]);
        if (len < 0) len = 0;
        if (start + len > n) len = n - start;
    }
    *out = make_string(vm, s + start, (size_t)len);
}

Value b_upper(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_upper); }
Value b_lower(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_lower); }
Value b_capitalize(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_capitalize); }
Value b_trim(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_trim); }
Value b_contains(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_contains); }
Value b_split(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_split); }
Value b_join(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_join); }
Value b_substr(VM *vm, int argc, Value *args) { return vm_native(vm, argc, args, native_substr); }
