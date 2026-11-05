/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "rwy_format.h"

#define XATTR_STREAM_MAX 65536

static void claim(check_ctx_t *c, uint32_t blk, uint64_t oid, const char *what) {
    if (blk >= c->total_blocks) {
        ERR(c, "block %u out of range (%s, oid %llu)", blk, what,
            (unsigned long long)oid);
        return;
    }
    if (blk < c->pool_start) {
        ERR(c, "block %u in superblock or image area (%s, oid %llu)", blk, what,
            (unsigned long long)oid);
        return;
    }
    if (c->owner[blk] != -1)
        ERR(c, "block %u doubly referenced (%s oid %llu, already %d)", blk, what,
            (unsigned long long)oid, c->owner[blk]);
    else
        c->owner[blk] = (int32_t)oid;
}

/* True when the stored CRC of a metadata block matches (the CRC field itself counts as zero). */
static int blk_crc_ok(const uint8_t *blk) {
    uint8_t tmp[RWY_BLOCK_SIZE];
    memcpy(tmp, blk, sizeof(tmp));
    tmp[24] = tmp[25] = tmp[26] = tmp[27] = 0;
    return rwy_crc32c(tmp, sizeof(tmp)) == ((const rwy_blk_header_t *)blk)->crc32c;
}

/*
 * Mount already verified these headers while loading. Verify them again,
 * reserved word included, so the corruption class is reported by name and
 * blocks repair (torn-write detection).
 */
void check_blk_headers(check_ctx_t *c, rwy_block_dev_t *dev, const rwy_store_t *s, uint32_t start,
                                              uint32_t count, uint32_t kind, const char *what) {
    uint8_t blk[RWY_BLOCK_SIZE];
    const rwy_blk_header_t *h = (const rwy_blk_header_t *)blk;
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (dev->read(dev->ctx, (uint64_t)(start + i) * RWY_BLOCK_SIZE, blk, sizeof(blk))) {
            ERR(c, "cannot read %s block %u", what, i);
            continue;
        }
        if (h->magic != RWY_BLK_MAGIC || h->kind != kind ||
            h->generation != s->sb.generation || h->index != i)
            ERR(c, "%s block %u: bad/torn header", what, i);
        if (h->reserved != 0)
            ERR(c, "%s block %u: nonzero reserved header field", what, i);
        if (!blk_crc_ok(blk))
            ERR(c, "%s block %u: bad CRC (torn write)", what, i);
    }
}

static void check_xattr_entry_name(check_ctx_t *c, uint64_t oid, const char *name, size_t len) {
    static const char *const ns[] = { "user.", "trusted.", "security.", "system." };
    int valid_ns = 0;
    size_t i;

    for (i = 0; i < sizeof(ns) / sizeof(ns[0]); i++) {
        size_t nl = strlen(ns[i]);
        if (len >= nl && !memcmp(name, ns[i], nl)) valid_ns = 1;
    }
    if (!valid_ns)
        ERR(c, "oid %llu: invalid xattr namespace for '%.*s'", (unsigned long long)oid, (int)len, name);
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch < 0x20 || ch == 0x7f)
            ERR(c, "oid %llu: invalid char in xattr name '%.*s'", (unsigned long long)oid, (int)len, name);
    }
}

static void check_xattr_stream(check_ctx_t *c, uint64_t oid, const uint8_t *stream, size_t stream_len,
                                               uint32_t total_count) {
    size_t eoff = 0;
    uint32_t ecount = 0;

    while (eoff < stream_len) {
        const rwy_xattr_entry_t *e = (const rwy_xattr_entry_t *)(stream + eoff);
        size_t elen;
        if (eoff + sizeof(rwy_xattr_entry_t) > stream_len) {
            ERR(c, "oid %llu: truncated xattr entry in stream", (unsigned long long)oid);
            break;
        }
        elen = sizeof(rwy_xattr_entry_t) + (size_t)e->namelen + (size_t)e->valuelen;
        if (eoff + elen > stream_len) {
            ERR(c, "oid %llu: xattr entry exceeds stream length", (unsigned long long)oid);
            break;
        }
        if (e->namelen == 0 || e->namelen > 255)
            ERR(c, "oid %llu: invalid xattr namelen %u", (unsigned long long)oid, e->namelen);
        else
            check_xattr_entry_name(c, oid, (const char *)(stream + eoff + sizeof(*e)), e->namelen);
        ecount++;
        eoff += elen;
    }
    if (ecount != total_count)
        ERR(c, "oid %llu: xattr count mismatch (header=%u, found=%u)",
            (unsigned long long)oid, total_count, ecount);
}

/* Check one xattr block and append its payload to the stream; -1 when the chain cannot go on. */
static int check_xattr_block(check_ctx_t *c, const rwy_store_t *s, uint64_t oid,
                                             uint32_t xblk, uint32_t chain_idx, const rwy_xattr_header_t *xh,
                                             uint8_t *stream, size_t *stream_len) {
    size_t max_payload = RWY_BLOCK_SIZE - sizeof(rwy_xattr_header_t);
    const uint8_t *payload = (const uint8_t *)xh + sizeof(rwy_xattr_header_t);

    if (xh->hdr.magic != RWY_BLK_MAGIC || xh->hdr.kind != RWY_BLK_KIND_XATTR ||
        xh->hdr.generation > s->sb.generation || xh->hdr.index != chain_idx)
        ERR(c, "oid %llu: bad/torn xattr header at block %u", (unsigned long long)oid, xblk);
    if (xh->hdr.reserved != 0)
        ERR(c, "oid %llu: nonzero reserved in xattr header at block %u", (unsigned long long)oid, xblk);
    if (!all_zero(xh->reserved, sizeof(xh->reserved)))
        ERR(c, "oid %llu: nonzero reserved bytes in xattr header at block %u", (unsigned long long)oid, xblk);
    if (!blk_crc_ok((const uint8_t *)xh))
        ERR(c, "oid %llu: bad CRC at xattr block %u (torn write)", (unsigned long long)oid, xblk);
    if (xh->used_bytes > max_payload) {
        ERR(c, "oid %llu: xattr block %u used_bytes %u exceeds max %zu",
            (unsigned long long)oid, xblk, xh->used_bytes, max_payload);
        return -1;
    }
    if (*stream_len + xh->used_bytes > XATTR_STREAM_MAX) {
        ERR(c, "oid %llu: xattr stream exceeds buffer", (unsigned long long)oid);
        return -1;
    }
    memcpy(stream + *stream_len, payload, xh->used_bytes);
    *stream_len += xh->used_bytes;
    return 0;
}

static void check_xattr_chain(check_ctx_t *c, rwy_block_dev_t *dev, const rwy_store_t *s, const rwy_inode_t *in) {
    static uint8_t stream[XATTR_STREAM_MAX];
    uint32_t xblk = (uint32_t)in->xattr_root;
    uint32_t chain_idx = 0, total_count = 0;
    size_t stream_len = 0;
    int chain_err = 0;

    while (xblk != 0) {
        uint8_t blk[RWY_BLOCK_SIZE];
        const rwy_xattr_header_t *xh = (const rwy_xattr_header_t *)blk;
        if (xblk < c->pool_start || xblk >= c->total_blocks) {
            ERR(c, "oid %llu: bad xattr block %u", (unsigned long long)in->oid, xblk);
            return;
        }
        claim(c, xblk, in->oid, "xattr");
        if (dev->read(dev->ctx, (uint64_t)xblk * RWY_BLOCK_SIZE, blk, sizeof(blk))) {
            ERR(c, "oid %llu: cannot read xattr block %u", (unsigned long long)in->oid, xblk);
            return;
        }
        if (check_xattr_block(c, s, in->oid, xblk, chain_idx, xh, stream, &stream_len))
            chain_err = 1;
        if (chain_idx == 0) total_count = xh->count;
        xblk = (uint32_t)xh->next;
        chain_idx++;
        if (chain_idx > c->total_blocks) {
            ERR(c, "oid %llu: loop detected in xattr chain", (unsigned long long)in->oid);
            return;
        }
    }
    if (!chain_err)
        check_xattr_stream(c, in->oid, stream, stream_len, total_count);
}

static void check_inode_reserved(check_ctx_t *c, const rwy_inode_t *in) {
    unsigned long long oid = (unsigned long long)in->oid;

    if (in->flags != 0)
        ERR(c, "oid %llu: nonzero flags (reserved)", oid);
    if (in->crc_reserved != 0)
        ERR(c, "oid %llu: nonzero crc_reserved (reserved)", oid);
    if (in->reserved0[0] || in->reserved0[1] || in->reserved0[2])
        ERR(c, "oid %llu: nonzero reserved0 (reserved)", oid);
    if (!all_zero(in->reserved1, sizeof(in->reserved1)))
        ERR(c, "oid %llu: nonzero reserved1 (reserved)", oid);
}

static uint32_t le32(const uint8_t *q) {
    return (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
}

/* Claim the blocks of the indirect extents; returns how many data blocks they hold. */
static uint32_t check_indirect(check_ctx_t *c, file_t *fl, const rwy_inode_t *in) {
    uint8_t blk[RWY_BLOCK_SIZE];
    const rwy_blk_header_t *h = (const rwy_blk_header_t *)blk;
    uint32_t pairs = (RWY_BLOCK_SIZE - 32) / 8, p, k, nb = 0;

    if (file_rd(fl, (uint64_t)in->indirect * RWY_BLOCK_SIZE, blk, sizeof(blk))) {
        ERR(c, "oid %llu: cannot read indirect", (unsigned long long)in->oid);
        return 0;
    }
    if (h->magic != RWY_BLK_MAGIC || h->kind != RWY_BLK_KIND_INDIR) {
        ERR(c, "oid %llu: bad indirect header (torn write)", (unsigned long long)in->oid);
        return 0;
    }
    for (p = 0; p < pairs; p++) {
        uint32_t st = le32(blk + 32 + p * 8), ln = le32(blk + 32 + p * 8 + 4);
        if (!st && !ln) break;
        if (ln == 0) ERR(c, "oid %llu: empty indirect extent", (unsigned long long)in->oid);
        for (k = 0; k < ln; k++) claim(c, st + k, in->oid, "indirect-data");
        nb += ln;
    }
    return nb;
}

static void check_inode_fields(check_ctx_t *c, const rwy_inode_t *in) {
    unsigned long long oid = (unsigned long long)in->oid;

    if (in->type != RWY_TYPE_FILE && in->type != RWY_TYPE_DIR && in->type != RWY_TYPE_SYMLINK)
        ERR(c, "oid %llu: bad type %u", oid, in->type);
    if (!(in->mode & RWY_S_IFMT))
        ERR(c, "oid %llu: mode lacks file-type bits", oid);
    if (in->extent_count > RWY_DIRECT_EXTENTS)
        ERR(c, "oid %llu: extent_count %u", oid, in->extent_count);
    if (in->nlink == 0)
        ERR(c, "oid %llu: nlink is zero", oid);
    check_inode_reserved(c, in);
    if (in->indirect && in->indirect < c->pool_start)
        ERR(c, "oid %llu: bad indirect %u", oid, in->indirect);
    if (in->atime_nsec > 999999999u || in->mtime_nsec > 999999999u ||
        in->ctime_nsec > 999999999u)
        ERR(c, "oid %llu: nsec out of range", oid);
}

/* Claim every data block of the inode and check that they cover its size. */
static void check_inode_blocks(check_ctx_t *c, file_t *fl, const rwy_inode_t *in, scan_t *sc) {
    unsigned long long oid = (unsigned long long)in->oid;
    uint32_t e, k, nb = 0, need;

    if (in->indirect) claim(c, in->indirect, in->oid, "indirect");
    for (e = 0; e < in->extent_count && e < RWY_DIRECT_EXTENTS; e++) {
        uint32_t st = in->extents[e].start, ln = in->extents[e].len;
        if (ln == 0) { ERR(c, "oid %llu: empty extent %u", oid, e); continue; }
        for (k = 0; k < ln; k++) claim(c, st + k, in->oid, "data");
        nb += ln;
        sc->referenced_data += ln;
    }
    if (in->indirect) nb += check_indirect(c, fl, in);
    need = (uint32_t)((in->size + RWY_BLOCK_SIZE - 1) / RWY_BLOCK_SIZE);
    if (nb < need)
        ERR(c, "oid %llu: size %llu needs %u blocks, has %u", oid,
            (unsigned long long)in->size, need, nb);
}

void check_inode(check_ctx_t *c, file_t *fl, rwy_block_dev_t *dev, const rwy_store_t *s, uint32_t i,
                                        scan_t *sc) {
    const rwy_inode_t *in = &s->inodes[i];

    if (in->type == RWY_TYPE_FREE) {
        if (in->oid != 0) ERR(c, "slot %u: free but oid=%llu", i, (unsigned long long)in->oid);
        if (in->xattr_root != 0)
            ERR(c, "slot %u: free inode has orphan xattr_root %llu", i,
                (unsigned long long)in->xattr_root);
        return;
    }
    if (in->oid != (uint64_t)i + 1)
        ERR(c, "slot %u: oid mismatch %llu", i, (unsigned long long)in->oid);
    sc->live_oids[sc->nlive++] = in->oid;
    check_inode_fields(c, in);
    if (in->xattr_root != 0)
        check_xattr_chain(c, dev, s, in);
    check_inode_blocks(c, fl, in, sc);
}
