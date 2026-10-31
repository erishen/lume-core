#ifndef LUME_COMPAT_SHA512_H
#define LUME_COMPAT_SHA512_H

#include <stddef.h>
#include <stdint.h>

/* SHA-512 (FIPS 180-4)。无外部依赖、字节序中立、按长度处理（可含 NUL）。
 *
 * 为什么需要它: Ed25519 (RFC 8032) 的签名/验签全部基于 SHA-512 —— 它不是
 * "更长的 SHA-256", 而是不同的轮常数与 64-bit 字宽, 无法用 sha256 拼出来。
 * 所以 Ed25519 落地必须连带 vendored 一份 SHA-512。
 *
 * 与 compat/sha256 同规矩: 纯 C、无外部依赖、所有平台行为一致。 */

typedef struct {
    uint64_t total_lo;   /* 已喂入字节数的低 64 位 */
    uint64_t total_hi;   /* 高 64 位（SHA-512 的消息长度字段是 128 bit） */
    uint64_t state[8];
    uint8_t  buffer[128];
} sha512_ctx;

void sha512_init(sha512_ctx *ctx);
void sha512_update(sha512_ctx *ctx, const uint8_t *data, size_t len);
void sha512_final(sha512_ctx *ctx, uint8_t digest[64]);

/* 一次性接口。 */
void sha512(const uint8_t *data, size_t len, uint8_t digest[64]);

#endif /* LUME_COMPAT_SHA512_H */
