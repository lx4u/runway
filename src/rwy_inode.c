/*
 * rwy_inode.c — Inode access, extent maps and inode allocation.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

const rwy_inode_t *rwy_inode_of(const rwy_store_t *s, uint64_t oid, int working) {
    const rwy_inode_t *tbl = working ? s->w_inodes : s->inodes;
    uint64_t use;
    if (oid == 0 || oid > RWY_MAX_INODES) return 0;
    use = oid - 1;
    if (tbl[use].oid != oid || tbl[use].type == RWY_TYPE_FREE) return 0;
    return &tbl[use];
}

rwy_inode_t *rwy_winode_of(rwy_store_t *s, uint64_t oid) {
    uint64_t use;
    if (!s->in_txn) return 0;
    if (oid == 0 || oid > RWY_MAX_INODES) return 0;
    use = oid - 1;
    if (s->w_inodes[use].oid != oid || s->w_inodes[use].type == RWY_TYPE_FREE) return 0;
    return &s->w_inodes[use];
}

int rwy_mut_inode(rwy_store_t *s, uint64_t oid, rwy_inode_t **out) {
    if (oid & RWY_OID_VIRT) return RWY_ERR_PERM;
    *out = rwy_winode_of(s, oid);
    return *out ? RWY_OK : RWY_ERR_NOENT;
}

/* Physical block for logical block `l` of inode `in` (reads committed OR
 * working image depending on which table `in` comes from). Indirect extents
 * are read from the backing device (they are never mutated in place). */
int rwy_phys_find(const rwy_store_t *s, const rwy_inode_t *in, uint32_t l,
                      uint32_t *out, uint32_t *run) {
    uint32_t acc = 0, e;
    for (e = 0; e < in->extent_count && e < RWY_DIRECT_EXTENTS; e++) {
        if (l >= acc && l < acc + in->extents[e].len) {
            *out = in->extents[e].start + (l - acc);
            *run = in->extents[e].len - (l - acc);
            return RWY_OK;
        }
        acc += in->extents[e].len;
    }
    if (in->indirect) {
        uint8_t blk[RWY_BLOCK_SIZE];
        uint32_t pairs = (RWY_BLOCK_SIZE - (uint32_t)sizeof(rwy_blk_header_t)) / 8;
        uint32_t i = 0, base = acc;
        int rc = rwy_blk_read(&s->dev, RWY_IO_EXTENT, in->indirect, blk);
        if (rc) return rc;
        /* verify header loosely (generation may be older than current) */
        {
            const rwy_blk_header_t *h = (const rwy_blk_header_t *)blk;
            if (h->magic != RWY_BLK_MAGIC || h->kind != RWY_BLK_KIND_INDIR)
                return RWY_ERR_CORRUPT;
            if (rwy_blk_compute_crc(blk) != h->crc32c) return RWY_ERR_CORRUPT;
        }
        for (i = 0; i < pairs; i++) {
            const uint8_t *q = blk + sizeof(rwy_blk_header_t) + i * 8;
            uint32_t st = (uint32_t)q[0] | ((uint32_t)q[1] << 8) |
                          ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
            uint32_t ln = (uint32_t)q[4] | ((uint32_t)q[5] << 8) |
                          ((uint32_t)q[6] << 16) | ((uint32_t)q[7] << 24);
            if (st == 0 && ln == 0) break;
            if (l >= base && l < base + ln) {
                *out = st + (l - base);
                *run = ln - (l - base);
                return RWY_OK;
            }
            base += ln;
        }
    }
    return RWY_ERR_IO;
}

int rwy_phys_of(const rwy_store_t *s, const rwy_inode_t *in, uint32_t l, uint32_t *out) {
    uint32_t run;
    return rwy_phys_find(s, in, l, out, &run);
}

uint32_t rwy_nblocks_of(uint64_t size) {
    return (uint32_t)((size + RWY_BLOCK_SIZE - 1) / RWY_BLOCK_SIZE);
}

/* Read bytes from an inode's data. Works on committed or working image.
 * Coalesces contiguous extent runs (direct or indirect) into single backing reads. */
int rwy_inode_pread(const rwy_store_t *s, const rwy_inode_t *in,
                       uint64_t pos, void *buf, size_t want, size_t *got) {
    uint8_t *out = buf;
    size_t done = 0;
    *got = 0;
    if (pos >= in->size) return RWY_OK;
    if (pos + want > in->size) want = (size_t)(in->size - pos);
    while (done < want) {
        uint64_t cur = pos + done;
        uint32_t l = (uint32_t)(cur / RWY_BLOCK_SIZE);
        uint32_t off = (uint32_t)(cur % RWY_BLOCK_SIZE);
        uint32_t phys, run, n;
        uint64_t maxb, rem = (uint64_t)(want - done);
        int rc = rwy_phys_find(s, in, l, &phys, &run);
        if (rc) return rc;
        maxb = (uint64_t)run * RWY_BLOCK_SIZE - off;
        n = maxb > rem ? (uint32_t)rem : (uint32_t)maxb;
        if (in->type == RWY_TYPE_DIR) {
            if (n > RWY_BLOCK_SIZE - off) n = RWY_BLOCK_SIZE - off;
            rc = rwy_dircache_read(&s->dev, phys, off, out + done, n);
        } else
            rc = rwy_dev_pread(&s->dev, RWY_IO_DATA, (uint64_t)phys * RWY_BLOCK_SIZE + off, out + done, n);
        if (rc) return rc;
        done += n;
    }
    *got = done;
    return RWY_OK;
}

/* Every extent of `in`, direct then indirect, in logical order. `out` holds RWY_MAX_EXTENTS. */
int rwy_extents_load(const rwy_store_t *s, const rwy_inode_t *in, rwy_extent_t *out, uint32_t *n) {
    uint32_t i, cnt = in->extent_count < RWY_DIRECT_EXTENTS ? in->extent_count : RWY_DIRECT_EXTENTS;
    for (i = 0; i < cnt; i++) out[i] = in->extents[i];
    if (in->indirect) {
        uint8_t blk[RWY_BLOCK_SIZE];
        const rwy_blk_header_t *h = (const rwy_blk_header_t *)blk;
        int rc = rwy_blk_read(&s->dev, RWY_IO_EXTENT, in->indirect, blk);
        if (rc) return rc;
        if (h->magic != RWY_BLK_MAGIC || h->kind != RWY_BLK_KIND_INDIR ||
            rwy_blk_compute_crc(blk) != h->crc32c)
            return RWY_ERR_CORRUPT;
        for (i = 0; i < RWY_INDIRECT_PAIRS; i++) {
            const uint8_t *q = blk + sizeof(*h) + i * 8;
            uint32_t st = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
            uint32_t ln = (uint32_t)q[4] | ((uint32_t)q[5] << 8) | ((uint32_t)q[6] << 16) | ((uint32_t)q[7] << 24);
            if (!st && !ln) break;
            out[cnt].start = st; out[cnt].len = ln; cnt++;
        }
    }
    *n = cnt;
    return RWY_OK;
}

/* Append a run, merged into the last extent when it continues it. */
void rwy_extents_push(rwy_extent_t *list, uint32_t *n, uint32_t start, uint32_t len) {
    if (!len) return;
    if (*n && list[*n - 1].start + list[*n - 1].len == start) list[*n - 1].len += len;
    else { list[*n].start = start; list[*n].len = len; (*n)++; }
}

/* Append the part of `src` that covers the logical blocks [from, to). */
void rwy_extents_range(rwy_extent_t *list, uint32_t *n, const rwy_extent_t *src, uint32_t sn,
                        uint32_t from, uint32_t to) {
    uint32_t i, acc = 0;
    for (i = 0; i < sn && acc < to; i++) {
        uint32_t lo = from > acc ? from : acc;
        uint32_t hi = acc + src[i].len < to ? acc + src[i].len : to;
        if (lo < hi) rwy_extents_push(list, n, src[i].start + (lo - acc), hi - lo);
        acc += src[i].len;
    }
}

/* Reserve the indirect block that `n` extents need (none when they fit the inode). Takes the
 * block with no compaction: callers hold staged blocks. Call before changing anything. */
int rwy_extents_reserve(rwy_store_t *s, uint32_t n, uint32_t *ind) {
    *ind = 0;
    if (n > RWY_MAX_EXTENTS) return RWY_ERR_NOSPC;
    if (n <= RWY_DIRECT_EXTENTS) return RWY_OK;
    if (rwy_w_reserve_ok(s, 1)) return RWY_ERR_NOSPC;
    return rwy_bmp_alloc_run(s->w_bitmap, s->sb.total_blocks, s->next_hint, 1, ind);
}

/* Make `list` (n extents) the extents of `in`; `ind` comes from rwy_extents_reserve(). The
 * indirect block the inode had is released. */
int rwy_extents_set(rwy_store_t *s, rwy_inode_t *in, const rwy_extent_t *list, uint32_t n, uint32_t ind) {
    uint32_t i, old_ind = in->indirect;
    if (ind) {
        uint8_t blk[RWY_BLOCK_SIZE] = {0};
        rwy_blk_header_t *h = (rwy_blk_header_t *)blk;
        int rc;
        rwy_blk_hdr_make(h, s->w_generation, 0, RWY_BLK_KIND_INDIR);
        for (i = RWY_DIRECT_EXTENTS; i < n; i++) {
            uint8_t *q = blk + sizeof(*h) + (i - RWY_DIRECT_EXTENTS) * 8;
            uint32_t v[2] = { list[i].start, list[i].len };
            uint32_t k;
            for (k = 0; k < 8; k++) q[k] = (uint8_t)(v[k / 4] >> (8 * (k % 4)));
        }
        h->crc32c = rwy_blk_compute_crc(blk);
        rc = rwy_blk_write(&s->dev, RWY_IO_EXTENT, ind, blk);
        if (rc) return rc;
    }
    for (i = 0; i < RWY_DIRECT_EXTENTS; i++) {
        if (i < n) in->extents[i] = list[i];
        else in->extents[i].start = in->extents[i].len = 0;
    }
    in->extent_count = n < RWY_DIRECT_EXTENTS ? n : RWY_DIRECT_EXTENTS;
    in->indirect = ind;
    if (old_ind) rwy_w_free(s, old_ind);
    return RWY_OK;
}

/* Free all data blocks of a working inode (truncate to 0 helper). */
void rwy_winode_free_data(rwy_store_t *s, rwy_inode_t *in) {
    uint32_t e;
    for (e = 0; e < in->extent_count && e < RWY_DIRECT_EXTENTS; e++) {
        uint32_t k;
        for (k = 0; k < in->extents[e].len; k++)
            rwy_w_free(s, in->extents[e].start + k);
        in->extents[e].start = in->extents[e].len = 0;
    }
    if (in->indirect) {
        uint8_t blk[RWY_BLOCK_SIZE];
        if (rwy_blk_read(&s->dev, RWY_IO_EXTENT, in->indirect, blk) == RWY_OK) {
            uint32_t pairs = (RWY_BLOCK_SIZE - (uint32_t)sizeof(rwy_blk_header_t)) / 8, i;
            for (i = 0; i < pairs; i++) {
                const uint8_t *q = blk + sizeof(rwy_blk_header_t) + i * 8;
                uint32_t st = (uint32_t)q[0]|((uint32_t)q[1]<<8)|((uint32_t)q[2]<<16)|((uint32_t)q[3]<<24);
                uint32_t ln = (uint32_t)q[4]|((uint32_t)q[5]<<8)|((uint32_t)q[6]<<16)|((uint32_t)q[7]<<24);
                uint32_t k;
                if (!st && !ln) break;
                for (k = 0; k < ln; k++) rwy_w_free(s, st + k);
            }
        }
        rwy_w_free(s, in->indirect);
        in->indirect = 0;
    }
    in->extent_count = 0;
    in->size = 0;
}

void rwy_fill_attr(const rwy_inode_t *in, rwy_attr_t *a) {
    a->oid = in->oid; a->mode = in->mode; a->uid = in->uid; a->gid = in->gid;
    a->size = in->size;
    a->atime_sec = in->atime_sec; a->atime_nsec = in->atime_nsec;
    a->mtime_sec = in->mtime_sec; a->mtime_nsec = in->mtime_nsec;
    a->ctime_sec = in->ctime_sec; a->ctime_nsec = in->ctime_nsec;
    a->btime_sec = in->btime_sec; a->btime_nsec = in->btime_nsec;
    a->type = in->type;
    a->nlink = in->nlink;
}

/* Allocate a free inode slot in the working table. */
rwy_inode_t *rwy_alloc_inode(rwy_store_t *s, uint64_t *out_oid) {
    uint32_t i;
    for (i = 1; i < RWY_MAX_INODES; i++) {
        if (s->w_inodes[i].type == RWY_TYPE_FREE) {
            *out_oid = (uint64_t)i + 1;
            return &s->w_inodes[i];
        }
    }
    return 0;
}

void rwy_inode_init(rwy_store_t *s, rwy_inode_t *in, uint64_t oid,
                       uint8_t type, uint32_t mode, uint32_t uid, uint32_t gid) {
    int64_t sec; uint32_t ns;
    uint32_t i;
    for (i = 0; i < sizeof(*in); i++) ((uint8_t *)in)[i] = 0;
    in->oid = oid;
    in->type = type;
    in->mode = mode;
    in->uid = uid; in->gid = gid;
    in->nlink = 1;
    rwy_now_of(s, &sec, &ns);
    in->atime_sec = in->mtime_sec = in->ctime_sec = in->btime_sec = sec;
    in->atime_nsec = in->mtime_nsec = in->ctime_nsec = in->btime_nsec = ns;
    in->generation = s->w_generation;
}
