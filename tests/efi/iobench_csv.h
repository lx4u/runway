/*
 * iobench_csv — the result rows of the benchmark, formatted without libc.
 * SPDX-License-Identifier: Apache-2.0
 * This header and its .c file build for the EFI application and for the host schema test.
 */
#ifndef IOBENCH_CSV_H
#define IOBENCH_CSV_H

#include <stddef.h>

/* Column order of a run row: run, disk, status, arch, build, bench_version, firmware, system_model,
 * devpath, media_id, disk_bytes, block_size, ioalign, write_cache, store_uuid,
 * store_kind, disk_set, config, started, finished, reason, tsc_hz, timing_note, aes. */
typedef struct {
    const char *run;
    const char *disk;
    const char *status;
    const char *arch;
    const char *build;
    const char *bench_version;
    const char *firmware;
    const char *system_model;
    const char *devpath;
    unsigned long long media_id;
    unsigned long long disk_bytes;
    unsigned long long block_size;
    unsigned long long ioalign;
    int write_cache;
    const char *store_uuid;
    const char *store_kind;
    const char *disk_set;
    const char *config;
    const char *started;
    const char *finished;
    const char *reason;
    unsigned long long tsc_hz; /* tick rate of the cycle counter that timed the samples */
    const char *timing_note;
    const char *aes; /* how the cipher of the encrypted store ran: hardware or portable */
} iobench_run_t;

/* Column order of a sample row: run, disk, dev, op, size, sample, seconds, bytes, calls_core, calls_dev,
 * us_per_call, flush_seconds, extents, encrypted, result. */
typedef struct {
    const char *run;
    const char *disk;
    const char *dev;
    const char *op;
    unsigned long long size;
    unsigned long long sample;
    const char *stat; /* "sample" for a timed sample, "median", "min" or "max" for a summary */
    unsigned long long seconds_us;
    unsigned long long bytes;
    unsigned long long calls_core;
    unsigned long long calls_dev;
    unsigned long long us_per_call_x100;
    unsigned long long flush_us;
    unsigned long long extents;
    int encrypted;
    const char *result;
} iobench_sample_t;

const char *iobench_runs_header(void);
const char *iobench_samples_header(void);

/* Format one row, with a trailing newline. Returns the length the row needs. A row that does not fit is
 * truncated and NUL terminated; the caller must compare the return with its capacity. */
size_t iobench_format_run(char *buf, size_t cap, const iobench_run_t *r);
size_t iobench_format_sample(char *buf, size_t cap, const iobench_sample_t *s);

#endif
