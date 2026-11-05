/*
 * rwy_fs_info.c — GetInfo and SetInfo of the file protocol, and the volume label.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_fs_internal.h"
#include "efi_util.h"

/* Length of the volume label, 6 for the default "RUNWAY". */
static UINTN label_length(const rwy_fs_instance_t *inst) {
    UINTN len = 0;
    while (inst->volume_label[len]) len++;
    return len ? len : 6;
}

static void copy_label(const rwy_fs_instance_t *inst, UINTN len, CHAR16 *dst) {
    if (inst->volume_label[0]) {
        for (UINTN i = 0; i <= len; i++) dst[i] = inst->volume_label[i];
    } else {
        dst[0] = (CHAR16)'R'; dst[1] = (CHAR16)'U'; dst[2] = (CHAR16)'N'; dst[3] = (CHAR16)'W'; dst[4] = (CHAR16)'A'; dst[5] = (CHAR16)'Y';
        dst[6] = 0;
    }
}

/* The last component of a path. */
static const char *base_name(const char *p) {
    const char *base = p;
    while (*p) { if (*p == '/') base = p + 1; p++; }
    return base;
}

static EFI_STATUS getinfo_file(rwy_file_t *f, UINTN *S, VOID *B) {
    rwy_fs_instance_t *inst = f->instance;
    const char *base;
    UINTN wl = 0, need;
    EFI_FILE_INFO *fi;
    UINT64 fsize = 0;
    UINT64 attr = EFI_FILE_ARCHIVE;
    rwy_attr_t a;
    int have_attr = 0;

    if (f->kind == RWY_FH_DIR) {
        base = base_name(f->dirpath);
        attr = EFI_FILE_DIRECTORY;
    } else {
        base = base_name(f->name);
        have_attr = rwy_getattr(&inst->store, f->oid, &a) == RWY_OK;
        if (have_attr) fsize = a.size;
    }
    while (base[wl]) wl++;
    need = SIZE_OF_EFI_FILE_INFO + (wl + 1) * sizeof(CHAR16);
    if (!B || *S < need) { *S = need; return EFI_BUFFER_TOO_SMALL; }
    fi = (EFI_FILE_INFO *)B;
    fi->Size = need;
    fi->FileSize = fsize;
    fi->PhysicalSize = fsize;
    fi->Attribute = attr;
    if (have_attr) {
        unix_to_efitime(a.mtime_sec, &fi->ModificationTime);
        unix_to_efitime(a.atime_sec, &fi->LastAccessTime);
        fi->CreateTime = fi->ModificationTime;
    }
    ascii_to_c16(base, fi->FileName, wl + 1);
    *S = need;
    return EFI_SUCCESS;
}

static EFI_STATUS getinfo_fs(rwy_fs_instance_t *inst, UINTN *S, VOID *B) {
    UINTN label_len = label_length(inst);
    UINTN need = SIZE_OF_EFI_FILE_SYSTEM_INFO + (label_len + 1) * sizeof(CHAR16);
    EFI_FILE_SYSTEM_INFO *fsi;
    UINT64 free = 0;

    if (!B || *S < need) { *S = need; return EFI_BUFFER_TOO_SMALL; }
    if (inst->mounted) free = (UINT64)rwy_free_blocks(&inst->store) * RWY_BLOCK_SIZE;
    fsi = (EFI_FILE_SYSTEM_INFO *)B;
    fsi->Size = need;
    fsi->ReadOnly = FALSE;
    fsi->VolumeSize = inst->mounted ? inst->store.sb.total_size : RWY_STORE_SIZE;
    fsi->FreeSpace = free;
    fsi->BlockSize = RWY_BLOCK_SIZE;
    copy_label(inst, label_len, fsi->VolumeLabel);
    *S = need;
    return EFI_SUCCESS;
}

static EFI_STATUS getinfo_label(rwy_fs_instance_t *inst, UINTN *S, VOID *B) {
    UINTN label_len = label_length(inst);
    UINTN need = SIZE_OF_EFI_FILE_SYSTEM_VOLUME_LABEL_INFO + (label_len + 1) * sizeof(CHAR16);

    if (!B || *S < need) { *S = need; return EFI_BUFFER_TOO_SMALL; }
    copy_label(inst, label_len, ((EFI_FILE_SYSTEM_VOLUME_LABEL *)B)->VolumeLabel);
    *S = need;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI tf_getinfo(EFI_FILE_HANDLE F, EFI_GUID *T,
                                    UINTN *S, VOID *B) {
    static EFI_GUID fi_guid = EFI_FILE_INFO_ID;
    static EFI_GUID fsi_guid = EFI_FILE_SYSTEM_INFO_ID;
    static EFI_GUID fsvl_guid = EFI_FILE_SYSTEM_VOLUME_LABEL_ID;
    rwy_file_t *f = (rwy_file_t *)F;
    if (!F || !T || !S) return EFI_INVALID_PARAMETER;
    if (!f->instance) return EFI_DEVICE_ERROR;

    if (guid_eq(T, &fi_guid)) return getinfo_file(f, S, B);
    if (guid_eq(T, &fsi_guid)) return getinfo_fs(f->instance, S, B);
    if (guid_eq(T, &fsvl_guid)) return getinfo_label(f->instance, S, B);
    return EFI_UNSUPPORTED;
}

/* Keep the label the caller set (at most 31 characters). */
void set_label(rwy_fs_instance_t *inst, const CHAR16 *label) {
    UINTN l = 0;
    while (label[l] && l < 31) {
        inst->volume_label[l] = label[l];
        l++;
    }
    inst->volume_label[l] = 0;
}

/* The new name as a full path: a name without '/' stays in the directory of the file. */
static EFI_STATUS rename_target(const rwy_file_t *f, const char *aname, char *full, UINTN size) {
    const char *last = NULL;
    int hasslash = 0;

    for (const char *p = aname; *p; p++) if (*p == '/') hasslash = 1;
    if (hasslash) return resolve_path("", aname, full, size);
    for (const char *p = f->name; *p; p++) if (*p == '/') last = p;
    if (last) {
        UINTN dl = (UINTN)(last - f->name);
        UINTN al = 0;
        while (aname[al]) al++;
        if (dl + 1 + al + 1 > size) return EFI_BAD_BUFFER_SIZE;
        for (UINTN i = 0; i < dl; i++) full[i] = f->name[i];
        full[dl] = '/';
        for (UINTN i = 0; i <= al; i++) full[dl + 1 + i] = aname[i];
    } else {
        UINTN i = 0;
        while (aname[i] && i + 1 < size) { full[i] = aname[i]; i++; }
        full[i] = 0;
    }
    return EFI_SUCCESS;
}

/* Same name: a smaller FileSize truncates, anything else changes nothing. */
static EFI_STATUS setinfo_truncate(rwy_file_t *f, UINT64 size) {
    rwy_fs_instance_t *inst = f->instance;
    rwy_attr_t a;
    int rc;

    if (rwy_getattr(&inst->store, f->oid, &a) != RWY_OK) return EFI_DEVICE_ERROR;
    if (size >= a.size) return EFI_SUCCESS;
    rc = rwy_truncate_oid(&inst->store, f->oid, size);
    if (rc == RWY_ERR_NOSPC) return EFI_VOLUME_FULL;
    if (rc != RWY_OK) return EFI_DEVICE_ERROR;
    if (f->pos > size) f->pos = size;
    return EFI_SUCCESS;
}

static EFI_STATUS setinfo_file(rwy_file_t *f, const EFI_FILE_INFO *fi) {
    rwy_fs_instance_t *inst = f->instance;
    char aname[RWY_MAX_PATH], full[RWY_MAX_PATH];
    EFI_STATUS s;
    UINTN i = 0;
    int rc;

    s = c16_to_store(fi->FileName, aname, sizeof(aname));
    if (EFI_ERROR(s)) return s;
    s = rename_target(f, aname, full, sizeof(full));
    if (EFI_ERROR(s)) return s;
    rc = ensure_txn(inst);
    if (rc != RWY_OK) return EFI_DEVICE_ERROR;
    while (full[i] == f->name[i] && full[i]) i++;
    if (full[i] == f->name[i])
        return setinfo_truncate(f, fi->FileSize);
    rc = rwy_rename(&inst->store, f->name, full);
    if (rc == RWY_ERR_EXIST) return EFI_ACCESS_DENIED;
    if (rc != RWY_OK) return rwy_rc_to_efi(rc);
    for (i = 0; full[i] && i + 1 < sizeof(f->name); i++) f->name[i] = full[i];
    f->name[i] = 0;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI tf_setinfo(EFI_FILE_HANDLE F, EFI_GUID *T,
                                    UINTN S, VOID *B) {
    static EFI_GUID fi_guid = EFI_FILE_INFO_ID;
    static EFI_GUID fsi_guid = EFI_FILE_SYSTEM_INFO_ID;
    static EFI_GUID fsvl_guid = EFI_FILE_SYSTEM_VOLUME_LABEL_ID;
    rwy_file_t *f = (rwy_file_t *)F;
    (void)S;
    if (!F || !T || !B) return EFI_INVALID_PARAMETER;
    if (!f->instance) return EFI_DEVICE_ERROR;

    if (guid_eq(T, &fsi_guid)) {
        set_label(f->instance, ((EFI_FILE_SYSTEM_INFO *)B)->VolumeLabel);
        return EFI_SUCCESS;
    }
    if (guid_eq(T, &fsvl_guid)) {
        set_label(f->instance, ((EFI_FILE_SYSTEM_VOLUME_LABEL *)B)->VolumeLabel);
        return EFI_SUCCESS;
    }
    if (!guid_eq(T, &fi_guid)) return EFI_UNSUPPORTED;
    if (f->kind != RWY_FH_FILE) return EFI_UNSUPPORTED;
    return setinfo_file(f, (const EFI_FILE_INFO *)B);
}
