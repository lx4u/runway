/*
 * filebe_test — the size guard and the block rule of the file backend against a fake EFI_FILE_PROTOCOL in memory.
 * SPDX-License-Identifier: Apache-2.0
 * The fake can behave like U-Boot without our patches: a Write cuts the file at pos + len.
 * Real firmware is not involved; the guard is only shown to react to the sizes it is told.
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_fs.h"

#define FILE_BYTES (RWY_MIN_STORE_SIZE)

typedef struct {
    EFI_FILE_PROTOCOL proto;    /* MUST be first */
    UINT8 *data;
    UINT64 size, pos;
    UINTN writes;
    BOOLEAN trim_on_write, grow_on_write, fail_info, fail_flush;
} fake_t;

static int failures;

static void check(const char *what, int ok) {
    uprintf("FILETEST: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static EFI_STATUS EFIAPI fake_setpos(EFI_FILE_PROTOCOL *This, UINT64 pos) {
    ((fake_t *)This)->pos = pos;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fake_read(EFI_FILE_PROTOCOL *This, UINTN *len, VOID *buf) {
    fake_t *f = (fake_t *)This;
    UINT64 left = f->pos < f->size ? f->size - f->pos : 0;

    if (*len > left) *len = (UINTN)left;
    memcpy(buf, f->data + f->pos, *len);
    f->pos += *len;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fake_write(EFI_FILE_PROTOCOL *This, UINTN *len, VOID *buf) {
    fake_t *f = (fake_t *)This;

    f->writes++;
    if (f->pos + *len > FILE_BYTES) return EFI_VOLUME_FULL;
    memcpy(f->data + f->pos, buf, *len);
    f->pos += *len;
    if (f->trim_on_write) f->size = f->pos;
    if (f->grow_on_write) f->size += 4096;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fake_flush(EFI_FILE_PROTOCOL *This) {
    return ((fake_t *)This)->fail_flush ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fake_getinfo(EFI_FILE_PROTOCOL *This, EFI_GUID *type, UINTN *sz, VOID *buf) {
    (void)type;
    fake_t *f = (fake_t *)This;
    EFI_FILE_INFO *fi = buf;

    if (f->fail_info) return EFI_DEVICE_ERROR;
    if (*sz < sizeof(*fi)) {
        *sz = sizeof(*fi);
        return EFI_BUFFER_TOO_SMALL;
    }
    memset(fi, 0, sizeof(*fi));
    fi->FileSize = f->size;
    *sz = sizeof(*fi);
    return EFI_SUCCESS;
}

static void fake_init(fake_t *f, UINT8 *data) {
    memset(f, 0, sizeof(*f));
    f->data = data;
    f->size = FILE_BYTES;
    f->proto.SetPosition = fake_setpos;
    f->proto.Read = fake_read;
    f->proto.Write = fake_write;
    f->proto.Flush = fake_flush;
    f->proto.GetInfo = fake_getinfo;
}

/* A commit as the core does it: write, flush, write, flush. */
static int commit(rwy_block_dev_t *d, const UINT8 *buf) {
    if (d->write(d->ctx, 4096, buf, 4096) || d->flush(d->ctx)) return -1;
    if (d->write(d->ctx, 0, buf, 4096) || d->flush(d->ctx)) return -1;
    return 0;
}

static void test_guard(UINT8 *data, UINT8 *buf, UINT8 *back) {
    static fake_t f;
    rwy_backend_t be;
    rwy_block_dev_t *d = &be.dev;

    fake_init(&f, data);
    check("the backend opens", !EFI_ERROR(rwy_fs_file_backend(&f.proto, f.size, &be)));
    check("the device size is the size given", d->size == FILE_BYTES && be.kind == RWY_BACKEND_FILE);
    check("commits pass while the size holds", commit(d, buf) == 0 && commit(d, buf) == 0);
    check("a flush without writes passes", d->flush(d->ctx) == 0);

    f.writes = 0;
    check("a write at an odd offset is refused", d->write(d->ctx, 100, buf, 4096) != 0);
    check("a short write is refused", d->write(d->ctx, 4096, buf, 100) != 0);
    check("a refused write does not reach the firmware", f.writes == 0);

    f.fail_flush = TRUE;
    check("a failed Flush fails the flush", d->flush(d->ctx) != 0);
    f.fail_flush = FALSE;
    check("the backend is not stopped by a failed Flush", commit(d, buf) == 0);

    f.trim_on_write = TRUE;
    check("a Write that cuts the file fails the commit", commit(d, buf) != 0);
    f.trim_on_write = FALSE;
    check("the file is as the firmware left it", f.size == 8192);
    check("writes fail after the size changed", d->write(d->ctx, 8192, buf, 4096) != 0);
    check("flush fails after the size changed", d->flush(d->ctx) != 0);
    f.size = FILE_BYTES;
    check("a size that comes back does not re-arm the backend", commit(d, buf) != 0);
    check("reads still work", d->read(d->ctx, 4096, back, 4096) == 0 && CompareMem(back, buf, 4096) == 0);

    fake_init(&f, data);
    check("the backend opens again", !EFI_ERROR(rwy_fs_file_backend(&f.proto, f.size, &be)));
    f.grow_on_write = TRUE;
    check("a file that grew fails the commit", commit(d, buf) != 0);
    check("writes stay stopped after growth", d->write(d->ctx, 0, buf, 4096) != 0);

    fake_init(&f, data);
    check("the backend opens a third time", !EFI_ERROR(rwy_fs_file_backend(&f.proto, f.size, &be)));
    f.fail_info = TRUE;
    check("a size that cannot be read fails the commit", commit(d, buf) != 0);
    check("writes stay stopped when the size is unknown", d->write(d->ctx, 0, buf, 4096) != 0);
}

/* Pages, so that the buffers are 4096-byte aligned. */
static UINT8 *alloc_pages(UINTN bytes) {
    EFI_PHYSICAL_ADDRESS a = 0;

    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, bytes / 4096, &a))) return NULL;
    return (UINT8 *)(UINTN)a;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    UINT8 *data, *buf, *back;

    util_init(ImageHandle, SystemTable);
    uprint("FILETEST\n");
    data = alloc_pages(FILE_BYTES);
    buf = alloc_pages(4096);
    back = alloc_pages(4096);
    if (!data || !buf || !back) {
        uprint("FILETEST FAIL: no memory\n");
        failures++;
    } else {
        memset(buf, 0x5a, 4096);
        test_guard(data, buf, back);
    }
    uprintf("FILETEST DONE failures=%d\n", failures);
    gBS->Stall(500000);
    gST->RuntimeServices->ResetSystem(EfiResetShutdown, failures ? EFI_ABORTED : EFI_SUCCESS, 0, NULL);
    return failures ? EFI_ABORTED : EFI_SUCCESS;
}
