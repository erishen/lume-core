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
