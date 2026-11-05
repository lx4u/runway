/*
 * defragtest — tests for `fsck.runway --defrag` (offline compaction).
 * SPDX-License-Identifier: Apache-2.0
 *
 *  1. compaction: checkerboard-fragmented store compacts so every object
 *     is in a single contiguous run, zero leftovers, byte-identical contents,
 *     remount OK, plain check clean afterwards.
 *  2. NOSPC recovery: craft the documented failure (large contiguous request
 *     fails with free blocks scattered), defrag, verify the same request now
 *     succeeds.
 *  3. large files: 200 MiB object preserved with pattern checksums intact
 *     under the 2x-space CoW constraint; fragmented large file compacts to 1 run.
 *  4. no-op: defrag on an already-compact store publishes no generation,
 *     bit-identical.
 *  5. refusal: each corruption class the checker detects exits 4 with the
 *     store bit-identical.
 *  6. fault injection: fail every write/flush/publication point during --defrag
 *     (via RWY_CHECK_FI_AT); every run remounts with contents intact, retry converges.
 *  7. idempotency: defrag twice; second run is a clean no-op.
 *
 * Usage: defragtest STORE RWY_CHECK_BIN
 * Works on private copies; never mutates STORE.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "testdir.h"
#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_format.h"

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

static void copy_file(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "wb");
    char buf[1 << 20];
    size_t n;
    if (!fa || !fb) { fprintf(stderr, "copy failed %s -> %s\n", a, b); exit(1); }
    while ((n = fread(buf, 1, sizeof(buf), fa)) > 0)
        if (fwrite(buf, 1, n, fb) != n) { fprintf(stderr, "copy failed\n"); exit(1); }
    fclose(fa); fclose(fb);
}

static int files_identical(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    char ba[1 << 20], bb[1 << 20];
    size_t na, nb;
    int eq = 1;
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
    for (;;) {
        na = fread(ba, 1, sizeof(ba), fa);
        nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || memcmp(ba, bb, na)) { eq = 0; break; }
        if (na == 0) break;
    }
    fclose(fa); fclose(fb);
    return eq;
}

static void open_store(rwy_store_t *s, rwy_block_dev_t *dev, file_t *fl,
                       const char *path, const char *mode) {
    fl->f = fopen(path, mode);
    if (!fl->f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    dev->read = file_rd; dev->write = file_wr; dev->flush = file_fl;
    dev->ctx = fl; dev->size = RWY_STORE_SIZE; dev->block_size = RWY_BLOCK_SIZE;
    if (rwy_mount(s, dev)) { fprintf(stderr, "mount failed: %s\n", path); exit(1); }
}

static int run_status(const char *cmd) {
    int rc = system(cmd);
    if (rc == -1) return -1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return -1;
}

static int run_check(const char *img) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s %s > %s 2>&1",
             g_checkbin, img, tpath("rwy_df_check.log"));
    return run_status(cmd);
}

static int run_defrag(const char *img) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s --defrag %s > %s 2>&1",
             g_checkbin, img, tpath("rwy_df_run.log"));
    return run_status(cmd);
}

static int run_defrag_fi(const char *img, long at) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "RWY_CHECK_FI_AT=%ld %s --defrag %s > %s 2>&1",
             at, g_checkbin, img, tpath("rwy_df_fi.log"));
    return run_status(cmd);
}

static uint64_t get_gen(const char *img) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    uint64_t g;
    open_store(&s, &dev, &fl, img, "rb");
    g = s.sb.generation;
    fclose(fl.f);
    return g;
}

static uint32_t get_reclaimable(const char *img) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    uint32_t r;
    open_store(&s, &dev, &fl, img, "rb");
    r = rwy_reclaimable(&s);
    fclose(fl.f);
    return r;
}

static uint32_t get_frag_objects(const char *img) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    rwy_frag_info_t fi;
    uint32_t fo = 0;
    open_store(&s, &dev, &fl, img, "rb");
    if (rwy_frag_info(&s, &fi) == RWY_OK) fo = fi.frag_objects;
    fclose(fl.f);
    return fo;
}

/* ---------- visible-content snapshots (byte-identical comparison) ---------- */
#define SNAP_MAX 2048
typedef struct {
    char path[RWY_MAX_PATH + 8];
    uint8_t type;
    uint64_t size;
    uint64_t h1, h2;
} entry_t;
typedef struct { entry_t e[SNAP_MAX]; int n; } snap_t;

static void hash_bytes(const uint8_t *p, size_t n, uint64_t *h1, uint64_t *h2) {
    size_t i;
    for (i = 0; i < n; i++) {
        *h1 ^= p[i]; *h1 *= 1099511628211ULL;
        *h2 ^= p[i] + 0x9e3779b9u; *h2 *= 16777619ULL;
    }
}

static int snap_add(snap_t *sn, rwy_store_t *s, const char *path) {
    rwy_attr_t a;
    entry_t *e;
    if (sn->n >= SNAP_MAX) return -1;
    if (rwy_getattr_path(s, path, &a) != RWY_OK) return -1;
    e = &sn->e[sn->n++];
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->type = a.type; e->size = a.size;
    e->h1 = 1469598103934665603ULL; e->h2 = 2166136261ULL;
    if (a.type == RWY_TYPE_FILE) {
        static uint8_t buf[1 << 20];
        uint64_t off = 0;
        while (off < a.size) {
            size_t want = sizeof(buf), got = want;
            uint64_t rem = a.size - off;
            if (rem < want) want = (size_t)rem;
            got = want;
            if (rwy_read_path(s, path, off, buf, &got) != RWY_OK) return -1;
            if (got != want) return -1;
            hash_bytes(buf, got, &e->h1, &e->h2);
            off += got;
        }
    } else if (a.type == RWY_TYPE_SYMLINK) {
        char tgt[4096];
        if (rwy_readlink(s, path, tgt, sizeof(tgt)) != RWY_OK) return -1;
        hash_bytes((uint8_t *)tgt, strlen(tgt), &e->h1, &e->h2);
    } else {
        char names[65536];
        size_t cnt = 4096;
        if (rwy_readdir(s, path, names, sizeof(names), &cnt, 0) != RWY_OK)
            return -1;
        hash_bytes((uint8_t *)names, strlen(names), &e->h1, &e->h2);
        {
            size_t off = 0, j;
            e->h1 = 1469598103934665603ULL; e->h2 = 2166136261ULL;
            for (j = 0; j < cnt; j++) {
                size_t l = strlen(names + off);
                hash_bytes((uint8_t *)(names + off), l + 1, &e->h1, &e->h2);
                off += l + 1;
            }
        }
    }
    return 0;
}

static int snap_crawl_dir(snap_t *sn, rwy_store_t *s, const char *path) {
    char names[65536];
    size_t cnt = 4096, off = 0, j;
    if (rwy_readdir(s, path, names, sizeof(names), &cnt, 0) != RWY_OK)
        return -1;
    for (j = 0; j < cnt; j++) {
        char child[RWY_MAX_PATH * 2];
        rwy_attr_t ca;
        uint64_t co;
        size_t l = strlen(names + off);
        int n = path[0] ? snprintf(child, sizeof(child), "%s/%s", path, names + off)
                        : snprintf(child, sizeof(child), "%s", names + off);
        if (n < 0 || (size_t)n >= sizeof(child)) return -1; /* path too long */
        off += l + 1;
        if (rwy_lookup(s, child, &co) != RWY_OK) return -2;
        if (rwy_getattr(s, co, &ca) != RWY_OK) return -1;
        if (snap_add(sn, s, child) != 0) return -1;
        if (ca.type == RWY_TYPE_DIR && snap_crawl_dir(sn, s, child) != 0)
            return -1;
    }
    return 0;
}

static int take_snap(const char *img, snap_t *sn) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    int rc = 0;
    sn->n = 0;
    open_store(&s, &dev, &fl, img, "rb");
    if (snap_add(sn, &s, "") != 0) rc = -1;
    else if (snap_crawl_dir(sn, &s, "") != 0) rc = -1;
    fclose(fl.f);
    return rc;
}

static int cmp_entry(const void *a, const void *b) {
    return strcmp(((const entry_t *)a)->path, ((const entry_t *)b)->path);
}

static int snaps_equal(snap_t *a, snap_t *b) {
    int i;
    if (a->n != b->n) return 0;
    qsort(a->e, (size_t)a->n, sizeof(entry_t), cmp_entry);
    qsort(b->e, (size_t)b->n, sizeof(entry_t), cmp_entry);
    for (i = 0; i < a->n; i++)
        if (strcmp(a->e[i].path, b->e[i].path) || a->e[i].type != b->e[i].type ||
            a->e[i].size != b->e[i].size || a->e[i].h1 != b->e[i].h1 ||
            a->e[i].h2 != b->e[i].h2)
            return 0;
    return 1;
}

/* ---------- deterministic pattern files ---------- */
static void fill_pattern(uint8_t *buf, uint64_t off, size_t len, uint8_t seed) {
    size_t i;
    for (i = 0; i < len; i++) buf[i] = (uint8_t)((off + i + seed) & 0xFF);
}

static int verify_pattern(rwy_store_t *s, const char *path, uint64_t size, uint8_t seed) {
    uint8_t buf[65536], expect[65536];
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

/* ---------- raw-image corruption crafters ---------- */
static void read_blk(const char *img, uint32_t blk, uint8_t out[RWY_BLOCK_SIZE]) {
    FILE *f = fopen(img, "rb");
    if (!f) { perror("read"); exit(1); }
    if (fseek(f, (long)blk * RWY_BLOCK_SIZE, SEEK_SET)) { perror("seek"); exit(1); }
    if (fread(out, 1, RWY_BLOCK_SIZE, f) != RWY_BLOCK_SIZE) {
        perror("read"); exit(1);
    }
    fclose(f);
}

static void write_blk(const char *img, uint32_t blk, const uint8_t in[RWY_BLOCK_SIZE]) {
    FILE *f = fopen(img, "r+b");
    if (!f) { perror("write"); exit(1); }
    if (fseek(f, (long)blk * RWY_BLOCK_SIZE, SEEK_SET)) { perror("seek"); exit(1); }
    if (fwrite(in, 1, RWY_BLOCK_SIZE, f) != RWY_BLOCK_SIZE) {
        perror("write"); exit(1);
    }
    fclose(f);
}

static void fix_blk_crc(uint8_t blk[RWY_BLOCK_SIZE]) {
    uint32_t c;
    blk[24] = blk[25] = blk[26] = blk[27] = 0;
    c = rwy_crc32c(blk, RWY_BLOCK_SIZE);
    blk[24] = (uint8_t)c; blk[25] = (uint8_t)(c >> 8);
    blk[26] = (uint8_t)(c >> 16); blk[27] = (uint8_t)(c >> 24);
}

static void pick_sb(const char *img, rwy_superblock_t *out) {
    uint8_t ra[RWY_BLOCK_SIZE], rb[RWY_BLOCK_SIZE];
    rwy_superblock_t a, b;
    int va, vb;
    read_blk(img, 0, ra);
    read_blk(img, 1, rb);
    memcpy(&a, ra, sizeof(a));
    memcpy(&b, rb, sizeof(b));
    va = (rwy_validate_sb(&a) == RWY_OK);
    vb = (rwy_validate_sb(&b) == RWY_OK);
    if (!va && !vb) { fprintf(stderr, "no valid sb in %s\n", img); exit(1); }
    if (va && vb) *out = (a.generation >= b.generation) ? a : b;
    else if (va) *out = a;
    else *out = b;
}

static void read_inode_raw(const char *img, const rwy_superblock_t *sb,
                           uint64_t oid, rwy_inode_t *out) {
    uint8_t blk[RWY_BLOCK_SIZE];
    uint32_t idx = (uint32_t)(oid - 1) / RWY_INODES_PER_BLOCK;
    uint32_t sub = (uint32_t)(oid - 1) % RWY_INODES_PER_BLOCK;
    read_blk(img, sb->inode_tbl_start + idx, blk);
    memcpy(out, blk + 32 + sub * RWY_INODE_SIZE, sizeof(*out));
}

static void write_inode_raw(const char *img, const rwy_superblock_t *sb,
                            uint64_t oid, const rwy_inode_t *in) {
    uint8_t blk[RWY_BLOCK_SIZE];
    uint32_t idx = (uint32_t)(oid - 1) / RWY_INODES_PER_BLOCK;
    uint32_t sub = (uint32_t)(oid - 1) % RWY_INODES_PER_BLOCK;
    read_blk(img, sb->inode_tbl_start + idx, blk);
    memcpy(blk + 32 + sub * RWY_INODE_SIZE, in, sizeof(*in));
    fix_blk_crc(blk);
    write_blk(img, sb->inode_tbl_start + idx, blk);
}

static void bitmap_clear(const char *img, const rwy_superblock_t *sb, uint32_t b) {
    uint8_t blk[RWY_BLOCK_SIZE];
    uint32_t byte = b >> 3, per = RWY_BLOCK_SIZE - 32;
    uint32_t bi = byte / per, off = 32 + byte % per;
    read_blk(img, sb->bitmap_start + bi, blk);
    blk[off] &= (uint8_t)~(1u << (b & 7));
    fix_blk_crc(blk);
    write_blk(img, sb->bitmap_start + bi, blk);
}

static uint64_t oid_of(const char *img, const char *path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    uint64_t o;
    open_store(&s, &dev, &fl, img, "rb");
    if (rwy_lookup(&s, path, &o) != RWY_OK) {
        printf("lookup %s failed\n", path); exit(1);
    }
    fclose(fl.f);
    return o;
}

static void craft_torn_metadata(const char *img) {
    rwy_superblock_t sb;
    uint8_t blk[RWY_BLOCK_SIZE];
    pick_sb(img, &sb);
    read_blk(img, sb.inode_tbl_start, blk);
    blk[100] ^= 0xFF;
    write_blk(img, sb.inode_tbl_start, blk);
}

static void craft_double_alloc(const char *img) {
    rwy_superblock_t sb;
    rwy_inode_t a, b;
    uint64_t oa, ob;
    pick_sb(img, &sb);
    oa = oid_of(img, "F00.DAT");
    ob = oid_of(img, "F01.DAT");
    read_inode_raw(img, &sb, oa, &a);
    read_inode_raw(img, &sb, ob, &b);
    if (a.extent_count == 0 || b.extent_count == 0) {
        printf("need extents for double-alloc\n"); exit(1);
    }
    b.extents[0].start = a.extents[0].start;
    b.extents[0].len = a.extents[0].len;
    write_inode_raw(img, &sb, ob, &b);
}

static void craft_unmarked(const char *img) {
    rwy_superblock_t sb;
    rwy_inode_t a;
    uint64_t oa;
    pick_sb(img, &sb);
    oa = oid_of(img, "F00.DAT");
    read_inode_raw(img, &sb, oa, &a);
    if (a.extent_count == 0) { printf("need extent for unmarked\n"); exit(1); }
    bitmap_clear(img, &sb, a.extents[0].start);
}

static void craft_dangling(const char *img) {
    rwy_superblock_t sb;
    rwy_inode_t root;
    uint8_t blk[RWY_BLOCK_SIZE];
    pick_sb(img, &sb);
    read_inode_raw(img, &sb, 1, &root);
    if (root.extent_count == 0) { printf("root has no data\n"); exit(1); }
    read_blk(img, root.extents[0].start, blk);
    memset(blk, 0, 8);
    blk[0] = 0x84; blk[1] = 0x03; /* oid 900 */
    write_blk(img, root.extents[0].start, blk);
}

static void craft_bad_gen(const char *img) {
    uint8_t blk[RWY_BLOCK_SIZE];
    read_blk(img, 0, blk);
    blk[8] ^= 0xFF;
    write_blk(img, 0, blk);
    read_blk(img, 1, blk);
    blk[8] ^= 0xFF;
    write_blk(img, 1, blk);
}

static void craft_reserved(const char *img) {
    rwy_superblock_t sb;
    rwy_inode_t a;
    uint64_t oa;
    pick_sb(img, &sb);
    oa = oid_of(img, "F00.DAT");
    read_inode_raw(img, &sb, oa, &a);
    a.xattr_root = 1;
    write_inode_raw(img, &sb, oa, &a);
}

/* Helper to build a fragmented store: N files written interleaved across K passes. */
static void build_fragmented_store(const char *img, int nfiles, int passes) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    int p, i;
    uint8_t w[4096];
    open_store(&s, &dev, &fl, img, "r+b");
    for (i = 0; i < nfiles; i++) {
        char name[64];
        snprintf(name, sizeof(name), "F%02d.DAT", i);
        if (rwy_txn_create(&s, name, RWY_S_IFREG | 0644, 0, 0) != RWY_OK) {
            printf("create %s failed\n", name); exit(1);
        }
    }
    for (p = 0; p < passes; p++) {
        for (i = 0; i < nfiles; i++) {
            char name[64];
            snprintf(name, sizeof(name), "F%02d.DAT", i);
            fill_pattern(w, (uint64_t)p * sizeof(w), sizeof(w), (uint8_t)(i * 10 + p));
            if (rwy_txn_write_path(&s, name, (uint64_t)p * sizeof(w), w, sizeof(w)) != RWY_OK) {
                printf("write pass %d %s failed\n", p, name); exit(1);
            }
        }
    }
    fclose(fl.f);
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

/* ================= 1. Compaction ================= */
static void test_compaction(const char *store_path) {
    snap_t before, after;
    uint64_t gen0, gen1;
    uint32_t fo_before, fo_after;
    printf("== defrag compaction ==\n");
    copy_file(store_path, tpath("rwy_df_comp.img"));
    build_fragmented_store(tpath("rwy_df_comp.img"), 20, 3);
    fo_before = get_frag_objects(tpath("rwy_df_comp.img"));
    printf("fragmented objects before: %u\n", fo_before);
    CHECK(fo_before == 20, "expected 20 fragmented objects");
    CHECK(take_snap(tpath("rwy_df_comp.img"), &before) == 0, "snap before");
    gen0 = get_gen(tpath("rwy_df_comp.img"));
    CHECK(run_defrag(tpath("rwy_df_comp.img")) == 0, "defrag exits 0");
    gen1 = get_gen(tpath("rwy_df_comp.img"));
    CHECK(gen1 == gen0 + 1, "published exactly one new generation");
    fo_after = get_frag_objects(tpath("rwy_df_comp.img"));
    CHECK(fo_after == 0, "zero fragmented objects after defrag");
    CHECK(get_reclaimable(tpath("rwy_df_comp.img")) == 0, "zero leftovers after defrag");
    CHECK(take_snap(tpath("rwy_df_comp.img"), &after) == 0, "snap after");
    CHECK(snaps_equal(&before, &after), "contents byte-identical");
    /* Verify directly that every live file is in a single contiguous run. */
    {
        file_t fl;
        rwy_block_dev_t dev;
        rwy_store_t s;
        int i;
        open_store(&s, &dev, &fl, tpath("rwy_df_comp.img"), "rb");
        for (i = 0; i < 20; i++) {
            char name[64];
            uint64_t o;
            rwy_attr_t a;
            snprintf(name, sizeof(name), "F%02d.DAT", i);
            CHECK(rwy_lookup(&s, name, &o) == RWY_OK, "lookup post-defrag");
            CHECK(rwy_getattr(&s, o, &a) == RWY_OK, "getattr post-defrag");
            CHECK(s.inodes[o - 1].extent_count == 1, "extent_count is 1");
            CHECK(s.inodes[o - 1].indirect == 0, "no indirect block");
            CHECK(s.inodes[o - 1].extents[0].len == 3, "extent len is 3");
        }
        fclose(fl.f);
    }
    CHECK(run_check(tpath("rwy_df_comp.img")) == 0, "plain check clean after defrag");
    printf("== compaction OK ==\n");
}

/* ================= 2. NOSPC Recovery ================= */
static void test_nospc_recovery(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    rwy_frag_info_t fi;
    printf("== defrag NOSPC recovery ==\n");
    copy_file(store_path, tpath("rwy_df_nospc.img"));
    /* Clear large perf files so pool has space to work. */
    open_store(&s, &dev, &fl, tpath("rwy_df_nospc.img"), "r+b");
    rwy_txn_unlink(&s, "PERF/READ1M.BIN");
    rwy_txn_unlink(&s, "PERF/READ16M.BIN");
    rwy_txn_unlink(&s, "PERF/READ64M.BIN");
    CHECK(rwy_gc(&s) == RWY_OK, "gc cleared space");
    fclose(fl.f);

    /* Craft interleaved files: 40 files, 4 blocks each, written interleaved. */
    build_fragmented_store(tpath("rwy_df_nospc.img"), 40, 4);

    /* Delete every other file to create holes of 1 block scattered everywhere. */
    open_store(&s, &dev, &fl, tpath("rwy_df_nospc.img"), "r+b");
    for (int i = 0; i < 40; i += 2) {
        char name[64];
        snprintf(name, sizeof(name), "F%02d.DAT", i);
        CHECK(rwy_txn_unlink(&s, name) == RWY_OK, "unlink even file");
    }
    CHECK(rwy_gc(&s) == RWY_OK, "gc to make deleted holes free");

    /* Consume the large free run at the tail, keeping a 100-block run for the defrag to work in. */
    CHECK(rwy_frag_info(&s, &fi) == RWY_OK, "frag info");
    if (fi.largest_free_run > 10) {
        uint64_t fill_sz = (uint64_t)(fi.largest_free_run - 2 - 100) * RWY_BLOCK_SIZE;
        CHECK(rwy_txn_create(&s, "TAIL_FILL.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create tail fill");
        CHECK(write_pattern_file(&s, "TAIL_FILL.BIN", fill_sz, 0x99) == RWY_OK, "write tail fill");
    }
    CHECK(rwy_frag_info(&s, &fi) == RWY_OK, "frag info post fill");
    uint32_t need_blocks = fi.largest_free_run + 20;
    printf("free blocks: %u, largest free run: %u, contiguous request: %u blocks\n",
           fi.free_blocks, fi.largest_free_run, need_blocks);
    CHECK(fi.free_blocks > need_blocks + 20, "plenty of free blocks exist overall");

    /* No free run is long enough for `need_blocks`: the write is split over several runs. */
    CHECK(rwy_txn_create(&s, "CONTIG_PROBE.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create contig probe");
    {
        size_t psz = (size_t)need_blocks * RWY_BLOCK_SIZE;
        uint8_t *w = malloc(psz);
        int rc;
        if (!w) { fprintf(stderr, "oom\n"); exit(1); }
        fill_pattern(w, 0, psz, 0x77);
        rc = rwy_begin(&s);
        if (!rc) rc = rwy_write_path(&s, "CONTIG_PROBE.BIN", 0, w, psz);
        if (s.in_txn) rwy_abort(&s);
        free(w);
        CHECK(rc == RWY_OK, "a request longer than the largest free run is split");
    }
    CHECK(rwy_txn_unlink(&s, "CONTIG_PROBE.BIN") == RWY_OK, "drop probe");
    fclose(fl.f);

    /* Run defrag! */
    CHECK(run_defrag(tpath("rwy_df_nospc.img")) == 0, "defrag exits 0");

    /* Now the exact same contiguous request MUST SUCCEED! */
    open_store(&s, &dev, &fl, tpath("rwy_df_nospc.img"), "r+b");
    CHECK(rwy_frag_info(&s, &fi) == RWY_OK, "frag info post defrag");
    printf("post-defrag largest free run: %u\n", fi.largest_free_run);
    CHECK(fi.largest_free_run >= need_blocks, "largest free run now >= need_blocks");
    CHECK(rwy_txn_create(&s, "CONTIG_PROBE.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create contig probe retry");
    {
        size_t psz = (size_t)need_blocks * RWY_BLOCK_SIZE;
        CHECK(write_pattern_file(&s, "CONTIG_PROBE.BIN", psz, 0x77) == RWY_OK, "contiguous write now succeeds!");
        CHECK(verify_pattern(&s, "CONTIG_PROBE.BIN", psz, 0x77) == 0, "probe verified");
    }
    fclose(fl.f);

    CHECK(run_check(tpath("rwy_df_nospc.img")) == 0, "check clean post NOSPC recovery");
    printf("== NOSPC recovery OK ==\n");
}

/* ================= 3. Large Files ================= */
#define LARGE_BYTES (200ULL << 20)
static void test_large_files(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    printf("== defrag large files (200 MiB CoW + 20 MiB compact) ==\n");
    copy_file(store_path, tpath("rwy_df_large.img"));
    open_store(&s, &dev, &fl, tpath("rwy_df_large.img"), "r+b");
    /* Clear seed payloads so a 200 MiB file fits the 256 MiB store. */
    rwy_txn_unlink(&s, "PERF/READ1M.BIN");
    rwy_txn_unlink(&s, "PERF/READ16M.BIN");
    rwy_txn_unlink(&s, "PERF/READ64M.BIN");
    rwy_txn_unlink(&s, "README.TXT");
    rwy_txn_unlink(&s, "TEST/HELLO.TXT");
    rwy_txn_unlink(&s, "EFI/TEST/SAMPLE.EFI");
    CHECK(rwy_gc(&s) == RWY_OK, "gc after clear");

    /* Create 200 MiB contiguous file. */
    CHECK(rwy_txn_create(&s, "LARGE.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create large");
    CHECK(write_pattern_file(&s, "LARGE.BIN", LARGE_BYTES, 0x33) == RWY_OK, "write 200 MiB");
    CHECK(verify_pattern(&s, "LARGE.BIN", LARGE_BYTES, 0x33) == 0, "verify large");

    /* Create a fragmented companion file: 2 passes of 2 blocks interleaved with another file. */
    CHECK(rwy_txn_create(&s, "COMPANION.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create companion");
    CHECK(rwy_txn_create(&s, "TMP.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create tmp");
    {
        uint8_t w[8192];
        fill_pattern(w, 0, sizeof(w), 0x44);
        CHECK(rwy_txn_write_path(&s, "COMPANION.BIN", 0, w, 4096) == RWY_OK, "comp p0");
        CHECK(rwy_txn_write_path(&s, "TMP.BIN", 0, w, 4096) == RWY_OK, "tmp p0");
        CHECK(rwy_txn_write_path(&s, "COMPANION.BIN", 4096, w + 4096, 4096) == RWY_OK, "comp p1");
        CHECK(rwy_txn_write_path(&s, "TMP.BIN", 4096, w + 4096, 4096) == RWY_OK, "tmp p1");
    }
    CHECK(rwy_txn_unlink(&s, "TMP.BIN") == RWY_OK, "drop tmp");

    /* Verify COMPANION is fragmented and LARGE is not. */
    {
        uint64_t o_lg, o_cp;
        CHECK(rwy_lookup(&s, "LARGE.BIN", &o_lg) == RWY_OK, "lookup large");
        CHECK(rwy_lookup(&s, "COMPANION.BIN", &o_cp) == RWY_OK, "lookup comp");
        CHECK(s.inodes[o_lg - 1].extent_count == 1, "large is single run");
        CHECK(s.inodes[o_cp - 1].extent_count > 1, "companion is fragmented");
    }
    fclose(fl.f);

    /* Run defrag: under CoW, 200 MiB file cannot be duplicated (needs 400 MiB),
     * so it must be left in place while COMPANION is defragmented! */
    CHECK(run_defrag(tpath("rwy_df_large.img")) == 0, "defrag with 200 MiB file exits 0");

    /* Verify pattern checksums for 200 MiB file are 100% intact! */
    open_store(&s, &dev, &fl, tpath("rwy_df_large.img"), "rb");
    CHECK(verify_pattern(&s, "LARGE.BIN", LARGE_BYTES, 0x33) == 0, "200 MiB checksums intact");
    CHECK(verify_pattern(&s, "COMPANION.BIN", 8192, 0x44) == 0, "companion checksums intact");
    {
        uint64_t o_cp;
        CHECK(rwy_lookup(&s, "COMPANION.BIN", &o_cp) == RWY_OK, "lookup comp");
        CHECK(s.inodes[o_cp - 1].extent_count == 1, "companion is now single run");
    }
    fclose(fl.f);
    CHECK(run_check(tpath("rwy_df_large.img")) == 0, "check clean with 200 MiB file");

    /* Also test a 20 MiB file that IS fragmented and compacts to 1 run. */
    copy_file(store_path, tpath("rwy_df_large20.img"));
    open_store(&s, &dev, &fl, tpath("rwy_df_large20.img"), "r+b");
    rwy_txn_unlink(&s, "PERF/READ1M.BIN");
    rwy_txn_unlink(&s, "PERF/READ16M.BIN");
    rwy_txn_unlink(&s, "PERF/READ64M.BIN");
    CHECK(rwy_gc(&s) == RWY_OK, "gc");
    CHECK(rwy_txn_create(&s, "FRAG20M.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create 20M");
    CHECK(rwy_txn_create(&s, "OTHER.BIN", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create other");
    {
        /* 4 chunks of 5 MiB interleaved with OTHER.BIN */
        size_t chunk = 5 << 20;
        uint8_t *cbuf = malloc(chunk);
        if (!cbuf) { fprintf(stderr, "oom\n"); exit(1); }
        fill_pattern(cbuf, 0, chunk, 0x66);
        for (int p = 0; p < 4; p++) {
            CHECK(rwy_txn_write_path(&s, "FRAG20M.BIN", (uint64_t)p * chunk, cbuf, chunk) == RWY_OK, "w frag20");
            CHECK(rwy_txn_write_path(&s, "OTHER.BIN", (uint64_t)p * 4096, cbuf, 4096) == RWY_OK, "w other");
        }
        free(cbuf);
    }
    {
        uint64_t o;
        CHECK(rwy_lookup(&s, "FRAG20M.BIN", &o) == RWY_OK, "lookup 20M");
        CHECK(s.inodes[o - 1].extent_count > 1, "20M is fragmented");
    }
    fclose(fl.f);

    CHECK(run_defrag(tpath("rwy_df_large20.img")) == 0, "defrag 20M exits 0");
    open_store(&s, &dev, &fl, tpath("rwy_df_large20.img"), "rb");
    {
        uint64_t o;
        CHECK(rwy_lookup(&s, "FRAG20M.BIN", &o) == RWY_OK, "lookup 20M post");
        CHECK(s.inodes[o - 1].extent_count == 1, "20M compacted to 1 run");
        CHECK(s.inodes[o - 1].indirect == 0, "no indirect on 20M");
    }
    CHECK(verify_pattern(&s, "FRAG20M.BIN", 20 << 20, 0x66) == 0, "20M pattern intact");
    fclose(fl.f);
    CHECK(run_check(tpath("rwy_df_large20.img")) == 0, "check clean 20M");
    printf("== large files OK ==\n");
}

/* ================= 4. No-op ================= */
static void test_noop(const char *store_path) {
    uint64_t gen0, gen1;
    printf("== defrag no-op ==\n");
    copy_file(store_path, tpath("rwy_df_noop.img"));
    gen0 = get_gen(tpath("rwy_df_noop.img"));
    copy_file(tpath("rwy_df_noop.img"), tpath("rwy_df_noop_pre.img"));
    CHECK(run_defrag(tpath("rwy_df_noop.img")) == 0, "no-op defrag exits 0");
    gen1 = get_gen(tpath("rwy_df_noop.img"));
    CHECK(gen1 == gen0, "no-op defrag publishes no generation");
    CHECK(files_identical(tpath("rwy_df_noop_pre.img"), tpath("rwy_df_noop.img")),
          "no-op defrag bit-identical");
    printf("== no-op OK ==\n");
}

/* ================= 5. Refusal ================= */
static void test_one_refusal(const char *store_path, const char *name,
                             void (*craft)(const char *)) {
    printf("-- refusal: %s --\n", name);
    copy_file(store_path, tpath("rwy_df_ref.img"));
    build_fragmented_store(tpath("rwy_df_ref.img"), 6, 2);
    craft(tpath("rwy_df_ref.img"));
    copy_file(tpath("rwy_df_ref.img"), tpath("rwy_df_ref_pre.img"));
    {
        int plain = run_check(tpath("rwy_df_ref.img"));
        CHECK(plain == 4, "plain check reports corruption");
    }
    CHECK(run_defrag(tpath("rwy_df_ref.img")) == 4, "defrag refuses (exit 4)");
    CHECK(files_identical(tpath("rwy_df_ref_pre.img"), tpath("rwy_df_ref.img")),
          "refused store bit-identical");
}

static void test_refusal(const char *store_path) {
    printf("== defrag refusal ==\n");
    test_one_refusal(store_path, "torn metadata", craft_torn_metadata);
    test_one_refusal(store_path, "double allocation", craft_double_alloc);
    test_one_refusal(store_path, "unmarked-but-referenced", craft_unmarked);
    test_one_refusal(store_path, "dangling dirent", craft_dangling);
    test_one_refusal(store_path, "bad generation", craft_bad_gen);
    test_one_refusal(store_path, "nonzero reserved", craft_reserved);
    printf("== refusal OK ==\n");
}

/* ================= 6. Fault Injection ================= */
static long defrag_points(const char *img) {
    char cmd[2048];
    FILE *p;
    long n = -1;
    snprintf(cmd, sizeof(cmd),
             "RWY_CHECK_FI_AT=-1 RWY_CHECK_FI_VERBOSE=1 %s --defrag %s 2>&1",
             g_checkbin, img);
    p = popen(cmd, "r");
    if (!p) { printf("popen failed\n"); exit(1); }
    {
        char line[512];
        while (fgets(line, sizeof(line), p))
            if (sscanf(line, "defrag io points: %ld", &n) == 1) break;
    }
    pclose(p);
    return n;
}

static void test_faults(const char *store_path) {
    snap_t base;
    long npoints, at;
    printf("== defrag fault injection ==\n");
    copy_file(store_path, tpath("rwy_df_fi.img"));
    build_fragmented_store(tpath("rwy_df_fi.img"), 4, 2);
    {
        file_t fl;
        rwy_block_dev_t dev;
        rwy_store_t s;
        uint8_t w[4096];
        open_store(&s, &dev, &fl, tpath("rwy_df_fi.img"), "r+b");
        CHECK(rwy_txn_create(&s, "CANARY.DAT", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "canary create");
        fill_pattern(w, 0, sizeof(w), 0xDE);
        CHECK(rwy_txn_write_path(&s, "CANARY.DAT", 0, w, sizeof(w)) == RWY_OK, "canary write");
        fclose(fl.f);
    }
    CHECK(take_snap(tpath("rwy_df_fi.img"), &base) == 0, "snap base");
    copy_file(tpath("rwy_df_fi.img"), tpath("rwy_df_fi_cnt.img"));
    npoints = defrag_points(tpath("rwy_df_fi_cnt.img"));
    printf("defrag persistence points: %ld\n", npoints);
    CHECK(npoints > 5, "expect defrag points > 5 (repair has 5; defrag moves data blocks)");

    for (at = 1; at <= npoints; at++) {
        snap_t cur;
        int rc;
        copy_file(tpath("rwy_df_fi.img"), tpath("rwy_df_fi_one.img"));
        rc = run_defrag_fi(tpath("rwy_df_fi_one.img"), at);
        (void)rc;
        /* Recovery: store must remount and visible contents must remain intact! */
        {
            file_t fl;
            rwy_block_dev_t dev;
            rwy_store_t s;
            fl.f = fopen(tpath("rwy_df_fi_one.img"), "r+b");
            dev.read = file_rd; dev.write = file_wr; dev.flush = file_fl;
            dev.ctx = &fl; dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
            if (rwy_mount(&s, &dev) != RWY_OK) {
                printf("FAIL: remount after defrag fault at=%ld\n", at);
                failures++;
                fclose(fl.f);
                continue;
            }
            fclose(fl.f);
        }
        CHECK(take_snap(tpath("rwy_df_fi_one.img"), &cur) == 0, "snap after fault");
        if (!snaps_equal(&base, &cur)) {
            printf("FAIL: contents changed at fault point %ld\n", at);
            failures++;
        }
        /* Retry must converge to clean compacted store. */
        CHECK(run_defrag(tpath("rwy_df_fi_one.img")) == 0, "retry converges");
        CHECK(get_frag_objects(tpath("rwy_df_fi_one.img")) == 0, "retry defrags all");
        CHECK(get_reclaimable(tpath("rwy_df_fi_one.img")) == 0, "retry reclaims all");
        CHECK(take_snap(tpath("rwy_df_fi_one.img"), &cur) == 0, "snap after retry");
        if (!snaps_equal(&base, &cur)) {
            printf("FAIL: contents changed after retry at=%ld\n", at);
            failures++;
        }
    }
    CHECK(run_check(tpath("rwy_df_fi_one.img")) == 0, "checker clean after defrag faults");
    printf("== faults OK ==\n");
}

/* ================= 7. Idempotency ================= */
static void test_idempotent(const char *store_path) {
    uint64_t g0, g1, g2;
    printf("== defrag idempotency ==\n");
    copy_file(store_path, tpath("rwy_df_idem.img"));
    build_fragmented_store(tpath("rwy_df_idem.img"), 8, 2);
    CHECK(run_defrag(tpath("rwy_df_idem.img")) == 0, "first defrag");
    g0 = get_gen(tpath("rwy_df_idem.img"));
    copy_file(tpath("rwy_df_idem.img"), tpath("rwy_df_idem_pre.img"));
    CHECK(run_defrag(tpath("rwy_df_idem.img")) == 0, "second defrag");
    g1 = get_gen(tpath("rwy_df_idem.img"));
    CHECK(g1 == g0, "second defrag is a no-op (no new generation)");
    CHECK(files_identical(tpath("rwy_df_idem_pre.img"), tpath("rwy_df_idem.img")),
          "second defrag byte-identical");
    CHECK(run_defrag(tpath("rwy_df_idem.img")) == 0, "third defrag");
    g2 = get_gen(tpath("rwy_df_idem.img"));
    CHECK(g2 == g0, "third defrag still no-op");
    printf("== idempotency OK ==\n");
}

/* ================= the state record ================= */
static void test_record(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    printf("== defrag keeps the state record ==\n");
    copy_file(store_path, tpath("rwy_df_rec.img"));
    build_fragmented_store(tpath("rwy_df_rec.img"), 8, 3);
    open_store(&s, &dev, &fl, tpath("rwy_df_rec.img"), "r+b");
    rec_put(&s);
    fclose(fl.f);
    CHECK(run_defrag(tpath("rwy_df_rec.img")) == 0, "defrag exits 0");
    CHECK(get_frag_objects(tpath("rwy_df_rec.img")) == 0, "defrag moved the files");
    open_store(&s, &dev, &fl, tpath("rwy_df_rec.img"), "rb");
    CHECK(rec_kept(&s), "the record is intact after defrag");
    fclose(fl.f);
    CHECK(run_check(tpath("rwy_df_rec.img")) == 0, "plain check clean after defrag");
    printf("== record OK ==\n");
}

int main(int argc, char **argv) {
    const char *store = argc > 1 ? argv[1] : "build/RUNWAY.DAT";
    if (argc > 2) g_checkbin = argv[2];
    printf("defragtest: opening %s (checker %s)\n", store, g_checkbin);
    {
        FILE *f = fopen(store, "rb");
        if (!f) { printf("SKIP: cannot open %s\n", store); return 1; }
        fclose(f);
    }
    test_compaction(store);
    test_nospc_recovery(store);
    test_large_files(store);
    test_noop(store);
    test_refusal(store);
    test_idempotent(store);
    test_faults(store);
    test_record(store);
    if (failures == 0) printf("\nDEFRAGTEST: PASS\n");
    else printf("\nDEFRAGTEST: FAIL (%d)\n", failures);
    return failures != 0;
}
