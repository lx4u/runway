/*
 * iobench_config — parse the key/value file on the ESP.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "iobench_config.h"

static int same(const char *a, size_t n, const char *b) {
    size_t i = 0;

    for (; i < n && b[i]; i++)
        if (a[i] != b[i]) return 0;
    return i == n && b[i] == 0;
}

static void copy_text(char *dst, size_t cap, const char *src, size_t n) {
    size_t i;

    if (!cap) return;
    for (i = 0; i < n && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int parse_uint(const char *s, size_t n, unsigned lo, unsigned hi, unsigned *out) {
    unsigned v = 0;
    size_t i;

    if (!n) return -1;
    for (i = 0; i < n; i++) {
        unsigned d;

        if (s[i] < '0' || s[i] > '9') return -1;
        d = (unsigned)(s[i] - '0');
        if (v > (hi - d) / 10) return -1;
        v = v * 10 + d;
    }
    if (v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

static int parse_label(const char *s, size_t n, char *out) {
    size_t i;

    if (!n || n >= BENCH_LABEL_MAX) return -1;
    for (i = 0; i < n; i++) {
        char c = s[i];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') continue;
        return -1;
    }
    copy_text(out, BENCH_LABEL_MAX, s, n);
    return 0;
}

static unsigned device_bit(const char *s, size_t n) {
    if (same(s, n, "block")) return BENCH_DEV_BLOCK;
    if (same(s, n, "file")) return BENCH_DEV_FILE;
    if (same(s, n, "rwy-blk")) return BENCH_DEV_RWY_BLOCK;
    if (same(s, n, "rwy-file")) return BENCH_DEV_RWY_FILE;
    if (same(s, n, "rwy-enc")) return BENCH_DEV_RWY_ENC;
    if (same(s, n, "probe")) return BENCH_DEV_PROBE;
    return 0;
}

static int next_token(const char *s, size_t n, size_t *pos, const char **tok, size_t *len) {
    size_t i = *pos, start, end;

    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i == n) return 0;
    start = i;
    while (i < n && s[i] != ',') i++;
    end = i;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
    if (end == start) return -1;
    *tok = s + start;
    *len = end - start;
    if (i < n && s[i] == ',') i++;
    *pos = i;
    return 1;
}

static int parse_devices(const char *s, size_t n, unsigned *out) {
    unsigned mask = 0;
    size_t pos = 0;
    const char *tok;
    size_t len;
    int r;

    if (same(s, n, "all")) {
        *out = BENCH_DEV_ALL;
        return 0;
    }
    while ((r = next_token(s, n, &pos, &tok, &len)) > 0) {
        unsigned bit = device_bit(tok, len);

        if (!bit) return -1;
        mask |= bit;
    }
    if (r < 0 || !mask) return -1;
    *out = mask;
    return 0;
}

static int parse_disks(const char *s, size_t n, bench_disks_t *out) {
    if (same(s, n, "internal")) *out = BENCH_DISKS_INTERNAL;
    else if (same(s, n, "all")) *out = BENCH_DISKS_ALL;
    else if (same(s, n, "boot")) *out = BENCH_DISKS_BOOT;
    else return -1;
    return 0;
}

static int parse_bool(const char *s, size_t n, int *out) {
    if (same(s, n, "0")) *out = 0;
    else if (same(s, n, "1")) *out = 1;
    else return -1;
    return 0;
}

static int parse_result(const char *s, size_t n, bench_result_mode_t *out) {
    if (same(s, n, "auto")) *out = BENCH_RESULT_AUTO;
    else if (same(s, n, "store")) *out = BENCH_RESULT_STORE;
    else if (same(s, n, "esp")) *out = BENCH_RESULT_ESP;
    else if (same(s, n, "serial")) *out = BENCH_RESULT_SERIAL;
    else return -1;
    return 0;
}

static int parse_pause(const char *s, size_t n, bench_pause_t *out) {
    if (same(s, n, "never")) *out = BENCH_PAUSE_NEVER;
    else if (same(s, n, "fail")) *out = BENCH_PAUSE_FAIL;
    else if (same(s, n, "always")) *out = BENCH_PAUSE_ALWAYS;
    else return -1;
    return 0;
}

void bench_config_defaults(bench_config_t *cfg) {
    cfg->layout = 1;
    copy_text(cfg->label, sizeof(cfg->label), "default", 7);
    cfg->rounds = 1;
    cfg->warmup = 1;
    cfg->payload_mib = 16;
    cfg->devices = BENCH_DEV_ALL;
    cfg->disks = BENCH_DISKS_INTERNAL;
    cfg->include_boot = 0;
    cfg->result = BENCH_RESULT_AUTO;
    cfg->pause = BENCH_PAUSE_NEVER;
}

static int set_layout(const char *v, size_t n, bench_config_t *cfg) {
    unsigned x;

    if (parse_uint(v, n, 1, 1, &x)) return -1;
    cfg->layout = x;
    return 0;
}

static int set_label(const char *v, size_t n, bench_config_t *cfg) {
    return parse_label(v, n, cfg->label);
}

static int set_rounds(const char *v, size_t n, bench_config_t *cfg) {
    return parse_uint(v, n, 1, 100, &cfg->rounds);
}

static int set_warmup(const char *v, size_t n, bench_config_t *cfg) {
    return parse_bool(v, n, &cfg->warmup);
}

static int set_payload(const char *v, size_t n, bench_config_t *cfg) {
    return parse_uint(v, n, 1, 256, &cfg->payload_mib);
}

static int set_devices(const char *v, size_t n, bench_config_t *cfg) {
    return parse_devices(v, n, &cfg->devices);
}

static int set_disks(const char *v, size_t n, bench_config_t *cfg) {
    return parse_disks(v, n, &cfg->disks);
}

static int set_include_boot(const char *v, size_t n, bench_config_t *cfg) {
    return parse_bool(v, n, &cfg->include_boot);
}

static int set_result(const char *v, size_t n, bench_config_t *cfg) {
    return parse_result(v, n, &cfg->result);
}

static int set_pause(const char *v, size_t n, bench_config_t *cfg) {
    return parse_pause(v, n, &cfg->pause);
}

static const struct {
    const char *name;
    int (*set)(const char *v, size_t n, bench_config_t *cfg);
} keys[] = {
    { "layout", set_layout },
    { "label", set_label },
    { "rounds", set_rounds },
    { "warmup", set_warmup },
    { "payload_mib", set_payload },
    { "devices", set_devices },
    { "disks", set_disks },
    { "include_boot", set_include_boot },
    { "result", set_result },
    { "pause", set_pause },
};

static int config_key(const char *key, size_t klen, const char *value, size_t vlen,
                      bench_config_t *cfg) {
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (same(key, klen, keys[i].name)) return keys[i].set(value, vlen, cfg);
    return -1;
}

static int next_line(const char **pos, const char **line, const char **end) {
    const char *p = *pos, *start, *stop;

    if (!*p) return 0;
    start = p;
    while (*p && *p != '\n') p++;
    stop = p;
    if (*p == '\n') p++;
    while (stop > start && (stop[-1] == '\r' || stop[-1] == ' ' || stop[-1] == '\t')) stop--;
    while (start < stop && (*start == ' ' || *start == '\t')) start++;
    *line = start;
    *end = stop;
    *pos = p;
    return 1;
}

static int split_line(const char *line, const char *end, const char **key, size_t *klen,
                      const char **value, size_t *vlen) {
    const char *eq = line, *key_end, *val, *val_end;

    while (eq < end && *eq != '=') eq++;
    if (eq == end) return -1;
    key_end = eq;
    while (key_end > line && (key_end[-1] == ' ' || key_end[-1] == '\t')) key_end--;
    val = eq + 1;
    while (val < end && (*val == ' ' || *val == '\t')) val++;
    val_end = end;
    while (val_end > val && (val_end[-1] == ' ' || val_end[-1] == '\t')) val_end--;
    if (key_end == line || val == val_end) return -1;
    *key = line;
    *klen = (size_t)(key_end - line);
    *value = val;
    *vlen = (size_t)(val_end - val);
    return 0;
}

static int parse_line(const char *line, const char *end, bench_config_t *cfg) {
    const char *key, *value;
    size_t klen, vlen;

    if (line == end || *line == '#') return 0;
    if (split_line(line, end, &key, &klen, &value, &vlen)) return -1;
    return config_key(key, klen, value, vlen, cfg);
}

int bench_config_parse(const char *text, bench_config_t *cfg) {
    const char *p = text, *line, *end;

    while (next_line(&p, &line, &end))
        if (parse_line(line, end, cfg)) return -1;
    return 0;
}

static void put(char *out, size_t cap, size_t *n, const char *s) {
    while (*s) {
        if (*n + 1 < cap) out[*n] = *s;
        (*n)++;
        s++;
    }
}

static void put_uint(char *out, size_t cap, size_t *n, unsigned v) {
    char buf[10];
    size_t i = 0;

    do {
        buf[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i) {
        char c = buf[--i];
        if (*n + 1 < cap) out[*n] = c;
        (*n)++;
    }
}

static void put_devices(char *out, size_t cap, size_t *n, unsigned devices) {
    static const struct {
        unsigned bit;
        const char *name;
    } names[] = {
        { BENCH_DEV_BLOCK, "block" },
        { BENCH_DEV_FILE, "file" },
        { BENCH_DEV_RWY_BLOCK, "rwy-blk" },
        { BENCH_DEV_RWY_FILE, "rwy-file" },
        { BENCH_DEV_RWY_ENC, "rwy-enc" },
        { BENCH_DEV_PROBE, "probe" },
    };
    int first = 1;

    if (devices == BENCH_DEV_ALL) {
        put(out, cap, n, "all");
        return;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(devices & names[i].bit)) continue;
        if (!first) put(out, cap, n, ",");
        put(out, cap, n, names[i].name);
        first = 0;
    }
}

size_t bench_config_text(char *out, size_t cap, const bench_config_t *cfg) {
    size_t n = 0;

    put(out, cap, &n, "layout=");
    put_uint(out, cap, &n, cfg->layout);
    put(out, cap, &n, " label=");
    put(out, cap, &n, cfg->label);
    put(out, cap, &n, " rounds=");
    put_uint(out, cap, &n, cfg->rounds);
    put(out, cap, &n, " warmup=");
    put(out, cap, &n, cfg->warmup ? "1" : "0");
    put(out, cap, &n, " payload_mib=");
    put_uint(out, cap, &n, cfg->payload_mib);
    put(out, cap, &n, " devices=");
    put_devices(out, cap, &n, cfg->devices);
    put(out, cap, &n, " disks=");
    switch (cfg->disks) {
    case BENCH_DISKS_ALL: put(out, cap, &n, "all"); break;
    case BENCH_DISKS_BOOT: put(out, cap, &n, "boot"); break;
    default: put(out, cap, &n, "internal"); break;
    }
    put(out, cap, &n, " include_boot=");
    put(out, cap, &n, cfg->include_boot ? "1" : "0");
    put(out, cap, &n, " result=");
    put(out, cap, &n, bench_result_name(cfg->result));
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return n;
}

const char *bench_result_name(bench_result_mode_t mode) {
    switch (mode) {
    case BENCH_RESULT_STORE: return "store";
    case BENCH_RESULT_ESP: return "esp";
    case BENCH_RESULT_SERIAL: return "serial";
    default: return "auto";
    }
}

const char *bench_config_disk_set_name(const bench_config_t *cfg) {
    if (cfg->disks == BENCH_DISKS_ALL) return "all";
    if (cfg->disks == BENCH_DISKS_BOOT) return "boot";
    return cfg->include_boot ? "internal+boot" : "internal";
}
