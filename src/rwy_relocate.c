/*
 * rwy_relocate.c — Relocate all live blocks to another block device.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "rwy_core.h"
#include "rwy_format.h"
#include "rwy_internal.h"
#include "rwy_transplog.h"

#define RELOC_CHUNK_BLOCKS 16

static reloc_ctx_t reloc_obj_ctx;
static reloc_ctx_t reloc_xattr_ctx;
static uint8_t reloc_scratch[RELOC_CHUNK_BLOCKS * RWY_BLOCK_SIZE];

static int count_xattr_blocks(const rwy_store_t *s, uint32_t root, uint32_t *out_count) {
    uint32_t xb = root, count = 0, guard = 0;

    while (xb != 0) {
        uint8_t blk[RWY_BLOCK_SIZE];
        const rwy_xattr_header_t *xh = (const rwy_xattr_header_t *)blk;
        if (xb < s->sb.pool_start || xb >= s->sb.total_blocks) return RWY_ERR_CORRUPT;
        if (++guard > 64) return RWY_ERR_CORRUPT;
        if (rwy_blk_read(&s->dev, RWY_IO_META, xb, blk)) return RWY_ERR_IO;
        if (xh->hdr.magic != RWY_BLK_MAGIC || xh->hdr.kind != RWY_BLK_KIND_XATTR)
            return RWY_ERR_CORRUPT;
        if (rwy_blk_compute_crc(blk) != xh->hdr.crc32c) return RWY_ERR_CORRUPT;
        count++;
        xb = xh->next;
    }
    *out_count = count;
    return RWY_OK;
}

static int relocate_plan_objects(rwy_store_t *s, reloc_ctx_t *ctx, uint32_t *hint) {
    uint32_t i;

    for (i = 0; i < RWY_MAX_INODES; i++)
        ctx->new_start[i] = ctx->new_len[i] = 0;

    for (i = 0; i < RWY_MAX_INODES; i++) {
        const rwy_inode_t *old = &s->inodes[i];
        uint32_t nb, st = 0;
        int rc;

        if (old->type == RWY_TYPE_FREE) continue;
        nb = rwy_nblocks_of(old->size);
        if (nb == 0) continue;

        rc = rwy_find_run_in(rwy_gc_prot, s->sb.total_blocks, *hint, nb, &st);
        if (rc) return rc;
        rwy_mark_range(rwy_gc_prot, st, nb);
        ctx->new_start[i] = st;
        ctx->new_len[i] = nb;
        *hint = st + nb;
        if (*hint >= s->sb.total_blocks) *hint = s->sb.pool_start;
    }
    return RWY_OK;
}

static int relocate_plan_xattrs(rwy_store_t *s, reloc_ctx_t *ctx, uint32_t *hint) {
    uint32_t i;

    for (i = 0; i < RWY_MAX_INODES; i++)
        ctx->new_start[i] = ctx->new_len[i] = 0;

    for (i = 0; i < RWY_MAX_INODES; i++) {
        const rwy_inode_t *old = &s->inodes[i];
        uint32_t cnt = 0, st = 0;
        int rc;

        if (old->type == RWY_TYPE_FREE || old->xattr_root == 0) continue;
        rc = count_xattr_blocks(s, (uint32_t)old->xattr_root, &cnt);
        if (rc) return rc;
        if (cnt == 0) continue;

        rc = rwy_find_run_in(rwy_gc_prot, s->sb.total_blocks, *hint, cnt, &st);
        if (rc) return rc;
        rwy_mark_range(rwy_gc_prot, st, cnt);
        ctx->new_start[i] = st;
        ctx->new_len[i] = cnt;
        *hint = st + cnt;
        if (*hint >= s->sb.total_blocks) *hint = s->sb.pool_start;
    }
    return RWY_OK;
}

static void relocate_build_bitmap(rwy_store_t *s, const reloc_ctx_t *obj_ctx,
                                  const reloc_ctx_t *xattr_ctx) {
    uint32_t i;

    for (i = 0; i < sizeof(s->w_bitmap); i++) s->w_bitmap[i] = 0;
    rwy_mark_meta_zone(s, s->w_bitmap);
    for (i = 0; i < RWY_MAX_INODES; i++) {
        if (obj_ctx->new_len[i])
            rwy_mark_range(s->w_bitmap, obj_ctx->new_start[i], obj_ctx->new_len[i]);
        if (xattr_ctx->new_len[i])
            rwy_mark_range(s->w_bitmap, xattr_ctx->new_start[i], xattr_ctx->new_len[i]);
    }
}

static int copy_blocks_run(const rwy_block_dev_t *src_dev, uint32_t src_start,
                           const rwy_block_dev_t *dst_dev, uint32_t dst_start,
                           uint32_t count) {
    uint32_t done = 0;

    while (done < count) {
        uint32_t cur = count - done;
        size_t bytes;

        if (cur > RELOC_CHUNK_BLOCKS) cur = RELOC_CHUNK_BLOCKS;
        bytes = (size_t)cur * RWY_BLOCK_SIZE;
        if ((uint64_t)(src_start + done) * RWY_BLOCK_SIZE + bytes > src_dev->size)
            return RWY_ERR_IO;
        if ((uint64_t)(dst_start + done) * RWY_BLOCK_SIZE + bytes > dst_dev->size)
            return RWY_ERR_IO;

        if (rwy_dev_pread(src_dev, RWY_IO_DATA, (uint64_t)(src_start + done) * RWY_BLOCK_SIZE,
                          reloc_scratch, bytes))
            return RWY_ERR_IO;
        if (rwy_dev_pwrite(dst_dev, RWY_IO_DATA, (uint64_t)(dst_start + done) * RWY_BLOCK_SIZE,
                           reloc_scratch, bytes))
            return RWY_ERR_IO;
        done += cur;
    }
    return RWY_OK;
}

static int relocate_copy_objects(rwy_store_t *s, const rwy_block_dev_t *to,
                                 const reloc_ctx_t *ctx) {
    uint32_t i;

    for (i = 0; i < RWY_MAX_INODES; i++) {
        const rwy_inode_t *old = &s->inodes[i];
        uint32_t l = 0, nb = ctx->new_len[i];

        while (l < nb) {
            uint32_t src = 0, run = 0, chunk;
            int rc = rwy_phys_find(s, old, l, &src, &run);

            if (rc) return rc;
            chunk = (run < nb - l) ? run : (nb - l);
            rc = copy_blocks_run(&s->dev, src, to, ctx->new_start[i] + l, chunk);
            if (rc) return rc;
            l += chunk;
        }
    }
    return RWY_OK;
}

static int relocate_copy_xattrs(rwy_store_t *s, const rwy_block_dev_t *to,
                                const reloc_ctx_t *ctx) {
    uint32_t i;

    for (i = 0; i < RWY_MAX_INODES; i++) {
        const rwy_inode_t *old = &s->inodes[i];
        uint32_t xb = (uint32_t)old->xattr_root;
        uint32_t st = ctx->new_start[i];
        uint32_t len = ctx->new_len[i], j = 0;

        if (old->type == RWY_TYPE_FREE || xb == 0) continue;
        while (xb != 0 && j < len) {
            uint8_t blk[RWY_BLOCK_SIZE];
            rwy_xattr_header_t *xh = (rwy_xattr_header_t *)blk;
            uint32_t old_next;
            int rc = rwy_blk_read(&s->dev, RWY_IO_META, xb, blk);

            if (rc) return rc;
            if (xh->hdr.magic != RWY_BLK_MAGIC || xh->hdr.kind != RWY_BLK_KIND_XATTR)
                return RWY_ERR_CORRUPT;
            old_next = xh->next;
            xh->hdr.generation = s->w_generation;
            xh->next = (j + 1 < len) ? (st + j + 1) : 0;
            xh->hdr.crc32c = rwy_blk_compute_crc(blk);
            rc = rwy_blk_write(to, RWY_IO_META, st + j, blk);
            if (rc) return rc;
            xb = old_next;
            j++;
        }
    }
    return RWY_OK;
}

static void relocate_patch_inodes(rwy_store_t *s, const reloc_ctx_t *obj_ctx,
                                  const reloc_ctx_t *xattr_ctx) {
    uint32_t i, e;

    for (i = 0; i < RWY_MAX_INODES; i++) {
        rwy_inode_t *w = &s->w_inodes[i];

        if (w->type == RWY_TYPE_FREE) continue;
        for (e = 0; e < RWY_DIRECT_EXTENTS; e++) {
            w->extents[e].start = 0;
            w->extents[e].len = 0;
        }
        w->extent_count = 0;
        w->indirect = 0;
        if (obj_ctx->new_len[i]) {
            w->extents[0].start = obj_ctx->new_start[i];
            w->extents[0].len = obj_ctx->new_len[i];
            w->extent_count = 1;
        }
        w->xattr_root = xattr_ctx->new_len[i] ? xattr_ctx->new_start[i] : 0;
    }
}

static int relocate_write_record(rwy_store_t *s, const rwy_block_dev_t *to, int *new_cur) {
    uint8_t blk[RWY_BLOCK_SIZE];
    rwy_blk_header_t *h = (rwy_blk_header_t *)blk;
    int next = s->rec_cur ? 0 : 1;
    int rc;

    memset(blk, 0, sizeof(blk));
    rwy_blk_hdr_make(h, s->rec_seq + 1, (uint32_t)next, RWY_BLK_KIND_RECORD);
    memcpy(blk + sizeof(*h), s->rec, RWY_REC_PAYLOAD);
    h->crc32c = rwy_blk_compute_crc(blk);
    rc = rwy_blk_write(to, RWY_IO_META, s->sb.rec_start + (uint32_t)next, blk);
    if (rc) return rc;
    rc = rwy_dev_flush(to);
    if (rc) return rc;
    *new_cur = next;
    return RWY_OK;
}

static int relocate_write_journal(rwy_store_t *s, const rwy_block_dev_t *to) {
    return rwy_transplog_relocate(s, to);
}

static int relocate_publish(rwy_store_t *s, const rwy_block_dev_t *to, const void *sb_tail) {
    rwy_superblock_t nsb;
    uint32_t tbl_start = RWY_SB_BLOCKS, bm_start;
    int rc;

    if (s->sb.inode_tbl_start == tbl_start) tbl_start += s->sb.area_blocks;
    bm_start = tbl_start + s->sb.inode_tbl_blocks;

    rc = rwy_write_inode_image_to(s, to, tbl_start);
    if (rc) return rc;
    rc = rwy_write_bitmap_image_to(s, to, bm_start);
    if (rc) return rc;
    rc = rwy_dev_flush(to);
    if (rc) return rc;

    nsb = s->sb;
    nsb.generation = s->w_generation;
    nsb.inode_tbl_start = tbl_start;
    nsb.bitmap_start = bm_start;
    nsb.crc32c = 0;
    nsb.crc32c = rwy_crc32c(&nsb, offsetof(rwy_superblock_t, crc32c));
    rc = rwy_write_superblock_to(s, to, &nsb, sb_tail);
    if (rc) return rc;
    rc = rwy_dev_flush(to);
    if (rc) return rc;

    s->sb = nsb;
    s->sb_slot = s->sb_slot ? 0 : 1;
    return RWY_OK;
}

static int relocate_prepare(rwy_store_t *s) {
    uint32_t i, hint;
    int rc;

    for (i = 0; i < sizeof(rwy_gc_prot); i++) rwy_gc_prot[i] = 0;
    rc = rwy_mark_data_reachable(s, s->inodes, rwy_gc_prot);
    if (rc) return rc;
    rwy_mark_meta_zone(s, rwy_gc_prot);

    hint = s->sb.pool_start;
    rc = relocate_plan_objects(s, &reloc_obj_ctx, &hint);
    if (rc) return rc;
    rc = relocate_plan_xattrs(s, &reloc_xattr_ctx, &hint);
    if (rc) return rc;

    relocate_build_bitmap(s, &reloc_obj_ctx, &reloc_xattr_ctx);
    for (i = 0; i < RWY_MAX_INODES; i++) s->w_inodes[i] = s->inodes[i];

    s->w_generation = s->sb.generation + 1;
    if (s->w_generation == 0) s->w_generation = 1;
    rwy_w_free_reset(s);
    return RWY_OK;
}

static void relocate_adopt(rwy_store_t *s, const rwy_block_dev_t *to, int next_rec_cur) {
    uint32_t i;

    s->dev = *to;
    s->dev.stats = &s->io;
    s->rec_seq++;
    s->rec_cur = next_rec_cur;
    s->rec_damaged = 0;
    for (i = 0; i < RWY_MAX_INODES; i++) s->inodes[i] = s->w_inodes[i];
    for (i = 0; i < sizeof(s->bitmap); i++) s->bitmap[i] = s->w_bitmap[i];
    s->in_txn = 0;
    s->next_hint = s->sb.pool_start;
}

int rwy_relocate(rwy_store_t *s, const rwy_block_dev_t *to, const void *sb_tail) {
    int next_rec_cur = 0, rc;

    if (!s || !to) return RWY_ERR_INVAL;
    if (s->in_txn) return RWY_ERR_INVAL;

    rc = relocate_prepare(s);
    if (rc) return rc;

    s->in_txn = 1;
    rc = relocate_copy_objects(s, to, &reloc_obj_ctx);
    if (rc) { s->in_txn = 0; return rc; }
    rc = relocate_copy_xattrs(s, to, &reloc_xattr_ctx);
    if (rc) { s->in_txn = 0; return rc; }

    relocate_patch_inodes(s, &reloc_obj_ctx, &reloc_xattr_ctx);

    rc = relocate_write_record(s, to, &next_rec_cur);
    if (rc) { s->in_txn = 0; return rc; }

    rc = relocate_write_journal(s, to);
    if (rc) { s->in_txn = 0; return rc; }

    rc = relocate_publish(s, to, sb_tail);
    if (rc) { s->in_txn = 0; return rc; }

    relocate_adopt(s, to, next_rec_cur);
    return RWY_OK;
}
