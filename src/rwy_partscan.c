/*
 * rwy_partscan.c — Find and open store partitions and store files on media.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_boot.h"
#include "rwy_partinfo.h"
#include "rwy_partscan.h"

BOOLEAN same_disk(EFI_HANDLE a, EFI_HANDLE b) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH *pa, *pb;

    if (EFI_ERROR(gBS->HandleProtocol(a, &dp_guid, (void **)&pa)) ||
        EFI_ERROR(gBS->HandleProtocol(b, &dp_guid, (void **)&pb)))
        return FALSE;
    for (;;) {
        UINT16 la = pa->Length[0] | (pa->Length[1] << 8);
        UINT16 lb = pb->Length[0] | (pb->Length[1] << 8);
        if (pa->Type == MEDIA_DEVICE_PATH || pa->Type == END_DEVICE_PATH_TYPE)
            return pb->Type == pa->Type;
        if (pa->Type != pb->Type || pa->SubType != pb->SubType || la != lb ||
            CompareMem(pa, pb, la) != 0)
            return FALSE;
        pa = (EFI_DEVICE_PATH *)((UINT8 *)pa + la);
        pb = (EFI_DEVICE_PATH *)((UINT8 *)pb + la);
    }
}

void file_store_close(file_store_t *fs) {
    fs->fh->Close(fs->fh);
    fs->root->Close(fs->root);
}

EFI_STATUS file_store_open(EFI_HANDLE h, file_store_t *fs) {
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID fig = EFI_FILE_INFO_ID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    UINT8 info[128];
    UINTN isz = sizeof(info);
    EFI_STATUS s;

    if (EFI_ERROR(gBS->HandleProtocol(h, &sfs_guid, (void **)&sfs)) || !sfs) return EFI_UNSUPPORTED;
    s = sfs->OpenVolume(sfs, &fs->root);
    if (EFI_ERROR(s)) return s;
    s = fs->root->Open(fs->root, &fs->fh, L"\\RUNWAY.DAT", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (s == EFI_ACCESS_DENIED || s == EFI_WRITE_PROTECTED)
        s = fs->root->Open(fs->root, &fs->fh, L"\\RUNWAY.DAT", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s)) {
        fs->root->Close(fs->root);
        return s;
    }
    fs->size = 0;
    if (!EFI_ERROR(fs->fh->GetInfo(fs->fh, &fig, &isz, info))) fs->size = ((EFI_FILE_INFO *)info)->FileSize;
    return EFI_SUCCESS;
}

static BOOLEAN sb_frozen_valid(const rwy_superblock_t *sb) {
    if (!sb) return FALSE;
    if (CompareMem(sb->magic, "RUNWAYSB", 8) != 0) return FALSE;
    if (sb->version < 9) return FALSE;
    if (sb->block_size != RWY_BLOCK_SIZE) return FALSE;
    if (sb->total_size < RWY_MIN_STORE_SIZE || sb->total_size > RWY_MAX_STORE_SIZE) return FALSE;
    if (sb->total_blocks != (uint32_t)(sb->total_size / RWY_BLOCK_SIZE)) return FALSE;
    if (rwy_crc32c(sb, offsetof(rwy_superblock_t, crc32c)) != sb->crc32c) return FALSE;
    return TRUE;
}

BOOLEAN read_sb_slot(const rwy_block_dev_t *dev, int slot, rwy_superblock_t *out) {
    UINT8 blk[RWY_BLOCK_SIZE];

    if (dev->read(dev->ctx, (UINT64)slot * RWY_BLOCK_SIZE, blk, sizeof(blk)) != 0) return FALSE;
    memcpy(out, blk, sizeof(*out));
    return sb_frozen_valid(out);
}

EFI_STATUS read_store_active_sb(const rwy_block_dev_t *dev, rwy_superblock_t *out_sb,
                                int *out_slot, UINT64 *out_gen) {
    rwy_superblock_t sb_a, sb_b;
    BOOLEAN valid_a = read_sb_slot(dev, RWY_SB_SLOT_A, &sb_a);
    BOOLEAN valid_b = read_sb_slot(dev, RWY_SB_SLOT_B, &sb_b);

    if (!valid_a && !valid_b) return EFI_VOLUME_CORRUPTED;
    if (valid_a && (!valid_b || sb_a.generation >= sb_b.generation)) {
        *out_sb = sb_a;
        *out_slot = 0;
    } else {
        *out_sb = sb_b;
        *out_slot = 1;
    }
    if (out_gen) *out_gen = out_sb->generation;
    return EFI_SUCCESS;
}

BOOLEAN is_store_partition(EFI_HANDLE h) {
    EFI_GUID want = RWY_STORE_PART_TYPE, have;

    return rwy_partition_type(h, &have) && guid_eq(&have, &want);
}

BOOLEAN block_store_open(EFI_HANDLE h, rwy_backend_t *be, rwy_superblock_t *sb, int *slot, UINT64 *gen) {
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO_PROTOCOL *bio = NULL;

    if (EFI_ERROR(gBS->HandleProtocol(h, &bio_guid, (void **)&bio)) || !bio) return FALSE;
    if (EFI_ERROR(rwy_fs_block_backend(bio, be))) return FALSE;
    if (EFI_ERROR(read_store_active_sb(&be->dev, sb, slot, gen))) {
        rwy_fs_block_close(be);
        return FALSE;
    }
    if (EFI_ERROR(rwy_fs_block_limit(be, sb->total_size))) {
        RWY_WARN("store partition on handle %p is smaller than its store (%llu bytes)\n", h,
                 (unsigned long long)sb->total_size);
        rwy_fs_block_close(be);
        return FALSE;
    }
    return TRUE;
}

typedef struct {
    rwy_backend_t be;
    rwy_superblock_t sb;
    int slot;
    UINT64 gen;
    BOOLEAN on_esp;
    BOOLEAN valid;
} best_store_t;

static BOOLEAN is_better_candidate(UINT64 cand_gen, BOOLEAN cand_on_esp,
                                    UINT64 best_gen, BOOLEAN best_on_esp) {
    if (cand_gen > best_gen) return TRUE;
    if (cand_gen == best_gen && cand_on_esp && !best_on_esp) return TRUE;
    return FALSE;
}

static void update_best_block_store(EFI_HANDLE h, EFI_HANDLE esp, best_store_t *best) {
    rwy_backend_t cand_be;
    rwy_superblock_t cand_sb;
    int cand_slot = 0;
    UINT64 cand_gen = 0;
    BOOLEAN cand_on_esp;

    if (!is_store_partition(h)) return;
    if (!block_store_open(h, &cand_be, &cand_sb, &cand_slot, &cand_gen)) return;

    cand_on_esp = same_disk(h, esp);
    if (!best->valid || is_better_candidate(cand_gen, cand_on_esp, best->gen, best->on_esp)) {
        if (best->valid) rwy_fs_block_close(&best->be);
        best->be = cand_be;
        best->sb = cand_sb;
        best->slot = cand_slot;
        best->gen = cand_gen;
        best->on_esp = cand_on_esp;
        best->valid = TRUE;
    } else {
        rwy_fs_block_close(&cand_be);
    }
}

static BOOLEAN scan_best_block_store(EFI_HANDLE esp, rwy_backend_t *be, rwy_superblock_t *sb,
                                     int *slot, UINT64 *gen) {
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    UINTN num_handles = 0;
    EFI_HANDLE *handles = NULL;
    best_store_t best;

    memset(&best, 0, sizeof(best));
    if (!EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &num_handles, &handles)) && handles) {
        for (UINTN i = 0; i < num_handles; i++)
            update_best_block_store(handles[i], esp, &best);
        gBS->FreePool(handles);
    }
    if (!best.valid) return FALSE;
    *be = best.be;
    *sb = best.sb;
    if (slot) *slot = best.slot;
    if (gen) *gen = best.gen;
    return TRUE;
}

static BOOLEAN scan_file_store(EFI_HANDLE esp, rwy_backend_t *be, rwy_superblock_t *sb,
                               int *slot, UINT64 *gen) {
    file_store_t fs;

    if (EFI_ERROR(file_store_open(esp, &fs))) return FALSE;
    if (EFI_ERROR(rwy_fs_file_backend(fs.fh, fs.size, be))) {
        file_store_close(&fs);
        return FALSE;
    }
    if (fs.size < RWY_MIN_STORE_SIZE || fs.size > RWY_MAX_STORE_SIZE ||
        EFI_ERROR(read_store_active_sb(&be->dev, sb, slot, gen))) {
        rwy_fs_file_close(be);
        file_store_close(&fs);
        return FALSE;
    }
    return TRUE;
}

BOOLEAN rwy_scan_boot_store(EFI_HANDLE esp, rwy_backend_t *be, rwy_superblock_t *sb,
                            int *slot, UINT64 *gen) {
    if (scan_best_block_store(esp, be, sb, slot, gen)) return TRUE;
    return scan_file_store(esp, be, sb, slot, gen);
}
