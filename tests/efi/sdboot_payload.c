/*
 * UKI stand-in for the systemd-boot QEMU test: reports that systemd-boot started it, prints its
 * load options (the UKI command line) and powers off.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include "efi_util.h"

EFI_STATUS efi_main(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *li;
    char opts[96];
    UINTN i = 0;

    util_init(ih, st);
    if (!EFI_ERROR(gBS->HandleProtocol(ih, &li_guid, (VOID **)&li)) && li->LoadOptions)
        for (CHAR16 *p = li->LoadOptions; *p && i < sizeof(opts) - 1; p++) opts[i++] = (char)*p;
    opts[i] = 0;
    uprintf("[SDBOOT-PAYLOAD] started [%s]\n", opts);
    gBS->Stall(500000);
    gST->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    return EFI_SUCCESS;
}
