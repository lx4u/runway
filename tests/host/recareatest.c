/*
 * recareatest.c — The recovery area bundle code (ADR 0004): write, pick, read, hostile headers, power cuts.
 * SPDX-License-Identifier: Apache-2.0
 * The area is a memory buffer of two copies; nothing touches a store.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rwy_crypto.h"
#include "rwy_recarea.h"

#define COPY_BLOCKS 8U
#define BASE        3U   /* a few blocks in front, as the state record is */
#define TOTAL       (BASE + 2 * COPY_BLOCKS)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

typedef struct {
    uint8_t mem[TOTAL * RWY_BLOCK_SIZE];
    int writes;      /* block writes done */
    int cut_after;   /* -1 = never; else writes fail from this count on */
    int flushes;
    int torn;        /* the failing write puts half of its block on the medium first */
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

static uint8_t *copy_at(mem_t *m, unsigned c) { return m->mem + (size_t)(BASE + c * COPY_BLOCKS) * RWY_BLOCK_SIZE; }

static void fill(uint8_t *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 7);
}

static uint8_t kern_a[9000], kern_b[5000], rd[4096], args[40];

static void make_bundle(rwy_ra_bundle_t *b, const uint8_t *kern, size_t klen) {
    memset(b, 0, sizeof(*b));
    b->part[RWY_RA_KERNEL].data = kern;
    b->part[RWY_RA_KERNEL].len = (uint32_t)klen;
}

static void test_blocks_needed(void) {
    rwy_ra_bundle_t b;
    printf("== recovery area: blocks needed ==\n");
    make_bundle(&b, kern_a, sizeof(kern_a));
    CHECK(rwy_ra_blocks(&b) == 1 + 3, "header plus 9000 bytes = 4 blocks");
    b.part[RWY_RA_INITRD].data = rd;
    b.part[RWY_RA_INITRD].len = sizeof(rd);
    CHECK(rwy_ra_blocks(&b) == 1 + 3 + 1, "an initrd of exactly one block adds one");
    b.part[RWY_RA_ARGS].data = args;
    b.part[RWY_RA_ARGS].len = sizeof(args);
    CHECK(rwy_ra_blocks(&b) == 1 + 3 + 1 + 1, "ARGS adds one");
    b.part[RWY_RA_ARGS].len = RWY_RA_ARGS_MAX + 1;
    CHECK(rwy_ra_blocks(&b) == 0, "ARGS over 4 KiB is refused");
    b.part[RWY_RA_ARGS].len = sizeof(args);
    b.part[RWY_RA_DTB].data = rd;
    b.part[RWY_RA_DTB].len = 0;
    CHECK(rwy_ra_blocks(&b) == 0, "an empty part is refused");
    b.part[RWY_RA_DTB].data = NULL;
    b.part[RWY_RA_KERNEL].data = NULL;
    CHECK(rwy_ra_blocks(&b) == 0, "no kernel is refused");
    CHECK(rwy_ra_blocks(NULL) == 0, "NULL bundle");
}

static void test_write_pick_read(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_ra_bundle_t b;
    rwy_ra_info_t info;
    uint8_t out[16384];
    size_t len = 0;

    printf("== recovery area: write, pick, read ==\n");
    mem_init(&m, &d);
    CHECK(rwy_ra_pick(&d, BASE, COPY_BLOCKS, &info) == RWY_ERR_CORRUPT, "an empty area is absent");
    CHECK(info.status[0] == RWY_RA_EMPTY && info.status[1] == RWY_RA_EMPTY, "both copies empty");

    make_bundle(&b, kern_a, sizeof(kern_a));
    b.part[RWY_RA_INITRD].data = rd;
    b.part[RWY_RA_INITRD].len = sizeof(rd);
    b.part[RWY_RA_ARGS].data = args;
    b.part[RWY_RA_ARGS].len = sizeof(args);
    CHECK(rwy_ra_write(&d, BASE, COPY_BLOCKS, &b) == RWY_OK, "first write");
    CHECK(rwy_ra_pick(&d, BASE, COPY_BLOCKS, &info) == RWY_OK, "pick after the first write");
    CHECK(info.copy == 0 && info.sequence == 1 && info.count == 3, "first write goes to copy A, sequence 1");
    CHECK(info.status[1] == RWY_RA_EMPTY && info.seq[1] == 0, "copy B still empty, no sequence");
    CHECK(rwy_ra_read(&d, BASE, COPY_BLOCKS, &info, RWY_RA_KERNEL, out, sizeof(out), &len) == RWY_OK &&
          len == sizeof(kern_a) && !memcmp(out, kern_a, len), "kernel reads back");
    CHECK(rwy_ra_read(&d, BASE, COPY_BLOCKS, &info, RWY_RA_ARGS, out, sizeof(out), &len) == RWY_OK &&
          len == sizeof(args) && !memcmp(out, args, len), "ARGS reads back");
    CHECK(rwy_ra_read(&d, BASE, COPY_BLOCKS, &info, RWY_RA_DTB, out, sizeof(out), &len) == RWY_ERR_NOENT,
          "a part that is not in the bundle");
    CHECK(rwy_ra_read(&d, BASE, COPY_BLOCKS, &info, RWY_RA_KERNEL, out, 100, &len) == RWY_ERR_SIZE,
          "a buffer that is too small");

    make_bundle(&b, kern_b, sizeof(kern_b));
    CHECK(rwy_ra_write(&d, BASE, COPY_BLOCKS, &b) == RWY_OK, "second write");
    CHECK(rwy_ra_pick(&d, BASE, COPY_BLOCKS, &info) == RWY_OK && info.copy == 1 && info.sequence == 2,
          "second write goes to copy B, sequence 2");
    CHECK(info.seq[0] == 1 && info.seq[1] == 2, "the sequence of each valid copy is kept");
    CHECK(rwy_ra_read(&d, BASE, COPY_BLOCKS, &info, RWY_RA_KERNEL, out, sizeof(out), &len) == RWY_OK &&
          len == sizeof(kern_b) && !memcmp(out, kern_b, len), "new kernel wins");
    make_bundle(&b, kern_a, sizeof(kern_a));
    CHECK(rwy_ra_write(&d, BASE, COPY_BLOCKS, &b) == RWY_OK, "third write");
    CHECK(rwy_ra_pick(&d, BASE, COPY_BLOCKS, &info) == RWY_OK && info.copy == 0 && info.sequence == 3,
          "third write goes back to copy A, sequence 3");
}

static void test_write_refused(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_ra_bundle_t b;
    static uint8_t big[(COPY_BLOCKS + 1) * RWY_BLOCK_SIZE];

    printf("== recovery area: a refused bundle writes nothing ==\n");
    mem_init(&m, &d);
    make_bundle(&b, big, sizeof(big));
    CHECK(rwy_ra_write(&d, BASE, COPY_BLOCKS, &b) == RWY_ERR_NOSPC, "bundle larger than a copy");
    b.part[RWY_RA_KERNEL].data = NULL;
    CHECK(rwy_ra_write(&d, BASE, COPY_BLOCKS, &b) == RWY_ERR_INVAL, "no kernel");
    CHECK(m.writes == 0, "nothing written");
}

/* Flip one byte of a copy and say what the pick reports for it. */
static int status_after_flip(mem_t *m, rwy_block_dev_t *d, unsigned c, size_t off) {
    rwy_ra_info_t info;
    copy_at(m, c)[off] ^= 0x5a;
    rwy_ra_pick(d, BASE, COPY_BLOCKS, &info);
    copy_at(m, c)[off] ^= 0x5a;
    return info.status[c];
}

static void test_corrupt_copy(void) {
    mem_t m;
    rwy_block_dev_t d;
    rwy_ra_bundle_t b;
    rwy_ra_info_t info;

    printf("== recovery area: a bad newest copy falls back ==\n");
    mem_init(&m, &d);
    make_bundle(&b, kern_a, sizeof(kern_a));
    rwy_ra_write(&d, BASE, COPY_BLOCKS, &b);
    make_bundle(&b, kern_b, sizeof(kern_b));
    rwy_ra_write(&d, BASE, COPY_BLOCKS, &b);   /* copy B is newest */

    CHECK(status_after_flip(&m, &d, 1, 9) == RWY_RA_BAD_HEADER, "a flipped header byte fails the CRC");
    CHECK(status_after_flip(&m, &d, 1, RWY_BLOCK_SIZE + 3) == RWY_RA_BAD_HASH, "a flipped payload byte fails the hash");
    CHECK(status_after_flip(&m, &d, 1, 0) == RWY_RA_EMPTY, "a flipped magic byte reads as empty");

    copy_at(&m, 1)[RWY_BLOCK_SIZE + 3] ^= 1;
    CHECK(rwy_ra_pick(&d, BASE, COPY_BLOCKS, &info) == RWY_OK && info.copy == 0 && info.sequence == 1,
          "the older valid copy is used");
    CHECK(info.status[1] == RWY_RA_BAD_HASH, "the reason for the newest copy is kept");
    copy_at(&m, 0)[RWY_BLOCK_SIZE + 3] ^= 1;
    CHECK(rwy_ra_pick(&d, BASE, COPY_BLOCKS, &info) == RWY_ERR_CORRUPT, "no valid copy");

    /* The next write replaces the bad newest copy, with the next sequence after the valid one. */
    copy_at(&m, 0)[RWY_BLOCK_SIZE + 3] ^= 1;
    make_bundle(&b, kern_a, sizeof(kern_a));
    CHECK(rwy_ra_write(&d, BASE, COPY_BLOCKS, &b) == RWY_OK, "write over a bad newest copy");
    CHECK(rwy_ra_pick(&d, BASE, COPY_BLOCKS, &info) == RWY_OK && info.copy == 1 && info.sequence == 2,
          "it went to the bad copy");
}

/* Rewrite the header of copy `c` with `edit` applied and a good CRC. */
static void forge(mem_t *m, unsigned c, void (*edit)(rwy_ra_header_t *)) {
    uint8_t *blk = copy_at(m, c);
    rwy_ra_header_t *h = (rwy_ra_header_t *)blk;
    uint32_t crc;
    edit(h);
    crc = rwy_crc32c(blk, RWY_RA_CRC_OFFSET);
    memcpy(blk + RWY_RA_CRC_OFFSET, &crc, 4);
}

static void e_offset_big(rwy_ra_header_t *h) { h->entry[0].offset = (uint64_t)COPY_BLOCKS * RWY_BLOCK_SIZE; }
static void e_len_big(rwy_ra_header_t *h) { h->entry[0].length = (uint64_t)COPY_BLOCKS * RWY_BLOCK_SIZE; }
static void e_overflow(rwy_ra_header_t *h) { h->entry[0].offset = RWY_BLOCK_SIZE; h->entry[0].length = ~0ULL - 100; }
static void e_offset_wrap(rwy_ra_header_t *h) { h->entry[0].offset = ~0ULL - 4095; h->entry[0].length = 8192; }
static void e_unaligned(rwy_ra_header_t *h) { h->entry[0].offset += 1; }
static void e_in_header(rwy_ra_header_t *h) { h->entry[0].offset = 0; }
static void e_zero_len(rwy_ra_header_t *h) { h->entry[0].length = 0; }
static void e_count_big(rwy_ra_header_t *h) { h->count = RWY_RA_KINDS + 1; }
static void e_count_zero(rwy_ra_header_t *h) { h->count = 0; }
static void e_bad_kind(rwy_ra_header_t *h) { h->entry[0].kind = RWY_RA_KINDS; }
static void e_no_kernel(rwy_ra_header_t *h) { h->entry[0].kind = RWY_RA_DTB; }
static void e_dup_kind(rwy_ra_header_t *h) { h->entry[1].kind = h->entry[0].kind; }
static void e_args_big(rwy_ra_header_t *h) { h->entry[1].kind = RWY_RA_ARGS; h->entry[1].length = RWY_RA_ARGS_MAX + 1; }
static void e_version(rwy_ra_header_t *h) { h->version = 2; }

static void test_hostile_header(void) {
    static const struct { const char *name; void (*edit)(rwy_ra_header_t *); int want; } t[] = {
        {"offset at the end of the copy", e_offset_big, RWY_RA_BAD_RANGE},
        {"length past the end of the copy", e_len_big, RWY_RA_BAD_RANGE},
        {"offset + length overflows", e_overflow, RWY_RA_BAD_RANGE},
        {"offset near 2^64", e_offset_wrap, RWY_RA_BAD_RANGE},
        {"offset not block aligned", e_unaligned, RWY_RA_BAD_RANGE},
        {"payload inside the header block", e_in_header, RWY_RA_BAD_RANGE},
        {"empty entry", e_zero_len, RWY_RA_BAD_RANGE},
        {"too many entries", e_count_big, RWY_RA_BAD_HEADER},
        {"no entries", e_count_zero, RWY_RA_BAD_HEADER},
        {"unknown kind", e_bad_kind, RWY_RA_BAD_HEADER},
        {"no kernel", e_no_kernel, RWY_RA_BAD_HEADER},
        {"kind twice", e_dup_kind, RWY_RA_BAD_HEADER},
        {"ARGS over 4 KiB", e_args_big, RWY_RA_BAD_RANGE},
        {"unknown version", e_version, RWY_RA_BAD_HEADER},
    };
    printf("== recovery area: hostile headers ==\n");
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        mem_t m;
        rwy_block_dev_t d;
        rwy_ra_bundle_t b;
        rwy_ra_info_t info;
        mem_init(&m, &d);
        make_bundle(&b, kern_a, sizeof(kern_a));
        b.part[RWY_RA_INITRD].data = rd;
        b.part[RWY_RA_INITRD].len = sizeof(rd);
        rwy_ra_write(&d, BASE, COPY_BLOCKS, &b);
        forge(&m, 0, t[i].edit);
        CHECK(rwy_ra_pick(&d, BASE, COPY_BLOCKS, &info) == RWY_ERR_CORRUPT && info.status[0] == t[i].want, t[i].name);
    }
}

/* A cut after n block writes of a replacement: the old bundle must still be picked. */
static void test_power_cut(void) {
    mem_t base, m;
    rwy_block_dev_t d;
    rwy_ra_bundle_t b;
    rwy_ra_info_t info;
    uint8_t out[16384];
    size_t len, cuts = 0;
    int rc;

    printf("== recovery area: a power cut at every write point ==\n");
    mem_init(&base, &d);
    make_bundle(&b, kern_a, sizeof(kern_a));
    rwy_ra_write(&d, BASE, COPY_BLOCKS, &b);
    make_bundle(&b, kern_b, sizeof(kern_b));
    rwy_ra_write(&d, BASE, COPY_BLOCKS, &b);   /* A: kern_a seq 1, B: kern_b seq 2 */

    for (int cut = 0; cut < 12; cut++) {
        int torn = cut % 2;   /* odd cuts leave half of the failing block behind, the header cut (3) included */
        make_bundle(&b, kern_a, sizeof(kern_a));
        mem_init(&m, &d);
        m.torn = torn;
        memcpy(m.mem, base.mem, sizeof(m.mem));
        m.cut_after = cut;
        rc = rwy_ra_write(&d, BASE, COPY_BLOCKS, &b);
        m.cut_after = -1;
        m.torn = 0;
        CHECK(rwy_ra_pick(&d, BASE, COPY_BLOCKS, &info) == RWY_OK, "a bundle survives the cut");
        if (rc == RWY_OK) {
            CHECK(info.sequence == 3, "no cut: the new bundle");
            break;
        }
        cuts++;
        CHECK(info.sequence == 2 && rwy_ra_read(&d, BASE, COPY_BLOCKS, &info, RWY_RA_KERNEL, out, sizeof(out), &len) == RWY_OK &&
              len == sizeof(kern_b) && !memcmp(out, kern_b, len), "after a cut the old bundle is still the one");
    }
    CHECK(cuts == 4, "cut before each of the 3 payload blocks and the header");
}

static void test_same_bundle(void) {
    mem_t m1, m2;
    rwy_block_dev_t d1, d2;
    rwy_ra_bundle_t b;
    rwy_ra_info_t i1, i2;

    printf("== recovery area: same bundle ==\n");
    mem_init(&m1, &d1);
    mem_init(&m2, &d2);
    make_bundle(&b, kern_a, sizeof(kern_a));
    CHECK(rwy_ra_write(&d1, BASE, COPY_BLOCKS, &b) == RWY_OK, "write member 1");
    CHECK(rwy_ra_write(&d1, BASE, COPY_BLOCKS, &b) == RWY_OK, "write member 1 again");
    CHECK(rwy_ra_write(&d2, BASE, COPY_BLOCKS, &b) == RWY_OK, "write member 2");
    CHECK(rwy_ra_pick(&d1, BASE, COPY_BLOCKS, &i1) == RWY_OK && rwy_ra_pick(&d2, BASE, COPY_BLOCKS, &i2) == RWY_OK,
          "pick both");
    CHECK(i1.sequence != i2.sequence && i1.copy != i2.copy, "sequence and copy differ");
    CHECK(rwy_ra_same(&i1, &i2), "same content in another copy and sequence is the same bundle");
    make_bundle(&b, kern_b, sizeof(kern_b));
    CHECK(rwy_ra_write(&d2, BASE, COPY_BLOCKS, &b) == RWY_OK && rwy_ra_pick(&d2, BASE, COPY_BLOCKS, &i2) == RWY_OK,
          "write another kernel");
    CHECK(!rwy_ra_same(&i1, &i2), "another kernel is not the same");
    make_bundle(&b, kern_a, sizeof(kern_a));
    b.part[RWY_RA_ARGS].data = args;
    b.part[RWY_RA_ARGS].len = sizeof(args);
    CHECK(rwy_ra_write(&d2, BASE, COPY_BLOCKS, &b) == RWY_OK && rwy_ra_pick(&d2, BASE, COPY_BLOCKS, &i2) == RWY_OK,
          "write the kernel with ARGS");
    CHECK(!rwy_ra_same(&i1, &i2), "an extra part is not the same");
}

int main(void) {
    fill(kern_a, sizeof(kern_a), 1);
    fill(kern_b, sizeof(kern_b), 50);
    fill(rd, sizeof(rd), 99);
    fill(args, sizeof(args), 7);
    test_blocks_needed();
    test_write_pick_read();
    test_write_refused();
    test_corrupt_copy();
    test_hostile_header();
    test_power_cut();
    test_same_bundle();
    if (failures) { printf("recareatest: %d failure(s)\n", failures); return 1; }
    printf("recareatest: all passed\n");
    return 0;
}
