/*
 * iobench_linux — the reference numbers of a disk under Linux, for the benchmark report.
 * SPDX-License-Identifier: Apache-2.0
 *
 * It times the raw I/O the EFI application times in its "block" device (the same 16 MiB area of the IOBENCH
 * partition, the same run sizes) and the AES-XTS cipher in memory, and writes rows in the report schema:
 * devices linux-block and linux-cipher. Direct I/O keeps the page cache out of the numbers. The process runs on
 * CPU 0 and times the clock of the CPU (a chain of adds) before and after each cipher pass, because the cpufreq
 * governor changes it. With --perf-governor the cipher also runs with the governor set to performance, as the
 * device linux-cipher-perf.
 *
 * Usage: iobench_linux [--rounds N] [--warmup 0|1] [--no-direct] [--perf-governor] --out DIR DEVICE
 */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "rwy_crypto.h"
#include "rwy_format.h"

#define BASE (96ull << 20)     /* the raw area starts after the 96 MiB store, as in the EFI application */
#define AREA (16u << 20)
#define SECTOR RWY_BLOCK_SIZE

static const unsigned run_sizes[] = { 4096, 65536, 1048576, AREA };
/* Run sizes also written with O_DSYNC: the cost of a write that the device must make durable. */
static const unsigned sync_sizes[] = { 4096, 65536 };

typedef struct {
    int fd, fd_sync;
    unsigned rounds, warmup;
    FILE *runs, *samples;
    char run[40], disk[256];
    uint8_t *want, *got;
    int ok, perf, perf_ran;
} bench_t;

static uint64_t now_us(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static void utc(char *out, size_t cap) {
    time_t t = time(NULL);

    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
}

static void fill_pattern(uint8_t *buf, unsigned seed) {
    uint64_t *p = (uint64_t *)buf;

    for (uint64_t i = 0; i < AREA / 8; i++) p[i] = (i + seed * 0x10001ull + 1) * 0x9E3779B97F4A7C15ull;
}

/* One sample row, the schema of the EFI application (a value that is not known is 0). */
static void emit(bench_t *b, const char *dev, const char *op, unsigned size, unsigned sample, uint64_t us,
                 uint64_t flush_us, int enc, int ok) {
    uint64_t calls = size ? AREA / size : 0;
    double per_call = calls ? (double)us / (double)calls : 0;

    fprintf(b->samples, "%s,\"%s\",%s,%s,%u,%u,sample,%llu.%06llu,%u,0,%llu,%.2f,%llu.%06llu,0,%d,%s\n", b->run, b->disk,
            dev, op, size, sample, (unsigned long long)(us / 1000000u), (unsigned long long)(us % 1000000u), AREA,
            (unsigned long long)calls, per_call, (unsigned long long)(flush_us / 1000000u),
            (unsigned long long)(flush_us % 1000000u), enc, ok ? "ok" : "failed");
    printf("IOBENCH linux %-12s %-7s %5u KiB: %.3f s  %.1f MiB/s\n", dev, op, size / 1024, (double)us / 1e6,
           us ? 16.0 * 1e6 / (double)us : 0.0);
}

static int xfer(int fd, int writing, unsigned run, uint8_t *buf) {
    for (unsigned off = 0; off < AREA; off += run) {
        ssize_t n = writing ? pwrite(fd, buf + off, run, (off_t)(BASE + off))
                            : pread(fd, buf + off, run, (off_t)(BASE + off));

        if (n != (ssize_t)run) return -1;
    }
    return 0;
}

/* Write the area in runs of `run` bytes and flush it, then read it back and compare. */
static void io_cell(bench_t *b, unsigned run, unsigned sample, int timed) {
    uint64_t t0, t1, t2, t3;
    int ok_w, ok_r, same;

    fill_pattern(b->want, run);
    t0 = now_us();
    ok_w = xfer(b->fd, 1, run, b->want) == 0;
    t1 = now_us();
    ok_w = ok_w && fdatasync(b->fd) == 0;
    t2 = now_us();
    ok_r = ok_w && xfer(b->fd, 0, run, b->got) == 0;
    t3 = now_us();
    same = ok_r && memcmp(b->want, b->got, AREA) == 0;
    if (!ok_w || !same) b->ok = 0;
    if (!timed) return;
    emit(b, "linux-block", "write", run, sample, t2 - t0, t2 - t1, 0, ok_w);
    emit(b, "linux-block", "read", run, sample, t3 - t2, 0, 0, same);
}

/* Write the area in runs of `run` bytes, each one durable when the call returns (O_DSYNC). */
static void sync_cell(bench_t *b, unsigned run, unsigned sample, int timed) {
    uint64_t t0, t1;
    int ok;

    fill_pattern(b->want, run);
    t0 = now_us();
    ok = xfer(b->fd_sync, 1, run, b->want) == 0;
    t1 = now_us();
    if (!ok) b->ok = 0;
    if (timed) emit(b, "linux-block", "write-sync", run, sample, t1 - t0, 0, 0, ok);
}

/* The clock of the CPU in MHz: a chain of dependent register adds runs at one add per cycle (an add of a constant
 * can be folded by the CPU, so it would run faster than the clock). */
static unsigned spin_mhz(uint64_t *us_out) {
    const uint64_t loops = 4u << 20;
    uint64_t x = 1, y = 3, t0 = now_us(), us;

    __asm__ volatile ("" : "+r"(y));

    for (uint64_t i = 0; i < loops; i++) {
        x += y; __asm__ volatile ("" : "+r"(x));
        x += y; __asm__ volatile ("" : "+r"(x));
        x += y; __asm__ volatile ("" : "+r"(x));
        x += y; __asm__ volatile ("" : "+r"(x));
        x += y; __asm__ volatile ("" : "+r"(x));
        x += y; __asm__ volatile ("" : "+r"(x));
        x += y; __asm__ volatile ("" : "+r"(x));
        x += y; __asm__ volatile ("" : "+r"(x));
    }
    us = now_us() - t0;
    if (us_out) *us_out = us;
    return us ? (unsigned)(loops * 8 / us) : 0;
}

/* A clock row: calls_core holds the MHz that the add chain reached. */
static void emit_clock(bench_t *b, const char *dev, const char *op, unsigned sample, unsigned mhz, uint64_t us) {
    fprintf(b->samples, "%s,\"%s\",%s,%s,0,%u,sample,%llu.%06llu,%u,%u,0,0.00,0.000000,0,1,ok\n", b->run, b->disk, dev,
            op, sample, (unsigned long long)(us / 1000000u), (unsigned long long)(us % 1000000u), AREA, mhz);
    printf("IOBENCH linux %-16s %-13s: %u MHz\n", dev, op, mhz);
}

/* The AES-XTS cipher alone: encrypt and decrypt the area in 4 KiB sectors, as the encrypted store does. The clock
 * is measured before the pass and right after each pass, as the governor may change it while the pass runs. */
static void cipher_cell(bench_t *b, const char *dev, unsigned sample, int timed) {
    rwy_xts_ctx_t ctx;
    uint8_t key[32];
    uint64_t t0, t1, t2, t3, spin_start, spin_enc, spin_dec;
    unsigned mhz_start, mhz_enc, mhz_dec;
    int same;

    for (unsigned i = 0; i < sizeof(key); i++) key[i] = (uint8_t)(i * 7 + 1);
    rwy_xts_init_from_dek(&ctx, key);
    fill_pattern(b->want, 0);
    memcpy(b->got, b->want, AREA);
    mhz_start = spin_mhz(&spin_start);
    t0 = now_us();
    for (unsigned i = 0; i < AREA / SECTOR; i++)
        rwy_xts_encrypt_sector(&ctx, i, b->got + (size_t)i * SECTOR, b->got + (size_t)i * SECTOR, SECTOR);
    t1 = now_us();
    mhz_enc = spin_mhz(&spin_enc);
    t2 = now_us();
    for (unsigned i = 0; i < AREA / SECTOR; i++)
        rwy_xts_decrypt_sector(&ctx, i, b->got + (size_t)i * SECTOR, b->got + (size_t)i * SECTOR, SECTOR);
    t3 = now_us();
    mhz_dec = spin_mhz(&spin_dec);
    same = memcmp(b->want, b->got, AREA) == 0;
    if (!same) b->ok = 0;
    if (!timed) return;
    emit(b, dev, "encrypt", 0, sample, t1 - t0, 0, 1, same);
    emit(b, dev, "decrypt", 0, sample, t3 - t2, 0, 1, same);
    emit_clock(b, dev, "start-clock", sample, mhz_start, spin_start);
    emit_clock(b, dev, "encrypt-clock", sample, mhz_enc, spin_enc);
    emit_clock(b, dev, "decrypt-clock", sample, mhz_dec, spin_dec);
}

#define GOVERNOR_FILE "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"

/* Set the governor of CPU 0; `old` gets the one that was set. Returns -1 when it cannot be read or written. */
static int governor_set(const char *name, char *old, size_t cap) {
    FILE *f = fopen(GOVERNOR_FILE, "r");
    int rc;

    if (!f) return -1;
    rc = fgets(old, (int)cap, f) ? 0 : -1;
    fclose(f);
    old[strcspn(old, "\n")] = 0;
    if (rc) return -1;
    f = fopen(GOVERNOR_FILE, "w");
    if (!f) return -1;
    rc = fputs(name, f) < 0;
    return fclose(f) || rc ? -1 : 0;
}

/* The cipher again with the governor set to performance, then the governor as it was. */
static void perf_cipher_passes(bench_t *b) {
    char old[64];

    if (governor_set("performance", old, sizeof(old))) {
        fprintf(stderr, "iobench_linux: cannot set the performance governor\n");
        return;
    }
    b->perf_ran = 1;
    for (unsigned pass = 0; pass <= b->rounds; pass++)
        cipher_cell(b, "linux-cipher-perf", pass ? pass - 1 : 0, pass != 0);
    governor_set(old, old, sizeof(old));
}

static void run_passes(bench_t *b) {
    cpu_set_t cpu0;

    CPU_ZERO(&cpu0);
    CPU_SET(0, &cpu0);
    sched_setaffinity(0, sizeof(cpu0), &cpu0);
    for (unsigned pass = b->warmup ? 0 : 1; pass <= b->rounds; pass++) {
        int timed = pass != 0;
        unsigned sample = timed ? pass - 1 : 0;

        for (unsigned i = 0; i < sizeof(run_sizes) / sizeof(run_sizes[0]); i++) io_cell(b, run_sizes[i], sample, timed);
        for (unsigned i = 0; i < sizeof(sync_sizes) / sizeof(sync_sizes[0]); i++)
            sync_cell(b, sync_sizes[i], sample, timed);
        cipher_cell(b, "linux-cipher", sample, timed);
    }
    if (b->perf) perf_cipher_passes(b);
}

static uint64_t device_bytes(int fd) {
    struct stat st;
    uint64_t n = 0;

    if (fstat(fd, &st)) return 0;
    if (S_ISBLK(st.st_mode)) return ioctl(fd, BLKGETSIZE64, &n) ? 0 : n;
    return (uint64_t)st.st_size;
}

static void product_name(char *out, size_t cap) {
    FILE *f = fopen("/sys/class/dmi/id/product_name", "r");

    snprintf(out, cap, "-");
    if (!f) return;
    if (fgets(out, (int)cap, f)) out[strcspn(out, "\n")] = 0;
    fclose(f);
}

static void headers(bench_t *b) {
    fputs("run,disk,status,arch,build,bench_version,firmware,system_model,devpath,media_id,disk_bytes,block_size,"
          "ioalign,write_cache,store_uuid,store_kind,disk_set,config,started,finished,reason,tsc_hz,timing_note,aes\n",
          b->runs);
    fputs("run,disk,dev,op,size,sample,stat,seconds,bytes,calls_core,calls_dev,us_per_call,flush_seconds,extents,"
          "encrypted,result\n", b->samples);
}

static void run_row(bench_t *b, uint64_t bytes, const char *started) {
    struct utsname u;
    char finished[32], product[96];

    uname(&u);
    utc(finished, sizeof(finished));
    product_name(product, sizeof(product));
    fprintf(b->runs, "%s,\"%s\",%s,%s,-,linux1,linux %s,\"%s\",\"%s\",0,%llu,512,0,0,-,raw,target,"
            "rounds=%u warmup=%u payload_mib=16 devices=linux-block|linux-cipher%s,%s,%s,,0,"
            "\"Linux direct I/O in the provisioning slot; the page cache is not used\",%s\n", b->run, b->disk,
            b->ok ? "ok" : "failed", u.machine, u.release, product, b->disk, (unsigned long long)bytes, b->rounds,
            b->warmup, b->perf_ran ? "|linux-cipher-perf" : "", started, finished,
            rwy_crypto_hw_aes() ? "hardware" : "portable");
}

static int open_files(bench_t *b, const char *dir, const char *dev, int direct) {
    char path[512];

    b->fd = open(dev, O_RDWR | (direct ? O_DIRECT : 0));
    b->fd_sync = open(dev, O_RDWR | O_DSYNC | (direct ? O_DIRECT : 0));
    if (b->fd < 0 || b->fd_sync < 0) { perror(dev); return -1; }
    snprintf(path, sizeof(path), "%s/RUNS.CSV", dir);
    b->runs = fopen(path, "w");
    snprintf(path, sizeof(path), "%s/SAMPLES.CSV", dir);
    b->samples = fopen(path, "w");
    if (!b->runs || !b->samples) { perror(dir); return -1; }
    return posix_memalign((void **)&b->want, 4096, AREA) || posix_memalign((void **)&b->got, 4096, AREA) ? -1 : 0;
}

static int parse(int argc, char **argv, bench_t *b, const char **dir, const char **dev, int *direct) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rounds") && i + 1 < argc) b->rounds = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) b->warmup = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-direct")) *direct = 0;
        else if (!strcmp(argv[i], "--perf-governor")) b->perf = 1;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) *dir = argv[++i];
        else if (argv[i][0] != '-' && !*dev) *dev = argv[i];
        else return -1;
    }
    return *dir && *dev && b->rounds >= 1 ? 0 : -1;
}

int main(int argc, char **argv) {
    bench_t b = { .rounds = 1, .warmup = 1, .ok = 1 };
    const char *dir = NULL, *dev = NULL;
    char started[32];
    uint64_t bytes;
    int direct = 1;

    if (parse(argc, argv, &b, &dir, &dev, &direct)) {
        fprintf(stderr, "usage: %s [--rounds N] [--warmup 0|1] [--no-direct] [--perf-governor] --out DIR DEVICE\n",
                argv[0]);
        return 2;
    }
    if (open_files(&b, dir, dev, direct)) return 1;
    bytes = device_bytes(b.fd);
    if (bytes < BASE + AREA) { fprintf(stderr, "%s: smaller than the benchmark area\n", dev); return 1; }
    utc(started, sizeof(started));
    snprintf(b.run, sizeof(b.run), "linux-%s", started);
    snprintf(b.disk, sizeof(b.disk), "%s", dev);
    headers(&b);
    run_passes(&b);
    run_row(&b, bytes, started);
    fclose(b.runs);
    fclose(b.samples);
    close(b.fd);
    close(b.fd_sync);
    printf("IOBENCH linux %s\n", b.ok ? "DONE" : "FAIL");
    return b.ok ? 0 : 1;
}
