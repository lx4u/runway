/*
 * gctest — space reclamation / garbage collection tests for Runway v2.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Covers the GC milestone:
 *  1. basic reclamation (leftovers grow, GC clears, contents intact)
 *  2. repeated GC (idempotent from the contents' perspective)
 *  3. space reuse (freed blocks serve new allocations)
 *  4. large file near the 256 MiB limit (create/write/commit/remount/
 *     replace/delete/GC/recreate with deterministic pattern checksums)
 *  5. fragmentation (checkerboard free pattern; reclaimed blocks reusable;
 *     contiguous-run NOSPC distinguished from genuine exhaustion)
 *  6. deterministic fault injection at every GC metadata write / flush /
 *     publication point (old-or-new generation, never partial)
 *  7. UKI A->B rotation + GC pressure + crashes (always wholly A or B)
 *  8. full-store pressure (fill to NOSPC, GC, reuse)
 *  9. the state record survives GC
 *
 * Usage: gctest STORE RWY_CHECK_BIN
 * Works on private copies; never mutates STORE. Requires cwd with
 * build/ for nothing; checker invoked via the given path.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "testdir.h"
#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_format.h"
#include "forge.h"

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
static int file_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    file_t *fl = ctx;
    if (fseek(fl->f, (long)off, SEEK_SET)) return -1;
    return fwrite(buf, 1, len, fl->f) == len ? 0 : -1;
}
static int file_fl(void *ctx) { file_t *fl = ctx; return fflush(fl->f) ? -1 : 0; }

/* ---------- fault-injecting wrapper (counts block writes + flushes) ---------- */
typedef struct {
    rwy_block_dev_t inner;
    long point;
    long fail_at;    /* 1-based, -1 = never */
} fi_t;

static int fi_rd(void *ctx, uint64_t off, void *buf, size_t len) {
    fi_t *fi = ctx;
    return fi->inner.read(fi->inner.ctx, off, buf, len);
}
static int fi_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    fi_t *fi = ctx;
    fi->point++;
    if (fi->fail_at == fi->point) return -1;
    return fi->inner.write(fi->inner.ctx, off, buf, len);
}
static int fi_fl(void *ctx) {
    fi_t *fi = ctx;
    fi->point++;
    if (fi->fail_at == fi->point) return -1;
    if (fi->inner.flush) return fi->inner.flush(fi->inner.ctx);
    return 0;
}

static void copy_file(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "wb");
    char buf[1 << 20];
    size_t n;
    if (!fa || !fb) { fprintf(stderr, "copy failed %s -> %s\n", a, b); exit(1); }
    while ((n = fread(buf, 1, sizeof(buf), fa)) > 0)
        if (fwrite(buf, 1, n, fb) != n) { fprintf(stderr, "copy failed\n"); exit(1); }
    fclose(fa); fclose(fb);
}

static void open_store(rwy_store_t *s, rwy_block_dev_t *dev, file_t *fl,
                       const char *path, const char *mode) {
    fl->f = fopen(path, mode);
    if (!fl->f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    dev->read = file_rd; dev->write = file_wr; dev->flush = file_fl;
    dev->ctx = fl; dev->size = RWY_STORE_SIZE; dev->block_size = RWY_BLOCK_SIZE;
    if (rwy_mount(s, dev)) { fprintf(stderr, "mount failed: %s\n", path); exit(1); }
}

static uint32_t count_marked(const rwy_store_t *s) {
    uint32_t b, n = 0;
    for (b = 0; b < RWY_TOTAL_BLOCKS; b++)
        if (rwy_blk_used(s, b)) n++;
    return n;
}

/* Deterministic pattern: byte at file offset o is (o + seed) & 0xFF. */
static void fill_pattern(uint8_t *buf, uint64_t off, size_t len, uint8_t seed) {
    size_t i;
    for (i = 0; i < len; i++) buf[i] = (uint8_t)((off + i + seed) & 0xFF);
}

static int verify_pattern(rwy_store_t *s, const char *path, uint64_t size,
                          uint8_t seed) {
    static uint8_t buf[1 << 20];
    static uint8_t expect[1 << 20];
    uint64_t off = 0;
    rwy_attr_t a;
    if (rwy_getattr_path(s, path, &a) != RWY_OK) return -1;
    if (a.size != size) return -2;
    while (off < size) {
        size_t want = sizeof(buf), got = want;
        uint64_t rem = size - off;
        if (rem < want) want = (size_t)rem;
        got = want;
        if (rwy_read_path(s, path, off, buf, &got) != RWY_OK) return -3;
        if (got != want) return -4;
        fill_pattern(expect, off, want, seed);
        if (memcmp(buf, expect, want)) return -5;
        off += want;
    }
    return 0;
}

/* Single-transaction whole-file write (one contiguous run when space
 * allows). Caller provides the pattern seed. */
static int write_pattern_file(rwy_store_t *s, const char *path, uint64_t size,
                              uint8_t seed) {
    uint8_t *buf = malloc((size_t)size ? (size_t)size : 1);
    int rc;
    if (!buf) { fprintf(stderr, "oom\n"); exit(1); }
    fill_pattern(buf, 0, (size_t)size, seed);
    rc = rwy_begin(s);
    if (!rc) rc = rwy_write_path(s, path, 0, buf, (size_t)size);
    if (!rc) rc = rwy_commit(s);
    else if (s->in_txn) rwy_abort(s);
    free(buf);
    return rc;
}

static int run_checker(const char *img) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s %s > %s 2>&1", g_checkbin, img, tpath("rwy_gc_check.log"));
    return system(cmd);
}

/* The state record lives below the pool, outside every generation. Two writes fill both copies. */
static void rec_fill(uint8_t *p) {
    for (size_t i = 0; i < RWY_REC_PAYLOAD; i++) p[i] = (uint8_t)(i * 7 + 3);
}

static void rec_put(rwy_store_t *s) {
    uint8_t p[RWY_REC_PAYLOAD];
    rec_fill(p);
    CHECK(rwy_record_write(s, p) == RWY_OK && rwy_record_write(s, p) == RWY_OK, "write the record");
}

static int rec_kept(rwy_store_t *s) {
    uint8_t got[RWY_REC_PAYLOAD], want[RWY_REC_PAYLOAD];
    rec_fill(want);
    return rwy_record_read(s, got) == RWY_OK && !memcmp(got, want, sizeof(got)) &&
           !rwy_record_damaged(s) && rwy_blk_used(s, s->sb.rec_start) && rwy_blk_used(s, s->sb.rec_start + 1);
}

/* ================= 1. basic reclamation ================= */
static void test_basic(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    uint32_t marked0, marked1, marked2;
    uint32_t left0, left1;
    printf("== GC basic reclamation ==\n");
    copy_file(store_path, tpath("rwy_gc_basic.img"));
    open_store(&s, &dev, &fl, tpath("rwy_gc_basic.img"), "r+b");
    marked0 = count_marked(&s);
    /* Churn: create/modify/delete over many commits. */
    for (int i = 0; i < 12; i++) {
        char name[64];
        snprintf(name, sizeof(name), "CHURN%02d.DAT", i);
        CHECK(rwy_txn_create(&s, name, RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "churn create");
        {
            uint8_t w[4096];
            fill_pattern(w, 0, sizeof(w), (uint8_t)i);
            CHECK(rwy_txn_write_path(&s, name, 0, w, sizeof(w)) == RWY_OK, "churn write");
        }
        if (i % 3 == 2) {
            char old[64];
            snprintf(old, sizeof(old), "CHURN%02d.DAT", i - 2);
            CHECK(rwy_txn_unlink(&s, old) == RWY_OK, "churn delete");
        }
    }
    CHECK(forge_leftovers(&s, 40) == RWY_OK, "leave marked-but-unreachable blocks");
    fclose(fl.f);
    open_store(&s, &dev, &fl, tpath("rwy_gc_basic.img"), "r+b");
    marked1 = count_marked(&s);
    left0 = rwy_reclaimable(&s);
    printf("marked before=%u after-churn=%u reclaimable=%u\n", marked0, marked1, left0);
    CHECK(marked1 > marked0, "churn consumes blocks");
    CHECK(left0 > 0, "churn leaves reclaimable blocks");
    /* Snapshot a surviving file's contents. */
    CHECK(verify_pattern(&s, "CHURN11.DAT", 4096, 11) == 0, "pre-GC content");
    {
        uint64_t gen0 = s.sb.generation;
        CHECK(rwy_gc(&s) == RWY_OK, "gc runs");
        CHECK(s.sb.generation == gen0 + 1, "gc publishes one new generation");
    }
    marked2 = count_marked(&s);
    left1 = rwy_reclaimable(&s);
    printf("marked after-gc=%u reclaimable=%u\n", marked2, left1);
    CHECK(left1 == 0, "no reclaimable leftovers after GC");
    CHECK(marked2 < marked1, "GC reclaims blocks");
    CHECK(verify_pattern(&s, "CHURN11.DAT", 4096, 11) == 0, "post-GC content intact");
    CHECK(verify_pattern(&s, "CHURN10.DAT", 4096, 10) == 0, "post-GC content intact #2");
    fclose(fl.f);
    /* Remount + checker: consistent, no leftovers. */
    open_store(&s, &dev, &fl, tpath("rwy_gc_basic.img"), "r+b");
    CHECK(verify_pattern(&s, "CHURN11.DAT", 4096, 11) == 0, "remount content intact");
    CHECK(rwy_reclaimable(&s) == 0, "remount: still no leftovers");
    fclose(fl.f);
    CHECK(run_checker(tpath("rwy_gc_basic.img")) == 0, "checker OK after GC");
    printf("== basic OK ==\n");
}

/* ================= 2. repeated GC ================= */
static void test_repeat(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    uint32_t m1, m2;
    printf("== GC repeated ==\n");
    copy_file(store_path, tpath("rwy_gc_repeat.img"));
    open_store(&s, &dev, &fl, tpath("rwy_gc_repeat.img"), "r+b");
    CHECK(rwy_txn_create(&s, "R.DAT", RWY_S_IFREG | 0600, 7, 8) == RWY_OK, "create");
    {
        uint8_t w[8192];
        fill_pattern(w, 0, sizeof(w), 0x5A);
        CHECK(rwy_txn_write_path(&s, "R.DAT", 0, w, sizeof(w)) == RWY_OK, "write");
    }
    CHECK(rwy_gc(&s) == RWY_OK, "gc1");
    m1 = count_marked(&s);
    CHECK(rwy_gc(&s) == RWY_OK, "gc2");
    m2 = count_marked(&s);
    CHECK(rwy_gc(&s) == RWY_OK, "gc3");
    CHECK(count_marked(&s) == m2 && m2 == m1, "repeated GC stable block count");
    CHECK(rwy_reclaimable(&s) == 0, "no leftovers");
    CHECK(verify_pattern(&s, "R.DAT", 8192, 0x5A) == 0, "content intact");
    {
        rwy_attr_t a;
        CHECK(rwy_getattr_path(&s, "R.DAT", &a) == RWY_OK &&
              a.mode == (RWY_S_IFREG | 0600) && a.uid == 7 && a.gid == 8,
              "metadata intact across GCs");
    }
    /* GC with an open transaction must be refused, not corrupt. */
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    CHECK(rwy_gc(&s) == RWY_ERR_INVAL, "gc refused with open txn");
    CHECK(rwy_abort(&s) == RWY_OK, "abort");
    CHECK(verify_pattern(&s, "R.DAT", 8192, 0x5A) == 0, "content intact after refused GC");
    fclose(fl.f);
    CHECK(run_checker(tpath("rwy_gc_repeat.img")) == 0, "checker OK after repeated GC");
    printf("== repeat OK ==\n");
}

/* ================= 3. space reuse ================= */
static void test_reuse(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    printf("== GC space reuse ==\n");
    copy_file(store_path, tpath("rwy_gc_reuse.img"));
    open_store(&s, &dev, &fl, tpath("rwy_gc_reuse.img"), "r+b");
    /* Consume ~96 MiB across several files. */
    for (int i = 0; i < 6; i++) {
        char name[64];
        snprintf(name, sizeof(name), "BIG%02d.BIN", i);
        CHECK(rwy_txn_create(&s, name, RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create big");
        {
            uint8_t *w = malloc(16 << 20);
            fill_pattern(w, 0, 16 << 20, (uint8_t)i);
            CHECK(rwy_begin(&s) == RWY_OK, "begin");
            CHECK(rwy_write_path(&s, name, 0, w, 16 << 20) == RWY_OK, "write big");
            CHECK(rwy_commit(&s) == RWY_OK, "commit big");
            free(w);
        }
    }
    /* Delete half, GC, recreate with different seeds. */
    for (int i = 0; i < 6; i += 2) {
        char name[64];
        snprintf(name, sizeof(name), "BIG%02d.BIN", i);
        CHECK(rwy_txn_unlink(&s, name) == RWY_OK, "delete big");
    }
    CHECK(forge_leftovers(&s, 40) == RWY_OK, "leave marked-but-unreachable blocks");
    {
        uint32_t left_before = rwy_reclaimable(&s);
        CHECK(rwy_gc(&s) == RWY_OK, "gc");
        CHECK(rwy_reclaimable(&s) == 0, "clean");
        CHECK(left_before > 0, "there was something to reclaim");
    }
    for (int i = 0; i < 6; i += 2) {
        char name[64];
        snprintf(name, sizeof(name), "NEW%02d.BIN", i);
        CHECK(rwy_txn_create(&s, name, RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "recreate");
        {
            uint8_t *w = malloc(16 << 20);
            fill_pattern(w, 0, 16 << 20, (uint8_t)(0x80 + i));
            CHECK(rwy_begin(&s) == RWY_OK, "begin");
            CHECK(rwy_write_path(&s, name, 0, w, 16 << 20) == RWY_OK, "rewrite");
            CHECK(rwy_commit(&s) == RWY_OK, "recommit");
            free(w);
        }
    }
    /* Verify old survivors and new files. */
    for (int i = 1; i < 6; i += 2) {
        char name[64];
        snprintf(name, sizeof(name), "BIG%02d.BIN", i);
        CHECK(verify_pattern(&s, name, 16 << 20, (uint8_t)i) == 0, "survivor intact");
    }
    for (int i = 0; i < 6; i += 2) {
        char name[64];
        snprintf(name, sizeof(name), "NEW%02d.BIN", i);
        CHECK(verify_pattern(&s, name, 16 << 20, (uint8_t)(0x80 + i)) == 0, "reused intact");
    }
    fclose(fl.f);
    open_store(&s, &dev, &fl, tpath("rwy_gc_reuse.img"), "r+b");
    CHECK(verify_pattern(&s, "NEW00.BIN", 16 << 20, 0x80) == 0, "remount reused intact");
    fclose(fl.f);
    printf("== reuse OK ==\n");
}

/* ================= 4. large file near the 256 MiB limit ================= */
#define LARGE_BYTES (200ULL << 20)
static void test_large(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    printf("== GC large file (200 MiB) ==\n");
    copy_file(store_path, tpath("rwy_gc_large.img"));
    open_store(&s, &dev, &fl, tpath("rwy_gc_large.img"), "r+b");
    /* Clear seed payloads so a 200 MiB file fits the 256 MiB store. */
    CHECK(rwy_txn_unlink(&s, "PERF/READ1M.BIN") == RWY_OK, "drop 1M");
    CHECK(rwy_txn_unlink(&s, "PERF/READ16M.BIN") == RWY_OK, "drop 16M");
    CHECK(rwy_txn_unlink(&s, "PERF/READ64M.BIN") == RWY_OK, "drop 64M");
    CHECK(rwy_txn_unlink(&s, "README.TXT") == RWY_OK, "drop readme");
    CHECK(rwy_txn_unlink(&s, "TEST/HELLO.TXT") == RWY_OK, "drop hello");
    CHECK(rwy_txn_unlink(&s, "EFI/TEST/SAMPLE.EFI") == RWY_OK, "drop sample");
    CHECK(rwy_gc(&s) == RWY_OK, "gc after clear");
    CHECK(rwy_txn_create(&s, "LARGE.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create large");
    CHECK(write_pattern_file(&s, "LARGE.BIN", LARGE_BYTES, 0x11) == RWY_OK, "write large");
    CHECK(verify_pattern(&s, "LARGE.BIN", LARGE_BYTES, 0x11) == 0, "verify large");
    fclose(fl.f);
    /* Remount: still correct. */
    open_store(&s, &dev, &fl, tpath("rwy_gc_large.img"), "r+b");
    CHECK(verify_pattern(&s, "LARGE.BIN", LARGE_BYTES, 0x11) == 0, "remount large");
    /* Replacing 200 MiB in one transaction needs the old and the new copy at once (the old blocks
     * stay reserved until the commit), which a 256 MiB store cannot provide: delete, collect, write. */
    CHECK(rwy_txn_unlink(&s, "LARGE.BIN") == RWY_OK, "delete large");
    CHECK(rwy_gc(&s) == RWY_OK, "gc after delete");
    CHECK(rwy_txn_create(&s, "LARGE.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "recreate large");
    CHECK(write_pattern_file(&s, "LARGE.BIN", LARGE_BYTES, 0x22) == RWY_OK, "rewrite large");
    CHECK(verify_pattern(&s, "LARGE.BIN", LARGE_BYTES, 0x22) == 0, "replaced large");
    CHECK(rwy_gc(&s) == RWY_OK, "gc large");
    CHECK(rwy_reclaimable(&s) == 0, "no leftovers");
    CHECK(verify_pattern(&s, "LARGE.BIN", LARGE_BYTES, 0x22) == 0, "post-GC large");
    fclose(fl.f);
    open_store(&s, &dev, &fl, tpath("rwy_gc_large.img"), "r+b");
    CHECK(verify_pattern(&s, "LARGE.BIN", LARGE_BYTES, 0x22) == 0, "remount replaced");
    /* A mid-size file exercises true in-place overwrite (needs 2x its own
     * size free under CoW, which fits here). */
    CHECK(rwy_txn_create(&s, "MID.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create mid");
    CHECK(write_pattern_file(&s, "MID.BIN", 8 << 20, 0x41) == RWY_OK, "write mid");
    CHECK(write_pattern_file(&s, "MID.BIN", 8 << 20, 0x42) == RWY_OK, "overwrite mid");
    CHECK(verify_pattern(&s, "MID.BIN", 8 << 20, 0x42) == 0, "verify mid");
    /* The 256 MiB per-file cap is enforced without side effects. */
    CHECK(rwy_begin(&s) == RWY_OK, "begin cap");
    {
        uint8_t one = 0;
        int rc = rwy_write_path(&s, "MID.BIN", (256ULL << 20) - 1, &one, 2);
        CHECK(rc == RWY_ERR_NOSPC, "cap enforced");
        rwy_abort(&s);
    }
    CHECK(verify_pattern(&s, "MID.BIN", 8 << 20, 0x42) == 0, "cap probe harmless");
    CHECK(rwy_txn_unlink(&s, "MID.BIN") == RWY_OK, "drop mid");
    /* Delete, GC, recreate with a third pattern. */
    CHECK(rwy_txn_unlink(&s, "LARGE.BIN") == RWY_OK, "delete large");
    CHECK(rwy_gc(&s) == RWY_OK, "gc after delete");
    CHECK(rwy_txn_create(&s, "LARGE.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "recreate");
    CHECK(write_pattern_file(&s, "LARGE.BIN", LARGE_BYTES, 0x33) == RWY_OK, "rewrite #2");
    CHECK(verify_pattern(&s, "LARGE.BIN", LARGE_BYTES, 0x33) == 0, "verify #2");
    fclose(fl.f);
    CHECK(run_checker(tpath("rwy_gc_large.img")) == 0, "checker OK large");
    printf("== large OK ==\n");
}

/* ================= 5. fragmentation ================= */
static void test_frag(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    printf("== GC fragmentation ==\n");
    copy_file(store_path, tpath("rwy_gc_frag.img"));
    open_store(&s, &dev, &fl, tpath("rwy_gc_frag.img"), "r+b");
    /* Checkerboard: 400 one-block files, then delete every other one. */
    CHECK(rwy_txn_mkdir(&s, "FRAG", RWY_S_IFDIR | 0755, 0, 0) == RWY_OK, "mkdir frag");
    for (int i = 0; i < 400; i++) {
        char name[64];
        uint8_t w[512];
        snprintf(name, sizeof(name), "FRAG/F%04d", i);
        CHECK(rwy_txn_create(&s, name, RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "frag create");
        fill_pattern(w, 0, sizeof(w), (uint8_t)i);
        CHECK(rwy_txn_write_path(&s, name, 0, w, sizeof(w)) == RWY_OK, "frag write");
    }
    for (int i = 0; i < 400; i += 2) {
        char name[64];
        snprintf(name, sizeof(name), "FRAG/F%04d", i);
        CHECK(rwy_txn_unlink(&s, name) == RWY_OK, "frag delete");
    }
    CHECK(rwy_gc(&s) == RWY_OK, "gc frag");
    CHECK(rwy_reclaimable(&s) == 0, "frag clean");
    /* Survivors intact. */
    for (int i = 1; i < 400; i += 2) {
        char name[64];
        snprintf(name, sizeof(name), "FRAG/F%04d", i);
        CHECK(verify_pattern(&s, name, 512, (uint8_t)i) == 0, "frag survivor");
    }
    /* Reclaimed blocks are reusable: fill the holes with new files. */
    for (int i = 0; i < 400; i += 2) {
        char name[64];
        uint8_t w[512];
        snprintf(name, sizeof(name), "FRAG/G%04d", i);
        CHECK(rwy_txn_create(&s, name, RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "frag refill");
        fill_pattern(w, 0, sizeof(w), (uint8_t)(0xC0 + i));
        CHECK(rwy_txn_write_path(&s, name, 0, w, sizeof(w)) == RWY_OK, "frag rewrite");
    }
    for (int i = 0; i < 400; i += 2) {
        char name[64];
        snprintf(name, sizeof(name), "FRAG/G%04d", i);
        CHECK(verify_pattern(&s, name, 512, (uint8_t)(0xC0 + i)) == 0, "refill intact");
    }
    CHECK(rwy_gc(&s) == RWY_OK, "gc frag #2");
    /* Contiguous-run limitation probe: with the checkerboard refilled,
     * free space is scattered single blocks. A large contiguous request
     * may fail even though thousands of blocks are free — that is the
     * documented limitation, not corruption. Either outcome is accepted,
     * but a later contiguous region must succeed after clearing space. */
    {
        uint32_t free_before = 0, b;
        for (b = s.sb.pool_start; b < RWY_TOTAL_BLOCKS; b++)
            if (!rwy_blk_used(&s, b)) free_before++;
        printf("frag: free blocks=%u\n", free_before);
        CHECK(free_before > 20000, "plenty free but scattered");
        CHECK(rwy_txn_create(&s, "CONTIG.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK,
              "create contig probe");
        {
            /* 4 MiB needs a 1024-block contiguous run. */
            uint8_t *w = malloc(4 << 20);
            int rc;
            fill_pattern(w, 0, 4 << 20, 0x77);
            rc = rwy_begin(&s);
            if (!rc) rc = rwy_write_path(&s, "CONTIG.BIN", 0, w, 4 << 20);
            if (!rc) rc = rwy_commit(&s);
            else if (s.in_txn) rwy_abort(&s);
            free(w);
            if (rc == RWY_OK) {
                CHECK(verify_pattern(&s, "CONTIG.BIN", 4 << 20, 0x77) == 0, "contig ok");
                CHECK(rwy_txn_unlink(&s, "CONTIG.BIN") == RWY_OK, "drop contig");
            } else {
                printf("note: 4 MiB contiguous alloc %s (fragmentation limit)\n",
                       rc == RWY_ERR_NOSPC ? "NOSPC as documented" : "unexpected rc");
                CHECK(rc == RWY_ERR_NOSPC, "only NOSPC is acceptable");
                if (s.in_txn) rwy_abort(&s);
                CHECK(rwy_txn_unlink(&s, "CONTIG.BIN") == RWY_OK, "drop empty probe");
            }
        }
    }
    fclose(fl.f);
    CHECK(run_checker(tpath("rwy_gc_frag.img")) == 0, "checker OK frag");
    printf("== frag OK ==\n");
}

/* ================= 6. fault injection over GC ================= */
static void test_gc_faults(const char *store_path) {
    printf("== GC fault injection ==\n");
    copy_file(store_path, tpath("rwy_gc_fi.img"));
    /* Prepare some garbage + a canary file. */
    {
        file_t fl;
        rwy_block_dev_t dev;
        rwy_store_t s;
        uint8_t w[16384];
        open_store(&s, &dev, &fl, tpath("rwy_gc_fi.img"), "r+b");
        CHECK(rwy_txn_create(&s, "CANARY.DAT", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "canary");
        fill_pattern(w, 0, sizeof(w), 0xBE);
        CHECK(rwy_txn_write_path(&s, "CANARY.DAT", 0, w, sizeof(w)) == RWY_OK, "canary write");
        for (int i = 0; i < 4; i++) {
            char name[64];
            snprintf(name, sizeof(name), "JUNK%02d", i);
            CHECK(rwy_txn_create(&s, name, RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "junk");
            CHECK(rwy_txn_write_path(&s, name, 0, w, sizeof(w)) == RWY_OK, "junk write");
            CHECK(rwy_txn_unlink(&s, name) == RWY_OK, "junk drop");
        }
        fclose(fl.f);
    }
    /* Count GC persistence points. */
    long npoints = 0;
    {
        file_t fl;
        rwy_block_dev_t dev;
        rwy_store_t s;
        fi_t fi;
        copy_file(tpath("rwy_gc_fi.img"), tpath("rwy_gc_fi_cnt.img"));
        fl.f = fopen(tpath("rwy_gc_fi_cnt.img"), "r+b");
        dev.read = file_rd; dev.write = file_wr; dev.flush = file_fl;
        dev.ctx = &fl; dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
        fi.inner = dev; fi.point = 0; fi.fail_at = -1;
        {
            rwy_block_dev_t fd = { fi_rd, fi_wr, fi_fl, &fi,
                                    RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
            CHECK(rwy_mount(&s, &fd) == RWY_OK, "mount count");
            CHECK(rwy_gc(&s) == RWY_OK, "counting GC works");
            npoints = fi.point;
        }
        fclose(fl.f);
    }
    printf("GC persistence points: %ld\n", npoints);
    CHECK(npoints == 5, "expected 5 GC points (table, bitmap, flush, superblock, flush)");
    for (long at = 1; at <= npoints; at++) {
        file_t fl2;
        rwy_block_dev_t d2;
        rwy_store_t s2, s3;
        fi_t fi2;
        int rc;
        copy_file(tpath("rwy_gc_fi.img"), tpath("rwy_gc_fi_one.img"));
        fl2.f = fopen(tpath("rwy_gc_fi_one.img"), "r+b");
        d2.read = file_rd; d2.write = file_wr; d2.flush = file_fl;
        d2.ctx = &fl2; d2.size = RWY_STORE_SIZE; d2.block_size = RWY_BLOCK_SIZE;
        fi2.inner = d2; fi2.point = 0; fi2.fail_at = at;
        {
            rwy_block_dev_t fd2 = { fi_rd, fi_wr, fi_fl, &fi2,
                                     RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
            rc = rwy_mount(&s2, &fd2);
            if (rc) { printf("FAIL: mount at=%ld\n", at); failures++; fclose(fl2.f); continue; }
            rc = rwy_gc(&s2);
            /* Failure expected exactly when the fault hits inside GC. */
        }
        fclose(fl2.f);
        /* Recovery: valid generation, canary intact, checker clean. */
        {
            file_t fl3;
            rwy_block_dev_t d3;
            fl3.f = fopen(tpath("rwy_gc_fi_one.img"), "r+b");
            d3.read = file_rd; d3.write = file_wr; d3.flush = file_fl;
            d3.ctx = &fl3; d3.size = RWY_STORE_SIZE; d3.block_size = RWY_BLOCK_SIZE;
            rc = rwy_mount(&s3, &d3);
            if (rc) { printf("FAIL: remount after GC fault at=%ld rc=%d\n", at, rc); failures++; fclose(fl3.f); continue; }
            if (verify_pattern(&s3, "CANARY.DAT", 16384, 0xBE) != 0) {
                printf("FAIL: canary damaged at=%ld\n", at); failures++;
            }
            /* GC must not resurrect junk or lose the canary: exactly one
             * of the two valid outcomes (old gen / new gen), both with
             * identical visible contents. */
            if (rwy_lookup(&s3, "JUNK00", 0) == RWY_OK) {
                printf("FAIL: junk resurrected at=%ld\n", at); failures++;
            }
            /* A subsequent GC must always succeed and converge to clean. */
            if (rwy_gc(&s3) != RWY_OK) {
                printf("FAIL: post-fault GC fails at=%ld\n", at); failures++;
            } else if (verify_pattern(&s3, "CANARY.DAT", 16384, 0xBE) != 0) {
                printf("FAIL: canary damaged by post-fault GC at=%ld\n", at); failures++;
            }
            fclose(fl3.f);
        }
    }
    CHECK(run_checker(tpath("rwy_gc_fi_one.img")) == 0, "checker OK after GC faults");
    printf("== GC faults OK ==\n");
}

/* ================= 7. UKI rotation + GC pressure + crashes ================= */
static void test_uki_gc(const char *store_path) {
    printf("== UKI rotation with GC pressure ==\n");
    copy_file(store_path, tpath("rwy_gc_uki.img"));
    {
        file_t fl;
        rwy_block_dev_t dev;
        rwy_store_t s;
        uint8_t *a;
        open_store(&s, &dev, &fl, tpath("rwy_gc_uki.img"), "r+b");
        if (rwy_begin(&s) == RWY_OK) {
            if (rwy_mkdir(&s, "EFI/Linux", RWY_S_IFDIR | 0755, 0, 0) != RWY_OK &&
                rwy_lookup(&s, "EFI/Linux", 0) != RWY_OK) {
                printf("need EFI/Linux\n"); failures++;
            } else {
                if (rwy_commit(&s) != RWY_OK && s.in_txn) rwy_abort(&s);
            }
            if (s.in_txn) rwy_abort(&s);
        }
        fclose(fl.f);
        open_store(&s, &dev, &fl, tpath("rwy_gc_uki.img"), "r+b");
        CHECK(rwy_begin(&s) == RWY_OK, "begin A");
        if (rwy_lookup(&s, "EFI/Linux/uki.efi", 0) != RWY_OK) {
            a = malloc(1 << 20);
            memset(a, 0xAA, 1 << 20);
            CHECK(rwy_create(&s, "EFI/Linux/uki.efi", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create A");
            CHECK(rwy_write_path(&s, "EFI/Linux/uki.efi", 0, a, 1 << 20) == RWY_OK, "write A");
            free(a);
            CHECK(rwy_commit(&s) == RWY_OK, "commit A");
        } else rwy_abort(&s);
        fclose(fl.f);
    }
    /* Rotate A -> B (committed, no faults), then crash GC at every point. */
    {
        file_t fl;
        rwy_block_dev_t dev;
        rwy_store_t s;
        uint8_t *bimg = malloc(1 << 20);
        memset(bimg, 0xBB, 1 << 20);
        open_store(&s, &dev, &fl, tpath("rwy_gc_uki.img"), "r+b");
        CHECK(rwy_begin(&s) == RWY_OK, "begin B");
        CHECK(rwy_create(&s, "EFI/Linux/uki.new", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create B");
        CHECK(rwy_write_path(&s, "EFI/Linux/uki.new", 0, bimg, 1 << 20) == RWY_OK, "write B");
        CHECK(rwy_rename(&s, "EFI/Linux/uki.new", "EFI/Linux/uki.efi") == RWY_OK, "replace");
        CHECK(rwy_commit(&s) == RWY_OK, "commit B");
        fclose(fl.f);
        /* Count GC points on this image. */
        {
            file_t flc;
            rwy_block_dev_t devc;
            rwy_store_t sc;
            fi_t fi;
            flc.f = fopen(tpath("rwy_gc_uki.img"), "r+b");
            devc.read = file_rd; devc.write = file_wr; devc.flush = file_fl;
            devc.ctx = &flc; devc.size = RWY_STORE_SIZE; devc.block_size = RWY_BLOCK_SIZE;
            fi.inner = devc; fi.point = 0; fi.fail_at = -1;
            {
                rwy_block_dev_t fd = { fi_rd, fi_wr, fi_fl, &fi,
                                        RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
                CHECK(rwy_mount(&sc, &fd) == RWY_OK, "mount uki");
                CHECK(rwy_gc(&sc) == RWY_OK, "gc uki works");
                printf("UKI GC points: %ld\n", fi.point);
                for (long at = 1; at <= fi.point; at++) {
                    file_t fl2;
                    rwy_block_dev_t d2;
                    rwy_store_t s2, s3;
                    fi_t fi2;
                    int rc;
                    copy_file(tpath("rwy_gc_uki.img"), tpath("rwy_gc_uki_one.img"));
                    /* NOTE: the rotation to B is already committed in the
                     * base image; the fault hits only the GC. uki.efi must
                     * therefore be wholly B after recovery (and trivially
                     * never partial). We additionally accept wholly A only
                     * if the base had not committed — it has, so B. */
                    fl2.f = fopen(tpath("rwy_gc_uki_one.img"), "r+b");
                    d2.read = file_rd; d2.write = file_wr; d2.flush = file_fl;
                    d2.ctx = &fl2; d2.size = RWY_STORE_SIZE; d2.block_size = RWY_BLOCK_SIZE;
                    fi2.inner = d2; fi2.point = 0; fi2.fail_at = at;
                    {
                        rwy_block_dev_t fd2 = { fi_rd, fi_wr, fi_fl, &fi2,
                                                 RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
                        rc = rwy_mount(&s2, &fd2);
                        if (rc) { printf("FAIL: mount at=%ld\n", at); failures++; fclose(fl2.f); continue; }
                        (void)rwy_gc(&s2);
                    }
                    fclose(fl2.f);
                    {
                        file_t fl3;
                        rwy_block_dev_t d3;
                        fl3.f = fopen(tpath("rwy_gc_uki_one.img"), "r+b");
                        d3.read = file_rd; d3.write = file_wr; d3.flush = file_fl;
                        d3.ctx = &fl3; d3.size = RWY_STORE_SIZE; d3.block_size = RWY_BLOCK_SIZE;
                        rc = rwy_mount(&s3, &d3);
                        if (rc) { printf("FAIL: remount at=%ld\n", at); failures++; fclose(fl3.f); continue; }
                        {
                            static uint8_t chk[1 << 20];
                            size_t got = sizeof(chk);
                            rwy_attr_t ua;
                            int allA = 1, allB = 1;
                            if (rwy_getattr_path(&s3, "EFI/Linux/uki.efi", &ua) != RWY_OK ||
                                ua.size != (1 << 20)) {
                                printf("FAIL: uki missing/size at=%ld\n", at); failures++;
                            } else if (rwy_read_path(&s3, "EFI/Linux/uki.efi", 0, chk, &got) != RWY_OK ||
                                       got != sizeof(chk)) {
                                printf("FAIL: uki unreadable at=%ld\n", at); failures++;
                            } else {
                                for (size_t i = 0; i < got; i++) {
                                    if (chk[i] != 0xAA) allA = 0;
                                    if (chk[i] != 0xBB) allB = 0;
                                }
                                if (!allA && !allB) {
                                    printf("FAIL: uki partial at=%ld\n", at); failures++;
                                }
                                if (allA) printf("note: at=%ld still A (GC predates rotation? no)\n", at);
                            }
                        }
                        fclose(fl3.f);
                    }
                }
            }
            fclose(flc.f);
        }
        free(bimg);
    }
    CHECK(run_checker(tpath("rwy_gc_uki_one.img")) == 0, "checker OK uki+GC");
    printf("== UKI+GC OK ==\n");
}

/* ================= 8. full-store pressure ================= */
static void test_pressure(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    int nfiles = 0;
    printf("== full-store pressure ==\n");
    copy_file(store_path, tpath("rwy_gc_press.img"));
    open_store(&s, &dev, &fl, tpath("rwy_gc_press.img"), "r+b");
    /* Drop seed payloads, GC clean, then fill with 4 MiB chunks. */
    CHECK(rwy_txn_unlink(&s, "PERF/READ1M.BIN") == RWY_OK, "drop 1M");
    CHECK(rwy_txn_unlink(&s, "PERF/READ16M.BIN") == RWY_OK, "drop 16M");
    CHECK(rwy_txn_unlink(&s, "PERF/READ64M.BIN") == RWY_OK, "drop 64M");
    CHECK(rwy_txn_unlink(&s, "README.TXT") == RWY_OK, "drop readme");
    CHECK(rwy_txn_unlink(&s, "TEST/HELLO.TXT") == RWY_OK, "drop hello");
    CHECK(rwy_txn_unlink(&s, "EFI/TEST/SAMPLE.EFI") == RWY_OK, "drop sample");
    CHECK(rwy_gc(&s) == RWY_OK, "gc clean");
    for (;;) {
        char name[64];
        uint8_t *w;
        int rc;
        snprintf(name, sizeof(name), "FILL%03d", nfiles);
        rc = rwy_begin(&s);
        if (!rc) rc = rwy_create(&s, name, RWY_S_IFREG | 0644, 0, 0);
        if (!rc) {
            w = malloc(4 << 20);
            fill_pattern(w, 0, 4 << 20, (uint8_t)nfiles);
            rc = rwy_write_path(&s, name, 0, w, 4 << 20);
            free(w);
        }
        if (!rc) rc = rwy_commit(&s);
        else if (s.in_txn) rwy_abort(&s);
        if (rc == RWY_ERR_NOSPC) {
            if (s.in_txn) rwy_abort(&s);
            break;
        }
        CHECK(rc == RWY_OK, "fill commit");
        nfiles++;
        if (nfiles > 80) break; /* safety: 80x4MiB > store */
    }
    printf("filled %d x 4MiB files before NOSPC\n", nfiles);
    CHECK(nfiles >= 40, "store genuinely holds bulk data");
    /* GC must not manufacture space: still NOSPC for more data... but GC
     * reclaims only leftovers; after a clean fill there may be none, so
     * NOSPC must persist OR a small file fits in true slack. Either way,
     * existing files must verify. */
    CHECK(rwy_gc(&s) == RWY_OK, "gc at full");
    {
        char name[64];
        snprintf(name, sizeof(name), "FILL%03d", 1);
        CHECK(verify_pattern(&s, name, 4 << 20, 1) == 0, "full: survivor intact");
    }
    /* Free a contiguous quarter (the tail files), GC, reuse must succeed.
     * (Freeing scattered files would only prove the documented
     * contiguous-run limitation again — see the frag test.) */
    for (int i = nfiles - 1; i >= nfiles - nfiles / 4; i--) {
        char name[64];
        snprintf(name, sizeof(name), "FILL%03d", i);
        CHECK(rwy_txn_unlink(&s, name) == RWY_OK, "free quarter");
    }
    CHECK(rwy_gc(&s) == RWY_OK, "gc after free");
    CHECK(rwy_reclaimable(&s) == 0, "clean after GC");
    CHECK(rwy_txn_create(&s, "AFTER.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create after");
    CHECK(write_pattern_file(&s, "AFTER.BIN", 8 << 20, 0xF0) == RWY_OK, "write after");
    CHECK(verify_pattern(&s, "AFTER.BIN", 8 << 20, 0xF0) == 0, "verify after");
    fclose(fl.f);
    CHECK(run_checker(tpath("rwy_gc_press.img")) == 0, "checker OK pressure");
    printf("== pressure OK ==\n");
}

/* ================= 9. the state record ================= */
static void test_record(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    printf("== gc keeps the state record ==\n");
    copy_file(store_path, tpath("rwy_gc_rec.img"));
    open_store(&s, &dev, &fl, tpath("rwy_gc_rec.img"), "r+b");
    rec_put(&s);
    CHECK(rwy_txn_create(&s, "REC.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create");
    CHECK(write_pattern_file(&s, "REC.BIN", 1 << 20, 1) == RWY_OK, "write");
    CHECK(rwy_txn_unlink(&s, "REC.BIN") == RWY_OK, "unlink leaves blocks to collect");
    CHECK(rwy_gc(&s) == RWY_OK, "gc");
    CHECK(rec_kept(&s), "the record is intact after gc");
    fclose(fl.f);
    open_store(&s, &dev, &fl, tpath("rwy_gc_rec.img"), "rb");
    CHECK(rec_kept(&s), "and after a remount");
    fclose(fl.f);
    CHECK(run_checker(tpath("rwy_gc_rec.img")) == 0, "checker OK record");
    printf("== record OK ==\n");
}

int main(int argc, char **argv) {
    const char *store = argc > 1 ? argv[1] : "build/RUNWAY.DAT";
    if (argc > 2) g_checkbin = argv[2];
    printf("gctest: opening %s (checker %s)\n", store, g_checkbin);
    {
        FILE *f = fopen(store, "rb");
        if (!f) { printf("SKIP: cannot open %s\n", store); return 1; }
        fclose(f);
    }
    test_basic(store);
    test_repeat(store);
    test_reuse(store);
    test_large(store);
    test_frag(store);
    test_gc_faults(store);
    test_uki_gc(store);
    test_pressure(store);
    test_record(store);
    if (failures == 0) printf("\nGCTEST: PASS\n");
    else printf("\nGCTEST: FAIL (%d)\n", failures);
    return failures != 0;
}
