/*
 * iobench_probe — find what delays a read on the raw area of the disk.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Some disks (a RAID volume behind the firmware's driver, for one) take about a second more for a run of reads after
 * a run of small writes. Each cell reads or writes the 16 MiB area in calls of one size and prints its slowest call and
 * the number of calls over 1 ms, so that a delay of one call can be told from a cost spread over all of them. The cells
 * vary what came before the read (nothing, small writes, a second read, a pause), the place it reads (the store region
 * of the partition) and the idle time between the calls (the store spends some on each window it fills).
 */
#include <efi.h>
#include <efilib.h>
#include "efi_util.h"
#include "iobench.h"

#define PAUSE_US 2000000u
#define SLOW_US 1000u
#define STORE_REGION (32ull * 1048576u)

typedef struct {
    bench_dev_t *blk;
    UINT8 *want;
    UINT8 *got;
    int ok;
} probe_t;

static UINT64 us_to_ticks(UINT64 us) {
    return us * (UINT64)(1.0 / tsc_to_seconds(1000000));
}

static void wait_ticks(UINT64 ticks) {
    UINT64 t0 = tsc_now();

    while (tsc_now() - t0 < ticks) {
    }
}

/*
 * One pass over the area in calls of `run` bytes, `gap_us` of idle time after each call. A write pass flushes after its
 * timed part. The time reported is the time inside the calls.
 */
static void pass(probe_t *p, const char *cell, int write, UINTN run, UINT64 gap_us) {
    bench_dev_t d = { 0 };
    UINT64 t0, ticks = 0, flush_ticks = 0, worst = 0, worst_at = 0, calls = 0, over = 0;
    UINT64 slow = us_to_ticks(SLOW_US), gap = us_to_ticks(gap_us);
    EFI_STATUS s = EFI_SUCCESS;

    for (UINT64 off = 0; off < AREA && !EFI_ERROR(s); off += run, calls++) {
        UINT64 c0 = tsc_now(), took;

        s = write ? p->blk->write(p->blk, off, run, p->want + off) : p->blk->read(p->blk, off, run, p->got + off);
        took = tsc_now() - c0;
        ticks += took;
        if (took > worst) {
            worst = took;
            worst_at = calls;
        }
        if (took > slow) over++;
        if (gap) wait_ticks(gap);
    }
    if (write && !EFI_ERROR(s)) {
        t0 = tsc_now();
        s = p->blk->flush(p->blk);
        flush_ticks = tsc_now() - t0;
    }
    d.name = "probe";
    bench_work_emit(&d, cell, AREA, 0, ticks, flush_ticks, s);
    uprintf("IOBENCH probe %-18s slowest call %llu of %llu: %llu us, %llu over %u us\n", cell,
            (unsigned long long)worst_at, (unsigned long long)calls,
            (unsigned long long)(tsc_to_seconds(worst) * 1e6 + 0.5), (unsigned long long)over, SLOW_US);
    if (EFI_ERROR(s)) p->ok = 0;
}

/* The same reads with more and more idle time between the calls. */
static void gap_cells(probe_t *p) {
    static const struct {
        const char *cell;
        UINT64 gap_us;
    } gaps[] = { { "r64k-gap-0us", 0 },     { "r64k-gap-20us", 20 },   { "r64k-gap-100us", 100 },
                 { "r64k-gap-500us", 500 }, { "r64k-gap-2000us", 2000 } };

    for (UINTN i = 0; i < sizeof(gaps) / sizeof(gaps[0]); i++) pass(p, gaps[i].cell, 0, 65536, gaps[i].gap_us);
}

/* Reads the store region of the partition, where the stores keep their files, in the same calls. */
static void store_region_cell(probe_t *p) {
    bench_dev_t *raw = p->blk;
    bench_dev_t store = *raw;

    store.base = STORE_REGION;
    p->blk = &store;
    pass(p, "r64k-store-region", 0, 65536, 0);
    p->blk = raw;
}

int bench_probe_run(bench_dev_t *blk, UINT8 *want, UINT8 *got) {
    probe_t p = { blk, want, got, 1 };

    bench_fill_pattern(want, 400);
    pass(&p, "r64k-first", 0, 65536, 0);
    pass(&p, "w4k", 1, 4096, 0);
    pass(&p, "r64k-after-w4k", 0, 65536, 0);
    pass(&p, "r64k-again", 0, 65536, 0);
    pass(&p, "r4k-after-r64k", 0, 4096, 0);
    pass(&p, "w4k-second", 1, 4096, 0);
    gBS->Stall(PAUSE_US);
    pass(&p, "r64k-after-pause", 0, 65536, 0);
    pass(&p, "w64k", 1, 65536, 0);
    pass(&p, "r64k-after-w64k", 0, 65536, 0);
    gap_cells(&p);
    store_region_cell(&p);
    return p.ok;
}
