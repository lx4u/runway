/*
 * rwy_txn.c — Transactions: begin, abort and atomic root publication (commit).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

int rwy_begin(rwy_store_t *s) {
    uint32_t i;
    if (!s) return RWY_ERR_INVAL;
    if (s->in_txn) return RWY_ERR_INVAL;
    for (i = 0; i < RWY_MAX_INODES; i++) s->w_inodes[i] = s->inodes[i];
    for (i = 0; i < sizeof(s->w_bitmap); i++) s->w_bitmap[i] = s->bitmap[i];
    rwy_w_free_reset(s);
    s->w_generation = s->sb.generation + 1;
    if (s->w_generation == 0) s->w_generation = 1;
    s->in_txn = 1;
    s->bc_rev_pending = 0;
    s->bc_slot_removed = 0;
    s->bc_sweep = 0;
    rwy_bootcfg_invalidate(s);
    return RWY_OK;
}

int rwy_in_txn(const rwy_store_t *s) { return s ? s->in_txn : 0; }

int rwy_abort(rwy_store_t *s) {
    if (!s) return RWY_ERR_INVAL;
    if (!s->in_txn) return RWY_ERR_NOTXN;
    s->in_txn = 0;
    s->bc_rev_pending = 0;
    s->bc_slot_removed = 0;
    s->bc_sweep = 0;
    rwy_bootcfg_invalidate(s);
    return RWY_OK;
}

/* Build block i of the inode table image from the working inodes. */
static void inode_blk_fill(const rwy_store_t *s, uint32_t i, uint8_t *blk) {
    uint32_t per = RWY_BLOCK_SIZE - (uint32_t)sizeof(rwy_blk_header_t);
    uint32_t nino = per / RWY_INODE_SIZE, base = i * nino, j, k;
    rwy_blk_header_t *h = (rwy_blk_header_t *)blk;

    memset(blk, 0, RWY_BLOCK_SIZE);
    rwy_blk_hdr_make(h, s->w_generation, i, RWY_BLK_KIND_INODE);
    for (k = 0; k < nino && base + k < RWY_MAX_INODES; k++) {
        const uint8_t *src = (const uint8_t *)&s->w_inodes[base + k];
        uint8_t *d = blk + sizeof(*h) + k * RWY_INODE_SIZE;
        for (j = 0; j < RWY_INODE_SIZE; j++) d[j] = src[j];
    }
    h->crc32c = rwy_blk_compute_crc(blk);
}

/* Build block i of the bitmap image from the working bitmap. */
static void bitmap_blk_fill(const rwy_store_t *s, uint32_t i, uint8_t *blk) {
    size_t payload = RWY_BLOCK_SIZE - sizeof(rwy_blk_header_t);
    size_t off = (size_t)i * payload, j;
    rwy_blk_header_t *h = (rwy_blk_header_t *)blk;

    memset(blk, 0, RWY_BLOCK_SIZE);
    rwy_blk_hdr_make(h, s->w_generation, i, RWY_BLK_KIND_BITMAP);
    for (j = 0; j < payload && off + j < sizeof(s->w_bitmap); j++)
        blk[sizeof(*h) + j] = s->w_bitmap[off + j];
    h->crc32c = rwy_blk_compute_crc(blk);
}

typedef void (*blk_fill_fn)(const rwy_store_t *s, uint32_t i, uint8_t *blk);

/* Write `count` blocks of an image area, up to RWY_WRUN_BLOCKS blocks per device call. */
static int write_image_runs(rwy_store_t *s, const rwy_block_dev_t *dst, uint32_t start,
                            uint32_t count, blk_fill_fn fill) {
    uint32_t i = 0, j, n;

    while (i < count) {
        n = count - i < RWY_WRUN_BLOCKS ? count - i : RWY_WRUN_BLOCKS;
        for (j = 0; j < n; j++) fill(s, i + j, s->wrun + (size_t)j * RWY_BLOCK_SIZE);
        if (rwy_blk_write_run(dst, RWY_IO_META, start + i, s->wrun, n)) return RWY_ERR_IO;
        i += n;
    }
    return RWY_OK;
}

int rwy_write_inode_image_to(rwy_store_t *s, const rwy_block_dev_t *dst, uint32_t tbl_start) {
    return write_image_runs(s, dst, tbl_start, s->sb.inode_tbl_blocks, inode_blk_fill);
}

int rwy_write_bitmap_image_to(rwy_store_t *s, const rwy_block_dev_t *dst, uint32_t bm_start) {
    return write_image_runs(s, dst, bm_start, s->sb.bitmap_blocks, bitmap_blk_fill);
}

/* Write the new superblock into the alternate slot, carrying the tail from
 * the current slot unless a new tail is provided. */
int rwy_write_superblock_to(rwy_store_t *s, const rwy_block_dev_t *dst,
                            const rwy_superblock_t *nsb, const void *tail) {
    uint8_t sblk[RWY_BLOCK_SIZE];
    uint32_t alt_slot = (uint32_t)(s->sb_slot ? RWY_SB_SLOT_A : RWY_SB_SLOT_B);
    uint32_t cur_slot = (uint32_t)(s->sb_slot ? RWY_SB_SLOT_B : RWY_SB_SLOT_A);

    if (tail) {
        memset(sblk, 0, sizeof(sblk));
        memcpy(sblk + RWY_KEYSLOT_OFFSET, tail, sizeof(rwy_keyslot_header_t));
    } else {
        int rc = rwy_blk_read(&s->dev, RWY_IO_META, cur_slot, sblk);
        if (rc) return rc;
    }
    memcpy(sblk, nsb, RWY_KEYSLOT_OFFSET);
    return rwy_blk_write(dst, RWY_IO_META, alt_slot, sblk);
}

int rwy_write_superblock(rwy_store_t *s, const rwy_superblock_t *nsb, const void *tail) {
    return rwy_write_superblock_to(s, &s->dev, nsb, tail);
}

/* Write the working inode-table + bitmap images into the inactive image area, flush,
 * publish the new superblock generation in the alternate slot, flush, and adopt. The
 * caller must have arranged that s->w_bitmap is exactly the bitmap the new generation
 * should carry (the bits below the pool stay set).
 * Crash behavior: any failure before the slot write leaves the old generation valid
 * (the active area is not touched); the slot write is the single atomic visibility point. */
int rwy_commit_write_publish(rwy_store_t *s) {
    rwy_superblock_t nsb;
    uint32_t tbl_start = RWY_SB_BLOCKS, bm_start, i;
    int rc;

    if (s->sb.inode_tbl_start == tbl_start) tbl_start += s->sb.area_blocks;
    bm_start = tbl_start + s->sb.inode_tbl_blocks;

    /* Blocks the committed generation owned and this one drops become free. */
    for (i = 0; i < sizeof(s->w_free); i++) s->w_bitmap[i] &= (uint8_t)~s->w_free[i];

    rc = rwy_write_inode_image_to(s, &s->dev, tbl_start);
    if (rc) return rc;
    rc = rwy_write_bitmap_image_to(s, &s->dev, bm_start);
    if (rc) return rc;
    rc = rwy_dev_flush(&s->dev);
    if (rc) return rc;

    /* Publish new superblock in the alternate slot. */
    nsb = s->sb;
    nsb.generation = s->w_generation;
    nsb.inode_tbl_start = tbl_start;
    nsb.bitmap_start = bm_start;
    nsb.crc32c = 0;
    nsb.crc32c = rwy_crc32c(&nsb, offsetof(rwy_superblock_t, crc32c));
    rc = rwy_write_superblock(s, &nsb, NULL);
    if (rc) return rc;
    rc = rwy_dev_flush(&s->dev);
    if (rc) return rc;

    /* Adopt working state as committed. */
    s->sb = nsb;
    s->sb_slot = s->sb_slot ? 0 : 1;
    for (i = 0; i < RWY_MAX_INODES; i++) s->inodes[i] = s->w_inodes[i];
    for (i = 0; i < sizeof(s->bitmap); i++) s->bitmap[i] = s->w_bitmap[i];
    s->in_txn = 0;
    return RWY_OK;
}

/* Write the working inode table + bitmap images, then publish new SB. A failed commit
 * (I/O) discards the transaction, so the caller's next rwy_begin() works. */
int rwy_commit(rwy_store_t *s) {
    int rc;
    if (!s) return RWY_ERR_INVAL;
    if (!s->in_txn) return RWY_ERR_NOTXN;
    rc = rwy_bootcfg_sweep(s);
    if (!rc) rc = rwy_commit_write_publish(s);
    if (rc) (void)rwy_abort(s);
    else rwy_bootcfg_committed(s);
    return rc;
}
