/* NOTE (lume-core): vendored third-party-style code, not lume source.
 * SHA-256 per FIPS 180-4. Written fresh for lume-core (2026-10-15) in the
 * classic public-domain construction popularised by Olivier Gay's sha256.c
 * and Brad Conte's crypto-algorithms; no upstream file was copied. Public
 * domain / CC0-style: no attribution required, no license obligations.
 * Exists so the DSL can offer sha256()/hmac_sha256() builtins (payment
 * request signing / callback verification) without linking OpenSSL —
 * the tree is deliberately dependency-free (cf. src/compat/regex). Keep
 * this header intact when refreshing. */

#include <string.h>

#include "sha256.h"

#define SHFR(x, n) ((x) >> (n))
#define ROTR(x, n) (((x) >> (n)) | ((x) << ((sizeof(x) << 3) - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

#define SHA256_F1(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define SHA256_F2(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SHA256_F3(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ SHFR(x, 3))
#define SHA256_F4(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ SHFR(x, 10))

static const uint32_t sha256_h0[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(sha256_ctx *ctx, const uint8_t *data)
{
    uint32_t a, b, c, d, e, f, g, h, t1, t2, w[64];
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               ((uint32_t)data[i * 4 + 3]);
    for (i = 16; i < 64; i++)
        w[i] = SHA256_F4(w[i - 2]) + w[i - 7] + SHA256_F3(w[i - 15]) + w[i - 16];

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + SHA256_F2(e) + CH(e, f, g) + sha256_k[i] + w[i];
        t2 = SHA256_F1(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(sha256_ctx *ctx)
{
    int i;
    for (i = 0; i < 8; i++) ctx->state[i] = sha256_h0[i];
    ctx->total = 0;
}

void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len)
{
    size_t left = (size_t)(ctx->total & 63);
    size_t fill = 64 - left;

    ctx->total += len;

    if (left && len >= fill) {
        memcpy(ctx->buffer + left, data, fill);
        sha256_transform(ctx, ctx->buffer);
        data += fill;
        len -= fill;
        left = 0;
    }
    while (len >= 64) {
        sha256_transform(ctx, data);
        data += 64;
        len -= 64;
    }
    if (len > 0) memcpy(ctx->buffer + left, data, len);
}

void sha256_final(sha256_ctx *ctx, uint8_t digest[32])
{
    uint8_t pad[72]; /* 0x80 + zero fill + 8-byte big-endian bit length */
    size_t last = (size_t)(ctx->total & 63);
    size_t padn = (last < 56) ? (56 - last) : (120 - last);
    uint64_t bits = ctx->total << 3;
    int i;

    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (i = 0; i < 8; i++)
        pad[padn + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(ctx, pad, padn + 8);

    for (i = 0; i < 8; i++) {
        digest[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void sha256(const uint8_t *data, size_t len, uint8_t digest[32])
{
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
}

#define HMAC_BLOCK 64

void hmac_sha256(const uint8_t *key, size_t keylen,
                 const uint8_t *msg, size_t msglen, uint8_t out[32])
{
    uint8_t kblock[HMAC_BLOCK], ipad[HMAC_BLOCK], opad[HMAC_BLOCK], khash[32];
    sha256_ctx ctx;
    int i;

    memset(kblock, 0, sizeof kblock);
    if (keylen > HMAC_BLOCK) {
        sha256(key, keylen, khash);
        memcpy(kblock, khash, 32);
    } else {
        memcpy(kblock, key, keylen);
    }
    for (i = 0; i < HMAC_BLOCK; i++) {
        ipad[i] = kblock[i] ^ 0x36;
        opad[i] = kblock[i] ^ 0x5c;
    }
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, HMAC_BLOCK);
    sha256_update(&ctx, msg, msglen);
    sha256_final(&ctx, khash);
    sha256_init(&ctx);
    sha256_update(&ctx, opad, HMAC_BLOCK);
    sha256_update(&ctx, khash, 32);
    sha256_final(&ctx, out);
}
