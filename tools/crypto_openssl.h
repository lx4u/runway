/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RWY_CRYPTO_OPENSSL_H
#define RWY_CRYPTO_OPENSSL_H

#include "rwy_crypto.h"

extern const rwy_crypto_ops_t rwy_crypto_openssl_ops;

/* Use OpenSSL for the crypto table, unless RWY_CRYPTO=builtin is set. Call once at start-up. */
void rwy_crypto_host_init(void);


#endif
