/*
 * tests/host/reencrypttest.c — Tests for rwy_reencrypt() and finalize.
 * SPDX-License-Identifier: Apache-2.0
 */
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
#include "rwy_format.h"
#include "rwy_internal.h"

static int failures;
static const char *g_checkbin = "build/tools/fsck.runway";

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

typedef struct { FILE *f; } file_t;

static int file_rd(void *ctx, uint64_t off, void *buf, size_t len) {
    file_t *fl = ctx;
    if (fseek(fl->f, (long)off, SEEK_SET)) return -1;
    return fread(buf, 1, len, fl->f) == len ? 0 : -1;
}

static int file_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    file_t *fl = ctx;
    if (fseek(fl->f, (long)off, SEEK_SET)) return -1;
    return fwrite(buf, 1, len, fl->f) == len ? 0 : -1;
}

static int file_fl(void *ctx) {
    file_t *fl = ctx;
    return fflush(fl->f) ? -1 : 0;
}

static void copy_file(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "wb");
    char buf[1 << 20];
    size_t n;
    if (!fa || !fb) { fprintf(stderr, "copy failed %s -> %s\n", a, b); exit(1); }
    while ((n = fread(buf, 1, sizeof(buf), fa)) > 0)
        if (fwrite(buf, 1, n, fb) != n) { fprintf(stderr, "copy failed\n"); exit(1); }
    fclose(fa);
    fclose(fb);
}

static int files_identical(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    char ba[1 << 20], bb[1 << 20];
    size_t na, nb;
    int eq = 1;

    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
    for (;;) {
        na = fread(ba, 1, sizeof(ba), fa);
        nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || memcmp(ba, bb, na)) { eq = 0; break; }
        if (na == 0) break;
    }
    fclose(fa);
    fclose(fb);
    return eq;
}

static int run_checker(const char *img, const char *key) {
    char cmd[512];
    if (key)
        snprintf(cmd, sizeof(cmd), "%s --key '%s' %s > /dev/null 2>&1", g_checkbin, key, img);
    else
        snprintf(cmd, sizeof(cmd), "%s %s > /dev/null 2>&1", g_checkbin, img);
    int rc = system(cmd);
    return (WIFEXITED(rc) && WEXITSTATUS(rc) == 0) ? 0 : -1;
}

static void open_raw_dev(rwy_block_dev_t *dev, file_t *fl, const char *path, uint64_t sz) {
    fl->f = fopen(path, "r+b");
    if (!fl->f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    dev->read = file_rd;
    dev->write = file_wr;
    dev->flush = file_fl;
    dev->ctx = fl;
    dev->size = sz;
    dev->block_size = RWY_BLOCK_SIZE;
}

static int read_keyslot_at_slot(const rwy_block_dev_t *dev, int slot, rwy_keyslot_header_t *out) {
    uint8_t blk[RWY_BLOCK_SIZE];
    uint64_t off = (uint64_t)(slot ? RWY_SB_SLOT_B : RWY_SB_SLOT_A) * RWY_BLOCK_SIZE;
    if (dev->read(dev->ctx, off, blk, sizeof(blk))) return -1;
    memcpy(out, blk + RWY_KEYSLOT_OFFSET, sizeof(*out));
    return 0;
}

static int find_mem(const uint8_t *hay, size_t hlen, const char *str) {
    size_t slen = strlen(str);
    if (slen > hlen) return 0;
    for (size_t i = 0; i <= hlen - slen; i++) {
        if (memcmp(hay + i, str, slen) == 0) return 1;
    }
    return 0;
}

typedef struct {
    rwy_block_dev_t inner;
    long point;
    long fail_at;
    int failed;
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
        return -1;
    }
    return fi->inner.write(fi->inner.ctx, off, buf, len);
}

static int fi_fl(void *ctx) {
    fi_dev_t *fi = ctx;
    fi->point++;
    if (fi->fail_at == fi->point) {
        fi->failed = 1;
        return -1;
    }
    if (fi->inner.flush) return fi->inner.flush(fi->inner.ctx);
    return 0;
}

static void fi_wrap(rwy_block_dev_t *out, fi_dev_t *fi, const rwy_block_dev_t *inner, long fail_at) {
    fi->inner = *inner;
    fi->point = 0;
    fi->fail_at = fail_at;
    fi->failed = 0;
    out->read = fi_rd;
    out->write = fi_wr;
    out->flush = fi_fl;
    out->ctx = fi;
    out->size = inner->size;
    out->block_size = inner->block_size;
}

static void populate_test_tree(rwy_store_t *s) {
    uint8_t bigxattr[5000];
    uint8_t pattern[16384];
    size_t k;

    CHECK(rwy_begin(s) == RWY_OK, "begin populate");
    CHECK(rwy_mkdir(s, "/tree", 0755, 0, 0) == RWY_OK, "mkdir /tree");
    CHECK(rwy_create(s, "/tree/hello.txt", 0644, 0, 0) == RWY_OK, "create hello");
    CHECK(rwy_write_path(s, "/tree/hello.txt", 0, "Hello, Reencryption!", 20) == RWY_OK, "write hello");

    for (k = 0; k < sizeof(pattern); k++) pattern[k] = (uint8_t)(k & 0xff);
    CHECK(rwy_create(s, "/tree/pattern.bin", 0644, 0, 0) == RWY_OK, "create pattern");
    CHECK(rwy_write_path(s, "/tree/pattern.bin", 0, pattern, sizeof(pattern)) == RWY_OK, "write pattern");

    CHECK(rwy_link(s, "/tree/hello.txt", "/tree/hardlink") == RWY_OK, "link hello");
    CHECK(rwy_symlink(s, "/tree/symlink", "/tree/hello.txt", 0, 0) == RWY_OK, "symlink hello");

    CHECK(rwy_setxattr(s, "/tree/hello.txt", "user.tag", "reencrypt-v1", 12, 0) == RWY_OK, "set small xattr");

    for (k = 0; k < sizeof(bigxattr); k++) bigxattr[k] = (uint8_t)((k * 37) & 0xff);
    CHECK(rwy_setxattr(s, "/tree/pattern.bin", "user.large", bigxattr, sizeof(bigxattr), 0) == RWY_OK,
          "set large xattr");

    CHECK(rwy_commit(s) == RWY_OK, "commit populate");
}

static void verify_test_tree(rwy_store_t *s) {
    char hbuf[32], sbuf[64], tagbuf[32];
    uint8_t patbuf[16384], bigxbuf[5000];
    rwy_attr_t a_orig, a_link;
    size_t len, k;

    len = sizeof(hbuf) - 1;
    CHECK(rwy_read_path(s, "/tree/hello.txt", 0, hbuf, &len) == RWY_OK, "read hello");
    hbuf[len] = 0;
    CHECK(strcmp(hbuf, "Hello, Reencryption!") == 0, "hello content matches");

    len = sizeof(patbuf);
    CHECK(rwy_read_path(s, "/tree/pattern.bin", 0, patbuf, &len) == RWY_OK, "read pattern");
    CHECK(len == sizeof(patbuf), "pattern length matches");
    for (k = 0; k < sizeof(patbuf); k++) {
        if (patbuf[k] != (uint8_t)(k & 0xff)) {
            CHECK(0, "pattern byte mismatch");
            break;
        }
    }

    CHECK(rwy_stat(s, "/tree/hello.txt", &a_orig) == RWY_OK, "stat hello");
    CHECK(rwy_stat(s, "/tree/hardlink", &a_link) == RWY_OK, "stat hardlink");
    CHECK(a_orig.oid == a_link.oid, "hardlink OID matches");
    CHECK(a_orig.nlink == 2, "nlink count is 2");

    CHECK(rwy_readlink(s, "/tree/symlink", sbuf, sizeof(sbuf)) == RWY_OK, "readlink symlink");
    CHECK(strcmp(sbuf, "/tree/hello.txt") == 0, "symlink target matches");

    len = rwy_getxattr(s, "/tree/hello.txt", "user.tag", tagbuf, sizeof(tagbuf));
    CHECK(len == 12 && memcmp(tagbuf, "reencrypt-v1", 12) == 0, "small xattr matches");

    len = rwy_getxattr(s, "/tree/pattern.bin", "user.large", bigxbuf, sizeof(bigxbuf));
    CHECK(len == sizeof(bigxbuf), "large xattr length matches");
    for (k = 0; k < sizeof(bigxbuf); k++) {
        if (bigxbuf[k] != (uint8_t)((k * 37) & 0xff)) {
            CHECK(0, "large xattr content mismatch");
            break;
        }
    }
}

/* ========================================================================= */
/* 1. Round Trip Re-encryption                                               */
/* ========================================================================= */
static void test_reencrypt_roundtrip(void) {
    const char *img = tpath("reencrypt_roundtrip.img");
    const char *pass_factory = "FactoryPassphrase2026!";
    const char *pass_runtime = "RuntimePassphrase2026!";
    uint8_t dek_factory[32], dek_runtime[32];
    rwy_keyslot_header_t khdr_factory, khdr_runtime, kh_a, kh_b;
    rwy_crypto_dev_ctx_t cctx;
    rwy_block_dev_t raw_dev, crypto_dev;
    file_t fl;
    rwy_store_t s;
    char mk_cmd[512];
    int rc;

    printf("=== Test 1: Round Trip Re-encryption ===\n");
    snprintf(mk_cmd, sizeof(mk_cmd), "%s --size 64 --encrypt --passphrase '%s' %s > /dev/null",
             mkstore_path(), pass_factory, img);
    CHECK(system(mk_cmd) == 0, "mkstore --encrypt failed");

    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    CHECK(rwy_keyslot_read_from_dev(&raw_dev, &khdr_factory) == 0, "read factory keyslot");
    CHECK(rwy_keyslot_unwrap(&khdr_factory, pass_factory, dek_factory) == 0, "unwrap factory dek");
    CHECK(rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_factory, RWY_SB_BLOCKS) == 0, "wrap crypto");
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount with factory dek");

    populate_test_tree(&s);

    for (int i = 0; i < 32; i++) dek_runtime[i] = (uint8_t)(0xd0 + i);
    rwy_keyslot_init_header(&khdr_runtime);
    CHECK(rwy_keyslot_set_password(&khdr_runtime, 0, pass_runtime, dek_runtime, 1000) == 0, "set runtime slot 0");

    uint64_t old_gen = s.sb.generation;
    rc = rwy_reencrypt(&s, &raw_dev, dek_factory, dek_runtime, &khdr_runtime);
    CHECK(rc == RWY_OK, "rwy_reencrypt returns OK");
    CHECK(s.sb.generation == old_gen + 2, "generation incremented by 2 after finalize");
    fclose(fl.f);

    /* Mounting with factory key must fail */
    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    CHECK(rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_factory, RWY_SB_BLOCKS) == 0, "wrap factory");
    CHECK(rwy_mount(&s, &crypto_dev) != RWY_OK, "mount with factory key fails");

    /* Both slots must hold the runtime keyslot */
    CHECK(read_keyslot_at_slot(&raw_dev, 0, &kh_a) == 0, "read slot A keyslot");
    CHECK(read_keyslot_at_slot(&raw_dev, 1, &kh_b) == 0, "read slot B keyslot");
    CHECK(kh_a.crc32c == khdr_runtime.crc32c, "slot A has runtime keyslot");
    CHECK(kh_b.crc32c == khdr_runtime.crc32c, "slot B has runtime keyslot");
    fclose(fl.f);

    /* Mounting with runtime key must succeed and verify all data */
    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    CHECK(rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_runtime, RWY_SB_BLOCKS) == 0, "wrap runtime");
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount with runtime key succeeds");
    verify_test_tree(&s);
    fclose(fl.f);

    /* Authenticated checker verification */
    CHECK(run_checker(img, pass_runtime) == 0, "fsck.runway with runtime key clean");
    CHECK(run_checker(img, pass_factory) != 0, "fsck.runway with factory key rejected");
}

/* ========================================================================= */
/* 2. Fault Injection at Every Write/Flush                                   */
/* ========================================================================= */
static void check_fault_point_state(const char *img,
                                    const char *pass_factory, const uint8_t dek_factory[32],
                                    const char *pass_runtime, const uint8_t dek_runtime[32]) {
    rwy_crypto_dev_ctx_t cctx;
    rwy_block_dev_t raw_dev, crypto_dev;
    file_t fl;
    rwy_store_t s;
    int runtime_clean = (run_checker(img, pass_runtime) == 0);
    int factory_clean = (run_checker(img, pass_factory) == 0);

    /* Exactly one of the two keys must be accepted */
    CHECK((runtime_clean ^ factory_clean) == 1, "exactly one key is valid");

    const char *exp_pass = runtime_clean ? pass_runtime : pass_factory;
    const char *rej_pass = runtime_clean ? pass_factory : pass_runtime;
    const uint8_t *exp_dek = runtime_clean ? dek_runtime : dek_factory;
    const uint8_t *rej_dek = runtime_clean ? dek_factory : dek_runtime;

    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, exp_dek, RWY_SB_BLOCKS);
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount with expected key succeeds");
    verify_test_tree(&s);

    if (runtime_clean) {
        int pending = 0;
        CHECK(rwy_reencrypt_pending(&s, &pending) == RWY_OK, "check pending");
        if (pending) {
            CHECK(rwy_reencrypt_finish(&s, dek_factory) == RWY_OK, "finish pending reencryption");
            CHECK(rwy_reencrypt_pending(&s, &pending) == RWY_OK && pending == 0, "pending cleared");
        }
    }

    /* Perform a commit after recovery to verify keyslot carry-forward */
    CHECK(rwy_begin(&s) == RWY_OK, "begin txn after recovery");
    CHECK(rwy_create(&s, "/tree/after_fi.txt", 0644, 0, 0) == RWY_OK, "create after_fi");
    CHECK(rwy_write_path(&s, "/tree/after_fi.txt", 0, "commit-ok", 9) == RWY_OK, "write after_fi");
    CHECK(rwy_commit(&s) == RWY_OK, "commit after recovery");
    fclose(fl.f);

    /* Re-verify after commit */
    CHECK(run_checker(img, exp_pass) == 0, "checker clean after follow-up commit");
    CHECK(run_checker(img, rej_pass) != 0, "checker rejected after follow-up commit");

    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, rej_dek, RWY_SB_BLOCKS);
    CHECK(rwy_mount(&s, &crypto_dev) != RWY_OK, "mount with rejected key fails");
    fclose(fl.f);
}

static void test_reencrypt_fault_injection(void) {
    const char *img = tpath("reencrypt_fi.img");
    const char *bak = tpath("reencrypt_fi.bak");
    const char *pass_factory = "FactoryPassphrase2026!";
    const char *pass_runtime = "RuntimePassphrase2026!";
    uint8_t dek_factory[32], dek_runtime[32];
    rwy_keyslot_header_t khdr_factory, khdr_runtime;
    rwy_crypto_dev_ctx_t cctx;
    rwy_block_dev_t raw_dev, crypto_dev, fi_raw;
    fi_dev_t fi;
    file_t fl;
    rwy_store_t s;
    char mk_cmd[512];
    long total_points, p;
    int rc;

    printf("=== Test 2: Crash at Every Write and Flush ===\n");
    snprintf(mk_cmd, sizeof(mk_cmd), "%s --size 64 --encrypt --passphrase '%s' %s > /dev/null",
             mkstore_path(), pass_factory, bak);
    CHECK(system(mk_cmd) == 0, "mkstore --encrypt failed");

    open_raw_dev(&raw_dev, &fl, bak, 64ULL * 1024 * 1024);
    CHECK(rwy_keyslot_read_from_dev(&raw_dev, &khdr_factory) == 0, "read factory keyslot");
    CHECK(rwy_keyslot_unwrap(&khdr_factory, pass_factory, dek_factory) == 0, "unwrap factory dek");
    CHECK(rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_factory, RWY_SB_BLOCKS) == 0, "wrap crypto");
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount with factory dek");
    populate_test_tree(&s);
    fclose(fl.f);

    for (int i = 0; i < 32; i++) dek_runtime[i] = (uint8_t)(0xb0 + i);
    rwy_keyslot_init_header(&khdr_runtime);
    CHECK(rwy_keyslot_set_password(&khdr_runtime, 0, pass_runtime, dek_runtime, 1000) == 0, "set runtime slot 0");

    /* Count write and flush points */
    copy_file(bak, img);
    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_factory, RWY_SB_BLOCKS);
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount for point count");
    fi_wrap(&fi_raw, &fi, &raw_dev, -1);
    rc = rwy_reencrypt(&s, &fi_raw, dek_factory, dek_runtime, &khdr_runtime);
    CHECK(rc == RWY_OK, "baseline clean reencrypt");
    total_points = fi.point;
    fclose(fl.f);

    printf("Total fault injection points in rwy_reencrypt: %ld\n", total_points);
    CHECK(total_points > 5, "reasonable point count");

    for (p = 1; p <= total_points; p++) {
        copy_file(bak, img);
        open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
        rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_factory, RWY_SB_BLOCKS);
        if (rwy_mount(&s, &crypto_dev) != RWY_OK) {
            fclose(fl.f);
            CHECK(0, "mount failed before fault injection");
            continue;
        }

        fi_wrap(&fi_raw, &fi, &raw_dev, p);
        rc = rwy_reencrypt(&s, &fi_raw, dek_factory, dek_runtime, &khdr_runtime);
        (void)rc;
        fclose(fl.f);

        check_fault_point_state(img, pass_factory, dek_factory, pass_runtime, dek_runtime);
    }
}

/* ========================================================================= */
/* 3. Crash During Finalize                                                  */
/* ========================================================================= */
static void test_reencrypt_finalize_fi(void) {
    const char *img = tpath("reencrypt_fin_fi.img");
    const char *bak = tpath("reencrypt_fin_fi.bak");
    const char *pass_factory = "FactoryPassphrase2026!";
    const char *pass_runtime = "RuntimePassphrase2026!";
    uint8_t dek_factory[32], dek_runtime[32];
    rwy_keyslot_header_t khdr_factory, khdr_runtime;
    rwy_crypto_dev_ctx_t cctx, new_cctx;
    rwy_block_dev_t raw_dev, crypto_dev, new_dev, fi_raw;
    fi_dev_t fi;
    file_t fl;
    rwy_store_t s;
    char mk_cmd[512];
    long total_points, p;
    int pending, rc;

    printf("=== Test 3: Crash During Finalize ===\n");
    snprintf(mk_cmd, sizeof(mk_cmd), "%s --size 64 --encrypt --passphrase '%s' %s > /dev/null",
             mkstore_path(), pass_factory, bak);
    CHECK(system(mk_cmd) == 0, "mkstore --encrypt failed");

    open_raw_dev(&raw_dev, &fl, bak, 64ULL * 1024 * 1024);
    CHECK(rwy_keyslot_read_from_dev(&raw_dev, &khdr_factory) == 0, "read factory keyslot");
    CHECK(rwy_keyslot_unwrap(&khdr_factory, pass_factory, dek_factory) == 0, "unwrap factory dek");
    CHECK(rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_factory, RWY_SB_BLOCKS) == 0, "wrap crypto");
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount with factory dek");
    populate_test_tree(&s);

    for (int i = 0; i < 32; i++) dek_runtime[i] = (uint8_t)(0xaa + i);
    rwy_keyslot_init_header(&khdr_runtime);
    CHECK(rwy_keyslot_set_password(&khdr_runtime, 0, pass_runtime, dek_runtime, 1000) == 0, "set runtime slot 0");

    /* Relocate only without finalize, so finalize is pending */
    CHECK(rwy_crypto_dev_wrap(&new_dev, &new_cctx, &raw_dev, dek_runtime, RWY_SB_BLOCKS) == 0, "wrap new dev");
    CHECK(rwy_relocate(&s, &new_dev, &khdr_runtime) == RWY_OK, "relocate to target");
    fclose(fl.f);

    /* Verify finalize is pending on bak */
    open_raw_dev(&raw_dev, &fl, bak, 64ULL * 1024 * 1024);
    rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_runtime, RWY_SB_BLOCKS);
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount after relocate");
    pending = 0;
    CHECK(rwy_reencrypt_pending(&s, &pending) == RWY_OK && pending == 1, "finalize is pending");

    /* Measure finalize write and flush points */
    fi_wrap(&fi_raw, &fi, &raw_dev, -1);
    rwy_crypto_dev_wrap(&crypto_dev, &cctx, &fi_raw, dek_runtime, RWY_SB_BLOCKS);
    s.dev = crypto_dev;
    rc = rwy_reencrypt_finish(&s, dek_factory);
    CHECK(rc == RWY_OK, "baseline clean finalize");
    total_points = fi.point;
    fclose(fl.f);

    printf("Total fault points in finalize: %ld\n", total_points);
    CHECK(total_points > 5, "reasonable finalize points");

    for (p = 1; p <= total_points; p++) {
        copy_file(bak, img);
        open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
        fi_wrap(&fi_raw, &fi, &raw_dev, p);
        rwy_crypto_dev_wrap(&crypto_dev, &cctx, &fi_raw, dek_runtime, RWY_SB_BLOCKS);
        if (rwy_mount(&s, &crypto_dev) != RWY_OK) {
            fclose(fl.f);
            CHECK(0, "mount failed before finalize fi");
            continue;
        }

        rc = rwy_reencrypt_finish(&s, dek_factory);
        (void)rc;
        fclose(fl.f);

        /* After cut: runtime mounts, factory is rejected */
        CHECK(run_checker(img, pass_runtime) == 0, "checker clean with runtime after cut");
        CHECK(run_checker(img, pass_factory) != 0, "checker rejected with factory after cut");

        open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
        rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_runtime, RWY_SB_BLOCKS);
        CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount after cut succeeds");

        /* Resume finalize */
        rc = rwy_reencrypt_finish(&s, dek_factory);
        CHECK(rc == RWY_OK, "resume finalize succeeds");

        pending = 1;
        CHECK(rwy_reencrypt_pending(&s, &pending) == RWY_OK && pending == 0, "pending cleared after resume");
        verify_test_tree(&s);
        fclose(fl.f);
    }
}

/* ========================================================================= */
/* 4. No Leak Verification                                                   */
/* ========================================================================= */
static void test_reencrypt_no_leak(void) {
    const char *img = tpath("reencrypt_noleak.img");
    const char *pass_factory = "FactoryPassphrase2026!";
    uint8_t dek_factory[32], dek_runtime[32];
    rwy_keyslot_header_t khdr_factory, khdr_runtime, kh_a, kh_b;
    rwy_crypto_dev_ctx_t cctx;
    rwy_block_dev_t raw_dev, crypto_dev;
    file_t fl;
    rwy_store_t s;
    rwy_xts_ctx_t xts_fac;
    char mk_cmd[512];
    uint8_t raw_blk[RWY_BLOCK_SIZE], dec_blk[RWY_BLOCK_SIZE];
    static const char *markers[] = {
        "Hello, Reencryption!",
        "reencrypt-v1",
        "user.large",
    };
    uint64_t total_blocks;
    int rc;

    printf("=== Test 4: No Leak Verification ===\n");
    snprintf(mk_cmd, sizeof(mk_cmd), "%s --size 64 --encrypt --passphrase '%s' %s > /dev/null",
             mkstore_path(), pass_factory, img);
    CHECK(system(mk_cmd) == 0, "mkstore --encrypt failed");

    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    CHECK(rwy_keyslot_read_from_dev(&raw_dev, &khdr_factory) == 0, "read factory keyslot");
    CHECK(rwy_keyslot_unwrap(&khdr_factory, pass_factory, dek_factory) == 0, "unwrap factory dek");
    CHECK(rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_factory, RWY_SB_BLOCKS) == 0, "wrap crypto");
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount with factory dek");

    populate_test_tree(&s);

    for (int i = 0; i < 32; i++) dek_runtime[i] = (uint8_t)(0x77 + i);
    rwy_keyslot_init_header(&khdr_runtime);
    /* Enrolled state: keyslot 0 empty, keyslot 1 active */
    uint8_t dummy_blob[64] = { 0x5a };
    uint8_t dummy_pcr[32] = { 0xaa };
    CHECK(rwy_keyslot_set_tpm2_blob(&khdr_runtime, 1, dummy_blob, sizeof(dummy_blob), dummy_pcr) == 0,
          "set runtime slot 1");
    khdr_runtime.slots[0].state = RWY_KEYSLOT_STATE_EMPTY;
    khdr_runtime.crc32c = 0;
    khdr_runtime.crc32c = rwy_crc32c(&khdr_runtime, sizeof(khdr_runtime));

    rc = rwy_reencrypt(&s, &raw_dev, dek_factory, dek_runtime, &khdr_runtime);
    CHECK(rc == RWY_OK, "rwy_reencrypt clean");
    total_blocks = s.sb.total_blocks;
    fclose(fl.f);

    /* Assert slot A and slot B have no active keyslot 0 */
    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    CHECK(read_keyslot_at_slot(&raw_dev, 0, &kh_a) == 0, "read slot A keyslot");
    CHECK(read_keyslot_at_slot(&raw_dev, 1, &kh_b) == 0, "read slot B keyslot");
    CHECK(kh_a.slots[0].state == RWY_KEYSLOT_STATE_EMPTY, "slot A has no active keyslot 0");
    CHECK(kh_b.slots[0].state == RWY_KEYSLOT_STATE_EMPTY, "slot B has no active keyslot 0");
    CHECK(kh_a.slots[1].state == RWY_KEYSLOT_STATE_ACTIVE, "slot A slot 1 active");
    CHECK(kh_b.slots[1].state == RWY_KEYSLOT_STATE_ACTIVE, "slot B slot 1 active");

    /* Decrypt every block >= 2 with DEK_factory and assert no leaks */
    rwy_xts_init_from_dek(&xts_fac, dek_factory);
    for (uint64_t b = RWY_SB_BLOCKS; b < total_blocks; b++) {
        CHECK(raw_dev.read(raw_dev.ctx, b * RWY_BLOCK_SIZE, raw_blk, RWY_BLOCK_SIZE) == 0, "read block");
        rwy_xts_decrypt_sector(&xts_fac, b, raw_blk, dec_blk, RWY_BLOCK_SIZE);

        for (size_t m = 0; m < sizeof(markers) / sizeof(markers[0]); m++) {
            if (find_mem(dec_blk, RWY_BLOCK_SIZE, markers[m])) {
                printf("FAIL: leak found at block %lu: marker %s\n", (unsigned long)b, markers[m]);
                failures++;
            }
        }

        const rwy_blk_header_t *h = (const rwy_blk_header_t *)dec_blk;
        if (h->magic == RWY_BLK_MAGIC && h->generation != 0 &&
            rwy_blk_compute_crc(dec_blk) == h->crc32c) {
            printf("FAIL: leak found at block %lu: valid block header under factory key\n", (unsigned long)b);
            failures++;
        }
    }
    fclose(fl.f);
}

/* ========================================================================= */
/* 5. NOSPC Handling                                                         */
/* ========================================================================= */
static void test_reencrypt_nospc(void) {
    const char *img = tpath("reencrypt_nospc.img");
    const char *bak = tpath("reencrypt_nospc.bak");
    const char *pass_factory = "FactoryPassphrase2026!";
    const char *pass_runtime = "RuntimePassphrase2026!";
    uint8_t dek_factory[32], dek_runtime[32];
    rwy_keyslot_header_t khdr_factory, khdr_runtime;
    rwy_crypto_dev_ctx_t cctx;
    rwy_block_dev_t raw_dev, crypto_dev;
    file_t fl;
    rwy_store_t s;
    char mk_cmd[512];
    uint32_t chunk_sz = 1 << 20;
    uint32_t passes = 36; /* 36 MiB in a 64 MiB store leaves < half space */
    uint8_t *chunk;
    int rc;

    printf("=== Test 5: NOSPC Condition ===\n");
    snprintf(mk_cmd, sizeof(mk_cmd), "%s --size 64 --encrypt --passphrase '%s' %s > /dev/null",
             mkstore_path(), pass_factory, img);
    CHECK(system(mk_cmd) == 0, "mkstore --encrypt failed");

    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    CHECK(rwy_keyslot_read_from_dev(&raw_dev, &khdr_factory) == 0, "read factory keyslot");
    CHECK(rwy_keyslot_unwrap(&khdr_factory, pass_factory, dek_factory) == 0, "unwrap factory dek");
    CHECK(rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_factory, RWY_SB_BLOCKS) == 0, "wrap crypto");
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount with factory dek");

    chunk = malloc(chunk_sz);
    CHECK(chunk != NULL, "malloc chunk");
    memset(chunk, 0xaa, chunk_sz);

    CHECK(rwy_begin(&s) == RWY_OK, "begin bigfill");
    CHECK(rwy_create(&s, "/bigfill.bin", 0644, 0, 0) == RWY_OK, "create bigfill");
    for (uint32_t p = 0; p < passes; p++) {
        rc = rwy_write_path(&s, "/bigfill.bin", (uint64_t)p * chunk_sz, chunk, chunk_sz);
        if (rc != RWY_OK) break;
    }
    CHECK(rwy_commit(&s) == RWY_OK, "commit bigfill");
    free(chunk);
    fclose(fl.f);

    copy_file(img, bak);

    for (int i = 0; i < 32; i++) dek_runtime[i] = (uint8_t)(0xc0 + i);
    rwy_keyslot_init_header(&khdr_runtime);
    rwy_keyslot_set_password(&khdr_runtime, 0, pass_runtime, dek_runtime, 1000);

    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_factory, RWY_SB_BLOCKS);
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount before NOSPC reencrypt");

    rc = rwy_reencrypt(&s, &raw_dev, dek_factory, dek_runtime, &khdr_runtime);
    CHECK(rc == RWY_ERR_NOSPC, "rwy_reencrypt returns NOSPC");
    fclose(fl.f);

    CHECK(files_identical(img, bak), "image byte-identical after NOSPC");
}

/* ========================================================================= */
/* 6. State Record Preservation and Cut Behavior                             */
/* ========================================================================= */
static void test_reencrypt_record(void) {
    const char *img = tpath("reencrypt_record.img");
    const char *pass_factory = "FactoryPassphrase2026!";
    const char *pass_runtime = "RuntimePassphrase2026!";
    uint8_t dek_factory[32], dek_runtime[32];
    rwy_keyslot_header_t khdr_factory, khdr_runtime;
    rwy_crypto_dev_ctx_t cctx, new_cctx;
    rwy_block_dev_t raw_dev, crypto_dev, new_dev, fi_raw;
    fi_dev_t fi;
    file_t fl;
    rwy_store_t s;
    char mk_cmd[512];
    uint64_t epoch;
    int enrolled, rc;

    printf("=== Test 6: State Record Preservation ===\n");
    snprintf(mk_cmd, sizeof(mk_cmd), "%s --size 64 --encrypt --passphrase '%s' %s > /dev/null",
             mkstore_path(), pass_factory, img);
    CHECK(system(mk_cmd) == 0, "mkstore --encrypt failed");

    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    CHECK(rwy_keyslot_read_from_dev(&raw_dev, &khdr_factory) == 0, "read factory keyslot");
    CHECK(rwy_keyslot_unwrap(&khdr_factory, pass_factory, dek_factory) == 0, "unwrap factory dek");
    CHECK(rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_factory, RWY_SB_BLOCKS) == 0, "wrap crypto");
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount with factory dek");

    CHECK(rwy_anchor_set(&s, 42, 1) == RWY_OK, "set anchor record");
    uint64_t prev_rec_seq = s.rec_seq;
    fclose(fl.f);

    for (int i = 0; i < 32; i++) dek_runtime[i] = (uint8_t)(0x88 + i);
    rwy_keyslot_init_header(&khdr_runtime);
    rwy_keyslot_set_password(&khdr_runtime, 0, pass_runtime, dek_runtime, 1000);

    /* Clean re-encryption: record is preserved with seq+2 */
    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_factory, RWY_SB_BLOCKS);
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount for clean reencrypt");
    CHECK(rwy_reencrypt(&s, &raw_dev, dek_factory, dek_runtime, &khdr_runtime) == RWY_OK, "clean reencrypt");
    fclose(fl.f);

    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_runtime, RWY_SB_BLOCKS);
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount under runtime key");
    CHECK(s.rec_seq == prev_rec_seq + 2, "rec_seq incremented by 2");
    rwy_anchor_get(&s, &epoch, &enrolled);
    CHECK(epoch == 42 && enrolled == 1, "anchor values preserved under runtime key");
    fclose(fl.f);

    /* Cut after record write and flush, before publish. */
    const char *rec_bak = tpath("reencrypt_record.bak");
    const char *rec_dek = tpath("reencrypt_record.dek");
    FILE *df = fopen(rec_dek, "wb");
    CHECK(df && fwrite(dek_factory, 1, 32, df) == 32, "write the factory key");
    if (df) fclose(df);
    snprintf(mk_cmd, sizeof(mk_cmd), "%s --size 64 --encrypt --passphrase '%s' --dek-in %s %s > /dev/null",
             mkstore_path(), pass_factory, rec_dek, rec_bak);
    CHECK(system(mk_cmd) == 0, "mkstore --encrypt 2");

    open_raw_dev(&raw_dev, &fl, rec_bak, 64ULL * 1024 * 1024);
    rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_factory, RWY_SB_BLOCKS);
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount the record store");
    CHECK(rwy_begin(&s) == RWY_OK, "begin rec txn");
    CHECK(rwy_commit(&s) == RWY_OK, "commit rec txn");
    rwy_anchor_set(&s, 77, 1);
    prev_rec_seq = s.rec_seq;
    fclose(fl.f);

    /* Measure points in rwy_relocate */
    copy_file(rec_bak, img);
    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_factory, RWY_SB_BLOCKS);
    rwy_mount(&s, &crypto_dev);
    CHECK(rwy_crypto_dev_wrap(&new_dev, &new_cctx, &raw_dev, dek_runtime, RWY_SB_BLOCKS) == 0, "wrap new dev");
    fi_wrap(&fi_raw, &fi, &new_dev, -1);
    CHECK(rwy_relocate(&s, &fi_raw, &khdr_runtime) == RWY_OK, "measure relocate points");
    long reloc_total = fi.point;
    fclose(fl.f);

    /* Cut after the record write and flush, before the publish: 20 points before the end of the relocation */
    copy_file(rec_bak, img);
    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_factory, RWY_SB_BLOCKS);
    rwy_mount(&s, &crypto_dev);
    fi_wrap(&fi_raw, &fi, &raw_dev, reloc_total - 20);
    rc = rwy_reencrypt(&s, &fi_raw, dek_factory, dek_runtime, &khdr_runtime);
    CHECK(rc != RWY_OK, "reencrypt failed at cut before publish");
    fclose(fl.f);

    /* Mount under factory key: sees current copy intact, ignores corrupt new copy */
    open_raw_dev(&raw_dev, &fl, img, 64ULL * 1024 * 1024);
    rwy_crypto_dev_wrap(&crypto_dev, &cctx, &raw_dev, dek_factory, RWY_SB_BLOCKS);
    CHECK(rwy_mount(&s, &crypto_dev) == RWY_OK, "mount under factory key after record cut");
    CHECK(s.rec_seq == prev_rec_seq, "rec_seq unchanged");
    rwy_anchor_get(&s, &epoch, &enrolled);
    CHECK(epoch == 77 && enrolled == 1, "anchor values intact after record cut");
    fclose(fl.f);
}

int main(int argc, char **argv) {
    if (argc > 2) g_checkbin = argv[2];

    test_reencrypt_roundtrip();
    test_reencrypt_fault_injection();
    test_reencrypt_finalize_fi();
    test_reencrypt_no_leak();
    test_reencrypt_nospc();
    test_reencrypt_record();

    if (failures) {
        printf("FAILED (%d errors)\n", failures);
        return 1;
    }
    printf("ALL RE-ENCRYPT TESTS PASSED\n");
    return 0;
}
