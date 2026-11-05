/*
 * mirrortest.c — Unit and integration tests for Multi-Device Mirror Block Layer.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Verifies:
 * 1. Device registration, initialization, limit enforcement, and primary selection.
 * 2. Sequential write propagation across all registered mirror devices.
 * 3. Sequential flush propagation across all registered mirror devices.
 * 4. Normal read routing exclusively to the primary/master device.
 * 5. Resilient read failover to alternate replicas upon primary I/O errors.
 * 6. Cascading read failover across 3+ replicas.
 * 7. Configurable write policies (BEST_EFFORT, REQUIRE_ALL, REQUIRE_PRIMARY).
 * 8. Real filesystem CoW transaction commit over mirrored stores and dual-store validation.
 * 9. Transparent filesystem read failover when primary storage encounters read errors.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>

#include "testdir.h"
#include "rwy_format.h"
#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_internal.h"

static int failures = 0;
static const char *g_checkbin = "build/tools/fsck.runway";

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        failures++; \
    } \
} while (0)

/* ========================================================================= */
/*                          Mock In-Memory Block Device                      */
/* ========================================================================= */

#define MOCK_DEV_SIZE (128 * 1024) /* 128 KiB buffer */

#define MAX_OP_LOG 256
static int g_op_log[MAX_OP_LOG];
static size_t g_op_count = 0;

static void log_op(int dev_id) {
    if (g_op_count < MAX_OP_LOG) {
        g_op_log[g_op_count++] = dev_id;
    }
}

static void reset_op_log(void) {
    g_op_count = 0;
    memset(g_op_log, 0, sizeof(g_op_log));
}

typedef struct {
    int dev_id;
    uint8_t data[MOCK_DEV_SIZE];
    uint64_t size;
    int fail_read;
    int fail_write;
    int fail_flush;
    int flush_called;
} mock_dev_t;

static void mock_dev_init(mock_dev_t *d, int dev_id) {
    memset(d, 0, sizeof(*d));
    d->dev_id = dev_id;
    d->size = MOCK_DEV_SIZE;
}

static int mock_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mock_dev_t *d = (mock_dev_t *)ctx;
    log_op(d->dev_id);
    if (d->fail_read) return -1;
    if (off + len > d->size) return -1;
    memcpy(buf, d->data + off, len);
    return 0;
}

static int mock_write(void *ctx, uint64_t off, const void *buf, size_t len) {
    mock_dev_t *d = (mock_dev_t *)ctx;
    log_op(d->dev_id);
    if (d->fail_write) return -1;
    if (off + len > d->size) return -1;
    memcpy(d->data + off, buf, len);
    return 0;
}

static int mock_flush(void *ctx) {
    mock_dev_t *d = (mock_dev_t *)ctx;
    log_op(d->dev_id);
    d->flush_called = 1;
    if (d->fail_flush) return -1;
    return 0;
}

static void setup_mock_blk(rwy_block_dev_t *bdev, mock_dev_t *mdev) {
    bdev->read = mock_read;
    bdev->write = mock_write;
    bdev->flush = mock_flush;
    bdev->ctx = mdev;
    bdev->size = mdev->size;
    bdev->block_size = RWY_BLOCK_SIZE;
}

/* ========================================================================= */
/*                          File-backed Device Helpers                       */
/* ========================================================================= */

typedef struct {
    FILE *f;
    int fail_read;
    int fail_write;
} file_dev_t;

static int file_rd(void *ctx, uint64_t off, void *buf, size_t len) {
    file_dev_t *fl = (file_dev_t *)ctx;
    if (fl->fail_read) return -1;
    if (fseek(fl->f, (long)off, SEEK_SET)) return -1;
    return fread(buf, 1, len, fl->f) == len ? 0 : -1;
}

static int file_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    file_dev_t *fl = (file_dev_t *)ctx;
    if (fl->fail_write) return -1;
    if (fseek(fl->f, (long)off, SEEK_SET)) return -1;
    return fwrite(buf, 1, len, fl->f) == len ? 0 : -1;
}

static int file_fl(void *ctx) {
    file_dev_t *fl = (file_dev_t *)ctx;
    return fflush(fl->f) ? -1 : 0;
}

static void copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    char buf[65536];
    size_t n;
    CHECK(in != NULL, "open source file");
    CHECK(out != NULL, "open dest file");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        size_t written = fwrite(buf, 1, n, out);
        CHECK(written == n, "fwrite copy chunk");
    }
    fclose(in);
    fclose(out);
}

/* ========================================================================= */
/*                                Test Cases                                 */
/* ========================================================================= */

static void test_mirror_init_and_registration(void) {
    printf("--- Unit: Mirror Initialization & Registration ---\n");
    rwy_mirror_dev_t m;
    rwy_block_dev_t bdev;
    mock_dev_t md;

    CHECK(rwy_mirror_init(NULL) == RWY_MIRROR_ERR_INVAL, "init NULL returns INVAL");

    int rc = rwy_mirror_init(&m);
    CHECK(rc == RWY_MIRROR_OK, "init succeeds");
    CHECK(m.num_devs == 0, "num_devs initially 0");
    CHECK(m.primary_idx == 0, "primary_idx initially 0");
    CHECK(m.write_policy == RWY_MIRROR_WRITE_BEST_EFFORT, "default write policy is BEST_EFFORT");

    mock_dev_init(&md, 1);
    setup_mock_blk(&bdev, &md);

    CHECK(rwy_mirror_add(NULL, &bdev) == RWY_MIRROR_ERR_INVAL, "add NULL mdev returns INVAL");
    CHECK(rwy_mirror_add(&m, NULL) == RWY_MIRROR_ERR_INVAL, "add NULL dev returns INVAL");

    /* Add up to max devices */
    for (size_t i = 0; i < RWY_MIRROR_MAX_DEVS; i++) {
        rc = rwy_mirror_add(&m, &bdev);
        CHECK(rc == RWY_MIRROR_OK, "add device within limit");
        CHECK(m.num_devs == i + 1, "num_devs matches added count");
    }

    /* Adding one more must fail with FULL */
    rc = rwy_mirror_add(&m, &bdev);
    CHECK(rc == RWY_MIRROR_ERR_FULL, "add beyond max returns FULL");

    /* Primary index configuration */
    CHECK(rwy_mirror_set_primary(&m, 2) == RWY_MIRROR_OK, "set primary index 2");
    CHECK(m.primary_idx == 2, "primary index updated to 2");
    CHECK(rwy_mirror_set_primary(&m, RWY_MIRROR_MAX_DEVS) == RWY_MIRROR_ERR_INVAL,
          "set primary out of bounds returns INVAL");

    /* Wrapping */
    rwy_block_dev_t wrapped;
    CHECK(rwy_mirror_wrap(NULL, &m) == RWY_MIRROR_ERR_INVAL, "wrap with NULL out returns INVAL");
    CHECK(rwy_mirror_wrap(&wrapped, NULL) == RWY_MIRROR_ERR_INVAL, "wrap with NULL mdev returns INVAL");

    rc = rwy_mirror_dev_wrap(&wrapped, &m);
    CHECK(rc == RWY_MIRROR_OK, "wrap succeeds");
    CHECK(wrapped.read == rwy_mirror_read, "wrapped read function pointer set");
    CHECK(wrapped.write == rwy_mirror_write, "wrapped write function pointer set");
    CHECK(wrapped.flush == rwy_mirror_flush, "wrapped flush function pointer set");
    CHECK(wrapped.ctx == &m, "wrapped ctx points to mdev");
    CHECK(wrapped.size == MOCK_DEV_SIZE, "wrapped size matches underlying");
    CHECK(wrapped.block_size == RWY_BLOCK_SIZE, "wrapped block size matches underlying");
}

static void test_sequential_write_propagation(void) {
    printf("--- Unit: Sequential Write Propagation ---\n");
    mock_dev_t md0, md1, md2;
    rwy_block_dev_t b0, b1, b2, wrapped;
    rwy_mirror_dev_t m;

    mock_dev_init(&md0, 0);
    mock_dev_init(&md1, 1);
    mock_dev_init(&md2, 2);
    setup_mock_blk(&b0, &md0);
    setup_mock_blk(&b1, &md1);
    setup_mock_blk(&b2, &md2);

    rwy_mirror_init(&m);
    rwy_mirror_add(&m, &b0);
    rwy_mirror_add(&m, &b1);
    rwy_mirror_add(&m, &b2);
    rwy_mirror_wrap(&wrapped, &m);

    const char test_data[] = "RUNWAY RAID1 Sequential Write Test Block 0123456789";
    size_t len = sizeof(test_data);
    uint64_t offset = 4096;

    reset_op_log();
    int rc = wrapped.write(wrapped.ctx, offset, test_data, len);
    CHECK(rc == 0, "wrapped write returns 0");

    /* Verify execution sequence: dev 0 written first, then dev 1, then dev 2 */
    CHECK(g_op_count == 3, "3 write operations logged");
    CHECK(g_op_log[0] == 0, "dev 0 written first");
    CHECK(g_op_log[1] == 1, "dev 1 written second");
    CHECK(g_op_log[2] == 2, "dev 2 written third");

    /* Verify byte-for-byte propagation across all replicas */
    CHECK(memcmp(md0.data + offset, test_data, len) == 0, "dev 0 received correct data");
    CHECK(memcmp(md1.data + offset, test_data, len) == 0, "dev 1 received correct data");
    CHECK(memcmp(md2.data + offset, test_data, len) == 0, "dev 2 received correct data");

    /* Verify operational counters */
    CHECK(m.write_ops[0] == 1, "write_ops[0] is 1");
    CHECK(m.write_ops[1] == 1, "write_ops[1] is 1");
    CHECK(m.write_ops[2] == 1, "write_ops[2] is 1");
    CHECK(m.write_errors[0] == 0, "write_errors[0] is 0");
    CHECK(m.write_errors[1] == 0, "write_errors[1] is 0");
    CHECK(m.write_errors[2] == 0, "write_errors[2] is 0");
}

static void test_sequential_flush_propagation(void) {
    printf("--- Unit: Sequential Flush Propagation ---\n");
    mock_dev_t md0, md1;
    rwy_block_dev_t b0, b1, wrapped;
    rwy_mirror_dev_t m;

    mock_dev_init(&md0, 0);
    mock_dev_init(&md1, 1);
    setup_mock_blk(&b0, &md0);
    setup_mock_blk(&b1, &md1);

    rwy_mirror_init(&m);
    rwy_mirror_add(&m, &b0);
    rwy_mirror_add(&m, &b1);
    rwy_mirror_wrap(&wrapped, &m);

    reset_op_log();
    int rc = wrapped.flush(wrapped.ctx);
    CHECK(rc == 0, "wrapped flush returns 0");

    CHECK(g_op_count == 2, "2 flush operations logged");
    CHECK(g_op_log[0] == 0, "dev 0 flushed first");
    CHECK(g_op_log[1] == 1, "dev 1 flushed second");

    CHECK(md0.flush_called == 1, "dev 0 flush called");
    CHECK(md1.flush_called == 1, "dev 1 flush called");
    CHECK(m.flush_ops[0] == 1, "flush_ops[0] is 1");
    CHECK(m.flush_ops[1] == 1, "flush_ops[1] is 1");

    /* Test NULL flush handler handled as no-op success */
    b1.flush = NULL;
    m.devs[1] = b1;
    rc = wrapped.flush(wrapped.ctx);
    CHECK(rc == 0, "wrapped flush with NULL callback returns 0");
    CHECK(m.flush_ops[1] == 2, "flush_ops[1] incremented even with NULL handler");
}

static void test_read_routing_primary(void) {
    printf("--- Unit: Primary Read Routing ---\n");
    mock_dev_t md0, md1;
    rwy_block_dev_t b0, b1, wrapped;
    rwy_mirror_dev_t m;

    mock_dev_init(&md0, 0);
    mock_dev_init(&md1, 1);
    setup_mock_blk(&b0, &md0);
    setup_mock_blk(&b1, &md1);

    /* Populate different contents in dev0 and dev1 to verify source */
    const char data0[] = "PRIMARY-PAYLOAD-0000";
    const char data1[] = "REPLICA-PAYLOAD-1111";
    memcpy(md0.data + 1024, data0, sizeof(data0));
    memcpy(md1.data + 1024, data1, sizeof(data1));

    rwy_mirror_init(&m);
    rwy_mirror_add(&m, &b0);
    rwy_mirror_add(&m, &b1);
    rwy_mirror_wrap(&wrapped, &m);

    char buf[64];
    memset(buf, 0, sizeof(buf));

    reset_op_log();
    int rc = wrapped.read(wrapped.ctx, 1024, buf, sizeof(data0));
    CHECK(rc == 0, "read returns 0");
    CHECK(memcmp(buf, data0, sizeof(data0)) == 0, "read payload came from primary dev0");
    CHECK(g_op_count == 1, "exactly 1 read was dispatched");
    CHECK(g_op_log[0] == 0, "read was dispatched exclusively to dev0");
    CHECK(m.read_ops[0] == 1, "read_ops[0] incremented");
    CHECK(m.read_ops[1] == 0, "read_ops[1] stayed 0 (no replica read)");

    /* Switch primary to dev1 and read again */
    rwy_mirror_set_primary(&m, 1);
    reset_op_log();
    memset(buf, 0, sizeof(buf));

    rc = wrapped.read(wrapped.ctx, 1024, buf, sizeof(data1));
    CHECK(rc == 0, "read returns 0 after switching primary");
    CHECK(memcmp(buf, data1, sizeof(data1)) == 0, "read payload came from new primary dev1");
    CHECK(g_op_count == 1, "exactly 1 read dispatched");
    CHECK(g_op_log[0] == 1, "read dispatched exclusively to dev1");
    CHECK(m.read_ops[1] == 1, "read_ops[1] incremented");
    CHECK(m.read_ops[0] == 1, "read_ops[0] remained 1");
}

static void test_resilient_read_failover(void) {
    printf("--- Unit: Resilient Read Failover ---\n");
    mock_dev_t md0, md1;
    rwy_block_dev_t b0, b1, wrapped;
    rwy_mirror_dev_t m;

    mock_dev_init(&md0, 0);
    mock_dev_init(&md1, 1);
    setup_mock_blk(&b0, &md0);
    setup_mock_blk(&b1, &md1);

    const char replica_data[] = "HEALTHY-REPLICA-CONTENT-1234";
    memcpy(md1.data + 2048, replica_data, sizeof(replica_data));

    rwy_mirror_init(&m);
    rwy_mirror_add(&m, &b0);
    rwy_mirror_add(&m, &b1);
    rwy_mirror_wrap(&wrapped, &m);

    /* Inject read failure into primary (dev0) */
    md0.fail_read = 1;

    char buf[64];
    memset(buf, 0, sizeof(buf));
    reset_op_log();

    int rc = wrapped.read(wrapped.ctx, 2048, buf, sizeof(replica_data));
    CHECK(rc == 0, "read succeeded despite primary read failure");
    CHECK(memcmp(buf, replica_data, sizeof(replica_data)) == 0, "read payload recovered from replica dev1");

    CHECK(g_op_count == 2, "2 read operations attempted (dev0 then dev1)");
    CHECK(g_op_log[0] == 0, "primary dev0 attempted first");
    CHECK(g_op_log[1] == 1, "replica dev1 attempted second upon failure");

    CHECK(m.read_ops[0] == 1, "read_ops[0] is 1");
    CHECK(m.read_errors[0] == 1, "read_errors[0] recorded 1 failure");
    CHECK(m.read_ops[1] == 1, "read_ops[1] is 1");
    CHECK(m.read_errors[1] == 0, "read_errors[1] is 0 (succeeded)");

    /* If replica also fails, read must report error */
    md1.fail_read = 1;
    reset_op_log();
    rc = wrapped.read(wrapped.ctx, 2048, buf, sizeof(replica_data));
    CHECK(rc != 0, "read returns error when all replicas fail");
    CHECK(m.read_errors[0] == 2, "read_errors[0] recorded second failure");
    CHECK(m.read_errors[1] == 1, "read_errors[1] recorded failure");
}

static void test_cascading_read_failover_3_devs(void) {
    printf("--- Unit: Cascading Read Failover (3 Replicas) ---\n");
    mock_dev_t md0, md1, md2;
    rwy_block_dev_t b0, b1, b2, wrapped;
    rwy_mirror_dev_t m;

    mock_dev_init(&md0, 0);
    mock_dev_init(&md1, 1);
    mock_dev_init(&md2, 2);
    setup_mock_blk(&b0, &md0);
    setup_mock_blk(&b1, &md1);
    setup_mock_blk(&b2, &md2);

    const char dev2_data[] = "LAST-SURVIVING-REPLICA-DATA";
    memcpy(md2.data + 512, dev2_data, sizeof(dev2_data));

    rwy_mirror_init(&m);
    rwy_mirror_add(&m, &b0);
    rwy_mirror_add(&m, &b1);
    rwy_mirror_add(&m, &b2);
    rwy_mirror_wrap(&wrapped, &m);

    /* Fail dev0 and dev1 */
    md0.fail_read = 1;
    md1.fail_read = 1;

    char buf[64];
    memset(buf, 0, sizeof(buf));
    reset_op_log();

    int rc = wrapped.read(wrapped.ctx, 512, buf, sizeof(dev2_data));
    CHECK(rc == 0, "cascading read succeeded on dev2");
    CHECK(memcmp(buf, dev2_data, sizeof(dev2_data)) == 0, "data matches dev2 contents");

    CHECK(g_op_count == 3, "all 3 devices checked in cascade");
    CHECK(g_op_log[0] == 0, "dev 0 attempted 1st");
    CHECK(g_op_log[1] == 1, "dev 1 attempted 2nd");
    CHECK(g_op_log[2] == 2, "dev 2 attempted 3rd and succeeded");

    CHECK(m.read_errors[0] == 1, "dev0 failed");
    CHECK(m.read_errors[1] == 1, "dev1 failed");
    CHECK(m.read_errors[2] == 0, "dev2 succeeded");
}

static void test_write_policies(void) {
    printf("--- Unit: Configurable Mirror Write Policies ---\n");
    mock_dev_t md0, md1;
    rwy_block_dev_t b0, b1, wrapped;
    rwy_mirror_dev_t m;

    mock_dev_init(&md0, 0);
    mock_dev_init(&md1, 1);
    setup_mock_blk(&b0, &md0);
    setup_mock_blk(&b1, &md1);

    rwy_mirror_init(&m);
    rwy_mirror_add(&m, &b0);
    rwy_mirror_add(&m, &b1);
    rwy_mirror_wrap(&wrapped, &m);

    const char buf[] = "POLICY-WRITE-TEST";
    size_t len = sizeof(buf);

    /* Policy 1: BEST_EFFORT (default). dev1 fails write -> operation still succeeds */
    m.write_policy = RWY_MIRROR_WRITE_BEST_EFFORT;
    md1.fail_write = 1;
    int rc = wrapped.write(wrapped.ctx, 0, buf, len);
    CHECK(rc == 0, "BEST_EFFORT write succeeds when dev1 fails");
    CHECK(m.write_ops[0] == 1, "dev0 write attempted");
    CHECK(m.write_ops[1] == 1, "dev1 write attempted");
    CHECK(m.write_errors[1] == 1, "dev1 write error recorded");
    CHECK(memcmp(md0.data, buf, len) == 0, "dev0 received data");

    /* When all devices fail in BEST_EFFORT -> operation must fail */
    md0.fail_write = 1;
    rc = wrapped.write(wrapped.ctx, 0, buf, len);
    CHECK(rc != 0, "BEST_EFFORT write fails when all devices fail");

    /* Policy 2: REQUIRE_ALL. dev0 restored, dev1 failing */
    md0.fail_write = 0;
    md1.fail_write = 1;
    m.write_policy = RWY_MIRROR_WRITE_REQUIRE_ALL;
    rc = wrapped.write(wrapped.ctx, 0, buf, len);
    CHECK(rc != 0, "REQUIRE_ALL write returns error when dev1 fails");

    /* Policy 3: REQUIRE_PRIMARY. dev0 fails, dev1 succeeds */
    md0.fail_write = 1;
    md1.fail_write = 0;
    m.write_policy = RWY_MIRROR_WRITE_REQUIRE_PRIMARY;
    rc = wrapped.write(wrapped.ctx, 0, buf, len);
    CHECK(rc != 0, "REQUIRE_PRIMARY write returns error when primary dev0 fails");

    /* REQUIRE_PRIMARY: dev0 succeeds, dev1 fails */
    md0.fail_write = 0;
    md1.fail_write = 1;
    rc = wrapped.write(wrapped.ctx, 0, buf, len);
    CHECK(rc == 0, "REQUIRE_PRIMARY write succeeds when primary dev0 succeeds");
}

static void test_filesystem_commit_over_mirror(const char *seed_path) {
    printf("--- Integration: Filesystem CoW Commits over Mirror Layer ---\n");
    char path0[256], path1[256];
    snprintf(path0, sizeof(path0), "%s", tpath("rwy_mirrortest_0.store"));
    snprintf(path1, sizeof(path1), "%s", tpath("rwy_mirrortest_1.store"));

    copy_file(seed_path, path0);
    copy_file(seed_path, path1);

    file_dev_t fdev0 = { fopen(path0, "r+b"), 0, 0 };
    file_dev_t fdev1 = { fopen(path1, "r+b"), 0, 0 };
    CHECK(fdev0.f != NULL, "open store 0");
    CHECK(fdev1.f != NULL, "open store 1");

    rwy_block_dev_t b0, b1, mirror_blk;
    b0.read = file_rd; b0.write = file_wr; b0.flush = file_fl;
    b0.ctx = &fdev0; b0.size = RWY_STORE_SIZE; b0.block_size = RWY_BLOCK_SIZE;

    b1.read = file_rd; b1.write = file_wr; b1.flush = file_fl;
    b1.ctx = &fdev1; b1.size = RWY_STORE_SIZE; b1.block_size = RWY_BLOCK_SIZE;

    rwy_mirror_dev_t mdev;
    rwy_mirror_init(&mdev);
    rwy_mirror_add(&mdev, &b0);
    rwy_mirror_add(&mdev, &b1);
    rwy_mirror_wrap(&mirror_blk, &mdev);

    /* Mount filesystem over mirror device */
    rwy_store_t store;
    int rc = rwy_mount(&store, &mirror_blk);
    CHECK(rc == RWY_OK, "mount over mirror block device");

    uint64_t initial_gen = store.sb.generation;
    uint8_t initial_uuid[16];
    memcpy(initial_uuid, store.sb.uuid, 16);

    /* Begin transaction and make filesystem modifications */
    rc = rwy_begin(&store);
    CHECK(rc == RWY_OK, "begin txn");

    rc = rwy_mkdir(&store, "/EFI/RAID", 0755, 0, 0);
    CHECK(rc == RWY_OK, "mkdir /EFI/RAID");

    rc = rwy_create(&store, "/EFI/RAID/TEST.TXT", 0644, 0, 0);
    CHECK(rc == RWY_OK, "create /EFI/RAID/TEST.TXT");

    /* Write 8 KiB test payload (spans 2 data blocks) */
    uint8_t test_payload[8192];
    for (size_t i = 0; i < sizeof(test_payload); i++) {
        test_payload[i] = (uint8_t)((i * 37 + 13) & 0xFF);
    }
    rc = rwy_write_path(&store, "/EFI/RAID/TEST.TXT", 0, test_payload, sizeof(test_payload));
    CHECK(rc == RWY_OK, "write 8192 bytes payload");

    /* Commit transaction (propagates CoW data, inode table, bitmap, and superblock slot) */
    rc = rwy_commit(&store);
    CHECK(rc == RWY_OK, "commit txn over mirror");
    mirror_blk.flush(mirror_blk.ctx);

    CHECK(store.sb.generation == initial_gen + 1, "generation incremented");
    CHECK(memcmp(store.sb.uuid, initial_uuid, 16) == 0, "UUID preserved");

    fclose(fdev0.f);
    fclose(fdev1.f);

    /* Verify both backing stores independently with fsck.runway */
    char cmd0[512], cmd1[512];
    snprintf(cmd0, sizeof(cmd0), "%s %s > /dev/null 2>&1", g_checkbin, path0);
    snprintf(cmd1, sizeof(cmd1), "%s %s > /dev/null 2>&1", g_checkbin, path1);
    CHECK(system(cmd0) == 0, "fsck.runway on replica 0 passed");
    CHECK(system(cmd1) == 0, "fsck.runway on replica 1 passed");

    /* Open store 0 directly as standalone filesystem */
    fdev0.f = fopen(path0, "rb");
    rwy_store_t s0;
    rc = rwy_mount(&s0, &b0);
    CHECK(rc == RWY_OK, "mount standalone store 0");
    CHECK(s0.sb.generation == initial_gen + 1, "store 0 gen matches committed gen");
    CHECK(memcmp(s0.sb.uuid, initial_uuid, 16) == 0, "store 0 UUID matches");

    uint8_t readback0[8192];
    size_t nr0 = sizeof(readback0);
    rc = rwy_read_path(&s0, "/EFI/RAID/TEST.TXT", 0, readback0, &nr0);
    CHECK(rc == RWY_OK && nr0 == sizeof(test_payload), "read from store 0");
    CHECK(memcmp(readback0, test_payload, sizeof(test_payload)) == 0, "store 0 content verified");
    fclose(fdev0.f);

    /* Open store 1 directly as standalone filesystem */
    fdev1.f = fopen(path1, "rb");
    rwy_store_t s1;
    rc = rwy_mount(&s1, &b1);
    CHECK(rc == RWY_OK, "mount standalone store 1");
    CHECK(s1.sb.generation == initial_gen + 1, "store 1 gen matches committed gen");
    CHECK(memcmp(s1.sb.uuid, initial_uuid, 16) == 0, "store 1 UUID matches");

    uint8_t readback1[8192];
    size_t nr1 = sizeof(readback1);
    rc = rwy_read_path(&s1, "/EFI/RAID/TEST.TXT", 0, readback1, &nr1);
    CHECK(rc == RWY_OK && nr1 == sizeof(test_payload), "read from store 1");
    CHECK(memcmp(readback1, test_payload, sizeof(test_payload)) == 0, "store 1 content verified");
    fclose(fdev1.f);

    unlink(path0);
    unlink(path1);
}

static void test_filesystem_read_failover(const char *seed_path) {
    printf("--- Integration: Filesystem Read Failover under Storage Errors ---\n");
    char path0[256], path1[256];
    snprintf(path0, sizeof(path0), "%s", tpath("rwy_mirrortest_fo0_.store"));
    snprintf(path1, sizeof(path1), "%s", tpath("rwy_mirrortest_fo1_.store"));

    copy_file(seed_path, path0);
    copy_file(seed_path, path1);

    file_dev_t fdev0 = { fopen(path0, "r+b"), 0, 0 };
    file_dev_t fdev1 = { fopen(path1, "r+b"), 0, 0 };
    CHECK(fdev0.f != NULL, "open store 0");
    CHECK(fdev1.f != NULL, "open store 1");

    rwy_block_dev_t b0, b1, mirror_blk;
    b0.read = file_rd; b0.write = file_wr; b0.flush = file_fl;
    b0.ctx = &fdev0; b0.size = RWY_STORE_SIZE; b0.block_size = RWY_BLOCK_SIZE;

    b1.read = file_rd; b1.write = file_wr; b1.flush = file_fl;
    b1.ctx = &fdev1; b1.size = RWY_STORE_SIZE; b1.block_size = RWY_BLOCK_SIZE;

    rwy_mirror_dev_t mdev;
    rwy_mirror_init(&mdev);
    rwy_mirror_add(&mdev, &b0);
    rwy_mirror_add(&mdev, &b1);
    rwy_mirror_wrap(&mirror_blk, &mdev);

    /* Mount through mirror device */
    rwy_store_t store;
    int rc = rwy_mount(&store, &mirror_blk);
    CHECK(rc == RWY_OK, "mount over mirror block device");

    /* Induce read errors on primary store 0 */
    fdev0.fail_read = 1;

    /* Read a known seed file (e.g. /README.TXT) */
    char buf[256];
    size_t nr = sizeof(buf) - 1;
    rc = rwy_read_path(&store, "/README.TXT", 0, buf, &nr);
    CHECK(rc == RWY_OK, "rwy_read_path succeeded despite primary store I/O error");
    CHECK(nr > 0, "bytes read > 0");
    buf[nr] = 0;
    CHECK(strstr(buf, "Runway store") != NULL, "content verified via replica failover");
    CHECK(mdev.read_errors[0] > 0, "read errors recorded on store 0");
    CHECK(mdev.read_ops[1] > 0, "replica store 1 serviced reads");

    fclose(fdev0.f);
    fclose(fdev1.f);
    unlink(path0);
    unlink(path1);
}

/* The state record is written in place and does not change the tree generation: members
 * of the same generation can hold different records. The newest sequence must win. */
static void rec_pay(uint8_t *p, int seed) {
    for (size_t i = 0; i < RWY_REC_PAYLOAD; i++) p[i] = (uint8_t)(i * 7 + seed);
}

static int rec_has(rwy_store_t *s, int seed) {
    uint8_t got[RWY_REC_PAYLOAD], want[RWY_REC_PAYLOAD];
    rec_pay(want, seed);
    return rwy_record_read(s, got) == RWY_OK && !memcmp(got, want, sizeof(got));
}

static int rec_put(const char *path, int seed) {   /* a standalone record write */
    file_dev_t fl = { fopen(path, "r+b"), 0, 0 };
    rwy_block_dev_t d = { file_rd, file_wr, file_fl, &fl, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    rwy_store_t s;
    uint8_t p[RWY_REC_PAYLOAD];
    int rc = fl.f ? rwy_mount(&s, &d) : -1;
    if (!rc) { rec_pay(p, seed); rc = rwy_record_write(&s, p); }
    if (fl.f) fclose(fl.f);
    return rc;
}

static int rec_on(const char *path, int seed) {   /* the record a standalone mount sees */
    file_dev_t fl = { fopen(path, "rb"), 0, 0 };
    rwy_block_dev_t d = { file_rd, file_wr, file_fl, &fl, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    rwy_store_t s;
    int ok = fl.f && rwy_mount(&s, &d) == RWY_OK && rec_has(&s, seed);
    if (fl.f) fclose(fl.f);
    return ok;
}

static void rec_flip_blk(const char *path, uint32_t blk) {
    FILE *f = fopen(path, "r+b");
    long off = (long)blk * RWY_BLOCK_SIZE + 100;
    int c;
    if (!f || fseek(f, off, SEEK_SET)) exit(1);
    c = fgetc(f);
    if (fseek(f, off, SEEK_SET) || fputc(c ^ 0xff, f) == EOF) exit(1);
    fclose(f);
}

static void test_record_across_members(const char *seed_path) {
    const char *p0 = tpath("rwy-mirrortest-rec0.store"), *p1 = tpath("rwy-mirrortest-rec1.store");
    file_dev_t f0, f1;
    rwy_block_dev_t m[2];
    rwy_mirror_dev_t md;
    rwy_block_dev_t mirror_blk;
    rwy_store_t s;
    uint32_t rs;
    size_t newest = 9;
    printf("--- Integration: State Record across Mirror Members ---\n");
    copy_file(seed_path, p0);
    copy_file(seed_path, p1);
    f0.f = fopen(p0, "r+b"); f0.fail_read = f0.fail_write = 0;
    f1.f = fopen(p1, "r+b"); f1.fail_read = f1.fail_write = 0;
    CHECK(f0.f && f1.f, "open both members");
    m[0] = (rwy_block_dev_t){ file_rd, file_wr, file_fl, &f0, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    m[1] = (rwy_block_dev_t){ file_rd, file_wr, file_fl, &f1, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    CHECK(rwy_mount(&s, &m[0]) == RWY_OK, "mount member 0");
    rs = s.sb.rec_start;
    fclose(f0.f); fclose(f1.f);

    /* Same generation, member 1 holds a newer record. */
    CHECK(rec_put(p0, 1) == RWY_OK && rec_put(p1, 1) == RWY_OK && rec_put(p1, 2) == RWY_OK, "write the records");
    f0.f = fopen(p0, "r+b"); f1.f = fopen(p1, "r+b");
    rwy_mirror_init(&md);
    rwy_mirror_add(&md, &m[0]); rwy_mirror_add(&md, &m[1]);
    rwy_mirror_wrap(&mirror_blk, &md);
    CHECK(rwy_mount(&s, &mirror_blk) == RWY_OK && rec_has(&s, 1), "a mirror mount reads the primary's record");
    CHECK(rwy_record_sync(m, 2, rs, 0, &newest) == RWY_OK && newest == 1, "member 1 holds the newest record");
    CHECK(rwy_record_adopt(&s, &m[1]) == RWY_OK && rec_has(&s, 2), "the store takes that record");
    fclose(f0.f); fclose(f1.f);
    CHECK(rec_on(p0, 1), "without heal member 0 is unchanged");

    /* Heal: the stale member gets the record, in its copy that is not current. */
    f0.f = fopen(p0, "r+b"); f1.f = fopen(p1, "r+b");
    CHECK(rwy_record_sync(m, 2, rs, 1, &newest) == RWY_OK && newest == 1, "heal");
    fclose(f0.f); fclose(f1.f);
    CHECK(rec_on(p0, 2) && rec_on(p1, 2), "both members hold the newest record");
    CHECK(rec_put(p0, 3) == RWY_OK && rec_on(p0, 3), "a write after the heal works");

    /* Members in step: nothing is written. Newest copy damaged: the older valid copy counts. */
    CHECK(rec_put(p1, 3) == RWY_OK, "bring member 1 level");
    f0.f = fopen(p0, "r+b"); f1.f = fopen(p1, "r+b");
    CHECK(rwy_record_sync(m, 2, rs, 1, &newest) == RWY_OK && newest == 0, "equal records: the first member");
    fclose(f0.f); fclose(f1.f);
    CHECK(rec_put(p1, 4) == RWY_OK, "member 1 moves ahead");
    rec_flip_blk(p1, rs); rec_flip_blk(p1, rs + 1);
    f0.f = fopen(p0, "r+b"); f1.f = fopen(p1, "r+b");
    CHECK(rwy_record_sync(m, 2, rs, 1, &newest) == RWY_OK && newest == 0, "a member with no valid copy is not the newest");
    fclose(f0.f); fclose(f1.f);
    CHECK(rec_on(p1, 3), "and it is healed from the other member");

    /* A failing member does not stop the others. */
    CHECK(rec_put(p0, 5) == RWY_OK, "member 0 moves ahead");
    f0.f = fopen(p0, "r+b"); f1.f = fopen(p1, "r+b");
    f1.fail_write = 1;
    CHECK(rwy_record_sync(m, 2, rs, 1, &newest) == RWY_OK && newest == 0, "a member that cannot be written is skipped");
    f1.fail_write = 0;
    fclose(f0.f); fclose(f1.f);
    CHECK(rec_on(p0, 5), "the newest record is untouched");
    unlink(p0);
    unlink(p1);
}

/* Members hold different health tables: the newest record wins, and no member's counter is lost. */
static int note_on(const char *path, const uint8_t uuid[16], int event) {
    file_dev_t fl = { fopen(path, "r+b"), 0, 0 };
    rwy_block_dev_t d = { file_rd, file_wr, file_fl, &fl, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    rwy_store_t s;
    uint8_t p[RWY_REC_PAYLOAD];
    int rc = fl.f ? rwy_mount(&s, &d) : -1;
    if (!rc) rc = rwy_record_read(&s, p);
    if (!rc) rc = rwy_ss_health_note(p, uuid, event, s.rec_seq + 1);
    if (!rc) rc = rwy_record_write(&s, p);
    if (fl.f) fclose(fl.f);
    return rc;
}

static void test_health_merge(const char *seed_path) {
    const char *p0 = tpath("rwy-mirrortest-hm0.store"), *p1 = tpath("rwy-mirrortest-hm1.store");
    const uint8_t ua[16] = { 0xa1 }, ub[16] = { 0xb2 };
    file_dev_t f0, f1;
    rwy_block_dev_t m[2];
    rwy_store_t s;
    rwy_ss_health_t h;
    uint8_t rec[RWY_REC_PAYLOAD];
    size_t newest = 9;
    printf("--- Integration: Health Counters across Mirror Members ---\n");
    copy_file(seed_path, p0);
    copy_file(seed_path, p1);
    CHECK(note_on(p0, ua, RWY_SS_EV_LAGGING) == RWY_OK, "member 0 saw an event");
    CHECK(note_on(p1, ub, RWY_SS_EV_MISSING) == RWY_OK && note_on(p1, ub, RWY_SS_EV_MISSING) == RWY_OK, "member 1 saw two others");
    f0.f = fopen(p0, "r+b"); f0.fail_read = f0.fail_write = 0;
    f1.f = fopen(p1, "r+b"); f1.fail_read = f1.fail_write = 0;
    m[0] = (rwy_block_dev_t){ file_rd, file_wr, file_fl, &f0, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    m[1] = (rwy_block_dev_t){ file_rd, file_wr, file_fl, &f1, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    CHECK(rwy_mount(&s, &m[0]) == RWY_OK, "mount member 0");
    CHECK(rwy_record_sync(m, 2, s.sb.rec_start, 0, &newest) == RWY_OK && newest == 1, "member 1 is the newest");
    CHECK(rwy_record_adopt(&s, &m[1]) == RWY_OK, "the store takes it");
    rwy_record_read(&s, rec);
    CHECK(!rwy_ss_health_get(rec, ua, &h), "the newest record alone lacks the event of member 0");
    CHECK(rwy_record_merge_health(&s, m, 2) == RWY_OK, "merge");
    rwy_record_read(&s, rec);
    CHECK(rwy_ss_health_get(rec, ua, &h) && h.count[RWY_SS_EV_LAGGING] == 1, "the event of member 0 is kept");
    CHECK(rwy_ss_health_get(rec, ub, &h) && h.count[RWY_SS_EV_MISSING] == 2, "and those of member 1");
    CHECK(rwy_record_merge_health(&s, m, 2) == RWY_OK, "merge again");
    rwy_record_read(&s, rec);
    CHECK(rwy_ss_health_get(rec, ub, &h) && h.count[RWY_SS_EV_MISSING] == 2, "a second merge does not add up");
    f1.fail_read = 1;
    CHECK(rwy_record_merge_health(&s, m, 2) == RWY_OK, "a member that cannot be read is skipped");
    f1.fail_read = 0;
    {
        uint8_t u[16];
        unsigned i, used = 0;
        for (i = 0; i < RWY_MIRROR_MAX_DEVS; i++) used += rwy_ss_health_at(rec, i, u, &h);
        CHECK(used == 2, "the table lists the two members");
        CHECK(rwy_ss_health_at(rec, RWY_MIRROR_MAX_DEVS, u, &h) == 0, "an index past the table is empty");
        CHECK(!strcmp(rwy_ss_event_name(RWY_SS_EV_MISSING), "missing") && !strcmp(rwy_ss_event_name(99), "none"), "event names");
    }
    CHECK(rwy_record_clear_health(&s) == RWY_OK, "clear");
    rwy_record_read(&s, rec);
    CHECK(!rwy_ss_health_get(rec, ua, &h) && !rwy_ss_health_get(rec, ub, &h), "the counters are gone");
    fclose(f0.f); fclose(f1.f);
    unlink(p0);
    unlink(p1);
}

/* The roster is written once, merged across members, and only forget removes an entry. */
static void test_roster(const char *seed_path) {
    const char *p0 = tpath("rwy-mirrortest-ro0.store"), *p1 = tpath("rwy-mirrortest-ro1.store");
    const uint8_t u2[2][16] = { { 0xa1 }, { 0xb2 } };
    file_dev_t f0 = { 0 }, f1 = { 0 };
    rwy_block_dev_t m[2];
    rwy_store_t s;
    uint8_t rec[RWY_REC_PAYLOAD];
    uint64_t seq;
    printf("--- Integration: Member Roster ---\n");
    copy_file(seed_path, p0);
    copy_file(seed_path, p1);
    f0.f = fopen(p0, "r+b");
    f1.f = fopen(p1, "r+b");
    m[0] = (rwy_block_dev_t){ file_rd, file_wr, file_fl, &f0, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    m[1] = (rwy_block_dev_t){ file_rd, file_wr, file_fl, &f1, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    CHECK(rwy_mount(&s, &m[0]) == RWY_OK, "mount member 0");
    seq = s.rec_seq;
    CHECK(rwy_record_note_roster(&s, m, 2, u2, 1) == RWY_OK && s.rec_seq == seq, "a single member writes no roster");
    CHECK(rwy_record_note_roster(&s, m, 2, u2, 2) == RWY_OK && s.rec_seq == seq + 1, "two members are listed with one write");
    rwy_record_read(&s, rec);
    CHECK(rwy_ss_roster_has(rec, u2[0]) && rwy_ss_roster_has(rec, u2[1]), "both are in the roster");
    CHECK(rwy_record_note_roster(&s, m, 2, u2, 2) == RWY_OK && s.rec_seq == seq + 1, "a stable mirror writes nothing");
    CHECK(rwy_record_forget_member(&s, u2[1]) == RWY_OK, "forget a member");
    rwy_record_read(&s, rec);
    CHECK(rwy_ss_roster_has(rec, u2[0]) && !rwy_ss_roster_has(rec, u2[1]), "only that member is gone");
    CHECK(rwy_record_forget_member(&s, u2[1]) == RWY_ERR_NOENT, "forgetting it twice is an error");
    fclose(f0.f); fclose(f1.f);
    unlink(p0);
    unlink(p1);
}

/* ========================================================================= */
/*                               Main Driver                                 */
/* ========================================================================= */

/* ========================================================================= */
/*                          Per-member superblock UUID                       */
/* ========================================================================= */

static int test_random(void *buf, size_t len) {
    static uint8_t next = 0x40;
    memset(buf, next++, len);
    return 0;
}

/* The member UUID of the newest valid superblock slot of a store file. */
static int member_uuid_of(const char *path, uint8_t out[16]) {
    FILE *f = fopen(path, "rb");
    uint64_t best = 0;
    int found = 0;
    for (int slot = 0; f && slot < 2; slot++) {
        rwy_superblock_t sb;
        if (fseek(f, (long)slot * RWY_BLOCK_SIZE, SEEK_SET) || fread(&sb, sizeof(sb), 1, f) != 1) continue;
        if (rwy_validate_sb(&sb) != RWY_OK || sb.generation < best) continue;
        best = sb.generation;
        memcpy(out, sb.member_uuid, 16);
        found = 1;
    }
    if (f) fclose(f);
    return found;
}

static void zero_member_uuid(const char *path) {
    FILE *f = fopen(path, "r+b");
    for (int slot = 0; f && slot < 2; slot++) {
        rwy_superblock_t sb;
        if (fseek(f, (long)slot * RWY_BLOCK_SIZE, SEEK_SET) || fread(&sb, sizeof(sb), 1, f) != 1) continue;
        memset(sb.member_uuid, 0, 16);
        sb.crc32c = rwy_crc32c(&sb, offsetof(rwy_superblock_t, crc32c));
        fseek(f, (long)slot * RWY_BLOCK_SIZE, SEEK_SET);
        if (fwrite(&sb, sizeof(sb), 1, f) != 1) { /* ignore or check */ }
    }
    if (f) fclose(f);
}

static void test_member_uuids(const char *seed_path) {
    printf("--- Per-member superblock UUID ---\n");
    char path0[256], path1[256];
    uint8_t a0[16], a1[16], b0[16], b1[16], zero[16] = {0};
    snprintf(path0, sizeof(path0), "%s", tpath("rwy_mirrortest_mu0.store"));
    snprintf(path1, sizeof(path1), "%s", tpath("rwy_mirrortest_mu1.store"));
    copy_file(seed_path, path0);
    copy_file(seed_path, path1);

    file_dev_t fd0 = { fopen(path0, "r+b"), 0, 0 }, fd1 = { fopen(path1, "r+b"), 0, 0 };
    rwy_block_dev_t d0 = { file_rd, file_wr, file_fl, &fd0, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    rwy_block_dev_t d1 = { file_rd, file_wr, file_fl, &fd1, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    rwy_block_dev_t blk;
    rwy_mirror_dev_t mdev;
    rwy_store_t store;
    rwy_mirror_init(&mdev);
    rwy_mirror_add(&mdev, &d0);
    rwy_mirror_add(&mdev, &d1);
    rwy_mirror_wrap(&blk, &mdev);

    CHECK(rwy_mirror_name_members(&mdev, test_random) == 0, "name the members of a copied pair");
    fflush(fd0.f); fflush(fd1.f);
    CHECK(member_uuid_of(path0, a0) && member_uuid_of(path1, a1), "both members have a valid superblock");
    CHECK(memcmp(a0, zero, 16) && memcmp(a1, zero, 16), "both members have a UUID");
    CHECK(memcmp(a0, a1, 16) != 0, "a copied pair gets two different UUIDs");

    uint64_t ops = mdev.write_ops[0] + mdev.write_ops[1];
    CHECK(rwy_mirror_name_members(&mdev, test_random) == 0, "name them again");
    CHECK(mdev.write_ops[0] + mdev.write_ops[1] == ops, "named members are not written again");

    CHECK(rwy_mount(&store, &blk) == RWY_OK, "mount over the named pair");
    CHECK(rwy_begin(&store) == RWY_OK && rwy_mkdir(&store, "/EFI/MU", 0755, 0, 0) == RWY_OK &&
          rwy_commit(&store) == RWY_OK, "commit over the named pair");
    fflush(fd0.f); fflush(fd1.f);
    CHECK(member_uuid_of(path0, b0) && member_uuid_of(path1, b1), "superblocks valid after the commit");
    CHECK(!memcmp(a0, b0, 16) && !memcmp(a1, b1, 16), "a commit keeps each member's own UUID");

    zero_member_uuid(path1);
    CHECK(rwy_mirror_name_members(&mdev, test_random) == 0, "name a member that has none");
    fflush(fd0.f); fflush(fd1.f);
    CHECK(member_uuid_of(path0, b0) && !memcmp(a0, b0, 16), "the member that had a UUID keeps it");
    CHECK(member_uuid_of(path1, b1) && memcmp(b1, zero, 16) && memcmp(b1, b0, 16), "the other one gets its own");

    fclose(fd0.f);
    fclose(fd1.f);
    unlink(path0);
    unlink(path1);
}

/* A member that fails a record write is counted once; a healthy mirror writes no health record. */
static void test_health_failures(const char *seed_path) {
    const char *p0 = tpath("rwy-mirrortest-hf0.store"), *p1 = tpath("rwy-mirrortest-hf1.store");
    file_dev_t f0 = { NULL, 0, 0 }, f1 = { NULL, 0, 0 };
    rwy_block_dev_t d[2], blk;
    rwy_mirror_dev_t md;
    rwy_store_t s;
    rwy_ss_health_t h;
    uint8_t u0[16], u1[16], rec[RWY_REC_PAYLOAD];
    uint64_t seq;
    printf("--- Health Events of a Failing Member ---\n");
    copy_file(seed_path, p0);
    copy_file(seed_path, p1);
    f0.f = fopen(p0, "r+b");
    f1.f = fopen(p1, "r+b");
    d[0] = (rwy_block_dev_t){ file_rd, file_wr, file_fl, &f0, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    d[1] = (rwy_block_dev_t){ file_rd, file_wr, file_fl, &f1, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    rwy_mirror_init(&md);
    rwy_mirror_add(&md, &d[0]);
    rwy_mirror_add(&md, &d[1]);
    rwy_mirror_wrap(&blk, &md);
    CHECK(rwy_mirror_name_members(&md, test_random) == 0, "name the members");
    CHECK(rwy_mount(&s, &blk) == RWY_OK, "mount the pair");
    CHECK(rwy_block_member_uuid(&d[0], u0) && rwy_block_member_uuid(&d[1], u1), "read both UUIDs");

    seq = s.rec_seq;
    CHECK(rwy_mirror_note_failures(&s, &md, d, 2) == RWY_OK && s.rec_seq == seq, "a healthy mirror writes no record");

    f1.fail_write = 1;
    CHECK(rwy_record_note_health(&s, d, 2, u0, RWY_SS_EV_LAGGING) == RWY_OK, "a record write with a failing member");
    f1.fail_write = 0;
    CHECK(rwy_mirror_note_failures(&s, &md, d, 2) == RWY_OK, "note the failure");
    rwy_record_read(&s, rec);
    CHECK(rwy_ss_health_get(rec, u1, &h) && h.count[RWY_SS_EV_WRITE_FAILED] == 1, "the failing member is counted");
    CHECK(!rwy_ss_health_get(rec, u0, &h) || h.count[RWY_SS_EV_WRITE_FAILED] == 0, "the healthy one is not");
    seq = s.rec_seq;
    CHECK(rwy_mirror_note_failures(&s, &md, d, 2) == RWY_OK && s.rec_seq == seq, "the same failure is not counted twice");

    rwy_mirror_set_faulted(&md, 1, 1);
    CHECK(rwy_record_note_health(&s, d, 2, u0, RWY_SS_EV_LAGGING) == RWY_OK, "a record write skips a faulted member");
    CHECK(rwy_mirror_note_failures(&s, &md, d, 2) == RWY_OK, "note again");
    rwy_record_read(&s, rec);
    CHECK(rwy_ss_health_get(rec, u1, &h) && h.count[RWY_SS_EV_WRITE_FAILED] == 1, "a faulted member is not counted");
    fclose(f0.f);
    fclose(f1.f);
    unlink(p0);
    unlink(p1);
}

/* A mount lists its members once, counts a roster member that is gone, and stops after a forget. */
static void test_members(const char *seed_path) {
    const char *p0 = tpath("rwy-mirrortest-mb0.store"), *p1 = tpath("rwy-mirrortest-mb1.store");
    file_dev_t f0 = { 0 }, f1 = { 0 };
    rwy_block_dev_t d[2], blk;
    rwy_mirror_dev_t pair, alone;
    rwy_store_t s;
    rwy_ss_health_t h;
    uint8_t u0[16], u1[16], rec[RWY_REC_PAYLOAD];
    uint64_t seq;
    printf("--- Integration: Roster Check at Mount ---\n");
    copy_file(seed_path, p0);
    copy_file(seed_path, p1);
    f0.f = fopen(p0, "r+b");
    f1.f = fopen(p1, "r+b");
    d[0] = (rwy_block_dev_t){ file_rd, file_wr, file_fl, &f0, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    d[1] = (rwy_block_dev_t){ file_rd, file_wr, file_fl, &f1, RWY_STORE_SIZE, RWY_BLOCK_SIZE, NULL, NULL };
    rwy_mirror_init(&pair);
    rwy_mirror_add(&pair, &d[0]);
    rwy_mirror_add(&pair, &d[1]);
    rwy_mirror_wrap(&blk, &pair);
    CHECK(rwy_mirror_name_members(&pair, test_random) == 0, "name the members");
    CHECK(rwy_mount(&s, &blk) == RWY_OK, "mount the pair");
    CHECK(rwy_block_member_uuid(&d[0], u0) && rwy_block_member_uuid(&d[1], u1), "read both UUIDs");

    seq = s.rec_seq;
    CHECK(rwy_mirror_note_members(&s, &pair, d, 2) == RWY_OK && s.rec_seq == seq + 1, "the pair is listed with one write");
    rwy_record_read(&s, rec);
    CHECK(rwy_ss_roster_has(rec, u0) && rwy_ss_roster_has(rec, u1), "both are in the roster");
    CHECK(!rwy_ss_health_get(rec, u0, &h) && !rwy_ss_health_get(rec, u1, &h), "nothing is counted");
    CHECK(rwy_mirror_note_members(&s, &pair, d, 2) == RWY_OK && s.rec_seq == seq + 1, "a stable mirror writes nothing");

    rwy_mirror_init(&alone);
    rwy_mirror_add(&alone, &d[0]);
    CHECK(rwy_mirror_note_members(&s, &alone, d, 1) == RWY_OK, "mount with one member gone");
    rwy_record_read(&s, rec);
    CHECK(rwy_ss_health_get(rec, u1, &h) && h.count[RWY_SS_EV_MISSING] == 1 && h.last_event == RWY_SS_EV_MISSING,
          "the missing member is counted");
    CHECK(!rwy_ss_health_get(rec, u0, &h) || h.count[RWY_SS_EV_MISSING] == 0, "the present one is not");
    CHECK(rwy_mirror_note_members(&s, &alone, d, 1) == RWY_OK, "mount again");
    rwy_record_read(&s, rec);
    CHECK(rwy_ss_health_get(rec, u1, &h) && h.count[RWY_SS_EV_MISSING] == 2, "counted at every mount");

    CHECK(rwy_record_forget_member(&s, u1) == RWY_OK, "forget it");
    seq = s.rec_seq;
    CHECK(rwy_mirror_note_members(&s, &alone, d, 1) == RWY_OK && s.rec_seq == seq, "a forgotten member is not missing");
    fclose(f0.f); fclose(f1.f);
    unlink(p0);
    unlink(p1);
}

int main(int argc, char **argv) {
    const char *seed = (argc >= 2) ? argv[1] : "build/RUNWAY.DAT";
    if (argc >= 3) g_checkbin = argv[2];

    printf("=================================================================\n");
    printf("         Runway Multi-Device Mirror Block Layer Test Suite         \n");
    printf("=================================================================\n");

    test_mirror_init_and_registration();
    test_sequential_write_propagation();
    test_sequential_flush_propagation();
    test_read_routing_primary();
    test_resilient_read_failover();
    test_cascading_read_failover_3_devs();
    test_write_policies();
    test_filesystem_commit_over_mirror(seed);
    test_filesystem_read_failover(seed);
    test_record_across_members(seed);
    test_health_merge(seed);
    test_roster(seed);
    test_members(seed);
    test_member_uuids(seed);
    test_health_failures(seed);

    printf("=================================================================\n");
    if (failures == 0) {
        printf("MIRRORTEST: PASS (all tests passed with 0 failures)\n");
        return 0;
    } else {
        printf("MIRRORTEST: FAIL (%d failures)\n", failures);
        return 1;
    }
}
