/*
 * src/rwy_xts.c — AES-256-XTS sector cipher (IEEE 1619, NIST SP 800-38E).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "rwy_crypto.h"
#include "rwy_aes_arch.h"
#include "rwy_crypto_internal.h"

static void xts_gf_mul(uint8_t t[16]) {
    uint8_t carry = 0;
    int i;
    for (i = 0; i < 16; ++i) {
        uint8_t next_carry = (t[i] >> 7) & 1;
        t[i] = (uint8_t)((t[i] << 1) | carry);
        carry = next_carry;
    }
    if (carry)
        t[0] ^= 0x87;
}

void rwy_xts_init(rwy_xts_ctx_t *ctx, const uint8_t key1[32], const uint8_t key2[32]) {
    rwy_aes256_set_encrypt_key(&ctx->key1_enc, key1);
    rwy_aes256_set_decrypt_key(&ctx->key1_dec, key1);
    rwy_aes256_set_encrypt_key(&ctx->key2_enc, key2);
    memcpy(ctx->key, key1, 32);
    memcpy(ctx->key + 32, key2, 32);
}

void rwy_xts_init_from_dek(rwy_xts_ctx_t *ctx, const uint8_t dek[32]) {
    uint8_t key1[32];
    uint8_t key2[32];
    uint8_t buf1[40];
    uint8_t buf2[41];

    memcpy(buf1, dek, 32);
    memcpy(buf1 + 32, "DATA_KEY", 8);
    rwy_sha256(buf1, 40, key1);

    memcpy(buf2, dek, 32);
    memcpy(buf2 + 32, "TWEAK_KEY", 9);
    rwy_sha256(buf2, 41, key2);

    rwy_xts_init(ctx, key1, key2);
}

static void builtin_xts_encrypt(const rwy_xts_ctx_t *ctx, uint64_t sector_idx,
                                const uint8_t *in, uint8_t *out, size_t len) {
    if (arch_aes_ok()) {
        arch_aes_xts_enc(ctx, sector_idx, in, out, len);
        return;
    }
    uint8_t tweak[16];
    uint8_t t[16];
    size_t blocks = len / 16;
    size_t b;

    memset(tweak, 0, 16);
    for (int i = 0; i < 8; ++i)
        tweak[i] = (uint8_t)(sector_idx >> (i * 8));

    rwy_aes256_encrypt_block(&ctx->key2_enc, tweak, t);

    for (b = 0; b < blocks; ++b) {
        uint8_t tmp[16];
        const uint8_t *p = in + b * 16;
        for (int i = 0; i < 16; ++i) tmp[i] = p[i] ^ t[i];
        rwy_aes256_encrypt_block(&ctx->key1_enc, tmp, tmp);
        for (int i = 0; i < 16; ++i) out[b * 16 + i] = tmp[i] ^ t[i];
        xts_gf_mul(t);
    }
}

static void builtin_xts_decrypt(const rwy_xts_ctx_t *ctx, uint64_t sector_idx,
                                const uint8_t *in, uint8_t *out, size_t len) {
    if (arch_aes_ok()) {
        arch_aes_xts_dec(ctx, sector_idx, in, out, len);
        return;
    }
    uint8_t tweak[16];
    uint8_t t[16];
    size_t blocks = len / 16;
    size_t b;

    memset(tweak, 0, 16);
    for (int i = 0; i < 8; ++i)
        tweak[i] = (uint8_t)(sector_idx >> (i * 8));

    rwy_aes256_encrypt_block(&ctx->key2_enc, tweak, t);

    for (b = 0; b < blocks; ++b) {
        uint8_t tmp[16];
        const uint8_t *p = in + b * 16;
        for (int i = 0; i < 16; ++i) tmp[i] = p[i] ^ t[i];
        rwy_aes256_decrypt_block(&ctx->key1_dec, tmp, tmp);
        for (int i = 0; i < 16; ++i) out[b * 16 + i] = tmp[i] ^ t[i];
        xts_gf_mul(t);
    }
}

void rwy_builtin_xts_crypt(const rwy_xts_ctx_t *ctx, uint64_t sector_idx, const uint8_t *in,
                              uint8_t *out, size_t len, int encrypt) {
    if (encrypt)
        builtin_xts_encrypt(ctx, sector_idx, in, out, len);
    else
        builtin_xts_decrypt(ctx, sector_idx, in, out, len);
}
