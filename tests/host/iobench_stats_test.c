/* iobench_stats_test — check the median, lowest and highest sample of a cell. */
/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>
#include "../efi/iobench_stats.h"

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1; \
        } \
    } while (0)

static iobench_sample_t make(const char *dev, const char *op, unsigned long long size,
                             unsigned long long sample, unsigned long long us, const char *result) {
    iobench_sample_t s = { 0 };

    s.dev = dev;
    s.op = op;
    s.size = size;
    s.sample = sample;
    s.stat = "sample";
    s.seconds_us = us;
    s.calls_core = us + 1;
    s.result = result;
    return s;
}

static int check_odd_count(void) {
    iobench_sample_t in[] = {
        make("block", "write", 4096, 0, 300, "ok"),
        make("block", "write", 4096, 1, 100, "ok"),
        make("block", "write", 4096, 2, 200, "ok"),
    };
    iobench_sample_t out[8];

    CHECK(iobench_stats_summarize(in, 3, out, 8) == 3);
    CHECK(strcmp(out[0].stat, "median") == 0 && out[0].seconds_us == 200 && out[0].sample == 2);
    CHECK(strcmp(out[1].stat, "min") == 0 && out[1].seconds_us == 100 && out[1].sample == 1);
    CHECK(strcmp(out[2].stat, "max") == 0 && out[2].seconds_us == 300 && out[2].sample == 0);
    CHECK(out[0].calls_core == 201);
    return 0;
}

static int check_even_count_takes_lower_middle(void) {
    iobench_sample_t in[] = {
        make("block", "read", 4096, 0, 40, "ok"),
        make("block", "read", 4096, 1, 10, "ok"),
        make("block", "read", 4096, 2, 30, "ok"),
        make("block", "read", 4096, 3, 20, "ok"),
    };
    iobench_sample_t out[8];

    CHECK(iobench_stats_summarize(in, 4, out, 8) == 3);
    CHECK(out[0].seconds_us == 20);
    CHECK(out[1].seconds_us == 10);
    CHECK(out[2].seconds_us == 40);
    return 0;
}

static int check_cells_stay_apart(void) {
    iobench_sample_t in[] = {
        make("block", "write", 4096, 0, 5, "ok"),
        make("rwy-blk", "write", 4096, 0, 50, "ok"),
        make("block", "write", 65536, 0, 7, "ok"),
        make("block", "write", 4096, 1, 6, "ok"),
        make("rwy-blk", "write", 4096, 1, 60, "ok"),
        make("block", "write", 65536, 1, 8, "ok"),
    };
    iobench_sample_t out[16];

    CHECK(iobench_stats_summarize(in, 6, out, 16) == 9);
    CHECK(strcmp(out[0].dev, "block") == 0 && out[0].size == 4096 && out[0].seconds_us == 5);
    CHECK(strcmp(out[3].dev, "rwy-blk") == 0 && out[3].seconds_us == 50);
    CHECK(out[6].size == 65536 && out[6].seconds_us == 7);
    return 0;
}

static int check_failed_and_single_samples(void) {
    iobench_sample_t in[] = {
        make("block", "write", 4096, 0, 1, "failed"),
        make("block", "write", 4096, 1, 9, "ok"),
        make("block", "read", 4096, 0, 3, "ok"),
        make("block", "read", 4096, 1, 4, "failed"),
    };
    iobench_sample_t out[8];

    CHECK(iobench_stats_summarize(in, 4, out, 8) == 0);
    CHECK(iobench_stats_summarize(in, 0, out, 8) == 0);
    return 0;
}

static int check_output_limit(void) {
    iobench_sample_t in[] = {
        make("block", "write", 4096, 0, 1, "ok"),
        make("block", "write", 4096, 1, 2, "ok"),
    };
    iobench_sample_t out[2];

    CHECK(iobench_stats_summarize(in, 2, out, 2) == 2);
    CHECK(strcmp(out[1].stat, "min") == 0);
    return 0;
}

int main(void) {
    if (check_odd_count() || check_even_count_takes_lower_middle() || check_cells_stay_apart() ||
        check_failed_and_single_samples() || check_output_limit())
        return 1;
    printf("iobench_stats: ok\n");
    return 0;
}
