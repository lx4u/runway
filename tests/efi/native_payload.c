/*
 * tests/native_payload.c — fake UKI for the ADR 0002 native-boot QEMU test.
 * SPDX-License-Identifier: Apache-2.0
 * Behaviour is chosen by LoadOptions (the slot's ARGS):
 *   confirm  report what Runway handed over, write OK to EFI/RUNWAY/BOOTED/STATE, power off
 *   hang     report and never return (the host resets the VM, a watchdog stand-in)
 *   (empty)  report and power off (also used as the "next BootOrder entry" marker)
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"

static int same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

typedef struct lf2 lf2_t;
struct lf2 { EFI_STATUS (EFIAPI *load)(lf2_t *, EFI_DEVICE_PATH *, BOOLEAN, UINTN *, VOID *); };

typedef struct { VENDOR_DEVICE_PATH vendor; EFI_DEVICE_PATH end; } vpath_t;

static EFI_STATUS done(EFI_STATUS s) {
    gBS->Stall(500000);
    gST->RuntimeServices->ResetSystem(EfiResetShutdown, s, 0, NULL);
    return s;
}

static void report_initrd(void) {
    EFI_GUID lf2_guid = { 0x4006c0c1, 0xfcb3, 0x403e, { 0x99, 0x6d, 0x4a, 0x6c, 0x87, 0x24, 0xe0, 0x6d } };
    vpath_t p;
    EFI_DEVICE_PATH *dp = &p.vendor.Header;
    EFI_HANDLE h;
    lf2_t *lf;
    UINTN size = 0;
    char *buf = NULL;
    memset(&p, 0, sizeof(p));
    p.vendor.Header.Type = MEDIA_DEVICE_PATH;
    p.vendor.Header.SubType = MEDIA_VENDOR_DP;
    p.vendor.Header.Length[0] = sizeof(VENDOR_DEVICE_PATH);
    p.vendor.Guid = (EFI_GUID){ 0x5568e427, 0x68fc, 0x4f3d, { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68 } };
    p.end.Type = END_DEVICE_PATH_TYPE;
    p.end.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    p.end.Length[0] = sizeof(EFI_DEVICE_PATH);
    if (EFI_ERROR(gBS->LocateDevicePath(&lf2_guid, &dp, &h)) ||
        EFI_ERROR(gBS->HandleProtocol(h, &lf2_guid, (void **)&lf))) {
        uprint("[NATIVE] initrd: none\n");
        return;
    }
    if (lf->load(lf, dp, FALSE, &size, NULL) != EFI_BUFFER_TOO_SMALL ||
        EFI_ERROR(gBS->AllocatePool(EfiLoaderData, size + 1, (void **)&buf)) ||
        EFI_ERROR(lf->load(lf, dp, FALSE, &size, buf))) {
        uprint("[NATIVE] initrd: LoadFile2 failed\n");
        return;
    }
    buf[size] = 0;
    uprintf("[NATIVE] initrd: %u bytes: %s\n", (unsigned)size, buf);
}

static void report_dtb(void) {
    EFI_GUID dtb = { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };
    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++)
        if (guid_eq(&gST->ConfigurationTable[i].VendorGuid, &dtb)) {
            uprintf("[NATIVE] dtb: %s\n", (char *)gST->ConfigurationTable[i].VendorTable);
            return;
        }
    uprint("[NATIVE] dtb: none\n");
}

/* The volatile variable RUNWAY.EFI sets before it starts a slot (ADR 0002 N2). */
static void report_boot_var(void) {
    EFI_GUID g = { 0xb371a076, 0xb7ef, 0x4e6e, { 0xb9, 0x8a, 0x8d, 0x0d, 0x2c, 0x49, 0x3c, 0x7b } };
    char v[64];
    UINTN n = sizeof(v) - 1;
    UINT32 attr = 0;
    if (EFI_ERROR(gST->RuntimeServices->GetVariable(L"RunwayBoot", &g, &attr, &n, v))) {
        uprint("[NATIVE] RunwayBoot: none\n");
        return;
    }
    v[n] = 0;
    uprintf("[NATIVE] RunwayBoot: %s (attr 0x%x)\n", v, (unsigned)attr);
}

/* The volatile variable RUNWAY.EFI sets for the slot label. */
static void report_boot_label_var(void) {
    EFI_GUID g = { 0xb371a076, 0xb7ef, 0x4e6e, { 0xb9, 0x8a, 0x8d, 0x0d, 0x2c, 0x49, 0x3c, 0x7b } };
    char v[140];
    UINTN n = sizeof(v) - 1;
    UINT32 attr = 0;
    if (EFI_ERROR(gST->RuntimeServices->GetVariable(L"RunwayBootLabel", &g, &attr, &n, v))) {
        uprint("[NATIVE] RunwayBootLabel: none\n");
        return;
    }
    v[n] = 0;
    uprintf("[NATIVE] RunwayBootLabel: %s (attr 0x%x)\n", v, (unsigned)attr);
}

/* A volatile variable of RUNWAY.EFI that holds partition UUIDs as UTF-16 text (RunwayBootESP, RunwayBootMembers). */
static void report_uuid_var(CHAR16 *name, const char *label) {
    EFI_GUID g = { 0xb371a076, 0xb7ef, 0x4e6e, { 0xb9, 0x8a, 0x8d, 0x0d, 0x2c, 0x49, 0x3c, 0x7b } };
    CHAR16 w[320];
    char v[320];
    UINTN n = sizeof(w) - 2, i;
    UINT32 attr = 0;
    if (EFI_ERROR(gST->RuntimeServices->GetVariable(name, &g, &attr, &n, w))) {
        uprintf("[NATIVE] %s: none\n", label);
        return;
    }
    for (i = 0; i < n / 2; i++) v[i] = (char)w[i];
    v[i] = 0;
    uprintf("[NATIVE] %s: %s (attr 0x%x)\n", label, v, (unsigned)attr);
}

/* A volatile variable of the Boot Loader Interface (GUID 4a67b082-0a4c-41cf-b6c7-440b29bb8c4f). */
static void report_loader_var(const CHAR16 *name, const char *label) {
    EFI_GUID g = { 0x4a67b082, 0x0a4c, 0x41cf, { 0xb6, 0xc7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f } };
    CHAR16 w[320];
    char v[320];
    UINTN n = sizeof(w) - 2, i;
    UINT32 attr = 0;
    if (EFI_ERROR(gST->RuntimeServices->GetVariable((CHAR16 *)name, &g, &attr, &n, w))) {
        uprintf("[NATIVE] %s: none\n", label);
        return;
    }
    /* A list shows as comma separated names; the NUL that ends the last one is dropped. */
    for (i = 0; i < n / 2; i++) v[i] = w[i] ? (char)w[i] : ',';
    if (i && !w[i - 1]) i--;
    v[i] = 0;
    uprintf("[NATIVE] %s: %s (attr 0x%x)\n", label, v, (unsigned)attr);
}

static void report_loader_features(void) {
    EFI_GUID g = { 0x4a67b082, 0x0a4c, 0x41cf, { 0xb6, 0xc7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f } };
    UINT64 f = 0;
    UINTN n = sizeof(f);
    if (EFI_ERROR(gST->RuntimeServices->GetVariable(L"LoaderFeatures", &g, NULL, &n, &f)))
        uprint("[NATIVE] LoaderFeatures: none\n");
    else
        uprintf("[NATIVE] LoaderFeatures: 0x%x\n", (unsigned)f);
}

/* Read a small file through the Runway volume (BOOTED is a symlink, as seen by the EFI adapter). */
static void slurp(EFI_FILE_HANDLE root, const CHAR16 *path, char *out, UINTN cap) {
    EFI_FILE_HANDLE f;
    UINTN n = cap - 1;
    out[0] = 0;
    if (EFI_ERROR(root->Open(root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ, 0))) return;
    if (!EFI_ERROR(f->Read(f, &n, out))) out[n] = 0; else out[0] = 0;
    f->Close(f);
}

/* Whether directory `dir` lists `name`. */
static int listed(EFI_FILE_HANDLE root, const CHAR16 *dir, const CHAR16 *name) {
    EFI_FILE_HANDLE d;
    UINT8 buf[512];
    int found = 0;
    if (EFI_ERROR(root->Open(root, &d, (CHAR16 *)dir, EFI_FILE_MODE_READ, 0))) return 0;
    for (;;) {
        UINTN n = sizeof(buf);
        if (EFI_ERROR(d->Read(d, &n, buf)) || !n) break;
        if (!StrCmp(((EFI_FILE_INFO *)buf)->FileName, (CHAR16 *)name)) found = 1;
    }
    d->Close(d);
    return found;
}

/* The label of every file system the payload sees. With a store partition the firmware FAT of the ESP stays beside
 * the Runway volume; "own" marks the volume of this image's device handle. */
static void report_volumes(EFI_HANDLE own) {
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, fsi_id = EFI_FILE_SYSTEM_INFO_ID;
    EFI_HANDLE *h = NULL;
    UINTN n = 0;

    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &sfs_guid, NULL, &n, &h))) return;
    for (UINTN i = 0; i < n; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
        EFI_FILE_HANDLE root;
        UINT8 buf[256];
        UINTN sz = sizeof(buf), k;
        char label[32];

        if (EFI_ERROR(gBS->HandleProtocol(h[i], &sfs_guid, (void **)&sfs)) || EFI_ERROR(sfs->OpenVolume(sfs, &root)))
            continue;
        if (!EFI_ERROR(root->GetInfo(root, &fsi_id, &sz, buf))) {
            CHAR16 *l = ((EFI_FILE_SYSTEM_INFO *)buf)->VolumeLabel;
            for (k = 0; k < sizeof(label) - 1 && l[k]; k++) label[k] = (char)l[k];
            label[k] = 0;
            uprintf("[NATIVE] volume: %s%s\n", label, h[i] == own ? " (own)" : "");
        }
        root->Close(root);
    }
    gBS->FreePool(h);
}

/* Create RECOVER_NEXT, see it, delete it (ADR 0002 V1). */
static void recover_next_flag(EFI_FILE_HANDLE root) {
    const CHAR16 *path = L"\\EFI\\RUNWAY\\RECOVER_NEXT";
    EFI_FILE_HANDLE f;
    EFI_STATUS c, d = EFI_NOT_FOUND;
    int seen, gone;
    c = root->Open(root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (!EFI_ERROR(c)) f->Close(f);
    seen = listed(root, L"\\EFI\\RUNWAY", L"RECOVER_NEXT");
    if (!EFI_ERROR(root->Open(root, &f, (CHAR16 *)path, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) d = f->Delete(f);
    gone = !listed(root, L"\\EFI\\RUNWAY", L"RECOVER_NEXT");
    uprintf("[NATIVE] RECOVER_NEXT create=0x%llx visible=%s delete=0x%llx gone=%s\n", (unsigned long long)c,
            seen ? "yes" : "NO", (unsigned long long)d, gone ? "yes" : "NO");
}

EFI_STATUS efi_main(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID, sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *li;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
    EFI_FILE_HANDLE root, f;
    char args[64] = "", reason[32], state[32], index[32];
    EFI_STATUS s;

    util_init(ih, st);
    if (EFI_ERROR(gBS->HandleProtocol(ih, &li_guid, (void **)&li))) return done(EFI_ABORTED);
    if (li->LoadOptions && li->LoadOptionsSize >= sizeof(CHAR16)) {
        CHAR16 *o = li->LoadOptions;
        UINTN i;
        for (i = 0; i < sizeof(args) - 1 && o[i]; i++) args[i] = (char)o[i];
        args[i] = 0;
    }
    uprintf("[NATIVE] started args=[%s]\n", args);
    report_initrd();
    report_dtb();
    report_boot_var();
    report_boot_label_var();
    report_uuid_var(L"RunwayBootESP", "RunwayBootESP");
    report_uuid_var(L"RunwayBootMembers", "RunwayBootMembers");
    report_loader_var(L"LoaderInfo", "LoaderInfo");
    report_loader_features();
    report_loader_var(L"LoaderDevicePartUUID", "LoaderDevicePartUUID");
    report_loader_var(L"LoaderEntrySelected", "LoaderEntrySelected");
    report_loader_var(L"LoaderEntryDefault", "LoaderEntryDefault");
    report_loader_var(L"LoaderEntries", "LoaderEntries");
    report_loader_var(L"LoaderBootCountPath", "LoaderBootCountPath");
    if (same(args, "hang")) {
        uprint("[NATIVE] hanging\n");
        for (;;) gBS->Stall(1000000);
    }
    if (same(args, "reset")) {
        uprint("[NATIVE] resetting\n");
        gBS->Stall(200000);
        gST->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    }
    if (!same(args, "confirm")) return done(EFI_SUCCESS);

    if (EFI_ERROR(gBS->HandleProtocol(li->DeviceHandle, &sfs_guid, (void **)&sfs)) ||
        EFI_ERROR(sfs->OpenVolume(sfs, &root))) {
        uprint("[NATIVE] FAIL: no volume\n");
        return done(EFI_ABORTED);
    }
    report_volumes(li->DeviceHandle);
    slurp(root, L"\\EFI\\RUNWAY\\BOOTED\\STATE", state, sizeof(state));
    slurp(root, L"\\EFI\\RUNWAY\\REASON", reason, sizeof(reason));
    uprintf("[NATIVE] BOOTED/STATE=%s REASON=%s\n", state[0] ? state : "?", reason[0] ? reason : "?");
    /* virtual INDEX (ADR 0002 §2.8, T8): readable through BOOTED, every write refused */
    slurp(root, L"\\EFI\\RUNWAY\\BOOTED\\INDEX", index, sizeof(index));
    for (UINTN i = 0; index[i]; i++) if (index[i] == '\n') index[i] = 0;
    uprintf("[NATIVE] BOOTED/INDEX=%s\n", index[0] ? index : "?");
    /* every boot: a write-mode Close with nothing staged must not commit (E1) */
    s = root->Open(root, &f, L"\\EFI\\RUNWAY\\BOOTED\\INDEX", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (!EFI_ERROR(s)) {
        UINTN n = 2;
        s = f->Write(f, &n, "9\n");
        f->Close(f);
    }
    uprintf("[NATIVE] write INDEX refused: %s\n", EFI_ERROR(s) ? "yes" : "NO");
    uprintf("[NATIVE] CONFIG2 lists STATE=%s TRIES=%s INDEX=%s\n",
            listed(root, L"\\EFI\\RUNWAY\\CONFIG2", L"STATE") ? "yes" : "NO",
            listed(root, L"\\EFI\\RUNWAY\\CONFIG2", L"TRIES") ? "yes" : "NO",
            listed(root, L"\\EFI\\RUNWAY\\CONFIG2", L"INDEX") ? "yes" : "NO");
    uprintf("[NATIVE] BOOTED lists STATE=%s TRIES=%s INDEX=%s\n",
            listed(root, L"\\EFI\\RUNWAY\\BOOTED", L"STATE") ? "yes" : "NO",
            listed(root, L"\\EFI\\RUNWAY\\BOOTED", L"TRIES") ? "yes" : "NO",
            listed(root, L"\\EFI\\RUNWAY\\BOOTED", L"INDEX") ? "yes" : "NO");
    recover_next_flag(root);
    if (state[0] == 'O' && state[1] == 'K') {
        uprint("[NATIVE] already confirmed\n");
        root->Close(root);
        return done(EFI_SUCCESS);
    }
    s = root->Open(root, &f, L"\\EFI\\RUNWAY\\BOOTED\\STATE", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (!EFI_ERROR(s)) {
        UINTN n = 3;
        s = f->Write(f, &n, "OK\n");
        f->Close(f);
    }
    uprintf("[NATIVE] confirm via BOOTED: 0x%llx\n", (unsigned long long)s);
    root->Close(root);
    return done(EFI_SUCCESS);
}
