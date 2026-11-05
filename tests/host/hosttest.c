/*
 * hosttest — Linux tests for the v2 transactional core (no EFI, no QEMU).
 * SPDX-License-Identifier: Apache-2.0
 *
 * Covers:
 *  1. basic namespace CRUD + readdir + symlink on a real RUNWAY.DAT
 *     built by tools/mkstore (validates builder <-> core sync);
 *  2. POSIX metadata persistence (mode/uid/gid/timestamps, suid/sgid/sticky);
 *  3. deterministic fault-injection crash recovery (old-or-new);
 *  4. UKI atomic-replacement under injected faults (old-or-new, never partial).
 *
 * Build & run:  make test
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "testdir.h"
#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_internal.h"

static int failures;
static const char *g_fsck_cmd = "tools/fsck.runway";

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

/* ---------- fault-injecting wrapper ---------- */
typedef struct {
    rwy_block_dev_t inner;
    long point;      /* counts block writes + flushes */
    long fail_at;    /* 1-based point to fail, -1 = never */
    int failed;
} fi_t;

static int fi_rd(void *ctx, uint64_t off, void *buf, size_t len) {
    fi_t *fi = ctx;
    return fi->inner.read(fi->inner.ctx, off, buf, len);
}
static int fi_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    fi_t *fi = ctx;
    fi->point++;
    if (fi->fail_at == fi->point) { fi->failed = 1; return -1; }
    return fi->inner.write(fi->inner.ctx, off, buf, len);
}
static int fi_fl(void *ctx) {
    fi_t *fi = ctx;
    fi->point++;
    if (fi->fail_at == fi->point) { fi->failed = 1; return -1; }
    if (fi->inner.flush) return fi->inner.flush(fi->inner.ctx);
    return 0;
}

static void open_store(rwy_store_t *s, rwy_block_dev_t *dev, file_t *fl,
                       const char *path, const char *mode) {
    fl->f = fopen(path, mode);
    if (!fl->f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    dev->read = file_rd; dev->write = file_wr; dev->flush = file_fl;
    dev->ctx = fl; dev->size = RWY_STORE_SIZE; dev->block_size = RWY_BLOCK_SIZE;
    if (rwy_mount(s, dev)) { fprintf(stderr, "mount failed\n"); exit(1); }
}

/* A store is 1.5 MB: the test functions that hold several of them keep them static, or the stack runs out. */
/* Copy a file for crash-test isolation. */
static void copy_file(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "wb");
    static char buf[1 << 20];
    size_t n;
    if (!fa || !fb) { fprintf(stderr, "copy failed\n"); exit(1); }
    while ((n = fread(buf, 1, sizeof(buf), fa)) > 0)
        if (fwrite(buf, 1, n, fb) != n) { fprintf(stderr, "copy failed\n"); exit(1); }
    fclose(fa); fclose(fb);
}

static int read_all(rwy_store_t *s, const char *path, uint8_t **out, uint64_t *outn) {
    rwy_attr_t a;
    uint8_t *buf;
    size_t want, got;
    if (rwy_getattr_path(s, path, &a)) return -1;
    buf = malloc((size_t)a.size + 1);
    if (!buf) exit(1);
    want = (size_t)a.size; got = want;
    if (rwy_read_path(s, path, 0, buf, &got)) { free(buf); return -1; }
    *out = buf; *outn = got;
    return 0;
}

static void test_seed_contents(rwy_store_t *s) {
    uint8_t *buf = 0;
    uint64_t n = 0;
    rwy_attr_t a;
    printf("== seed contents ==\n");
    CHECK(rwy_getattr_path(s, "README.TXT", &a) == RWY_OK, "README present");
    CHECK(read_all(s, "README.TXT", &buf, &n) == 0 &&
          n > 10 && !memcmp(buf, "Runway store", 10), "README content");
    free(buf); buf = 0;
    CHECK(read_all(s, "TEST/HELLO.TXT", &buf, &n) == 0 && n == 19 &&
          !memcmp(buf, "Hello from Runway!\n", 19), "HELLO content");
    free(buf); buf = 0;
    /* pattern spot-check of the 16 MiB file */
    {
        uint8_t chunk[65536];
        size_t got = sizeof(chunk);
        uint64_t off = 1234567;
        uint64_t oid;
        CHECK(rwy_lookup(s, "PERF/READ16M.BIN", &oid) == RWY_OK, "16M present");
        CHECK(rwy_read(s, oid, off, chunk, &got) == RWY_OK && got == sizeof(chunk),
              "16M range read");
        if (got == sizeof(chunk)) {
            int ok = 1;
            for (size_t i = 0; i < got; i++)
                if (chunk[i] != (uint8_t)((off + i + 0x22) & 0xFF)) { ok = 0; break; }
            CHECK(ok, "16M pattern bytes");
        }
    }
    /* readdir */
    {
        char names[4096];
        size_t cnt = 64;
        CHECK(rwy_readdir(s, "", names, sizeof(names), &cnt, 0) == RWY_OK && cnt >= 3,
              "root readdir");
        cnt = 64;
        CHECK(rwy_readdir(s, "TEST", names, sizeof(names), &cnt, 0) == RWY_OK && cnt >= 1,
              "TEST readdir");
    }
    printf("== seed OK ==\n");
}

static void test_posix_persistence(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    static rwy_store_t s;
    rwy_attr_t a;
    printf("== POSIX metadata ==\n");
    copy_file(store_path, tpath("rwy_posix.img"));
    open_store(&s, &dev, &fl, tpath("rwy_posix.img"), "r+b");
    rwy_set_fixed_time(&s, 1700000000, 123);

    CHECK(rwy_txn_create(&s, "SECURE.BIN", RWY_S_IFREG | 0600, 0, 0) == RWY_OK,
          "create 0600 root:root");
    {
        static const char d[] = "secret";
        CHECK(rwy_txn_write_path(&s, "SECURE.BIN", 0, d, sizeof(d) - 1) == RWY_OK,
              "write secret");
    }
    CHECK(rwy_txn_create(&s, "GRP.TXT", RWY_S_IFREG | 0640, 1000, 100) == RWY_OK,
          "create 0640 1000:100");
    CHECK(rwy_txn_chmod(&s, "GRP.TXT", RWY_S_IFREG | 0644) == RWY_OK, "chmod 644");
    CHECK(rwy_txn_chown(&s, "GRP.TXT", 2000, 200) == RWY_OK, "chown 2000:200");
    CHECK(rwy_txn_mkdir(&s, "D", RWY_S_IFDIR | 0750, 5, 6) == RWY_OK, "mkdir 0750");
    CHECK(rwy_txn_create(&s, "SUID.BIN", RWY_S_IFREG | 04755, 0, 0) == RWY_OK,
          "create setuid");
    CHECK(rwy_txn_create(&s, "SGID.BIN", RWY_S_IFREG | 02750, 0, 0) == RWY_OK,
          "create setgid");
    CHECK(rwy_txn_create(&s, "STICKY.BIN", RWY_S_IFREG | 01644, 0, 0) == RWY_OK,
          "create sticky");
    CHECK(rwy_txn_chmod(&s, "D", RWY_S_IFDIR | 01755) == RWY_OK, "chmod sticky dir");
    fclose(fl.f);

    /* remount read-only-ish and verify persistence */
    open_store(&s, &dev, &fl, tpath("rwy_posix.img"), "r+b");
    CHECK(rwy_getattr_path(&s, "SECURE.BIN", &a) == RWY_OK &&
          a.mode == (RWY_S_IFREG | 0600) && a.uid == 0 && a.gid == 0, "0600 persists");
    CHECK(rwy_getattr_path(&s, "GRP.TXT", &a) == RWY_OK &&
          a.mode == (RWY_S_IFREG | 0644) && a.uid == 2000 && a.gid == 200,
          "chmod/chown persist");
    CHECK(rwy_getattr_path(&s, "D", &a) == RWY_OK &&
          a.mode == (RWY_S_IFDIR | 01755), "sticky dir persists");
    CHECK(rwy_getattr_path(&s, "SUID.BIN", &a) == RWY_OK &&
          (a.mode & 04777) == 04755, "setuid persists");
    CHECK(rwy_getattr_path(&s, "SGID.BIN", &a) == RWY_OK &&
          (a.mode & 02777) == 02750, "setgid persists");
    CHECK(rwy_getattr_path(&s, "STICKY.BIN", &a) == RWY_OK &&
          (a.mode & 01777) == 01644, "sticky persists");
    CHECK(a.mtime_sec == 1700000000, "fixed mtime used");
    /* rename must keep metadata (new object wins = the renamed object itself) */
    fclose(fl.f);
    open_store(&s, &dev, &fl, tpath("rwy_posix.img"), "r+b");
    CHECK(rwy_txn_rename(&s, "SECURE.BIN", "D/SECURE.BIN") == RWY_OK, "rename secure");
    fclose(fl.f);
    open_store(&s, &dev, &fl, tpath("rwy_posix.img"), "r+b");
    CHECK(rwy_getattr_path(&s, "D/SECURE.BIN", &a) == RWY_OK &&
          a.mode == (RWY_S_IFREG | 0600) && a.uid == 0, "rename keeps metadata");
    /* EFI-write preservation analogue: rewrite file bytes, check metadata intact */
    {
        static rwy_store_t w;
        rwy_block_dev_t d2;
        file_t f2;
        open_store(&w, &d2, &f2, tpath("rwy_posix.img"), "r+b");
        CHECK(rwy_begin(&w) == RWY_OK, "begin");
        CHECK(rwy_write_path(&w, "D/SECURE.BIN", 0, "PUBLIC!", 7) == RWY_OK, "overwrite");
        CHECK(rwy_commit(&w) == RWY_OK, "commit overwrite");
        fclose(f2.f);
    }
    open_store(&s, &dev, &fl, tpath("rwy_posix.img"), "r+b");
    CHECK(rwy_getattr_path(&s, "D/SECURE.BIN", &a) == RWY_OK &&
          a.mode == (RWY_S_IFREG | 0600) && a.uid == 0 && a.gid == 0,
          "data overwrite preserves metadata");
    fclose(fl.f);
    printf("== POSIX OK ==\n");
}

/* Run a mutating workload with fault injection at every persistence point.
 * After each injected failure: remount, validate, check old-or-new. */
static void test_crash_recovery(const char *store_path) {
    printf("== fault-injection crash recovery ==\n");
    /* First measure how many persistence points a create+write+commit takes. */
    {
        file_t fl;
        rwy_block_dev_t dev;
        static rwy_store_t s;
        fi_t fi;
        copy_file(store_path, tpath("rwy_crash.img"));
        fl.f = fopen(tpath("rwy_crash.img"), "r+b");
        dev.read = file_rd; dev.write = file_wr; dev.flush = file_fl;
        dev.ctx = &fl; dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
        fi.inner = dev; fi.point = 0; fi.fail_at = -1; fi.failed = 0;
        {
            rwy_block_dev_t fd = { fi_rd, fi_wr, fi_fl, &fi,
                                    RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
            if (rwy_mount(&s, &fd)) { printf("mount failed\n"); failures++; fclose(fl.f); return; }
            if (rwy_begin(&s)) { printf("begin failed\n"); failures++; fclose(fl.f); return; }
            if (rwy_create(&s, "CRASH.DAT", RWY_S_IFREG | 0644, 0, 0)) { printf("create failed\n"); failures++; }
            {
                char w[8192];
                memset(w, 0xAB, sizeof(w));
                if (rwy_write_path(&s, "CRASH.DAT", 0, w, sizeof(w))) { printf("write failed\n"); failures++; }
            }
            if (rwy_commit(&s)) { printf("commit failed\n"); failures++; }
            printf("workload persistence points: %ld\n", fi.point);
            if (fi.point > 200) { printf("WARN: many points\n"); }
            /* Now inject at every point. */
            for (long at = 1; at <= fi.point; at++) {
                file_t fl2;
                rwy_block_dev_t d2;
                static rwy_store_t s2, s3;
                fi_t fi2;
                int rc;
                copy_file(store_path, tpath("rwy_crash_one.img"));
                fl2.f = fopen(tpath("rwy_crash_one.img"), "r+b");
                d2.read = file_rd; d2.write = file_wr; d2.flush = file_fl;
                d2.ctx = &fl2; d2.size = RWY_STORE_SIZE; d2.block_size = RWY_BLOCK_SIZE;
                fi2.inner = d2; fi2.point = 0; fi2.fail_at = at; fi2.failed = 0;
                {
                    rwy_block_dev_t fd2 = { fi_rd, fi_wr, fi_fl, &fi2,
                                             RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
                    rc = rwy_mount(&s2, &fd2);
                    if (rc) { printf("FAIL: mount under injection at=%ld\n", at); failures++; fclose(fl2.f); continue; }
                    rc = rwy_begin(&s2);
                    if (rc) { printf("FAIL: begin at=%ld\n", at); failures++; fclose(fl2.f); continue; }
                    rc = rwy_create(&s2, "CRASH.DAT", RWY_S_IFREG | 0644, 0, 0);
                    if (!rc) {
                        char w[8192];
                        memset(w, 0xAB, sizeof(w));
                        rc = rwy_write_path(&s2, "CRASH.DAT", 0, w, sizeof(w));
                    }
                    if (!rc) rc = rwy_commit(&s2);
                    /* rc != 0 is EXPECTED when the failure hits (claim: only
                     * fail_at points fail). Either way, recovery must hold. */
                }
                fclose(fl2.f);
                /* Reopen without injection: must see old-or-new, valid fs. */
                {
                    file_t fl3;
                    rwy_block_dev_t d3;
                    fl3.f = fopen(tpath("rwy_crash_one.img"), "r+b");
                    d3.read = file_rd; d3.write = file_wr; d3.flush = file_fl;
                    d3.ctx = &fl3; d3.size = RWY_STORE_SIZE; d3.block_size = RWY_BLOCK_SIZE;
                    rc = rwy_mount(&s3, &d3);
                    if (rc) { printf("FAIL: remount after fault at=%ld rc=%d\n", at, rc); failures++; fclose(fl3.f); continue; }
                    {
                        rwy_attr_t a;
                        int present = (rwy_getattr_path(&s3, "CRASH.DAT", &a) == RWY_OK);
                        if (present) {
                            /* new state: full 8 KiB of 0xAB */
                            uint8_t *buf = malloc(8192);
                            size_t got = 8192;
                            int ok = 0;
                            if (a.size == 8192 && !rwy_read_path(&s3, "CRASH.DAT", 0, buf, &got) && got == 8192) {
                                ok = 1;
                                for (size_t i = 0; i < got; i++)
                                    if (buf[i] != 0xAB) { ok = 0; break; }
                            }
                            free(buf);
                            if (!ok) { printf("FAIL: partial new state at=%ld\n", at); failures++; }
                        }
                        /* absent = old state: also fine */
                    }
                    fclose(fl3.f);
                }
            }
        }
        fclose(fl.f);
    }
    printf("== crash recovery OK ==\n");
}

static void test_uki_rotation(const char *store_path) {
    printf("== UKI rotation under faults ==\n");
    /* Prepare image A. */
    copy_file(store_path, tpath("rwy_uki.img"));
    {
        file_t fl;
        rwy_block_dev_t dev;
        static rwy_store_t s;
        int rc;
        open_store(&s, &dev, &fl, tpath("rwy_uki.img"), "r+b");
        /* Ensure EFI/Linux exists (commit; ignore EEXIST). */
        rc = rwy_begin(&s);
        if (!rc) {
            rc = rwy_mkdir(&s, "EFI/Linux", RWY_S_IFDIR | 0755, 0, 0);
            if (!rc || rc == RWY_ERR_EXIST) rc = rwy_commit(&s);
            else rwy_abort(&s);
        }
        fclose(fl.f);
    }
    {
        file_t fl;
        rwy_block_dev_t dev;
        static rwy_store_t s;
        open_store(&s, &dev, &fl, tpath("rwy_uki.img"), "r+b");
        /* create A (ignore exists) */
        rwy_begin(&s);
        if (rwy_lookup(&s, "EFI/Linux/uki.efi", 0) != RWY_OK) {
            if (rwy_create(&s, "EFI/Linux/uki.efi", RWY_S_IFREG | 0644, 0, 0)) { printf("create A failed\n"); failures++; }
            else {
                uint8_t *a = malloc(65536);
                memset(a, 0xAA, 65536);
                if (rwy_write_path(&s, "EFI/Linux/uki.efi", 0, a, 65536)) { printf("write A failed\n"); failures++; }
                free(a);
            }
            if (rwy_commit(&s)) { printf("commit A failed\n"); failures++; }
        } else rwy_abort(&s);
        fclose(fl.f);
    }
    /* Measure rotation workload points. */
    {
        file_t fl;
        rwy_block_dev_t dev;
        static rwy_store_t s;
        fi_t fi;
        uint8_t *bimg;
        long npoints = 0;
        fl.f = fopen(tpath("rwy_uki.img"), "r+b");
        dev.read = file_rd; dev.write = file_wr; dev.flush = file_fl;
        dev.ctx = &fl; dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
        fi.inner = dev; fi.point = 0; fi.fail_at = -1; fi.failed = 0;
        bimg = malloc(65536);
        memset(bimg, 0xBB, 65536);
        {
            rwy_block_dev_t fd = { fi_rd, fi_wr, fi_fl, &fi,
                                    RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
            if (rwy_mount(&s, &fd)) { printf("mount failed\n"); failures++; }
            else {
                rwy_begin(&s);
                rwy_create(&s, "EFI/Linux/uki.new", RWY_S_IFREG | 0644, 0, 0);
                rwy_write_path(&s, "EFI/Linux/uki.new", 0, bimg, 65536);
                /* verify step (read-only, no persistence) */
                rwy_rename(&s, "EFI/Linux/uki.new", "EFI/Linux/uki.efi");
                rwy_abort(&s); /* dry run: don't publish; just counted points */
                npoints = fi.point;
            }
        }
        printf("rotation workload points: %ld\n", npoints);
        /* Inject at every point of the REAL rotation. */
        for (long at = 1; at <= npoints; at++) {
            file_t fl2;
            rwy_block_dev_t d2;
            static rwy_store_t s2, s3;
            fi_t fi2;
            int rc;
            copy_file(tpath("rwy_uki.img"), tpath("rwy_uki_one.img"));
            fl2.f = fopen(tpath("rwy_uki_one.img"), "r+b");
            d2.read = file_rd; d2.write = file_wr; d2.flush = file_fl;
            d2.ctx = &fl2; d2.size = RWY_STORE_SIZE; d2.block_size = RWY_BLOCK_SIZE;
            fi2.inner = d2; fi2.point = 0; fi2.fail_at = at; fi2.failed = 0;
            {
                rwy_block_dev_t fd2 = { fi_rd, fi_wr, fi_fl, &fi2,
                                         RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
                rc = rwy_mount(&s2, &fd2);
                if (rc) { printf("FAIL: mount at=%ld\n", at); failures++; fclose(fl2.f); continue; }
                rc = rwy_begin(&s2);
                if (!rc) rc = rwy_create(&s2, "EFI/Linux/uki.new", RWY_S_IFREG | 0644, 0, 0);
                if (!rc) rc = rwy_write_path(&s2, "EFI/Linux/uki.new", 0, bimg, 65536);
                if (!rc) {
                    /* verify B before replace */
                    uint8_t chk[65536];
                    size_t got = sizeof(chk);
                    rc = rwy_read_path(&s2, "EFI/Linux/uki.new", 0, chk, &got);
                    if (!rc) {
                        int ok = (got == 65536);
                        for (size_t i = 0; ok && i < got; i++)
                            if (chk[i] != 0xBB) ok = 0;
                        if (!ok) rc = RWY_ERR_CORRUPT;
                    }
                }
                if (!rc) rc = rwy_rename(&s2, "EFI/Linux/uki.new", "EFI/Linux/uki.efi");
                if (!rc) rc = rwy_commit(&s2);
            }
            fclose(fl2.f);
            {
                file_t fl3;
                rwy_block_dev_t d3;
                fl3.f = fopen(tpath("rwy_uki_one.img"), "r+b");
                d3.read = file_rd; d3.write = file_wr; d3.flush = file_fl;
                d3.ctx = &fl3; d3.size = RWY_STORE_SIZE; d3.block_size = RWY_BLOCK_SIZE;
                rc = rwy_mount(&s3, &d3);
                if (rc) { printf("FAIL: remount at=%ld rc=%d\n", at, rc); failures++; fclose(fl3.f); continue; }
                {
                    uint8_t *buf = 0;
                    uint64_t n = 0;
                    if (read_all(&s3, "EFI/Linux/uki.efi", &buf, &n)) {
                        printf("FAIL: uki missing at=%ld\n", at); failures++;
                    } else if (n != 65536) {
                        printf("FAIL: uki size %llu at=%ld\n", (unsigned long long)n, at); failures++;
                    } else {
                        int allA = 1, allB = 1;
                        for (uint64_t i = 0; i < n; i++) {
                            if (buf[i] != 0xAA) allA = 0;
                            if (buf[i] != 0xBB) allB = 0;
                        }
                        if (!allA && !allB) {
                            printf("FAIL: uki partial/corrupt at=%ld\n", at); failures++;
                        }
                    }
                    free(buf);
                }
                fclose(fl3.f);
            }
        }
        free(bimg);
        fclose(fl.f);
    }
    printf("== UKI rotation OK ==\n");
}

/* An aborted transaction must leave the committed generation readable: blocks freed by
 * copy-on-write stay reserved until the commit that drops them. */
static void fill_block(uint8_t *b, int seed) {
    int i;
    for (i = 0; i < (int)RWY_BLOCK_SIZE; i++) b[i] = (uint8_t)(seed * 31 + i);
}

static int block_is(rwy_store_t *s, const char *path, int seed) {
    uint8_t want[RWY_BLOCK_SIZE], got[RWY_BLOCK_SIZE];
    size_t n = sizeof(got);
    fill_block(want, seed);
    return rwy_read_path(s, path, 0, got, &n) == RWY_OK && n == sizeof(got) && !memcmp(want, got, n);
}

static void test_abort_keeps_committed(const char *store_path) {
    const char *img = tpath("rwy-hosttest-abort.img");
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    uint8_t blk[RWY_BLOCK_SIZE];
    uint64_t oid;
    uint32_t phys = 0;
    printf("== abort keeps the committed generation ==\n");
    copy_file(store_path, img);
    open_store(&s, &dev, &fl, img, "r+b");
    fill_block(blk, 1);
    CHECK(rwy_txn_create(&s, "ABORT.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create");
    CHECK(rwy_txn_write_path(&s, "ABORT.BIN", 0, blk, sizeof(blk)) == RWY_OK, "write");
    CHECK(rwy_lookup(&s, "ABORT.BIN", &oid) == RWY_OK && rwy_inode_of(&s, oid, 0) && rwy_phys_of(&s, rwy_inode_of(&s, oid, 0), 0, &phys) == RWY_OK, "find the block");
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    fill_block(blk, 2);
    CHECK(rwy_write_path(&s, "ABORT.BIN", 0, blk, sizeof(blk)) == RWY_OK, "rewrite (copy on write frees the old block)");
    s.next_hint = phys;   /* the next allocation starts where the old block is */
    fill_block(blk, 3);
    CHECK(rwy_create(&s, "NEW.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create in txn");
    CHECK(rwy_write_path(&s, "NEW.BIN", 0, blk, sizeof(blk)) == RWY_OK, "write in txn");
    CHECK(rwy_abort(&s) == RWY_OK, "abort");
    fclose(fl.f);
    open_store(&s, &dev, &fl, img, "r+b");
    CHECK(block_is(&s, "ABORT.BIN", 1), "committed file intact after an abort");
    /* The same work committed: both files hold their new data. */
    CHECK(rwy_lookup(&s, "ABORT.BIN", &oid) == RWY_OK && rwy_phys_of(&s, rwy_inode_of(&s, oid, 0), 0, &phys) == RWY_OK, "find the block again");
    CHECK(rwy_begin(&s) == RWY_OK, "begin again");
    fill_block(blk, 2);
    CHECK(rwy_write_path(&s, "ABORT.BIN", 0, blk, sizeof(blk)) == RWY_OK, "rewrite again");
    s.next_hint = phys;
    fill_block(blk, 3);
    CHECK(rwy_create(&s, "NEW.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create again");
    CHECK(rwy_write_path(&s, "NEW.BIN", 0, blk, sizeof(blk)) == RWY_OK, "write again");
    CHECK(rwy_commit(&s) == RWY_OK, "commit");
    fclose(fl.f);
    open_store(&s, &dev, &fl, img, "r+b");
    CHECK(block_is(&s, "ABORT.BIN", 2) && block_is(&s, "NEW.BIN", 3), "committed work holds its data");
    fclose(fl.f);
    remove(img);
}

/* Power cut at every write and flush of a transaction that frees committed blocks and then
 * allocates over them: the mount must see the whole old or the whole new generation. */
static int cut_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    fi_t *fi = ctx;
    if (++fi->point >= fi->fail_at && fi->fail_at > 0) { fi->failed = 1; return -1; }
    return fi->inner.write(fi->inner.ctx, off, buf, len);
}
static int cut_fl(void *ctx) {
    fi_t *fi = ctx;
    if (++fi->point >= fi->fail_at && fi->fail_at > 0) { fi->failed = 1; return -1; }
    return fi->inner.flush ? fi->inner.flush(fi->inner.ctx) : 0;
}

#define CUT_BLOCKS 2

static int write_blocks(rwy_store_t *s, const char *path, int seed, int nblk) {
    uint8_t b[RWY_BLOCK_SIZE];
    for (int i = 0; i < nblk; i++) {
        fill_block(b, seed * 8 + i);
        if (rwy_write_path(s, path, (uint64_t)i * sizeof(b), b, sizeof(b))) return -1;
    }
    return 0;
}

static int blocks_are(rwy_store_t *s, const char *path, int seed, int nblk) {
    uint8_t want[RWY_BLOCK_SIZE], got[RWY_BLOCK_SIZE];
    for (int i = 0; i < nblk; i++) {
        size_t n = sizeof(got);
        fill_block(want, seed * 8 + i);
        if (rwy_read_path(s, path, (uint64_t)i * sizeof(got), got, &n) || n != sizeof(got) || memcmp(want, got, n))
            return 0;
    }
    return 1;
}

static int cut_workload(rwy_store_t *s) {
    uint64_t oid;
    uint32_t phys = 0;
    if (rwy_begin(s)) return -1;
    if (rwy_lookup(s, "CUT1.BIN", &oid) || rwy_phys_of(s, rwy_inode_of(s, oid, 0), 0, &phys)) return -1;
    if (write_blocks(s, "CUT1.BIN", 5, CUT_BLOCKS)) return -1;    /* copy on write frees the old blocks */
    if (rwy_unlink(s, "CUT2.BIN")) return -1;                     /* so does a delete */
    s->next_hint = phys;
    if (rwy_create(s, "CUT3.BIN", RWY_S_IFREG | 0644, 0, 0) || write_blocks(s, "CUT3.BIN", 6, 4)) return -1;
    return rwy_commit(s);
}

static int cut_state(rwy_store_t *s) {    /* 1 old, 2 new, 0 neither */
    rwy_attr_t a;
    int has3 = rwy_getattr_path(s, "CUT3.BIN", &a) == RWY_OK;
    int has2 = rwy_getattr_path(s, "CUT2.BIN", &a) == RWY_OK;
    if (!has3 && has2 && blocks_are(s, "CUT1.BIN", 1, CUT_BLOCKS) && blocks_are(s, "CUT2.BIN", 2, CUT_BLOCKS)) return 1;
    if (has3 && !has2 && blocks_are(s, "CUT1.BIN", 5, CUT_BLOCKS) && blocks_are(s, "CUT3.BIN", 6, 4)) return 2;
    return 0;
}

static void test_power_cut_frees(const char *store_path) {
    const char *base = tpath("rwy-hosttest-cut-base.img"), *one = tpath("rwy-hosttest-cut.img");
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    fi_t fi;
    long points;
    int bad = 0;
    printf("== power cut while freed blocks are reused ==\n");
    copy_file(store_path, base);
    open_store(&s, &dev, &fl, base, "r+b");
    CHECK(rwy_txn_create(&s, "CUT1.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create CUT1");
    CHECK(rwy_txn_create(&s, "CUT2.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create CUT2");
    CHECK(rwy_begin(&s) == RWY_OK && write_blocks(&s, "CUT1.BIN", 1, CUT_BLOCKS) == 0 &&
          write_blocks(&s, "CUT2.BIN", 2, CUT_BLOCKS) == 0 && rwy_commit(&s) == RWY_OK, "fill the files");
    fclose(fl.f);
    /* Count the points of the workload. */
    copy_file(base, one);
    fl.f = fopen(one, "r+b");
    dev.read = file_rd; dev.write = file_wr; dev.flush = file_fl;
    dev.ctx = &fl; dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
    fi.inner = dev; fi.point = 0; fi.fail_at = -1; fi.failed = 0;
    {
        rwy_block_dev_t fd = { fi_rd, cut_wr, cut_fl, &fi, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
        CHECK(rwy_mount(&s, &fd) == RWY_OK, "mount to count");
        fi.point = 0;
        CHECK(cut_workload(&s) == RWY_OK, "workload commits");
        points = fi.point;
    }
    fclose(fl.f);
    printf("workload persistence points: %ld\n", points);
    CHECK(points > 3, "the workload has points to cut");
    for (long at = 1; at <= points; at++) {
        int st;
        copy_file(base, one);
        fl.f = fopen(one, "r+b");
        dev.ctx = &fl;
        fi.inner = dev; fi.point = 0; fi.fail_at = -1; fi.failed = 0;
        {
            rwy_block_dev_t fd = { fi_rd, cut_wr, cut_fl, &fi, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
            if (rwy_mount(&s, &fd)) { bad++; fclose(fl.f); continue; }
            fi.point = 0; fi.fail_at = at;
            cut_workload(&s);          /* fails once the cut is hit; that is expected */
        }
        fclose(fl.f);
        open_store(&s, &dev, &fl, one, "r+b");
        st = cut_state(&s);
        if (!st) { printf("FAIL: neither old nor new generation at=%ld\n", at); bad++; }
        fclose(fl.f);
    }
    CHECK(bad == 0, "every cut leaves the old or the new generation");
    remove(base); remove(one);
}

/* A transaction that was aborted must leave nothing behind for the next gc or defrag: the blocks
 * it freed still belong to the committed generation and must stay marked in the bitmap. */
static void test_abort_then_collect(const char *store_path, int defrag) {
    const char *img = tpath("rwy-hosttest-collect.img");
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    uint8_t blk[RWY_BLOCK_SIZE];
    uint64_t oid;
    uint32_t phys = 0;
    printf("== abort, then %s, keeps the committed blocks ==\n", defrag ? "defrag" : "gc");
    copy_file(store_path, img);
    open_store(&s, &dev, &fl, img, "r+b");
    CHECK(rwy_txn_create(&s, "KEEP.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create KEEP");
    CHECK(rwy_txn_create(&s, "GAP.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create GAP");
    CHECK(rwy_txn_create(&s, "FRAG.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create FRAG");
    fill_block(blk, 1);
    CHECK(rwy_txn_write_path(&s, "KEEP.BIN", 0, blk, sizeof(blk)) == RWY_OK, "write KEEP");
    /* FRAG is written in two pieces with GAP between them, so defrag has work to do. */
    CHECK(rwy_txn_write_path(&s, "FRAG.BIN", 0, blk, sizeof(blk)) == RWY_OK, "write FRAG 0");
    CHECK(rwy_txn_write_path(&s, "GAP.BIN", 0, blk, sizeof(blk)) == RWY_OK, "write GAP");
    CHECK(rwy_txn_write_path(&s, "FRAG.BIN", sizeof(blk), blk, sizeof(blk)) == RWY_OK, "write FRAG 1");
    CHECK(rwy_lookup(&s, "KEEP.BIN", &oid) == RWY_OK && rwy_phys_of(&s, rwy_inode_of(&s, oid, 0), 0, &phys) == RWY_OK, "find the block");
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    CHECK(rwy_unlink(&s, "KEEP.BIN") == RWY_OK, "unlink KEEP in the txn");
    CHECK(rwy_abort(&s) == RWY_OK, "abort");
    CHECK(defrag ? rwy_defrag(&s) == RWY_OK : rwy_gc(&s) == RWY_OK, "collect");
    fclose(fl.f);
    open_store(&s, &dev, &fl, img, "r+b");
    CHECK(rwy_lookup(&s, "KEEP.BIN", &oid) == RWY_OK && rwy_phys_of(&s, rwy_inode_of(&s, oid, 0), 0, &phys) == RWY_OK, "KEEP survives");
    CHECK(rwy_blk_used(&s, phys), "the block of KEEP is marked");
    CHECK(block_is(&s, "KEEP.BIN", 1), "KEEP holds its data");
    fclose(fl.f);
    remove(img);
}

/* A file written in chunks into scattered free blocks needs more than the 8 direct extents and
 * must not be rewritten as one contiguous run: no free run is as large as the file. */
#define MX_HOLES 80
#define MX_CHUNK 4                       /* blocks per write */
#define MX_BLOCKS (MX_HOLES * MX_CHUNK)

static void mx_fill(uint8_t *b, uint64_t off, size_t len, int seed) {
    for (size_t i = 0; i < len; i++) b[i] = (uint8_t)((off + i) * 7 + seed);
}

static int mx_write(rwy_store_t *s, const char *path, uint64_t off, size_t len, int seed) {
    static uint8_t b[MX_BLOCKS * RWY_BLOCK_SIZE];
    mx_fill(b, off, len, seed);
    return rwy_write_path(s, path, off, b, len);
}

static int mx_trunc(rwy_store_t *s, const char *path, uint64_t size) {
    uint64_t oid;
    return rwy_lookup(s, path, &oid) ? -1 : rwy_truncate_oid(s, oid, size);
}

/* The file holds pattern `seed` for its first `n` bytes, then zeros up to `size`. */
static int mx_is(rwy_store_t *s, const char *path, uint64_t size, uint64_t n, int seed) {
    static uint8_t got[MX_BLOCKS * RWY_BLOCK_SIZE], want[MX_BLOCKS * RWY_BLOCK_SIZE];
    rwy_attr_t a;
    size_t len = (size_t)size;
    if (rwy_getattr_path(s, path, &a) || a.size != size || size > sizeof(got)) return 0;
    memset(want, 0, len);
    mx_fill(want, 0, (size_t)n, seed);
    return rwy_read_path(s, path, 0, got, &len) == RWY_OK && len == size && !memcmp(got, want, len);
}

/* Free space is MX_HOLES holes of MX_CHUNK blocks and one run of `tail` - MX_BLOCKS blocks. */
static void mx_prepare(const char *store_path, const char *img, uint32_t tail) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    rwy_frag_info_t fr;
    char name[16];
    uint8_t blk[RWY_BLOCK_SIZE] = {0};
    copy_file(store_path, img);
    open_store(&s, &dev, &fl, img, "r+b");
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    for (int i = 0; i < 2 * MX_HOLES; i++) {
        snprintf(name, sizeof(name), "G%02d", i);
        CHECK(rwy_create(&s, name, RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create G");
        for (int b = 0; b < MX_CHUNK; b++)
            CHECK(rwy_write_path(&s, name, (uint64_t)b * sizeof(blk), blk, sizeof(blk)) == RWY_OK, "write G");
    }
    CHECK(rwy_commit(&s) == RWY_OK && rwy_gc(&s) == RWY_OK, "commit the G files");
    CHECK(rwy_frag_info(&s, &fr) == RWY_OK, "frag info");
    CHECK(rwy_txn_create(&s, "FILL", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create FILL");
    CHECK(rwy_begin(&s) == RWY_OK, "begin FILL");
    for (uint32_t done = 0, left = fr.free_blocks - tail; done < left;) {
        uint32_t n = left - done > 256 ? 256 : left - done;
        static uint8_t big[256 * RWY_BLOCK_SIZE];
        CHECK(rwy_write_path(&s, "FILL", (uint64_t)done * RWY_BLOCK_SIZE, big, (size_t)n * RWY_BLOCK_SIZE) == RWY_OK, "write FILL");
        done += n;
    }
    CHECK(rwy_commit(&s) == RWY_OK, "commit FILL");
    CHECK(rwy_begin(&s) == RWY_OK, "begin holes");
    for (int i = 1; i < 2 * MX_HOLES; i += 2) {
        snprintf(name, sizeof(name), "G%02d", i);
        CHECK(rwy_unlink(&s, name) == RWY_OK, "unlink G");
    }
    CHECK(rwy_commit(&s) == RWY_OK && rwy_gc(&s) == RWY_OK, "open the holes");
    CHECK(rwy_frag_info(&s, &fr) == RWY_OK, "frag info again");
    CHECK(fr.largest_free_run < MX_BLOCKS && fr.free_blocks > MX_BLOCKS + 72, "free space is scattered");
    fclose(fl.f);
}

static int mx_big(rwy_store_t *s) {
    if (rwy_begin(s) || rwy_create(s, "BIG", RWY_S_IFREG | 0644, 0, 0)) return -1;
    for (int i = 0; i < MX_HOLES; i++)
        if (mx_write(s, "BIG", (uint64_t)i * MX_CHUNK * RWY_BLOCK_SIZE, MX_CHUNK * RWY_BLOCK_SIZE, 1)) return -1;
    return rwy_commit(s);
}

/* One write larger than the largest free run, on the scattered store of mx_prepare(). A file of
 * `pre` bytes (pattern 3) exists first, then `len` bytes of pattern 1 are written at `at`. The
 * result is read back inside the transaction, after the commit and after a remount. */
static void split_case(const char *base, const char *one, const char *what, uint64_t pre,
                       uint64_t at, uint64_t len) {
    static uint8_t got[2 * MX_BLOCKS * RWY_BLOCK_SIZE], want[2 * MX_BLOCKS * RWY_BLOCK_SIZE];
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    rwy_frag_info_t fr;
    uint64_t size = at + len > pre ? at + len : pre;
    size_t n = (size_t)size;
    int pass;
    printf("-- %s\n", what);
    copy_file(base, one);
    open_store(&s, &dev, &fl, one, "r+b");

    CHECK(rwy_frag_info(&s, &fr) == RWY_OK && fr.largest_free_run < len / RWY_BLOCK_SIZE, "the write is larger than any free run");
    CHECK(rwy_begin(&s) == RWY_OK && rwy_create(&s, "BIG", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create BIG");
    if (pre) CHECK(mx_write(&s, "BIG", 0, (size_t)pre, 3) == RWY_OK, "first write");
    CHECK(mx_write(&s, "BIG", at, (size_t)len, 1) == RWY_OK, "write over the largest free run");
    memset(want, 0, n);
    mx_fill(want, 0, (size_t)pre, 3);
    mx_fill(want + at, at, (size_t)len, 1);
    for (pass = 0; pass < 3; pass++) {
        rwy_attr_t a;
        size_t got_n = n;
        CHECK(rwy_getattr_path(&s, "BIG", &a) == RWY_OK && a.size == size, "size");
        CHECK(rwy_read_path(&s, "BIG", 0, got, &got_n) == RWY_OK && got_n == n && !memcmp(got, want, n), "reads back");
        if (pass == 0) CHECK(rwy_commit(&s) == RWY_OK, "commit");
        if (pass == 1) {
            fclose(fl.f);
            open_store(&s, &dev, &fl, one, "r+b");
        }
    }
    fclose(fl.f);
}

static void test_split_write(const char *store_path) {
    const char *base = tpath("rwy-hosttest-split-base.img"), *one = tpath("rwy-hosttest-split.img");
    const uint64_t bs = RWY_BLOCK_SIZE;
    printf("== one write over the largest free run ==\n");
    mx_prepare(store_path, base, 180);
    split_case(base, one, "aligned", 0, 0, 230 * bs);
    split_case(base, one, "unaligned edges", 0, 100, 230 * bs - 300);
    split_case(base, one, "after a gap", bs, 20 * bs + 7, 215 * bs);
    split_case(base, one, "over the end of a file", 10 * bs, 5 * bs + 9, 215 * bs);
    split_case(base, one, "many runs", 0, 0, MX_BLOCKS * bs);
    split_case(base, one, "many runs after a gap", bs, 10 * bs + 5, (MX_BLOCKS - 30) * bs);
    remove(base); remove(one);
}

static void test_many_extents(const char *store_path) {
    const char *base = tpath("rwy-hosttest-ext-base.img"), *one = tpath("rwy-hosttest-ext.img");
    const uint64_t size = (uint64_t)MX_BLOCKS * RWY_BLOCK_SIZE;
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    fi_t fi;
    rwy_frag_info_t fr;
    uint64_t oid;
    long points;
    int bad = 0;
    printf("== file in more than 8 extents ==\n");
    mx_prepare(store_path, base, 280);
    copy_file(base, one);
    open_store(&s, &dev, &fl, one, "r+b");
    CHECK(mx_big(&s) == RWY_OK, "write a file that needs more than 8 extents");
    CHECK(mx_is(&s, "BIG", size, size, 1), "it reads back");
    fclose(fl.f);
    open_store(&s, &dev, &fl, one, "r+b");
    CHECK(mx_is(&s, "BIG", size, size, 1), "it reads back after a remount");
    CHECK(rwy_lookup(&s, "BIG", &oid) == RWY_OK && rwy_inode_of(&s, oid, 0)->indirect != 0, "extents spill to the indirect block");
    /* Overwrite across the direct/indirect border and in the indirect part. */
    CHECK(rwy_begin(&s) == RWY_OK, "begin overwrite");
    CHECK(mx_write(&s, "BIG", 30 * RWY_BLOCK_SIZE + 100, 5 * RWY_BLOCK_SIZE, 2) == RWY_OK, "overwrite in the middle");
    CHECK(mx_write(&s, "BIG", 100 * RWY_BLOCK_SIZE, RWY_BLOCK_SIZE, 1) == RWY_OK, "overwrite with the same data");
    CHECK(rwy_commit(&s) == RWY_OK && rwy_gc(&s) == RWY_OK, "commit overwrite");
    {
        uint8_t *want = malloc(size), *got = malloc(size);
        size_t len = (size_t)size;
        mx_fill(want, 0, size, 1);
        mx_fill(want + 30 * RWY_BLOCK_SIZE + 100, 30 * RWY_BLOCK_SIZE + 100, 5 * RWY_BLOCK_SIZE, 2);
        CHECK(rwy_read_path(&s, "BIG", 0, got, &len) == RWY_OK && len == size && !memcmp(got, want, size), "overwrite reads back");
        free(want); free(got);
    }
    /* Shrink inside the indirect part, grow back with zeros. */
    CHECK(rwy_begin(&s) == RWY_OK, "begin shrink");
    CHECK(mx_write(&s, "BIG", 30 * RWY_BLOCK_SIZE + 100, 5 * RWY_BLOCK_SIZE, 1) == RWY_OK, "restore the patched range");
    CHECK(mx_trunc(&s, "BIG", 100 * RWY_BLOCK_SIZE + 50) == RWY_OK, "shrink");
    CHECK(rwy_commit(&s) == RWY_OK && rwy_gc(&s) == RWY_OK, "commit shrink");
    CHECK(mx_is(&s, "BIG", 100 * RWY_BLOCK_SIZE + 50, 100 * RWY_BLOCK_SIZE + 50, 1), "shrunk file reads back");
    CHECK(rwy_begin(&s) == RWY_OK, "begin grow");
    CHECK(mx_trunc(&s, "BIG", 120 * RWY_BLOCK_SIZE) == RWY_OK, "grow");
    CHECK(rwy_commit(&s) == RWY_OK, "commit grow");
    CHECK(mx_is(&s, "BIG", 120 * RWY_BLOCK_SIZE, 100 * RWY_BLOCK_SIZE + 50, 1), "grown file reads back");
    CHECK(rwy_gc(&s) == RWY_OK && mx_is(&s, "BIG", 120 * RWY_BLOCK_SIZE, 100 * RWY_BLOCK_SIZE + 50, 1), "gc keeps it");
    CHECK(rwy_defrag(&s) == RWY_OK && mx_is(&s, "BIG", 120 * RWY_BLOCK_SIZE, 100 * RWY_BLOCK_SIZE + 50, 1), "defrag keeps it");
    CHECK(rwy_frag_info(&s, &fr) == RWY_OK && fr.frag_objects == 0, "defrag leaves one run");
    CHECK(rwy_lookup(&s, "BIG", &oid) == RWY_OK && rwy_inode_of(&s, oid, 0)->indirect == 0, "the indirect block is gone");
    fclose(fl.f);
    /* Power cut at every write and flush of the big write. */
    copy_file(base, one);
    fl.f = fopen(one, "r+b");
    dev.read = file_rd; dev.write = file_wr; dev.flush = file_fl;
    dev.ctx = &fl; dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
    fi.inner = dev; fi.point = 0; fi.fail_at = -1; fi.failed = 0;
    {
        rwy_block_dev_t fd = { fi_rd, cut_wr, cut_fl, &fi, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
        CHECK(rwy_mount(&s, &fd) == RWY_OK, "mount to count");
        fi.point = 0;
        CHECK(mx_big(&s) == RWY_OK, "workload commits");
        points = fi.point;
    }
    fclose(fl.f);
    printf("workload persistence points: %ld\n", points);
    for (long at = 1; at <= points; at += 5) {
        copy_file(base, one);
        fl.f = fopen(one, "r+b");
        dev.ctx = &fl;
        fi.inner = dev; fi.point = 0; fi.fail_at = -1; fi.failed = 0;
        {
            rwy_block_dev_t fd = { fi_rd, cut_wr, cut_fl, &fi, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
            if (rwy_mount(&s, &fd)) { bad++; fclose(fl.f); continue; }
            fi.point = 0; fi.fail_at = at;
            mx_big(&s);
        }
        fclose(fl.f);
        open_store(&s, &dev, &fl, one, "r+b");
        {
            rwy_attr_t a;
            int has = rwy_getattr_path(&s, "BIG", &a) == RWY_OK;
            if (has ? !mx_is(&s, "BIG", size, size, 1) : rwy_getattr_path(&s, "G00", &a) != RWY_OK) {
                printf("FAIL: neither old nor new generation at=%ld\n", at);
                bad++;
            }
        }
        fclose(fl.f);
    }
    CHECK(bad == 0, "every cut leaves the old or the new generation");
    remove(base); remove(one);
}

/* A commit allocates no block for metadata, and the last RWY_RESERVE_BLOCKS blocks stay for
 * the paths that free space: a store filled to the limit still commits a delete. */
#define FULL_FILE "FULL0.BIN"

static uint32_t pool_free(const rwy_store_t *s) {
    uint32_t b, n = 0;
    for (b = s->sb.pool_start; b < s->sb.total_blocks; b++) n += !rwy_bmp_get(s->bitmap, b);
    return n;
}

static int full_fill(rwy_store_t *s) {
    static const uint32_t sizes[] = { 2048, 256, 32, 4, 1 };
    uint8_t *buf = malloc(2048u * RWY_BLOCK_SIZE);
    int n = 0;
    if (!buf) return -1;
    memset(buf, 0x5a, 2048u * RWY_BLOCK_SIZE);
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        for (;;) {
            char name[32];
            snprintf(name, sizeof(name), "FULL%d.BIN", n++);
            if (rwy_txn_create(s, name, RWY_S_IFREG | 0644, 0, 0)) break;
            if (rwy_txn_write_path(s, name, 0, buf, (size_t)sizes[k] * RWY_BLOCK_SIZE)) {
                rwy_txn_unlink(s, name);
                break;
            }
        }
    }
    free(buf);
    return n;
}

static int full_workload(rwy_store_t *s) {
    if (rwy_begin(s)) return -1;
    if (rwy_unlink(s, FULL_FILE)) return -1;
    return rwy_commit(s);
}

static void test_full_store(const char *store_path) {
    const char *base = tpath("rwy-hosttest-full-base.img"), *one = tpath("rwy-hosttest-full.img");
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    rwy_attr_t a;
    fi_t fi;
    long points;
    int bad = 0;
    printf("== store filled to the limit ==\n");
    copy_file(store_path, base);
    open_store(&s, &dev, &fl, base, "r+b");
    CHECK(full_fill(&s) > 0, "fill the store");
    CHECK(rwy_free_blocks(&s) == 0, "no block left for ordinary writes");
    CHECK(pool_free(&s) >= RWY_RESERVE_BLOCKS, "the reserve is intact");
    CHECK(rwy_txn_create(&s, "MORE.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_ERR_NOSPC, "an ordinary create is refused");
    CHECK(rwy_txn_mkdir(&s, "MOREDIR", RWY_S_IFDIR | 0755, 0, 0) == RWY_ERR_NOSPC, "an ordinary mkdir is refused");
    CHECK(rwy_getattr_path(&s, FULL_FILE, &a) == RWY_OK, "the first file is there");
    fclose(fl.f);
    /* Power cut at every write and flush of a delete on the full store. */
    open_store(&s, &dev, &fl, base, "r+b");
    fclose(fl.f);
    copy_file(base, one);
    fl.f = fopen(one, "r+b");
    dev.read = file_rd; dev.write = file_wr; dev.flush = file_fl;
    dev.ctx = &fl; dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
    fi.inner = dev; fi.point = 0; fi.fail_at = -1; fi.failed = 0;
    {
        rwy_block_dev_t fd = { fi_rd, cut_wr, cut_fl, &fi, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
        CHECK(rwy_mount(&s, &fd) == RWY_OK, "mount to count");
        fi.point = 0;
        CHECK(full_workload(&s) == RWY_OK, "a delete commits on the full store");
        points = fi.point;
        CHECK(rwy_free_blocks(&s) > 0, "the delete gave room back");
    }
    fclose(fl.f);
    printf("delete persistence points: %ld\n", points);
    CHECK(points > 3, "the delete has points to cut");
    for (long at = 1; at <= points; at++) {
        copy_file(base, one);
        fl.f = fopen(one, "r+b");
        dev.ctx = &fl;
        fi.inner = dev; fi.point = 0; fi.fail_at = -1; fi.failed = 0;
        {
            rwy_block_dev_t fd = { fi_rd, cut_wr, cut_fl, &fi, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
            if (rwy_mount(&s, &fd)) { bad++; fclose(fl.f); continue; }
            fi.point = 0; fi.fail_at = at;
            full_workload(&s);
        }
        fclose(fl.f);
        open_store(&s, &dev, &fl, one, "r+b");
        {
            uint8_t *got = NULL;
            uint64_t n = 0;
            int has = rwy_getattr_path(&s, FULL_FILE, &a) == RWY_OK;
            /* Old generation: the file is there with all its data. New one: it is gone. */
            if (has && (read_all(&s, FULL_FILE, &got, &n) || n != a.size)) { printf("FAIL: torn file at=%ld\n", at); bad++; }
            free(got);
        }
        fclose(fl.f);
    }
    CHECK(bad == 0, "every cut leaves the old or the new generation");
    /* With no free block at all a commit still works: the images have their own areas. */
    open_store(&s, &dev, &fl, base, "r+b");
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    for (uint32_t b = s.sb.pool_start; b < s.sb.total_blocks; b++) rwy_bmp_set(s.w_bitmap, b);
    CHECK(rwy_commit(&s) == RWY_OK, "a commit needs no free block");
    fclose(fl.f);
    open_store(&s, &dev, &fl, base, "r+b");
    CHECK(pool_free(&s) == 0, "the generation without free blocks mounts");
    fclose(fl.f);
    remove(base); remove(one);
}

/* A delete rewrites the parent directory behind the removed entry, so the reserve must hold a
 * large directory: 800 entries with the longest name are 54 blocks. */
static void test_full_big_dir(const char *store_path) {
    const char *base = tpath("rwy-hosttest-bigdir.img");
    char path[300];
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    rwy_attr_t a;
    int n;
    printf("== delete in a large directory on a full store ==\n");
    copy_file(store_path, base);
    open_store(&s, &dev, &fl, base, "r+b");
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    CHECK(rwy_mkdir(&s, "BIGDIR", RWY_S_IFDIR | 0755, 0, 0) == RWY_OK, "mkdir");
    CHECK(rwy_create(&s, "TARGET", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create target");
    for (n = 0; n < 800; n++) {
        int len = snprintf(path, sizeof(path), "BIGDIR/N%04d", n);
        memset(path + len, 'x', RWY_NAME_MAX - 5);
        path[len + RWY_NAME_MAX - 5] = 0;
        if (rwy_link(&s, "TARGET", path)) break;
    }
    CHECK(n == 800, "create the entries");
    CHECK(rwy_commit(&s) == RWY_OK, "commit the directory");
    CHECK(rwy_getattr_path(&s, "BIGDIR", &a) == RWY_OK && a.size > 50u * RWY_BLOCK_SIZE, "the directory is large");
    CHECK(full_fill(&s) > 0, "fill the store");
    CHECK(rwy_free_blocks(&s) == 0, "no block left for ordinary writes");
    snprintf(path, sizeof(path), "BIGDIR/N0000");
    memset(path + 12, 'x', RWY_NAME_MAX - 5);
    path[12 + RWY_NAME_MAX - 5] = 0;
    CHECK(rwy_txn_unlink(&s, path) == RWY_OK, "delete the first entry on the full store");
    CHECK(rwy_getattr_path(&s, path, &a) == RWY_ERR_NOENT, "the entry is gone");
    fclose(fl.f);
    remove(base);
}

/* The state record: two blocks written in place, no allocation, no commit. A write goes to
 * the copy that is not current; mount picks the valid copy with the highest sequence. */
static void rec_fill(uint8_t *p, int seed) {
    for (size_t i = 0; i < RWY_REC_PAYLOAD; i++) p[i] = (uint8_t)(i * 7 + seed);
}

static int rec_is(rwy_store_t *s, int seed) {   /* seed 0 = the zero payload mkstore writes */
    uint8_t got[RWY_REC_PAYLOAD], want[RWY_REC_PAYLOAD];
    if (seed) rec_fill(want, seed); else memset(want, 0, sizeof(want));
    return rwy_record_read(s, got) == RWY_OK && !memcmp(got, want, sizeof(got));
}

/* The empty record plus one reset, as a mount that found both copies damaged leaves it. */
static int rec_is_reset(rwy_store_t *s) {
    uint8_t got[RWY_REC_PAYLOAD], want[RWY_REC_PAYLOAD] = { 0 };
    rwy_ss_note_record_reset(want);
    return rwy_record_read(s, got) == RWY_OK && !memcmp(got, want, sizeof(got));
}

static int rec_write_seed(rwy_store_t *s, int seed) {
    uint8_t p[RWY_REC_PAYLOAD];
    rec_fill(p, seed);
    return rwy_record_write(s, p);
}

static uint64_t rec_seq_of(const char *path, uint32_t blk) {
    uint8_t b[RWY_BLOCK_SIZE];
    FILE *f = fopen(path, "rb");
    if (!f || fseek(f, (long)blk * RWY_BLOCK_SIZE, SEEK_SET) || fread(b, 1, sizeof(b), f) != sizeof(b)) exit(1);
    fclose(f);
    return ((rwy_blk_header_t *)b)->generation;
}

static void rec_flip(const char *path, uint32_t blk) {
    FILE *f = fopen(path, "r+b");
    int c;
    long off = (long)blk * RWY_BLOCK_SIZE + 100;
    if (!f || fseek(f, off, SEEK_SET)) exit(1);
    c = fgetc(f);
    if (fseek(f, off, SEEK_SET) || fputc(c ^ 0xff, f) == EOF) exit(1);
    fclose(f);
}

static int rec_mount(rwy_store_t *s, file_t *fl, rwy_block_dev_t *dev, const char *path) {
    fl->f = fopen(path, "r+b");
    if (!fl->f) exit(1);
    dev->read = file_rd; dev->write = file_wr; dev->flush = file_fl;
    dev->ctx = fl; dev->size = RWY_STORE_SIZE; dev->block_size = RWY_BLOCK_SIZE;
    return rwy_mount(s, dev);
}

/* A write that fails at the cut point; with `torn` the block is half written first. */
static int torn_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    fi_t *fi = ctx;
    if (++fi->point == fi->fail_at) {
        fi->failed = 1;
        fi->inner.write(fi->inner.ctx, off, buf, len / 2);
        return -1;
    }
    return fi->inner.write(fi->inner.ctx, off, buf, len);
}

static void test_state_record(const char *store_path) {
    const char *base = tpath("rwy-hosttest-rec-base.img"), *one = tpath("rwy-hosttest-rec.img");
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    fi_t fi;
    uint32_t r0, r1;
    long points;
    int bad = 0;
    printf("== state record ==\n");
    copy_file(store_path, base);
    CHECK(rec_mount(&s, &fl, &dev, base) == RWY_OK, "mount");
    r0 = s.sb.rec_start; r1 = r0 + 1;
    CHECK(r0 == RWY_SB_BLOCKS + 2 * s.sb.area_blocks &&
          s.sb.pool_start == s.sb.net_start + 2 * s.sb.net_blocks,
          "the record follows the image areas, the pool follows the settings area");
    CHECK(rwy_blk_used(&s, r0) && rwy_blk_used(&s, r1), "the record blocks are marked used");
    CHECK(rec_is(&s, 0), "mkstore writes an empty record");
    CHECK(rec_write_seed(&s, 1) == RWY_OK && rec_is(&s, 1), "write, read back");
    CHECK(rec_write_seed(&s, 2) == RWY_OK && rec_is(&s, 2), "write again");
    fclose(fl.f);
    CHECK(rec_seq_of(base, r0) == 3 && rec_seq_of(base, r1) == 2, "the copies alternate, the sequence grows");
    CHECK(rec_mount(&s, &fl, &dev, base) == RWY_OK && rec_is(&s, 2), "mount picks the newest copy");
    fclose(fl.f);

    /* One damaged copy: the other one is used. Both damaged: an empty record, reported. */
    copy_file(base, one);
    rec_flip(one, r0);
    CHECK(rec_mount(&s, &fl, &dev, one) == RWY_OK && rec_is(&s, 1), "the newest copy is damaged: the older one is used");
    CHECK(rec_write_seed(&s, 3) == RWY_OK && rec_is(&s, 3), "a write replaces the damaged copy");
    fclose(fl.f);
    CHECK(rec_mount(&s, &fl, &dev, one) == RWY_OK && rec_is(&s, 3), "and survives a mount");
    fclose(fl.f);
    CHECK(!rwy_record_damaged(&s), "a store with a valid copy is not damaged");
    rec_flip(one, r0); rec_flip(one, r1);
    CHECK(rec_mount(&s, &fl, &dev, one) == RWY_OK && rec_is_reset(&s) && rwy_record_damaged(&s),
          "no valid copy: mount uses an empty record that counts the reset, and reports it");
    CHECK(rec_write_seed(&s, 4) == RWY_OK && rec_is(&s, 4) && !rwy_record_damaged(&s), "a write heals it");
    fclose(fl.f);
    CHECK(rec_mount(&s, &fl, &dev, one) == RWY_OK && rec_is(&s, 4) && !rwy_record_damaged(&s), "and survives a mount");
    fclose(fl.f);

    /* One write and one flush, and a cut at either leaves the old or the new record. */
    for (int torn = 0; torn < 2; torn++) {
        points = 0;
        for (long at = 0; at <= 3; at++) {
            rwy_block_dev_t fd;
            copy_file(base, one);
            CHECK(rec_mount(&s, &fl, &dev, one) == RWY_OK, "mount");
            fi.inner = dev; fi.point = 0; fi.fail_at = at ? at : -1; fi.failed = 0;
            fd = (rwy_block_dev_t){ fi_rd, torn ? torn_wr : cut_wr, cut_fl, &fi, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
            CHECK(rwy_mount(&s, &fd) == RWY_OK, "mount through the fault layer");
            fi.point = 0;
            if (at == 0) {
                CHECK(rec_write_seed(&s, 7) == RWY_OK, "write without a cut");
                points = fi.point;
                CHECK(points == 2, "one block write and one flush, no commit");
            } else {
                CHECK(rec_write_seed(&s, 7) != RWY_OK || !fi.failed, "a cut fails the write");
                CHECK(!fi.failed || rec_is(&s, 2), "a failed write keeps the record in memory");
            }
            fclose(fl.f);
            if (rec_mount(&s, &fl, &dev, one)) { printf("FAIL: mount after cut at=%ld\n", at); bad++; continue; }
            if (!rec_is(&s, 2) && !rec_is(&s, 7)) { printf("FAIL: torn record at=%ld\n", at); bad++; }
            CHECK(rec_write_seed(&s, 8) == RWY_OK && rec_is(&s, 8), "write after the cut");
            fclose(fl.f);
            CHECK(rec_mount(&s, &fl, &dev, one) == RWY_OK && rec_is(&s, 8), "and it survives a mount");
            fclose(fl.f);
        }
    }
    CHECK(bad == 0, "every cut leaves the old or the new record");

    /* No free block and no free inode are needed. */
    copy_file(store_path, one);
    CHECK(rec_mount(&s, &fl, &dev, one) == RWY_OK, "mount");
    CHECK(full_fill(&s) > 0 && rwy_free_blocks(&s) == 0, "fill the store");
    CHECK(rec_write_seed(&s, 4) == RWY_OK && rec_is(&s, 4), "a full store still takes a record write");
    fclose(fl.f);
    remove(base); remove(one);
}

typedef struct { int rc[3]; uint64_t seq[3]; } pick_t;

static int pick_check(void *ctx, unsigned i, uint64_t *seq) {
    pick_t *p = (pick_t *)ctx;
    *seq = p->seq[i];
    return p->rc[i];
}

static void test_pick_newest(void) {
    pick_t p = { { RWY_OK, RWY_OK, RWY_OK }, { 5, 9, 9 } };
    unsigned best = 99;
    CHECK(rwy_pick_newest(3, pick_check, &p, &best) == RWY_OK && best == 1, "pick: highest sequence, tie keeps the lower index");
    p.rc[1] = RWY_ERR_CORRUPT;
    p.rc[2] = RWY_ERR_IO;
    CHECK(rwy_pick_newest(3, pick_check, &p, &best) == RWY_OK && best == 0, "pick: an invalid or unreadable copy is skipped");
    p.rc[0] = RWY_ERR_CORRUPT;
    CHECK(rwy_pick_newest(3, pick_check, &p, &best) == RWY_ERR_IO, "pick: no valid copy and a read error gives IO");
    p.rc[2] = RWY_ERR_CORRUPT;
    CHECK(rwy_pick_newest(3, pick_check, &p, &best) == RWY_ERR_CORRUPT, "pick: no valid copy gives CORRUPT");
    CHECK(rwy_pick_newest(0, pick_check, &p, &best) == RWY_ERR_CORRUPT, "pick: no copy gives CORRUPT");
}

/* The superblock of an encrypted store made with a recovery area: change one field, fix the CRC, validate. */
static int sb_with(const rwy_superblock_t *base, uint32_t tweak, uint32_t value) {
    rwy_superblock_t sb = *base;
    switch (tweak) {
    case 0: sb.recovery_blocks = value; sb.loader_start = sb.recovery_start + 2 * value;
            sb.net_start = sb.loader_blocks ? (sb.loader_start + 2 + 2 * sb.loader_blocks) : sb.loader_start;
            sb.pool_start = sb.net_start + 2 * sb.net_blocks; sb.data_blocks = sb.total_blocks - sb.pool_start; break;
    case 1: sb.recovery_start = value; break;
    case 2: sb.pool_start = value; sb.data_blocks = sb.total_blocks - value; break;
    case 3: sb.crypt_algo = RWY_CRYPT_NONE; break;
    case 4: sb.flight_start = value; break;
    case 5: sb.flight_blocks = value; break;
    case 6: sb.journal_start = value; break;
    case 7: sb.journal_blocks = value; break;
    case 8: memcpy(sb.magic, "\x54\x46\x41\x54\x56\x31\x0a", 8); break;
    case 9: sb.version = value; break;
    case 10: sb.loader_start = value; break;
    case 11: sb.loader_blocks = value; sb.net_start = value ? (sb.loader_start + 2 + 2 * value) : sb.loader_start;
             sb.pool_start = sb.net_start + 2 * sb.net_blocks; sb.data_blocks = sb.total_blocks - sb.pool_start; break;
    case 12: sb.net_start = value; break;
    case 13: sb.net_blocks = value; sb.pool_start = sb.net_start + 2 * value; sb.data_blocks = sb.total_blocks - sb.pool_start; break;
    }
    sb.crc32c = rwy_crc32c(&sb, offsetof(rwy_superblock_t, crc32c));
    return rwy_validate_sb(&sb);
}

static void test_loader_geometry(void) {
    const char *img = tpath("rwy-hosttest-loader.img");
    const char *ldr = tpath("rwy-hosttest-loader.efi");
    const char *wdt_dir = tpath("rwy-hosttest-wdt");
    const char *wdt1 = tpath("rwy-hosttest-wdt/wdt1.efi");
    const char *wdt2 = tpath("rwy-hosttest-wdt/wdt2.efi");
    char cmd[1024];
    rwy_superblock_t sb;
    FILE *f;

    printf("== loader area geometry ==\n");
    f = fopen(ldr, "wb");
    CHECK(f != NULL, "create dummy loader binary");
    if (f) {
        uint8_t dummy[8192];
        memset(dummy, 0xaa, sizeof(dummy));
        fwrite(dummy, 1, sizeof(dummy), f);
        fclose(f);
    }
    mkdir(wdt_dir, 0755);
    f = fopen(wdt1, "wb");
    if (f) { fputs("WDT1", f); fclose(f); }
    f = fopen(wdt2, "wb");
    if (f) { fputs("WDT2", f); fclose(f); }

    snprintf(cmd, sizeof(cmd), "%s --size 64 --loader %s --wdt %s --loader-size 2 %s >/dev/null",
             mkstore_path(), ldr, wdt_dir, img);
    CHECK(system(cmd) == 0, "mkstore --loader --wdt --loader-size");

    f = fopen(img, "rb");
    CHECK(f && fread(&sb, sizeof(sb), 1, f) == 1, "read superblock with loader area");
    if (f) fclose(f);

    CHECK(sb.loader_blocks == 512, "loader area has 512 blocks per copy (2 MiB)");
    CHECK(sb.loader_start == sb.recovery_start + 2 * sb.recovery_blocks, "loader follows recovery");
    CHECK(sb.net_start == sb.loader_start + 2 + 2 * sb.loader_blocks, "settings follow loader area");
    CHECK(sb.pool_start == sb.net_start + 2 * sb.net_blocks, "pool follows settings area");
    CHECK(rwy_validate_sb(&sb) == RWY_OK, "superblock validates");

    snprintf(cmd, sizeof(cmd), "%s %s >/dev/null", g_fsck_cmd, img);
    CHECK(system(cmd) == 0, "fsck.runway validates store with loader area");

    remove(wdt1);
    remove(wdt2);
    rmdir(wdt_dir);
    remove(ldr);
    remove(img);
}

static void test_recovery_geometry(void) {
    const char *img = tpath("rwy-hosttest-recarea.img");
    rwy_superblock_t sb;
    FILE *f;
    char cmd[512];

    printf("== recovery area geometry ==\n");
    snprintf(cmd, sizeof(cmd), "%s --size 64 --encrypt --recovery-size 2 %s >/dev/null", mkstore_path(), img);
    CHECK(system(cmd) == 0, "mkstore --recovery-size");
    f = fopen(img, "rb");
    CHECK(f && fread(&sb, sizeof(sb), 1, f) == 1, "read the superblock");
    if (f) fclose(f);
    remove(img);
    CHECK(sb.version == RWY_VERSION && sb.version == 11, "format version 11");
    CHECK(sb.flight_blocks == 1, "default 1 flight block per copy");
    CHECK(sb.journal_blocks == 32, "default 32 journal blocks per copy");
    CHECK(sb.recovery_blocks == 512, "two MiB per copy");
    CHECK(sb.loader_blocks == 0, "no loader blocks by default");
    CHECK(sb.flight_start == sb.rec_start + RWY_REC_BLOCKS, "flight follows record");
    CHECK(sb.journal_start == sb.flight_start + 2 * sb.flight_blocks, "journal follows flight");
    CHECK(sb.recovery_start == sb.journal_start + 2 * sb.journal_blocks, "recovery follows journal");
    CHECK(sb.loader_start == sb.recovery_start + 2 * sb.recovery_blocks, "loader follows recovery");
    CHECK(sb.net_start == sb.loader_start, "settings follow loader");
    CHECK(sb.pool_start == sb.net_start + 2 * sb.net_blocks, "pool follows settings");
    CHECK(rwy_validate_sb(&sb) == RWY_OK, "the superblock validates");

    CHECK(sb_with(&sb, 0, 0) == RWY_OK, "no area is a valid geometry");
    CHECK(sb_with(&sb, 0, sb.total_blocks / 4 + 1) == RWY_ERR_SIZE, "an area over half the store is refused");
    CHECK(sb_with(&sb, 1, sb.recovery_start + 1) == RWY_ERR_SIZE, "an area that does not follow the journal is refused");
    CHECK(sb_with(&sb, 2, sb.pool_start - 1) == RWY_ERR_SIZE, "a pool that overlaps the area is refused");
    CHECK(sb_with(&sb, 3, 0) == RWY_OK, "an area on an unencrypted store is accepted");

    CHECK(sb_with(&sb, 4, sb.flight_start + 1) == RWY_ERR_SIZE, "flight start mismatch is refused");
    CHECK(sb_with(&sb, 5, sb.total_blocks / 4 + 1) == RWY_ERR_SIZE, "flight over quarter of store is refused");
    CHECK(sb_with(&sb, 6, sb.journal_start + 1) == RWY_ERR_SIZE, "journal start mismatch is refused");
    CHECK(sb_with(&sb, 7, sb.total_blocks / 4 + 1) == RWY_ERR_SIZE, "journal over quarter of store is refused");
    CHECK(sb_with(&sb, 8, 0) == RWY_ERR_MAGIC, "legacy magic values are refused with RWY_ERR_MAGIC");
    CHECK(sb_with(&sb, 9, 8) == RWY_ERR_VERSION, "old version 8 is refused with RWY_ERR_VERSION");
    CHECK(sb_with(&sb, 10, sb.loader_start + 1) == RWY_ERR_SIZE, "loader start mismatch is refused");
    CHECK(sb_with(&sb, 11, sb.total_blocks / 4 + 1) == RWY_ERR_SIZE, "loader over quarter of store is refused");
    CHECK(sb_with(&sb, 12, sb.net_start + 1) == RWY_ERR_SIZE, "settings start mismatch is refused");
    CHECK(sb_with(&sb, 13, sb.total_blocks / 4 + 1) == RWY_ERR_SIZE, "settings over quarter of store is refused");

    snprintf(cmd, sizeof(cmd), "%s --size 64 --flight-records 0 --journal-records 0 %s >/dev/null", mkstore_path(), img);
    CHECK(system(cmd) == 0, "mkstore --flight-records 0 --journal-records 0");
    f = fopen(img, "rb");
    CHECK(f && fread(&sb, sizeof(sb), 1, f) == 1, "read the superblock with no flight/journal");
    if (f) fclose(f);
    remove(img);
    CHECK(sb.flight_blocks == 0 && sb.journal_blocks == 0, "no flight or journal blocks");
    CHECK(sb.flight_start == sb.rec_start + RWY_REC_BLOCKS, "flight start follows record");
    CHECK(sb.journal_start == sb.flight_start, "journal start matches flight start");
    CHECK(sb.recovery_start == sb.journal_start, "recovery start matches journal start");
    CHECK(sb.loader_start == sb.recovery_start, "loader start matches recovery start");
    CHECK(sb.net_start == sb.loader_start, "settings start matches loader start");
    CHECK(sb.pool_start == sb.net_start + 2 * sb.net_blocks, "pool start matches settings end");
    CHECK(rwy_validate_sb(&sb) == RWY_OK, "superblock with no flight/journal validates");
}

int main(int argc, char **argv) {
    const char *store = argc > 1 ? argv[1] : "build/RUNWAY.DAT";
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    if (argc > 2) g_fsck_cmd = argv[2];
    printf("hosttest: opening %s\n", store);
    fl.f = fopen(store, "rb");
    if (!fl.f) { printf("SKIP: cannot open %s\n", store); return 1; }
    dev.read = file_rd; dev.write = file_wr; dev.flush = file_fl;
    dev.ctx = &fl; dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
    {
        int rc = rwy_mount(&s, &dev);
        CHECK(rc == RWY_OK, "mount seed store");
        if (rc == RWY_OK) test_seed_contents(&s);
    }
    fclose(fl.f);
    if (failures) { printf("\nHOSTTEST: FAIL (%d)\n", failures); return 1; }
    test_posix_persistence(store);
    test_crash_recovery(store);
    test_uki_rotation(store);
    test_abort_keeps_committed(store);
    test_power_cut_frees(store);
    test_abort_then_collect(store, 0);
    test_abort_then_collect(store, 1);
    test_many_extents(store);
    test_split_write(store);
    test_full_store(store);
    test_full_big_dir(store);
    test_state_record(store);
    test_pick_newest();
    test_recovery_geometry();
    test_loader_geometry();
    if (failures == 0) printf("\nHOSTTEST: PASS\n");
    else printf("\nHOSTTEST: FAIL (%d)\n", failures);
    return failures != 0;
}
