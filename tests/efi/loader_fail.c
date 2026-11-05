/*
 * loader_fail.c — Test loader that exits with EFI_DEVICE_ERROR.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include "efi_util.h"

EFI_STATUS efi_main(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st) {
    util_init(ih, st);
    uprint("[TEST_LOADER] failing on purpose\n");
    return EFI_DEVICE_ERROR;
}
