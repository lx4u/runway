/*
 * src/rwy_crypto_internal.h — shared by the parts of the crypto code. Private.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_CRYPTO_INTERNAL_H
#define RWY_CRYPTO_INTERNAL_H

#include "rwy_crypto.h"

/* The built-in primitives, the entries of rwy_crypto_builtin_ops. */
void rwy_builtin_sha256(const void *data, size_t len, uint8_t out[32]);
int  rwy_builtin_sha256_init(rwy_sha256_stream_t *s);
void rwy_builtin_sha256_update(rwy_sha256_stream_t *s, const void *data, size_t len);
void rwy_builtin_sha256_final(rwy_sha256_stream_t *s, uint8_t out[32]);
void rwy_builtin_hmac_sha256(const void *key, size_t key_len, const void *data, size_t data_len,
                              uint8_t out[32]);
void rwy_builtin_pbkdf2_sha256(const char *passphrase, size_t pass_len, const uint8_t *salt,
                                size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len);
void rwy_builtin_xts_crypt(const rwy_xts_ctx_t *ctx, uint64_t sector_idx, const uint8_t *in,
                            uint8_t *out, size_t len, int encrypt);

#endif
