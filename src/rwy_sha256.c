/*
 * src/rwy_sha256.c — SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 (FIPS 180-4, RFC 2104, RFC 8018).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "rwy_crypto.h"
#include "rwy_crypto_internal.h"

static const uint32_t K256[64] = {
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

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROR(x, 2) ^ ROR(x, 13) ^ ROR(x, 22))
#define EP1(x) (ROR(x, 6) ^ ROR(x, 11) ^ ROR(x, 25))
#define SIG0(x) (ROR(x, 7) ^ ROR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROR(x, 17) ^ ROR(x, 19) ^ ((x) >> 10))

static void sha256_transform(rwy_sha256_ctx_t *ctx, const uint8_t data[64]) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    int i;
    for (i = 0; i < 16; ++i) {
        m[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               ((uint32_t)data[i * 4 + 3]);
    }
    for (; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e, f, g) + K256[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void rwy_sha256_init(rwy_sha256_ctx_t *ctx) {
    ctx->count = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

void rwy_sha256_update(rwy_sha256_ctx_t *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    for (i = 0; i < len; ++i) {
        ctx->buf[ctx->count % 64] = p[i];
        ctx->count++;
        if ((ctx->count % 64) == 0)
            sha256_transform(ctx, ctx->buf);
    }
}

void rwy_sha256_final(rwy_sha256_ctx_t *ctx, uint8_t out[32]) {
    uint8_t pad[64];
    uint64_t total_bits = ctx->count * 8;
    size_t rem = ctx->count % 64;
    size_t pad_len = (rem < 56) ? (56 - rem) : (120 - rem);
    int i;

    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    rwy_sha256_update(ctx, pad, pad_len);

    for (i = 0; i < 8; ++i)
        pad[i] = (uint8_t)(total_bits >> ((7 - i) * 8));
    rwy_sha256_update(ctx, pad, 8);

    for (i = 0; i < 4; ++i) {
        out[i]      = (uint8_t)(ctx->state[0] >> ((3 - i) * 8));
        out[i + 4]  = (uint8_t)(ctx->state[1] >> ((3 - i) * 8));
        out[i + 8]  = (uint8_t)(ctx->state[2] >> ((3 - i) * 8));
        out[i + 12] = (uint8_t)(ctx->state[3] >> ((3 - i) * 8));
        out[i + 16] = (uint8_t)(ctx->state[4] >> ((3 - i) * 8));
        out[i + 20] = (uint8_t)(ctx->state[5] >> ((3 - i) * 8));
        out[i + 24] = (uint8_t)(ctx->state[6] >> ((3 - i) * 8));
        out[i + 28] = (uint8_t)(ctx->state[7] >> ((3 - i) * 8));
    }
}

void rwy_builtin_sha256(const void *data, size_t len, uint8_t out[32]) {
    rwy_sha256_ctx_t ctx;
    rwy_sha256_init(&ctx);
    rwy_sha256_update(&ctx, data, len);
    rwy_sha256_final(&ctx, out);
}

__attribute__((weak)) void rwy_sha256(const void *data, size_t len, uint8_t out[32]) {
    rwy_builtin_sha256(data, len, out);
}

_Static_assert(sizeof(rwy_sha256_ctx_t) <= sizeof(rwy_sha256_stream_t), "stream state too small");

int rwy_builtin_sha256_init(rwy_sha256_stream_t *s) {
    rwy_sha256_init((rwy_sha256_ctx_t *)s);
    return RWY_CRYPTO_OK;
}

void rwy_builtin_sha256_update(rwy_sha256_stream_t *s, const void *data, size_t len) {
    rwy_sha256_update((rwy_sha256_ctx_t *)s, data, len);
}

void rwy_builtin_sha256_final(rwy_sha256_stream_t *s, uint8_t out[32]) {
    rwy_sha256_final((rwy_sha256_ctx_t *)s, out);
}

void rwy_builtin_hmac_sha256(const void *key, size_t key_len,
                                const void *data, size_t data_len,
                                uint8_t out[32]) {
    rwy_sha256_ctx_t ctx;
    uint8_t k[64];
    uint8_t ipad[64];
    uint8_t opad[64];
    uint8_t inner[32];
    size_t i;

    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        rwy_builtin_sha256(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }

    for (i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    rwy_sha256_init(&ctx);
    rwy_sha256_update(&ctx, ipad, 64);
    rwy_sha256_update(&ctx, data, data_len);
    rwy_sha256_final(&ctx, inner);

    rwy_sha256_init(&ctx);
    rwy_sha256_update(&ctx, opad, 64);
    rwy_sha256_update(&ctx, inner, 32);
    rwy_sha256_final(&ctx, out);
}

void rwy_builtin_pbkdf2_sha256(const char *passphrase, size_t pass_len,
                                  const uint8_t *salt, size_t salt_len,
                                  uint32_t iterations, uint8_t *out, size_t out_len) {
    uint32_t block_idx = 1;
    size_t pos = 0;
    uint8_t u[32];
    uint8_t f[32];
    uint8_t salt_block[256];
    size_t sb_len = salt_len + 4;

    if (sb_len > sizeof(salt_block)) return;

    memcpy(salt_block, salt, salt_len);

    while (pos < out_len) {
        uint32_t iter;
        size_t j;
        salt_block[salt_len]     = (uint8_t)(block_idx >> 24);
        salt_block[salt_len + 1] = (uint8_t)(block_idx >> 16);
        salt_block[salt_len + 2] = (uint8_t)(block_idx >> 8);
        salt_block[salt_len + 3] = (uint8_t)(block_idx);

        rwy_builtin_hmac_sha256(passphrase, pass_len, salt_block, sb_len, u);
        memcpy(f, u, 32);

        for (iter = 1; iter < iterations; ++iter) {
            rwy_builtin_hmac_sha256(passphrase, pass_len, u, 32, u);
            for (j = 0; j < 32; ++j)
                f[j] ^= u[j];
        }

        size_t copy_len = (out_len - pos < 32) ? (out_len - pos) : 32;
        memcpy(out + pos, f, copy_len);
        pos += copy_len;
        block_idx++;
    }
}
