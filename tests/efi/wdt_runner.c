/*
 * Test runner: loads \WDT.EFI like RUNWAY.EFI would (LoadImage from a buffer,
 * load options "arm 5" or "stop"), starts it, and either hangs so the watchdog
 * fires, or verifies that stopping prevented reset.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include "efi_util.h"

static EFI_STATUS run_driver(EFI_HANDLE ih, const CHAR16 *opts, UINTN opts_sz) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *li;
    EFI_FILE_HANDLE root;
    EFI_HANDLE child;
    UINT8 *buf;
    UINTN size;
    EFI_STATUS s = fat_open_our_root(ih, &root);

    if (!EFI_ERROR(s)) s = fat_read_file(root, L"\\WDT.EFI", &buf, &size);
    if (!EFI_ERROR(s)) s = gBS->LoadImage(FALSE, ih, NULL, buf, size, &child);
    if (!EFI_ERROR(s)) s = gBS->HandleProtocol(child, &li_guid, (VOID **)&li);
    if (EFI_ERROR(s)) return s;

    li->LoadOptions = (VOID *)opts;
    li->LoadOptionsSize = (UINT32)opts_sz;
    s = gBS->StartImage(child, NULL, NULL);
    gBS->UnloadImage(child);
    return s;
}

EFI_STATUS efi_main(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st) {
    static CHAR16 arm_opts[] = L"arm 5";
    static CHAR16 stop_opts[] = L"stop";
    EFI_FILE_HANDLE root;
    UINT8 *mode = NULL;
    UINTN mode_sz = 0;
    EFI_STATUS s;
    int is_stop_test = 0;
    int is_stop_only = 0;

    util_init(ih, st);
    uprint("WDT RUNNER: start\n");
    if (!EFI_ERROR(fat_open_our_root(ih, &root)))
        (void)fat_read_file(root, L"\\MODE.TXT", &mode, &mode_sz);

    if (mode && mode_sz >= 8 && mode[0] == 's' && mode[1] == 't' &&
        mode[2] == 'o' && mode[3] == 'p' && mode[4] == '_' && mode[5] == 'a') {
        is_stop_test = 1;
    } else if (mode && mode_sz >= 4 && mode[0] == 's' && mode[1] == 't' &&
               mode[2] == 'o' && mode[3] == 'p') {
        is_stop_only = 1;
    }

    if (is_stop_test) {
        s = run_driver(ih, arm_opts, sizeof(arm_opts));
        uprintf("WDT RUNNER: arm status 0x%lx\n", (unsigned long)s);
        if (EFI_ERROR(s)) return ufail("WDT RUNNER: arm failed\n", s);
        s = run_driver(ih, stop_opts, sizeof(stop_opts));
        uprintf("WDT RUNNER: stop status 0x%lx\n", (unsigned long)s);
        if (EFI_ERROR(s)) return ufail("WDT RUNNER: stop failed\n", s);
        for (UINTN i = 0; i < 8; i++) gBS->Stall(1000000);
        uprint("WDT RUNNER: stop verified (no reset)\n");
        gRT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
        return EFI_SUCCESS;
    }

    if (is_stop_only) {
        s = run_driver(ih, stop_opts, sizeof(stop_opts));
        uprintf("WDT RUNNER: driver status 0x%lx\n", (unsigned long)s);
        if (EFI_ERROR(s)) uprint("WDT RUNNER: stop unsupported\n");
        else uprint("WDT RUNNER: stopped\n");
        gRT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
        return s;
    }

    s = run_driver(ih, arm_opts, sizeof(arm_opts));
    uprintf("WDT RUNNER: driver status 0x%lx\n", (unsigned long)s);
    if (EFI_ERROR(s)) uprint("WDT RUNNER: not armed\n");
    for (;;) gBS->Stall(1000000);
}
