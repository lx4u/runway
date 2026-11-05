/*
 * tools/crypto_openssl.c — OpenSSL (libcrypto) backend of the crypto table for the host tools.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The on-disk algorithms are the same as the built-in code, so both give the same bytes.
 */
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "crypto_openssl.h"

static void ossl_sha256(const void *data, size_t len, uint8_t out[32]) {
    SHA256(data, len, out);
}

/* The state is a pointer to an EVP context: the one-shot SHA256() is not deprecated, the SHA256_Init family is */
_Static_assert(sizeof(EVP_MD_CTX *) <= sizeof(rwy_sha256_stream_t), "stream state too small");

static int ossl_sha256_init(rwy_sha256_stream_t *s) {
    EVP_MD_CTX *c = EVP_MD_CTX_new();

    memcpy(s, &c, sizeof(c));
    if (!c || !EVP_DigestInit_ex(c, EVP_sha256(), NULL)) return RWY_CRYPTO_ERR_IO;
    return RWY_CRYPTO_OK;
}

static void ossl_sha256_update(rwy_sha256_stream_t *s, const void *data, size_t len) {
    EVP_MD_CTX *c;

    memcpy(&c, s, sizeof(c));
    if (c) EVP_DigestUpdate(c, data, len);
}

static void ossl_sha256_final(rwy_sha256_stream_t *s, uint8_t out[32]) {
    EVP_MD_CTX *c;
    unsigned int n = 32;

    memcpy(&c, s, sizeof(c));
    if (c) EVP_DigestFinal_ex(c, out, &n);
    EVP_MD_CTX_free(c);
    c = NULL;
    memcpy(s, &c, sizeof(c));
}

static void ossl_hmac_sha256(const void *key, size_t key_len, const void *data, size_t data_len,
                             uint8_t out[32]) {
    unsigned int n = 32;
    HMAC(EVP_sha256(), key_len ? key : "", (int)key_len, data, data_len, out, &n);
}

static void ossl_pbkdf2_sha256(const char *passphrase, size_t pass_len, const uint8_t *salt,
                               size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len) {
    PKCS5_PBKDF2_HMAC(passphrase, (int)pass_len, salt, (int)salt_len, (int)iterations,
                      EVP_sha256(), (int)out_len, out);
}

/* The tweak is the sector number in 16 bytes, little endian: the same as the built-in code */
static void ossl_xts_crypt(const rwy_xts_ctx_t *ctx, uint64_t sector_idx, const uint8_t *in,
                           uint8_t *out, size_t len, int encrypt) {
    static __thread EVP_CIPHER_CTX *c;
    uint8_t iv[16] = { 0 };
    int n = 0, m = 0;

    for (int i = 0; i < 8; i++) iv[i] = (uint8_t)(sector_idx >> (i * 8));
    if (!c) c = EVP_CIPHER_CTX_new();
    if (!c || !EVP_CipherInit_ex(c, EVP_aes_256_xts(), NULL, ctx->key, iv, encrypt) ||
        !EVP_CipherUpdate(c, out, &n, in, (int)len) || !EVP_CipherFinal_ex(c, out + n, &m))
        abort();
}

static int ossl_random(void *buf, size_t len) {
    return RAND_bytes(buf, (int)len) == 1 ? RWY_CRYPTO_OK : RWY_CRYPTO_ERR_EMPTY;
}

const rwy_crypto_ops_t rwy_crypto_openssl_ops = {
    .sha256        = ossl_sha256,
    .sha256_init   = ossl_sha256_init,
    .sha256_update = ossl_sha256_update,
    .sha256_final  = ossl_sha256_final,
    .hmac_sha256   = ossl_hmac_sha256,
    .pbkdf2_sha256 = ossl_pbkdf2_sha256,
    .xts_crypt     = ossl_xts_crypt,
    .random        = ossl_random,
};

void rwy_crypto_host_init(void) {
    const char *want = getenv("RWY_CRYPTO");

    if (want && !strcmp(want, "builtin")) return;
    rwy_crypto_set_ops(&rwy_crypto_openssl_ops);
}
