/*
 * rwy_file.c — File data: CoW write, read and truncate.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

/* Extents of `in` after a write of blocks [first, last]: the old blocks before `start`, the fresh
 * runs (they cover the logical blocks from `start`: the gap, when the write starts past the old
 * end, then the write), then the old blocks after `last`. NOSPC means the list is too long or
 * there is no room for its indirect block; `in` is then unchanged. */
static int cow_set_extents(rwy_store_t *s, rwy_inode_t *in, uint32_t old_nb, uint32_t start,
                           uint32_t first, uint32_t last, const rwy_extent_t *runs, uint32_t nruns) {
    rwy_extent_t old[RWY_MAX_EXTENTS], ne[RWY_MAX_EXTENTS + 2];
    uint32_t on, nn = 0, ind, l, r;
    int rc = rwy_extents_load(s, in, old, &on);
    if (rc) return rc;
    rwy_extents_range(ne, &nn, old, on, 0, start);
    for (r = 0; r < nruns; r++) rwy_extents_push(ne, &nn, runs[r].start, runs[r].len);
    rwy_extents_range(ne, &nn, old, on, last + 1, old_nb);
    rc = rwy_extents_reserve(s, nn, &ind);
    if (rc) return rc;
    for (l = first; l <= last && l < old_nb; l++) {
        uint32_t op;
        if (rwy_phys_of(s, in, l, &op) == RWY_OK) rwy_w_free(s, op);
    }
    return rwy_extents_set(s, in, ne, nn, ind);
}

/* Write one block of the caller's data into `dst`: the old block (when there is one) with the
 * bytes of [pos, pos + len) laid over it. */
static int cow_edge(rwy_store_t *s, const rwy_inode_t *in, uint64_t pos, const uint8_t *buf, size_t len,
                    uint32_t old_nb, uint32_t b, uint32_t dst) {
    uint8_t blk[RWY_BLOCK_SIZE] = {0};
    uint64_t boff = (uint64_t)b * RWY_BLOCK_SIZE, end = pos + (uint64_t)len;
    uint32_t k, oldphys;
    if (boff < in->size && b < old_nb && rwy_phys_of(s, in, b, &oldphys) == RWY_OK)
        if (rwy_blk_read(&s->dev, RWY_IO_DATA, oldphys, blk)) return RWY_ERR_IO;
    for (k = 0; k < RWY_BLOCK_SIZE; k++)
        if (boff + k >= pos && boff + k < end) blk[k] = buf[boff + k - pos];
    return rwy_blk_write(&s->dev, RWY_IO_DATA, dst, blk);
}

/* Write the bytes of [pos, pos + len) that fall in the logical blocks [lo, hi] into the fresh run
 * `run`. Whole blocks stream in one backing write; the edge blocks the write only covers in part
 * are read-modify-write. */
static int cow_fill_run(rwy_store_t *s, const rwy_inode_t *in, uint64_t pos, const uint8_t *buf,
                        size_t len, uint32_t old_nb, uint32_t lo, uint32_t hi, uint32_t run) {
    uint64_t end = pos + (uint64_t)len;
    uint32_t from = lo, to = hi;
    int rc;
    if ((uint64_t)lo * RWY_BLOCK_SIZE < pos || (uint64_t)(lo + 1) * RWY_BLOCK_SIZE > end) {
        rc = cow_edge(s, in, pos, buf, len, old_nb, lo, run);
        if (rc) return rc;
        from++;
    }
    if (hi >= from && ((uint64_t)hi * RWY_BLOCK_SIZE < pos || (uint64_t)(hi + 1) * RWY_BLOCK_SIZE > end)) {
        rc = cow_edge(s, in, pos, buf, len, old_nb, hi, run + (hi - lo));
        if (rc) return rc;
        to--;
    }
    if (from > to) return RWY_OK;
    return rwy_dev_pwrite(&s->dev, RWY_IO_DATA, (uint64_t)(run + (from - lo)) * RWY_BLOCK_SIZE,
                          buf + ((uint64_t)from * RWY_BLOCK_SIZE - pos), (size_t)(to - from + 1) * RWY_BLOCK_SIZE);
}

/* Fill the fresh runs of a write. They cover the logical blocks from `start`; the blocks before
 * `first` are the gap and are zero. */
static int cow_fill_runs(rwy_store_t *s, const rwy_inode_t *in, uint64_t pos, const uint8_t *buf,
                         size_t len, uint32_t old_nb, uint32_t start, uint32_t first,
                         const rwy_extent_t *runs, uint32_t nruns) {
    uint8_t zero[RWY_BLOCK_SIZE] = {0};
    uint32_t r, l = start;
    for (r = 0; r < nruns; r++) {
        uint32_t hi = l + runs[r].len - 1, z, lo;
        for (z = l; z <= hi && z < first; z++) {
            int rc = rwy_blk_write(&s->dev, RWY_IO_DATA, runs[r].start + (z - l), zero);
            if (rc) return rc;
        }
        lo = l > first ? l : first;
        if (lo <= hi) {
            int rc = cow_fill_run(s, in, pos, buf, len, old_nb, lo, hi, runs[r].start + (lo - l));
            if (rc) return rc;
        }
        l = hi + 1;
    }
    return RWY_OK;
}

/* A write changed the inode: new size, times and generation. */
static void cow_touch(rwy_store_t *s, rwy_inode_t *in, uint64_t end) {
    int64_t sec; uint32_t ns;
    if (end > in->size) in->size = end;
    rwy_now_of(s, &sec, &ns);
    in->mtime_sec = sec; in->mtime_nsec = ns;
    in->ctime_sec = sec; in->ctime_nsec = ns;
    in->generation = s->w_generation;
}

/* A write inside one block that was allocated in this transaction is done in place.
 * *done tells whether it was. */
static int cow_in_place(rwy_store_t *s, rwy_inode_t *in, uint64_t pos, const uint8_t *buf, size_t len,
                        uint32_t first, int *done) {
    uint8_t blk[RWY_BLOCK_SIZE] = {0};
    uint64_t boff = (uint64_t)first * RWY_BLOCK_SIZE;
    uint32_t oldphys = 0;
    int rc;

    *done = 0;
    if (rwy_phys_of(s, in, first, &oldphys) != RWY_OK || rwy_bmp_get(s->bitmap, oldphys))
        return RWY_OK;
    if (boff < in->size && rwy_blk_read(&s->dev, RWY_IO_DATA, oldphys, blk)) return RWY_ERR_IO;
    for (size_t k = 0; k < len; k++)
        blk[(uint32_t)(pos - boff) + k] = buf[k];
    rc = rwy_blk_write(&s->dev, RWY_IO_DATA, oldphys, blk);
    if (rc) return rc;
    cow_touch(s, in, pos + (uint64_t)len);
    *done = 1;
    return RWY_OK;
}

/* Block `l` of the file as it will be after the write: the old bytes with the new ones laid over. */
static int cow_merged_block(rwy_store_t *s, const rwy_inode_t *in, uint32_t l, uint32_t old_nb, uint64_t pos,
                            const uint8_t *buf, uint64_t end, uint8_t *blk) {
    uint64_t boff = (uint64_t)l * RWY_BLOCK_SIZE;
    uint32_t op;

    memset(blk, 0, RWY_BLOCK_SIZE);
    if (boff < in->size && l < old_nb && rwy_phys_of(s, in, l, &op) == RWY_OK)
        if (rwy_blk_read(&s->dev, RWY_IO_DATA, op, blk)) return RWY_ERR_IO;
    for (uint32_t k = 0; k < RWY_BLOCK_SIZE; k++) {
        uint64_t g = boff + k;
        if (g >= pos && g < end) blk[k] = buf[g - pos];
    }
    return RWY_OK;
}

/* Too many extents: rewrite the whole file as one run. */
static int cow_rewrite_whole(rwy_store_t *s, rwy_inode_t *in, uint64_t pos, const uint8_t *buf,
                             uint64_t end, uint32_t old_nb, uint32_t new_nb) {
    uint8_t blk[RWY_BLOCK_SIZE];
    uint32_t crun = 0, l;
    int rc = rwy_w_reserve_ok(s, new_nb);

    if (!rc) rc = rwy_bmp_alloc_run(s->w_bitmap, s->sb.total_blocks, s->sb.pool_start, new_nb, &crun);
    if (rc) return rc;
    for (l = 0; l < new_nb; l++) {
        rc = cow_merged_block(s, in, l, old_nb, pos, buf, end, blk);
        if (rc) return rc;
        rc = rwy_blk_write(&s->dev, RWY_IO_DATA, crun + l, blk);
        if (rc) return rc;
    }
    for (l = 0; l < old_nb; l++) {
        uint32_t op;
        if (rwy_phys_of(s, in, l, &op) == RWY_OK)
            rwy_w_free(s, op);
    }
    if (in->indirect) { rwy_w_free(s, in->indirect); in->indirect = 0; }
    in->extents[0].start = crun; in->extents[0].len = new_nb;
    in->extent_count = new_nb ? 1 : 0;
    cow_touch(s, in, end);
    return RWY_OK;
}

/* Replace working inode data image with new bytes, CoW per affected block.
 * Unchanged blocks are shared (no copy); affected blocks are freshly
 * allocated; tail freed on shrink; grown regions zero-filled. */
int rwy_cow_write(rwy_store_t *s, rwy_inode_t *in, uint64_t pos,
                     const uint8_t *buf, size_t len) {
    uint64_t end = pos + (uint64_t)len;
    uint32_t old_nb, new_nb, first, last, gap, l, r, nruns = 0;
    rwy_extent_t runs[RWY_MAX_EXTENTS];
    int rc, done;
    if (len == 0) return RWY_OK;
    if (end > s->sb.total_size) return RWY_ERR_NOSPC;
    old_nb = rwy_nblocks_of(in->size);
    new_nb = rwy_nblocks_of(end > in->size ? end : in->size);
    first = (uint32_t)(pos / RWY_BLOCK_SIZE);
    last = (uint32_t)((end - 1) / RWY_BLOCK_SIZE);
    gap = (first > old_nb) ? (first - old_nb) : 0;
    if (first == last && first < old_nb) {
        rc = cow_in_place(s, in, pos, buf, len, first, &done);
        if (rc || done) return rc;
    }
    /* Allocate every fresh run for this write upfront (bits only), as several runs when no
     * single one is long enough. Compaction is only safe here, before staged blocks exist;
     * no allocation may happen again below until extents are set. */
    rc = rwy_w_alloc_split(s, gap + last - first + 1, runs, &nruns);
    if (rc == RWY_OK) {
        rc = cow_fill_runs(s, in, pos, buf, len, old_nb, first - gap, first, runs, nruns);
        if (rc) return rc;
        rc = cow_set_extents(s, in, old_nb, first - gap, first, last, runs, nruns);
        if (rc == RWY_OK) {
            cow_touch(s, in, end);
            return RWY_OK;
        }
        if (rc != RWY_ERR_NOSPC) return rc;
        /* More extents than the inode and its indirect block hold: drop the fresh runs and
         * rewrite the whole file as one run. */
        for (r = 0; r < nruns; r++)
            for (l = 0; l < runs[r].len; l++) rwy_w_free(s, runs[r].start + l);
    }
    return cow_rewrite_whole(s, in, pos, buf, end, old_nb, new_nb);
}

int rwy_write(rwy_store_t *s, uint64_t oid, uint64_t pos, const void *buf, size_t len) {
    rwy_inode_t *in;
    int rc, bc = 0;
    if (!s || (!buf && len)) return RWY_ERR_INVAL;
    rc = rwy_need_txn(s);
    if (rc) return rc;
    if (oid & RWY_OID_VIRT) return rwy_virt_write(s, oid, pos, buf, len);
    rc = rwy_mut_inode(s, oid, &in);
    if (rc) return rc;
    if (in->type == RWY_TYPE_DIR) return RWY_ERR_ISDIR;
    {
        rc = rwy_bootcfg_check_write(s, oid, pos, buf, len, &bc);
        if (rc) return rc;
        if (bc & RWY_BC_REPLACE) {
            rc = rwy_inode_truncate(s, in, 0);
            if (rc) return rc;
        }
    }
    /* EFI writes must not reset POSIX metadata: rwy_cow_write only touches
     * size/mtime/ctime, preserving mode/uid/gid. */
    /* Sparse gap beyond size is zero-filled by rwy_cow_write. */
    rc = rwy_cow_write(s, in, pos, (const uint8_t *)buf, len);
    if (rc || !(bc & RWY_BC_FINALIZE)) return rc;
    return rwy_bootcfg_finalize(s, oid);
}

int rwy_write_path(rwy_store_t *s, const char *path, uint64_t pos, const void *buf, size_t len) {
    uint64_t o;
    int rc;
    if (!s || !path) return RWY_ERR_INVAL;
    rc = rwy_resolve_ext(s, path, 1, 1, &o);
    if (rc) return rc;
    return rwy_write(s, o, pos, buf, len);
}

int rwy_read(rwy_store_t *s, uint64_t oid, uint64_t pos, void *buf, size_t *len) {
    const rwy_inode_t *in;
    size_t got = 0;
    int rc;
    if (!s || !buf || !len) return RWY_ERR_INVAL;
    if (oid & RWY_OID_VIRT) return rwy_virt_read(s, oid, pos, buf, len);
    in = rwy_inode_of(s, oid, s->in_txn);
    if (!in) return RWY_ERR_NOENT;
    if (in->type == RWY_TYPE_DIR) return RWY_ERR_ISDIR;
    rc = rwy_inode_pread(s, in, pos, buf, *len, &got);
    if (rc) return rc;
    *len = got;
    return RWY_OK;
}

int rwy_read_path(rwy_store_t *s, const char *path, uint64_t pos, void *buf, size_t *len) {
    uint64_t o;
    int rc;
    if (!s || !path) return RWY_ERR_INVAL;
    rc = rwy_resolve_ext(s, path, s->in_txn, 1, &o);
    if (rc) return rc;
    return rwy_read(s, o, pos, buf, len);
}

/* Zero the tail of the last kept block; a block of the committed generation is rewritten into a
 * fresh block *pb, which then replaces the kept block (*cut). */
static int truncate_tail(rwy_store_t *s, const rwy_inode_t *in, uint64_t size, uint32_t keep,
                         uint32_t *pb, uint32_t *cut) {
    uint8_t blk[RWY_BLOCK_SIZE];
    uint32_t op, k;
    int rc;

    if (!(size & (RWY_BLOCK_SIZE - 1)) || !keep) return RWY_OK;
    if (rwy_phys_of(s, in, keep - 1, &op) != RWY_OK) return RWY_OK;
    if (rwy_blk_read(&s->dev, RWY_IO_DATA, op, blk)) return RWY_ERR_IO;
    for (k = (uint32_t)(size % RWY_BLOCK_SIZE); k < RWY_BLOCK_SIZE; k++)
        blk[k] = 0;
    if (!rwy_bmp_get(s->bitmap, op))
        return rwy_blk_write(&s->dev, RWY_IO_DATA, op, blk);
    rc = rwy_w_alloc_run(s, 1, pb);
    if (rc) return rc;
    rc = rwy_blk_write(&s->dev, RWY_IO_DATA, *pb, blk);
    if (rc) return rc;
    *cut = keep - 1;
    return RWY_OK;
}

static int truncate_shrink(rwy_store_t *s, rwy_inode_t *in, uint64_t size) {
    uint32_t keep = rwy_nblocks_of(size), total = rwy_nblocks_of(in->size), l;
    rwy_extent_t old[RWY_MAX_EXTENTS], ne[RWY_MAX_EXTENTS + 2];
    uint32_t on, nn = 0, ind, pb = 0, cut = keep;
    int rc = rwy_extents_load(s, in, old, &on);

    if (rc) return rc;
    rc = truncate_tail(s, in, size, keep, &pb, &cut);
    if (rc) return rc;
    rwy_extents_range(ne, &nn, old, on, 0, cut);
    rwy_extents_push(ne, &nn, pb, pb ? 1 : 0);
    rc = rwy_extents_reserve(s, nn, &ind);
    if (rc) {
        if (pb) rwy_w_free(s, pb);
        return rc;
    }
    for (l = cut; l < total; l++) {
        uint32_t op;
        if (rwy_phys_of(s, in, l, &op) == RWY_OK) rwy_w_free(s, op);
    }
    return rwy_extents_set(s, in, ne, nn, ind);
}

/* Grow with zeros: the bytes after the end of the last block are already zero. */
static int truncate_grow(rwy_store_t *s, rwy_inode_t *in, uint64_t size) {
    uint32_t old_nb = rwy_nblocks_of(in->size);
    uint32_t new_nb = rwy_nblocks_of(size);
    uint32_t gap, grun = 0, l, on, ind;
    uint8_t z[RWY_BLOCK_SIZE] = {0};
    rwy_extent_t list[RWY_MAX_EXTENTS + 1];
    int rc;

    if (new_nb <= old_nb) return RWY_OK;
    gap = new_nb - old_nb;
    rc = rwy_extents_load(s, in, list, &on);
    if (rc) return rc;
    rc = rwy_w_alloc_run(s, gap, &grun);
    if (rc) return rc;
    for (l = 0; l < gap; l++) {
        rc = rwy_blk_write(&s->dev, RWY_IO_DATA, grun + l, z);
        if (rc) return rc;
    }
    rwy_extents_push(list, &on, grun, gap);
    rc = rwy_extents_reserve(s, on, &ind);
    if (rc) {
        for (l = 0; l < gap; l++) rwy_w_free(s, grun + l);
        return rc;
    }
    return rwy_extents_set(s, in, list, on, ind);
}

int rwy_inode_truncate(rwy_store_t *s, rwy_inode_t *in, uint64_t size) {
    int rc;
    int64_t sec; uint32_t ns;
    if (size == in->size) return RWY_OK;
    rc = size < in->size ? truncate_shrink(s, in, size) : truncate_grow(s, in, size);
    if (rc) return rc;
    in->size = size;
    rwy_now_of(s, &sec, &ns);
    in->mtime_sec = sec; in->mtime_nsec = ns;
    in->ctime_sec = sec; in->ctime_nsec = ns;
    in->generation = s->w_generation;
    return RWY_OK;
}

int rwy_truncate_oid(rwy_store_t *s, uint64_t oid, uint64_t size) {
    rwy_inode_t *in;
    int rc, shrink;
    if (!s) return RWY_ERR_INVAL;
    rc = rwy_need_txn(s);
    if (rc) return rc;
    if (oid & RWY_OID_VIRT) return rwy_virt_truncate(s, oid, size);
    rc = rwy_mut_inode(s, oid, &in);
    if (rc) return rc;
    if (in->type == RWY_TYPE_DIR) return RWY_ERR_ISDIR;
    {
        int noop = 0;
        rc = rwy_bootcfg_check_truncate(s, oid, size, &noop);
        if (rc) return rc;
        if (noop) return RWY_OK;
    }
    /* Shrinking frees space: it may take the state-change reserve. */
    shrink = size <= in->size;
    s->alloc_reserve += shrink;
    rc = rwy_inode_truncate(s, in, size);
    s->alloc_reserve -= shrink;
    return rc;
}

int rwy_truncate(rwy_store_t *s, const char *path, uint64_t size) {
    uint64_t o;
    int rc;
    if (!s || !path) return RWY_ERR_INVAL;
    rc = rwy_resolve_ext(s, path, 1, 1, &o);
    if (rc) return rc;
    return rwy_truncate_oid(s, o, size);
}
