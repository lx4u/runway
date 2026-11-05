/*
 * rwy_filebe.c — The file backend: a store kept in RUNWAY.DAT on a firmware file system.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Size guard: some firmware cuts the file at the end of a Write (U-Boot without our patches).
 * The size is read after every flush. When it is not the size seen at scan, the backend logs
 * it once and fails every later write and flush; reads still work.
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_fs.h"
#include "../include/rwy_core.h"
#include "../include/rwy_block.h"

typedef struct {
    EFI_FILE_HANDLE fh;
    UINT64 size;        /* the file size seen at scan */
    BOOLEAN stopped;    /* the size changed: no more writes */
    rwy_dev_stats_t stats;
} file_dev_t;

static int file_pread(void *ctx, UINT64 off, void *buf, size_t len) {
    EFI_FILE_HANDLE fh = ((file_dev_t *)ctx)->fh;
    EFI_STATUS s;
    UINTN n;
    if (off + len > RWY_MAX_STORE_SIZE) return -1;
    s = fh->SetPosition(fh, off);
    if (EFI_ERROR(s)) return -1;
    n = (UINTN)len;
    s = fh->Read(fh, &n, buf);
    if (EFI_ERROR(s) || n != (UINTN)len) return -1;
    ((file_dev_t *)ctx)->stats.reads++;
    ((file_dev_t *)ctx)->stats.read_bytes += len;
    return 0;
}

static int file_pwrite(void *ctx, UINT64 off, const void *buf, size_t len) {
    file_dev_t *d = ctx;
    EFI_STATUS s;
    UINTN n;
    if (d->stopped || off + len > RWY_MAX_STORE_SIZE) return -1;
    if (off % RWY_BLOCK_SIZE || len % RWY_BLOCK_SIZE) return -1;
    s = d->fh->SetPosition(d->fh, off);
    if (EFI_ERROR(s)) return -1;
    n = (UINTN)len;
    s = d->fh->Write(d->fh, &n, (void *)buf);
    if (EFI_ERROR(s) || n != (UINTN)len) return -1;
    d->stats.writes++;
    d->stats.write_bytes += len;
    return 0;
}

/* The size of the open file, 0 when the firmware cannot tell. */
static UINT64 file_size_now(EFI_FILE_HANDLE fh) {
    EFI_GUID fig = EFI_FILE_INFO_ID;
    UINT8 info[128];
    UINTN isz = sizeof(info);

    if (EFI_ERROR(fh->GetInfo(fh, &fig, &isz, info))) return 0;
    return ((EFI_FILE_INFO *)info)->FileSize;
}

static int file_flush(void *ctx) {
    file_dev_t *d = ctx;
    UINT64 now;

    if (d->stopped || EFI_ERROR(d->fh->Flush(d->fh))) return -1;
    d->stats.flushes++;
    now = file_size_now(d->fh);
    if (now == d->size) return 0;
    d->stopped = TRUE;
    RWY_ERROR("RUNWAY.DAT changed size from %llu to %llu bytes, store writes stopped\n",
              (unsigned long long)d->size, (unsigned long long)now);
    return -1;
}

EFI_STATUS rwy_fs_file_backend(EFI_FILE_HANDLE fh, UINT64 size, rwy_backend_t *be) {
    file_dev_t *d;
    EFI_STATUS s = gBS->AllocatePool(EfiLoaderData, sizeof(*d), (void **)&d);

    if (EFI_ERROR(s)) return s;
    d->fh = fh;
    d->size = size;
    d->stopped = FALSE;
    memset(&d->stats, 0, sizeof(d->stats));
    be->kind = RWY_BACKEND_FILE;
    be->dev.read = file_pread;
    be->dev.write = file_pwrite;
    be->dev.flush = file_flush;
    be->dev.ctx = d;
    be->dev.size = size;
    be->dev.block_size = RWY_BLOCK_SIZE;
    be->dev.stats = NULL;
    be->dev.dircache = NULL;
    be->stats = &d->stats;
    return EFI_SUCCESS;
}

void rwy_fs_file_close(rwy_backend_t *be) {
    if (!be || be->kind != RWY_BACKEND_FILE) return;
    if (be->dev.ctx) gBS->FreePool(be->dev.ctx);
    memset(be, 0, sizeof(*be));
}
