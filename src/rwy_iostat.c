/*
 * rwy_iostat.c — Text summary of the I/O counters of a store and of its devices.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

void rwy_dev_stats_add(rwy_dev_stats_t *sum, const rwy_dev_stats_t *add) {
    sum->reads += add->reads;
    sum->writes += add->writes;
    sum->read_bytes += add->read_bytes;
    sum->write_bytes += add->write_bytes;
    sum->flushes += add->flushes;
    sum->window_hits += add->window_hits;
    sum->window_fills += add->window_fills;
}

typedef struct {
    char *p, *end;
} out_t;

static void put_str(out_t *o, const char *s) {
    while (*s && o->p < o->end) *o->p++ = *s++;
}

static void put_u64(out_t *o, uint64_t v) {
    char tmp[20];
    int n = 0;

    do tmp[n++] = (char)('0' + v % 10); while ((v /= 10) != 0);
    while (n) {
        if (o->p < o->end) *o->p++ = tmp[--n];
        else n--;
    }
}

static void put_core_row(out_t *o, const rwy_io_stats_t *c, int write, rwy_io_kind_t kind) {
    static const char *const kinds[RWY_IO_KINDS] = { "meta", "dir", "extent", "data" };
    static const char *const sizes[RWY_IO_BUCKETS] = { "4K", "64K", "1M", "big" };
    uint64_t total = 0;
    int b;

    for (b = 0; b < RWY_IO_BUCKETS; b++) total += c->calls[write][kind][b];
    if (!total) return;
    put_str(o, write ? "core write " : "core read ");
    put_str(o, kinds[kind]);
    put_str(o, ": ");
    put_u64(o, total);
    put_str(o, " calls (");
    for (b = 0; b < RWY_IO_BUCKETS; b++) {
        if (b) put_str(o, ", ");
        put_str(o, sizes[b]);
        put_str(o, " ");
        put_u64(o, c->calls[write][kind][b]);
    }
    put_str(o, "), ");
    put_u64(o, c->bytes[write][kind]);
    put_str(o, " bytes\n");
}

static void put_dev_line(out_t *o, const rwy_dev_stats_t *d) {
    put_str(o, "dev: reads ");
    put_u64(o, d->reads);
    put_str(o, " (");
    put_u64(o, d->read_bytes);
    put_str(o, " bytes), writes ");
    put_u64(o, d->writes);
    put_str(o, " (");
    put_u64(o, d->write_bytes);
    put_str(o, " bytes), flushes ");
    put_u64(o, d->flushes);
    put_str(o, ", window hits ");
    put_u64(o, d->window_hits);
    put_str(o, ", fills ");
    put_u64(o, d->window_fills);
    put_str(o, "\n");
}

size_t rwy_io_format(char *buf, size_t cap, const rwy_io_stats_t *core, const rwy_dev_stats_t *dev) {
    out_t o;
    int w, k;

    if (!cap) return 0;
    o.p = buf;
    o.end = buf + cap - 1;
    for (w = 0; w < 2; w++)
        for (k = 0; k < RWY_IO_KINDS; k++) put_core_row(&o, core, w, (rwy_io_kind_t)k);
    put_str(&o, "core flushes: ");
    put_u64(&o, core->flushes);
    put_str(&o, "\n");
    if (dev) put_dev_line(&o, dev);
    *o.p = 0;
    return (size_t)(o.p - buf);
}
