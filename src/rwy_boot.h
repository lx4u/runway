/*
 * rwy_boot.h — volume groups, mounting and boot steps of RUNWAY.EFI. Internal.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_BOOT_H
#define RWY_BOOT_H

#include <efi.h>
#include "efi_util.h"
#include "rwy_fs.h"
#include "rwy_native.h"
#include "../include/rwy_format.h"

static inline int is_zero_uuid(const UINT8 u[16]) {
    UINT8 acc = 0;
    for (UINTN i = 0; i < 16; i++) acc |= u[i];
    return acc == 0;
}

static inline int uuid_eq(const uint8_t a[16], const uint8_t b[16]) {
    int i;
    for (i = 0; i < 16; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

typedef struct {
    EFI_HANDLE handle;
    EFI_FILE_HANDLE root;    /* file backend: volume root of the store file, kept open; NULL for a partition */
    rwy_backend_t backend;
    rwy_superblock_t sb;
    int slot;
    UINT64 gen;
    BOOLEAN is_booted;
    BOOLEAN degraded; /* could not be caught up: left out of the mount */
    UINT32 events;    /* mirror health events seen at mount: a bit (1 << RWY_SS_EV_*) each */
} mirror_member_t;

#define RWY_MAX_VOLUMES 8

/* One logical volume: all RUNWAY.DAT replicas sharing the same UUID. */
typedef struct {
    UINT8 uuid[16];
    UINTN count;
    BOOLEAN has_boot;
    mirror_member_t members[RWY_MIRROR_MAX_DEVS];
    rwy_fs_instance_t *inst;
} vol_group_t;

extern vol_group_t gGroups[RWY_MAX_VOLUMES];
extern UINTN gGroupCount;

/* A mount-time failure: reported like ufail, but the machine is not powered off. The caller decides
 * (boot group: leave to the firmware, any other group: skip it). */
static inline EFI_STATUS mfail(const char *msg, EFI_STATUS st) {
    uprintf("FAIL: %s (status 0x%llx)\n", msg, (unsigned long long)st);
    return st;
}

/* TRUE if both handles sit on the same disk (equal device paths up to the partition node). */
BOOLEAN same_disk(EFI_HANDLE a, EFI_HANDLE b);

/* Probe the store partitions, then every Simple File System handle for a RUNWAY.DAT, and cluster the stores by
 * UUID. A RUNWAY.DAT on a disk that has a store partition is ignored. The booted store is on the loading ESP,
 * or on the disk of the loading ESP when it is a partition. */
void scan_all_volumes(EFI_HANDLE fat_fs_handle);

/* The group that holds the booted store, or NULL. */
vol_group_t *find_boot_group(void);

/* Bring every lagging member up to the master's generation; a member that fails is marked degraded. */
EFI_STATUS catch_up_members(vol_group_t *g, UINTN master_idx, UINT64 max_gen, BOOLEAN is_encrypted,
                            const UINT8 *dek);

/* Find out whether the master is encrypted and get its DEK; every other member must agree. */
EFI_STATUS unlock_group(vol_group_t *g, UINTN master_idx, BOOLEAN *is_encrypted, UINT8 *dek,
                        BOOLEAN *needs_enroll);

/* Mount an instance per volume group. Only the boot store is mandatory: when it fails, EFI_NOT_FOUND is
 * returned with its status in *boot_err and nothing is printed. */
EFI_STATUS mount_all_groups(EFI_HANDLE fat_fs_handle, EFI_STATUS *boot_err);

/* Reinstall SFS on every enrolled partition handle. */
EFI_STATUS install_all_groups(void);

/* Set up the anti-rollback counter of a store (TPM NV index under PCR 7) when the store uses one. */
void anchor_attach(rwy_store_t *bst);

/* Print why the store is taken for rolled back. */
void anchor_report_rollback(rwy_store_t *bst);

/* The payload of a slot, read and loaded. */
typedef struct {
    blob_t kernel, initrd, dtb, args;
    EFI_HANDLE child;
    EFI_LOADED_IMAGE *li;
} native_img_t;

void native_free(native_img_t *img);

/* Load the image in img->kernel (the Secure Boot verdict included). */
EFI_STATUS native_image_load(native_img_t *img, EFI_HANDLE parent, EFI_HANDLE fat_fs_handle);

/* Select the slot to boot and load its payload, before any firmware handle is touched. */
EFI_STATUS native_select(rwy_store_t *bst, EFI_HANDLE self, EFI_HANDLE fat_fs_handle, native_img_t *img,
                         rwy_boot_choice_t *choice, int *unrecorded);

/* Volatile variables that tell the OS which slot, ESP and store members were used. */
void set_boot_var(const char *slot, int unrecorded);
void set_boot_label_var(const char *label);
void set_boot_esp_var(EFI_HANDLE esp);
void set_boot_members_var(const vol_group_t *g);
UINTN part_uuid_text(EFI_HANDLE h, CHAR16 v[37]);

/* Count an entry into the OK slot `revision` for the boot loop guard: a RWY_GUARD_* verdict (rwy_loopguard.h). */
int guard_check(uint64_t revision, const rwy_loop_guard_t *g);
/* Delete the boot loop guard counter. */
void guard_clear(void);

/* Boot Loader Interface variables (vendor GUID 4a67b082-0a4c-41cf-b6c7-440b29bb8c4f). */
void set_loader_vars(EFI_HANDLE esp, const rwy_boot_choice_t *choice, rwy_store_t *s);

typedef struct lf2 lf2_t;
struct lf2 {
    EFI_STATUS (EFIAPI *load)(lf2_t *, EFI_DEVICE_PATH *, BOOLEAN, UINTN *, VOID *);
    blob_t data;
};

typedef struct { VENDOR_DEVICE_PATH vendor; EFI_DEVICE_PATH end; } initrd_path_t;

typedef struct {
    EFI_HANDLE initrd_handle;
    lf2_t initrd_lf2;
    initrd_path_t initrd_path;
    int dtb;
} handover_t;

void handover_init(handover_t *h);

/* Native handover: ARGS as LoadOptions, INITRD as LoadFile2, DTB as the DTB configuration table. */
EFI_STATUS publish_handover(handover_t *h, EFI_LOADED_IMAGE *child_li, const native_img_t *img);

EFI_STATUS prepare_native(handover_t *h, EFI_LOADED_IMAGE *child_li, const native_img_t *img, UINT64 boot_delay);

/* Start the child, then power off so that the QEMU harness exits. */
EFI_STATUS run_child(EFI_HANDLE child, handover_t *h);

#endif
