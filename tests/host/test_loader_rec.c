/*
 * test_loader_rec.c — Loader state record unit tests (torn writes, pick, write, corrupt).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rwy_crypto.h"
#include "rwy_format.h"
#include "rwy_loader_fsm.h"

#define TOTAL_BLOCKS 8U

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

typedef struct {
    uint8_t mem[TOTAL_BLOCKS * RWY_BLOCK_SIZE];
    int fail_flush;
} mem_t;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_t *m = ctx;
    if (off + len > sizeof(m->mem)) return -1;
    memcpy(buf, m->mem + off, len);
    return 0;
}

static int mem_write(void *ctx, uint64_t off, const void *buf, size_t len) {
    mem_t *m = ctx;
    if (off + len > sizeof(m->mem)) return -1;
    memcpy(m->mem + off, buf, len);
    return 0;
}

static int mem_flush(void *ctx) {
    mem_t *m = ctx;
    if (m->fail_flush) return -1;
    return 0;
}

static void mem_init(mem_t *m, rwy_block_dev_t *d) {
    memset(m, 0, sizeof(*m));
    d->read = mem_read;
    d->write = mem_write;
    d->flush = mem_flush;
    d->ctx = m;
    d->size = sizeof(m->mem);
    d->block_size = RWY_BLOCK_SIZE;
    d->stats = NULL;
    d->dircache = NULL;
}

static void test_record_check(void) {
    rwy_loader_record_t rec;

    printf("== loader record: check ==\n");
    memset(&rec, 0, sizeof(rec));
    rec.magic = RWY_BLK_MAGIC;
    rec.version = RWY_LOADER_RECORD_VERSION;
    rec.sequence = 1;
    rec.good = 0;
    rec.trial = 0;
    rec.crc32c = rwy_crc32c(&rec, offsetof(rwy_loader_record_t, crc32c));

    CHECK(rwy_loader_record_check(&rec) == RWY_OK, "valid record passes");
    CHECK(rwy_loader_record_check(NULL) == RWY_ERR_INVAL, "NULL record rejected");

    rec.magic = 0xdeadbeef;
    CHECK(rwy_loader_record_check(&rec) == RWY_ERR_CORRUPT, "bad magic rejected");
    rec.magic = RWY_BLK_MAGIC;

    rec.version = 2;
    CHECK(rwy_loader_record_check(&rec) == RWY_ERR_CORRUPT, "bad version rejected");
    rec.version = RWY_LOADER_RECORD_VERSION;

    rec.good = 2;
    CHECK(rwy_loader_record_check(&rec) == RWY_ERR_CORRUPT, "good > 1 rejected");
    rec.good = 0;

    rec.trial = 3;
    CHECK(rwy_loader_record_check(&rec) == RWY_ERR_CORRUPT, "trial > 2 rejected");
    rec.trial = 0;

    rec.crc32c ^= 1;
    CHECK(rwy_loader_record_check(&rec) == RWY_ERR_CORRUPT, "bad crc rejected");
}

static void test_alternating_writes(void) {
    mem_t mem;
    rwy_block_dev_t dev;
    rwy_loader_record_t rec, picked;
    int cur = -1, rc;

    printf("== loader record: alternating writes ==\n");
    mem_init(&mem, &dev);

    rc = rwy_loader_record_pick(&dev, 0, &picked, &cur);
    CHECK(rc == RWY_ERR_CORRUPT && cur == -1, "empty store has no valid record");

    memset(&rec, 0, sizeof(rec));
    rec.good = 0;
    rec.timeout = RWY_LOADER_TIMEOUT_DEFAULT;

    rc = rwy_loader_record_write(&dev, 0, cur, &rec, &cur);
    CHECK(rc == RWY_OK && cur == 0, "first write goes to copy 0");

    rc = rwy_loader_record_pick(&dev, 0, &picked, &cur);
    CHECK(rc == RWY_OK && cur == 0 && picked.sequence == 1, "picked copy 0 with seq 1");

    rec = picked;
    rec.good = 1;
    rc = rwy_loader_record_write(&dev, 0, cur, &rec, &cur);
    CHECK(rc == RWY_OK && cur == 1, "second write goes to copy 1");

    rc = rwy_loader_record_pick(&dev, 0, &picked, &cur);
    CHECK(rc == RWY_OK && cur == 1 && picked.sequence == 2, "picked copy 1 with seq 2");
    CHECK(picked.good == 1, "good is 1");

    rec = picked;
    rec.trial = RWY_LOADER_TRIAL_A;
    rec.tries = 3;
    rc = rwy_loader_record_write(&dev, 0, cur, &rec, &cur);
    CHECK(rc == RWY_OK && cur == 0, "third write goes to copy 0");

    rc = rwy_loader_record_pick(&dev, 0, &picked, &cur);
    CHECK(rc == RWY_OK && cur == 0 && picked.sequence == 3, "picked copy 0 with seq 3");
    CHECK(picked.trial == RWY_LOADER_TRIAL_A && picked.tries == 3, "trial is A with 3 tries");

    rec = picked;
    rc = rwy_loader_record_write(&dev, 0, cur, &rec, &cur);
    CHECK(rc == RWY_OK && cur == 1, "fourth write goes to copy 1");

    rc = rwy_loader_record_pick(&dev, 0, &picked, &cur);
    CHECK(rc == RWY_OK && cur == 1 && picked.sequence == 4, "picked copy 1 with seq 4");
}

static void test_corruption_recovery(void) {
    mem_t mem;
    rwy_block_dev_t dev;
    rwy_loader_record_t rec, picked;
    int cur = -1, rc;

    printf("== loader record: corruption recovery ==\n");
    mem_init(&mem, &dev);

    memset(&rec, 0, sizeof(rec));
    rec.good = 0;
    rwy_loader_record_write(&dev, 0, cur, &rec, &cur); /* copy 0, seq 1 */
    rec.good = 1;
    rwy_loader_record_write(&dev, 0, cur, &rec, &cur); /* copy 1, seq 2 */

    /* Corrupt copy 1 */
    mem.mem[RWY_BLOCK_SIZE + 10] ^= 0xff;

    rc = rwy_loader_record_pick(&dev, 0, &picked, &cur);
    CHECK(rc == RWY_OK && cur == 0 && picked.sequence == 1, "corrupt copy 1 falls back to copy 0");
    CHECK(picked.good == 0, "recovered copy 0 content");

    /* Writing now should overwrite corrupted copy 1 */
    rec = picked;
    rec.good = 1;
    rc = rwy_loader_record_write(&dev, 0, cur, &rec, &cur);
    CHECK(rc == RWY_OK && cur == 1, "overwrites corrupted copy 1");

    rc = rwy_loader_record_pick(&dev, 0, &picked, &cur);
    CHECK(rc == RWY_OK && cur == 1 && picked.sequence == 2, "picked renewed copy 1 with seq 2");

    /* Corrupt copy 0 */
    mem.mem[5] ^= 0xaa;
    rc = rwy_loader_record_pick(&dev, 0, &picked, &cur);
    CHECK(rc == RWY_OK && cur == 1 && picked.sequence == 2, "corrupt copy 0 falls back to copy 1");

    /* Corrupt both copies */
    mem.mem[RWY_BLOCK_SIZE + 5] ^= 0x55;
    rc = rwy_loader_record_pick(&dev, 0, &picked, &cur);
    CHECK(rc == RWY_ERR_CORRUPT && cur == -1, "both corrupt yields CORRUPT");
}

static void test_torn_writes(void) {
    mem_t mem;
    rwy_block_dev_t dev;
    rwy_loader_record_t good_rec, new_rec, picked;
    uint8_t raw[RWY_BLOCK_SIZE];
    int cur = -1, rc;

    printf("== loader record: torn writes at every byte boundary ==\n");
    mem_init(&mem, &dev);

    memset(&good_rec, 0, sizeof(good_rec));
    good_rec.good = 0;
    rwy_loader_record_write(&dev, 0, cur, &good_rec, &cur); /* copy 0, seq 1 */

    new_rec = good_rec;
    new_rec.magic = RWY_BLK_MAGIC;
    new_rec.version = RWY_LOADER_RECORD_VERSION;
    new_rec.sequence = 2;
    new_rec.good = 1;
    new_rec.crc32c = rwy_crc32c(&new_rec, offsetof(rwy_loader_record_t, crc32c));
    memcpy(raw, &new_rec, sizeof(raw));

    for (size_t b = 0; b < RWY_BLOCK_SIZE; b++) {
        /* Reset copy 1 to zeros, then write only b bytes */
        memset(mem.mem + RWY_BLOCK_SIZE, 0, RWY_BLOCK_SIZE);
        memcpy(mem.mem + RWY_BLOCK_SIZE, raw, b);

        cur = -1;
        rc = rwy_loader_record_pick(&dev, 0, &picked, &cur);
        CHECK(rc == RWY_OK, "pick succeeds despite torn write");
        CHECK(cur == 0, "torn copy 1 ignored, copy 0 selected");
        CHECK(picked.sequence == 1, "picked sequence 1");
    }

    /* Symmetric test: copy 1 is valid (written with seq 2), copy 0 is torn */
    new_rec.sequence = 1;
    rwy_loader_record_write(&dev, 0, 0, &new_rec, &cur); /* copy 1, seq becomes 2 */

    new_rec.sequence = 3;
    new_rec.good = 0;
    new_rec.crc32c = rwy_crc32c(&new_rec, offsetof(rwy_loader_record_t, crc32c));
    memcpy(raw, &new_rec, sizeof(raw));

    for (size_t b = 0; b < RWY_BLOCK_SIZE; b++) {
        memset(mem.mem, 0, RWY_BLOCK_SIZE);
        memcpy(mem.mem, raw, b);

        cur = -1;
        rc = rwy_loader_record_pick(&dev, 0, &picked, &cur);
        CHECK(rc == RWY_OK, "pick succeeds despite torn write on copy 0");
        CHECK(cur == 1, "torn copy 0 ignored, copy 1 selected");
        CHECK(picked.sequence == 2, "picked sequence 2");
    }
}

static void test_flush_failure(void) {
    mem_t mem;
    rwy_block_dev_t dev;
    rwy_loader_record_t rec, picked;
    int cur = -1, rc;

    printf("== loader record: flush failure ==\n");
    mem_init(&mem, &dev);

    memset(&rec, 0, sizeof(rec));
    rec.good = 0;
    rwy_loader_record_write(&dev, 0, cur, &rec, &cur); /* copy 0, seq 1 */

    rec.good = 1;
    mem.fail_flush = 1;
    rc = rwy_loader_record_write(&dev, 0, cur, &rec, &cur);
    CHECK(rc == RWY_ERR_IO, "flush failure returns IO");

    mem.fail_flush = 0;
    /* Inverted target copy may have been written but uncommitted; pick still finds copy 0 valid */
    rc = rwy_loader_record_pick(&dev, 0, &picked, &cur);
    CHECK(rc == RWY_OK, "pick succeeds after flush failure");
}

static void test_reason_strings(void) {
    printf("== loader record: reason strings ==\n");
    CHECK(strcmp(rwy_loader_reason_str(RWY_LOADER_REASON_NONE), "none") == 0, "none");
    CHECK(strcmp(rwy_loader_reason_str(RWY_LOADER_REASON_BAD_HEADER), "bad header") == 0, "bad header");
    CHECK(strcmp(rwy_loader_reason_str(RWY_LOADER_REASON_BAD_RANGE), "bad range") == 0, "bad range");
    CHECK(strcmp(rwy_loader_reason_str(RWY_LOADER_REASON_BAD_HASH), "bad hash") == 0, "bad hash");
    CHECK(strcmp(rwy_loader_reason_str(RWY_LOADER_REASON_START_FAILED), "start failed") == 0, "start failed");
    CHECK(strcmp(rwy_loader_reason_str(RWY_LOADER_REASON_TRIES_EXHAUSTED), "tries exhausted") == 0, "tries exhausted");
    CHECK(strcmp(rwy_loader_reason_str(RWY_LOADER_REASON_RECORD_IO), "record io") == 0, "record io");
    CHECK(strcmp(rwy_loader_reason_str(999), "unknown") == 0, "unknown");
}

int main(void) {
    test_record_check();
    test_alternating_writes();
    test_corruption_recovery();
    test_torn_writes();
    test_flush_failure();
    test_reason_strings();

    if (failures) {
        printf("%d tests failed\n", failures);
        return 1;
    }
    printf("all loader record tests passed\n");
    return 0;
}
