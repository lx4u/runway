/*
 * iobench.h — the devices iobench times and the call counters it prints.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IOBENCH_H
#define IOBENCH_H

#include <efi.h>
#include <efilib.h>
#include "iobench_csv.h"
#include "iobench_config.h"
#include "rwy_core.h"

#define AREA (16u * 1048576u)
#define BENCH_OBJECT "PERF/READ16M.BIN"
#define BENCH_FILE "BENCH.BIN"

/* Call sizes are counted in buckets: up to 4 KiB, 16 KiB, 64 KiB, 256 KiB, 1 MiB, and more. */
#define BENCH_BUCKETS 6

/* The slowest call seen: its time, its number in the run, and what it asked for. */
typedef struct {
    UINT64 ticks, index, lba, len;
} bench_slow_t;

/* Calls that reached the firmware, by size. */
typedef struct {
    UINT64 reads, writes, flushes, seeks;
    UINT64 read_ticks, write_ticks, flush_ticks; /* time inside the firmware calls */
    UINT64 read_size[BENCH_BUCKETS], write_size[BENCH_BUCKETS];
    bench_slow_t slow_read, slow_write; /* the slowest call of each direction */
} bench_counts_t;

struct bench_rwy;

typedef struct bench_dev {
    const char *name;
    EFI_STATUS (*write)(struct bench_dev *d, UINT64 off, UINTN len, void *buf);
    EFI_STATUS (*read)(struct bench_dev *d, UINT64 off, UINTN len, void *buf);
    EFI_STATUS (*flush)(struct bench_dev *d);
    EFI_BLOCK_IO_PROTOCOL *bio;
    EFI_FILE_HANDLE fh;
    UINT64 pos;  /* file position after the last call */
    UINT64 base; /* offset of the area on the device */
    struct bench_rwy *rwy; /* the store under test, NULL for a baseline */
    int encrypted;
} bench_dev_t;

/* The disk a report row describes. */
typedef struct {
    char devpath[160];
    char store_uuid[37];
    UINT64 media_id;
    UINT64 bytes;
    UINT32 block_size;
    UINT32 ioalign;
    int write_cache;
} bench_disk_info_t;

/* One timed sample: what ran, how much it moved and what it cost. */
typedef struct {
    const char *op;
    UINTN size;      /* run size of the calls, 0 when the op is not made of runs */
    UINT64 bytes;    /* bytes moved by the sample */
    UINT64 extents;  /* extents of the object read or written, 0 when not known */
    UINT64 seconds_us;
    UINT64 flush_us;
    UINT64 calls_core;
    UINT64 calls_dev;
    int ok;
} bench_measure_t;

/* iobench.c */
void bench_fill_pattern(UINT8 *buf, UINTN seed);
UINT64 bench_first_mismatch(const UINT8 *a, const UINT8 *b);
void bench_work_emit(const bench_dev_t *d, const char *op, UINT64 bytes, UINT64 extents,
                     UINT64 ticks, UINT64 flush_ticks, EFI_STATUS s);
/* iobench_cipher.c: encrypt and decrypt the area in memory (`want` is the content, `got` the scratch copy). Returns 1 when the round trip is exact. */
int bench_cipher_run(const UINT8 *want, UINT8 *got);
/* iobench_wpath.c: write the raw area in pieces of 64 and 128 KiB, step by step as the encrypted store does. Returns 1 when it ran. */
int bench_wpath_run(bench_dev_t *blk, UINT8 *want);
/* iobench_probe.c: read and write the raw area in the patterns of the stores, to find what delays a read. Returns 1 when it ran. */
int bench_probe_run(bench_dev_t *blk, UINT8 *want, UINT8 *got);
void bench_report(const char *dev, const char *op, UINTN run, UINT64 ticks, EFI_STATUS s);
void bench_sample_emit(const bench_dev_t *d, const char *op, UINTN run, UINT64 ticks,
                       UINT64 flush_ticks, int ok);
EFI_STATUS bench_esp_open(EFI_HANDLE ih, EFI_FILE_HANDLE *out_root);

/* iobench_result.c */
void bench_result_open(EFI_HANDLE ih, const bench_config_t *cfg);
void bench_result_close(void);
/* Save the console text of the run in the results store and on the ESP. */
void bench_result_log(EFI_HANDLE ih, const char *text, UINTN len);
void bench_result_disk_begin(const bench_disk_info_t *d);
/* The round the next samples belong to. Samples of a warm-up round are dropped. */
void bench_result_round(UINTN round, int warmup);
void bench_result_add_sample(const bench_dev_t *d, const bench_measure_t *m);
void bench_result_disk_done(const bench_disk_info_t *d, int ok, const char *reason);
void bench_result_skip(const bench_disk_info_t *d, const char *reason);
void bench_time_str(char *out, UINTN cap);

/* iobench_rwy.c: Runway on a partition (dek NULL: not encrypted) and Runway on RWYBENCH.DAT of the volume `root`. */
EFI_STATUS bench_rwy_block(bench_dev_t *d, EFI_HANDLE part, const UINT8 *dek);
EFI_STATUS bench_rwy_file(bench_dev_t *d, EFI_HANDLE esp, EFI_FILE_HANDLE root);
/* The size of the store, to see that it leaves the raw area alone. */
UINT64 bench_rwy_store_size(const bench_dev_t *d);
/* The store UUID as text, for the report. */
void bench_rwy_uuid(const bench_dev_t *d, char out[37]);
/* Read a small file from a mounted store. */
EFI_STATUS bench_rwy_read_path(bench_dev_t *d, const char *path, char *buf, UINTN cap, UINTN *len);
/* Close a file and free a mounted store. Safe on a zeroed device. */
void bench_dev_close(bench_dev_t *d);
/* Create the file of the timed runs, filled once and untimed, and read the large object of the store. */
EFI_STATUS bench_rwy_prepare(bench_dev_t *d, UINT8 *buf);
/* The calls counted since the last reset, reads and writes together. */
void bench_calls_all(const bench_dev_t *d, UINT64 *core, UINT64 *dev);
/* What the mount of the store cost: time, store calls and firmware calls. */
void bench_rwy_mount_cost(const bench_dev_t *d, UINT64 *ticks, UINT64 *core, UINT64 *dev);
/* The mounted store, for the workloads. */
rwy_store_t *bench_rwy_store(const bench_dev_t *d);
/* The boot (B1) and install (B2) workloads on a mounted store. B2 only when `install` is set. Returns 1 when all checks passed. */
int bench_work_run(bench_dev_t *d, int install, UINT8 *want, UINT8 *got);
/* Remove the files that earlier passes left in a mounted store, and commit. */
EFI_STATUS bench_work_clean(bench_dev_t *d);

/* Forget the calls counted so far; print the ones counted since. No-ops for a baseline. */
void bench_calls_reset(bench_dev_t *d);
void bench_calls_print(const bench_dev_t *d, const char *op, UINTN run);
/* A sample of the time the firmware calls took in the last timed run (op `read-fw` or `write-fw`). No-op for a baseline. */
void bench_fw_emit(const bench_dev_t *d, const char *op, UINTN run);
void bench_calls_get(const bench_dev_t *d, int writing, UINT64 *core, UINT64 *dev);

#endif
