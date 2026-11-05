/*
 * src/rwy_aes_arch.h — AES code of the CPU we run on, when there is one. Private.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The arch_aes_*() functions may only be used when arch_aes_ok() is true; otherwise (also after
 * rwy_crypto_set_portable(1)) the portable C code runs. A new CPU adds its own header and one more branch here.
 */
#ifndef RWY_AES_ARCH_H
#define RWY_AES_ARCH_H

#include "rwy_crypto.h"

extern int rwy_aes_portable;

#if defined(__x86_64__) || defined(_M_X64)

#include "rwy_aesni.h"

static inline int arch_aes_ok(void) {
    return !rwy_aes_portable && rwy_has_aesni();
}

static inline void arch_aes_set_enc_key(rwy_aes256_ctx_t *ctx, const uint8_t key[32]) {
    rwy_aesni_expand_enc_keys(key, (uint8_t *)ctx->rk);
}

static inline void arch_aes_set_dec_key(rwy_aes256_ctx_t *ctx, const uint8_t key[32]) {
    uint8_t enc_rk[240];

    rwy_aesni_expand_enc_keys(key, enc_rk);
    rwy_aesni_expand_dec_keys(enc_rk, (uint8_t *)ctx->rk);
}

static inline void arch_aes_enc_block(const rwy_aes256_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]) {
    rwy_aesni_enc_block(ctx->rk, in, out);
}

static inline void arch_aes_dec_block(const rwy_aes256_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]) {
    rwy_aesni_dec_block(ctx->rk, in, out);
}

static inline void arch_aes_xts_enc(const rwy_xts_ctx_t *ctx, uint64_t sector_idx, const uint8_t *in,
                                    uint8_t *out, size_t len) {
    rwy_aesni_xts_enc_sector((const uint8_t *)ctx->key1_enc.rk, (const uint8_t *)ctx->key2_enc.rk,
                              sector_idx, in, out, len);
}

static inline void arch_aes_xts_dec(const rwy_xts_ctx_t *ctx, uint64_t sector_idx, const uint8_t *in,
                                    uint8_t *out, size_t len) {
    rwy_aesni_xts_dec_sector((const uint8_t *)ctx->key1_dec.rk, (const uint8_t *)ctx->key2_enc.rk,
                              sector_idx, in, out, len);
}

#elif defined(__aarch64__)

#include "rwy_aesarm.h"

/* The wrappers call the intrinsics, so they carry the same target */
#pragma GCC push_options
#pragma GCC target("+crypto")

static inline int arch_aes_ok(void) {
    return !rwy_aes_portable && rwy_has_armaes();
}

static inline void arch_aes_set_enc_key(rwy_aes256_ctx_t *ctx, const uint8_t key[32]) {
    rwy_armaes_expand_enc_keys(key, (uint8_t *)ctx->rk);
}

static inline void arch_aes_set_dec_key(rwy_aes256_ctx_t *ctx, const uint8_t key[32]) {
    uint8_t enc_rk[240];

    rwy_armaes_expand_enc_keys(key, enc_rk);
    rwy_armaes_expand_dec_keys(enc_rk, (uint8_t *)ctx->rk);
}

static inline void arch_aes_enc_block(const rwy_aes256_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]) {
    rwy_armaes_enc_block(ctx->rk, in, out);
}

static inline void arch_aes_dec_block(const rwy_aes256_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]) {
    rwy_armaes_dec_block(ctx->rk, in, out);
}

static inline void arch_aes_xts_enc(const rwy_xts_ctx_t *ctx, uint64_t sector_idx, const uint8_t *in,
                                    uint8_t *out, size_t len) {
    rwy_armaes_xts_sector((const uint8_t *)ctx->key1_enc.rk, (const uint8_t *)ctx->key2_enc.rk,
                          sector_idx, in, out, len, 1);
}

static inline void arch_aes_xts_dec(const rwy_xts_ctx_t *ctx, uint64_t sector_idx, const uint8_t *in,
                                    uint8_t *out, size_t len) {
    rwy_armaes_xts_sector((const uint8_t *)ctx->key1_dec.rk, (const uint8_t *)ctx->key2_enc.rk,
                          sector_idx, in, out, len, 0);
}

#pragma GCC pop_options

#else

static inline int arch_aes_ok(void) { return 0; }
static inline void arch_aes_set_enc_key(rwy_aes256_ctx_t *ctx, const uint8_t key[32]) { (void)ctx; (void)key; }
static inline void arch_aes_set_dec_key(rwy_aes256_ctx_t *ctx, const uint8_t key[32]) { (void)ctx; (void)key; }
static inline void arch_aes_enc_block(const rwy_aes256_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]) {
    (void)ctx; (void)in; (void)out;
}
static inline void arch_aes_dec_block(const rwy_aes256_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]) {
    (void)ctx; (void)in; (void)out;
}
static inline void arch_aes_xts_enc(const rwy_xts_ctx_t *ctx, uint64_t sector_idx, const uint8_t *in,
                                    uint8_t *out, size_t len) {
    (void)ctx; (void)sector_idx; (void)in; (void)out; (void)len;
}
static inline void arch_aes_xts_dec(const rwy_xts_ctx_t *ctx, uint64_t sector_idx, const uint8_t *in,
                                    uint8_t *out, size_t len) {
    (void)ctx; (void)sector_idx; (void)in; (void)out; (void)len;
}

#endif

#endif
