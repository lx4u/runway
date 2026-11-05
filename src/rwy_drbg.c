/*
 * src/rwy_drbg.c — seeded HMAC-DRBG (NIST SP 800-90A, SHA-256) for reproducible images.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tools and tests only: it is not linked into RUNWAY.EFI. The output depends on the seed alone,
 * so it must never be the default source of a store. There is no reseed counter: a run draws a
 * few hundred bytes.
 */
#include <string.h>

#include "rwy_crypto.h"

static uint8_t g_k[32];
static uint8_t g_v[32];

#define SEED_MAX 256

static void drbg_update(const uint8_t *data, size_t len) {
    uint8_t buf[32 + 1 + SEED_MAX];
    uint8_t tmp[32];
    uint8_t pass;

    for (pass = 0; pass < (len ? 2 : 1); pass++) {
        memcpy(buf, g_v, 32);
        buf[32] = pass;
        if (len) memcpy(buf + 33, data, len);
        rwy_hmac_sha256(g_k, 32, buf, 33 + len, tmp);
        memcpy(g_k, tmp, 32);
        rwy_hmac_sha256(g_k, 32, g_v, 32, tmp);
        memcpy(g_v, tmp, 32);
    }
}

static int drbg_generate(void *out, size_t len) {
    uint8_t *p = out;

    while (len) {
        size_t n = len < 32 ? len : 32;
        uint8_t tmp[32];
        rwy_hmac_sha256(g_k, 32, g_v, 32, tmp);
        memcpy(g_v, tmp, 32);
        memcpy(p, g_v, n);
        p += n;
        len -= n;
    }
    drbg_update(NULL, 0);
    return RWY_CRYPTO_OK;
}

int rwy_drbg_seed(const void *seed, size_t len) {
    if (!seed || !len || len > SEED_MAX) return RWY_CRYPTO_ERR_INVAL;
    memset(g_k, 0x00, sizeof(g_k));
    memset(g_v, 0x01, sizeof(g_v));
    drbg_update(seed, len);
    rwy_crypto_set_random(drbg_generate);
    return RWY_CRYPTO_OK;
}
