/* Stub watchdog application that returns EFI_DEVICE_ERROR. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "wdt_stub.h"

EFI_STATUS efi_main(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st) {
    return wdt_stub(ih, st, "fail", EFI_DEVICE_ERROR);
}
