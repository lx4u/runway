/*
 * rwy_alloc.c — Block allocation bitmap and (working) run allocator.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

int rwy_bmp_get(const uint8_t *bm, uint32_t b) {
    return (bm[b >> 3] >> (b & 7)) & 1;
}

void rwy_bmp_set(uint8_t *bm, uint32_t b) { bm[b >> 3] |= (uint8_t)(1u << (b & 7)); }

void rwy_bmp_clear(uint8_t *bm, uint32_t b) { bm[b >> 3] &= (uint8_t)~(1u << (b & 7)); }

int rwy_blk_used(const rwy_store_t *s, uint32_t blk) {
    const uint8_t *bm;
    if (!s || blk >= s->sb.total_blocks) return 0;
    bm = s->in_txn ? s->w_bitmap : s->bitmap;
    return rwy_bmp_get(bm, blk);
}


/* Number of clear bits below total_blocks, counted a byte at a time. */
static uint32_t free_in(const rwy_store_t *s, const uint8_t *bm) {
    uint32_t whole = s->sb.total_blocks / 8, i, n = 0;
    for (i = 0; i < whole; i++) {
        uint8_t v = (uint8_t)~bm[i];
        v = (uint8_t)(v - ((v >> 1) & 0x55));
        v = (uint8_t)((v & 0x33) + ((v >> 2) & 0x33));
        n += (uint8_t)((v + (v >> 4)) & 0x0f);
    }
    for (i = whole * 8; i < s->sb.total_blocks; i++) n += !rwy_bmp_get(bm, i);
    return n;
}

uint32_t rwy_free_blocks(const rwy_store_t *s) {
    uint32_t n;
    if (!s) return 0;
    n = free_in(s, s->in_txn ? s->w_bitmap : s->bitmap);
    return n > RWY_RESERVE_BLOCKS ? n - RWY_RESERVE_BLOCKS : 0;
}

void rwy_pool_usage(rwy_store_t *s, uint32_t *used, uint32_t *usable) {
    uint32_t pool = s->sb.total_blocks - s->sb.pool_start;
    uint32_t free_raw = free_in(s, s->in_txn ? s->w_bitmap : s->bitmap);
    uint32_t held = pool - free_raw, garbage = rwy_reclaimable(s);
    *used = held > garbage ? held - garbage : 0;
    *usable = pool > RWY_RESERVE_BLOCKS ? pool - RWY_RESERVE_BLOCKS : 0;
}

/* An allocation of `need` blocks must leave the reserve alone, unless the operation may use it. */
int rwy_w_reserve_ok(const rwy_store_t *s, uint32_t need) {
    if (s->alloc_reserve > 0) return RWY_OK;
    return rwy_free_blocks(s) >= need ? RWY_OK : RWY_ERR_NOSPC;
}

/* Free a block in the working bitmap. A block of the committed generation stays marked until
 * the commit that publishes its removal, so nothing allocated in this transaction can overwrite
 * it (an abort or a crash must leave the committed generation readable). */
void rwy_w_free(rwy_store_t *s, uint32_t blk) {
    if (rwy_bmp_get(s->bitmap, blk)) rwy_bmp_set(s->w_free, blk);
    else rwy_bmp_clear(s->w_bitmap, blk);
}

/* Forget the blocks freed by an earlier transaction. Every transaction, and every staged
 * gc or defrag publication, starts here: a stale set would clear live blocks at publish. */
void rwy_w_free_reset(rwy_store_t *s) {
    uint32_t i;
    for (i = 0; i < sizeof(s->w_free); i++) s->w_free[i] = 0;
}

/* Find a contiguous run of `need` blocks whose bits are all CLEAR in
 * `occupied` (first-fit from `hint`, wrapping after the superblocks). Runs never
 * cross the end of the store. The bits below the pool are always set, so a run
 * never lands in an image area. Does not modify `occupied`. */
int rwy_find_run_in(const uint8_t *occ, uint32_t total_blocks,
                       uint32_t hint, uint32_t need, uint32_t *out) {
    uint32_t span = total_blocks - RWY_SB_BLOCKS;
    uint32_t off, k;
    if (!occ || !out || need == 0 || need > span) return RWY_ERR_NOSPC;
    if (hint < RWY_SB_BLOCKS || hint >= total_blocks)
        hint = RWY_SB_BLOCKS;
    for (off = 0; off < span; off++) {
        uint32_t b = RWY_SB_BLOCKS +
            (uint32_t)((hint - RWY_SB_BLOCKS + off) % span);
        if (b + need > total_blocks) continue;
        for (k = 0; k < need; k++) {
            if (rwy_bmp_get(occ, b + k)) break;
        }
        if (k == need) { *out = b; return RWY_OK; }
    }
    return RWY_ERR_NOSPC;
}

/* Find a contiguous run of `need` free blocks; mark used in bm. */
int rwy_bmp_alloc_run(uint8_t *bm, uint32_t total_blocks,
                         uint32_t hint, uint32_t need, uint32_t *out) {
    uint32_t b;
    int rc;
    if (!bm || !out) return RWY_ERR_INVAL;
    rc = rwy_find_run_in(bm, total_blocks, hint, need, &b);
    if (rc) return rc;
    {
        uint32_t k;
        for (k = 0; k < need; k++) rwy_bmp_set(bm, b + k);
    }
    *out = b;
    return RWY_OK;
}

/* Reserve each run in the working bitmap; *done counts those that succeeded. */
static int alloc_runs_once(rwy_store_t *s, const uint32_t *needs, uint32_t *outs, uint32_t n, uint32_t total,
                           uint32_t *done) {
    uint32_t i;
    int rc = rwy_w_reserve_ok(s, total);

    *done = 0;
    for (i = 0; rc == RWY_OK && i < n; i++) {
        if (needs[i] == 0) { outs[i] = 0; continue; }
        rc = rwy_bmp_alloc_run(s->w_bitmap, s->sb.total_blocks, s->next_hint, needs[i], &outs[i]);
        if (rc) break;
        s->next_hint = outs[i] + needs[i];
        if (s->next_hint >= s->sb.total_blocks)
            s->next_hint = s->sb.pool_start;
        (*done)++;
    }
    return rc;
}

/* Transactional allocation: normal first-fit, with one transparent
 * compaction retry on NOSPC. Transactional semantics are unchanged:
 * compaction only discards provably-unreachable bits in the in-memory
 * working copy; the committed generation is never touched.
 *
 * SAFETY CONTRACT: callers must invoke this only when no staged blocks
 * exist — i.e. no freshly-allocated blocks have been written (or even
 * reserved) without an in-inode reference yet. Compaction cannot see
 * staged blocks and would recycle them. Multi-run writes therefore
 * allocate ALL runs upfront through rwy_w_alloc_runs() (all-or-nothing with
 * a single compaction between bit-level attempts, before any device
 * write), and never allocate again once staged blocks exist. */
int rwy_w_alloc_runs(rwy_store_t *s, const uint32_t *needs, uint32_t *outs,
                       uint32_t n) {
    int attempt;
    uint32_t i, total = 0;
    if (!s || !needs || !outs || n == 0 || n > 2) return RWY_ERR_INVAL;
    for (i = 0; i < n; i++) total += needs[i];
    for (attempt = 0; attempt < 2; attempt++) {
        uint32_t done;
        int rc = alloc_runs_once(s, needs, outs, n, total, &done);
        if (rc == RWY_OK) return RWY_OK;
        /* Unwind partial bit reservations (no writes happened yet). */
        for (i = 0; i < done; i++) {
            uint32_t k;
            for (k = 0; k < needs[i]; k++)
                rwy_bmp_clear(s->w_bitmap, outs[i] + k);
        }
        if (rc != RWY_ERR_NOSPC || attempt) return rc;
        rc = rwy_compact_working(s);
        if (rc) return rc;
    }
    return RWY_ERR_NOSPC;
}

/* Mark the longest free run of at most `want` blocks (first-fit when one of `want` exists). */
static int bmp_alloc_upto(uint8_t *bm, uint32_t total_blocks, uint32_t hint, uint32_t want,
                          uint32_t *start, uint32_t *len) {
    uint32_t b, best = 0, best_len = 0, cur = 0, k;
    if (rwy_bmp_alloc_run(bm, total_blocks, hint, want, start) == RWY_OK) { *len = want; return RWY_OK; }
    for (b = 0; b <= total_blocks; b++) {
        if (b < total_blocks && !rwy_bmp_get(bm, b)) { cur++; continue; }
        if (cur > best_len) { best = b - cur; best_len = cur; }
        cur = 0;
    }
    if (!best_len) return RWY_ERR_NOSPC;
    if (best_len > want) best_len = want;
    for (k = 0; k < best_len; k++) rwy_bmp_set(bm, best + k);
    *start = best; *len = best_len;
    return RWY_OK;
}

/* Reserve runs for `total` blocks in the working bitmap; *n counts those reserved. */
static int alloc_split_once(rwy_store_t *s, uint32_t total, rwy_extent_t *runs, uint32_t *n) {
    uint32_t left = total;
    int rc = rwy_w_reserve_ok(s, total);

    *n = 0;
    while (rc == RWY_OK && left) {
        if (*n == RWY_MAX_EXTENTS) return RWY_ERR_NOSPC;
        rc = bmp_alloc_upto(s->w_bitmap, s->sb.total_blocks, s->next_hint, left, &runs[*n].start, &runs[*n].len);
        if (rc) break;
        left -= runs[*n].len;
        s->next_hint = runs[*n].start + runs[*n].len;
        if (s->next_hint >= s->sb.total_blocks) s->next_hint = s->sb.pool_start;
        (*n)++;
    }
    return rc;
}

/* Allocate `total` blocks as at most RWY_MAX_EXTENTS runs, the longest free runs first, all or
 * nothing. Same contract as rwy_w_alloc_runs(): no staged blocks may exist when it is called. */
int rwy_w_alloc_split(rwy_store_t *s, uint32_t total, rwy_extent_t *runs, uint32_t *nruns) {
    int attempt;
    if (!s || !runs || !nruns || !total) return RWY_ERR_INVAL;
    for (attempt = 0; attempt < 2; attempt++) {
        uint32_t n, i, k;
        int rc = alloc_split_once(s, total, runs, &n);
        if (rc == RWY_OK) { *nruns = n; return RWY_OK; }
        for (i = 0; i < n; i++)
            for (k = 0; k < runs[i].len; k++) rwy_bmp_clear(s->w_bitmap, runs[i].start + k);
        if (rc != RWY_ERR_NOSPC || attempt) return rc;
        rc = rwy_compact_working(s);
        if (rc) return rc;
    }
    return RWY_ERR_NOSPC;
}

int rwy_w_alloc_run(rwy_store_t *s, uint32_t need, uint32_t *out) {
    return rwy_w_alloc_runs(s, &need, out, 1);
}
