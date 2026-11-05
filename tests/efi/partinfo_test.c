/*
 * partinfo_test — the type of a partition must be the same with and without the partition info protocol.
 * SPDX-License-Identifier: Apache-2.0
 *
 * For every partition the firmware shows, ask for the type with the protocol, remove the protocol from the handle
 * as old firmware does not have it, and ask again: the loader then reads the type from the GPT of the disk.
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_partinfo.h"

static int failures;

static void check_partition(EFI_HANDLE h) {
    EFI_GUID info_guid = PARTITION_INFO_GUID;
    partition_info_t *info = NULL;
    EFI_GUID with, without;

    if (EFI_ERROR(gBS->HandleProtocol(h, &info_guid, (void **)&info)) || !info) {
        uprint("PARTINFOTEST: FAIL no partition info protocol to compare with\n");
        failures++;
        return;
    }
    if (!rwy_partition_type(h, &with)) {
        uprint("PARTINFOTEST: FAIL no type with the protocol\n");
        failures++;
        return;
    }
    if (EFI_ERROR(gBS->UninstallProtocolInterface(h, &info_guid, info))) {
        uprint("PARTINFOTEST: FAIL cannot remove the protocol\n");
        failures++;
        return;
    }
    if (!rwy_partition_type(h, &without) || !guid_eq(&with, &without)) {
        uprintf("PARTINFOTEST: FAIL type %08x without the protocol (read: %s)\n", (unsigned)without.Data1,
                rwy_partition_type(h, &without) ? "other" : "none");
        failures++;
        return;
    }
    uprintf("PARTINFOTEST: ok type %08x\n", (unsigned)with.Data1);
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE *hs = NULL;
    UINTN n = 0, parts = 0;

    util_init(ImageHandle, SystemTable);
    uprint("PARTINFOTEST\n");
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &n, &hs))) failures++;
    for (UINTN i = 0; !failures && i < n; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio = NULL;

        if (EFI_ERROR(gBS->HandleProtocol(hs[i], &bio_guid, (void **)&bio)) || !bio->Media->LogicalPartition) continue;
        parts++;
        check_partition(hs[i]);
    }
    if (parts < 3) {
        uprintf("PARTINFOTEST: FAIL only %u partitions\n", (unsigned)parts);
        failures++;
    }
    uprintf("PARTINFOTEST DONE partitions=%u failures=%d\n", (unsigned)parts, failures);
    gBS->Stall(500000);
    gST->RuntimeServices->ResetSystem(EfiResetShutdown, failures ? EFI_ABORTED : EFI_SUCCESS, 0, NULL);
    return failures ? EFI_ABORTED : EFI_SUCCESS;
}
