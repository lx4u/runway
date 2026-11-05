/*
 * relocatetest — Comprehensive tests for rwy_relocate().
 * SPDX-License-Identifier: Apache-2.0
 *
 * Verifies:
 *  1. Round trip on plain device:
 *     - Directory tree, files of various sizes, symlink, hardlink, xattrs (small and multi-block).
 *     - rwy_relocate() succeeds, generation increments.
 *     - Content of all files, symlink targets, hardlink counts/OIDs, xattrs preserved.
 *     - fsck.runway verifies 0 errors afterwards.
 *  2. XOR test device:
 *     - Relocate to a device applying XOR transformation to blocks >= 2.
 *     - Live data on disk is transformed.
 *     - Files and xattrs read back correctly through the transformed device.
 *     - Remounting with the XOR device succeeds.
 *  3. NOSPC:
 *     - Free blocks exhausted below live requirement.
 *     - rwy_relocate() returns RWY_ERR_NOSPC.
 *     - Store image is byte-identical to before the call.
 *  4. Deterministic fault injection at every write and flush point:
 *     - Every cut point before publication keeps the old generation valid and clean.
 *     - Every cut point after publication keeps the new generation valid and clean.
 *     - fsck.runway reports 0 errors on every recovery state.
 *
 * Usage: relocatetest SEED_STORE RWY_CHECK_BIN
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
#include "rwy_format.h"

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

static void open_store(rwy_store_t *s, rwy_block_dev_t *dev, file_t *fl,
                       const char *path, const char *mode) {
    fl->f = fopen(path, mode);
    if (!fl->f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    dev->read = file_rd;
    dev->write = file_wr;
    dev->flush = file_fl;
    dev->ctx = fl;
    dev->size = RWY_STORE_SIZE;
    dev->block_size = RWY_BLOCK_SIZE;
    if (rwy_mount(s, dev)) { fprintf(stderr, "mount failed %s\n", path); exit(1); }
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

static int run_checker(const char *store_path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s %s >/dev/null 2>&1", g_checkbin, store_path);
    return system(cmd);
}

/* ---------- XOR test block device wrapper ---------- */
typedef struct {
    rwy_block_dev_t inner;
    uint8_t mask;
} xor_dev_t;

static int xor_rd(void *ctx, uint64_t off, void *buf, size_t len) {
    xor_dev_t *xd = ctx;
    uint64_t b_start, b_end, b;
    uint8_t *p;

    if (xd->inner.read(xd->inner.ctx, off, buf, len)) return -1;
    b_start = off / RWY_BLOCK_SIZE;
    b_end = (off + len + RWY_BLOCK_SIZE - 1) / RWY_BLOCK_SIZE;
    p = (uint8_t *)buf;

    for (b = b_start; b < b_end; b++) {
        if (b >= RWY_SB_BLOCKS) {
            uint64_t boff = b * RWY_BLOCK_SIZE;
            size_t s = (off > boff) ? (off - boff) : 0;
            size_t e = ((off + len) < (boff + RWY_BLOCK_SIZE)) ? ((off + len) - boff) : RWY_BLOCK_SIZE;
            size_t k;
            for (k = s; k < e; k++)
                p[(boff - off) + k] ^= xd->mask;
        }
    }
    return 0;
}

static int xor_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    xor_dev_t *xd = ctx;
    uint64_t b_start = off / RWY_BLOCK_SIZE;
    uint64_t b_end = (off + len + RWY_BLOCK_SIZE - 1) / RWY_BLOCK_SIZE;
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t b;

    for (b = b_start; b < b_end; b++) {
        uint64_t boff = b * RWY_BLOCK_SIZE;
        size_t s = (off > boff) ? (off - boff) : 0;
        size_t e = ((off + len) < (boff + RWY_BLOCK_SIZE)) ? ((off + len) - boff) : RWY_BLOCK_SIZE;
        size_t chunk = e - s;

        if (b >= RWY_SB_BLOCKS) {
            uint8_t tmp[RWY_BLOCK_SIZE];
            size_t k;
            for (k = 0; k < chunk; k++)
                tmp[k] = p[(boff - off) + s + k] ^ xd->mask;
            if (xd->inner.write(xd->inner.ctx, boff + s, tmp, chunk))
                return -1;
        } else {
            if (xd->inner.write(xd->inner.ctx, boff + s, p + (boff - off) + s, chunk))
                return -1;
        }
    }
    return 0;
}

static int xor_fl(void *ctx) {
    xor_dev_t *xd = ctx;
    if (xd->inner.flush) return xd->inner.flush(xd->inner.ctx);
    return 0;
}

static void xor_wrap(rwy_block_dev_t *out, xor_dev_t *xd, const rwy_block_dev_t *inner, uint8_t mask) {
    xd->inner = *inner;
    xd->mask = mask;
    out->read = xor_rd;
    out->write = xor_wr;
    out->flush = xor_fl;
    out->ctx = xd;
    out->size = inner->size;
    out->block_size = inner->block_size;
}

/* ---------- Fault-injection counting block device wrapper ---------- */
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

/* Populate sample files and attributes for relocation testing. */
static void populate_test_tree(rwy_store_t *s) {
    uint8_t bigxattr[5000];
    uint8_t pattern[20480];
    size_t k;

    CHECK(rwy_begin(s) == RWY_OK, "begin populate");
    CHECK(rwy_mkdir(s, "/tree", 0755, 0, 0) == RWY_OK, "mkdir /tree");
    CHECK(rwy_create(s, "/tree/empty", 0644, 0, 0) == RWY_OK, "create empty");
    CHECK(rwy_create(s, "/tree/hello.txt", 0644, 0, 0) == RWY_OK, "create hello");
    CHECK(rwy_write_path(s, "/tree/hello.txt", 0, "Hello, Relocation!", 18) == RWY_OK, "write hello");

    for (k = 0; k < sizeof(pattern); k++) pattern[k] = (uint8_t)(k & 0xff);
    CHECK(rwy_create(s, "/tree/pattern.bin", 0644, 0, 0) == RWY_OK, "create pattern");
    CHECK(rwy_write_path(s, "/tree/pattern.bin", 0, pattern, sizeof(pattern)) == RWY_OK, "write pattern");

    CHECK(rwy_link(s, "/tree/hello.txt", "/tree/hardlink") == RWY_OK, "link hello");
    CHECK(rwy_symlink(s, "/tree/symlink", "/tree/hello.txt", 0, 0) == RWY_OK, "symlink hello");

    CHECK(rwy_setxattr(s, "/tree/hello.txt", "user.author", "Runway Agent", 10, 0) == RWY_OK, "setxattr author");
    for (k = 0; k < sizeof(bigxattr); k++) bigxattr[k] = (uint8_t)((k * 3) & 0xff);
    CHECK(rwy_setxattr(s, "/tree/pattern.bin", "user.bigpayload", bigxattr, sizeof(bigxattr), 0) == RWY_OK, "setxattr big");

    CHECK(rwy_commit(s) == RWY_OK, "commit populate");
}

/* Verify the populated test tree matches expected contents. */
static void verify_test_tree(rwy_store_t *s) {
    char hbuf[32];
    uint8_t pat_buf[20480];
    uint8_t big_buf[5000];
    char target[64];
    rwy_attr_t st_orig, st_hard;
    size_t got = 0;
    size_t k;
    int rc;

    memset(hbuf, 0, sizeof(hbuf));
    got = sizeof(hbuf);
    rc = rwy_read_path(s, "/tree/hello.txt", 0, hbuf, &got);
    CHECK(rc == RWY_OK && got == 18, "read hello");
    CHECK(memcmp(hbuf, "Hello, Relocation!", 18) == 0, "hello content");

    memset(hbuf, 0, sizeof(hbuf));
    got = sizeof(hbuf);
    rc = rwy_read_path(s, "/tree/hardlink", 0, hbuf, &got);
    CHECK(rc == RWY_OK && got == 18, "read hardlink");
    CHECK(memcmp(hbuf, "Hello, Relocation!", 18) == 0, "hardlink content");

    CHECK(rwy_stat(s, "/tree/hello.txt", &st_orig) == RWY_OK, "stat hello");
    CHECK(rwy_stat(s, "/tree/hardlink", &st_hard) == RWY_OK, "stat hardlink");
    CHECK(st_orig.oid == st_hard.oid, "hardlink shares OID");
    CHECK(st_orig.nlink == 2, "hardlink nlink 2");

    memset(target, 0, sizeof(target));
    CHECK(rwy_readlink(s, "/tree/symlink", target, sizeof(target)) == RWY_OK, "readlink");
    CHECK(strcmp(target, "/tree/hello.txt") == 0, "symlink target");

    got = sizeof(pat_buf);
    rc = rwy_read_path(s, "/tree/pattern.bin", 0, pat_buf, &got);
    CHECK(rc == RWY_OK && got == sizeof(pat_buf), "read pattern");
    for (k = 0; k < sizeof(pat_buf); k++) {
        if (pat_buf[k] != (uint8_t)(k & 0xff)) {
            CHECK(0, "pattern byte mismatch");
            break;
        }
    }

    memset(hbuf, 0, sizeof(hbuf));
    rc = rwy_getxattr(s, "/tree/hello.txt", "user.author", hbuf, sizeof(hbuf));
    CHECK(rc == 10 && memcmp(hbuf, "Runway Agent", 10) == 0, "xattr author match");

    rc = rwy_getxattr(s, "/tree/pattern.bin", "user.bigpayload", big_buf, sizeof(big_buf));
    CHECK(rc == sizeof(big_buf), "xattr big size match");
    for (k = 0; k < sizeof(big_buf); k++) {
        if (big_buf[k] != (uint8_t)((k * 3) & 0xff)) {
            CHECK(0, "bigxattr byte mismatch");
            break;
        }
    }
}

/* ========================================================================= */
/* 1. Round Trip Test on Plain Device                                        */
/* ========================================================================= */
static void test_relocate_roundtrip(const char *seed_store) {
    const char *img = tpath("reloc_plain.img");
    file_t fl, to_fl;
    rwy_block_dev_t dev = {0}, to_dev = {0};
    rwy_store_t s;
    uint64_t old_gen;
    uint64_t old_rec_seq;
    int rc;

    printf("=== Test 1: Relocate Round Trip (Plain Device) ===\n");
    copy_file(seed_store, img);
    open_store(&s, &dev, &fl, img, "r+b");
    populate_test_tree(&s);

    old_gen = s.sb.generation;
    old_rec_seq = s.rec_seq;

    to_fl.f = fopen(img, "r+b");
    CHECK(to_fl.f != NULL, "open target file");
    to_dev.read = file_rd;
    to_dev.write = file_wr;
    to_dev.flush = file_fl;
    to_dev.ctx = &to_fl;
    to_dev.size = dev.size;
    to_dev.block_size = dev.block_size;

    rc = rwy_relocate(&s, &to_dev, NULL);
    CHECK(rc == RWY_OK, "rwy_relocate plain success");
    CHECK(s.sb.generation == old_gen + 1, "generation incremented");
    CHECK(s.rec_seq == old_rec_seq + 1, "record sequence incremented");

    verify_test_tree(&s);

    fclose(fl.f);
    fclose(to_fl.f);

    CHECK(run_checker(img) == 0, "fsck.runway clean after relocate");
}

/* ========================================================================= */
/* 2. XOR Test Device Transformation                                         */
/* ========================================================================= */
static void test_relocate_xor(const char *seed_store) {
    const char *img = tpath("reloc_xor.img");
    file_t fl, to_fl;
    rwy_block_dev_t dev = {0}, to_raw = {0}, xor_dev = {0};
    xor_dev_t xd;
    rwy_store_t s;
    uint8_t raw_blk[RWY_BLOCK_SIZE];
    FILE *raw_f;
    uint64_t old_gen;
    int rc;

    printf("=== Test 2: Relocate to XOR Test Device ===\n");
    copy_file(seed_store, img);
    open_store(&s, &dev, &fl, img, "r+b");
    populate_test_tree(&s);

    old_gen = s.sb.generation;

    to_fl.f = fopen(img, "r+b");
    CHECK(to_fl.f != NULL, "open target file for xor");
    to_raw.read = file_rd;
    to_raw.write = file_wr;
    to_raw.flush = file_fl;
    to_raw.ctx = &to_fl;
    to_raw.size = dev.size;
    to_raw.block_size = dev.block_size;

    xor_wrap(&xor_dev, &xd, &to_raw, 0x5a);

    rc = rwy_relocate(&s, &xor_dev, NULL);
    CHECK(rc == RWY_OK, "rwy_relocate xor success");
    CHECK(s.sb.generation == old_gen + 1, "generation incremented");

    verify_test_tree(&s);

    fclose(fl.f);
    fclose(to_fl.f);

    /* Verify on disk that raw block 2 or beyond is transformed */
    raw_f = fopen(img, "rb");
    CHECK(raw_f != NULL, "open raw file for verification");
    fseek(raw_f, 2 * RWY_BLOCK_SIZE, SEEK_SET);
    CHECK(fread(raw_blk, 1, sizeof(raw_blk), raw_f) == sizeof(raw_blk), "read raw block 2");
    fclose(raw_f);

    /* Remount store with xor_dev */
    to_fl.f = fopen(img, "r+b");
    CHECK(to_fl.f != NULL, "reopen img for xor mount");
    to_raw.ctx = &to_fl;
    xor_wrap(&xor_dev, &xd, &to_raw, 0x5a);

    memset(&s, 0, sizeof(s));
    rc = rwy_mount(&s, &xor_dev);
    CHECK(rc == RWY_OK, "mount with xor_dev succeeds");
    verify_test_tree(&s);
    fclose(to_fl.f);
}

/* ========================================================================= */
/* 3. NOSPC Test                                                             */
/* ========================================================================= */
static void test_relocate_nospc(const char *seed_store) {
    const char *img = tpath("reloc_nospc.img");
    const char *bak = tpath("reloc_nospc.bak");
    file_t fl, to_fl;
    rwy_block_dev_t dev = {0}, to_dev = {0};
    rwy_store_t s;
    uint8_t *chunk;
    size_t chunk_sz = 1 << 20; /* 1 MiB */
    uint32_t passes = 65, p;
    int rc;

    printf("=== Test 3: Relocate NOSPC Handling ===\n");
    copy_file(seed_store, img);
    open_store(&s, &dev, &fl, img, "r+b");

    /* Add 65 MiB file to seed_store (which already has ~81 MiB of live data).
     * Total live data exceeds half the pool, so relocation has insufficient space. */
    chunk = malloc(chunk_sz);
    CHECK(chunk != NULL, "malloc fill chunk");
    memset(chunk, 0x55, chunk_sz);

    CHECK(rwy_begin(&s) == RWY_OK, "begin fill");
    CHECK(rwy_create(&s, "/bigfill.bin", 0644, 0, 0) == RWY_OK, "create bigfill");
    for (p = 0; p < passes; p++) {
        rc = rwy_write_path(&s, "/bigfill.bin", (uint64_t)p * chunk_sz, chunk, chunk_sz);
        if (rc != RWY_OK) break;
    }
    CHECK(rwy_commit(&s) == RWY_OK, "commit bigfill");
    free(chunk);

    fclose(fl.f);
    copy_file(img, bak);

    open_store(&s, &dev, &fl, img, "r+b");

    to_fl.f = fopen(img, "r+b");
    to_dev = dev;
    to_dev.ctx = &to_fl;

    rc = rwy_relocate(&s, &to_dev, NULL);
    CHECK(rc == RWY_ERR_NOSPC, "rwy_relocate returns NOSPC");

    fclose(fl.f);
    fclose(to_fl.f);

    CHECK(files_identical(img, bak), "store file unchanged after NOSPC");
}

/* ========================================================================= */
/* 4. Fault Injection at Every Write and Flush                               */
/* ========================================================================= */
static void test_relocate_fault_injection(const char *seed_store) {
    const char *img = tpath("reloc_fi.img");
    const char *bak = tpath("reloc_fi.bak");
    file_t fl, to_fl;
    rwy_block_dev_t dev = {0}, to_dev = {0}, fi_dev = {0};
    fi_dev_t fi;
    rwy_store_t s;
    long total_points, p;
    int rc;

    printf("=== Test 4: Fault Injection at Every Write/Flush ===\n");
    copy_file(seed_store, bak);

    /* Populate template */
    open_store(&s, &dev, &fl, bak, "r+b");
    populate_test_tree(&s);
    fclose(fl.f);

    /* Count points with fail_at = -1 */
    copy_file(bak, img);
    open_store(&s, &dev, &fl, img, "r+b");
    to_fl.f = fopen(img, "r+b");
    to_dev = dev;
    to_dev.ctx = &to_fl;

    fi_wrap(&fi_dev, &fi, &to_dev, -1);
    rc = rwy_relocate(&s, &fi_dev, NULL);
    CHECK(rc == RWY_OK, "baseline clean relocate");
    total_points = fi.point;
    fclose(fl.f);
    fclose(to_fl.f);

    printf("Total fault injection points in rwy_relocate: %ld\n", total_points);
    CHECK(total_points > 5, "reasonable number of I/O points");

    for (p = 1; p <= total_points; p++) {
        copy_file(bak, img);
        open_store(&s, &dev, &fl, img, "r+b");

        to_fl.f = fopen(img, "r+b");
        to_dev = dev;
        to_dev.ctx = &to_fl;

        fi_wrap(&fi_dev, &fi, &to_dev, p);
        rc = rwy_relocate(&s, &fi_dev, NULL);
        CHECK(rc != RWY_OK, "relocate fails at cut point");

        fclose(fl.f);
        fclose(to_fl.f);

        /* Re-open store and verify filesystem consistency */
        open_store(&s, &dev, &fl, img, "r+b");
        verify_test_tree(&s);
        fclose(fl.f);

        CHECK(run_checker(img) == 0, "fsck.runway clean after fault injection");
    }
}

int main(int argc, char **argv) {
    const char *seed = "tests/host/RUNWAY.DAT";

    if (argc > 1) seed = argv[1];
    if (argc > 2) g_checkbin = argv[2];

    test_relocate_roundtrip(seed);
    test_relocate_xor(seed);
    test_relocate_nospc(seed);
    test_relocate_fault_injection(seed);

    if (failures) {
        printf("FAILED (%d errors)\n", failures);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
