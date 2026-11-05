/*
 * blockio_test — the block backend against a fake EFI_BLOCK_IO that lives in memory.
 * SPDX-License-Identifier: Apache-2.0
 * The fake behaves like a strict firmware: it refuses a buffer that breaks IoAlign, a
 * range outside the media and a stale MediaId. The test counts calls to see the coalescing.
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_fs.h"

#define PART_BYTES (RWY_MIN_STORE_SIZE)

typedef struct {
    EFI_BLOCK_IO_PROTOCOL proto;    /* MUST be first */
    EFI_BLOCK_IO_MEDIA media;
    UINT8 *data;
    UINTN reads, writes, flushes;
    UINT64 last_lba;
    UINTN last_len;
    BOOLEAN fail_flush;
} fake_t;

static int failures;

static void check(const char *what, int ok) {
    uprintf("BLOCKTEST: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static EFI_STATUS fake_range(fake_t *f, UINT32 id, EFI_LBA lba, UINTN len, void *buf) {
    if (id != f->media.MediaId) return EFI_MEDIA_CHANGED;
    if (len % f->media.BlockSize) return EFI_BAD_BUFFER_SIZE;
    if (f->media.IoAlign > 1 && ((UINTN)buf % f->media.IoAlign)) return EFI_INVALID_PARAMETER;
    if (lba + len / f->media.BlockSize > f->media.LastBlock + 1) return EFI_INVALID_PARAMETER;
    f->last_lba = lba;
    f->last_len = len;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI fake_read(EFI_BLOCK_IO_PROTOCOL *This, UINT32 id, EFI_LBA lba, UINTN len, VOID *buf) {
    fake_t *f = (fake_t *)This;
    EFI_STATUS s = fake_range(f, id, lba, len, buf);

    f->reads++;
    if (!EFI_ERROR(s)) memcpy(buf, f->data + lba * f->media.BlockSize, len);
    return s;
}

static EFI_STATUS EFIAPI fake_write(EFI_BLOCK_IO_PROTOCOL *This, UINT32 id, EFI_LBA lba, UINTN len, VOID *buf) {
    fake_t *f = (fake_t *)This;
    EFI_STATUS s = fake_range(f, id, lba, len, buf);

    f->writes++;
    if (!EFI_ERROR(s)) memcpy(f->data + lba * f->media.BlockSize, buf, len);
    return s;
}

static EFI_STATUS EFIAPI fake_flush(EFI_BLOCK_IO_PROTOCOL *This) {
    fake_t *f = (fake_t *)This;

    f->flushes++;
    return f->fail_flush ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

static void fake_init(fake_t *f, UINT32 block_size, UINT32 io_align) {
    memset(&f->media, 0, sizeof(f->media));
    f->media.MediaId = 7;
    f->media.MediaPresent = TRUE;
    f->media.LogicalPartition = TRUE;
    f->media.BlockSize = block_size;
    f->media.IoAlign = io_align;
    f->media.LastBlock = PART_BYTES / block_size - 1;
    f->proto.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
    f->proto.Media = &f->media;
    f->proto.ReadBlocks = fake_read;
    f->proto.WriteBlocks = fake_write;
    f->proto.FlushBlocks = fake_flush;
    f->reads = f->writes = f->flushes = 0;
    f->fail_flush = FALSE;
}

static BOOLEAN same(const void *a, const void *b, UINTN n) {
    return CompareMem(a, b, n) == 0;
}

static void fill(UINT8 *p, UINTN n, UINT8 seed) {
    for (UINTN i = 0; i < n; i++) p[i] = (UINT8)(seed + i * 7 + (i >> 8));
}

/* Rules on the media: what the backend refuses to open. */
static void test_media_rules(fake_t *f) {
    rwy_backend_t be;
    UINT32 sizes[] = { 256, 520, 1000, 8192 };

    for (UINTN i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        fake_init(f, 512, 0);
        f->media.BlockSize = sizes[i];
        check("block size outside 512..4096 or not a power of two is refused",
              EFI_ERROR(rwy_fs_block_backend(&f->proto, &be)));
    }
    fake_init(f, 512, 0);
    f->media.LogicalPartition = FALSE;
    check("a whole disk is refused", EFI_ERROR(rwy_fs_block_backend(&f->proto, &be)));
    fake_init(f, 512, 0);
    f->media.MediaPresent = FALSE;
    check("no media is refused", EFI_ERROR(rwy_fs_block_backend(&f->proto, &be)));
    fake_init(f, 512, 8192);
    check("IoAlign above 4096 is refused", EFI_ERROR(rwy_fs_block_backend(&f->proto, &be)));
    fake_init(f, 512, 12);
    check("IoAlign that is not a power of two is refused", EFI_ERROR(rwy_fs_block_backend(&f->proto, &be)));
    fake_init(f, 512, 0);
    f->media.LastBlock = RWY_MIN_STORE_SIZE / 512 - 2;
    check("a partition below the smallest store is refused", EFI_ERROR(rwy_fs_block_backend(&f->proto, &be)));
    fake_init(f, 4096, 0);
    check("size is (LastBlock + 1) * BlockSize",
          !EFI_ERROR(rwy_fs_block_backend(&f->proto, &be)) && be.dev.size == PART_BYTES);
    rwy_fs_block_close(&be);
}

/* The read window: hits, misses, invalidation by a write, partial blocks, MediaId and flush. */
static void test_cache(fake_t *f, rwy_block_dev_t *d, UINT8 *buf, UINT8 *back) {
    UINTN base = 5 * 65536;
    UINT8 old[64];

    fill(f->data + base, 3 * 65536, 11);
    f->reads = 0;
    check("a small read is right", d->read(d->ctx, base + 100, back, 32) == 0 && same(back, f->data + base + 100, 32));
    check("the first small read fills the window in one call", f->reads == 1);
    check("a read across a block boundary in the window is a hit",
          d->read(d->ctx, base + 4000, back, 200) == 0 && same(back, f->data + base + 4000, 200) && f->reads == 1);
    check("a read across the end of the window takes the next window",
          d->read(d->ctx, base + 65536 - 8, back, 16) == 0 && same(back, f->data + base + 65536 - 8, 16) && f->reads == 2);
    check("a read in another window is a miss", d->read(d->ctx, base + 2 * 65536 + 5, back, 8) == 0 &&
          same(back, f->data + base + 2 * 65536 + 5, 8) && f->reads == 3);
    f->reads = 0;
    check("a large read is not cached", d->read(d->ctx, base, back, 65536) == 0 && same(back, f->data + base, 65536) &&
          f->reads == 1);

    memcpy(old, f->data + base + 2 * 65536 + 100, 64);
    check("fill the window", d->read(d->ctx, base + 2 * 65536 + 100, back, 64) == 0);
    f->data[base + 2 * 65536 + 100] ^= 0xff;
    f->reads = 0;
    check("a hit comes from memory", d->read(d->ctx, base + 2 * 65536 + 100, back, 64) == 0 && same(back, old, 64) &&
          f->reads == 0);
    f->data[base + 2 * 65536 + 100] ^= 0xff;
    fill(buf, 4096, 21);
    check("a write in another window invalidates", d->write(d->ctx, base + 4096, buf, 4096) == 0);
    f->reads = 0;
    check("a read after a write fetches again", d->read(d->ctx, base + 4096 + 10, back, 100) == 0 &&
          same(back, buf + 10, 100) && f->reads == 1);
    fill(buf, 4096, 22);
    check("a block write is seen by an unaligned read across its edge",
          d->write(d->ctx, base + 8192, buf, 4096) == 0 && d->read(d->ctx, base + 8192 - 20, back, 100) == 0 &&
          same(back, f->data + base + 8192 - 20, 100) && same(back + 20, buf, 80));

    check("fill the window again", d->read(d->ctx, base + 300, back, 16) == 0);
    f->reads = 0;
    check("flush keeps the window", d->flush(d->ctx) == 0 && d->read(d->ctx, base + 300, back, 16) == 0 && f->reads == 0);
    f->media.MediaId++;
    check("a changed MediaId fails a read that is in the window", d->read(d->ctx, base + 300, back, 16) != 0);
    f->media.MediaId--;
}

/* One block size and IoAlign: round trip, run coalescing, alignment, bounds. */
static void test_io(fake_t *f, UINT32 bs, UINT32 align, UINT8 *buf, UINT8 *back) {
    rwy_backend_t be;
    rwy_block_dev_t *d;
    UINTN run = 1048576;

    uprintf("BLOCKTEST: block size %u, IoAlign %u\n", bs, align);
    fake_init(f, bs, align);
    if (EFI_ERROR(rwy_fs_block_backend(&f->proto, &be))) {
        check("backend opens", 0);
        return;
    }
    d = &be.dev;
    fill(buf, run, (UINT8)bs);

    check("write of a 1 MiB run", d->write(d->ctx, 8192, buf, run) == 0);
    check("read back of the run", d->read(d->ctx, 8192, back, run) == 0 && same(buf, back, run));
    check("the run is at the right LBA", same(f->data + 8192, buf, run));
    check("an aligned run is one WriteBlocks call", f->writes == 1);
    check("an aligned run is one ReadBlocks call", f->reads == 1);

    if (align > 1) {
        /* A buffer one byte off IoAlign goes through the bounce buffer, in aligned calls */
        f->reads = f->writes = 0;
        fill(buf + 1, run, 3);
        check("write from a misaligned buffer", d->write(d->ctx, 1048576, buf + 1, run) == 0);
        memset(back, 0, run + 1);
        check("read into a misaligned buffer", d->read(d->ctx, 1048576, back + 1, run) == 0);
        check("misaligned data is intact", same(back + 1, buf + 1, run) && same(f->data + 1048576, buf + 1, run));
        check("a 1 MiB misaligned run is one bounce call each way", f->writes == 1 && f->reads == 1);
        f->writes = 0;
        fill(buf + 1, 3 * run, 5);
        check("a 3 MiB misaligned write is split by the bounce size",
              d->write(d->ctx, 4194304, buf + 1, 3 * run) == 0 && f->writes == 3 &&
              same(f->data + 4194304, buf + 1, 3 * run));
    }

    /* Reads may be unaligned. Writes must be whole blocks at block offsets: nothing else reaches the media. */
    {
        UINT8 *shadow = back + 2 * run;
        UINTN base = 100000 / 4096 * 4096;

        memcpy(shadow, f->data + base, 4 * 4096);
        check("a 16 byte read at an odd offset", d->read(d->ctx, 100003, back, 16) == 0 &&
              same(back, f->data + 100003, 16));
        check("a read across three blocks at an odd offset", d->read(d->ctx, 100003, back, 5000) == 0 &&
              same(back, f->data + 100003, 5000));
        fill(buf, 8192, 9);
        f->writes = 0;
        check("a write at an odd offset is refused", d->write(d->ctx, 100003, buf, 4096) != 0);
        check("a write at a 512 byte offset is refused", d->write(d->ctx, base + 512, buf, 4096) != 0);
        check("a short write is refused", d->write(d->ctx, base, buf, 100) != 0);
        check("a write that is not a block multiple is refused", d->write(d->ctx, base, buf, 4096 + 512) != 0);
        check("a refused write reaches no media call", f->writes == 0 && same(f->data + base, shadow, 4 * 4096));
        memcpy(shadow + 4096, buf, 8192);
        check("a two block write at a block offset", d->write(d->ctx, base + 4096, buf, 8192) == 0 &&
              same(f->data + base, shadow, 4 * 4096));
    }
    check("the last block reads", d->read(d->ctx, PART_BYTES - 4096, back, 4096) == 0);
    check("a read past the end is refused", d->read(d->ctx, PART_BYTES, back, 4096) != 0);
    check("a read across the end is refused", d->read(d->ctx, PART_BYTES - 4096, back, 8192) != 0);

    check("limit above the partition is refused", EFI_ERROR(rwy_fs_block_limit(&be, PART_BYTES + 4096)));
    check("limit to the store size", !EFI_ERROR(rwy_fs_block_limit(&be, PART_BYTES / 2)) && d->size == PART_BYTES / 2);
    check("a read past the limit is refused", d->read(d->ctx, PART_BYTES / 2, back, 4096) != 0);
    check("a write past the limit is refused", d->write(d->ctx, PART_BYTES / 2, buf, 4096) != 0);

    f->media.MediaId++;
    check("a changed MediaId fails reads", d->read(d->ctx, 0, back, 4096) != 0);
    check("a changed MediaId fails writes", d->write(d->ctx, 0, buf, 4096) != 0);
    f->media.MediaId--;

    f->flushes = 0;
    check("flush calls FlushBlocks", d->flush(d->ctx) == 0 && f->flushes == 1);
    f->fail_flush = TRUE;
    check("a failed FlushBlocks fails the flush", d->flush(d->ctx) != 0);
    f->fail_flush = FALSE;

    test_cache(f, d, buf, back);

    f->media.ReadOnly = TRUE;
    {
        rwy_backend_t ro;
        check("a read-only media opens", !EFI_ERROR(rwy_fs_block_backend(&f->proto, &ro)));
        check("a read-only media refuses writes", ro.dev.write(ro.dev.ctx, 0, buf, 4096) != 0);
        check("a read-only media reads", ro.dev.read(ro.dev.ctx, 0, back, 4096) == 0);
        rwy_fs_block_close(&ro);
    }
    rwy_fs_block_close(&be);
}

/* Pages, so that the buffers are 4096-byte aligned. */
static UINT8 *alloc_pages(UINTN bytes) {
    EFI_PHYSICAL_ADDRESS a = 0;

    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, bytes / 4096, &a))) return NULL;
    return (UINT8 *)(UINTN)a;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    static fake_t f;
    UINT8 *buf, *back;
    UINT32 sizes[] = { 512, 4096 };
    UINT32 aligns[] = { 0, 512 };

    util_init(ImageHandle, SystemTable);
    uprint("BLOCKTEST\n");
    f.data = alloc_pages(PART_BYTES);
    buf = alloc_pages(4 * 1048576 + 4096);
    back = alloc_pages(4 * 1048576 + 4096);
    if (!f.data || !buf || !back) {
        uprint("BLOCKTEST FAIL: no memory\n");
        failures++;
    } else {
        test_media_rules(&f);
        for (UINTN i = 0; i < 2; i++)
            for (UINTN j = 0; j < 2; j++)
                test_io(&f, sizes[i], aligns[j], buf, back);
    }
    uprintf("BLOCKTEST DONE failures=%d\n", failures);
    gBS->Stall(500000);
    gST->RuntimeServices->ResetSystem(EfiResetShutdown, failures ? EFI_ABORTED : EFI_SUCCESS, 0, NULL);
    return failures ? EFI_ABORTED : EFI_SUCCESS;
}
