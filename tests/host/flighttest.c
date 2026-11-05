/*
 * flighttest.c — Flight recorder unit tests: ping-pong A/B, sequence monotonicity,
 * torn-block recovery, ring wrap-around, CRC checks, corrupt headers, empty area,
 * rec_seq uncommitted entry discard, and the syslog line of an entry.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_internal.h"
#include "rwy_format.h"
#include "rwy_flight.h"

#define BASE_BLOCK   4U
#define COPY_BLOCKS  1U
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
    CHECK(sizeof(rwy_flight_header_t) == 64, "header size 64");
    CHECK(sizeof(rwy_flight_entry_t) == 64, "entry size 64");
    CHECK(rwy_flight_capacity(0) == 0, "capacity 0 for 0 blocks");
    CHECK(rwy_flight_capacity(1) == 63, "capacity 63 for 1 block");
    CHECK(rwy_flight_capacity(2) == 127, "capacity 127 for 2 blocks");
}

static void test_empty_area(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_flight_info_t info;
    int rc;

    mem_init(&m, &d);
    rc = rwy_flight_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info);
    CHECK(rc == RWY_ERR_CORRUPT, "pick on empty returns CORRUPT");
    CHECK(info.status[0] == RWY_FLIGHT_EMPTY, "copy A empty");
    CHECK(info.status[1] == RWY_FLIGHT_EMPTY, "copy B empty");
}

static void test_first_append(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_flight_info_t info;
    rwy_flight_entry_t e, out;
    int rc;

    mem_init(&m, &d);
    memset(&e, 0, sizeof(e));
    e.event = RWY_FLIGHT_EVENT_BOOT;
    e.reason = RWY_SS_REASON_NORMAL;
    e.rec_seq = 10;
    e.epoch = 1;
    snprintf(e.booted, sizeof(e.booted), "CONFIG0");

    rc = rwy_flight_append(&d, BASE_BLOCK, COPY_BLOCKS, &e);
    CHECK(rc == RWY_OK, "first append succeeds");

    rc = rwy_flight_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info);
    CHECK(rc == RWY_OK, "pick succeeds after first append");
    CHECK(info.copy == 0, "winner is copy A (0)");
    CHECK(info.sequence == 1, "sequence is 1");
    CHECK(info.count == 1, "count is 1");
    CHECK(info.head == 1, "head is 1");
    CHECK(info.capacity == 63, "capacity is 63");

    rc = rwy_flight_read_entry(&d, BASE_BLOCK, COPY_BLOCKS, &info, 0, &out);
    CHECK(rc == RWY_OK, "read entry 0");
    CHECK(out.seq == 1, "entry seq assigned to 1");
    CHECK(out.event == RWY_FLIGHT_EVENT_BOOT, "event is BOOT");
    CHECK(out.reason == RWY_SS_REASON_NORMAL, "reason is NORMAL");
    CHECK(out.rec_seq == 10, "rec_seq is 10");
    CHECK(!strcmp(out.booted, "CONFIG0"), "booted slot CONFIG0");

    rc = rwy_flight_read_entry(&d, BASE_BLOCK, COPY_BLOCKS, &info, 1, &out);
    CHECK(rc == RWY_ERR_INVAL, "read past count fails");
}

static void test_ping_pong_sequence_monotonicity(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_flight_info_t info;
    rwy_flight_entry_t e, out;
    int rc;

    mem_init(&m, &d);
    memset(&e, 0, sizeof(e));
    e.event = RWY_FLIGHT_EVENT_BOOT;
    snprintf(e.booted, sizeof(e.booted), "CONFIG0");
    CHECK(rwy_flight_append(&d, BASE_BLOCK, COPY_BLOCKS, &e) == RWY_OK, "append 1");

    memset(&e, 0, sizeof(e));
    e.event = RWY_FLIGHT_EVENT_CONDEMN;
    e.reason = RWY_SS_REASON_REQUESTED;
    snprintf(e.booted, sizeof(e.booted), "CONFIG0");
    CHECK(rwy_flight_append(&d, BASE_BLOCK, COPY_BLOCKS, &e) == RWY_OK, "append 2");

    rc = rwy_flight_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info);
    CHECK(rc == RWY_OK && info.copy == 1, "winner is copy B (1)");
    CHECK(info.sequence == 2, "copy sequence is 2");
    CHECK(info.count == 2, "count is 2");

    rc = rwy_flight_read_entry(&d, BASE_BLOCK, COPY_BLOCKS, &info, 0, &out);
    CHECK(rc == RWY_OK && out.seq == 1 && out.event == RWY_FLIGHT_EVENT_BOOT, "entry 0 intact");
    rc = rwy_flight_read_entry(&d, BASE_BLOCK, COPY_BLOCKS, &info, 1, &out);
    CHECK(rc == RWY_OK && out.seq == 2 && out.event == RWY_FLIGHT_EVENT_CONDEMN, "entry 1 intact");

    memset(&e, 0, sizeof(e));
    e.event = RWY_FLIGHT_EVENT_BOOT;
    snprintf(e.booted, sizeof(e.booted), "RECOVERY");
    CHECK(rwy_flight_append(&d, BASE_BLOCK, COPY_BLOCKS, &e) == RWY_OK, "append 3");

    rc = rwy_flight_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info);
    CHECK(rc == RWY_OK && info.copy == 0, "winner is copy A (0)");
    CHECK(info.sequence == 3, "copy sequence is 3");
    CHECK(info.count == 3, "count is 3");
}

static void test_corrupted_header_and_crc(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_flight_info_t info;
    rwy_flight_entry_t e;
    uint8_t *copy1;

    mem_init(&m, &d);
    memset(&e, 0, sizeof(e));
    e.event = RWY_FLIGHT_EVENT_BOOT;
    CHECK(rwy_flight_append(&d, BASE_BLOCK, COPY_BLOCKS, &e) == RWY_OK, "append 1 to copy 0");
    CHECK(rwy_flight_append(&d, BASE_BLOCK, COPY_BLOCKS, &e) == RWY_OK, "append 2 to copy 1");

    copy1 = m.mem + (BASE_BLOCK + 1 * COPY_BLOCKS) * RWY_BLOCK_SIZE;

    /* Flip a byte in the header payload of copy 1 to invalidate CRC */
    copy1[40] ^= 0xFF;
    CHECK(rwy_flight_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info) == RWY_OK, "pick fallback");
    CHECK(info.copy == 0, "fallback picked valid copy 0");
    CHECK(info.status[1] == RWY_FLIGHT_BAD_HEADER, "copy 1 has bad header");

    /* Restore byte, corrupt magic */
    copy1[40] ^= 0xFF;
    copy1[0] = 'X';
    CHECK(rwy_flight_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info) == RWY_OK, "pick fallback magic");
    CHECK(info.copy == 0, "picked copy 0 after magic corruption");
}

static void test_torn_write_recovery(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_flight_info_t info;
    rwy_flight_entry_t e, out;

    mem_init(&m, &d);
    memset(&e, 0, sizeof(e));
    e.event = RWY_FLIGHT_EVENT_BOOT;
    snprintf(e.booted, sizeof(e.booted), "CONFIG0");
    CHECK(rwy_flight_append(&d, BASE_BLOCK, COPY_BLOCKS, &e) == RWY_OK, "initial append");

    /* Cut the next write halfway through header */
    m.cut_after = m.writes;
    m.torn = 16;
    memset(&e, 0, sizeof(e));
    e.event = RWY_FLIGHT_EVENT_CONDEMN;
    CHECK(rwy_flight_append(&d, BASE_BLOCK, COPY_BLOCKS, &e) != RWY_OK, "torn append fails");

    /* Old copy must survive unharmed */
    m.cut_after = -1;
    CHECK(rwy_flight_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info) == RWY_OK, "pick unharmed copy");
    CHECK(info.copy == 0 && info.count == 1, "copy 0 preserved intact");
    CHECK(rwy_flight_read_entry(&d, BASE_BLOCK, COPY_BLOCKS, &info, 0, &out) == RWY_OK, "read");
    CHECK(out.event == RWY_FLIGHT_EVENT_BOOT, "original event intact");

    /* New append succeeds and overwrites the torn copy */
    CHECK(rwy_flight_append(&d, BASE_BLOCK, COPY_BLOCKS, &e) == RWY_OK, "retry append");
    CHECK(rwy_flight_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info) == RWY_OK, "pick new");
    CHECK(info.copy == 1 && info.count == 2, "copy 1 active with 2 entries");
}

static void test_ring_wraparound(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_flight_info_t info;
    rwy_flight_entry_t e, out;
    uint32_t cap = rwy_flight_capacity(COPY_BLOCKS);

    mem_init(&m, &d);
    CHECK(cap == 63, "capacity is 63");

    /* Fill ring to capacity */
    for (uint32_t i = 1; i <= cap; i++) {
        memset(&e, 0, sizeof(e));
        e.event = RWY_FLIGHT_EVENT_BOOT;
        e.seq = i;
        snprintf(e.booted, sizeof(e.booted), "SLOT%u", i);
        CHECK(rwy_flight_append(&d, BASE_BLOCK, COPY_BLOCKS, &e) == RWY_OK, "append fill");
    }

    CHECK(rwy_flight_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info) == RWY_OK, "pick full ring");
    CHECK(info.count == cap, "ring is full");
    CHECK(info.head == 0, "head wrapped to 0");

    /* Overwrite oldest entry */
    memset(&e, 0, sizeof(e));
    e.event = RWY_FLIGHT_EVENT_CONFIRM;
    e.seq = cap + 1;
    snprintf(e.booted, sizeof(e.booted), "SLOT%u", cap + 1);
    CHECK(rwy_flight_append(&d, BASE_BLOCK, COPY_BLOCKS, &e) == RWY_OK, "append wrap");

    CHECK(rwy_flight_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info) == RWY_OK, "pick wrapped");
    CHECK(info.count == cap, "count stays at capacity");
    CHECK(info.head == 1, "head advanced to 1");

    /* Chronological order: oldest should now be seq 2, newest seq 64 */
    CHECK(rwy_flight_read_entry(&d, BASE_BLOCK, COPY_BLOCKS, &info, 0, &out) == RWY_OK, "read oldest");
    CHECK(out.seq == 2 && !strcmp(out.booted, "SLOT2"), "oldest is seq 2");

    CHECK(rwy_flight_read_entry(&d, BASE_BLOCK, COPY_BLOCKS, &info, cap - 1, &out) == RWY_OK, "read newest");
    CHECK(out.seq == cap + 1 && out.event == RWY_FLIGHT_EVENT_CONFIRM, "newest is seq cap + 1");
}

static void test_rec_seq_discard_uncommitted(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_flight_info_t info;
    rwy_flight_entry_t e, out;

    mem_init(&m, &d);

    /* Append 3 entries: rec_seq 5, 5, 6 */
    memset(&e, 0, sizeof(e));
    e.event = RWY_FLIGHT_EVENT_BOOT;
    e.rec_seq = 5;
    snprintf(e.booted, sizeof(e.booted), "A");
    CHECK(rwy_flight_append(&d, BASE_BLOCK, COPY_BLOCKS, &e) == RWY_OK, "append A");

    e.rec_seq = 5;
    snprintf(e.booted, sizeof(e.booted), "B");
    CHECK(rwy_flight_append(&d, BASE_BLOCK, COPY_BLOCKS, &e) == RWY_OK, "append B");

    e.rec_seq = 6;
    snprintf(e.booted, sizeof(e.booted), "C");
    CHECK(rwy_flight_append(&d, BASE_BLOCK, COPY_BLOCKS, &e) == RWY_OK, "append C");

    CHECK(rwy_flight_pick(&d, BASE_BLOCK, COPY_BLOCKS, &info) == RWY_OK, "pick");
    CHECK(info.count == 3, "raw count is 3");

    /* Filter with max_rec_seq = 5 (entry C is uncommitted) */
    CHECK(rwy_flight_count_committed(&d, BASE_BLOCK, COPY_BLOCKS, &info, 5) == 2, "2 committed entries");
    CHECK(rwy_flight_read_committed(&d, BASE_BLOCK, COPY_BLOCKS, &info, 5, 0, &out) == RWY_OK, "read 0");
    CHECK(!strcmp(out.booted, "A"), "entry A");
    CHECK(rwy_flight_read_committed(&d, BASE_BLOCK, COPY_BLOCKS, &info, 5, 1, &out) == RWY_OK, "read 1");
    CHECK(!strcmp(out.booted, "B"), "entry B");
    CHECK(rwy_flight_read_committed(&d, BASE_BLOCK, COPY_BLOCKS, &info, 5, 2, &out) == RWY_ERR_NOENT,
          "entry C excluded");

    /* When state record advances to 6, entry C becomes committed */
    CHECK(rwy_flight_count_committed(&d, BASE_BLOCK, COPY_BLOCKS, &info, 6) == 3, "3 committed entries");
    CHECK(rwy_flight_read_committed(&d, BASE_BLOCK, COPY_BLOCKS, &info, 6, 2, &out) == RWY_OK, "read 2");
    CHECK(!strcmp(out.booted, "C"), "entry C now committed");
}

static void test_multi_block(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_flight_info_t info;
    rwy_flight_entry_t e, out;
    uint32_t blocks = 2;
    uint32_t cap = rwy_flight_capacity(blocks);

    mem_init(&m, &d);
    CHECK(cap == 127, "capacity 127 for 2 blocks");

    for (uint32_t i = 1; i <= 70; i++) {
        memset(&e, 0, sizeof(e));
        e.event = RWY_FLIGHT_EVENT_BOOT;
        e.seq = i;
        snprintf(e.booted, sizeof(e.booted), "SLOT%u", i);
        CHECK(rwy_flight_append(&d, BASE_BLOCK, blocks, &e) == RWY_OK, "append across boundary");
    }

    CHECK(rwy_flight_pick(&d, BASE_BLOCK, blocks, &info) == RWY_OK, "pick multi block");
    CHECK(info.count == 70, "count 70");
    CHECK(rwy_flight_read_entry(&d, BASE_BLOCK, blocks, &info, 0, &out) == RWY_OK, "read in block 0");
    CHECK(out.seq == 1, "seq 1 in block 0");
    CHECK(rwy_flight_read_entry(&d, BASE_BLOCK, blocks, &info, 68, &out) == RWY_OK, "read in block 1");
    CHECK(out.seq == 69, "seq 69 in block 1");
}

static void test_severity(void) {
    CHECK(rwy_flight_severity(RWY_FLIGHT_EVENT_NONE, RWY_SS_REASON_NONE) == RWY_FLIGHT_SEV_NORMAL, "no event is normal");
    CHECK(rwy_flight_severity(RWY_FLIGHT_EVENT_CONFIRM, RWY_SS_REASON_NORMAL) == RWY_FLIGHT_SEV_NORMAL, "confirm is normal");
    CHECK(rwy_flight_severity(RWY_FLIGHT_EVENT_BOOT, RWY_SS_REASON_NORMAL) == RWY_FLIGHT_SEV_NORMAL, "normal boot");
    CHECK(rwy_flight_severity(RWY_FLIGHT_EVENT_BOOT, RWY_SS_REASON_NONE) == RWY_FLIGHT_SEV_NORMAL, "boot without a reason");
    CHECK(rwy_flight_severity(RWY_FLIGHT_EVENT_BOOT, RWY_SS_REASON_ALL_FAILED) == RWY_FLIGHT_SEV_ERROR, "all failed");
    CHECK(rwy_flight_severity(RWY_FLIGHT_EVENT_BOOT, RWY_SS_REASON_INVALID_STORE) == RWY_FLIGHT_SEV_ERROR, "invalid store");
    CHECK(rwy_flight_severity(RWY_FLIGHT_EVENT_BOOT, RWY_SS_REASON_REQUESTED) == RWY_FLIGHT_SEV_WARNING, "requested recovery");
    CHECK(rwy_flight_severity(RWY_FLIGHT_EVENT_BOOT, RWY_SS_REASON_BOOT_LOOP) == RWY_FLIGHT_SEV_WARNING, "boot loop recovery");
    CHECK(rwy_flight_severity(RWY_FLIGHT_EVENT_CONDEMN, RWY_SS_REASON_NORMAL) == RWY_FLIGHT_SEV_ERROR, "condemn");
    CHECK(rwy_flight_severity(RWY_FLIGHT_EVENT_FAIL, RWY_SS_REASON_BOOT_LOOP) == RWY_FLIGHT_SEV_ERROR, "fail");
    CHECK(rwy_flight_severity(RWY_FLIGHT_EVENT_TRIES, RWY_SS_REASON_NORMAL) == RWY_FLIGHT_SEV_WARNING, "tries");
    CHECK(rwy_flight_severity(RWY_FLIGHT_EVENT_RECOVER_NEXT, RWY_SS_REASON_NORMAL) == RWY_FLIGHT_SEV_WARNING, "recover next");
    CHECK(rwy_flight_severity(RWY_FLIGHT_EVENT_LOOP_LIMIT, RWY_SS_REASON_NORMAL) == RWY_FLIGHT_SEV_WARNING, "loop limit");
    CHECK(rwy_flight_severity(RWY_FLIGHT_EVENT_STORE, RWY_SS_REASON_INVALID_STORE) == RWY_FLIGHT_SEV_ERROR, "store");
    CHECK(rwy_flight_severity(99, RWY_SS_REASON_NORMAL) == RWY_FLIGHT_SEV_ERROR, "unknown event");
}

static void test_record_keeps_report_events(void) {
    static rwy_store_t s;
    mem_t m;
    rwy_block_dev_t d;
    rwy_flight_info_t info;

    memset(&s, 0, sizeof(s));
    mem_init(&m, &d);
    s.dev = d;
    s.sb.flight_start = BASE_BLOCK;
    s.sb.flight_blocks = COPY_BLOCKS;
    s.sb.generation = 3;
    s.rec_seq = 5;

    CHECK(rwy_flight_record(&s, NULL, "CONFIG0", RWY_FLIGHT_EVENT_BOOT, RWY_SS_REASON_NORMAL, 6) == RWY_OK, "normal boot recorded");
    CHECK(s.flight_boot_count == 0, "normal boot not kept for the report");
    CHECK(rwy_flight_pick(&s.dev, BASE_BLOCK, COPY_BLOCKS, &info) == RWY_OK && info.count == 1, "normal boot in the ring");

    CHECK(rwy_flight_record(&s, NULL, "CONFIG0", RWY_FLIGHT_EVENT_CONDEMN, RWY_SS_REASON_NORMAL, 7) == RWY_OK, "condemn recorded");
    CHECK(s.flight_boot_count == 1, "condemn kept for the report");
    CHECK(s.flight_boot[0].event == RWY_FLIGHT_EVENT_CONDEMN && !strcmp(s.flight_boot[0].booted, "CONFIG0"), "kept entry");

    CHECK(rwy_flight_record(&s, NULL, "CONFIG1", RWY_FLIGHT_EVENT_BOOT, RWY_SS_REASON_NORMAL, 8) == RWY_OK, "second normal boot");
    CHECK(s.flight_boot_count == 1, "second normal boot not kept");
    CHECK(rwy_flight_pick(&s.dev, BASE_BLOCK, COPY_BLOCKS, &info) == RWY_OK && info.count == 3, "second normal boot in the ring");

    s.sb.flight_blocks = 0;
    CHECK(rwy_flight_record(&s, NULL, "RECOVERY", RWY_FLIGHT_EVENT_RECOVER_NEXT, RWY_SS_REASON_NORMAL, 9) == RWY_OK, "report without a ring");
    CHECK(s.flight_boot_count == 2, "event kept without a ring");
    CHECK(s.flight_boot[1].event == RWY_FLIGHT_EVENT_RECOVER_NEXT, "kept without a ring");

    for (uint32_t i = s.flight_boot_count; i < RWY_FLIGHT_BOOT_MAX; i++)
        CHECK(rwy_flight_record(&s, NULL, "CONFIG0", RWY_FLIGHT_EVENT_CONDEMN, RWY_SS_REASON_NORMAL, 10) == RWY_OK, "fill report");
    CHECK(rwy_flight_record(&s, NULL, "CONFIG0", RWY_FLIGHT_EVENT_CONDEMN, RWY_SS_REASON_NORMAL, 11) == RWY_OK, "overflow is ignored");
    CHECK(s.flight_boot_count == RWY_FLIGHT_BOOT_MAX, "report array stays full");
}

static void test_syslog(void) {
    rwy_flight_entry_t e;
    char buf[RWY_FLIGHT_SYSLOG_MAX];
    char host[600];
    size_t n;

    memset(&e, 0, sizeof(e));
    e.seq = 7;
    e.rec_seq = 5;
    e.event = RWY_FLIGHT_EVENT_CONDEMN;
    e.reason = RWY_SS_REASON_NORMAL;
    strcpy(e.booted, "CONFIG0");
    e.epoch = 3;
    e.rev_hw = 2;

    n = rwy_flight_format_syslog(&e, "2026-10-27T10:00:00Z", "5f0c2a7e-uuid", buf);
    CHECK(n == strlen("<131>1 2026-10-27T10:00:00Z 5f0c2a7e-uuid runway - CONDEMN - "
                      "seq=7 rec_seq=5 event=CONDEMN slot=CONFIG0 reason=normal epoch=3 rev_hw=2"),
          "condemn line length");
    CHECK(!memcmp(buf, "<131>1 2026-10-27T10:00:00Z 5f0c2a7e-uuid runway - CONDEMN - "
                       "seq=7 rec_seq=5 event=CONDEMN slot=CONFIG0 reason=normal epoch=3 rev_hw=2", n),
          "condemn line text");

    e.event = RWY_FLIGHT_EVENT_TRIES;
    n = rwy_flight_format_syslog(&e, NULL, NULL, buf);
    CHECK(n > 0 && !memcmp(buf, "<132>1 - - runway - TRIES - seq=7 ", 34), "warning with nil fields");

    e.event = RWY_FLIGHT_EVENT_BOOT;
    CHECK(rwy_flight_format_syslog(&e, NULL, NULL, buf) == 0, "normal event is not sent");

    e.event = RWY_FLIGHT_EVENT_CONDEMN;
    memset(host, 'h', sizeof(host) - 1);
    host[sizeof(host) - 1] = 0;
    n = rwy_flight_format_syslog(&e, NULL, host, buf);
    CHECK(n == RWY_FLIGHT_SYSLOG_MAX, "long line is cut to the bound");
    CHECK(!memcmp(buf, "<131>1 - hhhh", 13), "cut line keeps the header");
}

static void test_tail(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_flight_entry_t e, out[8];

    mem_init(&m, &d);
    for (uint64_t r = 1; r <= 4; r++) {
        memset(&e, 0, sizeof(e));
        e.rec_seq = r == 4 ? 9 : r;
        e.event = RWY_FLIGHT_EVENT_CONDEMN;
        CHECK(rwy_flight_append(&d, BASE_BLOCK, COPY_BLOCKS, &e) == RWY_OK, "tail entry appended");
    }
    CHECK(rwy_flight_tail(&d, BASE_BLOCK, COPY_BLOCKS, 0, 2, out) == 2, "tail of two");
    CHECK(out[0].rec_seq == 3 && out[1].rec_seq == 9, "tail oldest first");
    CHECK(rwy_flight_tail(&d, BASE_BLOCK, COPY_BLOCKS, 5, 8, out) == 3, "tail skips uncommitted");
    CHECK(out[0].rec_seq == 1 && out[1].rec_seq == 2 && out[2].rec_seq == 3, "tail committed order");
    CHECK(rwy_flight_tail(&d, BASE_BLOCK, 0, 0, 2, out) == 0, "tail without a ring");
}

static void test_helpers(void) {
    CHECK(!strcmp(rwy_flight_reason(RWY_FLIGHT_OK), "valid"), "reason valid");
    CHECK(!strcmp(rwy_flight_reason(RWY_FLIGHT_EMPTY), "empty"), "reason empty");
    CHECK(!strcmp(rwy_flight_reason(RWY_FLIGHT_BAD_HEADER), "bad header"), "reason bad header");
    CHECK(!strcmp(rwy_flight_reason(RWY_FLIGHT_IO), "I/O error"), "reason io");
    CHECK(!strcmp(rwy_flight_event_name(RWY_FLIGHT_EVENT_BOOT), "BOOT"), "event boot");
    CHECK(!strcmp(rwy_flight_event_name(RWY_FLIGHT_EVENT_CONDEMN), "CONDEMN"), "event condemn");
    CHECK(!strcmp(rwy_flight_event_name(RWY_FLIGHT_EVENT_CONFIRM), "CONFIRM"), "event confirm");
    CHECK(!strcmp(rwy_flight_event_name(RWY_FLIGHT_EVENT_FAIL), "FAIL"), "event fail");
    CHECK(!strcmp(rwy_flight_event_name(RWY_FLIGHT_EVENT_TRIES), "TRIES"), "event tries");
    CHECK(!strcmp(rwy_flight_event_name(RWY_FLIGHT_EVENT_RECOVER_NEXT), "RECOVER_NEXT"), "event recover_next");
    CHECK(!strcmp(rwy_flight_event_name(RWY_FLIGHT_EVENT_LOOP_LIMIT), "LOOP_LIMIT"), "event loop_limit");
    CHECK(!strcmp(rwy_flight_event_name(RWY_FLIGHT_EVENT_STORE), "STORE"), "event store");
}

int main(void) {
    test_sizes_and_capacity();
    test_empty_area();
    test_first_append();
    test_ping_pong_sequence_monotonicity();
    test_corrupted_header_and_crc();
    test_torn_write_recovery();
    test_ring_wraparound();
    test_rec_seq_discard_uncommitted();
    test_multi_block();
    test_severity();
    test_record_keeps_report_events();
    test_syslog();
    test_tail();
    test_helpers();

    if (failures) {
        printf("FAILED (%d errors)\n", failures);
        return 1;
    }
    printf("ALL FLIGHT TESTS PASSED\n");
    return 0;
}
