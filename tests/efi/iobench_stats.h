/*
 * iobench_stats — the median, lowest and highest sample of each timed cell.
 * SPDX-License-Identifier: Apache-2.0
 * Plain C so the host test can use it too.
 */
#ifndef IOBENCH_STATS_H
#define IOBENCH_STATS_H

#include <stddef.h>
#include "iobench_csv.h"

/* The most samples of one cell that are summarised: the largest `rounds` the configuration accepts. */
#define IOBENCH_STATS_MAX_ROUNDS 100

/*
 * Write one "median", "min" and "max" row for every cell (dev, op, size) that has two or more samples
 * with result "ok". The rows are copies of the samples they pick, so the counters and the flush time
 * belong to the same sample as the seconds. The median of an even count is the lower middle sample.
 * Returns the number of rows written to `out`, at most `cap`.
 */
size_t iobench_stats_summarize(const iobench_sample_t *in, size_t n, iobench_sample_t *out, size_t cap);

#endif
