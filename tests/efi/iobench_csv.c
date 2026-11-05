/* iobench_csv — build the benchmark CSV rows. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "iobench_csv.h"

static const char runs_head[] =
    "run,disk,status,arch,build,bench_version,firmware,system_model,devpath,"
    "media_id,disk_bytes,block_size,ioalign,write_cache,store_uuid,store_kind,"
    "disk_set,config,started,finished,reason,tsc_hz,timing_note,aes\n";

static const char samples_head[] =
    "run,disk,dev,op,size,sample,stat,seconds,bytes,calls_core,calls_dev,"
    "us_per_call,flush_seconds,extents,encrypted,result\n";

static size_t put_char(char *buf, size_t cap, size_t n, char c) {
    if (n + 1 < cap) buf[n] = c;
    return n + 1;
}

static size_t put_str(char *buf, size_t cap, size_t n, const char *s) {
    while (s && *s) n = put_char(buf, cap, n, *s++);
    return n;
}

static size_t put_u64(char *buf, size_t cap, size_t n, unsigned long long v) {
    char b[24];
    size_t i = sizeof(b);

    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i < sizeof(b)) n = put_char(buf, cap, n, b[i++]);
    return n;
}

static size_t put_zeros(char *buf, size_t cap, size_t n, size_t count) {
    while (count--) n = put_char(buf, cap, n, '0');
    return n;
}

static int needs_quote(const char *s) {
    for (; s && *s; s++)
        if (*s == ',' || *s == '"' || *s == '\n' || *s == '\r') return 1;
    return 0;
}

static size_t put_csv_str(char *buf, size_t cap, size_t n, const char *s) {
    if (!needs_quote(s)) return put_str(buf, cap, n, s);
    n = put_char(buf, cap, n, '"');
    for (; *s; s++) {
        if (*s == '"') n = put_char(buf, cap, n, '"');
        n = put_char(buf, cap, n, *s);
    }
    return put_char(buf, cap, n, '"');
}

static size_t put_sep(char *buf, size_t cap, size_t n) {
    return put_char(buf, cap, n, ',');
}

static size_t put_us(char *buf, size_t cap, size_t n, unsigned long long us) {
    unsigned long long frac = us % 1000000ull, digits = 1, t = frac;

    while (t >= 10) {
        t /= 10;
        digits++;
    }
    n = put_u64(buf, cap, n, us / 1000000ull);
    n = put_char(buf, cap, n, '.');
    n = put_zeros(buf, cap, n, 6 - (size_t)digits);
    return put_u64(buf, cap, n, frac);
}

static size_t put_x100(char *buf, size_t cap, size_t n, unsigned long long v) {
    unsigned long long frac = v % 100ull;

    n = put_u64(buf, cap, n, v / 100ull);
    n = put_char(buf, cap, n, '.');
    if (frac < 10ull) n = put_char(buf, cap, n, '0');
    return put_u64(buf, cap, n, frac);
}

static size_t finish(char *buf, size_t cap, size_t n) {
    if (cap) buf[n < cap ? n : cap - 1] = 0;
    return n;
}

const char *iobench_runs_header(void) {
    return runs_head;
}

const char *iobench_samples_header(void) {
    return samples_head;
}

size_t iobench_format_run(char *buf, size_t cap, const iobench_run_t *r) {
    size_t n = 0;

    n = put_csv_str(buf, cap, n, r->run);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->disk);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->status);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->arch);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->build);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->bench_version);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->firmware);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->system_model);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->devpath);
    n = put_sep(buf, cap, n);
    n = put_u64(buf, cap, n, r->media_id);
    n = put_sep(buf, cap, n);
    n = put_u64(buf, cap, n, r->disk_bytes);
    n = put_sep(buf, cap, n);
    n = put_u64(buf, cap, n, r->block_size);
    n = put_sep(buf, cap, n);
    n = put_u64(buf, cap, n, r->ioalign);
    n = put_sep(buf, cap, n);
    n = put_u64(buf, cap, n, r->write_cache ? 1 : 0);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->store_uuid);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->store_kind);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->disk_set);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->config);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->started);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->finished);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->reason);
    n = put_sep(buf, cap, n);
    n = put_u64(buf, cap, n, r->tsc_hz);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->timing_note);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, r->aes);
    n = put_char(buf, cap, n, '\n');
    return finish(buf, cap, n);
}

size_t iobench_format_sample(char *buf, size_t cap, const iobench_sample_t *s) {
    size_t n = 0;

    n = put_csv_str(buf, cap, n, s->run);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, s->disk);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, s->dev);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, s->op);
    n = put_sep(buf, cap, n);
    n = put_u64(buf, cap, n, s->size);
    n = put_sep(buf, cap, n);
    n = put_u64(buf, cap, n, s->sample);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, s->stat);
    n = put_sep(buf, cap, n);
    n = put_us(buf, cap, n, s->seconds_us);
    n = put_sep(buf, cap, n);
    n = put_u64(buf, cap, n, s->bytes);
    n = put_sep(buf, cap, n);
    n = put_u64(buf, cap, n, s->calls_core);
    n = put_sep(buf, cap, n);
    n = put_u64(buf, cap, n, s->calls_dev);
    n = put_sep(buf, cap, n);
    n = put_x100(buf, cap, n, s->us_per_call_x100);
    n = put_sep(buf, cap, n);
    n = put_us(buf, cap, n, s->flush_us);
    n = put_sep(buf, cap, n);
    n = put_u64(buf, cap, n, s->extents);
    n = put_sep(buf, cap, n);
    n = put_u64(buf, cap, n, s->encrypted ? 1 : 0);
    n = put_sep(buf, cap, n);
    n = put_csv_str(buf, cap, n, s->result);
    n = put_char(buf, cap, n, '\n');
    return finish(buf, cap, n);
}
