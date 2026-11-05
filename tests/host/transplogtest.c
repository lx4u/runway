/*
 * transplogtest.c — Update journal unit tests: rolling SHA-256 chain, tamper
 * detection, epoch binding, label copy, wrap-around, and dual atomicity.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_crypto.h"
#include "rwy_format.h"
#include "rwy_internal.h"
#include "rwy_transplog.h"

#define BASE_BLOCK   4U
#define COPY_BLOCKS  2U
#define TOTAL_BLOCKS (BASE_BLOCK + 2 * COPY_BLOCKS + 4)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

typedef struct {
    uint8_t mem[TOTAL_BLOCKS * RWY_BLOCK_SIZE];
    int writes;
    int cut_after;
    int flushes;
    int torn;
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
    if (m->cut_after >= 0 && m->writes >= m->cut_after) {
        if (m->torn > 0) memcpy(m->mem + off, buf, (size_t)m->torn);
        return -1;
    }
    memcpy(m->mem + off, buf, len);
    m->writes++;
    return 0;
}

static int mem_flush(void *ctx) {
    ((mem_t *)ctx)->flushes++;
    return 0;
}

static void mem_init(mem_t *m, rwy_block_dev_t *d) {
    memset(m, 0, sizeof(*m));
    m->cut_after = -1;
    d->read = mem_read;
    d->write = mem_write;
    d->flush = mem_flush;
    d->ctx = m;
    d->size = sizeof(m->mem);
    d->block_size = RWY_BLOCK_SIZE;
    d->stats = NULL;
    d->dircache = NULL;
}

static void test_sizes_and_capacity(void) {
    CHECK(sizeof(rwy_journal_header_t) == 256, "journal header size 256");
    CHECK(sizeof(rwy_journal_entry_t) == 384, "journal entry size 384");
    CHECK(rwy_transplog_capacity(0) == 0, "capacity 0 for 0 blocks");
    CHECK(rwy_transplog_capacity(1) == 10, "capacity 10 for 1 block");
    CHECK(rwy_transplog_capacity(2) == 20, "capacity 20 for 2 blocks");
    CHECK(rwy_transplog_capacity(32) == 320, "capacity 320 for 32 blocks");
}

static void test_empty_area(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_transplog_info_t info;
    int rc;

    mem_init(&m, &d);
    rc = rwy_transplog_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info);
    CHECK(rc == RWY_ERR_CORRUPT, "pick on empty returns CORRUPT");
    CHECK(info.status[0] == RWY_TRANSPLOG_EMPTY, "copy A empty");
    CHECK(info.status[1] == RWY_TRANSPLOG_EMPTY, "copy B empty");
}

static void test_first_append(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_transplog_info_t info;
    rwy_journal_entry_t e, out;
    int rc;

    mem_init(&m, &d);
    memset(&e, 0, sizeof(e));
    e.event = RWY_JOURNAL_EVENT_INSTALL;
    e.generation = 2;
    e.rec_seq = 1;
    e.revision = 1;
    strcpy(e.slot, "CONFIG1");
    strcpy(e.label, "v1.0.0");
    strcpy(e.caller, "rwyctl");

    rc = rwy_transplog_append(&d, BASE_BLOCK, COPY_BLOCKS, &e);
    CHECK(rc == RWY_OK, "first append succeeds");

    rc = rwy_transplog_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info);
    CHECK(rc == RWY_OK, "pick succeeds after append");
    CHECK(info.copy == 0, "winning copy is 0");
    CHECK(info.sequence == 1, "sequence is 1");
    CHECK(info.count == 1, "count is 1");
    CHECK(info.head == 1, "head is 1");

    rc = rwy_transplog_read_entry(&d, BASE_BLOCK, COPY_BLOCKS, &info, 0, &out);
    CHECK(rc == RWY_OK, "read_entry succeeds");
    CHECK(out.seq == 1, "entry seq is 1");
    CHECK(out.event == RWY_JOURNAL_EVENT_INSTALL, "event matches");
    CHECK(strcmp(out.slot, "CONFIG1") == 0, "slot matches");
    CHECK(strcmp(out.label, "v1.0.0") == 0, "label matches");
    CHECK(memcmp(info.head_hash, out.entry_hash, 32) == 0, "head_hash matches entry_hash");
}

static void test_hash_chain_and_verify(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_transplog_info_t info;
    rwy_journal_entry_t e1, e2, out1, out2;
    int rc;

    mem_init(&m, &d);
    memset(&e1, 0, sizeof(e1));
    e1.event = RWY_JOURNAL_EVENT_INSTALL;
    strcpy(e1.slot, "CONFIG1");
    strcpy(e1.label, "v1.0");
    e1.epoch = 1;
    rc = rwy_transplog_append(&d, BASE_BLOCK, COPY_BLOCKS, &e1);
    CHECK(rc == RWY_OK, "append e1 succeeds");

    memset(&e2, 0, sizeof(e2));
    e2.event = RWY_JOURNAL_EVENT_UPDATE;
    strcpy(e2.slot, "CONFIG2");
    strcpy(e2.label, "v2.0");
    e2.epoch = 2;
    rc = rwy_transplog_append(&d, BASE_BLOCK, COPY_BLOCKS, &e2);
    CHECK(rc == RWY_OK, "append e2 succeeds");

    rc = rwy_transplog_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info);
    CHECK(rc == RWY_OK, "pick succeeds after two appends");
    CHECK(info.copy == 1, "winning copy is 1 (ping-pong)");
    CHECK(info.sequence == 2, "sequence bumped to 2");
    CHECK(info.count == 2, "count is 2");

    rc = rwy_transplog_read_entry(&d, BASE_BLOCK, COPY_BLOCKS, &info, 0, &out1);
    CHECK(rc == RWY_OK, "read e1 succeeds");
    rc = rwy_transplog_read_entry(&d, BASE_BLOCK, COPY_BLOCKS, &info, 1, &out2);
    CHECK(rc == RWY_OK, "read e2 succeeds");

    CHECK(out2.seq == out1.seq + 1, "seq is monotonic");
    CHECK(memcmp(out2.prev_hash, out1.entry_hash, 32) == 0, "out2.prev_hash == out1.entry_hash");
    CHECK(memcmp(info.head_hash, out2.entry_hash, 32) == 0, "head_hash == out2.entry_hash");

    rc = rwy_transplog_verify_chain(&d, BASE_BLOCK, COPY_BLOCKS, &info, 10);
    CHECK(rc == RWY_OK, "verify_chain succeeds on clean log");
}

static void test_tamper_detection(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_transplog_info_t info;
    rwy_journal_entry_t e1, e2, out;
    int rc;

    mem_init(&m, &d);
    memset(&e1, 0, sizeof(e1));
    e1.event = RWY_JOURNAL_EVENT_INSTALL;
    strcpy(e1.slot, "CONFIG1");
    rwy_transplog_append(&d, BASE_BLOCK, COPY_BLOCKS, &e1);

    memset(&e2, 0, sizeof(e2));
    e2.event = RWY_JOURNAL_EVENT_UPDATE;
    strcpy(e2.slot, "CONFIG2");
    rwy_transplog_append(&d, BASE_BLOCK, COPY_BLOCKS, &e2);

    rc = rwy_transplog_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info);
    CHECK(rc == RWY_OK, "pick ok");

    /* Flip byte in entry 1's slot name */
    size_t corrupt_off = (BASE_BLOCK + info.copy * COPY_BLOCKS) * RWY_BLOCK_SIZE + 256 + 57;
    m.mem[corrupt_off] ^= 0x55;

    rc = rwy_transplog_read_entry(&d, BASE_BLOCK, COPY_BLOCKS, &info, 0, &out);
    CHECK(rc == RWY_ERR_CORRUPT, "read_entry detects corrupt entry_hash");

    rc = rwy_transplog_verify_chain(&d, BASE_BLOCK, COPY_BLOCKS, &info, 0);
    CHECK(rc == RWY_ERR_CORRUPT, "verify_chain detects corrupt entry");

    /* Restore byte */
    m.mem[corrupt_off] ^= 0x55;
    rc = rwy_transplog_verify_chain(&d, BASE_BLOCK, COPY_BLOCKS, &info, 0);
    CHECK(rc == RWY_OK, "verify_chain succeeds after restore");
}

static void test_epoch_binding(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_transplog_info_t info;
    rwy_journal_entry_t e;
    int rc;

    mem_init(&m, &d);
    memset(&e, 0, sizeof(e));
    e.event = RWY_JOURNAL_EVENT_INSTALL;
    strcpy(e.slot, "CONFIG1");
    e.epoch = 5;
    rwy_transplog_append(&d, BASE_BLOCK, COPY_BLOCKS, &e);

    rc = rwy_transplog_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info);
    CHECK(rc == RWY_OK, "pick ok");

    rc = rwy_transplog_verify_chain(&d, BASE_BLOCK, COPY_BLOCKS, &info, 10);
    CHECK(rc == RWY_OK, "verify_chain with max_epoch 10 succeeds");

    rc = rwy_transplog_verify_chain(&d, BASE_BLOCK, COPY_BLOCKS, &info, 4);
    CHECK(rc == RWY_ERR_RANGE, "verify_chain with max_epoch 4 detects rollback");
}

static void test_ring_wrap_around(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_transplog_info_t info;
    int rc;
    uint32_t cap;

    mem_init(&m, &d);
    cap = rwy_transplog_capacity(1); /* 10 entries */
    CHECK(cap == 10, "cap is 10");

    for (int i = 1; i <= 25; i++) {
        rwy_journal_entry_t e;
        memset(&e, 0, sizeof(e));
        e.event = RWY_JOURNAL_EVENT_UPDATE;
        snprintf(e.slot, sizeof(e.slot), "CONFIG%d", i);
        snprintf(e.label, sizeof(e.label), "rev-%d", i);
        e.revision = (uint64_t)i;
        rc = rwy_transplog_append(&d, BASE_BLOCK, 1, &e);
        CHECK(rc == RWY_OK, "append in wrap test succeeds");
    }

    rc = rwy_transplog_pick(&d, BASE_BLOCK, 1, &info);
    CHECK(rc == RWY_OK, "pick ok after wrap");
    CHECK(info.count == 10, "count capped at capacity 10");
    CHECK(info.head == 5, "head wrapped around to 5");

    /* Entries stored should be 16 through 25 */
    for (uint32_t i = 0; i < 10; i++) {
        rwy_journal_entry_t out;
        rc = rwy_transplog_read_entry(&d, BASE_BLOCK, 1, &info, i, &out);
        CHECK(rc == RWY_OK, "read entry ok");
        CHECK(out.revision == (uint64_t)(16 + i), "revision matches wrapped sequence");
    }

    rc = rwy_transplog_verify_chain(&d, BASE_BLOCK, 1, &info, 0);
    CHECK(rc == RWY_OK, "hash chain intact across wrapped window");
}

static void test_dual_atomicity_filters(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_transplog_info_t info;
    rwy_journal_entry_t e1, e2, e3, out;
    uint32_t count;
    int rc;

    mem_init(&m, &d);

    /* Entry 1: gen 2, rec_seq 3 */
    memset(&e1, 0, sizeof(e1));
    e1.generation = 2;
    e1.rec_seq = 3;
    strcpy(e1.slot, "CONFIG1");
    rwy_transplog_append(&d, BASE_BLOCK, COPY_BLOCKS, &e1);

    /* Entry 2: gen 3, rec_seq 3 */
    memset(&e2, 0, sizeof(e2));
    e2.generation = 3;
    e2.rec_seq = 3;
    strcpy(e2.slot, "CONFIG2");
    rwy_transplog_append(&d, BASE_BLOCK, COPY_BLOCKS, &e2);

    /* Entry 3: gen 2, rec_seq 4 */
    memset(&e3, 0, sizeof(e3));
    e3.generation = 2;
    e3.rec_seq = 4;
    strcpy(e3.slot, "CONFIG3");
    rwy_transplog_append(&d, BASE_BLOCK, COPY_BLOCKS, &e3);

    rc = rwy_transplog_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info);
    CHECK(rc == RWY_OK, "pick ok");

    /* Both committed: max_gen 3, max_rec_seq 4 */
    count = rwy_transplog_count_committed(&d, BASE_BLOCK, COPY_BLOCKS, &info, 3, 4);
    CHECK(count == 3, "all 3 entries committed");

    /* Filter uncommitted gen: max_gen 2, max_rec_seq 4 -> e2 excluded */
    count = rwy_transplog_count_committed(&d, BASE_BLOCK, COPY_BLOCKS, &info, 2, 4);
    CHECK(count == 2, "2 entries committed with gen <= 2");
    rc = rwy_transplog_read_committed(&d, BASE_BLOCK, COPY_BLOCKS, &info, 2, 4, 1, &out);
    CHECK(rc == RWY_OK, "read committed index 1 ok");
    CHECK(strcmp(out.slot, "CONFIG3") == 0, "entry 2 was skipped");

    /* Filter uncommitted rec_seq: max_gen 3, max_rec_seq 3 -> e3 excluded */
    count = rwy_transplog_count_committed(&d, BASE_BLOCK, COPY_BLOCKS, &info, 3, 3);
    CHECK(count == 2, "2 entries committed with rec_seq <= 3");
    rc = rwy_transplog_read_committed(&d, BASE_BLOCK, COPY_BLOCKS, &info, 3, 3, 1, &out);
    CHECK(rc == RWY_OK, "read committed index 1 ok");
    CHECK(strcmp(out.slot, "CONFIG2") == 0, "entry 3 was skipped");

    /* Filter both: max_gen 2, max_rec_seq 3 -> only e1 */
    count = rwy_transplog_count_committed(&d, BASE_BLOCK, COPY_BLOCKS, &info, 2, 3);
    CHECK(count == 1, "only 1 entry committed with gen <= 2 and rec_seq <= 3");
    rc = rwy_transplog_read_committed(&d, BASE_BLOCK, COPY_BLOCKS, &info, 2, 3, 0, &out);
    CHECK(rc == RWY_OK, "read committed index 0 ok");
    CHECK(strcmp(out.slot, "CONFIG1") == 0, "entry 1 matched");
}

static void test_torn_write_recovery(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_transplog_info_t info;
    rwy_journal_entry_t e1, e2, out;
    int rc;

    mem_init(&m, &d);
    memset(&e1, 0, sizeof(e1));
    strcpy(e1.slot, "CONFIG1");
    rwy_transplog_append(&d, BASE_BLOCK, COPY_BLOCKS, &e1);

    /* Simulate cut before header block write during append of e2 */
    m.cut_after = m.writes;
    memset(&e2, 0, sizeof(e2));
    strcpy(e2.slot, "CONFIG2");
    rc = rwy_transplog_append(&d, BASE_BLOCK, COPY_BLOCKS, &e2);
    CHECK(rc != RWY_OK, "append fails due to power cut");

    /* Older copy must win and be undamaged */
    rc = rwy_transplog_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info);
    CHECK(rc == RWY_OK, "pick succeeds after power cut");
    CHECK(info.copy == 0, "older copy 0 remains winner");
    CHECK(info.count == 1, "count is still 1");
    rc = rwy_transplog_read_entry(&d, BASE_BLOCK, COPY_BLOCKS, &info, 0, &out);
    CHECK(rc == RWY_OK, "read older entry succeeds");
    CHECK(strcmp(out.slot, "CONFIG1") == 0, "older entry intact");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    test_sizes_and_capacity();
    test_empty_area();
    test_first_append();
    test_hash_chain_and_verify();
    test_tamper_detection();
    test_epoch_binding();
    test_ring_wrap_around();
    test_dual_atomicity_filters();
    test_torn_write_recovery();

    if (failures) {
        printf("FAILED (%d failures)\n", failures);
        return 1;
    }
    printf("ALL TRANSPLOG TESTS PASSED\n");
    return 0;
}
