/*
 * Stub watchdog application for the RUNWAY.EFI probing test: logs that it started with its load
 * options, then returns the status of the build.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WDT_STUB_H
#define WDT_STUB_H

#include <efi.h>
#include <efilib.h>
#include "efi_util.h"

static inline EFI_STATUS wdt_stub(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st, const char *name, EFI_STATUS result) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *li;
    char opts[32];
    UINTN i = 0;

    util_init(ih, st);
    if (!EFI_ERROR(gBS->HandleProtocol(ih, &li_guid, (VOID **)&li)) && li->LoadOptions)
        for (CHAR16 *p = li->LoadOptions; *p && i < sizeof(opts) - 1; p++) opts[i++] = (char)*p;
    opts[i] = 0;
    uprintf("WSTUB %s started [%s]\n", name, opts);
    return result;
}

#endif
