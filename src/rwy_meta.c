/*
 * rwy_meta.c — BLS Type 1 metadata file validation and parsing.
 * SPDX-License-Identifier: Apache-2.0
 *
 * META is host-only text of key-value lines, at most 1 KiB, Type 1 only:
 * version, machine-id, sort-key, architecture.
 */
#include "rwy_meta.h"
#include "rwy_core.h"

static const char *const META_KEYS[] = {
    "version",
    "machine-id",
    "sort-key",
    "architecture",
};
#define N_META_KEYS (sizeof(META_KEYS) / sizeof(META_KEYS[0]))

static int is_space(char c) {
    return c == ' ' || c == '\t';
}

static size_t skip_spaces(const char *s, size_t pos, size_t end) {
    while (pos < end && is_space(s[pos])) pos++;
    return pos;
}

static int match_key(const char *key, size_t klen) {
    for (size_t i = 0; i < N_META_KEYS; i++) {
        size_t l = rwy_strlen(META_KEYS[i]);
        if (klen == l && rwy_strncmp(key, META_KEYS[i], l) == 0) return (int)i;
    }
    return -1;
}

static int value_ok(const char *s, size_t len) {
    if (len == 0) return 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

static int parse_line(const char *text, size_t line_len, int *key_idx, const char **v_out, size_t *vl_out) {
    size_t kstart, kend, vstart, vend;

    kstart = skip_spaces(text, 0, line_len);
    if (kstart == line_len || text[kstart] == '#') return 0;

    kend = kstart;
    while (kend < line_len && !is_space(text[kend])) kend++;
    if (kend == line_len) return -1;

    *key_idx = match_key(text + kstart, kend - kstart);
    if (*key_idx < 0) return -1;

    vstart = skip_spaces(text, kend, line_len);
    if (vstart == line_len) return -1;

    vend = line_len;
    while (vend > vstart && (is_space(text[vend - 1]) || text[vend - 1] == '\r')) vend--;
    if (!value_ok(text + vstart, vend - vstart)) return -1;

    *v_out = text + vstart;
    *vl_out = vend - vstart;
    return 1;
}

int rwy_meta_ok(const char *text, size_t len) {
    size_t pos = 0;
    int seen_mask = 0;
    int count = 0;

    if (!text || len == 0 || len > RWY_META_MAX) return 0;

    while (pos < len) {
        size_t start = pos;
        const char *v;
        size_t vl;
        int key_idx = -1;
        int rc;

        while (pos < len && text[pos] != '\n') pos++;
        rc = parse_line(text + start, pos - start, &key_idx, &v, &vl);
        if (rc < 0) return 0;
        if (rc > 0) {
            if (seen_mask & (1 << key_idx)) return 0;
            seen_mask |= (1 << key_idx);
            count++;
        }
        if (pos < len && text[pos] == '\n') pos++;
    }

    return count > 0;
}

int rwy_meta_find(const char *text, size_t len, const char *key, const char **val_out, size_t *val_len_out) {
    size_t pos = 0;
    int target_idx;

    if (!text || len == 0 || !key) return 0;
    target_idx = match_key(key, rwy_strlen(key));
    if (target_idx < 0) return 0;

    while (pos < len) {
        size_t start = pos;
        const char *v = NULL;
        size_t vl = 0;
        int key_idx = -1;
        int rc;

        while (pos < len && text[pos] != '\n') pos++;
        rc = parse_line(text + start, pos - start, &key_idx, &v, &vl);
        if (rc > 0 && key_idx == target_idx) {
            if (val_out) *val_out = v;
            if (val_len_out) *val_len_out = vl;
            return 1;
        }
        if (pos < len && text[pos] == '\n') pos++;
    }

    return 0;
}
