/*
 * rwy_blockio.c — A store on a raw partition, read and written through EFI_BLOCK_IO.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_fs.h"

/* Largest transfer that goes through the bounce buffer in one call. */
#define BLOCKIO_BOUNCE_SIZE (1024u * 1024u)
#define BLOCKIO_BOUNCE_PAGES (BLOCKIO_BOUNCE_SIZE / 4096u)

/* The core reads small pieces (an inode, a directory entry) in large numbers, and a firmware call
 * costs the same for any size. A read under BLOCKIO_CACHE_SMALL is served from a window of
 * BLOCKIO_CACHE_SIZE bytes that one call fills. */
#define BLOCKIO_CACHE_SIZE (64u * 1024u)
#define BLOCKIO_CACHE_PAGES (BLOCKIO_CACHE_SIZE / 4096u)
#define BLOCKIO_CACHE_SMALL (BLOCKIO_CACHE_SIZE / 4u)

typedef struct {
    EFI_BLOCK_IO_PROTOCOL *bio;
    UINT32 media_id;
    UINT32 block_size;
    UINT32 io_align;
    BOOLEAN read_only;
    UINT64 size;                /* bytes the core may use */
    UINT64 part_size;           /* bytes of the partition */
    void *bounce;               /* page-aligned buffer for partial block reads and for buffers that break IoAlign */
    UINT8 *cache;               /* read window, page aligned */
    UINT64 cache_off;
    size_t cache_len;           /* 0: nothing cached */
    rwy_dev_stats_t stats;      /* firmware calls, window hits and fills */
} blockio_t;

static BOOLEAN pow2(UINT32 v) {
    return v && !(v & (v - 1));
}

/* A transfer must stay inside the store and on the media it was opened with. */
static BOOLEAN request_ok(const blockio_t *b, UINT64 off, size_t len) {
    if (!b->bio->Media || b->bio->Media->MediaId != b->media_id) return FALSE;
    return off <= b->size && len <= b->size - off;
}

static BOOLEAN misaligned(const blockio_t *b, const void *buf) {
    return b->io_align > 1 && ((UINTN)buf % b->io_align) != 0;
}

static EFI_STATUS blocks(blockio_t *b, BOOLEAN write, UINT64 off, size_t len, void *p) {
    EFI_STATUS s;

    if (write) {
        b->stats.writes++;
        b->stats.write_bytes += len;
        s = b->bio->WriteBlocks(b->bio, b->media_id, off / b->block_size, len, p);
    } else {
        b->stats.reads++;
        b->stats.read_bytes += len;
        s = b->bio->ReadBlocks(b->bio, b->media_id, off / b->block_size, len, p);
    }
    return s;
}

/* Whole blocks: one firmware call per run, or per bounce-sized piece when the caller's buffer breaks IoAlign. */
static int transfer_whole(blockio_t *b, BOOLEAN write, UINT64 off, UINT8 *buf, size_t len) {
    while (len) {
        size_t n = len;
        UINT8 *p = buf;

        if (misaligned(b, buf)) {
            if (n > BLOCKIO_BOUNCE_SIZE) n = BLOCKIO_BOUNCE_SIZE;
            p = b->bounce;
            if (write) memcpy(p, buf, n);
        }
        if (EFI_ERROR(blocks(b, write, off, n, p))) return -1;
        if (!write && p != buf) memcpy(buf, p, n);
        off += n;
        buf += n;
        len -= n;
    }
    return 0;
}

/* Part of one block, for a read: read the block and copy the bytes out. */
static int read_part(blockio_t *b, UINT64 off, UINT8 *buf, size_t len) {
    UINT64 lo = off - off % b->block_size;

    if (EFI_ERROR(blocks(b, FALSE, lo, b->block_size, b->bounce))) return -1;
    memcpy(buf, (UINT8 *)b->bounce + (size_t)(off - lo), len);
    return 0;
}

/* Any offset and length: a partial block at each end, whole blocks between. */
static int read_any(blockio_t *b, UINT64 off, UINT8 *buf, size_t len) {
    size_t head = off % b->block_size, whole;

    if (head) {
        size_t n = b->block_size - head;

        if (n > len) n = len;
        if (read_part(b, off, buf, n)) return -1;
        off += n;
        buf += n;
        len -= n;
    }
    whole = len - len % b->block_size;
    if (whole && transfer_whole(b, FALSE, off, buf, whole)) return -1;
    off += whole;
    buf += whole;
    len -= whole;
    return len ? read_part(b, off, buf, len) : 0;
}

/* Serve a small read from the window, filling it with the aligned piece that holds the offset. */
static int read_cached(blockio_t *b, UINT64 off, UINT8 *buf, size_t len) {
    while (len) {
        size_t n;

        if (!b->cache_len || off < b->cache_off || off >= b->cache_off + b->cache_len) {
            UINT64 lo = off - off % BLOCKIO_CACHE_SIZE;
            size_t want = lo + BLOCKIO_CACHE_SIZE > b->part_size ? (size_t)(b->part_size - lo) : BLOCKIO_CACHE_SIZE;

            b->cache_len = 0;
            if (read_any(b, lo, b->cache, want)) return -1;
            b->cache_off = lo;
            b->cache_len = want;
            b->stats.window_fills++;
        } else {
            b->stats.window_hits++;
        }
        n = (size_t)(b->cache_off + b->cache_len - off);
        if (n > len) n = len;
        memcpy(buf, b->cache + (off - b->cache_off), n);
        off += n;
        buf += n;
        len -= n;
    }
    return 0;
}

static int block_pread(void *ctx, UINT64 off, void *buf, size_t len) {
    blockio_t *b = ctx;

    if (!request_ok(b, off, len)) return -1;
    if (len < BLOCKIO_CACHE_SMALL) return read_cached(b, off, buf, len);
    return read_any(b, off, buf, len);
}

static int block_pwrite(void *ctx, UINT64 off, const void *buf, size_t len) {
    blockio_t *b = ctx;

    if (b->read_only || !request_ok(b, off, len)) return -1;
    if (off % RWY_BLOCK_SIZE || len % RWY_BLOCK_SIZE) return -1;
    b->cache_len = 0;
    return transfer_whole(b, TRUE, off, (UINT8 *)buf, len);
}

static int block_flush(void *ctx) {
    blockio_t *b = ctx;

    if (b->read_only) return 0;
    b->stats.flushes++;
    return EFI_ERROR(b->bio->FlushBlocks(b->bio)) ? -1 : 0;
}

/* The partition must be a logical partition with a block size we can map to store blocks. */
static EFI_STATUS check_media(const EFI_BLOCK_IO_MEDIA *m) {
    if (!m || !m->MediaPresent || !m->LogicalPartition) return EFI_UNSUPPORTED;
    if (!pow2(m->BlockSize) || m->BlockSize < 512 || m->BlockSize > RWY_BLOCK_SIZE) return EFI_UNSUPPORTED;
    if (m->IoAlign > RWY_BLOCK_SIZE || (m->IoAlign > 1 && !pow2(m->IoAlign))) return EFI_UNSUPPORTED;
    if (m->LastBlock >= (~0ULL) / m->BlockSize) return EFI_UNSUPPORTED;
    if ((m->LastBlock + 1) * m->BlockSize < RWY_MIN_STORE_SIZE) return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}

EFI_STATUS rwy_fs_block_backend(EFI_BLOCK_IO_PROTOCOL *bio, rwy_backend_t *be) {
    const EFI_BLOCK_IO_MEDIA *m = bio ? bio->Media : NULL;
    EFI_PHYSICAL_ADDRESS pages = 0, cache = 0;
    blockio_t *b;
    EFI_STATUS s = check_media(m);

    if (EFI_ERROR(s)) return s;
    s = gBS->AllocatePool(EfiLoaderData, sizeof(*b), (void **)&b);
    if (EFI_ERROR(s)) return s;
    memset(b, 0, sizeof(*b));
    s = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, BLOCKIO_BOUNCE_PAGES, &pages);
    if (EFI_ERROR(s)) {
        gBS->FreePool(b);
        return s;
    }
    s = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, BLOCKIO_CACHE_PAGES, &cache);
    if (EFI_ERROR(s)) {
        gBS->FreePages(pages, BLOCKIO_BOUNCE_PAGES);
        gBS->FreePool(b);
        return s;
    }
    b->bounce = (void *)(UINTN)pages;
    b->cache = (UINT8 *)(UINTN)cache;
    b->bio = bio;
    b->media_id = m->MediaId;
    b->block_size = m->BlockSize;
    b->io_align = m->IoAlign;
    b->read_only = m->ReadOnly;
    b->part_size = b->size = (m->LastBlock + 1) * m->BlockSize;

    be->kind = RWY_BACKEND_BLOCK;
    be->dev.read = block_pread;
    be->dev.write = block_pwrite;
    be->dev.flush = block_flush;
    be->dev.ctx = b;
    be->dev.size = b->size;
    be->dev.block_size = RWY_BLOCK_SIZE;
    be->dev.stats = NULL;
    be->dev.dircache = NULL;
    be->stats = &b->stats;
    return EFI_SUCCESS;
}

EFI_STATUS rwy_fs_block_limit(rwy_backend_t *be, UINT64 size) {
    blockio_t *b = be->dev.ctx;

    if (size > b->part_size) return EFI_BUFFER_TOO_SMALL;
    b->cache_len = 0;
    b->size = be->dev.size = size;
    return EFI_SUCCESS;
}

void rwy_fs_block_close(rwy_backend_t *be) {
    blockio_t *b = be->dev.ctx;

    gBS->FreePages((EFI_PHYSICAL_ADDRESS)(UINTN)b->bounce, BLOCKIO_BOUNCE_PAGES);
    gBS->FreePages((EFI_PHYSICAL_ADDRESS)(UINTN)b->cache, BLOCKIO_CACHE_PAGES);
    gBS->FreePool(b);
    memset(be, 0, sizeof(*be));
}
