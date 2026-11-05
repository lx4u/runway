/*
 * loader_hang.c — Test loader that busy loops until watchdog fires.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include "efi_util.h"

EFI_STATUS efi_main(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st) {
    util_init(ih, st);
    uprint("[TEST_LOADER] hanging on purpose\n");
    for (;;) gBS->Stall(1000000);
}
