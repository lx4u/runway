/*
 * rwy_io.c — Block I/O helpers and metadata block headers (magic/generation/CRC32C).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

void rwy_io_count(rwy_io_stats_t *st, rwy_io_kind_t kind, int write, size_t len) {
    int b = len <= 4096 ? 0 : len <= 65536 ? 1 : len <= 1048576 ? 2 : 3;

    if (!st) return;
    st->calls[write][kind][b]++;
    st->bytes[write][kind] += len;
}

void rwy_dircache_drop(const rwy_block_dev_t *d) {
    if (d->dircache) memset(d->dircache->tag, 0, sizeof(d->dircache->tag));
}

/* Copy len bytes at off (inside one block) of a directory block, reading the block once. */
int rwy_dircache_read(const rwy_block_dev_t *d, uint32_t blk, uint32_t off, void *buf, size_t len) {
    rwy_dircache_t *c = d->dircache;
    uint32_t i = blk % RWY_DIRCACHE_BLOCKS;
    int rc;

    if (!c) return rwy_dev_pread(d, RWY_IO_DIR, (uint64_t)blk * RWY_BLOCK_SIZE + off, buf, len);
    if (c->tag[i] != blk + 1) {
        c->tag[i] = 0;
        rc = rwy_blk_read(d, RWY_IO_DIR, blk, c->data[i]);
        if (rc) return rc;
        c->tag[i] = blk + 1;
    }
    memcpy(buf, c->data[i] + off, len);
    return RWY_OK;
}

int rwy_dev_pread(const rwy_block_dev_t *d, rwy_io_kind_t kind, uint64_t off, void *buf, size_t len) {
    if (d->read(d->ctx, off, buf, len)) return RWY_ERR_IO;
    rwy_io_count(d->stats, kind, 0, len);
    return RWY_OK;
}

int rwy_dev_pwrite(const rwy_block_dev_t *d, rwy_io_kind_t kind, uint64_t off, const void *buf, size_t len) {
    rwy_dircache_drop(d);
    if (d->write(d->ctx, off, buf, len)) return RWY_ERR_IO;
    rwy_io_count(d->stats, kind, 1, len);
    return RWY_OK;
}

int rwy_blk_read(const rwy_block_dev_t *d, rwy_io_kind_t kind, uint32_t blk, void *buf) {
    if ((uint64_t)blk * RWY_BLOCK_SIZE + RWY_BLOCK_SIZE > d->size) return RWY_ERR_IO;
    return rwy_dev_pread(d, kind, (uint64_t)blk * RWY_BLOCK_SIZE, buf, RWY_BLOCK_SIZE);
}

int rwy_blk_write(const rwy_block_dev_t *d, rwy_io_kind_t kind, uint32_t blk, const void *buf) {
    if ((uint64_t)blk * RWY_BLOCK_SIZE + RWY_BLOCK_SIZE > d->size) return RWY_ERR_IO;
    return rwy_dev_pwrite(d, kind, (uint64_t)blk * RWY_BLOCK_SIZE, buf, RWY_BLOCK_SIZE);
}

int rwy_blk_write_run(const rwy_block_dev_t *d, rwy_io_kind_t kind, uint32_t blk, const void *buf, uint32_t n) {
    uint64_t off = (uint64_t)blk * RWY_BLOCK_SIZE, len = (uint64_t)n * RWY_BLOCK_SIZE;
    if (!n || off + len > d->size) return RWY_ERR_IO;
    return rwy_dev_pwrite(d, kind, off, buf, (size_t)len);
}

int rwy_dev_flush(const rwy_block_dev_t *d) {
    if (d->flush && d->flush(d->ctx)) return RWY_ERR_IO;
    if (d->stats) d->stats->flushes++;
    return RWY_OK;
}

void rwy_blk_hdr_make(rwy_blk_header_t *h, uint64_t gen, uint32_t idx, uint32_t kind) {
    h->magic = RWY_BLK_MAGIC;
    h->generation = gen;
    h->index = idx;
    h->kind = kind;
    h->crc32c = 0;
    h->reserved = 0;
}

uint32_t rwy_blk_compute_crc(const uint8_t blk[RWY_BLOCK_SIZE]) {
    uint8_t tmp[RWY_BLOCK_SIZE];
    size_t i;
    uint32_t crc;
    for (i = 0; i < sizeof(tmp); i++) tmp[i] = blk[i];
    /* zero the crc field at offset 24 */
    tmp[24] = tmp[25] = tmp[26] = tmp[27] = 0;
    crc = rwy_crc32c(tmp, sizeof(tmp));
    return crc;
}

int rwy_blk_verify(const uint8_t blk[RWY_BLOCK_SIZE], uint64_t gen,
                      uint32_t idx, uint32_t kind) {
    const rwy_blk_header_t *h = (const rwy_blk_header_t *)blk;
    if (h->magic != RWY_BLK_MAGIC) return RWY_ERR_CORRUPT;
    if (h->generation != gen) return RWY_ERR_CORRUPT;
    if (h->index != idx) return RWY_ERR_CORRUPT;
    if (h->kind != kind) return RWY_ERR_CORRUPT;
    if (rwy_blk_compute_crc(blk) != h->crc32c) return RWY_ERR_CORRUPT;
    return RWY_OK;
}
