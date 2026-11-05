/*
 * iobench_config — the settings the benchmark reads from IOBENCH.CFG.
 * SPDX-License-Identifier: Apache-2.0
 * The parser is plain C so the host test can use it too.
 */
#ifndef IOBENCH_CONFIG_H
#define IOBENCH_CONFIG_H

#include <stddef.h>

#define BENCH_LABEL_MAX 32
#define BENCH_CONFIG_MAX 256

typedef enum {
    BENCH_RESULT_AUTO,
    BENCH_RESULT_STORE,
    BENCH_RESULT_ESP,
    BENCH_RESULT_SERIAL
} bench_result_mode_t;

enum {
    BENCH_DEV_BLOCK = 1u << 0,
    BENCH_DEV_FILE = 1u << 1,
    BENCH_DEV_RWY_BLOCK = 1u << 2,
    BENCH_DEV_RWY_FILE = 1u << 3,
    BENCH_DEV_RWY_ENC = 1u << 4,
    BENCH_DEV_ALL = (1u << 5) - 1,
    BENCH_DEV_PROBE = 1u << 5, /* a diagnosis, never part of "all" */
};

typedef enum {
    BENCH_DISKS_INTERNAL,
    BENCH_DISKS_ALL,
    BENCH_DISKS_BOOT
} bench_disks_t;

/* When the application waits for a key before it powers off, so that the screen can be read. */
typedef enum {
    BENCH_PAUSE_NEVER,
    BENCH_PAUSE_FAIL,
    BENCH_PAUSE_ALWAYS
} bench_pause_t;

typedef struct {
    unsigned layout;
    char label[BENCH_LABEL_MAX];
    unsigned rounds;
    int warmup;
    unsigned payload_mib;
    unsigned devices;
    bench_disks_t disks;
    int include_boot;
    bench_result_mode_t result;
    bench_pause_t pause;
} bench_config_t;

void bench_config_defaults(bench_config_t *cfg);
int bench_config_parse(const char *text, bench_config_t *cfg);
size_t bench_config_text(char *out, size_t cap, const bench_config_t *cfg);
const char *bench_result_name(bench_result_mode_t mode);
const char *bench_config_disk_set_name(const bench_config_t *cfg);

#endif
