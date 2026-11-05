/*
 * iobench_work — the boot (B1) and install (B2) workloads on a mounted store.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Each phase times one step with the call counters, checks the data and records one sample. A phase that
 * fails is recorded and the next one still runs: every phase works on its own file.
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"
#include "iobench.h"
#include "rwy_internal.h"

#define RUN 1048576u
#define STATE_SIZE 4096u
#define MIN_FRAG_EXTENTS 9
#define FRAG_FILE "PERF/FRAG.BIN"
#define FILL_FILE "PERF/FILL.BIN"
#define STATE_FILE "PERF/STATE.BIN"
#define INSTALL_FILE "PERF/INSTALL.BIN"
#define SEED_FRAG 200
#define SEED_STATE 201
#define SEED_INSTALL 202

static EFI_STATUS work_status(int rc) {
    return rc == RWY_OK ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

/* Write `len` bytes of `buf` to the object in runs of RUN. */
static EFI_STATUS write_runs(rwy_store_t *s, uint64_t oid, const UINT8 *buf, UINT64 len) {
    for (UINT64 off = 0; off < len; off += RUN)
        if (rwy_write(s, oid, off, buf + off, RUN) != RWY_OK) return EFI_DEVICE_ERROR;
    return EFI_SUCCESS;
}

static EFI_STATUS read_runs(rwy_store_t *s, uint64_t oid, UINT8 *buf, UINT64 len) {
    for (UINT64 off = 0; off < len; off += RUN) {
        size_t n = RUN;

        if (rwy_read(s, oid, off, buf + off, &n) != RWY_OK || n != RUN) return EFI_DEVICE_ERROR;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS create_file(rwy_store_t *s, const char *path, uint64_t *oid) {
    if (rwy_create(s, path, RWY_S_IFREG | 0644, 0, 0) != RWY_OK) return EFI_DEVICE_ERROR;
    return work_status(rwy_lookup(s, path, oid));
}

/* The extents of the committed object. */
static UINT64 extent_count(rwy_store_t *s, uint64_t oid) {
    rwy_extent_t list[RWY_MAX_EXTENTS];
    uint32_t n = 0;

    if (rwy_extents_load(s, &s->inodes[oid - 1], list, &n) != RWY_OK) return 0;
    return n;
}

static void emit_mount(bench_dev_t *d) {
    bench_measure_t m = { "mount", 0, 0, 0, 0, 0, 0, 0, 1 };
    UINT64 ticks;

    bench_rwy_mount_cost(d, &ticks, &m.calls_core, &m.calls_dev);
    m.seconds_us = (UINT64)(tsc_to_seconds(ticks) * 1e6 + 0.5);
    bench_result_add_sample(d, &m);
    uprintf("IOBENCH work %-8s %-9s: ok %llu us\n", d->name, "mount", (unsigned long long)m.seconds_us);
}

static int time_lookup(bench_dev_t *d) {
    uint64_t oid;
    UINT64 t0;
    EFI_STATUS st;

    bench_calls_reset(d);
    t0 = tsc_now();
    st = work_status(rwy_lookup(bench_rwy_store(d), BENCH_OBJECT, &oid));
    bench_work_emit(d, "lookup", 0, 0, tsc_now() - t0, 0, st);
    return !EFI_ERROR(st);
}

/* Write two files in turn, so the first one gets one extent per run. Untimed. */
static EFI_STATUS make_fragmented(bench_dev_t *d, UINT8 *want, uint64_t *oid) {
    rwy_store_t *s = bench_rwy_store(d);
    uint64_t fill;
    EFI_STATUS st = create_file(s, FRAG_FILE, oid);

    if (!EFI_ERROR(st)) st = create_file(s, FILL_FILE, &fill);
    bench_fill_pattern(want, SEED_FRAG);
    for (UINT64 off = 0; off < AREA && !EFI_ERROR(st); off += RUN) {
        st = work_status(rwy_write(s, *oid, off, want + off, RUN));
        if (!EFI_ERROR(st)) st = work_status(rwy_write(s, fill, off, want + off, RUN));
    }
    return EFI_ERROR(st) ? st : d->flush(d);
}

/* Read a payload that is cut into many extents, the way a loader reads a fragmented slot. */
static int time_fragmented_read(bench_dev_t *d, UINT8 *want, UINT8 *got) {
    rwy_store_t *s = bench_rwy_store(d);
    uint64_t oid = 0;
    UINT64 t0, ticks, extents = 0;
    EFI_STATUS st = make_fragmented(d, want, &oid);

    if (!EFI_ERROR(st)) {
        extents = extent_count(s, oid);
        bench_calls_reset(d);
        t0 = tsc_now();
        st = read_runs(s, oid, got, AREA);
        ticks = tsc_now() - t0;
        if (!EFI_ERROR(st) && bench_first_mismatch(want, got) != AREA) st = EFI_COMPROMISED_DATA;
        if (!EFI_ERROR(st) && extents < MIN_FRAG_EXTENTS) st = EFI_VOLUME_CORRUPTED;
    } else {
        ticks = 0;
    }
    bench_work_emit(d, "read-frag", AREA, extents, ticks, 0, st);
    return !EFI_ERROR(st);
}

/* One small state write and its commit; the commit is timed apart. */
static int time_state_write(bench_dev_t *d, UINT8 *want, UINT8 *got) {
    rwy_store_t *s = bench_rwy_store(d);
    uint64_t oid = 0;
    UINT64 t0, f0, ticks = 0, flush = 0;
    size_t n = STATE_SIZE;
    EFI_STATUS st = create_file(s, STATE_FILE, &oid);

    bench_fill_pattern(want, SEED_STATE);
    bench_calls_reset(d);
    t0 = tsc_now();
    if (!EFI_ERROR(st)) st = work_status(rwy_write(s, oid, 0, want, STATE_SIZE));
    f0 = tsc_now();
    if (!EFI_ERROR(st)) st = d->flush(d);
    flush = tsc_now() - f0;
    ticks = tsc_now() - t0;
    if (!EFI_ERROR(st)) st = work_status(rwy_read(s, oid, 0, got, &n));
    if (!EFI_ERROR(st) && (n != STATE_SIZE || CompareMem(want, got, STATE_SIZE))) st = EFI_COMPROMISED_DATA;
    bench_work_emit(d, "state", STATE_SIZE, 0, ticks, flush, st);
    return !EFI_ERROR(st);
}

/* Write a payload in runs of 1 MiB, commit it, read one small piece of metadata, read the payload back. */
static int time_install(bench_dev_t *d, UINT8 *want, UINT8 *got) {
    rwy_store_t *s = bench_rwy_store(d);
    rwy_attr_t a;
    uint64_t oid = 0;
    UINT64 t0, f0, flush = 0;
    EFI_STATUS st = create_file(s, INSTALL_FILE, &oid);
    int ok;

    bench_fill_pattern(want, SEED_INSTALL);
    bench_calls_reset(d);
    t0 = tsc_now();
    if (!EFI_ERROR(st)) st = write_runs(s, oid, want, AREA);
    f0 = tsc_now();
    if (!EFI_ERROR(st)) st = d->flush(d);
    flush = tsc_now() - f0;
    bench_work_emit(d, "install", AREA, EFI_ERROR(st) ? 0 : extent_count(s, oid), tsc_now() - t0, flush, st);
    ok = !EFI_ERROR(st);

    bench_calls_reset(d);
    t0 = tsc_now();
    st = work_status(rwy_stat(s, INSTALL_FILE, &a));
    bench_work_emit(d, "meta", 0, 0, tsc_now() - t0, 0, st);
    if (!EFI_ERROR(st) && a.size != AREA) st = EFI_COMPROMISED_DATA;
    ok &= !EFI_ERROR(st);

    if (ok) st = read_runs(s, oid, got, AREA);
    if (ok && (EFI_ERROR(st) || bench_first_mismatch(want, got) != AREA)) {
        uprintf("IOBENCH %s: installed payload differs\n", d->name);
        ok = 0;
    }
    return ok;
}

EFI_STATUS bench_work_clean(bench_dev_t *d) {
    static const char *const files[] = { BENCH_FILE, FRAG_FILE, FILL_FILE, STATE_FILE, INSTALL_FILE };
    rwy_store_t *s = bench_rwy_store(d);
    int rc = RWY_OK;

    for (UINTN i = 0; i < sizeof(files) / sizeof(files[0]) && (rc == RWY_OK || rc == RWY_ERR_NOENT); i++)
        rc = rwy_unlink(s, files[i]);
    if (rc == RWY_OK || rc == RWY_ERR_NOENT) return d->flush(d);
    return EFI_DEVICE_ERROR;
}

int bench_work_run(bench_dev_t *d, int install, UINT8 *want, UINT8 *got) {
    int ok;

    emit_mount(d);
    ok = time_lookup(d);
    ok &= time_fragmented_read(d, want, got);
    ok &= time_state_write(d, want, got);
    if (install) ok &= time_install(d, want, got);
    return ok;
}
