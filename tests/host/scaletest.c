/*
 * scaletest — Comprehensive tests for multi-extent dynamic directories
 * and configurable store sizing (64 MiB up to 4,095 MiB).
 * SPDX-License-Identifier: Apache-2.0
 *
 * Covers:
 *  1. Multi-extent large directory (>64 KiB cap, >3,000 entries):
 *     - directory growth across multiple non-contiguous extents
 *     - individual path lookup of all entries
 *     - sequential readdir stream enumeration
 *     - readdir_nth indexed enumeration
 *     - entry deletion, compaction, and truncation
 *     - fsck.runway consistency verification
 *  2. Variable store sizes: 64 MiB, 256 MiB, 1024 MiB, 4095 MiB:
 *     - sparse file creation via mkstore --size
 *     - dynamic geometry calculation verification
 *     - mounting, transactional write, readback, commit
 *     - fsck.runway verification on each size
 */
#define _FILE_OFFSET_BITS 64
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "testdir.h"
#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_format.h"

static int failures;
static char checker_path[1024] = "build/tools/fsck.runway";

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        failures++; \
    } \
} while (0)

typedef struct { FILE *f; } file_t;

static int file_rd(void *ctx, uint64_t off, void *buf, size_t len) {
    file_t *fl = ctx;
    if (fseeko(fl->f, (off_t)off, SEEK_SET)) return -1;
    return fread(buf, 1, len, fl->f) == len ? 0 : -1;
}

static int file_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    file_t *fl = ctx;
    if (fseeko(fl->f, (off_t)off, SEEK_SET)) return -1;
    return fwrite(buf, 1, len, fl->f) == len ? 0 : -1;
}

static int file_fl(void *ctx) {
    file_t *fl = ctx;
    return fflush(fl->f) ? -1 : 0;
}

static void open_store(rwy_store_t *s, rwy_block_dev_t *dev, file_t *fl,
                       const char *path, uint64_t size) {
    fl->f = fopen(path, "r+b");
    if (!fl->f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    dev->read = file_rd;
    dev->write = file_wr;
    dev->flush = file_fl;
    dev->ctx = fl;
    dev->size = size;
    dev->block_size = RWY_BLOCK_SIZE;
    if (rwy_mount(s, dev)) {
        fprintf(stderr, "mount failed: %s (size %llu)\n", path, (unsigned long long)size);
        exit(1);
    }
}

static void run_checker(const char *path) {
    char cmd[2048];
    int rc;
    snprintf(cmd, sizeof(cmd), "%s %s >/dev/null 2>&1", checker_path, path);
    rc = system(cmd);
    CHECK(rc == 0, "fsck.runway failed");
}

/* ------------------------------------------------------------------ */
/* Test 1: Multi-Extent Large Directory Test                          */
/* ------------------------------------------------------------------ */
#define NUM_ENTRIES 3000

static void test_large_directory(const char *base_store) {
    const char *img = tpath("rwy_test_largedir.img");
    rwy_store_t s;
    rwy_block_dev_t dev;
    file_t fl;
    char path[128];
    uint8_t visited[NUM_ENTRIES];
    uint64_t dir_oid = 0;
    rwy_attr_t a;
    int rc, i;
    char cmd[1024];

    printf("== Large Multi-Extent Directory Test (%d entries) ==\n", NUM_ENTRIES);
    snprintf(cmd, sizeof(cmd), "cp -f %s %s", base_store, img);
    CHECK(system(cmd) == 0, "copy base store");

    open_store(&s, &dev, &fl, img, RWY_STORE_SIZE);

    CHECK(rwy_begin(&s) == RWY_OK, "begin largedir");
    CHECK(rwy_mkdir(&s, "LARGEDIR", RWY_S_IFDIR | 0755, 0, 0) == RWY_OK, "mkdir LARGEDIR");
    CHECK(rwy_create(&s, "TARGET.TXT", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create TARGET.TXT");
    CHECK(rwy_write_path(&s, "TARGET.TXT", 0, "TARGET_DATA", 11) == RWY_OK, "write TARGET.TXT");

    /* Create entries while interleaving other file writes to force
     * non-contiguous multi-extent allocation for the directory. */
    for (i = 0; i < NUM_ENTRIES; i++) {
        snprintf(path, sizeof(path), "LARGEDIR/ENTRY_%04d.TXT", i);
        rc = rwy_link(&s, "TARGET.TXT", path);
        if (rc != RWY_OK) {
            printf("FAIL: link %s rc=%d (line %d)\n", path, rc, __LINE__);
            failures++;
            break;
        }

        /* Every 100 entries, create a filler file to break directory contiguity */
        if (i > 0 && (i % 100) == 0) {
            char filler[64];
            uint8_t buf[RWY_BLOCK_SIZE];
            memset(buf, (uint8_t)i, sizeof(buf));
            snprintf(filler, sizeof(filler), "FILLER_%04d.DAT", i);
            CHECK(rwy_create(&s, filler, RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create filler");
            CHECK(rwy_write_path(&s, filler, 0, buf, sizeof(buf)) == RWY_OK, "write filler");
        }
    }

    CHECK(rwy_commit(&s) == RWY_OK, "commit largedir");
    fclose(fl.f);

    /* Verify consistency with fsck.runway */
    run_checker(img);

    /* Remount and inspect directory metadata */
    open_store(&s, &dev, &fl, img, RWY_STORE_SIZE);
    CHECK(rwy_getattr_path(&s, "LARGEDIR", &a) == RWY_OK, "getattr LARGEDIR");
    /* 3000 entries * 32 B = 96,000 B > 64 KiB */
    CHECK(a.size >= 96000ULL, "directory size > 64 KiB cap");

    /* Check that directory spans multiple extents */
    rc = rwy_lookup(&s, "LARGEDIR", &dir_oid);
    CHECK(rc == RWY_OK, "lookup LARGEDIR OID");
    if (rc == RWY_OK && dir_oid >= 1 && dir_oid <= RWY_MAX_INODES) {
        const rwy_inode_t *in = &s.inodes[dir_oid - 1];
        printf("  LARGEDIR size=%llu extents=%u\n",
               (unsigned long long)in->size, in->extent_count);
        CHECK(in->extent_count > 1, "directory spans multiple extents");
    }

    /* Verify individual lookup for all 3,000 entries */
    for (i = 0; i < NUM_ENTRIES; i++) {
        snprintf(path, sizeof(path), "LARGEDIR/ENTRY_%04d.TXT", i);
        rc = rwy_getattr_path(&s, path, &a);
        if (rc != RWY_OK) {
            printf("FAIL: getattr %s rc=%d\n", path, rc);
            failures++;
            break;
        }
    }
    CHECK(rwy_getattr_path(&s, "LARGEDIR/NONEXIST.TXT", &a) == RWY_ERR_NOENT, "nonexist entry");

    /* Verify batch readdir enumeration across multiple extents */
    memset(visited, 0, sizeof(visited));
    {
        char names[128 * 1024];
        size_t count = NUM_ENTRIES + 10;
        rc = rwy_readdir(&s, "LARGEDIR", names, sizeof(names), &count, NULL);
        CHECK(rc == RWY_OK, "rwy_readdir LARGEDIR");
        CHECK(count == NUM_ENTRIES, "readdir count == NUM_ENTRIES");
        {
            size_t off = 0, c;
            for (c = 0; c < count; c++) {
                const char *entry_name = names + off;
                int idx = -1;
                if (sscanf(entry_name, "ENTRY_%04d.TXT", &idx) == 1) {
                    if (idx >= 0 && idx < NUM_ENTRIES) {
                        if (visited[idx]) {
                            printf("FAIL: duplicate entry seen: %s\n", entry_name);
                            failures++;
                        }
                        visited[idx] = 1;
                    }
                }
                off += strlen(entry_name) + 1;
            }
        }
    }

    /* Verify indexed readdir_nth */
    {
        char name[RWY_NAME_MAX + 1];
        uint8_t type = 0;

        /* First entry */
        CHECK(rwy_readdir_nth(&s, "LARGEDIR", 0, name, sizeof(name), &type) == RWY_OK, "nth 0");
        CHECK(strcmp(name, "ENTRY_0000.TXT") == 0, "nth 0 name");
        /* Middle entry */
        CHECK(rwy_readdir_nth(&s, "LARGEDIR", 1500, name, sizeof(name), &type) == RWY_OK, "nth 1500");
        CHECK(strcmp(name, "ENTRY_1500.TXT") == 0, "nth 1500 name");
        /* Last entry */
        CHECK(rwy_readdir_nth(&s, "LARGEDIR", NUM_ENTRIES - 1, name, sizeof(name), &type) == RWY_OK, "nth last");
        CHECK(strcmp(name, "ENTRY_2999.TXT") == 0, "nth last name");
        /* Beyond end */
        CHECK(rwy_readdir_nth(&s, "LARGEDIR", NUM_ENTRIES, name, sizeof(name), &type) == RWY_ERR_NOENT, "nth OOB");
    }

    /* Delete 1,500 entries (all even numbers) to test compaction and truncation */
    CHECK(rwy_begin(&s) == RWY_OK, "begin unlink half");
    for (i = 0; i < NUM_ENTRIES; i += 2) {
        snprintf(path, sizeof(path), "LARGEDIR/ENTRY_%04d.TXT", i);
        rc = rwy_unlink(&s, path);
        if (rc != RWY_OK) {
            printf("FAIL: unlink %s rc=%d\n", path, rc);
            failures++;
            break;
        }
    }
    CHECK(rwy_commit(&s) == RWY_OK, "commit unlink half");
    fclose(fl.f);

    /* Verify consistency after mass unlink */
    run_checker(img);

    /* Remount and verify deleted entries are gone, odd entries remain */
    open_store(&s, &dev, &fl, img, RWY_STORE_SIZE);
    for (i = 0; i < NUM_ENTRIES; i++) {
        snprintf(path, sizeof(path), "LARGEDIR/ENTRY_%04d.TXT", i);
        rc = rwy_getattr_path(&s, path, &a);
        if (i % 2 == 0) {
            CHECK(rc == RWY_ERR_NOENT, "even entry unlinked");
        } else {
            CHECK(rc == RWY_OK, "odd entry retained");
        }
    }

    /* Count entries via readdir */
    {
        char names[128 * 1024];
        size_t count = NUM_ENTRIES;
        rc = rwy_readdir(&s, "LARGEDIR", names, sizeof(names), &count, NULL);
        CHECK(rc == RWY_OK, "readdir remaining");
        CHECK(count == NUM_ENTRIES / 2, "exactly 1,500 entries remain after compaction");
    }

    fclose(fl.f);
    unlink(img);
    printf("== Large Directory Test OK ==\n");
}

/* ------------------------------------------------------------------ */
/* Test 2: Variable Store Sizing Test (64, 256, 1024, 4095 MiB)       */
/* ------------------------------------------------------------------ */
static void test_store_size(uint32_t size_mib) {
    char img[256];
    char cmd[1024];
    uint64_t size_bytes = (uint64_t)size_mib * 1024ULL * 1024ULL;
    uint32_t exp_total_blocks = (uint32_t)(size_bytes / RWY_BLOCK_SIZE);
    uint32_t exp_bm_bytes = (exp_total_blocks + 7) / 8;
    uint32_t exp_bm_blocks = (exp_bm_bytes + (RWY_BLOCK_SIZE - 32) - 1) / (RWY_BLOCK_SIZE - 32);
    rwy_store_t s;
    rwy_block_dev_t dev;
    file_t fl;
    uint8_t wbuf[1024], rbuf[1024];
    size_t got = 0;
    uint32_t k;

    printf("== Configurable Store Size: %u MiB ==\n", size_mib);
    snprintf(img, sizeof(img), "%s", tpath("rwy_test_size.img"));
    snprintf(cmd, sizeof(cmd), "%s %s --size %u >/dev/null 2>&1", mkstore_path(), img, size_mib);
    CHECK(system(cmd) == 0, "mkstore --size");

    /* Run checker on initial image */
    run_checker(img);

    /* Mount and verify superblock metadata */
    open_store(&s, &dev, &fl, img, size_bytes);
    CHECK(s.sb.total_blocks == exp_total_blocks, "sb.total_blocks match");
    CHECK(s.sb.bitmap_blocks == exp_bm_blocks, "sb.bitmap_blocks match");
    CHECK(s.sb.total_size == size_bytes, "sb.total_size match");
    CHECK(s.sb.data_blocks == s.sb.total_blocks - s.sb.pool_start, "sb.data_blocks match");

    /* Transactional mutation on the custom-sized store */
    CHECK(rwy_begin(&s) == RWY_OK, "begin txn");
    CHECK(rwy_mkdir(&s, "SIZEDIR", RWY_S_IFDIR | 0755, 0, 0) == RWY_OK, "mkdir SIZEDIR");
    CHECK(rwy_create(&s, "SIZEDIR/PAYLOAD.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create PAYLOAD.BIN");

    for (k = 0; k < sizeof(wbuf); k++) wbuf[k] = (uint8_t)(k ^ size_mib);
    CHECK(rwy_write_path(&s, "SIZEDIR/PAYLOAD.BIN", 0, wbuf, sizeof(wbuf)) == RWY_OK, "write payload");
    CHECK(rwy_commit(&s) == RWY_OK, "commit txn");

    /* Read back within mounted instance */
    got = sizeof(rbuf);
    CHECK(rwy_read_path(&s, "SIZEDIR/PAYLOAD.BIN", 0, rbuf, &got) == RWY_OK, "read payload");
    CHECK(got == sizeof(wbuf), "read length match");
    CHECK(memcmp(wbuf, rbuf, sizeof(wbuf)) == 0, "payload match");

    fclose(fl.f);

    /* Verify consistency after mutation */
    run_checker(img);

    /* Remount and verify persistence */
    open_store(&s, &dev, &fl, img, size_bytes);
    got = sizeof(rbuf);
    CHECK(rwy_read_path(&s, "SIZEDIR/PAYLOAD.BIN", 0, rbuf, &got) == RWY_OK, "remount read payload");
    CHECK(got == sizeof(wbuf), "remount read length");
    CHECK(memcmp(wbuf, rbuf, sizeof(wbuf)) == 0, "remount payload match");

    fclose(fl.f);
    unlink(img);
    printf("== Store Size %u MiB OK ==\n", size_mib);
}

int main(int argc, char **argv) {
    const char *base_store = "build/RUNWAY.DAT";
    if (argc > 1) base_store = argv[1];
    if (argc > 2) snprintf(checker_path, sizeof(checker_path), "%s", argv[2]);

    printf("=================================================================\n");
    printf("  Runway Multi-Extent Directory & Configurable Store Sizing Suite  \n");
    printf("=================================================================\n");

    test_large_directory(base_store);

    test_store_size(64);
    test_store_size(256);
    test_store_size(1024);
    test_store_size(4095);

    printf("=================================================================\n");
    if (failures == 0) {
        printf("SCALETEST: PASS (all scale & multi-extent tests passed)\n");
        return 0;
    } else {
        printf("SCALETEST: FAIL (%d failures)\n", failures);
        return 1;
    }
}
