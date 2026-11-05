/*
 * rwy_defrag.c — Fragmentation metric and offline compaction.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

/* ---------- fragmentation metric / offline compaction (defrag) ----------
 *
 * Policy: full repack to one run per object where it fits. Only
 * fragmented objects (extent_count > 1 or indirect in use) move;
 * already-contiguous objects stay in place. No format change: OIDs and
 * the directory tree are untouched, only data extents + table/bitmap
 * images are rewritten (the images go to the inactive image area). Single combined pass: defrag implies the GC
 * rebuild, published as exactly one new generation (defrag is the
 * explicit on-demand compaction entry point; repair stays
 * leftover-only and never compacts).
 *
 * Scratch for the relocation plan lives in static storage (the core is
 * single-threaded; EFI stacks are small). */

static reloc_ctx_t defrag_ctx;

int rwy_inode_is_frag(const rwy_inode_t *in) {
    if (!in || in->type == RWY_TYPE_FREE) return 0;
    if (in->indirect != 0) return 1;
    if (in->extent_count > 1) return 1;
    return 0;
}

static uint8_t frag_bits[REACH_BYTES];

int rwy_frag_info(const rwy_store_t *s, rwy_frag_info_t *out) {
    uint32_t i, b, run, best;
    if (!s || !out) return RWY_ERR_INVAL;
    out->frag_objects = 0;
    out->live_blocks = 0;
    out->free_blocks = 0;
    out->largest_free_run = 0;
    for (i = 0; i < RWY_MAX_INODES; i++) {
        if (s->inodes[i].type == RWY_TYPE_FREE) continue;
        if (inode_is_frag(&s->inodes[i])) out->frag_objects++;
    }
    for (b = 0; b < sizeof(frag_bits); b++) frag_bits[b] = 0;
    /* Reachability on the committed table; s->dev is only read. */
    {
        int rc = rwy_mark_data_reachable(s, s->inodes, frag_bits);
        if (rc) return rc;
    }
    for (b = s->sb.pool_start; b < s->sb.total_blocks; b++) {
        if (rwy_bmp_get(frag_bits, b)) out->live_blocks++;
        if (!rwy_bmp_get(s->bitmap, b)) out->free_blocks++;
    }
    run = 0; best = 0;
    for (b = s->sb.pool_start; b < s->sb.total_blocks; b++) {
        if (!rwy_bmp_get(s->bitmap, b)) {
            run++;
            if (run > best) best = run;
        } else {
            run = 0;
        }
    }
    out->largest_free_run = best;
    return RWY_OK;
}

static uint32_t count_frag_objects(const rwy_store_t *s) {
    uint32_t i, n = 0;
    for (i = 0; i < RWY_MAX_INODES; i++) {
        if (s->inodes[i].type == RWY_TYPE_FREE) continue;
        if (inode_is_frag(&s->inodes[i])) n++;
    }
    return n;
}

/*
 * Plan one fresh contiguous run per fragmented object, all allocated up
 * front (bits only) so NOSPC fails before any device write: the store is
 * untouched in that case.
 */
int rwy_defrag_plan(rwy_store_t *s, reloc_ctx_t *ctx) {
    uint32_t i, hint;
    int rc;

    if (!s || !ctx) return RWY_ERR_INVAL;
    hint = s->sb.pool_start;
    for (i = 0; i < RWY_MAX_INODES; i++)
        ctx->new_start[i] = ctx->new_len[i] = 0;
    for (i = 0; i < RWY_MAX_INODES; i++) {
        const rwy_inode_t *old = &s->inodes[i];
        uint32_t nb, st = 0;
        if (old->type == RWY_TYPE_FREE) continue;
        if (!inode_is_frag(old)) continue;
        nb = rwy_nblocks_of(old->size);
        if (nb == 0) continue; /* empty but "frag": just drop extents */
        rc = rwy_find_run_in(rwy_gc_prot, s->sb.total_blocks, hint, nb, &st);
        if (rc) return rc;
        rwy_mark_range(rwy_gc_prot, st, nb);
        ctx->new_start[i] = st;
        ctx->new_len[i] = nb;
        hint = st + nb;
        if (hint >= s->sb.total_blocks) hint = s->sb.pool_start;
    }
    return RWY_OK;
}

/* Mark the indirect block and the extents listed in it. */
static int defrag_mark_indirect(rwy_store_t *s, const rwy_inode_t *old) {
    uint8_t iblk[RWY_BLOCK_SIZE];
    uint32_t pairs = (RWY_BLOCK_SIZE - (uint32_t)sizeof(rwy_blk_header_t)) / 8;
    uint32_t p;

    rwy_bmp_set(s->w_bitmap, old->indirect);
    if (rwy_blk_read(&s->dev, RWY_IO_EXTENT, old->indirect, iblk))
        return RWY_ERR_IO;
    for (p = 0; p < pairs; p++) {
        const uint8_t *q = iblk + sizeof(rwy_blk_header_t) + p * 8;
        uint32_t st = (uint32_t)q[0] | ((uint32_t)q[1] << 8) |
                      ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
        uint32_t ln = (uint32_t)q[4] | ((uint32_t)q[5] << 8) |
                      ((uint32_t)q[6] << 16) | ((uint32_t)q[7] << 24);
        if (!st && !ln) break;
        rwy_mark_range(s->w_bitmap, st, ln);
    }
    return RWY_OK;
}

/* Mark the data blocks of an object that stays where it is. */
static int defrag_mark_retained(rwy_store_t *s, const rwy_inode_t *old) {
    uint32_t e;

    for (e = 0; e < old->extent_count && e < RWY_DIRECT_EXTENTS; e++)
        rwy_mark_range(s->w_bitmap, old->extents[e].start, old->extents[e].len);
    if (old->indirect)
        return defrag_mark_indirect(s, old);
    return RWY_OK;
}

static int defrag_mark_xattr(rwy_store_t *s, const rwy_inode_t *old) {
    uint32_t xb = (uint32_t)old->xattr_root;
    uint32_t guard = 0;

    while (xb != 0 && guard++ < 64) {
        uint8_t blk[RWY_BLOCK_SIZE];
        const rwy_xattr_header_t *xh;
        if (xb < s->sb.pool_start || xb >= s->sb.total_blocks) break;
        rwy_bmp_set(s->w_bitmap, xb);
        if (rwy_blk_read(&s->dev, RWY_IO_META, xb, blk)) return RWY_ERR_IO;
        xh = (const rwy_xattr_header_t *)blk;
        if (xh->hdr.magic != RWY_BLK_MAGIC || xh->hdr.kind != RWY_BLK_KIND_XATTR) break;
        xb = xh->next;
    }
    return RWY_OK;
}

/*
 * Build the replacement bitmap: retained in-place runs, new runs and the
 * blocks below the pool. Fragmented objects contribute only their new
 * runs; their old blocks become free.
 */
int rwy_defrag_build_bitmap(rwy_store_t *s, const reloc_ctx_t *ctx) {
    uint32_t i;
    int rc;

    if (!s || !ctx) return RWY_ERR_INVAL;
    for (i = 0; i < sizeof(s->w_bitmap); i++) s->w_bitmap[i] = 0;
    rwy_mark_meta_zone(s, s->w_bitmap);
    for (i = 0; i < RWY_MAX_INODES; i++) {
        const rwy_inode_t *old = &s->inodes[i];
        if (old->type == RWY_TYPE_FREE) continue;
        if (ctx->new_len[i]) {
            rwy_mark_range(s->w_bitmap, ctx->new_start[i], ctx->new_len[i]);
        } else if (!(inode_is_frag(old) && rwy_nblocks_of(old->size) == 0)) {
            /* an empty ex-frag object has no blocks */
            rc = defrag_mark_retained(s, old);
            if (rc) return rc;
        }
        if (old->xattr_root) {
            rc = defrag_mark_xattr(s, old);
            if (rc) return rc;
        }
    }
    return RWY_OK;
}

/*
 * Physical block copies only (no keys/plaintext). Every target lies in
 * previously-free-or-reclaimable space, so a crash mid-copy leaves the
 * old generation mountable and the partial output unreachable.
 */
int rwy_defrag_copy(rwy_store_t *s, const rwy_block_dev_t *to, const reloc_ctx_t *ctx) {
    uint32_t i;
    const rwy_block_dev_t *dst;

    if (!s || !ctx) return RWY_ERR_INVAL;
    dst = to ? to : &s->dev;
    for (i = 0; i < RWY_MAX_INODES; i++) {
        const rwy_inode_t *old = &s->inodes[i];
        uint32_t l, nb = ctx->new_len[i];
        for (l = 0; l < nb; l++) {
            uint32_t src = 0;
            uint8_t blk[RWY_BLOCK_SIZE];
            int rc = rwy_phys_of(s, old, l, &src);
            if (rc) return rc;
            rc = rwy_blk_read(&s->dev, RWY_IO_DATA, src, blk);
            if (rc) return rc;
            rc = rwy_blk_write(dst, RWY_IO_DATA, ctx->new_start[i] + l, blk);
            if (rc) return rc;
        }
    }
    return RWY_OK;
}

/* Point the working extents at the new single runs; all other metadata stays. */
void rwy_defrag_patch_inodes(rwy_store_t *s, const reloc_ctx_t *ctx) {
    uint32_t i, e;

    if (!s || !ctx) return;
    for (i = 0; i < RWY_MAX_INODES; i++) {
        rwy_inode_t *w = &s->w_inodes[i];
        if (w->type == RWY_TYPE_FREE) continue;
        if (!ctx->new_len[i] && !inode_is_frag(&s->inodes[i])) continue;
        for (e = 0; e < RWY_DIRECT_EXTENTS; e++)
            w->extents[e].start = w->extents[e].len = 0;
        w->extent_count = 0;
        w->indirect = 0;
        if (ctx->new_len[i]) {
            w->extents[0].start = ctx->new_start[i];
            w->extents[0].len = ctx->new_len[i];
            w->extent_count = 1;
        }
    }
}

int rwy_defrag_publish(rwy_store_t *s) {
    int rc;

    if (!s) return RWY_ERR_INVAL;
    rc = rwy_commit_write_publish(s);
    if (rc) return rc;
    s->next_hint = s->sb.pool_start;
    return RWY_OK;
}

static int defrag_prepare_maps(rwy_store_t *s) {
    uint32_t i;
    int rc;

    /* Fresh protection map: reachable data + current images + SB. */
    for (i = 0; i < sizeof(rwy_gc_prot); i++) rwy_gc_prot[i] = 0;
    rc = rwy_mark_data_reachable(s, s->inodes, rwy_gc_prot);
    if (rc) return rc;
    for (i = 0; i < sizeof(rwy_gc_work); i++) rwy_gc_work[i] = rwy_gc_prot[i];
    rwy_mark_meta_zone(s, rwy_gc_prot);
    /* Stage the working table now (extents patched after the copies). */
    for (i = 0; i < RWY_MAX_INODES; i++) s->w_inodes[i] = s->inodes[i];
    return RWY_OK;
}

int rwy_defrag(rwy_store_t *s) {
    int rc;

    if (!s) return RWY_ERR_INVAL;
    if (s->in_txn) return RWY_ERR_INVAL;
    if (count_frag_objects(s) == 0 && rwy_reclaimable(s) == 0)
        return RWY_OK; /* already compact: publish nothing */

    rc = defrag_prepare_maps(s);
    if (rc) return rc;

    rc = rwy_defrag_plan(s, &defrag_ctx);
    if (rc) return rc;

    rc = rwy_defrag_build_bitmap(s, &defrag_ctx);
    if (rc) return rc;

    s->w_generation = s->sb.generation + 1;
    if (s->w_generation == 0) s->w_generation = 1;
    rwy_w_free_reset(s);
    s->in_txn = 1;

    rc = rwy_defrag_copy(s, NULL, &defrag_ctx);
    if (rc) { s->in_txn = 0; return rc; }

    rwy_defrag_patch_inodes(s, &defrag_ctx);
    /* Suppress unused warning for rwy_gc_work copy (kept for symmetry). */
    (void)rwy_gc_work;

    rc = rwy_defrag_publish(s);
    if (rc) { s->in_txn = 0; return rc; }

    return RWY_OK;
}
