/*
 * rwy_handover.c — Hand the payload to the started image: LoadOptions, INITRD, DTB.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_boot.h"

/* LINUX_EFI_INITRD_MEDIA_GUID: the Linux EFI stub loads the initrd through LoadFile2 on this path. */
#define INITRD_MEDIA_GUID { 0x5568e427, 0x68fc, 0x4f3d, { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68 } }
#define LOAD_FILE2_GUID   { 0x4006c0c1, 0xfcb3, 0x403e, { 0x99, 0x6d, 0x4a, 0x6c, 0x87, 0x24, 0xe0, 0x6d } }
#define DTB_TABLE_GUID    { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } }

static EFI_STATUS EFIAPI initrd_load(lf2_t *This, EFI_DEVICE_PATH *Path, BOOLEAN BootPolicy, UINTN *Size, VOID *Buf) {
    (void)Path;
    if (BootPolicy) return EFI_UNSUPPORTED;
    if (!Size) return EFI_INVALID_PARAMETER;
    if (!Buf || *Size < This->data.size) {
        *Size = This->data.size;
        return EFI_BUFFER_TOO_SMALL;
    }
    memcpy(Buf, This->data.buf, This->data.size);
    *Size = This->data.size;
    return EFI_SUCCESS;
}

void handover_init(handover_t *h) {
    memset(h, 0, sizeof(*h));
    h->initrd_lf2.load = initrd_load;
}

/* ARGS become the child's LoadOptions (UTF-16). */
static void set_load_options(EFI_LOADED_IMAGE *child_li, blob_t args) {
    UINTN len = args.size;
    CHAR16 *opt = NULL;

    while (len && (args.buf[len - 1] == '\n' || args.buf[len - 1] == '\r')) len--;
    if (!len || EFI_ERROR(gBS->AllocatePool(EfiLoaderData, (len + 1) * sizeof(CHAR16), (void **)&opt)))
        return;
    for (UINTN i = 0; i < len; i++) opt[i] = args.buf[i];
    opt[len] = 0;
    child_li->LoadOptions = opt;
    child_li->LoadOptionsSize = (UINT32)((len + 1) * sizeof(CHAR16));
}

/* INITRD goes through the LoadFile2 protocol that the Linux stub looks for. */
static EFI_STATUS publish_initrd(handover_t *h, blob_t initrd) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID initrd_guid = INITRD_MEDIA_GUID, lf2_guid = LOAD_FILE2_GUID;
    EFI_STATUS s;

    memset(&h->initrd_path, 0, sizeof(h->initrd_path));
    h->initrd_path.vendor.Header.Type = MEDIA_DEVICE_PATH;
    h->initrd_path.vendor.Header.SubType = MEDIA_VENDOR_DP;
    h->initrd_path.vendor.Header.Length[0] = sizeof(VENDOR_DEVICE_PATH);
    h->initrd_path.vendor.Guid = initrd_guid;
    h->initrd_path.end.Type = END_DEVICE_PATH_TYPE;
    h->initrd_path.end.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    h->initrd_path.end.Length[0] = sizeof(EFI_DEVICE_PATH);
    h->initrd_lf2.data = initrd;
    s = gBS->InstallMultipleProtocolInterfaces(&h->initrd_handle, &dp_guid, &h->initrd_path,
                                               &lf2_guid, &h->initrd_lf2, NULL);
    if (EFI_ERROR(s)) return ufail("RUNWAY: cannot publish INITRD", s);
    return EFI_SUCCESS;
}

/* Native handover: ARGS as LoadOptions, INITRD as LoadFile2, DTB as the DTB configuration table. */
EFI_STATUS publish_handover(handover_t *h, EFI_LOADED_IMAGE *child_li, const native_img_t *img) {
    EFI_GUID dtb_guid = DTB_TABLE_GUID;
    EFI_STATUS s;

    if (img->args.size) set_load_options(child_li, img->args);
    if (img->initrd.size) {
        s = publish_initrd(h, img->initrd);
        if (EFI_ERROR(s)) return s;
    }
    if (img->dtb.size) {
        s = gBS->InstallConfigurationTable(&dtb_guid, img->dtb.buf);
        if (EFI_ERROR(s)) return ufail("RUNWAY: cannot publish DTB", s);
        h->dtb = 1;
    }
    return EFI_SUCCESS;
}

EFI_STATUS prepare_native(handover_t *h, EFI_LOADED_IMAGE *child_li, const native_img_t *img,
                                 UINT64 boot_delay) {
    EFI_STATUS s = publish_handover(h, child_li, img);

    if (EFI_ERROR(s)) return s;
    if (boot_delay) {
        RWY_INFO("BOOT-DELAY %llus\n", (unsigned long long)boot_delay);
        flush_and_stall((UINTN)boot_delay * 1000000);
    }
    return EFI_SUCCESS;
}

static void withdraw_handover(handover_t *h) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID lf2_guid = LOAD_FILE2_GUID, dtb_guid = DTB_TABLE_GUID;

    if (h->initrd_handle)
        gBS->UninstallMultipleProtocolInterfaces(h->initrd_handle, &dp_guid, &h->initrd_path,
                                                 &lf2_guid, &h->initrd_lf2, NULL);
    if (h->dtb) gBS->InstallConfigurationTable(&dtb_guid, NULL);
}

/* Start the child, then power off so that the QEMU harness exits. */
EFI_STATUS run_child(EFI_HANDLE child, handover_t *h) {
    EFI_STATUS s;

    flush_and_stall(200000);
    s = gBS->StartImage(child, NULL, NULL);
    RWY_DEBUG("chain-loaded application exit: 0x%llx\n", (unsigned long long)s);
    withdraw_handover(h);

    if (!EFI_ERROR(s)) uprint("\nRESULT: PASS\n");
    else uprint("\nRESULT: FAIL\n");
    flush_and_stall(1000000);

    /* Automated harness: shut down so QEMU exits. Under OVMF this powers
     * off the VM (exit code 0 regardless; PASS/FAIL is read from serial). */
    gST->RuntimeServices->ResetSystem(EfiResetShutdown, s, 0, NULL);
    return s; /* not reached */
}
