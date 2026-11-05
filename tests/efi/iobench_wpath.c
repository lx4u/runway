/*
 * iobench_wpath — the cost of one call of the encrypted write, taken apart on the raw area of the disk.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The encrypted store writes pieces of at most 128 KiB: it encrypts a piece into its bounce buffer, which is only 4
 * byte aligned, and the block backend copies it to an aligned buffer when the firmware asks for IoAlign 8. Each cell
 * writes the 16 MiB area in pieces of one size, and each one adds a step of that path to the one before it.
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"
#include "iobench.h"
#include "rwy_crypto.h"
#include "rwy_format.h"

#define PIECE_MAX (RWY_CRYPTO_BOUNCE_BLOCKS * RWY_BLOCK_SIZE)

typedef enum { STEP_PLAIN, STEP_COPY, STEP_ENCRYPT, STEP_ENCRYPT_COPY } step_t;

/* The piece sizes and the names of the cells, by size and step. */
static const UINTN pieces[] = { 65536, PIECE_MAX };
static const char *const cell_name[2][4] = {
    { "plain-64k", "copy-64k", "encrypt-64k", "encrypt-copy-64k" },
    { "plain-128k", "copy-128k", "encrypt-128k", "encrypt-copy-128k" },
};

typedef struct {
    bench_dev_t *blk;
    rwy_xts_ctx_t xts;
    rwy_crypto_dev_ctx_t *pool;  /* its bounce buffer is the one the store has */
    UINT8 *aligned;              /* page aligned, PIECE_MAX bytes */
    const UINT8 *src;
} wpath_t;

static void encrypt_piece(wpath_t *w, UINT64 off, UINTN len, UINT8 *dst) {
    for (UINTN i = 0; i < len / RWY_BLOCK_SIZE; i++)
        rwy_xts_encrypt_sector(&w->xts, off / RWY_BLOCK_SIZE + i, w->src + off + i * RWY_BLOCK_SIZE,
                               dst + i * RWY_BLOCK_SIZE, RWY_BLOCK_SIZE);
}

/* Get the data of one piece ready as far as the step goes, then write it with one call. */
static EFI_STATUS write_piece(wpath_t *w, step_t step, UINT64 off, UINTN len) {
    const UINT8 *out = w->src + off;

    if (step == STEP_COPY) {
        memcpy(w->aligned, out, len);
        out = w->aligned;
    } else if (step == STEP_ENCRYPT) {
        encrypt_piece(w, off, len, w->aligned);
        out = w->aligned;
    } else if (step == STEP_ENCRYPT_COPY) {
        encrypt_piece(w, off, len, w->pool->bounce);
        memcpy(w->aligned, w->pool->bounce, len);
        out = w->aligned;
    }
    return w->blk->write(w->blk, off, len, (void *)out);
}

static void time_step(wpath_t *w, step_t step, UINTN size) {
    bench_dev_t d = { 0 };
    UINTN piece = pieces[size];
    UINT64 t0, ticks;
    EFI_STATUS s = EFI_SUCCESS;

    t0 = tsc_now();
    for (UINT64 off = 0; off < AREA && !EFI_ERROR(s); off += piece) s = write_piece(w, step, off, piece);
    ticks = tsc_now() - t0;
    if (!EFI_ERROR(s)) s = w->blk->flush(w->blk);
    d.name = "wpath";
    d.encrypted = step >= STEP_ENCRYPT;
    bench_work_emit(&d, cell_name[size][step], AREA, 0, ticks, 0, s);
}

int bench_wpath_run(bench_dev_t *blk, UINT8 *want) {
    wpath_t w = { .blk = blk, .src = want };
    UINT8 key[32];
    EFI_PHYSICAL_ADDRESS pages;

    for (UINTN i = 0; i < sizeof(key); i++) key[i] = (UINT8)(i * 7 + 1);
    rwy_xts_init_from_dek(&w.xts, key);
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, sizeof(*w.pool), (void **)&w.pool))) return 0;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, PIECE_MAX / 4096, &pages))) {
        gBS->FreePool(w.pool);
        return 0;
    }
    w.aligned = (UINT8 *)(UINTN)pages;
    bench_fill_pattern(want, 300);
    for (UINTN p = 0; p < sizeof(pieces) / sizeof(pieces[0]); p++)
        for (step_t s = STEP_PLAIN; s <= STEP_ENCRYPT_COPY; s++) time_step(&w, s, p);
    gBS->FreePages(pages, PIECE_MAX / 4096);
    gBS->FreePool(w.pool);
    return 1;
}
