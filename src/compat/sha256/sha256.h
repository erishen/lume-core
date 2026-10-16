#ifndef LUME_COMPAT_SHA256_H
#define LUME_COMPAT_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* Streaming SHA-256 (FIPS 180-4) + one-shot helpers + HMAC-SHA256.
 * No external dependencies; endian-neutral; safe for embedded/NUL bytes
 * (length-based, not NUL-terminated). */

typedef struct {
    uint64_t total;      /* total bytes fed so far */
    uint32_t state[8];   /* running hash state */
    uint8_t buffer[64];  /* pending block */
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t digest[32]);

/* One-shot convenience wrappers. */
void sha256(const uint8_t *data, size_t len, uint8_t digest[32]);
void hmac_sha256(const uint8_t *key, size_t keylen,
                 const uint8_t *msg, size_t msglen, uint8_t out[32]);

#endif /* LUME_COMPAT_SHA256_H */
