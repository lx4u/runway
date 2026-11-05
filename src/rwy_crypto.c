/*
 * src/rwy_crypto.c — crypto backend table and the dispatch of the primitives.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "rwy_crypto.h"
#include "rwy_crypto_internal.h"

const rwy_crypto_ops_t rwy_crypto_builtin_ops = {
    .sha256        = rwy_builtin_sha256,
    .sha256_init   = rwy_builtin_sha256_init,
    .sha256_update = rwy_builtin_sha256_update,
    .sha256_final  = rwy_builtin_sha256_final,
    .hmac_sha256   = rwy_builtin_hmac_sha256,
    .pbkdf2_sha256 = rwy_builtin_pbkdf2_sha256,
    .xts_crypt     = rwy_builtin_xts_crypt,
    .random        = NULL,
};

static const rwy_crypto_ops_t *g_ops = &rwy_crypto_builtin_ops;
static int (*g_random)(void *buf, size_t len);

void rwy_crypto_set_ops(const rwy_crypto_ops_t *ops) {
    g_ops = ops ? ops : &rwy_crypto_builtin_ops;
    g_random = NULL;
}

void rwy_crypto_set_random(int (*fn)(void *buf, size_t len)) {
    g_random = fn;
}

int rwy_random(void *buf, size_t len) {
    if (g_random)
        return g_random(buf, len);
    return g_ops->random ? g_ops->random(buf, len) : RWY_CRYPTO_ERR_EMPTY;
}

void rwy_sha256(const void *data, size_t len, uint8_t out[32]) {
    g_ops->sha256(data, len, out);
}

int rwy_sha256_stream_init(rwy_sha256_stream_t *s) {
    return g_ops->sha256_init(s);
}

void rwy_sha256_stream_update(rwy_sha256_stream_t *s, const void *data, size_t len) {
    g_ops->sha256_update(s, data, len);
}

void rwy_sha256_stream_final(rwy_sha256_stream_t *s, uint8_t out[32]) {
    g_ops->sha256_final(s, out);
}

void rwy_hmac_sha256(const void *key, size_t key_len, const void *data, size_t data_len,
                      uint8_t out[32]) {
    g_ops->hmac_sha256(key, key_len, data, data_len, out);
}

void rwy_pbkdf2_sha256(const char *passphrase, size_t pass_len, const uint8_t *salt,
                        size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len) {
    g_ops->pbkdf2_sha256(passphrase, pass_len, salt, salt_len, iterations, out, out_len);
}

void rwy_xts_encrypt_sector(const rwy_xts_ctx_t *ctx, uint64_t sector_idx,
                             const uint8_t *in, uint8_t *out, size_t len) {
    g_ops->xts_crypt(ctx, sector_idx, in, out, len, 1);
}

void rwy_xts_decrypt_sector(const rwy_xts_ctx_t *ctx, uint64_t sector_idx,
                             const uint8_t *in, uint8_t *out, size_t len) {
    g_ops->xts_crypt(ctx, sector_idx, in, out, len, 0);
}
