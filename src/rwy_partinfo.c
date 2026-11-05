/*
 * rwy_partinfo.c — the GPT type of a partition handle (see rwy_partinfo.h).
 * SPDX-License-Identifier: Apache-2.0
 *
 * The partition info protocol gives it on UEFI 2.7 and later. Older firmware, such as the INSYDE 2.6 of some
 * industrial PCs, shows every partition as a block device with a device path node (number and unique GUID) and
 * nothing else, so the type is then read from the GPT of the disk.
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_gpt.h"
#include "rwy_partinfo.h"

#define BOUNCE_BYTES (64u * 1024u)

typedef struct {
    EFI_BLOCK_IO_PROTOCOL *bio;
    UINT8 *bounce; /* page aligned: it breaks no IoAlign */
} disk_t;

/* The disk is read in whole blocks through the bounce buffer. */
static int disk_read(void *ctx, uint64_t off, void *buf, size_t len) {
    disk_t *d = ctx;
    UINT32 bs = d->bio->Media->BlockSize;
    UINT64 lba = off / bs;
    size_t skip = off % bs;

    while (len) {
        size_t blocks = (skip + len + bs - 1) / bs, n;

        if (blocks * bs > BOUNCE_BYTES) blocks = BOUNCE_BYTES / bs;
        if (EFI_ERROR(d->bio->ReadBlocks(d->bio, d->bio->Media->MediaId, lba, blocks * bs, d->bounce))) return -1;
        n = blocks * bs - skip;
        if (n > len) n = len;
        memcpy(buf, d->bounce + skip, n);
        buf = (UINT8 *)buf + n;
        len -= n;
        lba += blocks;
        skip = 0;
    }
    return 0;
}

static EFI_BLOCK_IO_PROTOCOL *block_io(EFI_HANDLE h) {
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO_PROTOCOL *bio = NULL;

    if (EFI_ERROR(gBS->HandleProtocol(h, &bio_guid, (void **)&bio))) return NULL;
    return bio;
}

/* The hard drive node of the device path of a partition, if it names a GPT partition. */
static const HARDDRIVE_DEVICE_PATH *gpt_node(EFI_HANDLE part) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH *dp = NULL;

    if (EFI_ERROR(gBS->HandleProtocol(part, &dp_guid, (void **)&dp)) || !dp) return NULL;
    for (; !IsDevicePathEnd(dp); dp = NextDevicePathNode(dp)) {
        const HARDDRIVE_DEVICE_PATH *hd = (const HARDDRIVE_DEVICE_PATH *)dp;

        if (DevicePathType(dp) != MEDIA_DEVICE_PATH || DevicePathSubType(dp) != MEDIA_HARDDRIVE_DP) continue;
        return hd->SignatureType == SIGNATURE_TYPE_GUID ? hd : NULL;
    }
    return NULL;
}

/* The whole-disk block handle whose device path is the longest prefix of the one of the partition. */
static EFI_BLOCK_IO_PROTOCOL *parent_disk(EFI_HANDLE part) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID, bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_DEVICE_PATH *pp = NULL, *dp = NULL;
    EFI_BLOCK_IO_PROTOCOL *best = NULL;
    EFI_HANDLE *hs = NULL;
    UINTN n = 0, best_len = 0;

    if (EFI_ERROR(gBS->HandleProtocol(part, &dp_guid, (void **)&pp)) || !pp) return NULL;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &n, &hs))) return NULL;
    for (UINTN i = 0; i < n; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio = block_io(hs[i]);
        UINTN len;

        if (!bio || !bio->Media || bio->Media->LogicalPartition) continue;
        if (EFI_ERROR(gBS->HandleProtocol(hs[i], &dp_guid, (void **)&dp)) || !dp) continue;
        len = DevicePathSize(dp) - sizeof(EFI_DEVICE_PATH_PROTOCOL);
        if (len > best_len && len < DevicePathSize(pp) && !CompareMem(dp, pp, len)) {
            best = bio;
            best_len = len;
        }
    }
    gBS->FreePool(hs);
    return best;
}

/* The type of a GPT partition read from the table of its disk. */
static BOOLEAN type_from_table(EFI_HANDLE part, EFI_GUID *type) {
    const HARDDRIVE_DEVICE_PATH *hd = gpt_node(part);
    EFI_BLOCK_IO_PROTOCOL *own = block_io(part);
    disk_t d = { parent_disk(part), NULL };
    EFI_PHYSICAL_ADDRESS pages;
    int rc;

    if (!hd || !own || !own->Media || !own->Media->LogicalPartition || !d.bio || !d.bio->Media) return FALSE;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, BOUNCE_BYTES / 4096, &pages))) return FALSE;
    d.bounce = (UINT8 *)(UINTN)pages;
    rc = rwy_gpt_partition_type(disk_read, &d, d.bio->Media->BlockSize, d.bio->Media->LastBlock,
                                hd->PartitionNumber, hd->Signature, (uint8_t *)type);
    gBS->FreePages(pages, BOUNCE_BYTES / 4096);
    return rc == 0;
}

BOOLEAN rwy_partition_type(EFI_HANDLE part, EFI_GUID *type) {
    EFI_GUID info_guid = PARTITION_INFO_GUID;
    partition_info_t *info = NULL;

    if (EFI_ERROR(gBS->HandleProtocol(part, &info_guid, (void **)&info)) || !info) return type_from_table(part, type);
    if (info->type != PARTITION_INFO_GPT) return FALSE;
    memcpy(type, info->entry, sizeof(*type));
    return TRUE;
}
