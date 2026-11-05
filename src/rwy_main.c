/*
 * rwy_main.c — RUNWAY.EFI entry point and chain-loader (also installed as the default boot loader).
 * SPDX-License-Identifier: Apache-2.0
 *
 * Scans the volumes, mounts the groups, selects a native slot or falls back to the chain-loaded
 * consumer, repoints the started image at the Runway filesystem and starts it. The steps live in
 * rwy_scan.c, rwy_grpmount.c, rwy_slotboot.c and rwy_handover.c.
 */
#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_fs.h"
#include "rwy_native.h"
#include "rwy_boot.h"
#include "rwy_dbglog.h"
#include "rwy_tpm.h"
#include "../include/rwy_crypto.h"
#include "../include/rwy_recarea.h"
#include "../include/rwy_loader_fsm.h"
#include "rwy_netreport.h"

#if defined(__x86_64__) || defined(_M_X64)
#  define RWY_DEFAULT_CHAIN_PATH L"\\EFI\\BOOT\\BOOTX64.EFI"
#  define RWY_DEFAULT_CHAIN_NAME "EFI\\BOOT\\BOOTX64.EFI"
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define RWY_DEFAULT_CHAIN_PATH L"\\EFI\\BOOT\\BOOTAA64.EFI"
#  define RWY_DEFAULT_CHAIN_NAME "EFI\\BOOT\\BOOTAA64.EFI"
#elif defined(__arm__)
#  define RWY_DEFAULT_CHAIN_PATH L"\\EFI\\BOOT\\BOOTARM.EFI"
#  define RWY_DEFAULT_CHAIN_NAME "EFI\\BOOT\\BOOTARM.EFI"
#elif defined(__riscv) && (__riscv_xlen == 64)
#  define RWY_DEFAULT_CHAIN_PATH L"\\EFI\\BOOT\\BOOTRISCV64.EFI"
#  define RWY_DEFAULT_CHAIN_NAME "EFI\\BOOT\\BOOTRISCV64.EFI"
#elif defined(__i386__) || defined(_M_IX86)
#  define RWY_DEFAULT_CHAIN_PATH L"\\EFI\\BOOT\\BOOTIA32.EFI"
#  define RWY_DEFAULT_CHAIN_NAME "EFI\\BOOT\\BOOTIA32.EFI"
#else
#  define RWY_DEFAULT_CHAIN_PATH L"\\EFI\\BOOT\\BOOTX64.EFI"
#  define RWY_DEFAULT_CHAIN_NAME "EFI\\BOOT\\BOOTX64.EFI"
#endif

/* Open the root of the Runway volume. */
static EFI_STATUS open_volume(EFI_HANDLE rwy_handle, EFI_FILE_HANDLE *tv) {
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    EFI_STATUS s = gBS->HandleProtocol(rwy_handle, &sfs_guid, (void **)&sfs);

    if (EFI_ERROR(s)) return ufail("SFS missing on RUNWAY handle", s);
    s = sfs->OpenVolume(sfs, tv);
    if (EFI_ERROR(s)) return ufail("RUNWAY OpenVolume failed", s);
    return EFI_SUCCESS;
}

/* Load the consumer from RUNWAY.DAT first, then fall back to the test harness paths. */
static EFI_STATUS load_chain_target(EFI_FILE_HANDLE tv, EFI_FILE_HANDLE fat_root, UINT8 **buf, UINTN *size) {
    const CHAR16 *target_path = RWY_DEFAULT_CHAIN_PATH;
    const char *target_name = RWY_DEFAULT_CHAIN_NAME;
    EFI_STATUS s = fat_read_file(tv, target_path, buf, size);

    if (!EFI_ERROR(s)) {
        uprintf("chain-load target: %s (%llu bytes from RUNWAY.DAT)\n", target_name, (unsigned long long)*size);
        return s;
    }
    /* Fallback 1: \EFI\RUNWAY\CHAIN.EFI in store */
    target_path = L"\\EFI\\RUNWAY\\CHAIN.EFI";
    target_name = "EFI\\RUNWAY\\CHAIN.EFI";
    s = fat_read_file(tv, target_path, buf, size);
    if (!EFI_ERROR(s)) {
        uprintf("chain-load target: %s (%llu bytes from store)\n", target_name, (unsigned long long)*size);
        return s;
    }
    /* Fallback 2: backward-compatibility check in FAT ESP root */
    target_name = "EFI\\RUNWAY\\CHAIN.EFI (FAT ESP)";
    s = fat_root ? fat_read_file(fat_root, target_path, buf, size) : EFI_NOT_FOUND;
    if (EFI_ERROR(s)) return ufail("no bootable EFI application found in store (or ESP)", s);
    uprintf("chain-load target: %s (%llu bytes from FAT ESP)\n", target_name, (unsigned long long)*size);
    return s;
}

/* Load the consumer image that is not a native slot, repointing nothing yet. */
static EFI_STATUS load_chain_child(EFI_HANDLE ImageHandle, EFI_HANDLE rwy_handle, EFI_FILE_HANDLE tv,
                                   EFI_FILE_HANDLE fat_root, EFI_HANDLE *child, EFI_LOADED_IMAGE **child_li) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH *rwy_path = NULL;
    UINT8 *chain_buf = NULL;
    UINTN chain_size = 0;
    EFI_STATUS s = load_chain_target(tv, fat_root, &chain_buf, &chain_size);

    if (EFI_ERROR(s)) return s;
    s = gBS->HandleProtocol(rwy_handle, &dp_guid, (void **)&rwy_path);
    if (EFI_ERROR(s)) return ufail("no DevicePath on RUNWAY handle", s);
    s = gBS->LoadImage(FALSE, ImageHandle, rwy_path, chain_buf, chain_size, child);
    if (EFI_ERROR(s)) return ufail("LoadImage failed", s);
    s = gBS->HandleProtocol(*child, &li_guid, (void **)child_li);
    if (EFI_ERROR(s)) return ufail("no LoadedImage on child", s);
    return EFI_SUCCESS;
}

/* The boot store cannot be mounted or has no bootable config: start the recovery bundle of its area,
 * if it has one. Returns only when nothing could be started. */
static EFI_STATUS boot_recovery(EFI_HANDLE self, EFI_HANDLE fat_fs_handle, const vol_group_t *g, EFI_STATUS mount_err) {
    rwy_block_dev_t devs[RWY_MIRROR_MAX_DEVS];
    rwy_superblock_t sbs[RWY_MIRROR_MAX_DEVS];
    handover_t h;
    native_img_t img;
    blob_t part[RWY_RA_KINDS];
    EFI_STATUS s;

    handover_init(&h);
    memset(part, 0, sizeof(part));
    for (UINTN i = 0; i < g->count; i++) {
        devs[i] = g->members[i].backend.dev;
        sbs[i] = g->members[i].sb;
    }
    s = recboot_load(devs, sbs, g->count, mount_err, part);
    if (EFI_ERROR(mount_err)) rwy_netreport_send_unmounted(devs, sbs, g->count);
    else rwy_netreport_send(&g->inst->store);
    if (s == EFI_NOT_FOUND && EFI_ERROR(mount_err))
        RWY_ERROR("boot store cannot be mounted (0x%llx), not booting\n", (unsigned long long)mount_err);
    if (EFI_ERROR(s)) {
        flush_and_stall(3000000);
        return EFI_NOT_FOUND;
    }
    memset(&img, 0, sizeof(img));
    img.kernel = part[RWY_RA_KERNEL];
    img.initrd = part[RWY_RA_INITRD];
    img.dtb = part[RWY_RA_DTB];
    img.args = part[RWY_RA_ARGS];
    s = native_image_load(&img, self, fat_fs_handle);
    if (!EFI_ERROR(s)) s = publish_handover(&h, img.li, &img);
    if (EFI_ERROR(s)) {
        RWY_ERROR("recovery area invalid (image refused, 0x%llx), not booting\n", (unsigned long long)s);
        native_free(&img);
        flush_and_stall(3000000);
        return EFI_NOT_FOUND;
    }
    set_loader_vars(fat_fs_handle, NULL, NULL);
    return run_child(img.child, &h);
}

/* The root of the loading ESP: the one a file member kept open, else opened from the loaded image.
 * NULL when the ESP has no file system. */
static EFI_FILE_HANDLE esp_root(const vol_group_t *g, EFI_HANDLE self) {
    EFI_FILE_HANDLE root = NULL;

    for (UINTN i = 0; i < g->count; i++) {
        if (g->members[i].is_booted && g->members[i].root) return g->members[i].root;
    }
    if (EFI_ERROR(fat_open_our_root(self, &root))) return NULL;
    return root;
}

/* The calls the boot made to the store and to the firmware below it. */
static void log_io_summary(const rwy_fs_instance_t *inst) {
    rwy_dev_stats_t dev;
    char text[1024];

    rwy_fs_dev_stats(inst, &dev);
    rwy_io_format(text, sizeof(text), &inst->store.io, &dev);
    RWY_DEBUG("I/O of the boot:\n%s", text);
}

/* Commit confirmed loader state record and disarm stub watchdogs. */
static void loader_confirm(vol_group_t *boot_group, const rwy_loader_handshake_t *hs,
                           EFI_HANDLE ImageHandle, EFI_HANDLE rwy_handle, uint64_t slot_wdt) {
    for (UINTN i = 0; i < boot_group->count; i++) {
        mirror_member_t *m = &boot_group->members[i];
        rwy_loader_record_t rec;
        int active_idx = -1;

        if (!m->sb.loader_blocks) continue;
        if (rwy_loader_record_pick(&m->backend.dev, m->sb.loader_start, &rec, &active_idx) != RWY_OK)
            continue;
        rec.good = hs->copy;
        rec.trial = RWY_LOADER_TRIAL_NONE;
        rec.tries = 0;
        (void)rwy_loader_record_write(&m->backend.dev, m->sb.loader_start, active_idx, &rec, NULL);
    }
    gBS->SetWatchdogTimer(0, 0x0000, 0, NULL);
    if (hs->wdt_armed[0]) {
        wdt_stop(&boot_group->inst->store, ImageHandle, rwy_handle, hs->wdt_armed,
                 hs->timeout, slot_wdt);
    }
}

static void print_banner(void) {
    uprint_color(EFI_LIGHTCYAN,
                 "    ____                                 \n"
                 "   / __ \\__  ______ _      ______ ___  __\n"
                 "  / /_/ / / / / __ \\ | /| / / __ `/ / / /\n"
                 " / _, _/ /_/ / / / / |/ |/ / /_/ / /_/ / \n"
                 "/_/ |_|\\__,_/_/ /_/|__/|__/\\__,_/\\__, /  \n"
                 "                                /____/   \n");
    uprint_color(EFI_LIGHTGRAY, "Reliable boot. Every time.\n");
}

static EFI_STATUS discover_boot_group(EFI_HANDLE ImageHandle, EFI_HANDLE *fat_fs_handle,
                                      vol_group_t **out_bg) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *li;
    vol_group_t *bg;
    EFI_STATUS s;

    s = gBS->HandleProtocol(ImageHandle, &li_guid, (void **)&li);
    if (EFI_ERROR(s)) return ufail("no LoadedImage on self", s);
    *fat_fs_handle = li->DeviceHandle;
    set_boot_esp_var(*fat_fs_handle);

    scan_all_volumes(*fat_fs_handle);
    bg = find_boot_group();
    if (!bg) {
        RWY_ERROR("boot store cannot be mounted (not found or invalid), not booting\n");
        flush_and_stall(3000000);
        return EFI_NOT_FOUND;
    }
    RWY_DEBUG("scan: %u volume group(s) discovered\n", (unsigned int)gGroupCount);
    *out_bg = bg;
    return EFI_SUCCESS;
}

static EFI_STATUS mount_and_select(EFI_HANDLE ImageHandle, EFI_HANDLE fat_fs_handle,
                                   vol_group_t *bg, native_img_t *img,
                                   rwy_boot_choice_t *choice, int *unrecorded,
                                   EFI_HANDLE *rwy_handle) {
    EFI_STATUS s, mount_err = EFI_SUCCESS;

    s = mount_all_groups(fat_fs_handle, &mount_err);
    if (EFI_ERROR(s)) return boot_recovery(ImageHandle, fat_fs_handle, bg, mount_err);
    set_boot_members_var(bg);
    *rwy_handle = bg->inst->rwy_handle;

    s = native_select(&bg->inst->store, ImageHandle, *rwy_handle, img, choice, unrecorded);
    if (EFI_ERROR(s)) {
        if (s == EFI_NOT_FOUND) return boot_recovery(ImageHandle, fat_fs_handle, bg, EFI_SUCCESS);
        return s;
    }

    s = install_all_groups();
    if (EFI_ERROR(s)) return s;
    RWY_DEBUG("filesystem protocol: handle installed\n");
    return EFI_SUCCESS;
}

static EFI_STATUS run_selected_child(EFI_HANDLE ImageHandle, EFI_HANDLE fat_fs_handle,
                                     vol_group_t *bg, EFI_HANDLE rwy_handle,
                                     native_img_t *img, const rwy_boot_choice_t *choice,
                                     int unrecorded, const rwy_loader_handshake_t *hs,
                                     BOOLEAN have_handshake) {
    EFI_STATUS s;
    EFI_LOADED_IMAGE *child_li = NULL;
    EFI_FILE_HANDLE fat_root, tv = NULL;
    EFI_HANDLE child = NULL;
    handover_t h;
    BOOLEAN native = (img->child != NULL);

    handover_init(&h);
    if (!unrecorded) {
        s = open_volume(rwy_handle, &tv);
        if (EFI_ERROR(s)) return s;
    }
    if (native) {
        child = img->child;
        child_li = img->li;
    } else {
        fat_root = esp_root(bg, ImageHandle);
        s = load_chain_child(ImageHandle, rwy_handle, tv, fat_root, &child, &child_li);
        if (EFI_ERROR(s)) return s;
    }
    child_li->DeviceHandle = rwy_handle;

    if (have_handshake && hs->trial)
        loader_confirm(bg, hs, ImageHandle, rwy_handle, choice ? choice->watchdog : 0);

    /* Before the watchdog is armed below, so the report runs without one. */
    rwy_netreport_send(&bg->inst->store);

    if (native) {
        s = prepare_native(&h, child_li, img, choice->boot_delay);
        if (EFI_ERROR(s)) return s;
        wdt_arm(&bg->inst->store, ImageHandle, rwy_handle, choice->watchdog);
    }
    if (have_handshake) {
        EFI_GUID var_guid = RWY_BOOT_VAR_GUID;
        (void)gRT->SetVariable(RWY_LOADER_HANDSHAKE_VAR, &var_guid, 0, 0, NULL);
    }

    set_loader_vars(fat_fs_handle, choice, &bg->inst->store);
    log_io_summary(bg->inst);
    RWY_DEBUG("--- chain-loading ---\n");
    dbglog_flush();
    return run_child(child, &h);
}

static EFI_STATUS boot(EFI_HANDLE ImageHandle) {
    EFI_STATUS s;
    EFI_HANDLE fat_fs_handle = NULL, rwy_handle = NULL;
    vol_group_t *boot_group = NULL;
    native_img_t img;
    rwy_boot_choice_t choice;
    const rwy_boot_choice_t *choice_p = NULL;
    rwy_loader_handshake_t hs;
    EFI_GUID rwy_boot_guid = RWY_BOOT_VAR_GUID;
    UINTN hs_sz = sizeof(hs);
    BOOLEAN have_handshake = FALSE;
    int unrecorded = 0;

    s = gRT->GetVariable(RWY_LOADER_HANDSHAKE_VAR, &rwy_boot_guid, NULL, &hs_sz, &hs);
    if (!EFI_ERROR(s) && hs_sz == sizeof(hs)) have_handshake = TRUE;

    efi_register_tpm_random(rwy_tpm_random);
    rwy_crypto_set_random(efi_random);
    print_banner();
    RWY_DEBUG("crypto: AES %s\n", rwy_crypto_hw_aes() ? "on CPU instructions" : "portable");

    s = discover_boot_group(ImageHandle, &fat_fs_handle, &boot_group);
    if (EFI_ERROR(s)) return s;

    memset(&img, 0, sizeof(img));
    s = mount_and_select(ImageHandle, fat_fs_handle, boot_group, &img, &choice, &unrecorded, &rwy_handle);
    if (EFI_ERROR(s)) return s;

    if (img.child != NULL) {
        choice_p = &choice;
        set_boot_var(choice.slot, unrecorded);
        set_boot_label_var(choice.has_label ? choice.label : NULL);
    }

    return run_selected_child(ImageHandle, fat_fs_handle, boot_group, rwy_handle,
                              &img, choice_p, unrecorded, &hs, have_handshake);
}

/* NOTE: no EFIAPI here. Debian's crt0-efi-x86_64.o enters efi_main with
 * the SysV convention (RDI=ImageHandle, RSI=SystemTable); marking it
 * EFIAPI (ms_abi under HAVE_USE_MS_ABI) would swap the arguments. */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS s;

    util_init(ImageHandle, SystemTable);
    dbglog_start(ImageHandle);
    s = boot(ImageHandle);
    if (EFI_ERROR(s)) dbglog_pause();
    return s;
}
