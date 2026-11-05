/*
 * rwy_partscan.h — Partition and store scanning primitives for UEFI.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_PARTSCAN_H
#define RWY_PARTSCAN_H

#include <efi.h>
#include <efilib.h>
#include "rwy_core.h"
#include "rwy_fs.h"

/* Partition type GUID of the store partition (Extended Boot Loader partition). */
#define RWY_STORE_PART_TYPE \
    { 0xbc13c2ff, 0x59e6, 0x4262, { 0xa3, 0x52, 0xb2, 0x75, 0xfd, 0x6f, 0x71, 0x72 } }

/* A store file on a firmware file system, with the volume root it was opened from. */
typedef struct {
    EFI_FILE_HANDLE root;
    EFI_FILE_HANDLE fh;
    UINT64 size;
} file_store_t;

/* TRUE if both handles sit on the same disk (equal device paths up to the partition node). */
BOOLEAN same_disk(EFI_HANDLE a, EFI_HANDLE b);

/* TRUE if the handle is a GPT partition of the store partition type. */
BOOLEAN is_store_partition(EFI_HANDLE h);

/* Read and check one superblock slot of a store. */
BOOLEAN read_sb_slot(const rwy_block_dev_t *dev, int slot, rwy_superblock_t *out);

/* The newest valid superblock slot of a store. */
EFI_STATUS read_store_active_sb(const rwy_block_dev_t *dev, rwy_superblock_t *out_sb,
                                int *out_slot, UINT64 *out_gen);

/* Open a store partition and check its superblock. */
BOOLEAN block_store_open(EFI_HANDLE h, rwy_backend_t *be, rwy_superblock_t *sb,
                         int *slot, UINT64 *gen);

/* Open \RUNWAY.DAT on a Simple File System handle. */
EFI_STATUS file_store_open(EFI_HANDLE h, file_store_t *fs);

/* Close a file store and its volume root. */
void file_store_close(file_store_t *fs);

/* Discover and open the best store partition across disks, or the store file on the loaded ESP. */
BOOLEAN rwy_scan_boot_store(EFI_HANDLE esp, rwy_backend_t *be, rwy_superblock_t *sb,
                            int *slot, UINT64 *gen);

#endif /* RWY_PARTSCAN_H */
