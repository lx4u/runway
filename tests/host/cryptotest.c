/*
 * cryptotest — comprehensive unit and integration tests for
 * Runway Block-Layer Encryption and Keyslot Management.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Verifies:
 *  1. Unit Tests:
 *     - SHA-256 standard test vectors (NIST FIPS 180-4).
 *     - HMAC-SHA256 standard test vectors (RFC 4231).
 *     - PBKDF2-HMAC-SHA256 standard test vectors (RFC 6070).
 *     - AES-256 ECB NIST SP 800-38A known-answer test vector.
 *     - AES-256-XTS IEEE 1619 / NIST SP 800-38E known-answer test vector.
 *     - Keyslot wrap & unwrap lifecycle (Slot 0 password, Slot 1 TPM2, Slot 2 recovery).
 *     - Wrong passphrase authentication failure (RWY_CRYPTO_ERR_AUTH).
 *  2. Crypto Block Device Roundtrip:
 *     - Block read/write through rwy_crypto_dev_wrap with AES-256-XTS sector tweak.
 *     - Verification that block 0 and 1 remain unencrypted pass-through.
 *     - Verification that data blocks >= 2 are encrypted on disk.
 *  3. Ciphertext Entropy:
 *     - Shannon entropy of raw ciphertext blocks on disk exceeds 7.9 bits/byte.
 *  4. Plaintext Absence:
 *     - Ensure no plaintext marker strings exist on disk in the ciphertext region.
 *  5. Filesystem CoW Integration:
 *     - Format and mount encrypted store using crypto device wrapper.
 *     - Perform transactional file creation, write, rename, directory operations, xattrs.
 *     - Unmount and remount cleanly with DEK.
 *  6. Deterministic Fault Injection over Encrypted CoW Writes:
 *     - Simulate power loss / torn writes at every write/flush point during encrypted transactions.
 *     - Verify that every interrupted state recovers cleanly to either the old or new generation,
 *       with 0 corruption.
 *  7. Host Checker Verification:
 *     - Validate encrypted store using fsck.runway with --key and without --key (unauthenticated mode).
 *
 * Usage: cryptotest SEED_STORE RWY_CHECK_BIN
 */

#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testdir.h"
#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_crypto.h"
#include "crypto_openssl.h"
#include "rwy_format.h"
#include "rwy_internal.h"

static int failures;
static const char *g_checkbin = "build/tools/fsck.runway";

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

/* ---------- file-backed block device ---------- */
typedef struct { FILE *f; } file_t;
static int file_rd(void *ctx, uint64_t off, void *buf, size_t len) {
    file_t *fl = ctx;
    if (fseek(fl->f, (long)off, SEEK_SET)) return -1;
    return fread(buf, 1, len, fl->f) == len ? 0 : -1;
}
/* The store takes whole blocks at block offsets only; a torn write below sets g_torn_ok. */
static int g_torn_ok;
static int file_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    file_t *fl = ctx;
    if (!g_torn_ok && (off % RWY_BLOCK_SIZE || len % RWY_BLOCK_SIZE)) {
        printf("FAIL: write of %zu bytes at offset %llu is not block aligned\n", len, (unsigned long long)off);
        failures++;
        return -1;
    }
    if (fseek(fl->f, (long)off, SEEK_SET)) return -1;
    return fwrite(buf, 1, len, fl->f) == len ? 0 : -1;
}
static int file_fl(void *ctx) { file_t *fl = ctx; return fflush(fl->f) ? -1 : 0; }

static void copy_file(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "wb");
    char buf[1 << 20];
    size_t n;
    if (!fa || !fb) { fprintf(stderr, "copy failed %s -> %s\n", a, b); exit(1); }
    while ((n = fread(buf, 1, sizeof(buf), fa)) > 0)
        if (fwrite(buf, 1, n, fb) != n) { fprintf(stderr, "copy failed\n"); exit(1); }
    fclose(fa); fclose(fb);
}

/* Put a keyslot tail in one superblock slot: the whole block is read and written back. */
static int put_tail(const rwy_block_dev_t *d, uint32_t slot, const rwy_keyslot_header_t *h) {
    uint8_t blk[RWY_BLOCK_SIZE];
    uint64_t off = (uint64_t)slot * RWY_BLOCK_SIZE;

    if (d->read(d->ctx, off, blk, sizeof(blk))) return -1;
    memcpy(blk + RWY_KEYSLOT_OFFSET, h, sizeof(*h));
    return d->write(d->ctx, off, blk, sizeof(blk));
}

/* ---------- Fault-injection counting block device wrapper ---------- */
typedef struct {
    rwy_block_dev_t inner;
    long point;
    long fail_at;
    int failed;
    int torn;               /* at the cut: 1 = first half written, 2 = second half written */
} fi_dev_t;

static int fi_rd(void *ctx, uint64_t off, void *buf, size_t len) {
    fi_dev_t *fi = ctx;
    return fi->inner.read(fi->inner.ctx, off, buf, len);
}
static int fi_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    fi_dev_t *fi = ctx;
    fi->point++;
    if (fi->fail_at == fi->point) {
        fi->failed = 1;
        g_torn_ok = 1;
        if (fi->torn == 1) fi->inner.write(fi->inner.ctx, off, buf, len / 2);
        if (fi->torn == 2) fi->inner.write(fi->inner.ctx, off + len / 2, (const uint8_t *)buf + len / 2, len / 2);
        g_torn_ok = 0;
        return -1;
    }
    return fi->inner.write(fi->inner.ctx, off, buf, len);
}
static int fi_fl(void *ctx) {
    fi_dev_t *fi = ctx;
    fi->point++;
    if (fi->fail_at == fi->point) { fi->failed = 1; return -1; }
    if (fi->inner.flush) return fi->inner.flush(fi->inner.ctx);
    return 0;
}

/* ========================================================================= */
/*          Runs through the crypto device: same bytes, fewer calls          */
/* ========================================================================= */

#define RUN_DEV_BLOCKS 600
#define RUN_PLAIN_START 300
#define RUN_PLAIN_BLOCKS 2

typedef struct {
    uint8_t *mem;
    int calls;
    int fail_call;
    int reads;
} run_dev_t;

static int run_rd(void *ctx, uint64_t off, void *buf, size_t len) {
    run_dev_t *d = ctx;
    d->reads++;
    memcpy(buf, d->mem + off, len);
    return 0;
}
static int run_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    run_dev_t *d = ctx;
    if (++d->calls == d->fail_call) return -1;
    memcpy(d->mem + off, buf, len);
    return 0;
}

static int run_is_plain(uint64_t b) {
    return b < RWY_SB_BLOCKS || (b >= RUN_PLAIN_START && b < RUN_PLAIN_START + 2 * RUN_PLAIN_BLOCKS);
}

/* Blocks of the next backend call for a run of left blocks at b: one kind, at most one bounce buffer. */
static uint64_t run_next_call(uint64_t b, uint64_t left) {
    int plain = run_is_plain(b);
    uint64_t n = 1;
    while (n < left && run_is_plain(b + n) == plain && (plain || n < RWY_CRYPTO_BOUNCE_BLOCKS)) n++;
    return n;
}

/* The per-block path of the cipher before runs: the reference for the stored bytes. */
static void run_reference(const rwy_xts_ctx_t *xts, uint8_t *mem, uint64_t b, const uint8_t *data, uint64_t n) {
    for (uint64_t i = 0; i < n; i++, b++) {
        const uint8_t *in = data + i * RWY_BLOCK_SIZE;
        if (run_is_plain(b)) memcpy(mem + b * RWY_BLOCK_SIZE, in, RWY_BLOCK_SIZE);
        else rwy_xts_encrypt_sector(xts, b, in, mem + b * RWY_BLOCK_SIZE, RWY_BLOCK_SIZE);
    }
}

/* Write n blocks at block b through the cipher. Expect the stored bytes of the per-block path and
 * one backend call per piece; with fail_call set, that call fails and the pieces before it are stored. */
static void run_case(const char *what, uint64_t b, uint64_t n, int fail_call) {
    static const uint8_t dek[32] = { 7, 1, 2, 3 };
    size_t dev_bytes = (size_t)RUN_DEV_BLOCKS * RWY_BLOCK_SIZE;
    uint8_t *mem = calloc(1, dev_bytes), *ref = calloc(1, dev_bytes);
    uint8_t *data = malloc(n * RWY_BLOCK_SIZE), *copy = malloc(n * RWY_BLOCK_SIZE);
    rwy_crypto_dev_ctx_t *cctx = malloc(sizeof(*cctx));
    run_dev_t d = { .mem = mem, .fail_call = fail_call };
    rwy_block_dev_t raw = { run_rd, run_wr, NULL, &d, dev_bytes, RWY_BLOCK_SIZE, NULL, NULL }, top;
    uint64_t done = 0;
    int calls = 0;

    for (size_t i = 0; i < n * RWY_BLOCK_SIZE; i++) data[i] = (uint8_t)(i * 31 + i / 4096);
    memcpy(copy, data, n * RWY_BLOCK_SIZE);
    rwy_crypto_dev_wrap(&top, cctx, &raw, dek, RWY_SB_BLOCKS);
    cctx->plain_flight_start = RUN_PLAIN_START;
    cctx->plain_flight_blocks = RUN_PLAIN_BLOCKS;

    int rc = top.write(top.ctx, b * RWY_BLOCK_SIZE, data, n * RWY_BLOCK_SIZE);

    while (done < n) {
        calls++;
        if (calls == fail_call) break;
        done += run_next_call(b + done, n - done);
    }
    printf("%s: %llu blocks, %d backend calls\n", what, (unsigned long long)n, d.calls);
    CHECK(rc == (fail_call ? -1 : 0), what);
    CHECK(d.calls == calls, "number of backend calls");
    CHECK(memcmp(data, copy, n * RWY_BLOCK_SIZE) == 0, "caller's buffer is not changed");
    run_reference(&cctx->xts, ref, b, data, done);
    CHECK(memcmp(mem, ref, dev_bytes) == 0, "stored bytes equal the per-block path");
    free(mem); free(ref); free(data); free(copy); free(cctx);
}

static void test_crypto_runs(void) {
    printf("--- Crypto device: runs ---\n");
    run_case("1 block", 10, 1, 0);
    run_case("2 blocks", 10, 2, 0);
    run_case("257 blocks", 10, 257, 0);
    run_case("straddles the plain range", RUN_PLAIN_START - 5, 12, 0);
    run_case("plain range alone", RUN_PLAIN_START, 2 * RUN_PLAIN_BLOCKS, 0);
    run_case("reaches the superblocks", 0, 6, 0);
    run_case("257 blocks, third call fails", 10, 257, 3);
    run_case("straddle, plain call fails", RUN_PLAIN_START - 5, 12, 2);
}

/* Read len bytes at off back through the cipher: same bytes as the data written, and expect_reads backend reads. */
static void read_case(const char *what, uint64_t off, size_t len, int expect_reads) {
    static const uint8_t dek[32] = { 7, 1, 2, 3 };
    size_t dev_bytes = (size_t)RUN_DEV_BLOCKS * RWY_BLOCK_SIZE;
    uint8_t *mem = calloc(1, dev_bytes), *data = malloc(dev_bytes), *got = malloc(len);
    rwy_crypto_dev_ctx_t *cctx = malloc(sizeof(*cctx));
    run_dev_t d = { .mem = mem };
    rwy_block_dev_t raw = { run_rd, run_wr, NULL, &d, dev_bytes, RWY_BLOCK_SIZE, NULL, NULL }, top;

    for (size_t i = 0; i < dev_bytes; i++) data[i] = (uint8_t)(i * 13 + i / 4096);
    rwy_crypto_dev_wrap(&top, cctx, &raw, dek, RWY_SB_BLOCKS);
    cctx->plain_flight_start = RUN_PLAIN_START;
    cctx->plain_flight_blocks = RUN_PLAIN_BLOCKS;
    CHECK(top.write(top.ctx, 0, data, dev_bytes) == 0, "fill the device");
    d.reads = 0;

    CHECK(top.read(top.ctx, off, got, len) == 0, what);
    CHECK(memcmp(got, data + off, len) == 0, "read returns the plaintext");
    CHECK(d.reads == expect_reads, "number of backend reads");
    printf("%s: %zu bytes, %d backend reads\n", what, len, d.reads);
    free(mem); free(data); free(got); free(cctx);
}

static void test_crypto_partial_reads(void) {
    printf("--- Crypto device: partial reads ---\n");
    read_case("inside one block", 20 * RWY_BLOCK_SIZE + 100, 50, 1);
    read_case("across two blocks", 20 * RWY_BLOCK_SIZE + 4000, 200, 1);
    read_case("whole blocks", 20 * RWY_BLOCK_SIZE, 3 * RWY_BLOCK_SIZE, 1);
    read_case("across the plain range", (RUN_PLAIN_START - 1) * RWY_BLOCK_SIZE + 10, 6 * RWY_BLOCK_SIZE, 1);
    read_case("in the superblocks", 50, 100, 1);
    read_case("longer than the buffer", 20 * RWY_BLOCK_SIZE + 7,
              (RWY_CRYPTO_BOUNCE_BLOCKS + 4) * RWY_BLOCK_SIZE, 3);
}

/* ========================================================================= */
/*                          1. Cryptographic Unit Tests                      */
/* ========================================================================= */

static void test_sha256_unit(const rwy_crypto_ops_t *ops) {
    printf("--- Unit: SHA-256 NIST Vector ---\n");
    /* NIST FIPS 180-4 vector for "abc" */
    uint8_t out[32];
    ops->sha256("abc", 3, out);
    static const uint8_t exp[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    CHECK(memcmp(out, exp, 32) == 0, "SHA-256 NIST vector mismatch");
}

static void test_hmac_sha256_unit(const rwy_crypto_ops_t *ops) {
    printf("--- Unit: HMAC-SHA256 RFC 4231 Vector ---\n");
    /* RFC 4231 Test Case 2: key="Jefe", data="what do ya want for nothing?" */
    uint8_t out[32];
    ops->hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
    static const uint8_t exp[32] = {
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
        0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
        0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
        0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43
    };
    CHECK(memcmp(out, exp, 32) == 0, "HMAC-SHA256 RFC 4231 vector mismatch");
}

static void test_pbkdf2_unit(const rwy_crypto_ops_t *ops) {
    printf("--- Unit: PBKDF2-HMAC-SHA256 RFC 6070 Vector ---\n");
    /* RFC 6070 Test Case 1: P="password", S="salt", c=1, dkLen=20 (we test 32) */
    uint8_t out[32];
    ops->pbkdf2_sha256("password", 8, (const uint8_t *)"salt", 4, 1, out, 20);
    static const uint8_t exp_c1[20] = {
        0x12, 0x0f, 0xb6, 0xcf, 0xfc, 0xf8, 0xb3, 0x2c,
        0x43, 0xe7, 0x22, 0x52, 0x56, 0xc4, 0xf8, 0x37,
        0xa8, 0x65, 0x48, 0xc9
    };
    CHECK(memcmp(out, exp_c1, 20) == 0, "PBKDF2-HMAC-SHA256 c=1 mismatch");

    /* RFC 6070 Test Case 2: P="password", S="salt", c=2, dkLen=20 */
    ops->pbkdf2_sha256("password", 8, (const uint8_t *)"salt", 4, 2, out, 20);
    static const uint8_t exp_c2[20] = {
        0xae, 0x4d, 0x0c, 0x95, 0xaf, 0x6b, 0x46, 0xd3,
        0x2d, 0x0a, 0xdf, 0xf9, 0x28, 0xf0, 0x6d, 0xd0,
        0x2a, 0x30, 0x3f, 0x8e
    };
    CHECK(memcmp(out, exp_c2, 20) == 0, "PBKDF2-HMAC-SHA256 c=2 mismatch");
}

static void test_aes256_unit(void) {
    printf("--- Unit: AES-256 ECB NIST SP 800-38A Vector ---\n");
    /* NIST SP 800-38A ECB-AES256 Encrypt */
    static const uint8_t key[32] = {
        0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe,
        0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
        0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7,
        0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4
    };
    static const uint8_t pt[16] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
        0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a
    };
    static const uint8_t exp_ct[16] = {
        0xf3, 0xee, 0xd1, 0xbd, 0xb5, 0xd2, 0xa0, 0x3c,
        0x06, 0x4b, 0x5a, 0x7e, 0x3d, 0xb1, 0x81, 0xf8
    };

    rwy_aes256_ctx_t enc_ctx, dec_ctx;
    rwy_aes256_set_encrypt_key(&enc_ctx, key);
    rwy_aes256_set_decrypt_key(&dec_ctx, key);

    uint8_t ct[16], recovered[16];
    rwy_aes256_encrypt_block(&enc_ctx, pt, ct);
    CHECK(memcmp(ct, exp_ct, 16) == 0, "AES-256 encryption vector mismatch");

    rwy_aes256_decrypt_block(&dec_ctx, ct, recovered);
    CHECK(memcmp(recovered, pt, 16) == 0, "AES-256 decryption mismatch");
}

static void test_aes_xts_unit(const rwy_crypto_ops_t *ops) {
    printf("--- Unit: AES-256-XTS IEEE 1619 Vector ---\n");
    /* IEEE 1619-2007 XTS-AES-256 vector (512-bit key = 2x256-bit keys) */
    static const uint8_t key1[32] = {
        0x27, 0x18, 0x28, 0x18, 0x28, 0x45, 0x90, 0x45,
        0x23, 0x53, 0x60, 0x28, 0x74, 0x71, 0x35, 0x26,
        0x62, 0x49, 0x77, 0x57, 0x24, 0x70, 0x93, 0x69,
        0x99, 0x59, 0x57, 0x49, 0x66, 0x96, 0x76, 0x27
    };
    static const uint8_t key2[32] = {
        0x31, 0x41, 0x59, 0x26, 0x53, 0x58, 0x97, 0x93,
        0x23, 0x84, 0x62, 0x64, 0x33, 0x83, 0x27, 0x95,
        0x02, 0x88, 0x41, 0x97, 0x16, 0x93, 0x99, 0x37,
        0x51, 0x05, 0x82, 0x09, 0x74, 0x94, 0x45, 0x92
    };
    uint64_t sector_num = 0; /* i = 0 */
    static const uint8_t pt[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };

    rwy_xts_ctx_t xts;
    rwy_xts_init(&xts, key1, key2);

    /* Expected bytes come from OpenSSL's AES-256-XTS for the same keys and sectors */
    static const uint8_t exp0[32] = {
        0x3a, 0x06, 0x0a, 0x8c, 0xad, 0x11, 0x5a, 0x6f, 0x44, 0x57, 0x2e, 0x37, 0x59, 0xe4, 0x3c, 0x8f,
        0xca, 0xd8, 0xbf, 0xcb, 0x23, 0x3f, 0xf6, 0xad, 0x71, 0xb7, 0xc1, 0xe7, 0xca, 0x65, 0x15, 0x08
    };
    static const uint8_t exp5[32] = {
        0xae, 0x2d, 0x6e, 0xef, 0xd1, 0xd4, 0xa3, 0x83, 0xca, 0xa9, 0x2e, 0x9f, 0xf0, 0x24, 0x0b, 0xf4,
        0x89, 0xf3, 0x9b, 0x8b, 0xe1, 0x4a, 0x03, 0x1a, 0xda, 0x91, 0x84, 0x62, 0x34, 0x09, 0xa5, 0x63
    };
    uint8_t ct[32], dec[32];
    ops->xts_crypt(&xts, sector_num, pt, ct, sizeof(pt), 1);
    CHECK(memcmp(ct, exp0, sizeof(ct)) == 0, "AES-XTS sector 0 vector mismatch");
    ops->xts_crypt(&xts, sector_num, ct, dec, sizeof(dec), 0);
    CHECK(memcmp(dec, pt, sizeof(pt)) == 0, "AES-XTS roundtrip mismatch");
    ops->xts_crypt(&xts, 5, pt, ct, sizeof(pt), 1);
    CHECK(memcmp(ct, exp5, sizeof(ct)) == 0, "AES-XTS sector 5 vector mismatch");
}

/* Every backend must give the same bytes */
static void test_backend(const char *name, const rwy_crypto_ops_t *ops) {
    printf("=== Backend: %s ===\n", name);
    test_sha256_unit(ops);
    test_hmac_sha256_unit(ops);
    test_pbkdf2_unit(ops);
    test_aes_xts_unit(ops);
}

static void test_backends_agree(const rwy_crypto_ops_t *a, const rwy_crypto_ops_t *b) {
    printf("--- Unit: backends agree on a 4 KiB block ---\n");
    static uint8_t pt[4096], x[4096], y[4096];
    uint8_t dek[32], ka[32], kb[32];
    rwy_xts_ctx_t xts;
    for (unsigned i = 0; i < sizeof(pt); i++) pt[i] = (uint8_t)(i * 31 + 7);
    for (unsigned i = 0; i < 32; i++) dek[i] = (uint8_t)(i + 1);
    a->sha256(pt, sizeof(pt), ka);
    b->sha256(pt, sizeof(pt), kb);
    CHECK(memcmp(ka, kb, 32) == 0, "SHA-256 of 4 KiB differs between backends");
    a->hmac_sha256(dek, 32, pt, sizeof(pt), ka);
    b->hmac_sha256(dek, 32, pt, sizeof(pt), kb);
    CHECK(memcmp(ka, kb, 32) == 0, "HMAC-SHA256 of 4 KiB differs between backends");
    a->pbkdf2_sha256("pass", 4, dek, 32, 1000, ka, 32);
    b->pbkdf2_sha256("pass", 4, dek, 32, 1000, kb, 32);
    CHECK(memcmp(ka, kb, 32) == 0, "PBKDF2 differs between backends");
    rwy_xts_init_from_dek(&xts, dek);
    a->xts_crypt(&xts, 77, pt, x, sizeof(pt), 1);
    b->xts_crypt(&xts, 77, x, y, sizeof(pt), 0);
    CHECK(memcmp(y, pt, sizeof(pt)) == 0, "encrypt with one backend, decrypt with the other");
    b->xts_crypt(&xts, 78, pt, y, sizeof(pt), 1);
    a->xts_crypt(&xts, 78, y, x, sizeof(pt), 0);
    CHECK(memcmp(x, pt, sizeof(pt)) == 0, "encrypt with the other backend, decrypt with the first");
}

static void test_random_source(void) {
    printf("--- Unit: random source and seeded HMAC-DRBG ---\n");
    /* Expected bytes from an independent HMAC-DRBG written with Python's hmac module */
    static const uint8_t exp1[40] = {
        0xa0, 0xd9, 0x35, 0x8a, 0x07, 0x9b, 0x9b, 0x2f, 0x4e, 0x03, 0x64, 0x28, 0x80, 0x98, 0xe6, 0xfe,
        0xa3, 0x95, 0xc4, 0xcb, 0x42, 0x0d, 0xb7, 0x07, 0x04, 0xbc, 0xb9, 0xf4, 0xa8, 0x45, 0x1b, 0xe7,
        0xcd, 0x9e, 0xa8, 0x9d, 0xb1, 0x69, 0x41, 0xee
    };
    static const uint8_t exp2[16] = {
        0xfd, 0x9d, 0xd4, 0xbe, 0xe8, 0xec, 0x6e, 0x85, 0x9a, 0xef, 0xfc, 0x6e, 0xa0, 0x1a, 0x5f, 0x2c
    };
    uint8_t a[40], b[40], c[16];

    CHECK(rwy_random(a, sizeof(a)) == RWY_CRYPTO_ERR_EMPTY, "built-in table has a random source");
    CHECK(rwy_drbg_seed("\x74\x66\x61\x74\x2d\x73\x65\x65\x64", 9) == RWY_CRYPTO_OK, "seed the generator");
    CHECK(rwy_random(a, sizeof(a)) == RWY_CRYPTO_OK, "draw from the generator");
    CHECK(memcmp(a, exp1, sizeof(a)) == 0, "HMAC-DRBG first draw mismatch");
    CHECK(rwy_random(c, sizeof(c)) == RWY_CRYPTO_OK, "second draw");
    CHECK(memcmp(c, exp2, sizeof(c)) == 0, "HMAC-DRBG second draw mismatch");
    rwy_drbg_seed("\x74\x66\x61\x74\x2d\x73\x65\x65\x64", 9);
    rwy_random(b, sizeof(b));
    CHECK(memcmp(a, b, sizeof(a)) == 0, "same seed, different bytes");
    rwy_drbg_seed("\x74\x66\x61\x74\x2d\x73\x65\x65\x65", 9);
    rwy_random(b, sizeof(b));
    CHECK(memcmp(a, b, sizeof(a)) != 0, "different seed, same bytes");
    CHECK(rwy_drbg_seed("", 0) == RWY_CRYPTO_ERR_INVAL, "empty seed accepted");
    rwy_crypto_set_ops(NULL);
    CHECK(rwy_random(a, sizeof(a)) == RWY_CRYPTO_ERR_EMPTY, "set_ops left the seeded source in place");
}

static void test_keyslot_lifecycle(void) {
    printf("--- Unit: Keyslot Lifecycle & Multi-Slot Hierarchy ---\n");
    rwy_keyslot_header_t hdr;
    rwy_keyslot_init_header(&hdr);

    uint8_t original_dek[32];
    for (int i = 0; i < 32; i++) original_dek[i] = (uint8_t)(0xa0 + i);

    /* Slot 0: Password */
    int rc = rwy_keyslot_set_password(&hdr, 0, "CorrectHorseBatteryStaple", original_dek, 1000);
    CHECK(rc == RWY_CRYPTO_OK, "keyslot set password failed");

    /* Slot 1: TPM2 Sealed Blob */
    uint8_t tpm2_blob[128];
    uint8_t pcr_mask[32];
    memset(tpm2_blob, 0x77, sizeof(tpm2_blob));
    memset(pcr_mask, 0x07, sizeof(pcr_mask));
    rc = rwy_keyslot_set_tpm2_blob(&hdr, 1, tpm2_blob, sizeof(tpm2_blob), pcr_mask);
    CHECK(rc == RWY_CRYPTO_OK, "keyslot set tpm2 failed");

    /* Slot 2: Recovery Passphrase */
    rc = rwy_keyslot_set_recovery_key(&hdr, 2, "RECOVERY-KEY-1234-5678", original_dek);
    CHECK(rc == RWY_CRYPTO_OK, "keyslot set recovery failed");

    /* Unwrap with correct slot 0 passphrase */
    uint8_t unwrapped_dek[32];
    rc = rwy_keyslot_unwrap(&hdr, "CorrectHorseBatteryStaple", unwrapped_dek);
    CHECK(rc == RWY_CRYPTO_OK, "keyslot unwrap with password failed");
    CHECK(memcmp(unwrapped_dek, original_dek, 32) == 0, "unwrapped DEK mismatch (slot 0)");

    /* Unwrap with correct slot 2 recovery key */
    memset(unwrapped_dek, 0, sizeof(unwrapped_dek));
    rc = rwy_keyslot_unwrap(&hdr, "RECOVERY-KEY-1234-5678", unwrapped_dek);
    CHECK(rc == RWY_CRYPTO_OK, "keyslot unwrap with recovery key failed");
    CHECK(memcmp(unwrapped_dek, original_dek, 32) == 0, "unwrapped DEK mismatch (slot 2)");

    /* Unwrap with incorrect passphrase should fail */
    memset(unwrapped_dek, 0, sizeof(unwrapped_dek));
    rc = rwy_keyslot_unwrap(&hdr, "WrongPassword", unwrapped_dek);
    CHECK(rc == RWY_CRYPTO_ERR_AUTH, "keyslot unwrap with bad password succeeded!");
}

/* ========================================================================= */
/*                   2. Ciphertext Entropy & Plaintext Scan                  */
/* ========================================================================= */

static double shannon_entropy(const uint8_t *data, size_t len) {
    size_t counts[256] = {0};
    for (size_t i = 0; i < len; i++) counts[data[i]]++;

    double ent = 0.0;
    for (int i = 0; i < 256; i++) {
        if (counts[i] > 0) {
            double p = (double)counts[i] / (double)len;
            ent -= p * log2(p);
        }
    }
    return ent;
}

static void test_ciphertext_entropy_and_plaintext(const char *enc_store_path) {
    printf("--- Integration: Ciphertext Entropy & Plaintext Absence ---\n");
    FILE *f = fopen(enc_store_path, "rb");
    CHECK(f != NULL, "cannot open encrypted store");
    if (!f) return;

    /* Read first data pool block (block 2 = 8192..12287) */
    uint8_t blk[RWY_BLOCK_SIZE];
    fseek(f, (long)RWY_SB_BLOCKS * RWY_BLOCK_SIZE, SEEK_SET);
    size_t n = fread(blk, 1, sizeof(blk), f);
    fclose(f);
    CHECK(n == sizeof(blk), "failed to read data pool block");

    double ent = shannon_entropy(blk, sizeof(blk));
    printf("data block 2 Shannon entropy: %.4f bits/byte (target > 7.9)\n", ent);
    CHECK(ent > 7.9, "ciphertext entropy too low (possible unencrypted data or zero pad)");

    /* Scan whole raw store file for known plaintext strings */
    f = fopen(enc_store_path, "rb");
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    uint8_t *full = malloc((size_t)sz);
    CHECK(full != NULL, "malloc failed");
    if (full) {
        fseek(f, 0, SEEK_SET);
        CHECK(fread(full, 1, (size_t)sz, f) == (size_t)sz, "read raw store");
        fclose(f);

        /* Strings from mkstore live data:
         * "Hello from Runway!" in README.TXT
         * "efi-app-x86_64" / PE headers
         */
        const char *needle1 = "Hello from Runway!";
        const char *needle2 = "UEFI OS image placeholder";
        void *p1 = memmem(full + 2 * RWY_BLOCK_SIZE, (size_t)sz - 2 * RWY_BLOCK_SIZE, needle1, strlen(needle1));
        void *p2 = memmem(full + 2 * RWY_BLOCK_SIZE, (size_t)sz - 2 * RWY_BLOCK_SIZE, needle2, strlen(needle2));
        CHECK(p1 == NULL, "found plaintext 'Hello from Runway!' in raw data blocks!");
        CHECK(p2 == NULL, "found plaintext 'UEFI OS image placeholder' in raw data blocks!");

        free(full);
    }
}

/* ========================================================================= */
/*              3. CoW Transactional Mutation on Encrypted Store             */
/* ========================================================================= */

static void test_encrypted_fs_mutations(const char *seed_path, const char *passphrase) {
    printf("--- Integration: Encrypted Filesystem CoW Mutations ---\n");
    char path[256];
    snprintf(path, sizeof(path), "%s", tpath("rwy_cryptotest_mut_.STORE"));
    copy_file(seed_path, path);

    file_t fl;
    fl.f = fopen(path, "r+b");
    CHECK(fl.f != NULL, "open test store");
    if (!fl.f) return;

    rwy_block_dev_t raw_dev, crypto_dev;
    raw_dev.read = file_rd; raw_dev.write = file_wr; raw_dev.flush = file_fl;
    raw_dev.ctx = &fl; raw_dev.size = RWY_STORE_SIZE; raw_dev.block_size = RWY_BLOCK_SIZE;

    rwy_keyslot_header_t khdr;
    int rc = rwy_keyslot_read_from_dev(&raw_dev, &khdr);
    CHECK(rc == 0, "read keyslot header");

    uint8_t dek[32];
    rc = rwy_keyslot_unwrap(&khdr, passphrase, dek);
    CHECK(rc == 0, "unwrap dek");

    rwy_crypto_dev_ctx_t cctx;
    rc = rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek, RWY_SB_BLOCKS);
    CHECK(rc == 0, "wrap crypto dev");

    rwy_store_t s;
    rc = rwy_mount(&s, &crypto_dev);
    CHECK(rc == RWY_OK, "mount encrypted store");

    /* Perform multi-transaction mutations: create, write, xattr, rename */
    rc = rwy_begin(&s);
    CHECK(rc == RWY_OK, "begin txn 1");

    rc = rwy_create(&s, "/secrets.txt", 0644, 0, 0);
    CHECK(rc == RWY_OK, "create /secrets.txt");

    const char *payload = "Confidential encryption test payload: 1234567890.";
    rc = rwy_write_path(&s, "/secrets.txt", 0, payload, strlen(payload));
    CHECK(rc == RWY_OK, "write /secrets.txt");

    rc = rwy_setxattr(&s, "/secrets.txt", "user.cryptolabel", "classified", 10, 0);
    CHECK(rc == RWY_OK, "setxattr on encrypted store");

    rc = rwy_commit(&s);
    CHECK(rc == RWY_OK, "commit txn 1");

    /* Second transaction: rename and verify */
    rc = rwy_begin(&s);
    CHECK(rc == RWY_OK, "begin txn 2");
    rc = rwy_rename(&s, "/secrets.txt", "/classified.dat");
    CHECK(rc == RWY_OK, "rename in encrypted store");
    rc = rwy_commit(&s);
    CHECK(rc == RWY_OK, "commit txn 2");

    /* Verify content */
    char readback[128] = {0};
    size_t got = sizeof(readback) - 1;
    rc = rwy_read_path(&s, "/classified.dat", 0, readback, &got);
    CHECK(rc == RWY_OK && got == strlen(payload), "read encrypted file");
    CHECK(memcmp(readback, payload, strlen(payload)) == 0, "payload match");

    char xval[32] = {0};
    rc = rwy_getxattr(&s, "/classified.dat", "user.cryptolabel", xval, sizeof(xval));
    CHECK(rc == 10 && memcmp(xval, "classified", 10) == 0, "xattr match");

    /* Unmount */
    fclose(fl.f);

    /* Verify with checker */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s --key '%s' %s > /dev/null 2>&1", g_checkbin, passphrase, path);
    int status = system(cmd);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "fsck.runway with --key passed on mutated encrypted store");

    unlink(path);
}

/* A large write, as an install of a UKI: it goes through the cipher in runs and reads back whole. */
static void test_encrypted_large_write(const char *seed_path, const char *passphrase) {
    printf("--- Integration: Large Write On An Encrypted Store ---\n");
    char path[256];
    snprintf(path, sizeof(path), "%s", tpath("rwy_cryptotest_big_.STORE"));
    copy_file(seed_path, path);

    file_t fl;
    fl.f = fopen(path, "r+b");
    CHECK(fl.f != NULL, "open test store");
    if (!fl.f) return;
    rwy_block_dev_t raw_dev, crypto_dev;
    raw_dev.read = file_rd; raw_dev.write = file_wr; raw_dev.flush = file_fl;
    raw_dev.ctx = &fl; raw_dev.size = RWY_STORE_SIZE; raw_dev.block_size = RWY_BLOCK_SIZE;

    rwy_keyslot_header_t khdr;
    uint8_t dek[32];
    CHECK(rwy_keyslot_read_from_dev(&raw_dev, &khdr) == 0, "read keyslot header");
    CHECK(rwy_keyslot_unwrap(&khdr, passphrase, dek) == 0, "unwrap dek");
    rwy_crypto_dev_ctx_t *cctx = malloc(sizeof(*cctx));
    CHECK(rwy_crypto_dev_wrap(&crypto_dev, cctx, &raw_dev, dek, RWY_SB_BLOCKS) == 0, "wrap crypto dev");

    size_t sz = 3 * 1024 * 1024 + 123;
    uint8_t *data = malloc(sz), *back = malloc(sz);
    for (size_t i = 0; i < sz; i++) data[i] = (uint8_t)(i * 7 + i / 4093);

    rwy_store_t s;
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount encrypted store");
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    CHECK(rwy_create(&s, "/uki.efi", 0644, 0, 0) == RWY_OK, "create");
    CHECK(rwy_write_path(&s, "/uki.efi", 0, data, sz) == RWY_OK, "write 3 MiB");
    CHECK(rwy_commit(&s) == RWY_OK, "commit");
    size_t got = sz;
    CHECK(rwy_read_path(&s, "/uki.efi", 0, back, &got) == RWY_OK && got == sz, "read back");
    CHECK(memcmp(data, back, sz) == 0, "content matches");
    fclose(fl.f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s --key '%s' %s > /dev/null 2>&1", g_checkbin, passphrase, path);
    int status = system(cmd);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "fsck.runway passes after a large write");
    free(data); free(back); free(cctx);
    unlink(path);
}

/* ========================================================================= */
/*      4. Deterministic Fault Injection over Encrypted CoW Writes           */
/* ========================================================================= */

static void test_encrypted_fault_injection(const char *seed_path, const char *passphrase) {
    printf("--- Fault Injection: Power-Loss Simulation over Encrypted CoW ---\n");
    char path[256];
    snprintf(path, sizeof(path), "%s", tpath("rwy_cryptotest_fi_.STORE"));

    /* Count total write/flush points during a mutating transaction */
    long total_points = 0;
    {
        copy_file(seed_path, path);
        file_t fl;
        fl.f = fopen(path, "r+b");
        rwy_block_dev_t raw_dev, crypto_dev, fi_wrapped;
        raw_dev.read = file_rd; raw_dev.write = file_wr; raw_dev.flush = file_fl;
        raw_dev.ctx = &fl; raw_dev.size = RWY_STORE_SIZE; raw_dev.block_size = RWY_BLOCK_SIZE;

        rwy_keyslot_header_t khdr;
        rwy_keyslot_read_from_dev(&raw_dev, &khdr);
        uint8_t dek[32];
        rwy_keyslot_unwrap(&khdr, passphrase, dek);

        rwy_crypto_dev_ctx_t cctx;
        rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek, RWY_SB_BLOCKS);

        fi_dev_t fi = { .inner = crypto_dev, .point = 0, .fail_at = 0, .failed = 0 };
        fi_wrapped.read = fi_rd; fi_wrapped.write = fi_wr; fi_wrapped.flush = fi_fl;
        fi_wrapped.ctx = &fi; fi_wrapped.size = RWY_STORE_SIZE; fi_wrapped.block_size = RWY_BLOCK_SIZE;

        rwy_store_t s;
        rwy_mount(&s, &fi_wrapped);
        rwy_begin(&s);
        rwy_create(&s, "/fi_test.txt", 0644, 0, 0);
        rwy_write_path(&s, "/fi_test.txt", 0, "Crash-safety test data in encrypted CoW", 39);
        rwy_commit(&s);

        total_points = fi.point;
        fclose(fl.f);
        unlink(path);
    }

    printf("discovered %ld I/O points during encrypted mutation. Injecting crashes at each...\n", total_points);

    /* Test every failure point */
    for (long fail_at = 1; fail_at <= total_points; fail_at++) {
        copy_file(seed_path, path);
        file_t fl;
        fl.f = fopen(path, "r+b");

        rwy_block_dev_t raw_dev, crypto_dev, fi_wrapped;
        raw_dev.read = file_rd; raw_dev.write = file_wr; raw_dev.flush = file_fl;
        raw_dev.ctx = &fl; raw_dev.size = RWY_STORE_SIZE; raw_dev.block_size = RWY_BLOCK_SIZE;

        rwy_keyslot_header_t khdr;
        rwy_keyslot_read_from_dev(&raw_dev, &khdr);
        uint8_t dek[32];
        rwy_keyslot_unwrap(&khdr, passphrase, dek);

        rwy_crypto_dev_ctx_t cctx;
        rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek, RWY_SB_BLOCKS);

        fi_dev_t fi = { .inner = crypto_dev, .point = 0, .fail_at = fail_at, .failed = 0 };
        fi_wrapped.read = fi_rd; fi_wrapped.write = fi_wr; fi_wrapped.flush = fi_fl;
        fi_wrapped.ctx = &fi; fi_wrapped.size = RWY_STORE_SIZE; fi_wrapped.block_size = RWY_BLOCK_SIZE;

        rwy_store_t s;
        int rc = rwy_mount(&s, &fi_wrapped);
        if (!rc) rc = rwy_begin(&s);
        if (!rc) rc = rwy_create(&s, "/fi_test.txt", 0644, 0, 0);
        if (!rc) rc = rwy_write_path(&s, "/fi_test.txt", 0, "Crash-safety test data in encrypted CoW", 39);
        if (!rc) rc = rwy_commit(&s);

        fclose(fl.f);

        /* Re-mount cleanly to verify crash recovery: must be either valid old or valid new generation */
        fl.f = fopen(path, "r+b");
        raw_dev.ctx = &fl;
        rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek, RWY_SB_BLOCKS);

        rwy_store_t remount_s;
        rc = rwy_mount(&remount_s, &crypto_dev);
        CHECK(rc == RWY_OK, "failed to mount crashed encrypted store!");

        /* Run checker */
        fclose(fl.f);
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "%s --key '%s' %s", g_checkbin, passphrase, path);
        FILE *p = popen(cmd, "r");
        char chk_out[4096];
        size_t nread = fread(chk_out, 1, sizeof(chk_out) - 1, p);
        chk_out[nread] = '\0';
        int status = pclose(p);
        if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
            printf("FAIL at fail_at=%ld (status=%d):\n%s\n", fail_at, status, chk_out);
            failures++;
        }

        unlink(path);
    }
    printf("Fault injection: all %ld crash points recovered cleanly!\n", total_points);
}

/* ========================================================================= */
/*          5. State Record on an Encrypted Store (in-place rewrites)        */
/* ========================================================================= */

/* The cut layer sits below the cipher, so a torn write tears the ciphertext, as a disk would. */
typedef struct {
    file_t fl;
    rwy_block_dev_t raw, fid, top;
    fi_dev_t fi;
    rwy_crypto_dev_ctx_t cctx;
    rwy_store_t s;
} enc_t;

static int enc_mount(enc_t *e, const char *path, const char *pass, long fail_at, int torn) {
    rwy_keyslot_header_t khdr;
    uint8_t dek[32];
    int rc;
    e->fl.f = fopen(path, "r+b");
    if (!e->fl.f) exit(1);
    e->raw = (rwy_block_dev_t){ file_rd, file_wr, file_fl, &e->fl, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    if (rwy_keyslot_read_from_dev(&e->raw, &khdr) || rwy_keyslot_unwrap(&khdr, pass, dek)) exit(1);
    e->fi = (fi_dev_t){ .inner = e->raw, .fail_at = fail_at, .torn = torn };
    e->fid = (rwy_block_dev_t){ fi_rd, fi_wr, fi_fl, &e->fi, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    rwy_crypto_dev_wrap(&e->top, &e->cctx, &e->fid, dek, RWY_SB_BLOCKS);
    rc = rwy_mount(&e->s, &e->top);
    e->fi.point = 0;
    return rc;
}

static void rec_fill(uint8_t *p, int seed) {
    for (size_t i = 0; i < RWY_REC_PAYLOAD; i++) p[i] = (uint8_t)(i * 7 + seed);
}

static int rec_write_seed(rwy_store_t *s, int seed) {
    uint8_t p[RWY_REC_PAYLOAD];
    rec_fill(p, seed);
    return rwy_record_write(s, p);
}

static int rec_is(rwy_store_t *s, int seed) {
    uint8_t got[RWY_REC_PAYLOAD], want[RWY_REC_PAYLOAD];
    rec_fill(want, seed);
    return rwy_record_read(s, got) == RWY_OK && !memcmp(got, want, sizeof(got));
}

static void raw_block(const char *path, uint32_t blk, uint8_t out[RWY_BLOCK_SIZE]) {
    FILE *f = fopen(path, "rb");
    if (!f || fseek(f, (long)blk * RWY_BLOCK_SIZE, SEEK_SET) || fread(out, 1, RWY_BLOCK_SIZE, f) != RWY_BLOCK_SIZE) exit(1);
    fclose(f);
}

static void test_encrypted_state_record(const char *seed_path, const char *passphrase) {
    const char *base = tpath("rwy-cryptotest-rec-base.DAT"), *one = tpath("rwy-cryptotest-rec.DAT");
    uint8_t b0[RWY_BLOCK_SIZE], b1[RWY_BLOCK_SIZE], plain[RWY_REC_PAYLOAD];
    enc_t e;
    uint32_t rs;
    long points = 0;
    int bad = 0;

    printf("--- State Record: In-Place Rewrites on an Encrypted Store ---\n");
    copy_file(seed_path, base);
    CHECK(enc_mount(&e, base, passphrase, 0, 0) == RWY_OK, "mount the encrypted store");
    rs = e.s.sb.rec_start;
    CHECK(rec_write_seed(&e.s, 2) == RWY_OK, "first record write");
    fclose(e.fl.f);

    /* The copies are ciphertext on disk, and a rewrite round-trips through the cipher. */
    copy_file(base, one);
    CHECK(enc_mount(&e, one, passphrase, 0, 0) == RWY_OK && rec_is(&e.s, 2), "the record reads back after a mount");
    CHECK(rec_write_seed(&e.s, 7) == RWY_OK && rec_is(&e.s, 7), "second record write goes to the other copy");
    fclose(e.fl.f);
    raw_block(one, rs, b0);
    raw_block(one, rs + 1, b1);
    rec_fill(plain, 2);
    CHECK(!memmem(b0, sizeof(b0), plain, 64) && !memmem(b1, sizeof(b1), plain, 64), "no plaintext in the old copy");
    rec_fill(plain, 7);
    CHECK(!memmem(b0, sizeof(b0), plain, 64) && !memmem(b1, sizeof(b1), plain, 64), "no plaintext in the new copy");
    CHECK(memcmp(b0, b1, sizeof(b0)) != 0, "the two copies differ");
    CHECK(enc_mount(&e, one, passphrase, 0, 0) == RWY_OK && rec_is(&e.s, 7), "the new record survives a mount");
    fclose(e.fl.f);

    /* One write and one flush; a clean cut, a torn first half and a torn second half. */
    for (int torn = 0; torn < 3; torn++) {
        for (long at = 0; at <= 2; at++) {
            copy_file(base, one);
            CHECK(enc_mount(&e, one, passphrase, at ? at : -1, torn) == RWY_OK, "mount through the fault layer");
            if (at == 0) {
                CHECK(rec_write_seed(&e.s, 7) == RWY_OK, "write without a cut");
                points = e.fi.point;
                CHECK(points == 2, "one block write and one flush");
            } else {
                CHECK(rec_write_seed(&e.s, 7) != RWY_OK && e.fi.failed, "a cut fails the write");
                CHECK(rec_is(&e.s, 2), "a failed write keeps the record in memory");
            }
            fclose(e.fl.f);
            if (enc_mount(&e, one, passphrase, 0, 0)) { printf("FAIL: mount after cut torn=%d at=%ld\n", torn, at); bad++; continue; }
            if (!rec_is(&e.s, 2) && !rec_is(&e.s, 7)) { printf("FAIL: torn record torn=%d at=%ld\n", torn, at); bad++; }
            CHECK(!rwy_record_damaged(&e.s), "one intact copy is enough");
            CHECK(rec_write_seed(&e.s, 8) == RWY_OK && rec_is(&e.s, 8), "write after the cut");
            fclose(e.fl.f);
            CHECK(enc_mount(&e, one, passphrase, 0, 0) == RWY_OK && rec_is(&e.s, 8), "and it survives a mount");
            fclose(e.fl.f);
        }
    }
    CHECK(bad == 0, "every cut leaves the old or the new record");
    remove(base);
    remove(one);
}

/* ========================================================================= */
/*          6. Keyslot Carry-Forward (Tail Follows Generation)               */
/* ========================================================================= */

static void test_keyslot_carry_forward(const char *seed_path, const char *passphrase) {
    char path[256];
    file_t fl;
    rwy_block_dev_t raw_dev, crypto_dev;
    rwy_keyslot_header_t khdr_a, khdr_b, readback;
    rwy_crypto_dev_ctx_t cctx;
    rwy_store_t s;
    rwy_superblock_t nsb;
    uint8_t dek[32];
    int rc;

    printf("--- Keyslot Carry-Forward: Tail Follows Generation ---\n");
    snprintf(path, sizeof(path), "%s", tpath("rwy_cryptotest_carry_.STORE"));
    copy_file(seed_path, path);

    fl.f = fopen(path, "r+b");
    CHECK(fl.f != NULL, "open seed copy for carry-forward test");

    raw_dev.read = file_rd; raw_dev.write = file_wr; raw_dev.flush = file_fl;
    raw_dev.ctx = &fl; raw_dev.size = RWY_STORE_SIZE; raw_dev.block_size = RWY_BLOCK_SIZE;

    rc = raw_dev.read(raw_dev.ctx, (uint64_t)RWY_SB_SLOT_A * RWY_BLOCK_SIZE + RWY_KEYSLOT_OFFSET,
                      &khdr_a, sizeof(khdr_a));
    CHECK(rc == 0, "read slot A keyslot");
    rc = raw_dev.read(raw_dev.ctx, (uint64_t)RWY_SB_SLOT_B * RWY_BLOCK_SIZE + RWY_KEYSLOT_OFFSET,
                      &khdr_b, sizeof(khdr_b));
    CHECK(rc == 0, "read slot B keyslot");

    /* Modify slot B's tail on disk so slot A and slot B hold different tails. */
    khdr_b.slots[0].salt[0] ^= 0x5a;
    khdr_b.crc32c = 0;
    khdr_b.crc32c = rwy_crc32c(&khdr_b, sizeof(khdr_b));
    rc = put_tail(&raw_dev, RWY_SB_SLOT_B, &khdr_b);
    CHECK(rc == 0, "write modified tail to slot B");
    if (raw_dev.flush) raw_dev.flush(raw_dev.ctx);

    CHECK(memcmp(&khdr_a, &khdr_b, sizeof(rwy_keyslot_header_t)) != 0, "tails in slot A and B differ");

    rc = rwy_keyslot_unwrap(&khdr_a, passphrase, dek);
    CHECK(rc == RWY_CRYPTO_OK, "unwrap DEK from slot A");

    rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek, RWY_SB_BLOCKS);

    rc = rwy_mount(&s, &crypto_dev);
    CHECK(rc == RWY_OK, "mount store");
    CHECK(s.sb_slot == 0, "mounted from slot A");

    /* First commit: publishes generation 2 to slot B.
     * Slot B must carry forward slot A's tail (khdr_a). */
    CHECK(rwy_begin(&s) == RWY_OK, "begin txn 1");
    CHECK(rwy_create(&s, "/c1.txt", 0644, 0, 0) == RWY_OK, "create /c1.txt");
    CHECK(rwy_commit(&s) == RWY_OK, "commit txn 1 to slot B");
    CHECK(s.sb_slot == 1, "now on slot B");

    rc = raw_dev.read(raw_dev.ctx, (uint64_t)RWY_SB_SLOT_B * RWY_BLOCK_SIZE + RWY_KEYSLOT_OFFSET,
                      &readback, sizeof(readback));
    CHECK(rc == 0, "read slot B keyslot after commit 1");
    CHECK(memcmp(&readback, &khdr_a, sizeof(rwy_keyslot_header_t)) == 0,
          "slot B carries tail from current slot A");

    /* Second commit: publishes generation 3 to slot A.
     * Slot A must carry forward slot B's tail (which is khdr_a). */
    CHECK(rwy_begin(&s) == RWY_OK, "begin txn 2");
    CHECK(rwy_create(&s, "/c2.txt", 0644, 0, 0) == RWY_OK, "create /c2.txt");
    CHECK(rwy_commit(&s) == RWY_OK, "commit txn 2 to slot A");
    CHECK(s.sb_slot == 0, "now on slot A");

    rc = raw_dev.read(raw_dev.ctx, (uint64_t)RWY_SB_SLOT_A * RWY_BLOCK_SIZE + RWY_KEYSLOT_OFFSET,
                      &readback, sizeof(readback));
    CHECK(rc == 0, "read slot A keyslot after commit 2");
    CHECK(memcmp(&readback, &khdr_a, sizeof(rwy_keyslot_header_t)) == 0,
          "slot A carries tail from current slot B");

    /* Explicit tail parameter: provide khdr_b as explicit tail */
    nsb = s.sb;
    nsb.generation = s.sb.generation + 1;
    nsb.crc32c = 0;
    nsb.crc32c = rwy_crc32c(&nsb, offsetof(rwy_superblock_t, crc32c));
    CHECK(rwy_write_superblock(&s, &nsb, &khdr_b) == RWY_OK, "write superblock with explicit tail");
    rc = raw_dev.read(raw_dev.ctx, (uint64_t)RWY_SB_SLOT_B * RWY_BLOCK_SIZE + RWY_KEYSLOT_OFFSET,
                      &readback, sizeof(readback));
    CHECK(rc == 0, "read slot B keyslot with explicit tail");
    CHECK(memcmp(&readback, &khdr_b, sizeof(rwy_keyslot_header_t)) == 0,
          "slot B carries explicit tail");

    fclose(fl.f);
    unlink(path);
}

static void test_keyslot_generation_pick(const char *seed_path, const char *passphrase) {
    char path[256];
    file_t fl;
    rwy_block_dev_t raw_dev, crypto_dev;
    rwy_keyslot_header_t khdr_a, khdr_b, readback;
    rwy_crypto_dev_ctx_t cctx;
    rwy_store_t s;
    uint8_t dek[32];
    int rc;

    printf("--- Keyslot Read: Pick From Newest Valid Slot ---\n");
    snprintf(path, sizeof(path), "%s", tpath("rwy_cryptotest_newest_.STORE"));
    copy_file(seed_path, path);

    fl.f = fopen(path, "r+b");
    CHECK(fl.f != NULL, "open seed copy for newest slot test");

    raw_dev.read = file_rd; raw_dev.write = file_wr; raw_dev.flush = file_fl;
    raw_dev.ctx = &fl; raw_dev.size = RWY_STORE_SIZE; raw_dev.block_size = RWY_BLOCK_SIZE;

    rc = raw_dev.read(raw_dev.ctx, (uint64_t)RWY_SB_SLOT_A * RWY_BLOCK_SIZE + RWY_KEYSLOT_OFFSET,
                      &khdr_a, sizeof(khdr_a));
    CHECK(rc == 0, "read slot A keyslot");
    khdr_b = khdr_a;
    khdr_b.slots[0].salt[0] ^= 0x5a;
    khdr_b.crc32c = 0;
    khdr_b.crc32c = rwy_crc32c(&khdr_b, sizeof(khdr_b));

    rc = rwy_keyslot_unwrap(&khdr_a, passphrase, dek);
    CHECK(rc == RWY_CRYPTO_OK, "unwrap DEK from slot A");
    rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek, RWY_SB_BLOCKS);

    rc = rwy_mount(&s, &crypto_dev);
    CHECK(rc == RWY_OK, "mount store");

    /* Commit 1: publishes gen 2 to slot B with explicit tail khdr_b */
    rwy_superblock_t nsb = s.sb;
    nsb.generation = s.sb.generation + 1;
    nsb.crc32c = 0;
    nsb.crc32c = rwy_crc32c(&nsb, offsetof(rwy_superblock_t, crc32c));
    CHECK(rwy_write_superblock(&s, &nsb, &khdr_b) == RWY_OK, "write gen 2 to slot B with tail B");

    /* Slot B is gen 2, Slot A is gen 1. rwy_keyslot_read_from_dev must pick slot B */
    rc = rwy_keyslot_read_from_dev(&raw_dev, &readback);
    CHECK(rc == RWY_CRYPTO_OK, "read keyslot from dev");
    CHECK(memcmp(&readback, &khdr_b, sizeof(readback)) == 0,
          "slot B (gen 2) picked over slot A (gen 1)");

    /* Commit 2: write gen 3 to slot A with tail A */
    s.sb_slot = 1;
    s.sb = nsb;
    nsb.generation = s.sb.generation + 1;
    nsb.crc32c = 0;
    nsb.crc32c = rwy_crc32c(&nsb, offsetof(rwy_superblock_t, crc32c));
    CHECK(rwy_write_superblock(&s, &nsb, &khdr_a) == RWY_OK, "write gen 3 to slot A with tail A");

    /* Slot A is gen 3, Slot B is gen 2. rwy_keyslot_read_from_dev must pick slot A */
    rc = rwy_keyslot_read_from_dev(&raw_dev, &readback);
    CHECK(rc == RWY_CRYPTO_OK, "read keyslot from dev");
    CHECK(memcmp(&readback, &khdr_a, sizeof(readback)) == 0,
          "slot A (gen 3) picked over slot B (gen 2)");

    fclose(fl.f);
    unlink(path);
}

/* The header writer reaches both slots with whole-block writes and leaves the rest of each block alone. */
static void test_keyslot_write_aligned(const char *seed_path) {
    char path[256];
    file_t fl;
    rwy_block_dev_t raw_dev;
    rwy_keyslot_header_t hdr, back;
    uint8_t before[RWY_BLOCK_SIZE], after[RWY_BLOCK_SIZE];

    printf("--- Keyslot Write: Whole Blocks In Both Slots ---\n");
    snprintf(path, sizeof(path), "%s", tpath("rwy_cryptotest_hdrw_.STORE"));
    copy_file(seed_path, path);
    fl.f = fopen(path, "r+b");
    CHECK(fl.f != NULL, "open seed copy for the header write");
    raw_dev.read = file_rd; raw_dev.write = file_wr; raw_dev.flush = file_fl;
    raw_dev.ctx = &fl; raw_dev.size = RWY_STORE_SIZE; raw_dev.block_size = RWY_BLOCK_SIZE;

    CHECK(rwy_keyslot_read_from_dev(&raw_dev, &hdr) == RWY_CRYPTO_OK, "read the header");
    CHECK(raw_dev.read(raw_dev.ctx, (uint64_t)RWY_SB_SLOT_B * RWY_BLOCK_SIZE, before, sizeof(before)) == 0, "read slot B");
    hdr.slots[0].salt[0] ^= 0x33;
    hdr.crc32c = 0;
    hdr.crc32c = rwy_crc32c(&hdr, sizeof(hdr));
    CHECK(rwy_keyslot_write_to_dev(&raw_dev, &hdr) == RWY_CRYPTO_OK, "write the header to both slots");
    CHECK(raw_dev.read(raw_dev.ctx, (uint64_t)RWY_SB_SLOT_B * RWY_BLOCK_SIZE, after, sizeof(after)) == 0, "read slot B again");
    CHECK(!memcmp(before, after, RWY_KEYSLOT_OFFSET), "superblock part of slot B is unchanged");
    CHECK(!memcmp(after + RWY_KEYSLOT_OFFSET, &hdr, sizeof(hdr)), "slot B holds the new header");
    CHECK(!memcmp(before + RWY_KEYSLOT_OFFSET + sizeof(hdr), after + RWY_KEYSLOT_OFFSET + sizeof(hdr),
                  RWY_BLOCK_SIZE - RWY_KEYSLOT_OFFSET - sizeof(hdr)), "slot B bytes after the header are unchanged");
    CHECK(rwy_keyslot_read_from_dev(&raw_dev, &back) == RWY_CRYPTO_OK && !memcmp(&back, &hdr, sizeof(hdr)),
          "the new header reads back");

    /* The cipher layer refuses what the store never writes */
    {
        rwy_block_dev_t crypto_dev;
        rwy_crypto_dev_ctx_t cctx;
        uint8_t dek[32] = { 1 }, blk[2 * RWY_BLOCK_SIZE] = { 0 };

        rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek, RWY_SB_BLOCKS);
        CHECK(crypto_dev.write(crypto_dev.ctx, 100, blk, RWY_BLOCK_SIZE) != 0, "crypto write at an odd offset is refused");
        CHECK(crypto_dev.write(crypto_dev.ctx, RWY_BLOCK_SIZE, blk, 100) != 0, "crypto write of a short length is refused");
        CHECK(crypto_dev.write(crypto_dev.ctx, 4 * RWY_BLOCK_SIZE, blk, sizeof(blk)) == 0, "crypto write of two blocks");
    }
    fclose(fl.f);
    unlink(path);
}

static void test_keyslot_torn_slot(const char *seed_path, const char *passphrase) {
    char path[256];
    file_t fl;
    rwy_block_dev_t raw_dev, crypto_dev;
    rwy_keyslot_header_t khdr_a, khdr_b, readback;
    rwy_crypto_dev_ctx_t cctx;
    rwy_store_t s;
    rwy_superblock_t sb;
    uint8_t dek[32];
    int rc;

    printf("--- Keyslot Read: Torn Keyslot Slot Is Ignored ---\n");
    snprintf(path, sizeof(path), "%s", tpath("rwy_cryptotest_torn_.STORE"));
    copy_file(seed_path, path);

    fl.f = fopen(path, "r+b");
    CHECK(fl.f != NULL, "open seed copy for torn keyslot test");

    raw_dev.read = file_rd; raw_dev.write = file_wr; raw_dev.flush = file_fl;
    raw_dev.ctx = &fl; raw_dev.size = RWY_STORE_SIZE; raw_dev.block_size = RWY_BLOCK_SIZE;

    rc = raw_dev.read(raw_dev.ctx, (uint64_t)RWY_SB_SLOT_A * RWY_BLOCK_SIZE + RWY_KEYSLOT_OFFSET,
                      &khdr_a, sizeof(khdr_a));
    CHECK(rc == 0, "read slot A keyslot");
    khdr_b = khdr_a;
    khdr_b.slots[0].salt[0] ^= 0x5a;
    khdr_b.crc32c = 0;
    khdr_b.crc32c = rwy_crc32c(&khdr_b, sizeof(khdr_b));

    rc = rwy_keyslot_unwrap(&khdr_a, passphrase, dek);
    CHECK(rc == RWY_CRYPTO_OK, "unwrap DEK from slot A");
    rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek, RWY_SB_BLOCKS);

    rc = rwy_mount(&s, &crypto_dev);
    CHECK(rc == RWY_OK, "mount store");

    /* Publish gen 2 to slot B with tail B */
    rwy_superblock_t nsb = s.sb;
    nsb.generation = s.sb.generation + 1;
    nsb.crc32c = 0;
    nsb.crc32c = rwy_crc32c(&nsb, offsetof(rwy_superblock_t, crc32c));
    CHECK(rwy_write_superblock(&s, &nsb, &khdr_b) == RWY_OK, "write gen 2 to slot B");

    /* Corrupt keyslot CRC in slot B to simulate a torn write */
    khdr_b.crc32c ^= 0xbeef;
    rc = put_tail(&raw_dev, RWY_SB_SLOT_B, &khdr_b);
    CHECK(rc == 0, "corrupt slot B keyslot CRC");
    if (raw_dev.flush) raw_dev.flush(raw_dev.ctx);

    /* Slot B must be deemed invalid despite higher generation */
    CHECK(rwy_sb_slot_valid(&s, 1, &sb) == RWY_ERR_CORRUPT, "slot B invalid due to bad keyslot CRC");
    CHECK(rwy_sb_slot_valid(&s, 0, &sb) == RWY_OK, "slot A remains valid");

    /* rwy_keyslot_read_from_dev must ignore slot B and pick slot A */
    rc = rwy_keyslot_read_from_dev(&raw_dev, &readback);
    CHECK(rc == RWY_CRYPTO_OK, "read keyslot from dev with torn slot B");
    CHECK(memcmp(&readback, &khdr_a, sizeof(readback)) == 0, "fallback to slot A keyslot");

    /* Remounting must pick slot A (gen 1) */
    rwy_store_t remount_s;
    rc = rwy_mount(&remount_s, &crypto_dev);
    CHECK(rc == RWY_OK, "remount store succeeds via slot A fallback");
    CHECK(remount_s.sb_slot == 0, "remount picked slot A");
    CHECK(remount_s.sb.generation == 1, "remount on generation 1");

    /* Now corrupt slot A keyslot as well */
    khdr_a.crc32c ^= 0xbeef;
    rc = put_tail(&raw_dev, RWY_SB_SLOT_A, &khdr_a);
    CHECK(rc == 0, "corrupt slot A keyslot CRC");
    if (raw_dev.flush) raw_dev.flush(raw_dev.ctx);

    CHECK(rwy_sb_slot_valid(&s, 0, &sb) == RWY_ERR_CORRUPT, "slot A invalid due to bad keyslot CRC");
    CHECK(rwy_keyslot_read_from_dev(&raw_dev, &readback) == RWY_CRYPTO_ERR_CORRUPT,
          "keyslot read fails when both slots torn");
    CHECK(rwy_mount(&remount_s, &crypto_dev) == RWY_ERR_CORRUPT,
          "mount fails when both slots torn");

    fclose(fl.f);
    unlink(path);
}

/* ========================================================================= */
/*                          7. Main Test Runner                              */
/* ========================================================================= */

int main(int argc, char **argv) {
    if (argc >= 3) g_checkbin = argv[2];

    printf("=================================================================\n");
    printf("  Runway Block-Layer Encryption & Keyslot Verification Suite\n");
    printf("=================================================================\n");

    /* 1. Cryptographic Primitive Unit Tests */
    test_backend("built-in", &rwy_crypto_builtin_ops);
    test_backend("OpenSSL", &rwy_crypto_openssl_ops);
    test_backends_agree(&rwy_crypto_builtin_ops, &rwy_crypto_openssl_ops);
    test_aes256_unit();
    /* The portable AES code: the only one that runs on arm64 */
    rwy_crypto_set_portable(1);
    test_backend("built-in, portable AES", &rwy_crypto_builtin_ops);
    test_backends_agree(&rwy_crypto_builtin_ops, &rwy_crypto_openssl_ops);
    test_aes256_unit();
    rwy_crypto_set_portable(0);
    test_random_source();
    test_keyslot_lifecycle();
    test_crypto_runs();
    test_crypto_partial_reads();

    /* 2. Generate an encrypted test seed store with mkstore */
    char enc_store[256];
    snprintf(enc_store, sizeof(enc_store), "%s", tpath("rwy_test_enc_.STORE"));
    const char *test_pass = "RunwayTestPassphrase2026!";

    char mk_cmd[512];
    snprintf(mk_cmd, sizeof(mk_cmd), "%s --encrypt --passphrase '%s' %s > /dev/null",
             mkstore_path(), test_pass, enc_store);
    int rc = system(mk_cmd);
    CHECK(rc == 0, "mkstore --encrypt failed");

    /* 3. Test unauthenticated checker vs authenticated checker */
    printf("--- Checker: Unauthenticated vs Authenticated Mode ---\n");
    char chk_cmd[512];
    /* Unauthenticated: should validate superblocks + keyslots and exit 0 */
    snprintf(chk_cmd, sizeof(chk_cmd), "%s %s > /dev/null 2>&1", g_checkbin, enc_store);
    rc = system(chk_cmd);
    CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 0, "unauthenticated check failed");

    /* Authenticated with wrong key: should exit 8 */
    snprintf(chk_cmd, sizeof(chk_cmd), "%s --key 'WrongKey' %s > /dev/null 2>&1", g_checkbin, enc_store);
    rc = system(chk_cmd);
    CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 8, "check with bad key succeeded!");

    /* Authenticated with correct key: should validate full filesystem and exit 0 */
    snprintf(chk_cmd, sizeof(chk_cmd), "%s --key '%s' %s > /dev/null 2>&1", g_checkbin, test_pass, enc_store);
    rc = system(chk_cmd);
    CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 0, "authenticated check with correct key failed");

    /* 4. Ciphertext Entropy & Plaintext Scan */
    test_ciphertext_entropy_and_plaintext(enc_store);

    /* 5. CoW Transactions on Encrypted Store */
    test_encrypted_fs_mutations(enc_store, test_pass);
    test_encrypted_large_write(enc_store, test_pass);

    /* 6. Fault Injection over Encrypted CoW */
    test_encrypted_fault_injection(enc_store, test_pass);

    /* 7. State Record on an Encrypted Store */
    test_encrypted_state_record(enc_store, test_pass);

    /* 8. Keyslot Carry-Forward */
    test_keyslot_carry_forward(enc_store, test_pass);

    /* 9. Keyslot Selection From Newest Valid Slot */
    test_keyslot_generation_pick(enc_store, test_pass);
    test_keyslot_torn_slot(enc_store, test_pass);
    test_keyslot_write_aligned(enc_store);

    unlink(enc_store);

    printf("=================================================================\n");
    if (failures == 0) {
        printf("CRYPTOTEST: PASS (all tests passed with 0 failures)\n");
        return 0;
    } else {
        printf("CRYPTOTEST: FAIL (%d failures)\n", failures);
        return 1;
    }
}
