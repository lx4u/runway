/*
 * rwy_loadervars.c — Boot Loader Interface variables for systemd tools.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include "rwy_boot.h"
#include "rwy_bls.h"

#define LOADER_VAR_GUID { 0x4a67b082, 0x0a4c, 0x41cf, { 0xb6, 0xc7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f } }
#define LOADER_INFO_VAL L"RUNWAY.EFI 0.1"

#define LOADER_FEATURE_BOOT_COUNTING (1ULL << 4)
#define LOADER_FEATURE_DEVICETREE    (1ULL << 10)

static void set_var_u16(const CHAR16 *name, const CHAR16 *val, UINTN chars) {
    EFI_GUID g = LOADER_VAR_GUID;
    if (val && chars) {
        (void)gST->RuntimeServices->SetVariable((CHAR16 *)name, &g,
            EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
            chars * sizeof(CHAR16), (VOID *)val);
    } else {
        (void)gST->RuntimeServices->SetVariable((CHAR16 *)name, &g, 0, 0, NULL);
    }
}

static void set_var_ascii(const CHAR16 *name, const char *val) {
    CHAR16 w[RWY_ENTRY_MAX + 32];
    UINTN i;
    if (!val || !val[0]) {
        set_var_u16(name, NULL, 0);
        return;
    }
    for (i = 0; val[i] && i < (sizeof(w) / sizeof(w[0])) - 1; i++) {
        w[i] = (CHAR16)(UINT8)val[i];
    }
    w[i++] = 0;
    set_var_u16(name, w, i);
}

typedef struct {
    CHAR16 w[4096];
    UINTN n;
} entry_list_t;

/* One id per entry, each ended by a NUL, as the Boot Loader Interface wants LoaderEntries. */
static void add_entry(void *ctx, const char *entry) {
    entry_list_t *l = ctx;
    UINTN len = rwy_strlen(entry) + 1;
    if (l->n + len > sizeof(l->w) / sizeof(l->w[0])) return;
    for (UINTN i = 0; i < len; i++) l->w[l->n++] = (CHAR16)(UINT8)entry[i];
}

static void set_entries(rwy_store_t *s) {
    static entry_list_t l;
    l.n = 0;
    if (s && rwy_bootcfg_entries(s, add_entry, &l) == RWY_OK) set_var_u16(L"LoaderEntries", l.w, l.n);
    else set_var_u16(L"LoaderEntries", NULL, 0);
}

static void set_features(void) {
    EFI_GUID g = LOADER_VAR_GUID;
    UINT64 f = LOADER_FEATURE_BOOT_COUNTING | LOADER_FEATURE_DEVICETREE;
    (void)gST->RuntimeServices->SetVariable(L"LoaderFeatures", &g,
        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, sizeof(f), &f);
}

void set_loader_vars(EFI_HANDLE esp, const rwy_boot_choice_t *choice, rwy_store_t *s) {
    CHAR16 uuid[37];
    UINTN n = part_uuid_text(esp, uuid);

    set_var_u16(L"LoaderInfo", LOADER_INFO_VAL, sizeof(LOADER_INFO_VAL) / sizeof(CHAR16));
    set_features();
    set_var_u16(L"LoaderDevicePartUUID", n ? uuid : NULL, n);
    set_entries(s);

    if (choice && choice->entry[0]) {
        set_var_ascii(L"LoaderEntrySelected", choice->entry);
        set_var_ascii(L"LoaderEntryDefault", choice->entry);
        if (choice->trial) {
            char path[RWY_ENTRY_MAX + 32];
            int prc = rwy_bls_name_boot_count_path(choice->entry, rwy_strlen(choice->entry),
                                                   (uint32_t)choice->tries_left, path, sizeof(path));
            if (prc > 0) {
                set_var_ascii(L"LoaderBootCountPath", path);
            } else {
                set_var_u16(L"LoaderBootCountPath", NULL, 0);
            }
        } else {
            set_var_u16(L"LoaderBootCountPath", NULL, 0);
        }
    } else {
        set_var_u16(L"LoaderEntrySelected", NULL, 0);
        set_var_u16(L"LoaderEntryDefault", NULL, 0);
        set_var_u16(L"LoaderBootCountPath", NULL, 0);
    }
}
