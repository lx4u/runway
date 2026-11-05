/*
 * loader_reset.c — Test loader that resets the system to simulate a reset loop.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include "efi_util.h"

EFI_STATUS efi_main(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st) {
    util_init(ih, st);
    uprint("[TEST_LOADER] resetting on purpose\n");
    gRT->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
    return EFI_SUCCESS;
}
