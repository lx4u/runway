/*
 * rwy_dbglog.c — \RUNWAY.LOG on the ESP for debug builds (see rwy_dbglog.h).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include "efi_util.h"
#include "rwy_dbglog.h"

#if defined(RWY_DEBUG_LOG)

#define DBGLOG_MAX (64u * 1024u)
#define DBGLOG_PAUSE_100NS (600000000ULL)

/* The ESP root, opened before Runway puts its own file protocol on the handle. */
static EFI_FILE_HANDLE gRoot;

/* Write text with CR LF line ends, so that any editor on any OS shows it well. */
static EFI_STATUS write_text(EFI_FILE_HANDLE fh, const char *text, UINTN len) {
    UINTN start = 0;

    for (UINTN i = 0; i <= len; i++) {
        UINTN n = i - start;
        EFI_STATUS s;

        if (i < len && text[i] != '\n') continue;
        s = fh->Write(fh, &n, (void *)(text + start));
        if (EFI_ERROR(s)) return s;
        if (i < len) {
            n = 2;
            s = fh->Write(fh, &n, "\r\n");
            if (EFI_ERROR(s)) return s;
        }
        start = i + 1;
    }
    return EFI_SUCCESS;
}

void dbglog_flush(void) {
    const char *text = NULL;
    UINTN len = uprint_captured(&text);
    EFI_FILE_HANDLE root = gRoot, fh;
    EFI_STATUS s;

    if (!len || !root) return;
    /* A shorter log must not leave the tail of an older one: delete the file first. */
    if (!EFI_ERROR(root->Open(root, &fh, L"\\RUNWAY.LOG", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)))
        fh->Delete(fh);
    s = root->Open(root, &fh, L"\\RUNWAY.LOG",
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (!EFI_ERROR(s)) {
        write_text(fh, text, len);
        fh->Flush(fh);
        fh->Close(fh);
    }
}

void dbglog_pause(void) {
    EFI_EVENT ev[2];
    EFI_INPUT_KEY key;
    UINTN idx;
    static BOOLEAN diagnosed;

    if (!diagnosed) {
        diagnosed = TRUE;
        diag_firmware();
    }
    dbglog_flush();
    if (!gST->ConIn || EFI_ERROR(gBS->CreateEvent(EVT_TIMER, 0, NULL, NULL, &ev[1]))) return;
    uprint("\nLog saved to RUNWAY.LOG on the ESP. Press any key to continue.\n");
    ev[0] = gST->ConIn->WaitForKey;
    gST->ConIn->Reset(gST->ConIn, FALSE);
    gBS->SetTimer(ev[1], TimerRelative, DBGLOG_PAUSE_100NS);
    if (!EFI_ERROR(gBS->WaitForEvent(2, ev, &idx)) && idx == 0) gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
    gBS->CloseEvent(ev[1]);
}

void dbglog_start(EFI_HANDLE image) {
    char *buf = NULL;

    if (EFI_ERROR(fat_open_our_root(image, &gRoot))) gRoot = NULL;
    if (!EFI_ERROR(gBS->AllocatePool(EfiLoaderData, DBGLOG_MAX, (void **)&buf))) uprint_capture(buf, DBGLOG_MAX);
    ufail_hook(dbglog_pause);
}

#endif
