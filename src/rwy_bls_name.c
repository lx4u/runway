/*
 * rwy_bls_name.c — BLS entry name counter parsing, rendering, and rename matching.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared between host core and RUNWAY.EFI.
 */
#include "rwy_bls.h"
#include "rwy_core.h"

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int parse_u32(const char *s, size_t len, uint32_t *out) {
    uint64_t v = 0;
    size_t i;
    if (len == 0 || len > 10) return -1;
    for (i = 0; i < len; i++) {
        if (!is_digit(s[i])) return -1;
        v = v * 10 + (uint64_t)(s[i] - '0');
        if (v > UINT32_MAX) return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

/* Match trailing +<digits>-<digits>. */
static int match_two_counters(const char *name, size_t stem_len,
                             size_t *prefix_len_out,
                             uint32_t *tries_left_out,
                             int32_t *tries_done_out) {
    size_t i = stem_len, d1 = 0, d2 = 0, dash;
    uint32_t v1, v2;

    while (i > 0 && is_digit(name[i - 1])) { d1++; i--; }
    if (d1 == 0 || i == 0 || name[i - 1] != '-') return 0;
    dash = i - 1;
    i--;
    while (i > 0 && is_digit(name[i - 1])) { d2++; i--; }
    if (d2 == 0 || i == 0 || name[i - 1] != '+') return 0;

    if (parse_u32(name + i, d2, &v1) < 0 || parse_u32(name + dash + 1, d1, &v2) < 0) return -1;
    *prefix_len_out = i - 1;
    *tries_left_out = v1;
    *tries_done_out = (int32_t)v2;
    return 1;
}

/* Match trailing +<digits>. */
static int match_one_counter(const char *name, size_t stem_len,
                            size_t *prefix_len_out,
                            uint32_t *tries_left_out,
                            int32_t *tries_done_out) {
    size_t i = stem_len, d1 = 0;
    uint32_t v1;

    while (i > 0 && is_digit(name[i - 1])) { d1++; i--; }
    if (d1 == 0 || i == 0 || name[i - 1] != '+') return 0;

    if (parse_u32(name + i, d1, &v1) < 0) return -1;
    *prefix_len_out = i - 1;
    *tries_left_out = v1;
    *tries_done_out = -1;
    return 1;
}

static int find_counter(const char *name, size_t stem_len,
                        size_t *prefix_len_out,
                        uint32_t *tries_left_out,
                        int32_t *tries_done_out) {
    int rc = match_two_counters(name, stem_len, prefix_len_out, tries_left_out, tries_done_out);
    if (rc != 0) return rc;
    return match_one_counter(name, stem_len, prefix_len_out, tries_left_out, tries_done_out);
}

static int split_ext(const char *name, size_t len, const char **ext_out, size_t *ext_len_out) {
    if (len >= 4 && rwy_strncmp(name + len - 4, ".efi", 4) == 0) {
        *ext_out = ".efi";
        *ext_len_out = 4;
        return 1;
    }
    if (len >= 5 && rwy_strncmp(name + len - 5, ".conf", 5) == 0) {
        *ext_out = ".conf";
        *ext_len_out = 5;
        return 1;
    }
    return 0;
}

static int build_canonical(const char *stem, size_t stem_len,
                           const char *ext, size_t ext_len,
                           char *out, size_t out_sz) {
    size_t total = stem_len + ext_len, i;
    if (stem_len == 0) return RWY_ERR_INVAL;
    if (out_sz < total + 1) return RWY_ERR_NOSPC;
    if (rwy_stem_has_counter(stem, stem_len)) return RWY_ERR_INVAL;
    for (i = 0; i < stem_len; i++) out[i] = stem[i];
    for (i = 0; i < ext_len; i++) out[stem_len + i] = ext[i];
    out[total] = 0;
    return rwy_entry_name_ok(out, total) ? RWY_OK : RWY_ERR_INVAL;
}

int rwy_bls_name_parse(const char *name, size_t len,
                        char *canonical_out, size_t canonical_sz,
                        int *has_counter,
                        uint32_t *tries_left,
                        int32_t *tries_done) {
    size_t stem_len, ext_len, prefix_len;
    const char *ext;
    uint32_t t_left = 0;
    int32_t t_done = -1;
    int c_res, rc;

    if (!name || len == 0 || len > RWY_ENTRY_MAX) return RWY_ERR_INVAL;
    if (!split_ext(name, len, &ext, &ext_len)) return RWY_ERR_INVAL;

    stem_len = len - ext_len;
    if (stem_len == 0) return RWY_ERR_INVAL;
    prefix_len = stem_len;

    c_res = find_counter(name, stem_len, &prefix_len, &t_left, &t_done);
    if (c_res < 0) return RWY_ERR_INVAL;

    rc = build_canonical(name, prefix_len, ext, ext_len, canonical_out, canonical_sz);
    if (rc != RWY_OK) return rc;

    if (has_counter) *has_counter = (c_res > 0);
    if (tries_left) *tries_left = t_left;
    if (tries_done) *tries_done = t_done;
    return RWY_OK;
}

static size_t fmt_u32(char *buf, uint32_t v) {
    char tmp[16];
    size_t len = 0, i;
    if (v == 0) { buf[0] = '0'; return 1; }
    while (v > 0) {
        tmp[len++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    return len;
}

int rwy_bls_name_render(const char *canonical, size_t canonical_len,
                         const char *state, uint32_t tries,
                         char *out, size_t out_sz) {
    size_t stem_len, ext_len, out_len, i;
    int is_efi;
    char cbuf[16];
    size_t clen = 0;

    if (!rwy_entry_split(canonical, canonical_len, &stem_len, &is_efi)) return RWY_ERR_INVAL;
    ext_len = is_efi ? 4 : 5;

    if (rwy_strncmp(state, "OK", 3) == 0) {
        clen = 0;
    } else if (rwy_strncmp(state, "INSTALLED", 10) == 0 || rwy_strncmp(state, "TESTING", 8) == 0) {
        cbuf[0] = '+';
        clen = 1 + fmt_u32(cbuf + 1, tries);
    } else if (rwy_strncmp(state, "FAILED", 7) == 0) {
        cbuf[0] = '+'; cbuf[1] = '0';
        clen = 2;
    } else {
        return RWY_ERR_INVAL;
    }

    out_len = stem_len + clen + ext_len;
    if (out_sz < out_len + 1) return RWY_ERR_NOSPC;

    for (i = 0; i < stem_len; i++) out[i] = canonical[i];
    for (i = 0; i < clen; i++) out[stem_len + i] = cbuf[i];
    for (i = 0; i < ext_len; i++) out[stem_len + clen + i] = canonical[stem_len + i];
    out[out_len] = 0;

    return (int)out_len;
}

int rwy_bls_name_boot_count_path(const char *canonical, size_t canonical_len,
                                  uint32_t tries_left,
                                  char *out, size_t out_sz) {
    size_t stem_len, out_len, pfx_len, i;
    int is_efi;
    const char *pfx;
    char cbuf[16];
    size_t clen;

    if (!rwy_entry_split(canonical, canonical_len, &stem_len, &is_efi)) return RWY_ERR_INVAL;
    pfx = is_efi ? "/EFI/Linux/" : "/loader/entries/";
    pfx_len = is_efi ? 11 : 16;

    cbuf[0] = '+';
    clen = 1 + fmt_u32(cbuf + 1, tries_left);
    out_len = pfx_len + stem_len + clen + (is_efi ? 4 : 5);
    if (out_sz < out_len + 1) return RWY_ERR_NOSPC;

    for (i = 0; i < pfx_len; i++) out[i] = pfx[i];
    for (i = 0; i < stem_len; i++) out[pfx_len + i] = canonical[i];
    for (i = 0; i < clen; i++) out[pfx_len + stem_len + i] = cbuf[i];
    for (i = 0; i < (size_t)(is_efi ? 4 : 5); i++) out[pfx_len + stem_len + clen + i] = canonical[stem_len + i];
    out[out_len] = 0;

    return (int)out_len;
}

int rwy_bls_name_match_rename(const char *old_name, size_t old_len,
                               const char *new_name, size_t new_len,
                               int *target_state) {
    char old_canon[RWY_ENTRY_MAX + 1], new_canon[RWY_ENTRY_MAX + 1];
    int old_has_counter = 0, new_has_counter = 0;
    uint32_t old_left = 0, new_left = 0;
    int32_t old_done = -1, new_done = -1;
    size_t canon_len;

    if (rwy_bls_name_parse(old_name, old_len, old_canon, sizeof(old_canon),
                            &old_has_counter, &old_left, &old_done) != RWY_OK) {
        return RWY_ERR_PERM;
    }
    if (rwy_bls_name_parse(new_name, new_len, new_canon, sizeof(new_canon),
                            &new_has_counter, &new_left, &new_done) != RWY_OK) {
        return RWY_ERR_PERM;
    }

    canon_len = rwy_strlen(old_canon);
    if (canon_len != rwy_strlen(new_canon) ||
        rwy_strncmp(old_canon, new_canon, canon_len) != 0) {
        return RWY_ERR_PERM;
    }

    if (!old_has_counter) return RWY_ERR_PERM;

    if (!new_has_counter) {
        if (target_state) *target_state = RWY_BLS_STATE_OK;
        return RWY_OK;
    }

    if (new_left == 0) {
        if (target_state) *target_state = RWY_BLS_STATE_FAILED;
        return RWY_OK;
    }

    return RWY_ERR_PERM;
}
