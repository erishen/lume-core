/* Native crypt builtins for Lume.
 *
 * 2026-09-29 — crypt_sha512(pw): 用系统 crypt(3) 生成 $6$（SHA-512）强口令哈希，
 * 供运行时账号管理（写 htpasswd 文件）使用 —— math 站 admin 端点在在线增删账号时
 * 用它生成 /app/auth/htpasswd 条目，agent-httpd 认证层只认 $5$/$6$/bcrypt 强哈希。
 *
 * 2026-10-15 — sha256(s) / hmac_sha256(key, msg): 通用消息摘要内建（hex 输出），
 * 支撑支付方向（请求签名 / 回调验签）。SHA-256 实现 vendored 在
 * src/compat/sha256/（public domain，无外部依赖，与 regex 同规矩）。
 *
 * 2026-10-16 — base64_encode(s) / base64_decode(s): RFC 4648 标准字母表
 * 编解码（含 = 填充，二进制安全）。用途：PayPal OAuth2 的 HTTP Basic 凭证
 * base64(client_id:client_secret)，以及 webhook 验签材料的二进制安全处理。
 * decode 对非法字符 / 长度非 4 倍数 / 填充错位一律 vm_set_error。
 *
 * 平台约束（与 agent-httpd auth.c 的 HAVE_CRYPT 语义一致，仅 crypt_sha512 受影响）：
 *   - glibc (Linux/docker)：crypt() 支持 $6$，正常生成。
 *   - macOS：crypt(3) 只有 legacy DES，$6$ 返回 NULL → vm_set_error
 *     （开发机 make dev 认证关闭，不需要生成哈希；不影响构建/单测）。
 *   - 无 crypt() 的平台：直接 vm_set_error。
 *
 * 盐：getpid() + 单调时间低位，格式 "$6$%08lx%08lx$"（可读、够唯一，
 * 非密码学随机源但足以防重放；与 busybox cryptpw -m sha512 同格式）。
 */

#include "builtins_internal.h"
#include "compat/sha256/sha256.h"
#include "compat/sha512/sha512.h"
#include "compat/ed25519/ed25519.h"
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#elif defined(_WIN32)
/* Windows 随机源由 os_win32.c 提供。<windows.h> 经 winnt.h 定义的
 * TokenType 枚举成员会与 lume.h 的 TokenType 类型名在同一 TU 冲突,
 * 所以这里只做前向声明, 不 include 任何 Windows 头(见 os_win32.c 文件头)。 */
int lume_win32_random(unsigned char *buf, size_t n);
#endif

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

/* ---- sha256 / hmac_sha256 (2026-10-15) ----
 * 输出 64 字符小写 hex。入参限 string（lume 串可含 NUL，按显式长度哈希）。
 * 用途：支付请求签名（如 JWT HS256、支付宝 HMAC-SHA256 sign）与回调验签。 */

static void hash_to_hex(const uint8_t d[32], char out[65])
{
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = hexd[d[i] >> 4];
        out[i * 2 + 1] = hexd[d[i] & 0x0f];
    }
    out[64] = '\0';
}

static void native_sha256(VM *vm, int argc, Value *args, Value *out) {
    const char *s;
    if (argc != 1) {
        vm_set_error(vm, "sha256(): expects 1 argument (string)");
        return;
    }
    if (!arg_string(vm, args[0], &s)) return;
    uint8_t d[32];
    char hex[65];
    sha256((const uint8_t *)s, obj_string_len(AS_OBJ(args[0])), d);
    hash_to_hex(d, hex);
    *out = make_string_cstr(vm, hex);
}

static void native_hmac_sha256(VM *vm, int argc, Value *args, Value *out) {
    const char *key, *msg;
    if (argc != 2) {
        vm_set_error(vm, "hmac_sha256(): expects 2 arguments (key, message)");
        return;
    }
    if (!arg_string(vm, args[0], &key)) return;
    if (!arg_string(vm, args[1], &msg)) return;
    uint8_t d[32];
    char hex[65];
    hmac_sha256((const uint8_t *)key, obj_string_len(AS_OBJ(args[0])),
                (const uint8_t *)msg, obj_string_len(AS_OBJ(args[1])), d);
    hash_to_hex(d, hex);
    *out = make_string_cstr(vm, hex);
}

Value b_sha256(VM *vm, int argc, Value *args) {
    return vm_native(vm, argc, args, native_sha256);
}

Value b_hmac_sha256(VM *vm, int argc, Value *args) {
    return vm_native(vm, argc, args, native_hmac_sha256);
}

/* ---- base64 (2026-10-16) ----
 * RFC 4648 标准字母表（+/，含 = 填充），二进制安全。encode 输出不含 NUL；
 * decode 接受合法填充串，遇非法字符 / 长度非 4 倍数 / 填充错位则 vm_set_error。
 * 临时缓冲用 malloc（make_string 会把字节拷进 GC 管理的 ObjString，可立即释放）。 */

static const char B64_ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_index(char c) {
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a' + 26);
    if (c >= '0' && c <= '9') return (int)(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void native_base64_encode(VM *vm, int argc, Value *args, Value *out) {
    const char *s;
    if (argc != 1) {
        vm_set_error(vm, "base64_encode(): expects 1 argument (string)");
        return;
    }
    if (!arg_string(vm, args[0], &s)) return;
    size_t n = obj_string_len(AS_OBJ(args[0]));
    if (n == 0) { *out = make_string_cstr(vm, ""); return; }
    size_t outlen = ((n + 2) / 3) * 4;
    char *buf = (char *)malloc(outlen);
    if (!buf) { vm_set_error(vm, "base64_encode(): out of memory"); return; }
    size_t o = 0, i = 0;
    while (i < n) {
        unsigned int b0 = (unsigned char)s[i];
        unsigned int b1 = (i + 1 < n) ? (unsigned char)s[i + 1] : 0;
        unsigned int b2 = (i + 2 < n) ? (unsigned char)s[i + 2] : 0;
        unsigned int triple = (b0 << 16) | (b1 << 8) | b2;
        buf[o++] = B64_ENC[(triple >> 18) & 0x3f];
        buf[o++] = B64_ENC[(triple >> 12) & 0x3f];
        buf[o++] = (i + 1 < n) ? B64_ENC[(triple >> 6) & 0x3f] : '=';
        buf[o++] = (i + 2 < n) ? B64_ENC[triple & 0x3f] : '=';
        i += 3;
    }
    *out = make_string(vm, buf, outlen);
    free(buf);
}

static void native_base64_decode(VM *vm, int argc, Value *args, Value *out) {
    const char *s;
    if (argc != 1) {
        vm_set_error(vm, "base64_decode(): expects 1 argument (string)");
        return;
    }
    if (!arg_string(vm, args[0], &s)) return;
    size_t n = obj_string_len(AS_OBJ(args[0]));
    if (n == 0) { *out = make_string_cstr(vm, ""); return; }
    if (n % 4 != 0) {
        vm_set_error(vm, "base64_decode(): length must be a multiple of 4");
        return;
    }
    size_t outcap = (n / 4) * 3;
    char *buf = (char *)malloc(outcap ? outcap : 1);
    if (!buf) { vm_set_error(vm, "base64_decode(): out of memory"); return; }
    size_t o = 0;
    for (size_t g = 0; g < n; g += 4) {
        int v[4];
        int pad = 0;
        int pad_seen = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[g + k];
            if (c == '=') { pad++; pad_seen = 1; v[k] = 0; }
            else {
                if (pad_seen) {
                    free(buf);
                    vm_set_error(vm, "base64_decode(): misplaced padding");
                    return;
                }
                int idx = b64_index(c);
                if (idx < 0) {
                    free(buf);
                    vm_set_error(vm, "base64_decode(): invalid character");
                    return;
                }
                v[k] = idx;
            }
        }
        if (pad > 2) {
            free(buf);
            vm_set_error(vm, "base64_decode(): misplaced padding");
            return;
        }
        unsigned int triple = ((unsigned)v[0] << 18) | ((unsigned)v[1] << 12)
                            | ((unsigned)v[2] << 6) | (unsigned)v[3];
        buf[o++] = (char)((triple >> 16) & 0xff);
        if (pad < 2) buf[o++] = (char)((triple >> 8) & 0xff);
        if (pad < 1) buf[o++] = (char)(triple & 0xff);
    }
    *out = make_string(vm, buf, o);
    free(buf);
}

Value b_base64_encode(VM *vm, int argc, Value *args) {
    return vm_native(vm, argc, args, native_base64_encode);
}

Value b_base64_decode(VM *vm, int argc, Value *args) {
    return vm_native(vm, argc, args, native_base64_decode);
}


/* ---- Ed25519 (2026-10-31) ----
 *
 * 四个内建，全部走 hex 字符串（不引入二进制 Value 类型）:
 *   ed25519_pubkey(sk_hex)                  → pk_hex            (32B → 32B)
 *   ed25519_sign(msg, sk_hex)               → sig_hex           (任意长 → 64B)
 *   ed25519_verify(msg, sig_hex, pk_hex)    → bool
 *   ed25519_keypair()                       → {sk_hex, pk_hex}  (新随机密钥)
 *
 * 为什么是这四个: peer 跨机需要"每节点自己持私钥、别人只拿公钥"。HMAC 那套
 * 是对称的 —— 持 root 者能派生任何节点密钥, 等于能冒充任何节点。非对称之后,
 * 一台机器泄露私钥只影响它自己。
 *
 * 随机源: Linux 先 getrandom(2) 再退回 /dev/urandom, macOS 用
 * arc4random_buf, Windows 用 BCryptGenRandom(bcrypt.dll, Vista+ 随系统提供)。
 * 三者的共同底线: 拿不到 CSPRNG 就 vm_set_error 明确报错, 绝不静默降级成
 * rand()/时间种子 —— 可预测的私钥会让整套 peer 信任模型形同虚设。
 */

static int read_urandom(uint8_t *buf, size_t n) {
#if defined(_WIN32)
    return lume_win32_random(buf, n);
#elif defined(__APPLE__)
    /* arc4random_buf: macOS guarantees it (10.7+), no /dev dependency. */
    arc4random_buf(buf, n);
    return 0;
#else
#if defined(__linux__)
    /* getrandom(2) first: container/seccomp setups may hide or restrict
     * /dev, but the syscall is available on any modern Linux kernel. */
    ssize_t r = getrandom(buf, n, 0);
    if (r == (ssize_t)n) return 0;
#endif
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return got == n ? 0 : -1;
#endif
}

static void native_ed25519_pubkey(VM *vm, int argc, Value *args, Value *out) {
    const char *sk_hex;
    if (argc != 1) {
        vm_set_error(vm, "ed25519_pubkey(): expects 1 argument (secret key hex)");
        return;
    }
    if (!arg_string(vm, args[0], &sk_hex)) return;

    uint8_t sk[32], pk[32];
    if (ed25519_unhex(sk_hex, sk, 32) != 0) {
        vm_set_error(vm, "ed25519_pubkey(): secret key must be 64 hex chars");
        return;
    }
    if (ed25519_public_key(pk, sk) != 0) {
        vm_set_error(vm, "ed25519_pubkey(): failed");
        return;
    }
    char hex[65];
    ed25519_hex(pk, 32, hex);
    *out = make_string_cstr(vm, hex);
}

static void native_ed25519_sign(VM *vm, int argc, Value *args, Value *out) {
    const char *msg, *sk_hex;
    if (argc != 2) {
        vm_set_error(vm, "ed25519_sign(): expects 2 arguments (message, secret key hex)");
        return;
    }
    if (!arg_string(vm, args[0], &msg)) return;
    if (!arg_string(vm, args[1], &sk_hex)) return;

    uint8_t sk[32], pk[32], sig[64];
    if (ed25519_unhex(sk_hex, sk, 32) != 0) {
        vm_set_error(vm, "ed25519_sign(): secret key must be 64 hex chars");
        return;
    }
    if (ed25519_public_key(pk, sk) != 0) {
        vm_set_error(vm, "ed25519_sign(): bad secret key");
        return;
    }
    if (ed25519_sign(sig, (const uint8_t *)msg, obj_string_len(AS_OBJ(args[0])),
                     pk, sk) != 0) {
        vm_set_error(vm, "ed25519_sign(): failed");
        return;
    }
    char hex[129];
    ed25519_hex(sig, 64, hex);
    *out = make_string_cstr(vm, hex);
}

static void native_ed25519_verify(VM *vm, int argc, Value *args, Value *out) {
    const char *msg, *sig_hex, *pk_hex;
    if (argc != 3) {
        vm_set_error(vm, "ed25519_verify(): expects 3 arguments (message, sig hex, pubkey hex)");
        return;
    }
    if (!arg_string(vm, args[0], &msg)) return;
    if (!arg_string(vm, args[1], &sig_hex)) return;
    if (!arg_string(vm, args[2], &pk_hex)) return;

    uint8_t sig[64], pk[32];
    if (ed25519_unhex(sig_hex, sig, 64) != 0) {
        /* 格式错就是验签失败, 不抛错 —— 调用方(中间件)拿到 false 走拒绝分支,
         * 抛错反而会变成 500, 让"伪造签名"看起来像服务故障。 */
        *out = val_bool(false);
        return;
    }
    if (ed25519_unhex(pk_hex, pk, 32) != 0) {
        *out = val_bool(false);
        return;
    }
    int ok = ed25519_verify(sig, (const uint8_t *)msg, obj_string_len(AS_OBJ(args[0])), pk);
    *out = val_bool(ok == 1);
}

static void native_ed25519_keypair(VM *vm, int argc, Value *args, Value *out) {
    (void)args;
    if (argc != 0) {
        vm_set_error(vm, "ed25519_keypair(): expects no arguments");
        return;
    }
    uint8_t sk[32], pk[32];
    if (read_urandom(sk, 32) != 0) {
        vm_set_error(vm, "ed25519_keypair(): no secure random source available");
        return;
    }
    if (ed25519_public_key(pk, sk) != 0) {
        vm_set_error(vm, "ed25519_keypair(): failed");
        return;
    }
    char sk_hex[65], pk_hex[65];
    ed25519_hex(sk, 32, sk_hex);
    ed25519_hex(pk, 32, pk_hex);

    Obj *m = AS_OBJ(make_map(vm));
    vm_push(vm, val_obj(m));   /* 填表期间需要 GC root，否则压缩会搬走它 */
    map_set(vm, m, "sk", make_string_cstr(vm, sk_hex));
    map_set(vm, m, "pk", make_string_cstr(vm, pk_hex));
    *out = vm_pop(vm);
}

Value b_ed25519_pubkey(VM *vm, int argc, Value *args) {
    return vm_native(vm, argc, args, native_ed25519_pubkey);
}
Value b_ed25519_sign(VM *vm, int argc, Value *args) {
    return vm_native(vm, argc, args, native_ed25519_sign);
}
Value b_ed25519_verify(VM *vm, int argc, Value *args) {
    return vm_native(vm, argc, args, native_ed25519_verify);
}
Value b_ed25519_keypair(VM *vm, int argc, Value *args) {
    return vm_native(vm, argc, args, native_ed25519_keypair);
}

/* ---- crypt_verify / const_time_eq (2026-11-04, P3) ----
 *
 * crypt_verify(hash, password) → bool: 校验强口令哈希, 成功/失败都不泄露原因。
 *   - $5$/$6$ → crypt(3)(与 crypt_sha512 同平台约束; 无 crypt() 的构建恒 false)。
 *   - $2a$/$2b$/$2y$ → bcrypt。lume-core 不携带 bcrypt —— 第二份副本正是
 *     `make sync-lang` 要避免的漂移源; 本构建对 $2* 明确报错(文档化限制),
 *     host(lume, 链接 agent-httpd 的 bcrypt) 经 sync-lang 后以强符号覆盖
 *     实现, 见 sync-lang.py 的 builtins_crypt.c 处理。
 *   - 明文 / {SHA} / DES / $1$ / $apr1$ → 恒 false(与 agent-httpd
 *     is_strong_hash 同策略: 弱哈希不参与校验, 失败即拒绝)。
 *
 * const_time_eq(a, b) → bool: 等长 branchless 比较(长度本身不保密)。
 * 供 DSL 侧 token / CSRF 比较 —— 普通 == 在首个差异字节即短路, 泄露时序。
 */

static int ct_eq_bytes(const char *a, const char *b, size_t n) {
    unsigned char acc = 0;
    for (size_t i = 0; i < n; i++)
        acc |= (unsigned char)(a[i] ^ b[i]);
    return acc == 0;
}

/* -1 = 构建不支持该哈希族($2*: core 无 bcrypt), 0 = 校验失败, 1 = 通过 */
static int crypt_verify_impl(const char *hash, const char *pw) {
    size_t hl = strlen(hash);
    if (hl < 4) return 0;
    if (strncmp(hash, "$5$", 3) == 0 || strncmp(hash, "$6$", 3) == 0) {
#ifdef HAVE_CRYPT
        /* crypt(3) 第二参可传完整哈希串: 只读其盐段, 生成同格式结果。 */
        char *got = crypt(pw, hash);
        if (!got || strlen(got) != hl) return 0;
        return ct_eq_bytes(got, hash, hl) ? 1 : 0;
#else
        (void)pw;
        return 0;
#endif
    }
    if (strncmp(hash, "$2a$", 4) == 0 || strncmp(hash, "$2b$", 4) == 0 ||
        strncmp(hash, "$2y$", 4) == 0)
        return -1;   /* core 无 bcrypt: 文档化限制, host 覆盖 */
    return 0;
}

static void native_crypt_verify(VM *vm, int argc, Value *args, Value *out) {
    const char *hash, *pw;
    if (argc != 2) {
        vm_set_error(vm, "crypt_verify(): expects 2 arguments (hash, password)");
        return;
    }
    if (!arg_string(vm, args[0], &hash)) return;
    if (!arg_string(vm, args[1], &pw)) return;
    int r = crypt_verify_impl(hash, pw);
    if (r < 0) {
        vm_set_error(vm, "crypt_verify(): bcrypt ($2*) support lives in the host "
                         "build (lume links agent-httpd's bcrypt)");
        return;
    }
    *out = val_bool(r == 1);
}

Value b_crypt_verify(VM *vm, int argc, Value *args) {
    return vm_native(vm, argc, args, native_crypt_verify);
}

static void native_const_time_eq(VM *vm, int argc, Value *args, Value *out) {
    const char *a, *b;
    if (argc != 2) {
        vm_set_error(vm, "const_time_eq(): expects 2 arguments (a, b)");
        return;
    }
    if (!arg_string(vm, args[0], &a)) return;
    if (!arg_string(vm, args[1], &b)) return;
    size_t na = obj_string_len(AS_OBJ(args[0]));
    size_t nb = obj_string_len(AS_OBJ(args[1]));
    if (na != nb) { *out = val_bool(false); return; }
    *out = val_bool(ct_eq_bytes(a, b, na) ? true : false);
}

Value b_const_time_eq(VM *vm, int argc, Value *args) {
    return vm_native(vm, argc, args, native_const_time_eq);
}
