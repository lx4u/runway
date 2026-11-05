/*
 * rwy_bls_parse.c — BLS Type 1 entry parser and fault checker.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-only code.
 */
#include <string.h>
#include "rwy_bls.h"
#include "rwy_core.h"
#include "rwy_meta.h"

static int is_space(char c) {
    return c == ' ' || c == '\t';
}

static size_t skip_spaces(const char *s, size_t pos, size_t end) {
    while (pos < end && is_space(s[pos])) pos++;
    return pos;
}

static int is_dotdot_component(const char *p, size_t len, size_t start) {
    size_t end = start;
    while (end < len && p[end] != '/') end++;
    return (end - start == 2 && p[start] == '.' && p[start + 1] == '.');
}

static int path_has_dotdot(const char *p, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (is_dotdot_component(p, len, i)) return 1;
        while (i < len && p[i] != '/') i++;
        if (i < len && p[i] == '/') i++;
    }
    return 0;
}

static int path_ok(const char *p, size_t len) {
    size_t i;
    if (len == 0 || len >= RWY_BLS_MAX_PATH) return 0;
    if (path_has_dotdot(p, len)) return 0;
    for (i = 0; i < len; i++) {
        uint8_t c = (uint8_t)p[i];
        if (c < 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

static int append_options(rwy_bls_entry_t *e, const char *val, size_t vlen) {
    size_t needed, i;
    if (vlen == 0) return RWY_OK;
    needed = e->options_len + (e->options_len ? 1 : 0) + vlen;
    if (needed > RWY_BLS_MAX_ARGS) return RWY_ERR_INVAL;
    if (e->options_len) e->options[e->options_len++] = ' ';
    for (i = 0; i < vlen; i++) e->options[e->options_len++] = val[i];
    e->options[e->options_len] = 0;
    return RWY_OK;
}

static int append_initrd(rwy_bls_entry_t *e, const char *val, size_t vlen) {
    size_t i;
    if (e->num_initrd >= RWY_BLS_MAX_INITRD || !path_ok(val, vlen)) return RWY_ERR_INVAL;
    for (i = 0; i < vlen; i++) e->initrd_paths[e->num_initrd][i] = val[i];
    e->initrd_paths[e->num_initrd][vlen] = 0;
    e->num_initrd++;
    return RWY_OK;
}

static int append_meta(rwy_bls_entry_t *e, const char *k, size_t kl, const char *v, size_t vl) {
    size_t needed, i;
    needed = e->meta_len + kl + 1 + vl + 1;
    if (needed > RWY_META_MAX) return RWY_ERR_INVAL;
    for (i = 0; i < kl; i++) e->meta[e->meta_len++] = k[i];
    e->meta[e->meta_len++] = ' ';
    for (i = 0; i < vl; i++) e->meta[e->meta_len++] = v[i];
    e->meta[e->meta_len++] = '\n';
    e->meta[e->meta_len] = 0;
    return RWY_OK;
}

static int handle_payload_key(rwy_bls_entry_t *e, const char *k, size_t kl,
                              const char *v, size_t vl) {
    size_t i;
    if (kl == 5 && rwy_strncmp(k, "linux", 5) == 0) {
        if (e->has_linux || !path_ok(v, vl)) return RWY_ERR_INVAL;
        for (i = 0; i < vl; i++) e->linux_path[i] = v[i];
        e->linux_path[vl] = 0;
        e->has_linux = 1;
        return RWY_OK;
    }
    if (kl == 6 && rwy_strncmp(k, "initrd", 6) == 0) {
        return append_initrd(e, v, vl);
    }
    if (kl == 10 && rwy_strncmp(k, "devicetree", 10) == 0) {
        if (e->has_dtb || !path_ok(v, vl)) return RWY_ERR_INVAL;
        for (i = 0; i < vl; i++) e->dtb_path[i] = v[i];
        e->dtb_path[vl] = 0;
        e->has_dtb = 1;
        return RWY_OK;
    }
    return RWY_ERR_NOENT;
}

static int handle_meta_key(rwy_bls_entry_t *e, const char *k, size_t kl,
                           const char *v, size_t vl) {
    if ((kl == 7 && rwy_strncmp(k, "version", 7) == 0) ||
        (kl == 10 && rwy_strncmp(k, "machine-id", 10) == 0) ||
        (kl == 8 && rwy_strncmp(k, "sort-key", 8) == 0) ||
        (kl == 12 && rwy_strncmp(k, "architecture", 12) == 0)) {
        return append_meta(e, k, kl, v, vl);
    }
    return RWY_ERR_NOENT;
}

static int handle_text_key(rwy_bls_entry_t *e, const char *k, size_t kl,
                           const char *v, size_t vl) {
    size_t i;
    if (kl == 7 && rwy_strncmp(k, "options", 7) == 0) {
        return append_options(e, v, vl);
    }
    if (kl == 5 && rwy_strncmp(k, "title", 5) == 0) {
        if (e->has_title || vl > RWY_BLS_MAX_LABEL) return RWY_ERR_INVAL;
        for (i = 0; i < vl; i++) e->title[i] = v[i];
        e->title[vl] = 0;
        e->has_title = 1;
        return RWY_OK;
    }
    return handle_meta_key(e, k, kl, v, vl);
}

static int process_key_value(rwy_bls_entry_t *e, const char *k, size_t kl,
                             const char *v, size_t vl) {
    int rc;

    if ((kl == 18 && rwy_strncmp(k, "devicetree-overlay", 18) == 0) ||
        (kl == 3 && rwy_strncmp(k, "efi", 3) == 0)) {
        return RWY_ERR_INVAL;
    }

    rc = handle_payload_key(e, k, kl, v, vl);
    if (rc != RWY_ERR_NOENT) return rc;

    rc = handle_text_key(e, k, kl, v, vl);
    if (rc != RWY_ERR_NOENT) return rc;

    return RWY_OK;
}

static int parse_line(const char *text, size_t line_len,
                      const char **k_out, size_t *kl_out,
                      const char **v_out, size_t *vl_out) {
    size_t kstart, kend, vstart, vend;

    kstart = skip_spaces(text, 0, line_len);
    if (kstart == line_len || text[kstart] == '#') return 0;

    kend = kstart;
    while (kend < line_len && !is_space(text[kend])) kend++;
    if (kend == line_len) return RWY_ERR_INVAL;

    vstart = skip_spaces(text, kend, line_len);
    if (vstart == line_len) return RWY_ERR_INVAL;

    vend = line_len;
    while (vend > vstart && (is_space(text[vend - 1]) || text[vend - 1] == '\r')) vend--;
    if (vend == vstart) return RWY_ERR_INVAL;

    *k_out = text + kstart;
    *kl_out = kend - kstart;
    *v_out = text + vstart;
    *vl_out = vend - vstart;
    return 1;
}

static int parse_internal(const char *text, size_t len, rwy_bls_entry_t *entry) {
    size_t pos = 0;

    if (!text || len == 0) return RWY_ERR_INVAL;
    memset(entry, 0, sizeof(*entry));

    while (pos < len) {
        size_t start = pos;
        const char *k, *v;
        size_t kl, vl;
        int rc;

        while (pos < len && text[pos] != '\n') pos++;
        if (pos - start > 4096) return RWY_ERR_INVAL;

        rc = parse_line(text + start, pos - start, &k, &kl, &v, &vl);
        if (rc < 0) return rc;
        if (rc > 0) {
            rc = process_key_value(entry, k, kl, v, vl);
            if (rc != RWY_OK) return rc;
        }
        if (pos < len && text[pos] == '\n') pos++;
    }

    if (entry->meta_len > 0 && !rwy_meta_ok(entry->meta, entry->meta_len)) {
        return RWY_ERR_INVAL;
    }
    return RWY_OK;
}

int rwy_bls_parse(const char *text, size_t len, rwy_bls_entry_t *entry) {
    int rc = parse_internal(text, len, entry);
    if (rc != RWY_OK) return rc;
    if (!entry->has_linux) return RWY_ERR_INVAL;
    return RWY_OK;
}

int rwy_bls_check_definite_faults(const char *text, size_t len) {
    rwy_bls_entry_t entry;
    return parse_internal(text, len, &entry);
}
