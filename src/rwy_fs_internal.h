/*
 * rwy_fs_internal.h — shared by the parts of the EFI file system adapter. Internal.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_FS_INTERNAL_H
#define RWY_FS_INTERNAL_H

#include "rwy_fs.h"

#define RWY_FH_FILE 1
#define RWY_FH_DIR  2

typedef struct rwy_file {
    EFI_FILE_PROTOCOL   proto;       /* MUST be first */
    rwy_fs_instance_t *instance;    /* parent filesystem instance context */
    int                 kind;        /* FILE or DIR */
    UINT64              oid;         /* FILE: object id */
    UINT64              pos;         /* FILE: byte cursor */
    UINT64              open_mode;   /* OpenMode bits */
    char                dirpath[RWY_MAX_PATH]; /* DIR: "" for root, else "A/B" */
    UINTN               enum_cursor; /* DIR: Read-enumeration cursor */
    char                name[RWY_MAX_PATH];    /* FILE: store path (for Delete/GetInfo) */
} rwy_file_t;

/* Copy a string into a fixed buffer, cutting it when too long. */
void copy_str(char *dst, UINTN size, const char *src);

/* CHAR16 UEFI path -> ASCII store path. */
EFI_STATUS c16_to_store(const CHAR16 *w, char *out, UINTN outsz);

/* Resolve `name` against dir `base`. Result: normalized path ("" = root). */
EFI_STATUS resolve_path(const char *base, const char *name, char *out, UINTN outsz);

void ascii_to_c16(const char *a, CHAR16 *w, UINTN wcap);

/* Unix seconds -> EFI_TIME. */
void unix_to_efitime(INT64 secs, EFI_TIME *t);

EFI_STATUS EFIAPI tf_getinfo(EFI_FILE_HANDLE F, EFI_GUID *T, UINTN *S, VOID *B);
EFI_STATUS EFIAPI tf_setinfo(EFI_FILE_HANDLE F, EFI_GUID *T, UINTN S, VOID *B);

/* Keep the label the caller set (at most 31 characters). */
void set_label(rwy_fs_instance_t *inst, const CHAR16 *label);

/* Ensure a transaction is open for staged work. */
int ensure_txn(rwy_fs_instance_t *inst);

EFI_STATUS rwy_rc_to_efi(int rc);

#endif
