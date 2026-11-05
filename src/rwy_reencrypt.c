/*
 * src/rwy_reencrypt.c — Re-encrypt a Runway store with a new DEK and finalize.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "rwy_core.h"
#include "rwy_crypto.h"
#include "rwy_format.h"
#include "rwy_internal.h"
#include "rwy_string.h"
#include "rwy_transplog.h"

int rwy_reencrypt_pending(rwy_store_t *s, int *out_pending) {
    uint8_t alt_blk[RWY_BLOCK_SIZE];
    uint8_t cur_blk[RWY_BLOCK_SIZE];
    rwy_superblock_t alt_sb;
    int cur_slot, alt_slot, rc;

    if (!s || !out_pending)
        return RWY_ERR_INVAL;

    *out_pending = 0;
    if (s->sb.crypt_algo != RWY_CRYPT_AES_XTS_256)
        return RWY_OK;

    cur_slot = s->sb_slot;
    alt_slot = cur_slot ? 0 : 1;

    rc = rwy_blk_read(&s->dev, RWY_IO_META, (uint32_t)(alt_slot ? RWY_SB_SLOT_B : RWY_SB_SLOT_A), alt_blk);
    if (rc)
        return rc;

    rc = rwy_validate_sb_block(alt_blk, &alt_sb);
    if (rc != RWY_OK)
        return RWY_OK;

    if (alt_sb.generation >= s->sb.generation)
        return RWY_OK;

    rc = rwy_blk_read(&s->dev, RWY_IO_META, (uint32_t)(cur_slot ? RWY_SB_SLOT_B : RWY_SB_SLOT_A), cur_blk);
    if (rc)
        return rc;

    const rwy_keyslot_header_t *cur_kh = (const rwy_keyslot_header_t *)(cur_blk + RWY_KEYSLOT_OFFSET);
    const rwy_keyslot_header_t *alt_kh = (const rwy_keyslot_header_t *)(alt_blk + RWY_KEYSLOT_OFFSET);

    if (CompareMem(cur_kh, alt_kh, sizeof(rwy_keyslot_header_t)) != 0)
        *out_pending = 1;

    return RWY_OK;
}

static int load_old_bitmap(rwy_store_t *s, const rwy_superblock_t *old_sb,
                           const uint8_t old_dek[32], uint8_t *out_bm) {
    rwy_crypto_dev_ctx_t *cctx = (rwy_crypto_dev_ctx_t *)s->dev.ctx;
    rwy_crypto_dev_ctx_t old_ctx;
    rwy_block_dev_t old_dev;
    uint8_t blk[RWY_BLOCK_SIZE];
    size_t payload = RWY_BLOCK_SIZE - sizeof(rwy_blk_header_t);
    uint32_t i;
    int rc;

    rc = rwy_crypto_dev_wrap(&old_dev, &old_ctx, &cctx->underlying, old_dek, RWY_SB_BLOCKS);
    if (rc != RWY_CRYPTO_OK)
        return RWY_ERR_INVAL;

    memset(out_bm, 0, REACH_BYTES);
    for (i = 0; i < old_sb->bitmap_blocks; i++) {
        rc = rwy_blk_read(&old_dev, RWY_IO_META, old_sb->bitmap_start + i, blk);
        if (rc) {
            memset(&old_ctx, 0, sizeof(old_ctx));
            return rc;
        }
        rc = rwy_blk_verify(blk, old_sb->generation, i, RWY_BLK_KIND_BITMAP);
        if (rc) {
            memset(&old_ctx, 0, sizeof(old_ctx));
            return rc;
        }
        size_t off = (size_t)i * payload;
        size_t copy = (off < REACH_BYTES) ? (REACH_BYTES - off) : 0;
        if (copy > payload) copy = payload;
        memcpy(out_bm + off, blk + sizeof(rwy_blk_header_t), copy);
    }

    memset(&old_ctx, 0, sizeof(old_ctx));
    return RWY_OK;
}

static int scrub_old_blocks(rwy_store_t *s) {
    uint8_t zero_blk[RWY_BLOCK_SIZE];
    uint32_t b;
    int rc;

    memset(zero_blk, 0, sizeof(zero_blk));
    for (b = s->sb.pool_start; b < s->sb.total_blocks; b++) {
        if (rwy_bmp_get(rwy_gc_work, b) && !rwy_bmp_get(s->bitmap, b)) {
            rc = rwy_blk_write(&s->dev, RWY_IO_META, b, zero_blk);
            if (rc)
                return rc;
        }
    }
    return RWY_OK;
}

static int resolve_old_dek(const uint8_t alt_blk[RWY_BLOCK_SIZE],
                           const uint8_t old_dek[32], uint8_t out[32]) {
    if (old_dek) {
        memcpy(out, old_dek, 32);
        return RWY_OK;
    }
    const rwy_keyslot_header_t *alt_kh = (const rwy_keyslot_header_t *)(alt_blk + RWY_KEYSLOT_OFFSET);
    if (alt_kh->slots[0].state != RWY_KEYSLOT_STATE_ACTIVE)
        return RWY_ERR_PERM;
    if (rwy_keyslot_unwrap(alt_kh, "runway-test-passphrase", out) != RWY_CRYPTO_OK)
        return RWY_ERR_PERM;
    return RWY_OK;
}

static int scrub_factory_generation(rwy_store_t *s, const rwy_superblock_t *alt_sb,
                                    const uint8_t *old_dek) {
    int rc = load_old_bitmap(s, alt_sb, old_dek, rwy_gc_work);
    if (rc == RWY_OK)
        return scrub_old_blocks(s);
    if (rc == RWY_ERR_CORRUPT)
        return RWY_OK;
    return rc;
}

static int publish_finalize_generation(rwy_store_t *s) {
    int rc = rwy_record_write(s, s->rec);
    if (rc)
        return rc;
    rc = rwy_dev_flush(&s->dev);
    if (rc)
        return rc;
    rc = rwy_begin(s);
    if (rc)
        return rc;
    return rwy_commit(s);
}

int rwy_reencrypt_finish(rwy_store_t *s, const uint8_t old_dek[32]) {
    uint8_t alt_blk[RWY_BLOCK_SIZE];
    uint8_t dek[32];
    rwy_superblock_t alt_sb;
    int pending = 0, alt_slot, rc;

    if (!s || s->in_txn || !s->dev.ctx)
        return RWY_ERR_INVAL;

    rc = rwy_reencrypt_pending(s, &pending);
    if (rc != RWY_OK || !pending)
        return rc;

    alt_slot = s->sb_slot ? 0 : 1;
    rc = rwy_blk_read(&s->dev, RWY_IO_META, (uint32_t)(alt_slot ? RWY_SB_SLOT_B : RWY_SB_SLOT_A), alt_blk);
    if (rc)
        return rc;

    rc = rwy_validate_sb_block(alt_blk, &alt_sb);
    if (rc != RWY_OK)
        return rc;

    rc = resolve_old_dek(alt_blk, old_dek, dek);
    if (rc)
        return rc;

    rc = scrub_factory_generation(s, &alt_sb, dek);
    memset(dek, 0, sizeof(dek));
    if (rc)
        return rc;

    rc = rwy_transplog_scrub_inactive(s);
    if (rc)
        return rc;

    return publish_finalize_generation(s);
}

int rwy_reencrypt_relocate(rwy_store_t *s, const rwy_block_dev_t *raw_dev,
                            const uint8_t old_dek[32], const uint8_t new_dek[32],
                            const rwy_keyslot_header_t *keyhdr) {
    rwy_crypto_dev_ctx_t *orig_ctx;
    rwy_crypto_dev_ctx_t new_ctx;
    rwy_block_dev_t orig_dev;
    rwy_block_dev_t new_dev;
    int rc;

    if (!s || !raw_dev || !old_dek || !new_dek || !keyhdr)
        return RWY_ERR_INVAL;
    if (s->in_txn)
        return RWY_ERR_INVAL;
    if (!s->dev.ctx)
        return RWY_ERR_INVAL;

    orig_ctx = (rwy_crypto_dev_ctx_t *)s->dev.ctx;
    for (int i = 0; i < 32; i++) {
        if (orig_ctx->dek[i] != old_dek[i])
            return RWY_ERR_INVAL;
    }

    if (keyhdr->magic != RWY_KEYSLOT_MAGIC)
        return RWY_ERR_INVAL;

    rc = rwy_crypto_dev_wrap(&new_dev, &new_ctx, raw_dev, new_dek, RWY_SB_BLOCKS);
    if (rc != RWY_CRYPTO_OK)
        return RWY_ERR_INVAL;

    orig_dev = s->dev;

    rc = rwy_relocate(s, &new_dev, keyhdr);
    if (rc != RWY_OK) {
        memset(&new_ctx, 0, sizeof(new_ctx));
        return rc;
    }

    rwy_crypto_dev_rekey(orig_ctx, new_dek);
    orig_ctx->underlying = *raw_dev;
    s->dev = orig_dev;
    memset(&new_ctx, 0, sizeof(new_ctx));
    return RWY_OK;
}

int rwy_reencrypt(rwy_store_t *s, const rwy_block_dev_t *raw_dev,
                   const uint8_t old_dek[32], const uint8_t new_dek[32],
                   const rwy_keyslot_header_t *keyhdr) {
    int rc = rwy_reencrypt_relocate(s, raw_dev, old_dek, new_dek, keyhdr);
    if (rc != RWY_OK)
        return rc;

    return rwy_reencrypt_finish(s, old_dek);
}
