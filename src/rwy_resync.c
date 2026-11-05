/*
 * rwy_resync.c — Catch up the lagging members of a mirror from the master.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_boot.h"

/* Read or write `size` bytes at `pos` of a store; a failed transfer is a device error. */
static EFI_STATUS dev_read_at(const rwy_block_dev_t *dev, UINT64 pos, VOID *buf, UINTN size) {
    return dev->read(dev->ctx, pos, buf, size) == 0 ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

static EFI_STATUS dev_write_at(const rwy_block_dev_t *dev, UINT64 pos, VOID *buf, UINTN size) {
    return dev->write(dev->ctx, pos, buf, size) == 0 ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

static EFI_STATUS dev_flush(const rwy_block_dev_t *dev) {
    return dev->flush && dev->flush(dev->ctx) != 0 ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

/* Read bitmap block `i` of the master and check its header and CRC; the payload is left in blk. */
static EFI_STATUS read_bitmap_block(const rwy_block_dev_t *dev, const rwy_superblock_t *sb, uint32_t i,
                                    const UINT8 *dek, rwy_xts_ctx_t *xts, UINT8 *blk) {
    uint32_t blk_idx = sb->bitmap_start + i;
    UINT8 raw[RWY_BLOCK_SIZE];
    rwy_blk_header_t *h;
    uint32_t saved_crc;
    EFI_STATUS s = dev_read_at(dev, (UINT64)blk_idx * RWY_BLOCK_SIZE, raw, sizeof(raw));

    if (EFI_ERROR(s)) return s;
    if (dek) rwy_xts_decrypt_sector(xts, blk_idx, raw, blk, RWY_BLOCK_SIZE);
    else memcpy(blk, raw, RWY_BLOCK_SIZE);
    h = (rwy_blk_header_t *)blk;
    if (h->magic != RWY_BLK_MAGIC || h->generation != sb->generation || h->kind != RWY_BLK_KIND_BITMAP)
        return EFI_VOLUME_CORRUPTED;
    saved_crc = h->crc32c;
    h->crc32c = 0;
    if (rwy_crc32c(blk, RWY_BLOCK_SIZE) != saved_crc) return EFI_CRC_ERROR;
    h->crc32c = saved_crc;
    return EFI_SUCCESS;
}

static EFI_STATUS master_read_bitmap(const rwy_block_dev_t *master,
                                     const rwy_superblock_t *sb,
                                     BOOLEAN is_encrypted,
                                     const UINT8 *dek,
                                     UINT8 **out_bitmap,
                                     UINTN *out_bm_size) {
    UINTN bm_size = ((UINTN)sb->total_blocks + 7) / 8;
    UINT8 *bm = NULL;
    EFI_STATUS s;
    rwy_xts_ctx_t xts;
    size_t payload_len = RWY_BLOCK_SIZE - sizeof(rwy_blk_header_t);
    uint32_t i;

    s = gBS->AllocatePool(EfiLoaderData, bm_size, (void **)&bm);
    if (EFI_ERROR(s)) return s;
    memset(bm, 0, bm_size);

    if (is_encrypted && dek) {
        rwy_xts_init_from_dek(&xts, dek);
    }

    for (i = 0; i < sb->bitmap_blocks; i++) {
        UINT8 blk[RWY_BLOCK_SIZE];
        size_t off = (size_t)i * payload_len, copy_len;

        s = read_bitmap_block(master, sb, i, is_encrypted ? dek : NULL, &xts, blk);
        if (EFI_ERROR(s)) { gBS->FreePool(bm); return s; }
        copy_len = bm_size > off ? (bm_size - off) : 0;
        if (copy_len > payload_len) copy_len = payload_len;
        if (copy_len > 0) {
            memcpy(bm + off, blk + sizeof(rwy_blk_header_t), copy_len);
        }
    }

    *out_bitmap = bm;
    *out_bm_size = bm_size;
    return EFI_SUCCESS;
}

#define SYNC_CHUNK_BLOCKS 16
#define SYNC_CHUNK_BYTES  (SYNC_CHUNK_BLOCKS * RWY_BLOCK_SIZE)

static int bitmap_bit(const UINT8 *bm, uint32_t b) {
    return (bm[b >> 3] >> (b & 7)) & 1;
}

/* Copy the blocks [start, start + len) from the master to the replica, one chunk at a time. */
static EFI_STATUS sync_copy_run(const rwy_block_dev_t *master, const rwy_block_dev_t *replica, UINT8 *copy_buf,
                                uint32_t start, uint32_t len) {
    while (len > 0) {
        uint32_t chunk_blks = len > SYNC_CHUNK_BLOCKS ? SYNC_CHUNK_BLOCKS : len;
        UINTN chunk_bytes = (UINTN)chunk_blks * RWY_BLOCK_SIZE;
        UINT64 pos = (UINT64)start * RWY_BLOCK_SIZE;
        EFI_STATUS s = dev_read_at(master, pos, copy_buf, chunk_bytes);

        if (EFI_ERROR(s)) return s;
        s = dev_write_at(replica, pos, copy_buf, chunk_bytes);
        if (EFI_ERROR(s)) return s;
        start += chunk_blks;
        len -= chunk_blks;
    }
    return EFI_SUCCESS;
}

/* Copy the live metadata and data blocks of the master generation. */
static EFI_STATUS sync_copy_live(const rwy_block_dev_t *master, const rwy_block_dev_t *replica, UINT8 *copy_buf,
                                 const rwy_superblock_t *sb, const UINT8 *bitmap, UINT64 *copied) {
    uint32_t b = RWY_SB_BLOCKS;

    while (b < sb->total_blocks) {
        uint32_t run_start;
        EFI_STATUS s;

        while (b < sb->total_blocks && !bitmap_bit(bitmap, b)) b++;
        if (b >= sb->total_blocks) break;
        run_start = b;
        while (b < sb->total_blocks && bitmap_bit(bitmap, b)) b++;
        s = sync_copy_run(master, replica, copy_buf, run_start, b - run_start);
        if (EFI_ERROR(s)) return s;
        *copied += b - run_start;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS sync_replica_from_master(const rwy_block_dev_t *master,
                                           int master_active_slot,
                                           const rwy_superblock_t *master_sb,
                                           const UINT8 *master_bitmap,
                                           const rwy_block_dev_t *replica, const UINT8 replica_uuid[16]) {
    UINT8 *copy_buf = NULL;
    EFI_STATUS s;
    UINT64 blocks_copied = 0, sb_pos = (UINT64)master_active_slot * RWY_BLOCK_SIZE;

    s = gBS->AllocatePool(EfiLoaderData, SYNC_CHUNK_BYTES, (void **)&copy_buf);
    if (EFI_ERROR(s)) return s;

    s = sync_copy_live(master, replica, copy_buf, master_sb, master_bitmap, &blocks_copied);
    /* The data blocks reach the replica before its superblock does. */
    if (!EFI_ERROR(s)) s = dev_flush(replica);
    if (!EFI_ERROR(s)) s = dev_read_at(master, sb_pos, copy_buf, RWY_BLOCK_SIZE);
    /* The replica keeps its own member UUID */
    if (!EFI_ERROR(s) && !is_zero_uuid(replica_uuid)) rwy_sb_block_set_member(copy_buf, replica_uuid);
    if (!EFI_ERROR(s)) s = dev_write_at(replica, sb_pos, copy_buf, RWY_BLOCK_SIZE);
    if (!EFI_ERROR(s)) s = dev_flush(replica);
    gBS->FreePool(copy_buf);
    if (EFI_ERROR(s)) return s;

    RWY_DEBUG("mirror: replica caught up (%llu live blocks copied, slot %c committed)\n",
            (unsigned long long)blocks_copied, master_active_slot == 0 ? 'A' : 'B');
    return EFI_SUCCESS;
}

/* A caught-up member holds the master's superblock, except for its own member UUID. */
static void adopt_master_sb(mirror_member_t *m, const mirror_member_t *master) {
    UINT8 own[16];

    memcpy(own, m->sb.member_uuid, 16);
    m->sb = master->sb;
    if (!is_zero_uuid(own)) memcpy(m->sb.member_uuid, own, 16);
}

/* Bring every lagging member up to the master's generation. A member that cannot be caught up is marked
 * degraded and left out: the master is good, so the group still mounts. */
EFI_STATUS catch_up_members(vol_group_t *g, UINTN master_idx, UINT64 max_gen, BOOLEAN is_encrypted,
                                   const UINT8 *dek) {
    mirror_member_t *master = &g->members[master_idx];
    UINT8 *master_bitmap = NULL;
    UINTN bm_size = 0, lagging_count = 0;
    EFI_STATUS s;

    for (UINTN i = 0; i < g->count; i++) {
        if (g->members[i].gen < max_gen) lagging_count++;
    }
    if (lagging_count == 0) return EFI_SUCCESS;
    s = master_read_bitmap(&master->backend.dev, &master->sb, is_encrypted, is_encrypted ? dek : NULL,
                           &master_bitmap, &bm_size);
    if (EFI_ERROR(s) || !master_bitmap)
        return mfail("failed to load master allocation bitmap for sync", s);

    for (UINTN i = 0; i < g->count; i++) {
        if (g->members[i].gen >= max_gen) continue;
        g->members[i].events |= 1u << RWY_SS_EV_LAGGING;
        RWY_INFO("mirror: member %u is lagging (generation %llu < %llu); synchronizing...\n",
                (unsigned int)i, (unsigned long long)g->members[i].gen, (unsigned long long)max_gen);
        s = sync_replica_from_master(&master->backend.dev, master->slot, &master->sb, master_bitmap,
                                     &g->members[i].backend.dev, g->members[i].sb.member_uuid);
        if (EFI_ERROR(s)) {
            RWY_WARN("mirror: member %u cannot be caught up (status 0x%llx), continuing without it\n",
                    (unsigned int)i, (unsigned long long)s);
            g->members[i].degraded = TRUE;
            g->members[i].events |= 1u << RWY_SS_EV_SYNC_FAILED;
            lagging_count--;
            continue;
        }
        g->members[i].gen = max_gen;
        g->members[i].slot = master->slot;
        adopt_master_sb(&g->members[i], master);
    }
    gBS->FreePool(master_bitmap);
    if (lagging_count)
        RWY_INFO("mirror: catch-up synchronization complete (%u replica(s) caught up)\n",
                (unsigned int)lagging_count);
    return EFI_SUCCESS;
}
