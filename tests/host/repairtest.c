/*
 * repairtest — tests for `fsck.runway --repair` (conservative repair).
 * SPDX-License-Identifier: Apache-2.0
 *
 *  1. leftover repair: churned store (many commits of leftovers) repairs
 *     to zero reclaimable with byte-identical contents, remount OK, plain
 *     check clean afterwards.
 *  2. no-op repair: clean store keeps generation + bytes unchanged.
 *  3. refusal: each corruption class the checker detects (torn metadata,
 *     double allocation, unmarked-but-referenced, dangling dirent, bad
 *     generation selection, nonzero reserved) exits 4 with the store
 *     bit-identical.
 *  4. fault injection: fail every repair write/flush point (via the
 *     checker's RWY_CHECK_FI_AT hook); every run remounts with contents
 *     intact, and a retry converges.
 *  5. idempotency: repair twice; second run is a clean no-op.
 *  6. read-only: plain `check` never writes (clean and corrupt inputs).
 *
 * Usage: repairtest STORE RWY_CHECK_BIN
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
#include "forge.h"

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

/* Run a shell command, return the process exit status (or -1). */
static int run_status(const char *cmd) {
    int rc = system(cmd);
    if (rc == -1) return -1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return -1;
}

static int run_check(const char *img) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s %s > %s 2>&1",
             g_checkbin, img, tpath("rwy_repair_check.log"));
    return run_status(cmd);
}

static int run_repair(const char *img) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "%s --repair %s > %s 2>&1",
             g_checkbin, img, tpath("rwy_repair_run.log"));
    return run_status(cmd);
}

static int run_repair_fi(const char *img, long at) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "RWY_CHECK_FI_AT=%ld %s --repair %s > %s 2>&1",
             at, g_checkbin, img, tpath("rwy_repair_fi.log"));
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
        /* directory identity = sorted child listing */
        char names[65536];
        size_t cnt = 4096;
        if (rwy_readdir(s, path, names, sizeof(names), &cnt, 0) != RWY_OK)
            return -1;
        hash_bytes((uint8_t *)names, strlen(names), &e->h1, &e->h2);
        /* NOTE: names buffer is NUL-packed; use full used span below. */
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
        char child[RWY_MAX_PATH + 8];
        rwy_attr_t ca;
        uint64_t co;
        size_t l = strlen(names + off);
        int n = path[0] ? snprintf(child, sizeof(child), "%s/%s", path, names + off)
                        : snprintf(child, sizeof(child), "%s", names + off);
        if (n < 0 || (size_t)n >= sizeof(child)) return -1; /* path too long */
        off += l + 1;
        if (rwy_lookup(s, child, &co) != RWY_OK) return -2; /* dangling */
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

/* Churn: N create+write commits plus some deletes -> leftover accumulation. */
static void churn(const char *img, int n) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    int i;
    open_store(&s, &dev, &fl, img, "r+b");
    for (i = 0; i < n; i++) {
        char name[64];
        uint8_t w[8192];
        snprintf(name, sizeof(name), "CHURN%02d.DAT", i);
        if (rwy_txn_create(&s, name, RWY_S_IFREG | 0644, 0, 0) != RWY_OK) {
            printf("churn create failed\n"); exit(1);
        }
        fill_pattern(w, 0, sizeof(w), (uint8_t)i);
        if (rwy_txn_write_path(&s, name, 0, w, sizeof(w)) != RWY_OK) {
            printf("churn write failed\n"); exit(1);
        }
        if (i % 3 == 2) {
            char old[64];
            snprintf(old, sizeof(old), "CHURN%02d.DAT", i - 2);
            if (rwy_txn_unlink(&s, old) != RWY_OK) {
                printf("churn delete failed\n"); exit(1);
            }
        }
    }
    if (forge_leftovers(&s, 40) != RWY_OK) { printf("forge failed\n"); exit(1); }
    fclose(fl.f);
}

/* ---------- raw-image corruption crafters (with CRC upkeep, so the
 * defect lands in exactly one checker class) ---------- */
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

/* Recompute a metadata-block CRC in place (crc field at offset 24). */
static void fix_blk_crc(uint8_t blk[RWY_BLOCK_SIZE]) {
    uint32_t c;
    blk[24] = blk[25] = blk[26] = blk[27] = 0;
    c = rwy_crc32c(blk, RWY_BLOCK_SIZE);
    blk[24] = (uint8_t)c; blk[25] = (uint8_t)(c >> 8);
    blk[26] = (uint8_t)(c >> 16); blk[27] = (uint8_t)(c >> 24);
}

/* Pick the committed superblock (highest valid generation). */
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

/* Write one inode back, recomputing its table-block CRC. */
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

/* Clear one bitmap bit (referenced-but-unmarked), fixing the CRC. */
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

/* Crafters (each targets exactly one checker class). */
static void craft_torn_metadata(const char *img) {
    /* Flip a table-block payload byte without fixing the CRC. */
    rwy_superblock_t sb;
    uint8_t blk[RWY_BLOCK_SIZE];
    pick_sb(img, &sb);
    read_blk(img, sb.inode_tbl_start, blk);
    blk[100] ^= 0xFF;
    write_blk(img, sb.inode_tbl_start, blk);
}

static void craft_double_alloc(const char *img) {
    /* Two live files share one data block (CRCs kept valid). */
    rwy_superblock_t sb;
    rwy_inode_t a, b;
    uint64_t oa, ob;
    pick_sb(img, &sb);
    oa = oid_of(img, "CHURN01.DAT");
    ob = oid_of(img, "CHURN02.DAT");
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
    /* Clear the bitmap bit of a referenced data block. */
    rwy_superblock_t sb;
    rwy_inode_t a;
    uint64_t oa;
    pick_sb(img, &sb);
    oa = oid_of(img, "CHURN01.DAT");
    read_inode_raw(img, &sb, oa, &a);
    if (a.extent_count == 0) { printf("need extent for unmarked\n"); exit(1); }
    bitmap_clear(img, &sb, a.extents[0].start);
}

static void craft_dangling(const char *img) {
    /* Point the root directory's first file record at a free OID. */
    rwy_superblock_t sb;
    rwy_inode_t root;
    uint8_t blk[RWY_BLOCK_SIZE];
    pick_sb(img, &sb);
    read_inode_raw(img, &sb, 1, &root);
    if (root.extent_count == 0) { printf("root has no data\n"); exit(1); }
    read_blk(img, root.extents[0].start, blk);
    /* first record oid -> 900 (free slot: only ~10 inodes live) */
    memset(blk, 0, 8);
    blk[0] = 0x84; blk[1] = 0x03; /* 900 LE low bytes */
    write_blk(img, root.extents[0].start, blk);
}

static void craft_bad_gen(const char *img) {
    /* Kill both superblock slots. */
    uint8_t blk[RWY_BLOCK_SIZE];
    read_blk(img, 0, blk);
    blk[8] ^= 0xFF;
    write_blk(img, 0, blk);
    read_blk(img, 1, blk);
    blk[8] ^= 0xFF;
    write_blk(img, 1, blk);
}

static void craft_reserved(const char *img) {
    /* Nonzero reserved inode field with valid CRCs. */
    rwy_superblock_t sb;
    rwy_inode_t a;
    uint64_t oa;
    pick_sb(img, &sb);
    oa = oid_of(img, "CHURN01.DAT");
    read_inode_raw(img, &sb, oa, &a);
    a.xattr_root = 1;
    write_inode_raw(img, &sb, oa, &a);
}

static void craft_area_unmarked(const char *img) {
    /* Clear the bitmap bit of the last block of the image areas. */
    rwy_superblock_t sb;
    pick_sb(img, &sb);
    bitmap_clear(img, &sb, sb.pool_start - 1);
}

static void craft_extent_in_area(const char *img) {
    /* A file extent that points into an image area. */
    rwy_superblock_t sb;
    rwy_inode_t a;
    uint64_t oa;
    pick_sb(img, &sb);
    oa = oid_of(img, "CHURN01.DAT");
    read_inode_raw(img, &sb, oa, &a);
    if (a.extent_count == 0) { printf("need extent for extent-in-area\n"); exit(1); }
    a.extents[0].start = sb.pool_start - 1;
    a.extents[0].len = 1;
    write_inode_raw(img, &sb, oa, &a);
}

/* ================= 1. leftover repair ================= */
static void test_leftover(const char *store_path) {
    snap_t before, after;
    uint64_t gen0, gen1;
    uint32_t left0;
    printf("== repair leftover ==\n");
    copy_file(store_path, tpath("rwy_rp_left.img"));
    churn(tpath("rwy_rp_left.img"), 9);
    left0 = get_reclaimable(tpath("rwy_rp_left.img"));
    printf("reclaimable before repair: %u\n", left0);
    CHECK(left0 > 0, "churn leaves reclaimable blocks");
    CHECK(take_snap(tpath("rwy_rp_left.img"), &before) == 0, "snap before");
    gen0 = get_gen(tpath("rwy_rp_left.img"));
    CHECK(run_check(tpath("rwy_rp_left.img")) == 0, "plain check OK with leftovers");
    CHECK(run_repair(tpath("rwy_rp_left.img")) == 1, "repair exits 1 (fixed)");
    CHECK(get_reclaimable(tpath("rwy_rp_left.img")) == 0, "zero leftovers after repair");
    gen1 = get_gen(tpath("rwy_rp_left.img"));
    CHECK(gen1 == gen0 + 1, "repair publishes exactly one generation");
    CHECK(take_snap(tpath("rwy_rp_left.img"), &after) == 0, "snap after");
    CHECK(snaps_equal(&before, &after), "contents byte-identical");
    CHECK(run_check(tpath("rwy_rp_left.img")) == 0, "plain check clean afterwards");
    printf("== leftover OK ==\n");
}

/* ================= 2. no-op repair ================= */
static void test_noop(const char *store_path) {
    uint64_t gen0, gen1;
    printf("== repair no-op ==\n");
    copy_file(store_path, tpath("rwy_rp_noop.img"));
    /* Seed store is clean; if not, GC it via one repair on a scratch copy. */
    if (get_reclaimable(tpath("rwy_rp_noop.img")) != 0) {
        CHECK(run_repair(tpath("rwy_rp_noop.img")) == 1, "pre-clean repair");
    }
    copy_file(tpath("rwy_rp_noop.img"), tpath("rwy_rp_noop_pre.img"));
    gen0 = get_gen(tpath("rwy_rp_noop.img"));
    CHECK(run_repair(tpath("rwy_rp_noop.img")) == 0, "no-op repair exits 0");
    gen1 = get_gen(tpath("rwy_rp_noop.img"));
    CHECK(gen1 == gen0, "no-op repair publishes no generation");
    CHECK(files_identical(tpath("rwy_rp_noop_pre.img"), tpath("rwy_rp_noop.img")),
          "no-op repair byte-identical");
    printf("== no-op OK ==\n");
}

/* ================= 3. refusal ================= */
static void test_one_refusal(const char *store_path, const char *name,
                             void (*craft)(const char *)) {
    printf("-- refusal: %s --\n", name);
    copy_file(store_path, tpath("rwy_rp_ref.img"));
    churn(tpath("rwy_rp_ref.img"), 9);
    craft(tpath("rwy_rp_ref.img"));
    copy_file(tpath("rwy_rp_ref.img"), tpath("rwy_rp_ref_pre.img"));
    {
        int plain = run_check(tpath("rwy_rp_ref.img"));
        CHECK(plain == 4, "plain check reports corruption");
    }
    CHECK(run_repair(tpath("rwy_rp_ref.img")) == 4, "repair refuses (exit 4)");
    CHECK(files_identical(tpath("rwy_rp_ref_pre.img"), tpath("rwy_rp_ref.img")),
          "refused store bit-identical");
}

static void test_refusal(const char *store_path) {
    printf("== repair refusal ==\n");
    test_one_refusal(store_path, "torn metadata", craft_torn_metadata);
    test_one_refusal(store_path, "double allocation", craft_double_alloc);
    test_one_refusal(store_path, "unmarked-but-referenced", craft_unmarked);
    test_one_refusal(store_path, "dangling dirent", craft_dangling);
    test_one_refusal(store_path, "bad generation", craft_bad_gen);
    test_one_refusal(store_path, "nonzero reserved", craft_reserved);
    test_one_refusal(store_path, "unmarked image area", craft_area_unmarked);
    test_one_refusal(store_path, "extent in an image area", craft_extent_in_area);
    printf("== refusal OK ==\n");
}

/* ================= 4. fault injection over --repair ================= */
static long repair_points(const char *img) {
    /* RWY_CHECK_FI_AT=-1 never fails; VERBOSE prints the point count. */
    char cmd[2048];
    FILE *p;
    long n = -1;
    snprintf(cmd, sizeof(cmd),
             "RWY_CHECK_FI_AT=-1 RWY_CHECK_FI_VERBOSE=1 %s --repair %s 2>&1",
             g_checkbin, img);
    p = popen(cmd, "r");
    if (!p) { printf("popen failed\n"); exit(1); }
    {
        char line[512];
        while (fgets(line, sizeof(line), p))
            if (sscanf(line, "repair io points: %ld", &n) == 1) break;
    }
    pclose(p);
    return n;
}

static void test_faults(const char *store_path) {
    snap_t base;
    long npoints, at;
    printf("== repair fault injection ==\n");
    copy_file(store_path, tpath("rwy_rp_fi.img"));
    churn(tpath("rwy_rp_fi.img"), 6);
    {
        file_t fl;
        rwy_block_dev_t dev;
        rwy_store_t s;
        uint8_t w[4096];
        open_store(&s, &dev, &fl, tpath("rwy_rp_fi.img"), "r+b");
        if (rwy_txn_create(&s, "CANARY.DAT", RWY_S_IFREG | 0644, 0, 0) != RWY_OK) {
            printf("canary create failed\n"); exit(1);
        }
        fill_pattern(w, 0, sizeof(w), 0xBE);
        if (rwy_txn_write_path(&s, "CANARY.DAT", 0, w, sizeof(w)) != RWY_OK) {
            printf("canary write failed\n"); exit(1);
        }
        fclose(fl.f);
    }
    CHECK(take_snap(tpath("rwy_rp_fi.img"), &base) == 0, "snap base");
    copy_file(tpath("rwy_rp_fi.img"), tpath("rwy_rp_fi_cnt.img"));
    npoints = repair_points(tpath("rwy_rp_fi_cnt.img"));
    printf("repair persistence points: %ld\n", npoints);
    CHECK(npoints == 5, "expected 5 repair points (table, bitmap, flush, superblock, flush)");
    copy_file(store_path, tpath("rwy_rp_fi_cnt2.img"));
    CHECK(repair_points(tpath("rwy_rp_fi_cnt2.img")) == npoints ||
          get_reclaimable(tpath("rwy_rp_fi_cnt2.img")) == 0,
          "point count stable");
    for (at = 1; at <= npoints; at++) {
        snap_t cur;
        int rc;
        copy_file(tpath("rwy_rp_fi.img"), tpath("rwy_rp_fi_one.img"));
        rc = run_repair_fi(tpath("rwy_rp_fi_one.img"), at);
        (void)rc; /* any exit: failure expected inside the window */
        /* Recovery: must remount with contents intact. */
        {
            file_t fl;
            rwy_block_dev_t dev;
            rwy_store_t s;
            fl.f = fopen(tpath("rwy_rp_fi_one.img"), "r+b");
            dev.read = file_rd; dev.write = file_wr; dev.flush = file_fl;
            dev.ctx = &fl; dev.size = RWY_STORE_SIZE;
            dev.block_size = RWY_BLOCK_SIZE;
            if (rwy_mount(&s, &dev) != RWY_OK) {
                printf("FAIL: remount after repair fault at=%ld\n", at);
                failures++;
                fclose(fl.f);
                continue;
            }
            fclose(fl.f);
        }
        CHECK(take_snap(tpath("rwy_rp_fi_one.img"), &cur) == 0, "snap after fault");
        if (!snaps_equal(&base, &cur)) {
            printf("FAIL: contents changed at=%ld\n", at);
            failures++;
        }
        /* Retry must converge to clean. */
        CHECK(run_repair(tpath("rwy_rp_fi_one.img")) <= 1, "retry converges");
        CHECK(get_reclaimable(tpath("rwy_rp_fi_one.img")) == 0, "retry reclaims all");
        CHECK(take_snap(tpath("rwy_rp_fi_one.img"), &cur) == 0, "snap after retry");
        if (!snaps_equal(&base, &cur)) {
            printf("FAIL: contents changed by retry at=%ld\n", at);
            failures++;
        }
    }
    CHECK(run_check(tpath("rwy_rp_fi_one.img")) == 0, "checker OK after repair faults");
    printf("== faults OK ==\n");
}

/* ================= 5. idempotency ================= */
static void test_idempotent(const char *store_path) {
    uint64_t g0, g1, g2;
    printf("== repair idempotency ==\n");
    copy_file(store_path, tpath("rwy_rp_idem.img"));
    churn(tpath("rwy_rp_idem.img"), 9);
    CHECK(run_repair(tpath("rwy_rp_idem.img")) == 1, "first repair");
    g0 = get_gen(tpath("rwy_rp_idem.img"));
    copy_file(tpath("rwy_rp_idem.img"), tpath("rwy_rp_idem_pre.img"));
    CHECK(run_repair(tpath("rwy_rp_idem.img")) == 0, "second repair");
    g1 = get_gen(tpath("rwy_rp_idem.img"));
    CHECK(g1 == g0, "second repair is a no-op (no new generation)");
    CHECK(files_identical(tpath("rwy_rp_idem_pre.img"), tpath("rwy_rp_idem.img")),
          "second repair byte-identical");
    CHECK(run_repair(tpath("rwy_rp_idem.img")) == 0, "third repair");
    g2 = get_gen(tpath("rwy_rp_idem.img"));
    CHECK(g2 == g0, "third repair still no-op");
    printf("== idempotency OK ==\n");
}

/* ================= 6. plain check never writes ================= */
static void test_readonly(const char *store_path) {
    printf("== plain check read-only ==\n");
    copy_file(store_path, tpath("rwy_rp_ro.img"));
    churn(tpath("rwy_rp_ro.img"), 6);
    copy_file(tpath("rwy_rp_ro.img"), tpath("rwy_rp_ro_pre.img"));
    CHECK(run_check(tpath("rwy_rp_ro.img")) == 0, "plain check OK");
    CHECK(files_identical(tpath("rwy_rp_ro_pre.img"), tpath("rwy_rp_ro.img")),
          "plain check leaves clean store identical");
    /* ... including on corrupt inputs. */
    craft_double_alloc(tpath("rwy_rp_ro.img"));
    copy_file(tpath("rwy_rp_ro.img"), tpath("rwy_rp_ro_pre.img"));
    CHECK(run_check(tpath("rwy_rp_ro.img")) == 4, "plain check reports corrupt");
    CHECK(files_identical(tpath("rwy_rp_ro_pre.img"), tpath("rwy_rp_ro.img")),
          "plain check leaves corrupt store identical");
    craft_bad_gen(tpath("rwy_rp_ro.img"));
    copy_file(tpath("rwy_rp_ro.img"), tpath("rwy_rp_ro_pre.img"));
    CHECK(run_check(tpath("rwy_rp_ro.img")) == 4, "plain check reports bad gen");
    CHECK(files_identical(tpath("rwy_rp_ro_pre.img"), tpath("rwy_rp_ro.img")),
          "plain check leaves bad-gen store identical");
    printf("== read-only OK ==\n");
}

int main(int argc, char **argv) {
    const char *store = argc > 1 ? argv[1] : "build/RUNWAY.DAT";
    if (argc > 2) g_checkbin = argv[2];
    printf("repairtest: opening %s (checker %s)\n", store, g_checkbin);
    {
        FILE *f = fopen(store, "rb");
        if (!f) { printf("SKIP: cannot open %s\n", store); return 1; }
        fclose(f);
    }
    test_leftover(store);
    test_noop(store);
    test_refusal(store);
    test_idempotent(store);
    test_readonly(store);
    test_faults(store);
    if (failures == 0) printf("\nREPAIRTEST: PASS\n");
    else printf("\nREPAIRTEST: FAIL (%d)\n", failures);
    return failures != 0;
}
