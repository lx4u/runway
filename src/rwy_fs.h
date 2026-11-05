/*
 * rwy_fs — Runway-backed EFI_SIMPLE_FILE_SYSTEM_PROTOCOL implementation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UEFI mechanics (see README protocol findings):
 * - A file backend reinstalls SFS in place on the ESP handle that holds the store file; a block
 *   backend installs it on the store partition's handle. Either way the consumers that look for
 *   the boot partition find Runway there.
 * - The chain-loaded child is pointed at this handle by overwriting its
 *   LoadedImage->DeviceHandle between LoadImage() and StartImage().
 * - RWY_ANCHOR_GUID lets a cooperating child locate the Runway handle and
 *   the FAT baseline handle explicitly (for comparison benchmarks).
 */
#ifndef RWY_FS_H
#define RWY_FS_H

#include <efi.h>
#include "../include/rwy_core.h"
#include "../include/rwy_block.h"
#include "../include/rwy_crypto.h"

#ifndef CONTAINER_OF
#define CONTAINER_OF(ptr, type, member) \
    ((type *)((UINT8 *)(ptr) - __builtin_offsetof(type, member)))
#endif

/* Vendor anchor protocol installed on the Runway filesystem handle. */
#define RWY_ANCHOR_GUID \
    { 0x54464154, 0xface, 0x4001, {0x8e, 0x39, 0x54, 0x46, 0x41, 0x54, 0x30, 0x31} }

typedef struct {
    UINT32 magic;               /* 'RWAY' */
    UINT32 version;             /* 1 */
    EFI_HANDLE fat_fs_handle;   /* ESP handle (firmware FAT) for baseline */
    EFI_HANDLE rwy_fs_handle;  /* this handle */
    UINT64 store_size;          /* RWY_STORE_SIZE */
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fat_sfs; /* original firmware FAT SFS (pre-interception) */
} rwy_anchor_t;

#define RWY_ANCHOR_MAGIC 0x59415752u /* 'RWAY' LE */

/* How the loader reached a store. */
typedef enum {
    RWY_BACKEND_FILE = 1,   /* \RUNWAY.DAT of a firmware file system */
    RWY_BACKEND_BLOCK,      /* a whole partition through EFI_BLOCK_IO */
} rwy_backend_kind_t;

/* A store as the loader sees it: its kind and the raw block device below any encryption. */
typedef struct {
    rwy_backend_kind_t kind;
    rwy_block_dev_t dev;
    const rwy_dev_stats_t *stats;   /* what the backend did; owned by the backend */
} rwy_backend_t;

/*
 * Multi-tenant filesystem instance context.
 * Encapsulates the rwy_store_t core, block layer device contexts,
 * SFS protocol interface, and volume metadata.
 */
typedef struct rwy_fs_instance {
    rwy_store_t                     store;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  sfs;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *old_sfs;         /* original firmware SFS (file backend only) */
    EFI_HANDLE                       fat_handle;      /* Handle that gets the file protocol */
    rwy_backend_t                    backings[RWY_MIRROR_MAX_DEVS];
    UINTN                            num_backings;
    /* Partition handle (and original firmware SFS) of each backing member. The firmware SFS and
     * `installed` are for the file backend. */
    EFI_HANDLE                       member_handles[RWY_MIRROR_MAX_DEVS];
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *member_old_sfs[RWY_MIRROR_MAX_DEVS];
    BOOLEAN                          installed;       /* SFS reinstalled on member handles */
    rwy_mirror_dev_t                mirror_ctx;
    rwy_block_dev_t                 block_dev;
    rwy_crypto_dev_ctx_t            crypto_ctx;
    rwy_block_dev_t                 crypto_dev;
    rwy_anchor_t                    anchor;
    EFI_HANDLE                       rwy_handle;
    UINT8                            uuid[16];
    CHAR16                           volume_label[32];
    BOOLEAN                          mounted;
    BOOLEAN                          is_boot_volume;
    UINT64                           read_bytes;
    UINT64                           write_bytes;
    UINT64                           read_ops;
    UINT64                           write_ops;
    struct rwy_fs_instance         *next;
} rwy_fs_instance_t;

/* The file backend: a store file of `size` bytes, kept open by the caller. Its writes fail once a flush
 * finds the file at another size. Fails only when the small context cannot be allocated. */
EFI_STATUS rwy_fs_file_backend(EFI_FILE_HANDLE fh, UINT64 size, rwy_backend_t *be);

/* Release a file backend that was opened but not kept. */
void rwy_fs_file_close(rwy_backend_t *be);

/* The block backend: the partition behind an EFI_BLOCK_IO, from byte 0. The device size is the partition
 * size until rwy_fs_block_limit() sets the size of the store. Fails when the media does not fit the rules
 * of the backend (logical partition, block size 512 to 4096, IoAlign up to 4096). */
EFI_STATUS rwy_fs_block_backend(EFI_BLOCK_IO_PROTOCOL *bio, rwy_backend_t *be);

/* Give the core a device of exactly `size` bytes; fails when the partition is smaller. */
EFI_STATUS rwy_fs_block_limit(rwy_backend_t *be, UINT64 size);

/* Release a block backend that was opened but not kept. */
void rwy_fs_block_close(rwy_backend_t *be);

/* Put the file protocol (and the anchor) on the member at idx. A file backend replaces the firmware
 * protocol of its handle; a block backend gets it on a handle that has none. `baseline` is a handle with a
 * firmware FAT for the benchmark comparison of a block backend (NULL for none). */
void rwy_fs_select_view(rwy_fs_instance_t *inst, UINTN idx, EFI_HANDLE baseline);

/* Create the Runway filesystem on a partition handle via in-place protocol reinstallation.
 * backing: the store (copied into the instance).
 * fat_fs_handle: authentic partition handle (reinstalled in-place).
 * out_handle: the reinstalled SFS handle (fat_fs_handle). */
EFI_STATUS rwy_fs_create(const rwy_backend_t *backing, EFI_HANDLE fat_fs_handle,
                          EFI_HANDLE *out_handle);

/* Create Runway filesystem with optional DEK for block-layer decryption.
 * If dek is NULL, mounts unencrypted. If non-NULL, wraps backing with AES-256-XTS. */
EFI_STATUS rwy_fs_create_crypto(const rwy_backend_t *backing, EFI_HANDLE fat_fs_handle,
                                const UINT8 *dek, EFI_HANDLE *out_handle);

/* Create Runway filesystem over multiple mirrored backings.
 * backings: the store of each mirror member.
 * num_backings: number of mirror members (must be >= 1).
 * fat_fs_handle: primary partition handle (reinstalled in-place).
 * dek: optional crypto key.
 * out_handle: the reinstalled SFS handle. */
EFI_STATUS rwy_fs_create_mirror(const rwy_backend_t backings[], UINTN num_backings,
                                 EFI_HANDLE fat_fs_handle, const UINT8 *dek,
                                 EFI_HANDLE *out_handle);

/* Two-phase lifecycle.
 * rwy_fs_mount_group(): build block device (mirror for >1 members, single
 * device for one), mount the store and prepare the SFS/anchor, WITHOUT
 * touching the handle database. backings[i] lives on partition handles[i].
 * rwy_fs_install_group(): reinstall SFS in place on every member partition
 * handle (+ anchor on the primary). Call only after all groups are mounted. */
EFI_STATUS rwy_fs_mount_group(const rwy_backend_t backings[], EFI_HANDLE handles[],
                               UINTN num_members, const UINT8 *dek,
                               rwy_fs_instance_t **out_inst);

/* Release a mounted instance that was never installed. Removes it from the instance list. */
void rwy_fs_unmount_group(rwy_fs_instance_t *inst);

EFI_STATUS rwy_fs_install_group(rwy_fs_instance_t *inst);

/* Count the mirror events of a mounted group in its state record: events[i] has a bit (1 << RWY_SS_EV_*) for
 * backings[i], and the write errors of the mirror since the mount are counted too. Writes nothing when healthy. */
EFI_STATUS rwy_fs_note_events(rwy_fs_instance_t *inst, const rwy_backend_t backings[], const UINT32 events[],
                              UINTN num_members, const UINT8 *dek);

/* Copy the newest state record to every member that is behind. A record write does not change the
 * tree generation, so call this before the catch-up of lagging members. */
EFI_STATUS rwy_fs_sync_records(const rwy_backend_t backings[], UINTN num_members, const UINT8 *dek,
                                UINT32 rec_start);

/* Set volume label on instance. */
EFI_STATUS rwy_fs_set_label(rwy_fs_instance_t *inst, const CHAR16 *label);

/* Get volume label on instance. */
const CHAR16 *rwy_fs_get_label(const rwy_fs_instance_t *inst);

/* Enumerate active filesystem instances. */
rwy_fs_instance_t *rwy_fs_get_instances(void);

/* The calls the backings of an instance made to the firmware, added up. */
void rwy_fs_dev_stats(const rwy_fs_instance_t *inst, rwy_dev_stats_t *out);

/* For diagnostics: bytes read/written through the Runway layer. */
void rwy_fs_counters(UINT64 *out_read_bytes, UINT64 *out_write_bytes,
                      UINT64 *out_read_ops, UINT64 *out_write_ops);

#endif
