/*
 * src/rwy_cryptodev.c — crypto block device wrapper: AES-XTS below the Runway core.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "rwy_string.h"
#include "rwy_crypto.h"
#include "rwy_core.h"
#include "rwy_format.h"

static int is_encrypted_block(const rwy_crypto_dev_ctx_t *c, uint64_t b) {
    if (b < c->start_block) return 0;
    if (c->plain_flight_blocks &&
        b >= c->plain_flight_start &&
        b < c->plain_flight_start + 2 * c->plain_flight_blocks)
        return 0;
    if (c->plain_recov_blocks &&
        b >= c->plain_recov_start &&
        b < c->plain_recov_start + 2 * c->plain_recov_blocks)
        return 0;
    if (c->plain_loader_blocks &&
        b >= c->plain_loader_start &&
        b < c->plain_loader_start + 2 + 2 * c->plain_loader_blocks)
        return 0;
    if (c->plain_net_blocks &&
        b >= c->plain_net_start &&
        b < c->plain_net_start + 2 * c->plain_net_blocks)
        return 0;
    return 1;
}

static void init_plain_ranges(rwy_crypto_dev_ctx_t *ctx, const rwy_block_dev_t *underlying) {
    uint8_t blk[RWY_BLOCK_SIZE];
    for (unsigned slot = 0; slot < 2; slot++) {
        if (!underlying->read ||
            underlying->read(underlying->ctx, (uint64_t)slot * RWY_BLOCK_SIZE, blk, sizeof(blk)) != 0)
            continue;
        const rwy_superblock_t *sb = (const rwy_superblock_t *)blk;
        if (CompareMem(sb->magic, "RUNWAYSB", 8) == 0 &&
            rwy_crc32c(sb, offsetof(rwy_superblock_t, crc32c)) == sb->crc32c) {
            ctx->plain_flight_start = sb->flight_start;
            ctx->plain_flight_blocks = sb->flight_blocks;
            ctx->plain_recov_start = sb->recovery_start;
            ctx->plain_recov_blocks = sb->recovery_blocks;
            ctx->plain_loader_start = sb->loader_start;
            ctx->plain_loader_blocks = sb->loader_blocks;
            ctx->plain_net_start = sb->net_start;
            ctx->plain_net_blocks = sb->net_blocks;
            return;
        }
    }
}

/* A read that is not whole blocks and fits the bounce buffer: read the covering blocks once. */
static int read_covering(rwy_crypto_dev_ctx_t *c, uint64_t off, void *buf, size_t len) {
    uint64_t first = off / RWY_BLOCK_SIZE;
    uint64_t count = (off + len + RWY_BLOCK_SIZE - 1) / RWY_BLOCK_SIZE - first;

    if (c->underlying.read(c->underlying.ctx, first * RWY_BLOCK_SIZE, c->bounce, count * RWY_BLOCK_SIZE))
        return -1;
    for (uint64_t i = 0; i < count; i++) {
        uint8_t *blk = c->bounce + i * RWY_BLOCK_SIZE;
        if (is_encrypted_block(c, first + i))
            rwy_xts_decrypt_sector(&c->xts, first + i, blk, blk, RWY_BLOCK_SIZE);
    }
    memcpy(buf, c->bounce + (off - first * RWY_BLOCK_SIZE), len);
    return 0;
}

static int crypto_read_fn(void *ctx, uint64_t off, void *buf, size_t len) {
    rwy_crypto_dev_ctx_t *c = ctx;
    uint64_t span = (off + len + RWY_BLOCK_SIZE - 1) / RWY_BLOCK_SIZE - off / RWY_BLOCK_SIZE;

    if ((off % RWY_BLOCK_SIZE || len % RWY_BLOCK_SIZE) && span <= RWY_CRYPTO_BOUNCE_BLOCKS)
        return read_covering(c, off, buf, len);
    if (c->underlying.read(c->underlying.ctx, off, buf, len))
        return -1;

    uint64_t blk_start = off / RWY_BLOCK_SIZE;
    uint64_t blk_end = (off + len + RWY_BLOCK_SIZE - 1) / RWY_BLOCK_SIZE;

    for (uint64_t b = blk_start; b < blk_end; ++b) {
        if (is_encrypted_block(c, b)) {
            uint64_t blk_off = b * RWY_BLOCK_SIZE;
            size_t in_blk_start = (off > blk_off) ? (off - blk_off) : 0;
            size_t in_blk_end = ((off + len) < (blk_off + RWY_BLOCK_SIZE)) ?
                                ((off + len) - blk_off) : RWY_BLOCK_SIZE;

            if (in_blk_start == 0 && in_blk_end == RWY_BLOCK_SIZE) {
                uint8_t *p = (uint8_t *)buf + (blk_off - off);
                rwy_xts_decrypt_sector(&c->xts, b, p, p, RWY_BLOCK_SIZE);
            } else {
                uint8_t tmp[RWY_BLOCK_SIZE];
                if (c->underlying.read(c->underlying.ctx, blk_off, tmp, RWY_BLOCK_SIZE))
                    return -1;
                rwy_xts_decrypt_sector(&c->xts, b, tmp, tmp, RWY_BLOCK_SIZE);
                memcpy((uint8_t *)buf + (blk_off > off ? (blk_off - off) : 0),
                       tmp + in_blk_start, in_blk_end - in_blk_start);
            }
        }
    }
    return 0;
}

/* Number of blocks from b (up to max) that are all encrypted or all plain, like b. */
static uint64_t same_kind_run(const rwy_crypto_dev_ctx_t *c, uint64_t b, uint64_t max) {
    int enc = is_encrypted_block(c, b);
    uint64_t n = 1;
    while (n < max && is_encrypted_block(c, b + n) == enc) n++;
    return n;
}

/* Whole blocks at block offsets only; the store never writes less.
 * One backend call per plain range and per bounce buffer of encrypted blocks. */
static int crypto_write_fn(void *ctx, uint64_t off, const void *buf, size_t len) {
    rwy_crypto_dev_ctx_t *c = ctx;
    const uint8_t *p = buf;
    uint64_t b = off / RWY_BLOCK_SIZE, left = len / RWY_BLOCK_SIZE;

    if (off % RWY_BLOCK_SIZE || len % RWY_BLOCK_SIZE)
        return -1;

    while (left) {
        int enc = is_encrypted_block(c, b);
        uint64_t cap = (enc && left > RWY_CRYPTO_BOUNCE_BLOCKS) ? RWY_CRYPTO_BOUNCE_BLOCKS : left;
        uint64_t n = same_kind_run(c, b, cap);
        const uint8_t *out = p;

        if (enc) {
            for (uint64_t i = 0; i < n; i++)
                rwy_xts_encrypt_sector(&c->xts, b + i, p + i * RWY_BLOCK_SIZE,
                                       c->bounce + i * RWY_BLOCK_SIZE, RWY_BLOCK_SIZE);
            out = c->bounce;
        }
        if (c->underlying.write(c->underlying.ctx, b * RWY_BLOCK_SIZE, out, n * RWY_BLOCK_SIZE))
            return -1;
        b += n; p += n * RWY_BLOCK_SIZE; left -= n;
    }
    return 0;
}

static int crypto_flush_fn(void *ctx) {
    rwy_crypto_dev_ctx_t *c = ctx;
    if (c->underlying.flush)
        return c->underlying.flush(c->underlying.ctx);
    return 0;
}

int rwy_crypto_dev_wrap(rwy_block_dev_t *out_dev, rwy_crypto_dev_ctx_t *ctx,
                        const rwy_block_dev_t *underlying, const uint8_t dek[32],
                        uint32_t start_block) {
    if (!out_dev || !ctx || !underlying || !dek)
        return RWY_CRYPTO_ERR_INVAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->underlying = *underlying;
    ctx->start_block = start_block;
    memcpy(ctx->dek, dek, 32);

    rwy_xts_init_from_dek(&ctx->xts, dek);
    init_plain_ranges(ctx, underlying);

    out_dev->read = crypto_read_fn;
    out_dev->write = crypto_write_fn;
    out_dev->flush = crypto_flush_fn;
    out_dev->ctx = ctx;
    out_dev->size = underlying->size;
    out_dev->block_size = underlying->block_size;
    out_dev->stats = NULL;
    out_dev->dircache = NULL;

    return RWY_CRYPTO_OK;
}

int rwy_crypto_dev_rekey(rwy_crypto_dev_ctx_t *ctx, const uint8_t dek[32]) {
    if (!ctx || !dek)
        return RWY_CRYPTO_ERR_INVAL;

    memset(&ctx->xts, 0, sizeof(ctx->xts));
    memset(ctx->dek, 0, sizeof(ctx->dek));
    memcpy(ctx->dek, dek, 32);
    rwy_xts_init_from_dek(&ctx->xts, dek);
    return RWY_CRYPTO_OK;
}
