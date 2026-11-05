/*
 * rwy_scan.c — Group discovered stores by store UUID and pair the booted member.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_boot.h"
#include "rwy_partscan.h"

vol_group_t gGroups[RWY_MAX_VOLUMES];
UINTN gGroupCount;

/* The group of a store UUID; a new one when the UUID is not known yet. NULL when the table is full. */
static vol_group_t *find_or_add_group(const rwy_superblock_t *sb, const char *uuid_buf, EFI_HANDLE h) {
    for (UINTN i = 0; i < gGroupCount; i++) {
        if (uuid_eq(gGroups[i].uuid, sb->uuid)) return &gGroups[i];
    }
    if (gGroupCount >= RWY_MAX_VOLUMES) {
        RWY_WARN("max volume groups (%u) reached; ignoring handle %p\n", RWY_MAX_VOLUMES, h);
        return NULL;
    }
    vol_group_t *g = &gGroups[gGroupCount++];
    memcpy(g->uuid, sb->uuid, 16);
    g->count = 0;
    RWY_DEBUG("scan: new volume group %u, UUID %s\n", (unsigned int)(g - gGroups), uuid_buf);
    return g;
}

/* Add a member to its group, the booted one first. FALSE when the group is full. */
static BOOLEAN add_member(vol_group_t *g, const mirror_member_t *m, const char *uuid_buf, EFI_HANDLE h) {
    UINTN at = m->is_booted ? 0 : g->count;

    if (g->count >= RWY_MIRROR_MAX_DEVS) {
        RWY_WARN("max mirror devices (%u) reached; ignoring handle %p\n", RWY_MIRROR_MAX_DEVS, h);
        return FALSE;
    }
    for (UINTN i = g->count; i > at; i--) g->members[i] = g->members[i - 1];
    g->members[at] = *m;
    if (m->is_booted) {
        g->has_boot = TRUE;
        RWY_DEBUG("mirror: booted store UUID %s (generation %llu, slot %c)\n",
                uuid_buf, (unsigned long long)m->gen, m->slot == 0 ? 'A' : 'B');
    } else {
        RWY_DEBUG("mirror: discovered member %u of group %u on handle %p (generation %llu, slot %c)\n",
                (unsigned int)at, (unsigned int)(g - gGroups), h,
                (unsigned long long)m->gen, m->slot == 0 ? 'A' : 'B');
    }
    g->count++;
    return TRUE;
}

/* TRUE if a store partition of a valid group sits on the disk of the handle. */
static BOOLEAN disk_has_block_store(EFI_HANDLE h) {
    for (UINTN gi = 0; gi < gGroupCount; gi++) {
        for (UINTN i = 0; i < gGroups[gi].count; i++) {
            if (gGroups[gi].members[i].backend.kind == RWY_BACKEND_BLOCK &&
                same_disk(gGroups[gi].members[i].handle, h))
                return TRUE;
        }
    }
    return FALSE;
}

/* Fill the identity of a member from the superblock it was found with and enter it in its group.
 * FALSE when it does not fit; the caller then releases the store. */
static BOOLEAN enroll_member(mirror_member_t *m, EFI_HANDLE h, const rwy_superblock_t *sb, int slot,
                             UINT64 gen, BOOLEAN is_booted) {
    char uuid_buf[37];
    vol_group_t *g;

    rwy_format_uuid(sb->uuid, uuid_buf);
    m->handle = h;
    m->sb = *sb;
    m->slot = slot;
    m->gen = gen;
    m->is_booted = is_booted;
    m->degraded = FALSE;
    m->events = 0;
    g = find_or_add_group(sb, uuid_buf, h);
    return g && add_member(g, m, uuid_buf, h);
}

/* Scan and open: probe one SFS handle for \RUNWAY.DAT. If it carries a
 * valid store, keep the backing handle open and classify it into a volume
 * group by superblock UUID. Handles without a store are left untouched. */
static void scan_candidate(EFI_HANDLE h, BOOLEAN is_booted) {
    file_store_t fs;
    rwy_superblock_t sb;
    int slot = 0;
    UINT64 gen = 0;
    mirror_member_t m;

    if (EFI_ERROR(file_store_open(h, &fs))) return;
    if (disk_has_block_store(h)) {
        RWY_WARN("RUNWAY.DAT on handle %p ignored: the disk has a store partition\n", h);
        file_store_close(&fs);
        return;
    }
    if (EFI_ERROR(rwy_fs_file_backend(fs.fh, fs.size, &m.backend))) {
        file_store_close(&fs);
        return;
    }
    if (fs.size < RWY_MIN_STORE_SIZE || fs.size > RWY_MAX_STORE_SIZE ||
        EFI_ERROR(read_store_active_sb(&m.backend.dev, &sb, &slot, &gen))) {
        if (is_booted) RWY_WARN("booted RUNWAY.DAT invalid\n");
        file_store_close(&fs);
        return;
    }
    m.root = fs.root;
    if (!enroll_member(&m, h, &sb, slot, gen, is_booted))
        file_store_close(&fs);
}

/* The loaded ESP and the first store partition on its disk make the booted store. */
static BOOLEAN booted_block_store(EFI_HANDLE h, EFI_HANDLE esp) {
    for (UINTN gi = 0; gi < gGroupCount; gi++) {
        if (gGroups[gi].has_boot) return FALSE;
    }
    return same_disk(h, esp);
}

/* Probe one partition of the store type; a store on it joins its volume group. */
static void scan_block_candidate(EFI_HANDLE h, EFI_HANDLE esp) {
    rwy_superblock_t sb;
    int slot = 0;
    UINT64 gen = 0;
    mirror_member_t m;

    if (!is_store_partition(h) || !block_store_open(h, &m.backend, &sb, &slot, &gen)) return;
    m.root = NULL;
    if (!enroll_member(&m, h, &sb, slot, gen, booted_block_store(h, esp)))
        rwy_fs_block_close(&m.backend);
}

static void scan_block_stores(EFI_HANDLE esp) {
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    UINTN num_handles = 0;
    EFI_HANDLE *handles = NULL;

    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &num_handles, &handles)) || !handles)
        return;
    for (UINTN i = 0; i < num_handles; i++) scan_block_candidate(handles[i], esp);
    gBS->FreePool(handles);
}

void scan_all_volumes(EFI_HANDLE fat_fs_handle) {
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN num_handles = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS s;

    scan_block_stores(fat_fs_handle);
    scan_candidate(fat_fs_handle, TRUE);
    s = gBS->LocateHandleBuffer(ByProtocol, &sfs_guid, NULL, &num_handles, &handles);
    if (EFI_ERROR(s) || !handles) return;
    for (UINTN i = 0; i < num_handles; i++) {
        if (handles[i] == fat_fs_handle) continue;
        scan_candidate(handles[i], FALSE);
    }
    gBS->FreePool(handles);
}

vol_group_t *find_boot_group(void) {
    vol_group_t *boot_group = NULL;

    for (UINTN gi = 0; gi < gGroupCount; gi++) {
        if (gGroups[gi].has_boot) boot_group = &gGroups[gi];
    }
    return boot_group;
}
