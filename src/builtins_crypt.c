/* Native crypt builtin for Lume — SHA-512 crypt hash (2026-09-29).
 *
 * 给 DSL 层补 crypt_sha512(pw)：用系统 crypt(3) 生成 $6$（SHA-512）
 * 强口令哈希，供运行时账号管理（写 htpasswd 文件）使用 —— math 站
 * admin 端点在在线增删账号时用它生成 /app/auth/htpasswd 条目，
 * agent-httpd 认证层只认 $5$/$6$/bcrypt 强哈希。
 *
 * 平台约束（与 agent-httpd auth.c 的 HAVE_CRYPT 语义一致）：
 *   - glibc (Linux/docker)：crypt() 支持 $6$，正常生成。
 *   - macOS：crypt(3) 只有 legacy DES，$6$ 返回 NULL → vm_set_error
 *     （开发机 make dev 认证关闭，不需要生成哈希；不影响构建/单测）。
 *   - 无 crypt() 的平台：直接 vm_set_error。
 *
 * 盐：getpid() + 单调时间低位，格式 "$6$%08lx%08lx$"（可读、够唯一，
 * 非密码学随机源但足以防重放；与 busybox cryptpw -m sha512 同格式）。
 */

#include "builtins_internal.h"

#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#include <time.h>
#include <unistd.h>

#ifndef HAVE_CRYPT
/* 无 crypt() 的平台：crypt_sha512 恒报错（编译期禁用该内建行为）。 */
static const char *crypt_sha512_unavailable = "crypt_sha512(): SHA-512 crypt unavailable on this platform";
#endif

static void native_crypt_sha512(VM *vm, int argc, Value *args, Value *out) {
    const char *pw;
    if (argc < 1 || !arg_string(vm, args[0], &pw)) return;
#ifdef HAVE_CRYPT
    char salt[64];
    snprintf(salt, sizeof(salt), "$6$%08lx%08lx$",
             (unsigned long)getpid(), (unsigned long)time(NULL));
    char *got = crypt(pw, salt);
    if (!got || strncmp(got, "$6$", 3) != 0) {
        vm_set_error(vm, "crypt_sha512(): SHA-512 crypt unsupported on this platform");
        return;
    }
    *out = make_string_cstr(vm, got);
#else
    (void)out;
    vm_set_error(vm, "%s", crypt_sha512_unavailable);
#endif
}

Value b_crypt_sha512(VM *vm, int argc, Value *args) {
    return vm_native(vm, argc, args, native_crypt_sha512);
}
