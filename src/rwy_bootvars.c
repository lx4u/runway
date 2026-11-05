/*
 * rwy_bootvars.c — Variables that tell the OS how the machine was booted.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_boot.h"
#include "rwy_format.h"

/* Tell the OS which slot really started: a volatile variable, "<slot> recorded" or "<slot> unrecorded". */
void set_boot_var(const char *slot, int unrecorded) {
    EFI_GUID g = RWY_BOOT_VAR_GUID;
    const char *tail = unrecorded ? " unrecorded" : " recorded";
    char v[48];
    UINTN n = 0;
    for (; slot[n]; n++) v[n] = slot[n];
    for (UINTN k = 0; tail[k]; k++) v[n++] = tail[k];
    (void)gST->RuntimeServices->SetVariable(L"RunwayBoot", &g, EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, n, v);
}

/* Tell the OS the slot label: a volatile variable with raw UTF-8 bytes, or deleted when absent. */
void set_boot_label_var(const char *label) {
    EFI_GUID g = RWY_BOOT_VAR_GUID;
    UINTN n = 0;
    if (label) {
        for (; label[n]; n++);
        (void)gST->RuntimeServices->SetVariable(L"RunwayBootLabel", &g,
            EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, n, (void *)label);
    } else {
        (void)gST->RuntimeServices->SetVariable(L"RunwayBootLabel", &g, 0, 0, NULL);
    }
}

/* The GPT partition UUID of the partition a handle sits on, as upper-case UTF-16 text with a NUL.
 * Returns the number of CHAR16 written (37), or 0 when the handle has no GPT partition. */
UINTN part_uuid_text(EFI_HANDLE h, CHAR16 v[37]) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID, part;
    EFI_DEVICE_PATH *dp = NULL;
    static const char hex[] = "0123456789ABCDEF";
    UINTN n = 0;
    if (EFI_ERROR(gBS->HandleProtocol(h, &dp_guid, (void **)&dp)) || !dp) return 0;
    for (; !IsDevicePathEnd(dp); dp = NextDevicePathNode(dp)) {
        HARDDRIVE_DEVICE_PATH *hd = (HARDDRIVE_DEVICE_PATH *)dp;
        if (DevicePathType(dp) != MEDIA_DEVICE_PATH || DevicePathSubType(dp) != MEDIA_HARDDRIVE_DP || hd->SignatureType != 2)
            continue;
        memcpy(&part, hd->Signature, sizeof(part));
        for (int i = 0; i < 16; i++) {
            /* The first three GUID fields are little endian, the last eight bytes are in order */
            int k = i < 4 ? 3 - i : i < 6 ? 9 - i : i < 8 ? 13 - i : i;
            if (i == 4 || i == 6 || i == 8 || i == 10) v[n++] = L'-';
            v[n++] = hex[((UINT8 *)&part)[k] >> 4];
            v[n++] = hex[((UINT8 *)&part)[k] & 15];
        }
        v[n++] = 0;
        return n;
    }
    return 0;
}

/* Tell the OS which ESP we were loaded from: the GPT partition UUID in a volatile variable, as UTF-16.
 * Nothing is set without one. A failure is ignored: the variable is only a hint. */
void set_boot_esp_var(EFI_HANDLE esp) {
    EFI_GUID g = RWY_BOOT_VAR_GUID;
    CHAR16 v[37];
    UINTN n = part_uuid_text(esp, v);
    if (n) (void)gST->RuntimeServices->SetVariable(L"RunwayBootESP", &g, EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, n * sizeof(CHAR16), v);
}

/* Tell the OS which store members are in use: their partition UUIDs, booted one first, separated by
 * spaces, in a volatile variable. Members without a GPT UUID are left out. A failure is ignored. */
void set_boot_members_var(const vol_group_t *g) {
    EFI_GUID guid = RWY_BOOT_VAR_GUID;
    CHAR16 v[RWY_MIRROR_MAX_DEVS * 37], u[37];
    UINTN n = 0;
    for (UINTN i = 0; i < g->count; i++) {
        UINTN k = g->members[i].degraded ? 0 : part_uuid_text(g->members[i].handle, u);
        if (!k) continue;
        if (n) v[n - 1] = L' ';
        memcpy(&v[n], u, k * sizeof(CHAR16));
        n += k;
    }
    if (n) (void)gST->RuntimeServices->SetVariable(L"RunwayBootMembers", &guid, EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, n * sizeof(CHAR16), v);
}
