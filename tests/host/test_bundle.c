/*
 * test_bundle.c — Generic bundle unit test (write, pick, read, kind rules, WDT multi-entry).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rwy_crypto.h"
#include "rwy_bundle.h"

#define COPY_BLOCKS 16U
#define BASE        2U
#define TOTAL       (BASE + 2 * COPY_BLOCKS)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

typedef struct {
    uint8_t mem[TOTAL * RWY_BLOCK_SIZE];
    int writes;
    int cut_after;
    int flushes;
    int torn;
} mem_t;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_t *m = ctx;
    memcpy(buf, m->mem + off, len);
    return 0;
}

static int mem_write(void *ctx, uint64_t off, const void *buf, size_t len) {
    mem_t *m = ctx;
    if (m->cut_after >= 0 && m->writes >= m->cut_after) {
        if (m->torn) memcpy(m->mem + off, buf, len / 2);
        return -1;
    }
    memcpy(m->mem + off, buf, len);
    m->writes++;
    return 0;
}

static int mem_flush(void *ctx) { ((mem_t *)ctx)->flushes++; return 0; }

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

static uint8_t *copy_at(mem_t *m, unsigned c) {
    return m->mem + (size_t)(BASE + c * COPY_BLOCKS) * RWY_BLOCK_SIZE;
}

static void fill(uint8_t *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 11);
}

static const rwy_bundle_kind_rule_t loader_rules[2] = {
    { RWY_ENTRY_LOADER, 1, 1, 0 },
    { RWY_ENTRY_WDT,    0, 8, 0 },
};

static const rwy_bundle_spec_t loader_spec = {
    .magic = RWY_LOADER_MAGIC,
    .version = RWY_BUNDLE_VERSION,
    .max_entries = 9,
    .num_rules = 2,
    .rules = loader_rules,
};

static uint8_t loader_bin[8192];
static uint8_t wdt1[4096], wdt2[2000], wdt3[1000];

static void test_blocks_and_rules(void) {
    rwy_bundle_payload_t parts[10];

    printf("== bundle: blocks calculation and kind rules ==\n");
    memset(parts, 0, sizeof(parts));
    parts[0].kind = RWY_ENTRY_LOADER;
    parts[0].data = loader_bin;
    parts[0].len = sizeof(loader_bin);

    CHECK(rwy_bundle_blocks(&loader_spec, parts, 1) == 1 + 2, "loader only = header + 2 blocks");

    parts[1].kind = RWY_ENTRY_WDT;
    parts[1].data = wdt1;
    parts[1].len = sizeof(wdt1);
    CHECK(rwy_bundle_blocks(&loader_spec, parts, 2) == 1 + 2 + 1, "loader + 1 wdt = 4 blocks");

    parts[2].kind = RWY_ENTRY_WDT;
    parts[2].data = wdt2;
    parts[2].len = sizeof(wdt2);
    parts[3].kind = RWY_ENTRY_WDT;
    parts[3].data = wdt3;
    parts[3].len = sizeof(wdt3);
    CHECK(rwy_bundle_blocks(&loader_spec, parts, 4) == 1 + 2 + 1 + 1 + 1, "loader + 3 wdt = 6 blocks");

    /* Multiple LOADER entries refused */
    parts[1].kind = RWY_ENTRY_LOADER;
    CHECK(rwy_bundle_blocks(&loader_spec, parts, 2) == 0, "second loader entry refused");

    /* Missing required LOADER refused */
    parts[0].kind = RWY_ENTRY_WDT;
    parts[1].kind = RWY_ENTRY_WDT;
    CHECK(rwy_bundle_blocks(&loader_spec, parts, 2) == 0, "missing loader refused");

    /* Zero length refused */
    parts[0].kind = RWY_ENTRY_LOADER;
    parts[0].len = 0;
    CHECK(rwy_bundle_blocks(&loader_spec, parts, 1) == 0, "zero length refused");
}

static void test_write_pick_multi_wdt(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_bundle_payload_t parts[4];
    rwy_bundle_info_t info;
    uint8_t out[16384];
    size_t len = 0;

    printf("== bundle: write, pick, and multi-entry read ==\n");
    mem_init(&m, &d);
    CHECK(rwy_bundle_pick(&d, BASE, COPY_BLOCKS, &loader_spec, &info) == RWY_ERR_CORRUPT, "empty area absent");
    CHECK(info.status[0] == RWY_BUNDLE_EMPTY && info.status[1] == RWY_BUNDLE_EMPTY, "both empty");

    parts[0].kind = RWY_ENTRY_LOADER;
    parts[0].data = loader_bin;
    parts[0].len = sizeof(loader_bin);
    parts[1].kind = RWY_ENTRY_WDT;
    parts[1].data = wdt1;
    parts[1].len = sizeof(wdt1);
    parts[2].kind = RWY_ENTRY_WDT;
    parts[2].data = wdt2;
    parts[2].len = sizeof(wdt2);

    CHECK(rwy_bundle_write(&d, BASE, COPY_BLOCKS, &loader_spec, parts, 3) == RWY_OK, "write copy 0");
    CHECK(rwy_bundle_pick(&d, BASE, COPY_BLOCKS, &loader_spec, &info) == RWY_OK, "pick copy 0");
    CHECK(info.copy == 0 && info.sequence == 1 && info.count == 3, "copy 0 sequence 1");

    /* Read loader by kind */
    CHECK(rwy_bundle_read(&d, BASE, COPY_BLOCKS, &info, RWY_ENTRY_LOADER, out, sizeof(out), &len) == RWY_OK &&
          len == sizeof(loader_bin) && !memcmp(out, loader_bin, len), "loader payload matches");

    /* Read WDT entries by index */
    CHECK(rwy_bundle_read_at(&d, BASE, COPY_BLOCKS, &info, 1, out, sizeof(out), &len) == RWY_OK &&
          len == sizeof(wdt1) && !memcmp(out, wdt1, len), "wdt1 payload matches");
    CHECK(rwy_bundle_read_at(&d, BASE, COPY_BLOCKS, &info, 2, out, sizeof(out), &len) == RWY_OK &&
          len == sizeof(wdt2) && !memcmp(out, wdt2, len), "wdt2 payload matches");
    CHECK(rwy_bundle_read_at(&d, BASE, COPY_BLOCKS, &info, 3, out, sizeof(out), &len) == RWY_ERR_INVAL,
          "out-of-bounds entry index refused");

    /* Second write goes to copy 1 */
    parts[2].data = wdt3;
    parts[2].len = sizeof(wdt3);
    CHECK(rwy_bundle_write(&d, BASE, COPY_BLOCKS, &loader_spec, parts, 3) == RWY_OK, "write copy 1");
    CHECK(rwy_bundle_pick(&d, BASE, COPY_BLOCKS, &loader_spec, &info) == RWY_OK, "pick copy 1");
    CHECK(info.copy == 1 && info.sequence == 2, "copy 1 sequence 2 wins");
    CHECK(info.seq[0] == 1 && info.seq[1] == 2, "both sequences recorded");
}

static void test_tampering(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_bundle_payload_t parts[2];
    rwy_bundle_info_t info;
    uint8_t *copy0;

    printf("== bundle: corruption detection ==\n");
    mem_init(&m, &d);
    parts[0].kind = RWY_ENTRY_LOADER;
    parts[0].data = loader_bin;
    parts[0].len = sizeof(loader_bin);
    parts[1].kind = RWY_ENTRY_WDT;
    parts[1].data = wdt1;
    parts[1].len = sizeof(wdt1);

    rwy_bundle_write(&d, BASE, COPY_BLOCKS, &loader_spec, parts, 2);
    copy0 = copy_at(&m, 0);

    /* Flip a byte in payload block (block 1) */
    copy0[RWY_BLOCK_SIZE + 10] ^= 0x55;
    CHECK(rwy_bundle_pick(&d, BASE, COPY_BLOCKS, &loader_spec, &info) == RWY_ERR_CORRUPT, "corrupt payload rejected");
    CHECK(info.status[0] == RWY_BUNDLE_BAD_HASH, "reason is BAD_HASH");
    copy0[RWY_BLOCK_SIZE + 10] ^= 0x55;

    /* Flip magic byte */
    copy0[0] ^= 0xff;
    CHECK(rwy_bundle_pick(&d, BASE, COPY_BLOCKS, &loader_spec, &info) == RWY_ERR_CORRUPT, "corrupt magic rejected");
    CHECK(info.status[0] == RWY_BUNDLE_EMPTY, "reason is EMPTY");
    copy0[0] ^= 0xff;

    /* Flip CRC */
    copy0[RWY_BUNDLE_CRC_OFFSET] ^= 0x01;
    CHECK(rwy_bundle_pick(&d, BASE, COPY_BLOCKS, &loader_spec, &info) == RWY_ERR_CORRUPT, "corrupt crc rejected");
    CHECK(info.status[0] == RWY_BUNDLE_BAD_HEADER, "reason is BAD_HEADER");
}

static void test_same_bundle(void) {
    rwy_bundle_info_t a, b;

    printf("== bundle: equality comparison ==\n");
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.count = 2;
    b.count = 2;
    a.entry[0].kind = RWY_ENTRY_LOADER;
    a.entry[0].length = 100;
    a.entry[0].sha256[0] = 42;
    b.entry[0] = a.entry[0];
    a.entry[1].kind = RWY_ENTRY_WDT;
    a.entry[1].length = 200;
    a.entry[1].sha256[0] = 77;
    b.entry[1] = a.entry[1];

    CHECK(rwy_bundle_same(&a, &b), "identical bundles match");
    b.entry[1].length = 201;
    CHECK(!rwy_bundle_same(&a, &b), "different length does not match");
    b.entry[1].length = 200;
    b.entry[1].kind = RWY_ENTRY_LOADER;
    CHECK(!rwy_bundle_same(&a, &b), "different kind does not match");
}

int main(void) {
    fill(loader_bin, sizeof(loader_bin), 1);
    fill(wdt1, sizeof(wdt1), 10);
    fill(wdt2, sizeof(wdt2), 20);
    fill(wdt3, sizeof(wdt3), 30);

    test_blocks_and_rules();
    test_write_pick_multi_wdt();
    test_tampering();
    test_same_bundle();

    if (failures) {
        printf("test_bundle: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_bundle: all passed\n");
    return 0;
}
