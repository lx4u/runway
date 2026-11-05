/*
 * rwy_wdt.c — arm the hardware watchdog through the applications of EFI/RUNWAY/WDT.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_native.h"

/* Load options "arm <seconds>" as UTF-16. */
static UINTN wdt_options(CHAR16 *out, uint64_t seconds) {
    char digits[21];
    UINTN n = 0, d = 0;
    for (const char *p = "arm "; *p; p++) out[n++] = (CHAR16)*p;
    do digits[d++] = (char)('0' + seconds % 10); while ((seconds /= 10));
    while (d) out[n++] = (CHAR16)digits[--d];
    out[n++] = 0;
    return n * sizeof(CHAR16);
}

/* Load and start one application. EFI_SUCCESS means it armed the hardware. */
static EFI_STATUS wdt_try(rwy_store_t *st, const char *name, EFI_HANDLE self, EFI_DEVICE_PATH *path,
                          uint64_t seconds) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *li;
    EFI_HANDLE child = NULL;
    CHAR16 opts[32];
    blob_t b;
    EFI_STATUS s = native_read(st, "WDT", name, &b);

    if (!EFI_ERROR(s) && !b.size) s = EFI_LOAD_ERROR;
    if (!EFI_ERROR(s)) s = gBS->LoadImage(FALSE, self, path, b.buf, b.size, &child);
    if (!EFI_ERROR(s)) s = gBS->HandleProtocol(child, &li_guid, (void **)&li);
    if (!EFI_ERROR(s)) {
        li->LoadOptionsSize = (UINT32)wdt_options(opts, seconds);
        li->LoadOptions = opts;
        s = gBS->StartImage(child, NULL, NULL);
    }
    if (child) gBS->UnloadImage(child);
    if (b.buf) gBS->FreePool(b.buf);
    return s;
}

/* Try one application and log the outcome. Returns 1 when it armed the hardware. */
static int wdt_probe(rwy_store_t *st, const char *name, EFI_HANDLE self, EFI_DEVICE_PATH *path, uint64_t seconds) {
    EFI_STATUS s = wdt_try(st, name, self, path, seconds);
    if (!EFI_ERROR(s)) {
        RWY_INFO("WATCHDOG %llus armed by %s\n", (unsigned long long)seconds, name);
        return 1;
    }
    if (s != EFI_UNSUPPORTED)
        RWY_WARN("WATCHDOG %llus not armed (%s: 0x%llx), trying next\n", (unsigned long long)seconds, name,
                (unsigned long long)s);
    return 0;
}

/* Index of `name` in the list, or n. */
static size_t wdt_find(char names[][RWY_WDT_NAME_MAX + 1], size_t n, const char *name) {
    size_t i;
    for (i = 0; i < n && rwy_strcmp(names[i], name) != 0; i++) {}
    return i;
}

void wdt_arm(rwy_store_t *st, EFI_HANDLE self, EFI_HANDLE fat_fs_handle, uint64_t seconds) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH *path = NULL;
    char names[RWY_WDT_MAX][RWY_WDT_NAME_MAX + 1], hint[RWY_WDT_NAME_MAX + 1];
    const char *won = NULL;
    int total;
    size_t i, n, first;

    if (!seconds) return;
    total = rwy_bootcfg_wdt_list(st, names, RWY_WDT_MAX);
    if (total > RWY_WDT_MAX) RWY_WARN("WDT has %d applications, only %d probed\n", total, RWY_WDT_MAX);
    n = total < 0 ? 0 : total > RWY_WDT_MAX ? RWY_WDT_MAX : (size_t)total;
    (void)gBS->HandleProtocol(fat_fs_handle, &dp_guid, (void **)&path);

    /* The application that armed last time goes first; any other result runs the full loop. */
    (void)rwy_bootcfg_wdt_hint(st, hint);
    first = wdt_find(names, n, hint);
    if (first < n && wdt_probe(st, names[first], self, path, seconds)) won = names[first];
    for (i = 0; !won && i < n; i++)
        if (i != first && wdt_probe(st, names[i], self, path, seconds)) won = names[i];

    if (!won) RWY_WARN("WATCHDOG %llus not armed (no driver claimed the device)\n", (unsigned long long)seconds);
    if (rwy_bootcfg_wdt_set_hint(st, won)) RWY_WARN("WDT hint not recorded\n");
}

static UINTN wdt_stop_options(CHAR16 *out) {
    const char *p = "stop";
    UINTN n = 0;
    while (*p) out[n++] = (CHAR16)*p++;
    out[n++] = 0;
    return n * sizeof(CHAR16);
}

static EFI_STATUS wdt_try_stop(rwy_store_t *st, const char *name, EFI_HANDLE self, EFI_DEVICE_PATH *path) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *li;
    EFI_HANDLE child = NULL;
    CHAR16 opts[16];
    blob_t b;
    EFI_STATUS s = native_read(st, "WDT", name, &b);

    if (!EFI_ERROR(s) && !b.size) s = EFI_LOAD_ERROR;
    if (!EFI_ERROR(s)) s = gBS->LoadImage(FALSE, self, path, b.buf, b.size, &child);
    if (!EFI_ERROR(s)) s = gBS->HandleProtocol(child, &li_guid, (void **)&li);
    if (!EFI_ERROR(s)) {
        li->LoadOptionsSize = (UINT32)wdt_stop_options(opts);
        li->LoadOptions = opts;
        s = gBS->StartImage(child, NULL, NULL);
    }
    if (child) gBS->UnloadImage(child);
    if (b.buf) gBS->FreePool(b.buf);
    return s;
}

static int wdt_attempt_stop(rwy_store_t *st, const char *name, EFI_HANDLE self,
                            EFI_DEVICE_PATH *path, uint32_t timeout, uint64_t slot_wdt) {
    EFI_STATUS s = wdt_try_stop(st, name, self, path);

    if (!EFI_ERROR(s)) return 1;
    if (s == EFI_UNSUPPORTED) {
        RWY_INFO("WATCHDOG driver %s cannot stop; re-arming\n", name);
        if (!slot_wdt && timeout) (void)wdt_try(st, name, self, path, timeout);
        return 1;
    }
    return 0;
}

void wdt_stop(rwy_store_t *st, EFI_HANDLE self, EFI_HANDLE fat_fs_handle, const char *name,
              uint32_t timeout, uint64_t slot_wdt) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH *path = NULL;
    char names[RWY_WDT_MAX][RWY_WDT_NAME_MAX + 1];
    int total;
    size_t i, n, first;

    (void)gBS->HandleProtocol(fat_fs_handle, &dp_guid, (void **)&path);
    total = rwy_bootcfg_wdt_list(st, names, RWY_WDT_MAX);
    n = total < 0 ? 0 : total > RWY_WDT_MAX ? RWY_WDT_MAX : (size_t)total;

    first = (name && name[0]) ? wdt_find(names, n, name) : n;
    if (name && name[0]) {
        const char *armed = (first < n) ? names[first] : name;
        if (wdt_attempt_stop(st, armed, self, path, timeout, slot_wdt)) return;
    }

    for (i = 0; i < n; i++) {
        if (i != first && !EFI_ERROR(wdt_try_stop(st, names[i], self, path)))
            return;
    }
}
