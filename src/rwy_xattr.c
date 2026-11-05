/*
 * rwy_xattr.c — Extended attributes.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

#define RWY_XATTR_PAYLOAD_MAX (RWY_BLOCK_SIZE - (uint32_t)sizeof(rwy_xattr_header_t))

#define RWY_XATTR_STREAM_MAX  65536u

static uint8_t xattr_scratch[RWY_XATTR_STREAM_MAX];

static int xattr_valid_name(const char *name, size_t *out_namelen) {
    size_t len;
    if (!name) return RWY_ERR_INVAL;
    len = rwy_strlen(name);
    if (len == 0 || len > RWY_NAME_MAX) return RWY_ERR_INVAL;
    if ((rwy_strncmp(name, "user.", 5) == 0 && len > 5) ||
        (rwy_strncmp(name, "trusted.", 8) == 0 && len > 8) ||
        (rwy_strncmp(name, "security.", 9) == 0 && len > 9) ||
        (rwy_strncmp(name, "system.", 7) == 0 && len > 7)) {
        if (out_namelen) *out_namelen = len;
        return RWY_OK;
    }
    return RWY_ERR_INVAL;
}

static int xattr_load_stream(const rwy_store_t *s, uint64_t xattr_root,
                             uint8_t *stream, size_t max_stream,
                             size_t *out_stream_len, uint32_t *out_count) {
    uint32_t cur = (uint32_t)xattr_root;
    size_t tot_bytes = 0;
    uint32_t tot_count = 0;
    uint32_t guard = 0;

    while (cur != 0) {
        uint8_t blk[RWY_BLOCK_SIZE];
        const rwy_xattr_header_t *xh;
        int rc;

        if (cur < s->sb.pool_start || cur >= s->sb.total_blocks)
            return RWY_ERR_CORRUPT;
        if (++guard > 64) return RWY_ERR_CORRUPT; /* cycle detection */

        rc = rwy_blk_read(&s->dev, RWY_IO_META, cur, blk);
        if (rc) return rc;

        xh = (const rwy_xattr_header_t *)blk;
        if (xh->hdr.magic != RWY_BLK_MAGIC || xh->hdr.kind != RWY_BLK_KIND_XATTR)
            return RWY_ERR_CORRUPT;
        if (rwy_blk_compute_crc(blk) != xh->hdr.crc32c)
            return RWY_ERR_CORRUPT;

        if (xh->used_bytes > RWY_XATTR_PAYLOAD_MAX)
            return RWY_ERR_CORRUPT;
        if (tot_bytes + xh->used_bytes > max_stream)
            return RWY_ERR_CORRUPT;

        {
            const uint8_t *src = blk + sizeof(rwy_xattr_header_t);
            for (size_t k = 0; k < xh->used_bytes; k++)
                stream[tot_bytes + k] = src[k];
        }
        tot_bytes += xh->used_bytes;
        tot_count += xh->count;
        cur = xh->next;
    }

    *out_stream_len = tot_bytes;
    *out_count = tot_count;
    return RWY_OK;
}

static int xattr_stream_find(const uint8_t *stream, size_t stream_len,
                             const char *name, size_t namelen,
                             const rwy_xattr_entry_t **out_entry,
                             const void **out_value,
                             size_t *out_entry_offset,
                             size_t *out_entry_total_len) {
    size_t off = 0;
    while (off < stream_len) {
        const rwy_xattr_entry_t *e;
        size_t elen;
        const char *ename;

        if (off + sizeof(rwy_xattr_entry_t) > stream_len) return 0;
        e = (const rwy_xattr_entry_t *)(stream + off);
        elen = sizeof(rwy_xattr_entry_t) + (size_t)e->namelen + (size_t)e->valuelen;
        if (off + elen > stream_len) return 0;

        ename = (const char *)(stream + off + sizeof(rwy_xattr_entry_t));
        if (e->namelen == namelen && !rwy_strncmp(ename, name, namelen)) {
            if (out_entry) *out_entry = e;
            if (out_value) *out_value = stream + off + sizeof(rwy_xattr_entry_t) + e->namelen;
            if (out_entry_offset) *out_entry_offset = off;
            if (out_entry_total_len) *out_entry_total_len = elen;
            return 1;
        }
        off += elen;
    }
    return 0;
}

/* Free the blocks of an xattr chain; a damaged chain frees what could be followed. */
static void xattr_free_chain(rwy_store_t *s, uint32_t xb) {
    uint32_t guard = 0;

    while (xb != 0 && guard++ < 64) {
        uint8_t blk[RWY_BLOCK_SIZE];
        const rwy_xattr_header_t *xh = (const rwy_xattr_header_t *)blk;
        rwy_w_free(s, xb);
        if (rwy_blk_read(&s->dev, RWY_IO_META, xb, blk)) break;
        if (xh->hdr.magic != RWY_BLK_MAGIC || xh->hdr.kind != RWY_BLK_KIND_XATTR) break;
        xb = xh->next;
    }
}

/* Write the stream into the blocks blks[0..nb), chained in order. */
static int xattr_write_blocks(rwy_store_t *s, const uint8_t *stream, size_t stream_len, uint32_t count,
                              const uint32_t *blks, uint32_t nb) {
    size_t rem = stream_len, off = 0;

    for (uint32_t i = 0; i < nb; i++) {
        uint8_t blk[RWY_BLOCK_SIZE];
        rwy_xattr_header_t *xh = (rwy_xattr_header_t *)blk;
        size_t chunk = rem > RWY_XATTR_PAYLOAD_MAX ? RWY_XATTR_PAYLOAD_MAX : rem;
        int rc;

        memset(blk, 0, sizeof(blk));
        rwy_blk_hdr_make(&xh->hdr, s->w_generation, i, RWY_BLK_KIND_XATTR);
        xh->count = (i == 0) ? count : 0;
        xh->used_bytes = (uint32_t)chunk;
        xh->next = (i + 1 < nb) ? blks[i + 1] : 0;
        memcpy(blk + sizeof(rwy_xattr_header_t), stream + off, chunk);
        xh->hdr.crc32c = rwy_blk_compute_crc(blk);
        rc = rwy_blk_write(&s->dev, RWY_IO_META, blks[i], blk);
        if (rc) return rc;
        off += chunk;
        rem -= chunk;
    }
    return RWY_OK;
}

static int xattr_save_stream(rwy_store_t *s, rwy_inode_t *in,
                             const uint8_t *stream, size_t stream_len,
                             uint32_t count) {
    uint32_t old_root = (uint32_t)in->xattr_root;
    uint32_t nb, blks[64] = { 0 };
    int rc;

    if (stream_len == 0) {
        xattr_free_chain(s, old_root);
        in->xattr_root = 0;
        return RWY_OK;
    }

    nb = (uint32_t)((stream_len + RWY_XATTR_PAYLOAD_MAX - 1) / RWY_XATTR_PAYLOAD_MAX);
    if (nb > 64) return RWY_ERR_NOSPC;

    /* Allocate all new blocks CoW */
    for (uint32_t i = 0; i < nb; i++) {
        rc = rwy_w_alloc_run(s, 1, &blks[i]);
        if (rc) {
            for (uint32_t k = 0; k < i; k++)
                rwy_w_free(s, blks[k]);
            return rc;
        }
    }
    rc = xattr_write_blocks(s, stream, stream_len, count, blks, nb);
    if (rc) {
        for (uint32_t k = 0; k < nb; k++)
            rwy_w_free(s, blks[k]);
        return rc;
    }
    xattr_free_chain(s, old_root);
    in->xattr_root = blks[0];
    s->sb.feat_compat |= RWY_FEAT_COMPAT_XATTR;
    return RWY_OK;
}

int rwy_getxattr_oid(rwy_store_t *s, uint64_t oid, const char *name,
                      void *buf, size_t size) {
    const rwy_inode_t *in;
    size_t namelen = 0;
    size_t stream_len = 0;
    uint32_t count = 0;
    const rwy_xattr_entry_t *entry = 0;
    const void *val = 0;
    size_t off = 0, tot = 0;
    int rc;

    if (!s || !name) return RWY_ERR_INVAL;
    if (oid & RWY_OID_VIRT) return RWY_ERR_XATTR;   /* virtual INDEX: a file with none */
    in = rwy_inode_of(s, oid, s->in_txn);
    if (!in) return RWY_ERR_NOENT;
    if (!in->xattr_root) return RWY_ERR_XATTR;

    rc = xattr_valid_name(name, &namelen);
    if (rc) return rc;

    rc = xattr_load_stream(s, in->xattr_root, xattr_scratch, sizeof(xattr_scratch),
                           &stream_len, &count);
    if (rc) return rc;

    if (!xattr_stream_find(xattr_scratch, stream_len, name, namelen,
                           &entry, &val, &off, &tot))
        return RWY_ERR_XATTR;

    if (size == 0) return (int)entry->valuelen;
    if (size < entry->valuelen) return RWY_ERR_RANGE;
    for (size_t k = 0; k < entry->valuelen; k++)
        ((uint8_t *)buf)[k] = ((const uint8_t *)val)[k];
    return (int)entry->valuelen;
}

int rwy_getxattr(rwy_store_t *s, const char *path, const char *name,
                  void *buf, size_t size) {
    uint64_t o;
    int rc;
    if (!s || !path || !name) return RWY_ERR_INVAL;
    rc = rwy_resolve_ext(s, path, s->in_txn, 0, &o);
    if (rc) return rc;
    return rwy_getxattr_oid(s, o, name, buf, size);
}

/* Drop the entry `name` from the stream when there is one, as the flags allow. */
static int xattr_stream_drop(size_t namelen, const char *name, int flags, size_t *stream_len, uint32_t *count) {
    const rwy_xattr_entry_t *found_entry = 0;
    const void *found_val = 0;
    size_t found_off = 0, found_tot = 0, tail;

    if (!*stream_len ||
        !xattr_stream_find(xattr_scratch, *stream_len, name, namelen,
                           &found_entry, &found_val, &found_off, &found_tot))
        return (flags & XATTR_REPLACE) ? RWY_ERR_XATTR : RWY_OK;
    if (flags & XATTR_CREATE) return RWY_ERR_EXIST;
    tail = *stream_len - (found_off + found_tot);
    for (size_t k = 0; k < tail; k++)
        xattr_scratch[found_off + k] = xattr_scratch[found_off + found_tot + k];
    *stream_len -= found_tot;
    (*count)--;
    return RWY_OK;
}

/* Add the entry at the end of the stream. */
static int xattr_stream_append(const char *name, size_t namelen, const void *value, size_t size, int flags,
                               size_t *stream_len, uint32_t *count) {
    size_t new_entry_len = sizeof(rwy_xattr_entry_t) + namelen + size;
    rwy_xattr_entry_t *ne;
    uint8_t *payload;

    if (*stream_len + new_entry_len > sizeof(xattr_scratch)) return RWY_ERR_NOSPC;
    ne = (rwy_xattr_entry_t *)(xattr_scratch + *stream_len);
    payload = (uint8_t *)(ne + 1);
    ne->namelen = (uint16_t)namelen;
    ne->valuelen = (uint16_t)size;
    ne->flags = (uint32_t)flags;
    memcpy(payload, name, namelen);
    if (size) memcpy(payload + namelen, value, size);
    *stream_len += new_entry_len;
    (*count)++;
    return RWY_OK;
}

int rwy_setxattr_oid(rwy_store_t *s, uint64_t oid, const char *name,
                      const void *value, size_t size, int flags) {
    rwy_inode_t *in;
    size_t namelen = 0;
    size_t stream_len = 0;
    uint32_t count = 0;
    int rc;

    if (!s || !name || (!value && size > 0)) return RWY_ERR_INVAL;
    rc = rwy_need_txn(s);
    if (rc) return rc;
    rc = rwy_mut_inode(s, oid, &in);
    if (rc) return rc;
    rc = rwy_bootcfg_check_attr(s, oid);
    if (rc) return rc;

    rc = xattr_valid_name(name, &namelen);
    if (rc) return rc;
    if (size > 65535) return RWY_ERR_NOSPC;

    if (in->xattr_root) {
        rc = xattr_load_stream(s, in->xattr_root, xattr_scratch, sizeof(xattr_scratch),
                               &stream_len, &count);
        if (rc) return rc;
    }
    rc = xattr_stream_drop(namelen, name, flags, &stream_len, &count);
    if (rc) return rc;

    rc = xattr_stream_append(name, namelen, value, size, flags, &stream_len, &count);
    if (rc) return rc;

    rc = xattr_save_stream(s, in, xattr_scratch, stream_len, count);
    if (rc) return rc;

    {
        int64_t sec; uint32_t ns;
        rwy_now_of(s, &sec, &ns);
        in->ctime_sec = sec;
        in->ctime_nsec = ns;
        in->generation = s->w_generation;
    }
    return RWY_OK;
}

int rwy_setxattr(rwy_store_t *s, const char *path, const char *name,
                  const void *value, size_t size, int flags) {
    uint64_t o;
    int rc;
    if (!s || !path || !name || (!value && size > 0)) return RWY_ERR_INVAL;
    rc = rwy_resolve_ext(s, path, s->in_txn, 0, &o);
    if (rc) return rc;
    return rwy_setxattr_oid(s, o, name, value, size, flags);
}

int rwy_listxattr_oid(rwy_store_t *s, uint64_t oid, char *list, size_t size) {
    const rwy_inode_t *in;
    size_t stream_len = 0;
    uint32_t count = 0;
    size_t tot_list_len = 0;
    size_t off = 0;
    int rc;

    if (!s) return RWY_ERR_INVAL;
    if (oid & RWY_OID_VIRT) return 0;
    in = rwy_inode_of(s, oid, s->in_txn);
    if (!in) return RWY_ERR_NOENT;
    if (!in->xattr_root) return 0;

    rc = xattr_load_stream(s, in->xattr_root, xattr_scratch, sizeof(xattr_scratch),
                           &stream_len, &count);
    if (rc) return rc;

    off = 0;
    while (off < stream_len) {
        const rwy_xattr_entry_t *e;
        size_t elen;
        if (off + sizeof(rwy_xattr_entry_t) > stream_len) return RWY_ERR_CORRUPT;
        e = (const rwy_xattr_entry_t *)(xattr_scratch + off);
        elen = sizeof(rwy_xattr_entry_t) + (size_t)e->namelen + (size_t)e->valuelen;
        if (off + elen > stream_len) return RWY_ERR_CORRUPT;
        tot_list_len += (size_t)e->namelen + 1;
        off += elen;
    }

    if (size == 0) return (int)tot_list_len;
    if (size < tot_list_len) return RWY_ERR_RANGE;

    off = 0;
    size_t out_pos = 0;
    while (off < stream_len) {
        const rwy_xattr_entry_t *e = (const rwy_xattr_entry_t *)(xattr_scratch + off);
        const char *ename = (const char *)(xattr_scratch + off + sizeof(rwy_xattr_entry_t));
        for (size_t k = 0; k < e->namelen; k++) list[out_pos++] = ename[k];
        list[out_pos++] = '\0';
        off += sizeof(rwy_xattr_entry_t) + (size_t)e->namelen + (size_t)e->valuelen;
    }
    return (int)tot_list_len;
}

int rwy_listxattr(rwy_store_t *s, const char *path, char *list, size_t size) {
    uint64_t o;
    int rc;
    if (!s || !path) return RWY_ERR_INVAL;
    rc = rwy_resolve_ext(s, path, s->in_txn, 0, &o);
    if (rc) return rc;
    return rwy_listxattr_oid(s, o, list, size);
}

int rwy_removexattr_oid(rwy_store_t *s, uint64_t oid, const char *name) {
    rwy_inode_t *in;
    size_t namelen = 0;
    size_t stream_len = 0;
    uint32_t count = 0;
    const rwy_xattr_entry_t *entry = 0;
    const void *val = 0;
    size_t off = 0, tot = 0;
    int rc;

    if (!s || !name) return RWY_ERR_INVAL;
    rc = rwy_need_txn(s);
    if (rc) return rc;
    rc = rwy_mut_inode(s, oid, &in);
    if (rc) return rc;
    rc = rwy_bootcfg_check_attr(s, oid);
    if (rc) return rc;
    if (!in->xattr_root) return RWY_ERR_XATTR;

    rc = xattr_valid_name(name, &namelen);
    if (rc) return rc;

    rc = xattr_load_stream(s, in->xattr_root, xattr_scratch, sizeof(xattr_scratch),
                           &stream_len, &count);
    if (rc) return rc;

    if (!xattr_stream_find(xattr_scratch, stream_len, name, namelen,
                           &entry, &val, &off, &tot))
        return RWY_ERR_XATTR;

    /* Remove entry from stream */
    size_t tail = stream_len - (off + tot);
    for (size_t k = 0; k < tail; k++)
        xattr_scratch[off + k] = xattr_scratch[off + tot + k];
    stream_len -= tot;
    count--;

    rc = xattr_save_stream(s, in, xattr_scratch, stream_len, count);
    if (rc) return rc;

    {
        int64_t sec; uint32_t ns;
        rwy_now_of(s, &sec, &ns);
        in->ctime_sec = sec;
        in->ctime_nsec = ns;
        in->generation = s->w_generation;
    }
    return RWY_OK;
}

int rwy_removexattr(rwy_store_t *s, const char *path, const char *name) {
    uint64_t o;
    int rc;
    if (!s || !path || !name) return RWY_ERR_INVAL;
    rc = rwy_resolve_ext(s, path, s->in_txn, 0, &o);
    if (rc) return rc;
    return rwy_removexattr_oid(s, o, name);
}
