/* iobench_stats — summarise the samples of a disk. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "iobench_stats.h"

static int same_text(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int same_cell(const iobench_sample_t *a, const iobench_sample_t *b) {
    return a->size == b->size && same_text(a->dev, b->dev) && same_text(a->op, b->op);
}

static int is_ok(const iobench_sample_t *s) {
    return same_text(s->result, "ok");
}

/* Is `i` the first sample of its cell? */
static int first_of_cell(const iobench_sample_t *in, size_t i) {
    for (size_t j = 0; j < i; j++)
        if (same_cell(&in[j], &in[i])) return 0;
    return 1;
}

/* The indexes of the ok samples of the cell of `in[i]`, from the fastest to the slowest. */
static size_t sorted_cell(const iobench_sample_t *in, size_t n, size_t i, size_t *idx) {
    size_t count = 0;

    for (size_t j = i; j < n && count < IOBENCH_STATS_MAX_ROUNDS; j++) {
        size_t k = count;

        if (!same_cell(&in[j], &in[i]) || !is_ok(&in[j])) continue;
        while (k > 0 && in[idx[k - 1]].seconds_us > in[j].seconds_us) {
            idx[k] = idx[k - 1];
            k--;
        }
        idx[k] = j;
        count++;
    }
    return count;
}

static void put_row(iobench_sample_t *out, size_t *w, size_t cap, const iobench_sample_t *src, const char *stat) {
    if (*w >= cap) return;
    out[*w] = *src;
    out[*w].stat = stat;
    (*w)++;
}

size_t iobench_stats_summarize(const iobench_sample_t *in, size_t n, iobench_sample_t *out, size_t cap) {
    size_t idx[IOBENCH_STATS_MAX_ROUNDS], w = 0;

    for (size_t i = 0; i < n; i++) {
        size_t count;

        if (!first_of_cell(in, i)) continue;
        count = sorted_cell(in, n, i, idx);
        if (count < 2) continue;
        put_row(out, &w, cap, &in[idx[(count - 1) / 2]], "median");
        put_row(out, &w, cap, &in[idx[0]], "min");
        put_row(out, &w, cap, &in[idx[count - 1]], "max");
    }
    return w;
}
