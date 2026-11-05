/*
 * rwy_gc.c — Reachability and space reclamation (GC).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

/* ---------- reachability / space reclamation (GC) ----------
 *
 * Intended scale: ~2 GiB max store (~524k blocks at 4 KiB), so a full
 * mark-and-rebuild pass is cheap and deterministic. GC operates purely on
 * physical block ownership: no file data is read (only extent metadata
 * and indirect blocks), no keys/plaintext are needed, and the traversal
 * below is the single authoritative definition of "reachable", shared
 * with `fsck.runway`.
 *
 * Scratch: the core is single-threaded by design; 8 KiB work bitsets
 * live in static storage (never on the stack — EFI stacks are small). */


uint8_t rwy_gc_prot[REACH_BYTES];   /* protected = must not overwrite */

uint8_t rwy_gc_work[REACH_BYTES];   /* working-set reachable image */

void rwy_mark_range(uint8_t *bits, uint32_t start, uint32_t n) {
    uint32_t k;
    for (k = 0; k < n; k++) rwy_bmp_set(bits, start + k);
}

static uint32_t le32(const uint8_t *q) {
    return (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
}

/* Mark a run of data blocks; it has to lie in the pool. */
static int mark_run(const rwy_store_t *s, uint8_t *bits, uint32_t st, uint32_t ln) {
    if (ln == 0 || st < s->sb.pool_start || st + ln > s->sb.total_blocks)
        return RWY_ERR_CORRUPT;
    rwy_mark_range(bits, st, ln);
    return RWY_OK;
}

/* Mark the indirect block and the runs listed in it. */
static int mark_indirect(const rwy_store_t *s, const rwy_inode_t *in, uint8_t *bits) {
    uint8_t blk[RWY_BLOCK_SIZE];
    uint32_t pairs = (RWY_BLOCK_SIZE - (uint32_t)sizeof(rwy_blk_header_t)) / 8, p;
    const rwy_blk_header_t *h = (const rwy_blk_header_t *)blk;

    if (in->indirect < s->sb.pool_start || in->indirect >= s->sb.total_blocks)
        return RWY_ERR_CORRUPT;
    rwy_bmp_set(bits, in->indirect);
    if (rwy_blk_read(&s->dev, RWY_IO_EXTENT, in->indirect, blk)) return RWY_ERR_IO;
    if (h->magic != RWY_BLK_MAGIC || h->kind != RWY_BLK_KIND_INDIR)
        return RWY_ERR_CORRUPT;
    if (rwy_blk_compute_crc(blk) != h->crc32c) return RWY_ERR_CORRUPT;
    for (p = 0; p < pairs; p++) {
        const uint8_t *q = blk + sizeof(rwy_blk_header_t) + p * 8;
        uint32_t st = le32(q), ln = le32(q + 4);
        int rc;
        if (!st && !ln) break;
        rc = mark_run(s, bits, st, ln);
        if (rc) return rc;
    }
    return RWY_OK;
}

/* Mark the blocks of the xattr chain. */
static int mark_xattr(const rwy_store_t *s, const rwy_inode_t *in, uint8_t *bits) {
    uint32_t xb = (uint32_t)in->xattr_root;
    uint32_t guard = 0;

    while (xb != 0) {
        uint8_t blk[RWY_BLOCK_SIZE];
        const rwy_xattr_header_t *xh = (const rwy_xattr_header_t *)blk;
        if (xb < s->sb.pool_start || xb >= s->sb.total_blocks)
            return RWY_ERR_CORRUPT;
        if (++guard > 64) return RWY_ERR_CORRUPT;
        rwy_bmp_set(bits, xb);
        if (rwy_blk_read(&s->dev, RWY_IO_META, xb, blk)) return RWY_ERR_IO;
        if (xh->hdr.magic != RWY_BLK_MAGIC || xh->hdr.kind != RWY_BLK_KIND_XATTR)
            return RWY_ERR_CORRUPT;
        if (rwy_blk_compute_crc(blk) != xh->hdr.crc32c)
            return RWY_ERR_CORRUPT;
        xb = xh->next;
    }
    return RWY_OK;
}

static int mark_inode(const rwy_store_t *s, const rwy_inode_t *in, uint32_t i, uint8_t *bits) {
    uint32_t e;
    int rc;

    if (in->oid != (uint64_t)i + 1) return RWY_ERR_CORRUPT;
    if (in->type != RWY_TYPE_FILE && in->type != RWY_TYPE_DIR &&
        in->type != RWY_TYPE_SYMLINK)
        return RWY_ERR_CORRUPT;
    if (in->extent_count > RWY_DIRECT_EXTENTS) return RWY_ERR_CORRUPT;
    for (e = 0; e < in->extent_count; e++) {
        rc = mark_run(s, bits, in->extents[e].start, in->extents[e].len);
        if (rc) return rc;
    }
    if (in->indirect) {
        rc = mark_indirect(s, in, bits);
        if (rc) return rc;
    }
    return in->xattr_root ? mark_xattr(s, in, bits) : RWY_OK;
}

int rwy_mark_data_reachable(const rwy_store_t *s, const rwy_inode_t *tbl,
                             uint8_t *bits) {
    uint32_t i;
    if (!s || !tbl || !bits) return RWY_ERR_INVAL;
    for (i = 0; i < RWY_MAX_INODES; i++) {
        int rc;
        if (tbl[i].type == RWY_TYPE_FREE) continue;
        rc = mark_inode(s, &tbl[i], i, bits);
        if (rc) return rc;
    }
    return RWY_OK;
}

/* Superblock slots and both image areas (every block below the pool) in a bitset. */
void rwy_mark_meta_zone(const rwy_store_t *s, uint8_t *bits) {
    rwy_mark_range(bits, 0, s->sb.pool_start);
}

int rwy_blocks_freed(rwy_store_t *s, const uint16_t *refs, uint32_t *freed) {
    const rwy_inode_t *tbl;
    uint32_t i, b, n = 0;
    if (!s || !refs || !freed) return RWY_ERR_INVAL;
    tbl = s->in_txn ? s->w_inodes : s->inodes;
    for (b = 0; b < sizeof(rwy_gc_work); b++) rwy_gc_work[b] = 0;
    for (i = 0; i < RWY_MAX_INODES; i++) {
        int rc;
        if (!refs[i] || tbl[i].type == RWY_TYPE_FREE || refs[i] != tbl[i].nlink) continue;
        rc = mark_inode(s, &tbl[i], i, rwy_gc_work);
        if (rc) return rc;
    }
    for (b = s->sb.pool_start; b < s->sb.total_blocks; b++) n += rwy_bmp_get(rwy_gc_work, b) ? 1u : 0u;
    *freed = n;
    return RWY_OK;
}

uint32_t rwy_reclaimable(rwy_store_t *s) {
    const uint8_t *bm;
    const rwy_inode_t *tbl;
    uint32_t b, n = 0;
    if (!s) return 0;
    bm = s->in_txn ? s->w_bitmap : s->bitmap;
    tbl = s->in_txn ? s->w_inodes : s->inodes;
    for (b = 0; b < sizeof(rwy_gc_work); b++) rwy_gc_work[b] = 0;
    if (rwy_mark_data_reachable(s, tbl, rwy_gc_work)) return 0;
    for (b = s->sb.pool_start; b < s->sb.total_blocks; b++) {
        if (!rwy_bmp_get(bm, b)) continue;
        if (rwy_bmp_get(rwy_gc_work, b)) continue;
        n++;
    }
    return n;
}

/* In-memory compaction of the working bitmap: drop every bit that is
 * marked but reachable from NEITHER the committed NOR the working image
 * (old-generation leftovers). Blocks reachable from the committed
 * generation stay marked, so a later allocation in this transaction can
 * never overwrite them before publication — a crash still mounts the old
 * generation intact. Metadata images stay marked for the same reason.
 * No device writes; safe to run at any point inside a transaction. */
int rwy_compact_working(rwy_store_t *s) {
    uint32_t b, i;
    int rc;
    for (i = 0; i < sizeof(rwy_gc_prot); i++) rwy_gc_prot[i] = 0;
    rc = rwy_mark_data_reachable(s, s->inodes, rwy_gc_prot);
    if (rc) return rc;
    rc = rwy_mark_data_reachable(s, s->w_inodes, rwy_gc_prot);
    if (rc) return rc;
    rwy_mark_meta_zone(s, rwy_gc_prot);
    for (b = s->sb.pool_start; b < s->sb.total_blocks; b++) {
        if (rwy_bmp_get(s->w_bitmap, b) && !rwy_bmp_get(rwy_gc_prot, b))
            rwy_bmp_clear(s->w_bitmap, b);
    }
    return RWY_OK;
}

/* Explicit stop-the-world collection: rebuild the committed bitmap
 * from the committed reachable set and publish it as a new generation.
 * Contents are unchanged; unreachable blocks become free. See the API
 * contract in rwy_core.h. Requires no open transaction. */
int rwy_gc(rwy_store_t *s) {
    uint32_t i;
    int rc;
    if (!s) return RWY_ERR_INVAL;
    if (s->in_txn) return RWY_ERR_INVAL;
    for (i = 0; i < sizeof(rwy_gc_work); i++) rwy_gc_work[i] = 0;
    rc = rwy_mark_data_reachable(s, s->inodes, rwy_gc_work);
    if (rc) return rc;
    rwy_mark_meta_zone(s, rwy_gc_work);
    /* Stage the collection as a working transaction, then publish. */
    for (i = 0; i < RWY_MAX_INODES; i++) s->w_inodes[i] = s->inodes[i];
    for (i = 0; i < sizeof(s->w_bitmap); i++) s->w_bitmap[i] = rwy_gc_work[i];
    s->w_generation = s->sb.generation + 1;
    if (s->w_generation == 0) s->w_generation = 1;
    rwy_w_free_reset(s);
    s->in_txn = 1;
    rc = rwy_commit_write_publish(s);
    if (rc) { s->in_txn = 0; return rc; }
    s->next_hint = s->sb.pool_start;
    return RWY_OK;
}
