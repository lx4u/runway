/*
 * rwy_recboot.c — Read the recovery bundle of the boot group.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_fs.h"
#include "rwy_native.h"
#include "../include/rwy_recarea.h"

static char copy_name(unsigned copy) { return copy ? 'B' : 'A'; }

/* A copy that is not used while the other one is. A spare copy that was never written is not news. */
static void report_spare(const rwy_ra_info_t *info) {
    unsigned bad = info->copy ^ 1;

    if (info->status[bad] == RWY_RA_OK || info->status[bad] == RWY_RA_EMPTY) return;
    RWY_ERROR("recovery area copy %c invalid (%s), using copy %c\n", copy_name(bad),
            rwy_ra_reason(info->status[bad]), copy_name(info->copy));
}

/* The reason to show when no copy of a member is valid: the first one that is not just empty. */
static int area_reason(const rwy_ra_info_t *info) {
    return info->status[0] != RWY_RA_EMPTY ? info->status[0] : info->status[1];
}

static void free_parts(blob_t part[RWY_RA_KINDS]) {
    for (unsigned k = 0; k < RWY_RA_KINDS; k++) {
        if (part[k].buf) gBS->FreePool(part[k].buf);
        part[k].buf = NULL;
        part[k].size = 0;
    }
}

static EFI_STATUS read_parts(const rwy_block_dev_t *raw, const rwy_superblock_t *sb, const rwy_ra_info_t *info,
                             blob_t part[RWY_RA_KINDS]) {
    for (unsigned e = 0; e < info->count; e++) {
        const rwy_ra_entry_t *en = &info->entry[e];
        size_t len = 0;

        if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, (UINTN)en->length, (void **)&part[en->kind].buf)))
            return EFI_OUT_OF_RESOURCES;
        if (rwy_ra_read(raw, sb->recovery_start, sb->recovery_blocks, info, en->kind, part[en->kind].buf,
                         (size_t)en->length, &len) != RWY_OK)
            return EFI_VOLUME_CORRUPTED;
        part[en->kind].size = len;
    }
    return EFI_SUCCESS;
}

EFI_STATUS recboot_load(const rwy_block_dev_t devs[], const rwy_superblock_t sbs[], UINTN count, EFI_STATUS mount_err,
                        blob_t part[RWY_RA_KINDS]) {
    int reason = -1;

    for (UINTN i = 0; i < count; i++) {
        const rwy_block_dev_t *raw = &devs[i];
        rwy_ra_info_t info;
        EFI_STATUS s;

        if (!sbs[i].recovery_blocks) continue;
        if (rwy_ra_pick(raw, sbs[i].recovery_start, sbs[i].recovery_blocks, &info) != RWY_OK) {
            if (reason < 0) reason = area_reason(&info);
            continue;
        }
        if (EFI_ERROR(mount_err))
            RWY_ERROR("boot store cannot be mounted (0x%llx), booting the recovery area\n",
                    (unsigned long long)mount_err);
        else
            RWY_ERROR("no bootable config, booting the recovery area\n");
        report_spare(&info);
        s = read_parts(raw, &sbs[i], &info, part);
        if (!EFI_ERROR(s)) return EFI_SUCCESS;
        free_parts(part);
        if (reason < 0) reason = RWY_RA_BAD_HASH;
    }
    if (reason < 0) return EFI_NOT_FOUND;
    RWY_ERROR("recovery area invalid (%s), not booting\n", rwy_ra_reason(reason));
    return EFI_LOAD_ERROR;
}
