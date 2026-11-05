/*
 * iobench_cipher — the speed of the AES-XTS cipher alone, on memory. It tells the cost of the cipher apart
 * from the cost of the device when the encrypted store is slower than the plain one.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Four more cells tell where the time of a write to the encrypted store goes: the cipher on one sector that stays
 * in the cache, the cipher into a bounce buffer from the pool (as the store has one), the copy into that buffer
 * alone, and a plain copy of the area. The CPU clock is counted by a chain of adds as well, to check that method,
 * which the Linux reference uses, against APERF and MPERF.
 *
 * It also tells what the CPU did meanwhile. On x86_64 the ratio of the APERF and MPERF counters is the clock
 * against the base clock, and the time of each sector shows whether a slow pass is slow all along or in bursts.
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"
#include "iobench.h"
#include "rwy_crypto.h"
#include "rwy_format.h"

#define SECTORS (AREA / RWY_BLOCK_SIZE)
#define MSR_MPERF 0xE7u
#define MSR_APERF 0xE8u

static UINT64 sector_ticks[SECTORS];

#if defined(__x86_64__)
static int clock_counters_present(void) {
    UINT32 eax, ebx, ecx, edx;

    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0), "c"(0));
    if (eax < 6) return 0;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(6), "c"(0));
    return ecx & 1;
}

static UINT64 read_msr(UINT32 msr) {
    UINT32 lo, hi;

    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((UINT64)hi << 32) | lo;
}
#else
static int clock_counters_present(void) { return 0; }
static UINT64 read_msr(UINT32 msr) { (void)msr; return 0; }
#endif

typedef struct {
    UINT64 ticks, aperf, mperf;
} pass_t;

/* Run the cipher over the area, a sector at a time, and note the time of each sector. */
static void cipher_pass(rwy_xts_ctx_t *ctx, UINT8 *buf, int encrypt, int counters, pass_t *p) {
    UINT64 start, prev, now, a0 = 0, m0 = 0;

    if (counters) {
        m0 = read_msr(MSR_MPERF);
        a0 = read_msr(MSR_APERF);
    }
    start = prev = tsc_now();
    for (UINTN i = 0; i < SECTORS; i++) {
        UINT8 *sector = buf + i * RWY_BLOCK_SIZE;

        if (encrypt) rwy_xts_encrypt_sector(ctx, i, sector, sector, RWY_BLOCK_SIZE);
        else rwy_xts_decrypt_sector(ctx, i, sector, sector, RWY_BLOCK_SIZE);
        now = tsc_now();
        sector_ticks[i] = now - prev;
        prev = now;
    }
    p->ticks = prev - start;
    if (counters) {
        p->aperf = read_msr(MSR_APERF) - a0;
        p->mperf = read_msr(MSR_MPERF) - m0;
    }
}

/* Print the fastest and slowest sector, and how many took more than 2, 4 and 8 times the fastest. */
static void print_spread(const char *op, const pass_t *p) {
    UINT64 min = ~0ull, max = 0, over[3] = { 0, 0, 0 };

    for (UINTN i = 0; i < SECTORS; i++) {
        if (sector_ticks[i] < min) min = sector_ticks[i];
        if (sector_ticks[i] > max) max = sector_ticks[i];
    }
    for (UINTN i = 0; i < SECTORS; i++)
        for (UINTN k = 0; k < 3; k++)
            if (sector_ticks[i] > min << (k + 1)) over[k]++;
    uprintf("IOBENCH cipher %-7s: sector %llu to %llu us, over 2x %llu, 4x %llu, 8x %llu", op,
            (unsigned long long)(tsc_to_seconds(min) * 1e6), (unsigned long long)(tsc_to_seconds(max) * 1e6),
            (unsigned long long)over[0], (unsigned long long)over[1], (unsigned long long)over[2]);
    if (p->mperf)
        uprintf(", clock %llu MHz",
                (unsigned long long)((double)p->aperf / (double)p->mperf / tsc_to_seconds(1000000)));
    uprintf("\n");
}

/* The clock row: the time of the pass, with the APERF delta in calls_core and the MPERF delta in calls_dev. */
static void emit_clock(const bench_dev_t *d, const char *op, const pass_t *p, int ok) {
    bench_measure_t m = { op, 0, AREA, 0, (UINT64)(tsc_to_seconds(p->ticks) * 1e6 + 0.5), 0, p->aperf, p->mperf, ok };

    bench_result_add_sample(d, &m);
}

/* The CPU clock in MHz: a chain of dependent register adds runs at one add per cycle. */
static UINT64 spin_mhz(void) {
    const UINT64 loops = 4u << 20;
    UINT64 x = 1, y = 3, t0, ticks;

    __asm__ volatile ("" : "+r"(y));
    t0 = tsc_now();
    for (UINT64 i = 0; i < loops; i++) {
        x += y; __asm__ volatile ("" : "+r"(x));
        x += y; __asm__ volatile ("" : "+r"(x));
        x += y; __asm__ volatile ("" : "+r"(x));
        x += y; __asm__ volatile ("" : "+r"(x));
        x += y; __asm__ volatile ("" : "+r"(x));
        x += y; __asm__ volatile ("" : "+r"(x));
        x += y; __asm__ volatile ("" : "+r"(x));
        x += y; __asm__ volatile ("" : "+r"(x));
    }
    ticks = tsc_now() - t0;
    return (UINT64)((double)(loops * 8) * 1e-6 / tsc_to_seconds(ticks));
}

/* The same sector again and again: the cipher with its data in the cache. */
static void time_cache(const bench_dev_t *d, rwy_xts_ctx_t *ctx, UINT8 *buf) {
    UINT64 t0 = tsc_now();

    for (UINTN i = 0; i < SECTORS; i++) rwy_xts_encrypt_sector(ctx, i, buf, buf, RWY_BLOCK_SIZE);
    bench_work_emit(d, "encrypt-cache", AREA, 0, tsc_now() - t0, 0, EFI_SUCCESS);
}

/* The path of an encrypted write: a piece of the area goes through the cipher into the bounce buffer of the store.
 * The buffer comes from the pool, in a context of the size the store uses. */
static void time_bounce(const bench_dev_t *d, rwy_xts_ctx_t *ctx, const UINT8 *src, UINT8 *copy) {
    rwy_crypto_dev_ctx_t *c;
    UINT64 t0, t1, t2;

    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, sizeof(*c), (void **)&c))) return;
    uprintf("IOBENCH cipher bounce at 0x%llx\n", (unsigned long long)(UINTN)c->bounce);
    t0 = tsc_now();
    for (UINTN i = 0; i < SECTORS; i += RWY_CRYPTO_BOUNCE_BLOCKS)
        for (UINTN k = 0; k < RWY_CRYPTO_BOUNCE_BLOCKS; k++)
            rwy_xts_encrypt_sector(ctx, i + k, src + (i + k) * RWY_BLOCK_SIZE, c->bounce + k * RWY_BLOCK_SIZE,
                                   RWY_BLOCK_SIZE);
    t1 = tsc_now();
    for (UINTN i = 0; i < SECTORS; i += RWY_CRYPTO_BOUNCE_BLOCKS)
        memcpy(c->bounce, src + i * RWY_BLOCK_SIZE, sizeof(c->bounce));
    t2 = tsc_now();
    bench_work_emit(d, "encrypt-bounce", AREA, 0, t1 - t0, 0, EFI_SUCCESS);
    bench_work_emit(d, "copy-bounce", AREA, 0, t2 - t1, 0, EFI_SUCCESS);
    t0 = tsc_now();
    memcpy(copy, src, AREA);
    bench_work_emit(d, "copy-area", AREA, 0, tsc_now() - t0, 0, EFI_SUCCESS);
    gBS->FreePool(c);
}

int bench_cipher_run(const UINT8 *want, UINT8 *got) {
    bench_dev_t d = { 0 };
    rwy_xts_ctx_t ctx;
    UINT8 key[32];
    pass_t enc = { 0, 0, 0 }, dec = { 0, 0, 0 };
    int counters = clock_counters_present();
    EFI_STATUS s;
    int ok;

    for (UINTN i = 0; i < sizeof(key); i++) key[i] = (UINT8)(i * 7 + 1);
    rwy_xts_init_from_dek(&ctx, key);
    memcpy(got, want, AREA);
    cipher_pass(&ctx, got, 1, counters, &enc);
    print_spread("encrypt", &enc);
    cipher_pass(&ctx, got, 0, counters, &dec);
    print_spread("decrypt", &dec);
    ok = bench_first_mismatch(want, got) == AREA;
    s = ok ? EFI_SUCCESS : EFI_VOLUME_CORRUPTED;
    d.name = "cipher";
    d.encrypted = 1;
    bench_work_emit(&d, "encrypt", AREA, 0, enc.ticks, 0, s);
    bench_work_emit(&d, "decrypt", AREA, 0, dec.ticks, 0, s);
    if (counters) {
        emit_clock(&d, "encrypt-clock", &enc, ok);
        emit_clock(&d, "decrypt-clock", &dec, ok);
    }
    uprintf("IOBENCH cipher add chain: %llu MHz\n", (unsigned long long)spin_mhz());
    time_cache(&d, &ctx, got);
    time_bounce(&d, &ctx, want, got);
    return ok;
}
