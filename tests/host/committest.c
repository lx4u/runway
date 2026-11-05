/*
 * committest.c — Device calls of a commit.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Verifies:
 * 1. rwy_blk_write_run writes the same bytes as single block writes and checks its range.
 * 2. A commit makes five device calls: the inode table run, the bitmap run, a flush, the
 *    superblock slot and a flush, however many inodes it touches.
 * 3. A failed or torn write at any of these points leaves the old state mounted.
 * 4. Run writes keep the write policy of the mirror.
 * 5. The core counters match what the device saw, by kind.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "testdir.h"
#include "rwy_format.h"
#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_internal.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

#define STORE_MIB 64u
#define STORE_BYTES ((size_t)STORE_MIB << 20)
#define MAX_LOG 64

typedef struct { char kind; uint32_t blk, nblk; } call_t;

typedef struct {
    uint8_t *mem;
    call_t log[MAX_LOG];
    int nlog, nwrites, nflushes;
    int fail_write;      /* 1-based write call that fails, 0 = none */
    uint32_t tear;       /* blocks of the failing write that reach the media first */
    int fail_flush;      /* 1-based flush call that fails, 0 = none */
} mdev_t;

static int m_rd(void *ctx, uint64_t off, void *buf, size_t len) {
    mdev_t *m = ctx;
    if (off + len > STORE_BYTES) return -1;
    memcpy(buf, m->mem + off, len);
    return 0;
}

static void note(mdev_t *m, char kind, uint64_t off, size_t len) {
    if (m->nlog < MAX_LOG) {
        m->log[m->nlog].kind = kind;
        m->log[m->nlog].blk = (uint32_t)(off / RWY_BLOCK_SIZE);
        m->log[m->nlog].nblk = (uint32_t)(len / RWY_BLOCK_SIZE);
    }
    m->nlog++;
}

static int m_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    mdev_t *m = ctx;
    if (off + len > STORE_BYTES) return -1;
    note(m, 'W', off, len);
    if (m->fail_write && ++m->nwrites == m->fail_write) {
        size_t part = (size_t)m->tear * RWY_BLOCK_SIZE;
        memcpy(m->mem + off, buf, part < len ? part : len);
        return -1;
    }
    memcpy(m->mem + off, buf, len);
    return 0;
}

static int m_fl(void *ctx) {
    mdev_t *m = ctx;
    note(m, 'F', 0, 0);
    return m->fail_flush && ++m->nflushes == m->fail_flush ? -1 : 0;
}

static rwy_block_dev_t dev_of(mdev_t *m) {
    rwy_block_dev_t d = { m_rd, m_wr, m_fl, m, STORE_BYTES, RWY_BLOCK_SIZE, NULL, NULL };
    return d;
}

static void clear_faults(mdev_t *m) {
    m->fail_write = m->fail_flush = 0;
    m->nwrites = m->nflushes = m->nlog = 0;
    m->tear = 0;
}

static rwy_store_t g_s;
static uint8_t *g_base;

static void load_base(void) {
    const char *img = tpath("committest.img");
    char cmd[512];
    FILE *f;
    snprintf(cmd, sizeof(cmd), "%s --size %u %s >/dev/null", mkstore_path(), STORE_MIB, img);
    if (system(cmd) || !(f = fopen(img, "rb"))) { printf("cannot make the test store\n"); exit(1); }
    g_base = malloc(STORE_BYTES);
    if (!g_base || fread(g_base, 1, STORE_BYTES, f) != STORE_BYTES) { printf("cannot read the test store\n"); exit(1); }
    fclose(f);
    remove(img);
}

static void fresh(mdev_t *m) {
    memcpy(m->mem, g_base, STORE_BYTES);
    clear_faults(m);
}

static void fill(uint8_t *b, size_t n, int seed) {
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(seed * 31 + i);
}

/* Create `nfiles` files of one block each in a transaction, ready to commit. */
static int stage(rwy_store_t *s, int nfiles) {
    uint8_t b[RWY_BLOCK_SIZE];
    char name[32];
    if (rwy_begin(s)) return -1;
    for (int i = 0; i < nfiles; i++) {
        snprintf(name, sizeof(name), "F%d.BIN", i);
        fill(b, sizeof(b), i);
        if (rwy_create(s, name, RWY_S_IFREG | 0644, 0, 0) || rwy_write_path(s, name, 0, b, sizeof(b))) return -1;
    }
    return 0;
}

static int has_files(rwy_store_t *s, int nfiles) {
    uint8_t want[RWY_BLOCK_SIZE], got[RWY_BLOCK_SIZE];
    char name[32];
    for (int i = 0; i < nfiles; i++) {
        size_t n = sizeof(got);
        snprintf(name, sizeof(name), "F%d.BIN", i);
        fill(want, sizeof(want), i);
        if (rwy_read_path(s, name, 0, got, &n) || n != sizeof(got) || memcmp(want, got, n)) return 0;
    }
    return 1;
}

static int has_no_file(rwy_store_t *s) {
    rwy_attr_t a;
    return rwy_getattr_path(s, "F0.BIN", &a) != RWY_OK;
}

static void test_run_write(mdev_t *m) {
    rwy_block_dev_t d = dev_of(m);
    uint8_t buf[3 * RWY_BLOCK_SIZE];
    uint32_t last = (uint32_t)(STORE_BYTES / RWY_BLOCK_SIZE) - 1;

    printf("== run write ==\n");
    fresh(m);
    fill(buf, sizeof(buf), 7);
    CHECK(rwy_blk_write_run(&d, RWY_IO_META, 100, buf, 3) == RWY_OK, "run of three blocks");
    CHECK(m->nlog == 1 && m->log[0].nblk == 3 && m->log[0].blk == 100, "one device call");
    CHECK(!memcmp(m->mem + 100 * (size_t)RWY_BLOCK_SIZE, buf, sizeof(buf)), "bytes land in order");
    CHECK(rwy_blk_write_run(&d, RWY_IO_META, 100, buf, 0) == RWY_ERR_IO, "empty run is refused");
    CHECK(rwy_blk_write_run(&d, RWY_IO_META, last, buf, 2) == RWY_ERR_IO, "run past the end is refused");
    CHECK(rwy_blk_write_run(&d, RWY_IO_META, last, buf, 1) == RWY_OK, "last block is writable");
    CHECK(m->nlog == 2, "refused runs make no device call");
}

/* The five calls of a commit, in order. */
static void check_commit_calls(const mdev_t *m, const rwy_store_t *s, uint32_t old_sb_slot_blk, const char *what) {
    uint32_t tbl = s->sb.inode_tbl_start, bm = s->sb.bitmap_start;
    char msg[128];

    snprintf(msg, sizeof(msg), "%s: five calls", what);
    CHECK(m->nlog == 5, msg);
    if (m->nlog != 5) return;
    CHECK(m->log[0].kind == 'W' && m->log[0].blk == tbl && m->log[0].nblk == s->sb.inode_tbl_blocks,
          "inode table in one call");
    CHECK(m->log[1].kind == 'W' && m->log[1].blk == bm && m->log[1].nblk == s->sb.bitmap_blocks,
          "bitmap in one call");
    CHECK(m->log[2].kind == 'F', "flush before the superblock");
    CHECK(m->log[3].kind == 'W' && m->log[3].nblk == 1 && m->log[3].blk == old_sb_slot_blk,
          "superblock slot alone, in the other slot");
    CHECK(m->log[4].kind == 'F', "flush after the superblock");
}

static void test_call_counts(mdev_t *m) {
    rwy_block_dev_t d = dev_of(m);
    static const int sizes[] = { 1, 300 };

    printf("== calls of a commit ==\n");
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        uint32_t other_slot;
        fresh(m);
        CHECK(rwy_mount(&g_s, &d) == RWY_OK, "mount");
        CHECK(stage(&g_s, sizes[k]) == 0, "stage files");
        other_slot = g_s.sb_slot ? RWY_SB_SLOT_A : RWY_SB_SLOT_B;
        m->nlog = 0;
        CHECK(rwy_commit(&g_s) == RWY_OK, "commit");
        check_commit_calls(m, &g_s, other_slot, sizes[k] == 1 ? "one file" : "many files");
        CHECK(has_files(&g_s, sizes[k]), "files read back");
    }
}

/* Fail or tear the write at every point of a commit; the old state must stay mounted. */
static void test_failed_write(mdev_t *m, int point, uint32_t tear, const char *what) {
    rwy_block_dev_t d = dev_of(m);
    char msg[128];

    fresh(m);
    CHECK(rwy_mount(&g_s, &d) == RWY_OK, "mount");
    CHECK(stage(&g_s, 20) == 0, "stage files");
    m->nlog = 0;
    m->fail_write = point;
    m->tear = tear;
    snprintf(msg, sizeof(msg), "%s: commit fails", what);
    CHECK(rwy_commit(&g_s) != RWY_OK, msg);
    CHECK(!rwy_in_txn(&g_s), "failed commit ends the transaction");

    clear_faults(m);
    snprintf(msg, sizeof(msg), "%s: old state after remount", what);
    CHECK(rwy_mount(&g_s, &d) == RWY_OK && has_no_file(&g_s), msg);
    CHECK(stage(&g_s, 20) == 0 && rwy_commit(&g_s) == RWY_OK && has_files(&g_s, 20), "next commit works");
}

static void test_faults(mdev_t *m) {
    rwy_block_dev_t d = dev_of(m);
    uint32_t tbl_blocks;
    uint32_t tears[4];

    printf("== failed writes in a commit ==\n");
    fresh(m);
    CHECK(rwy_mount(&g_s, &d) == RWY_OK, "mount");
    tbl_blocks = g_s.sb.inode_tbl_blocks;
    tears[0] = 0; tears[1] = 1; tears[2] = tbl_blocks / 2; tears[3] = tbl_blocks - 1;
    for (int i = 0; i < 4; i++) test_failed_write(m, 1, tears[i], "torn inode table");
    test_failed_write(m, 2, 0, "bitmap");
    test_failed_write(m, 2, 1, "torn bitmap");
    test_failed_write(m, 3, 0, "superblock");

    printf("== failed flushes in a commit ==\n");
    fresh(m);
    CHECK(rwy_mount(&g_s, &d) == RWY_OK, "mount");
    CHECK(stage(&g_s, 20) == 0, "stage files");
    m->fail_flush = 1;
    CHECK(rwy_commit(&g_s) != RWY_OK, "first flush fails the commit");
    clear_faults(m);
    CHECK(rwy_mount(&g_s, &d) == RWY_OK && has_no_file(&g_s), "old state after a failed first flush");
    CHECK(stage(&g_s, 20) == 0, "stage files again");
    m->fail_flush = 2;
    CHECK(rwy_commit(&g_s) != RWY_OK, "second flush fails the commit");
    clear_faults(m);
    CHECK(rwy_mount(&g_s, &d) == RWY_OK && (has_no_file(&g_s) || has_files(&g_s, 20)),
          "old or new state after a failed second flush");
}

static void test_mirror(mdev_t *a, mdev_t *b) {
    static rwy_mirror_dev_t mir;
    rwy_block_dev_t d;
    int policy;

    printf("== run writes on a mirror ==\n");
    for (policy = 0; policy < 2; policy++) {
        fresh(a);
        fresh(b);
        rwy_mirror_init(&mir);
        d = dev_of(a); rwy_mirror_add(&mir, &d);
        d = dev_of(b); rwy_mirror_add(&mir, &d);
        mir.write_policy = policy ? RWY_MIRROR_WRITE_REQUIRE_ALL : RWY_MIRROR_WRITE_BEST_EFFORT;
        rwy_mirror_wrap(&d, &mir);
        CHECK(rwy_mount(&g_s, &d) == RWY_OK, "mount the mirror");
        CHECK(stage(&g_s, 20) == 0, "stage files");
        a->nlog = b->nlog = 0;
        b->fail_write = 1;                       /* the second member fails its inode table run */
        if (policy) {
            CHECK(rwy_commit(&g_s) != RWY_OK, "require-all: commit fails");
            CHECK(mir.write_errors[1] == 1 && mir.write_errors[0] == 0, "one error counted on the failing member");
        } else {
            CHECK(rwy_commit(&g_s) == RWY_OK, "best-effort: commit succeeds");
            CHECK(mir.write_errors[1] == 1 && mir.write_errors[0] == 0, "one error counted on the failing member");
            CHECK(a->nlog == 5 && b->nlog == 5, "both members see five calls");
        }
        clear_faults(a);
        d = dev_of(a);
        CHECK(rwy_mount(&g_s, &d) == RWY_OK && (policy ? has_no_file(&g_s) : has_files(&g_s, 20)),
              policy ? "primary keeps the old state" : "primary holds the new state");
    }
}

static uint64_t io_calls(const rwy_io_stats_t *st, int write, rwy_io_kind_t kind) {
    uint64_t n = 0;
    for (int b = 0; b < RWY_IO_BUCKETS; b++) n += st->calls[write][kind][b];
    return n;
}

static void test_io_stats(mdev_t *m) {
    rwy_block_dev_t d = dev_of(m);
    rwy_io_stats_t before;
    uint64_t wcalls = 0, wblocks = 0, flushes = 0;
    uint8_t buf[RWY_BLOCK_SIZE];
    size_t n = sizeof(buf);

    printf("== core counters ==\n");
    fresh(m);
    CHECK(rwy_mount(&g_s, &d) == RWY_OK, "mount");
    CHECK(io_calls(&g_s.io, 0, RWY_IO_META) > 0 && g_s.io.bytes[0][RWY_IO_META] > 0, "mount reads metadata");
    CHECK(io_calls(&g_s.io, 1, RWY_IO_META) == 0 && io_calls(&g_s.io, 1, RWY_IO_DATA) == 0, "mount writes nothing");

    CHECK(stage(&g_s, 3) == 0, "stage files");
    before = g_s.io;
    m->nlog = 0;
    CHECK(rwy_commit(&g_s) == RWY_OK, "commit");
    for (int i = 0; i < m->nlog && i < MAX_LOG; i++) {
        if (m->log[i].kind == 'W') { wcalls++; wblocks += m->log[i].nblk; }
        else flushes++;
    }
    CHECK(io_calls(&g_s.io, 1, RWY_IO_META) - io_calls(&before, 1, RWY_IO_META) == wcalls, "write calls match the device");
    CHECK(g_s.io.bytes[1][RWY_IO_META] - before.bytes[1][RWY_IO_META] == wblocks * RWY_BLOCK_SIZE,
          "write bytes match the device");
    CHECK(g_s.io.flushes - before.flushes == flushes, "flushes match the device");

    before = g_s.io;
    CHECK(rwy_read_path(&g_s, "F0.BIN", 0, buf, &n) == RWY_OK && n == sizeof(buf), "read a file");
    CHECK(g_s.io.bytes[0][RWY_IO_DATA] - before.bytes[0][RWY_IO_DATA] == RWY_BLOCK_SIZE, "one block of file data read");
    CHECK(io_calls(&g_s.io, 0, RWY_IO_DATA) - io_calls(&before, 0, RWY_IO_DATA) == 1, "in one call of the 4 KiB class");
    CHECK(g_s.io.calls[0][RWY_IO_DATA][0] - before.calls[0][RWY_IO_DATA][0] == 1, "counted in the first size class");

    {
        char text[1024], tiny[8];
        rwy_dev_stats_t dev = { 3, 2, 12288, 8192, 1, 5, 4 };
        rwy_io_stats_t io;

        memset(&io, 0, sizeof(io));
        io.calls[0][RWY_IO_DATA][0] = 1;
        io.calls[0][RWY_IO_DATA][2] = 2;
        io.bytes[0][RWY_IO_DATA] = 2101248;
        io.flushes = 2;
        n = rwy_io_format(text, sizeof(text), &io, &dev);
        CHECK(n == strlen(text), "the text length is returned");
        CHECK(strstr(text, "core read data: 3 calls (4K 1, 64K 0, 1M 2, big 0), 2101248 bytes\n"), "a read row");
        CHECK(strstr(text, "core flushes: 2\n"), "the flushes");
        CHECK(strstr(text, "dev: reads 3 (12288 bytes), writes 2 (8192 bytes), flushes 1, window hits 5, fills 4\n"),
              "the device line");
        CHECK(!strstr(text, "core write dir"), "a row without calls is left out");
        n = rwy_io_format(tiny, sizeof(tiny), &io, NULL);
        CHECK(n < sizeof(tiny) && n == strlen(tiny), "the text is cut to fit");
    }
    CHECK(rwy_mount(&g_s, &d) == RWY_OK && io_calls(&g_s.io, 1, RWY_IO_DATA) == 0, "a new mount starts at zero");
}

int main(void) {
    mdev_t m = { 0 }, m2 = { 0 };

    m.mem = malloc(STORE_BYTES);
    m2.mem = malloc(STORE_BYTES);
    if (!m.mem || !m2.mem) return 1;
    load_base();
    test_run_write(&m);
    test_call_counts(&m);
    test_faults(&m);
    test_mirror(&m, &m2);
    test_io_stats(&m);
    printf(failures ? "\nCOMMITTEST: FAIL (%d)\n" : "\nCOMMITTEST: PASS\n", failures);
    return failures != 0;
}
