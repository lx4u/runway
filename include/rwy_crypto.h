/*
 * rwy_crypto — block-layer encryption, keyslot management, and crypto block dev.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implements:
 *  - Standalone, portable SHA-256 and HMAC-SHA256
 *  - Standalone, portable PBKDF2-HMAC-SHA256
 *  - Standalone, portable AES-256 (encrypt & decrypt)
 *  - Standalone, portable AES-256-XTS sector cipher (IEEE 1619 / NIST SP 800-38E)
 *  - Keyslot management (Slot 0: password, Slot 1: TPM2, Slot 2: recovery)
 *  - rwy_block_dev_t crypto wrapper operating strictly below the Runway core
 */
#ifndef RWY_CRYPTO_H
#define RWY_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_format.h"

#define RWY_KEY_SIZE_BYTES 32U

/* Error codes for crypto operations */
#define RWY_CRYPTO_OK           0
#define RWY_CRYPTO_ERR_INVAL   -1
#define RWY_CRYPTO_ERR_AUTH    -2
#define RWY_CRYPTO_ERR_IO      -3
#define RWY_CRYPTO_ERR_CORRUPT -4
#define RWY_CRYPTO_ERR_EMPTY   -5

typedef struct rwy_xts_ctx rwy_xts_ctx_t;

/*
 * State of one streaming SHA-256. It is opaque and holds what the backend that started it needs, so
 * finish it with rwy_sha256_stream_final (which also releases it) before the table is replaced.
 */
typedef struct {
    uint64_t opaque[16];
} rwy_sha256_stream_t;

/*
 * Crypto backend table. The built-in code is the default and the reference; a host tool
 * may replace the table at start-up. The on-disk algorithms do not change, so every backend
 * must return the same bytes.
 */
typedef struct {
    void (*sha256)(const void *data, size_t len, uint8_t out[32]);
    /* Streaming form of sha256. init returns 0 or a RWY_CRYPTO_ERR_* code; final always releases the state */
    int  (*sha256_init)(rwy_sha256_stream_t *s);
    void (*sha256_update)(rwy_sha256_stream_t *s, const void *data, size_t len);
    void (*sha256_final)(rwy_sha256_stream_t *s, uint8_t out[32]);
    void (*hmac_sha256)(const void *key, size_t key_len, const void *data, size_t data_len,
                        uint8_t out[32]);
    void (*pbkdf2_sha256)(const char *passphrase, size_t pass_len, const uint8_t *salt,
                          size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len);
    /* One XTS sector with the keys of ctx->key (encrypt != 0) or its inverse */
    void (*xts_crypt)(const rwy_xts_ctx_t *ctx, uint64_t sector_idx, const uint8_t *in,
                      uint8_t *out, size_t len, int encrypt);
    /* Random bytes from the operating system; 0 or a RWY_CRYPTO_ERR_* code. NULL: no source */
    int  (*random)(void *buf, size_t len);
} rwy_crypto_ops_t;

extern const rwy_crypto_ops_t rwy_crypto_builtin_ops;

/* Replace the table (NULL restores the built-in code). Call before any other crypto call. */
void rwy_crypto_set_ops(const rwy_crypto_ops_t *ops);

/* Replace only the random source (NULL restores the one of the table); used by a seeded generator */
void rwy_crypto_set_random(int (*fn)(void *buf, size_t len));

/* Run the portable AES code even where the CPU has its own (tests; checks of the code that arm64 runs, on x86_64).
 * The round keys differ between the two: set it before any key is set up, and do not mix contexts made under the
 * two settings. */
void rwy_crypto_set_portable(int portable);

/* Nonzero when AES runs on the CPU's own instructions (AES-NI, ARMv8), zero for the portable code */
int rwy_crypto_hw_aes(void);

/* Random bytes; RWY_CRYPTO_ERR_EMPTY when there is no source (RUNWAY.EFI has none) */
int rwy_random(void *buf, size_t len);

/*
 * Seeded generator (src/rwy_drbg.c, host tools and tests only, not in RUNWAY.EFI): replaces the random
 * source so the output depends on the seed alone. For reproducible images; never the default.
 */
int rwy_drbg_seed(const void *seed, size_t len);

/* --- SHA-256 & HMAC --- */
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buf[64];
} rwy_sha256_ctx_t;

void rwy_sha256_init(rwy_sha256_ctx_t *ctx);
void rwy_sha256_update(rwy_sha256_ctx_t *ctx, const void *data, size_t len);
void rwy_sha256_final(rwy_sha256_ctx_t *ctx, uint8_t out[32]);
void rwy_sha256(const void *data, size_t len, uint8_t out[32]);

/* The same hash in pieces, through the table. Call final even after an error, to release the state. */
int  rwy_sha256_stream_init(rwy_sha256_stream_t *s);
void rwy_sha256_stream_update(rwy_sha256_stream_t *s, const void *data, size_t len);
void rwy_sha256_stream_final(rwy_sha256_stream_t *s, uint8_t out[32]);

void rwy_hmac_sha256(const void *key, size_t key_len,
                      const void *data, size_t data_len,
                      uint8_t out[32]);

void rwy_pbkdf2_sha256(const char *passphrase, size_t pass_len,
                        const uint8_t *salt, size_t salt_len,
                        uint32_t iterations, uint8_t *out, size_t out_len);

/* --- AES-256 --- */
typedef struct {
    uint32_t rk[60]; /* 15 round keys */
} rwy_aes256_ctx_t;

void rwy_aes256_set_encrypt_key(rwy_aes256_ctx_t *ctx, const uint8_t key[32]);
void rwy_aes256_set_decrypt_key(rwy_aes256_ctx_t *ctx, const uint8_t key[32]);
void rwy_aes256_encrypt_block(const rwy_aes256_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]);
void rwy_aes256_decrypt_block(const rwy_aes256_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]);

/* --- AES-256-XTS --- */
struct rwy_xts_ctx {
    rwy_aes256_ctx_t key1_enc;
    rwy_aes256_ctx_t key1_dec;
    rwy_aes256_ctx_t key2_enc;
    uint8_t           key[64]; /* key1 || key2, for backends that bring their own cipher */
};

void rwy_xts_init(rwy_xts_ctx_t *ctx, const uint8_t key1[32], const uint8_t key2[32]);
void rwy_xts_init_from_dek(rwy_xts_ctx_t *ctx, const uint8_t dek[32]);
void rwy_xts_encrypt_sector(const rwy_xts_ctx_t *ctx, uint64_t sector_idx,
                             const uint8_t *in, uint8_t *out, size_t len);
void rwy_xts_decrypt_sector(const rwy_xts_ctx_t *ctx, uint64_t sector_idx,
                             const uint8_t *in, uint8_t *out, size_t len);

/* --- Keyslot management --- */
int rwy_keyslot_init_header(rwy_keyslot_header_t *hdr);
int rwy_keyslot_set_password(rwy_keyslot_header_t *hdr, uint32_t slot_idx,
                             const char *passphrase, const uint8_t dek[32],
                             uint32_t iterations);
int rwy_keyslot_set_tpm2_blob(rwy_keyslot_header_t *hdr, uint32_t slot_idx,
                              const uint8_t *blob, uint32_t blob_len,
                              const uint8_t pcr_mask[32]);
int rwy_keyslot_set_recovery_key(rwy_keyslot_header_t *hdr, uint32_t slot_idx,
                                 const char *recovery_pass, const uint8_t dek[32]);
int rwy_keyslot_unwrap(const rwy_keyslot_header_t *hdr, const char *passphrase,
                        uint8_t out_dek[32]);
int rwy_keyslot_read_from_dev(const rwy_block_dev_t *dev, rwy_keyslot_header_t *out_hdr);
/* Put the header in both superblock slots, each as one whole-block write. */
int rwy_keyslot_write_to_dev(const rwy_block_dev_t *dev, const rwy_keyslot_header_t *hdr);

/* --- Crypto block device wrapper --- */
/* Blocks encrypted per backend write. The buffer lives in the context, so a write cannot fail to allocate. */
#define RWY_CRYPTO_BOUNCE_BLOCKS 32

typedef struct {
    rwy_block_dev_t underlying;
    rwy_xts_ctx_t   xts;
    uint8_t          dek[32];
    uint32_t         start_block;
    uint32_t         plain_flight_start;
    uint32_t         plain_flight_blocks;
    uint32_t         plain_recov_start;
    uint32_t         plain_recov_blocks;
    uint32_t         plain_loader_start;
    uint32_t         plain_loader_blocks;
    uint32_t         plain_net_start;
    uint32_t         plain_net_blocks;
    uint8_t          bounce[RWY_CRYPTO_BOUNCE_BLOCKS * RWY_BLOCK_SIZE];
} rwy_crypto_dev_ctx_t;

int rwy_crypto_dev_wrap(rwy_block_dev_t *out_dev, rwy_crypto_dev_ctx_t *ctx,
                        const rwy_block_dev_t *underlying, const uint8_t dek[32],
                        uint32_t start_block);
int rwy_crypto_dev_rekey(rwy_crypto_dev_ctx_t *ctx, const uint8_t dek[32]);

/* Re-encrypt a store with a new DEK and keyslot header. */
int rwy_reencrypt(rwy_store_t *s, const rwy_block_dev_t *raw_dev,
                   const uint8_t old_dek[32], const uint8_t new_dek[32],
                   const rwy_keyslot_header_t *keyhdr);
int rwy_reencrypt_relocate(rwy_store_t *s, const rwy_block_dev_t *raw_dev,
                            const uint8_t old_dek[32], const uint8_t new_dek[32],
                            const rwy_keyslot_header_t *keyhdr);
int rwy_reencrypt_pending(rwy_store_t *s, int *out_pending);
int rwy_reencrypt_finish(rwy_store_t *s, const uint8_t old_dek[32]);

#endif /* RWY_CRYPTO_H */
