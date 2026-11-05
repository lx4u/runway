/*
 * tests/reinstall_child.c — Child payload application for Phase 0.
 * SPDX-License-Identifier: Apache-2.0
 *
 * This payload is loaded by firmware's gBS->LoadImage() using a file device path
 * (L"\\INTERCEPTED.EFI") that exists solely inside the reinstalled protocol
 * interface (test_sfs) and NOT on the underlying physical FAT filesystem.
 *
 * Upon execution, it verifies that LoadedImage->DeviceHandle points to the
 * active boot partition handle and reports success.
 */
#include <efi.h>
#include <efilib.h>
#include "efi_util.h"

static EFI_STATUS EFIAPI child_payload(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS s;
    EFI_LOADED_IMAGE *li = NULL;
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;

    util_init(ImageHandle, SystemTable);

    uprint("\n  ==============================================================\n");
    uprint("  [CHILD-PAYLOAD] Intercepted payload running via StartImage!\n");

    s = gBS->HandleProtocol(ImageHandle, &li_guid, (void **)&li);
    if (!EFI_ERROR(s) && li) {
        uprintf("  [CHILD-PAYLOAD] LoadedImage.DeviceHandle = %p\n", li->DeviceHandle);
        uprintf("  [CHILD-PAYLOAD] LoadedImage.FilePath     = %p\n", li->FilePath);
        uprint("  [CHILD-PAYLOAD] Image loaded directly from reinstalled protocol!\n");
    } else {
        uprintf("  [CHILD-PAYLOAD] Failed to obtain LoadedImage: 0x%llx\n", (unsigned long long)s);
        return s;
    }

    uprint("  [CHILD-PAYLOAD] Returning EFI_SUCCESS to caller.\n");
    uprint("  ==============================================================\n\n");

    return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    return child_payload(ImageHandle, SystemTable);
}
