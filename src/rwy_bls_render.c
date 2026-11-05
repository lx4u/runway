/*
 * rwy_bls_render.c — BLS entry rendering from slot configuration.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-only code.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "rwy_bls.h"
#include "rwy_core.h"
#include "rwy_meta.h"

static size_t append_str(char *out, size_t pos, size_t max, const char *s) {
    size_t len = strlen(s);
    if (pos + len >= max) return max;
    memcpy(out + pos, s, len);
    pos += len;
    out[pos] = 0;
    return pos;
}

void rwy_bls_derive_prefix(const rwy_bls_slot_info_t *info, char *prefix, size_t max) {
    const char *ver = NULL, *mid = NULL;
    size_t vlen = 0, mlen = 0;
    char token[64], version[64];

    if (info->meta_len > 0) {
        (void)rwy_meta_find(info->meta, info->meta_len, "version", &ver, &vlen);
        (void)rwy_meta_find(info->meta, info->meta_len, "machine-id", &mid, &mlen);
    }
    if (mid && mlen < sizeof(token)) {
        memcpy(token, mid, mlen); token[mlen] = 0;
    } else {
        snprintf(token, sizeof(token), "%.40s", info->entry_name);
        char *dot = strrchr(token, '.');
        if (dot) *dot = 0;
    }
    if (ver && vlen < sizeof(version)) {
        memcpy(version, ver, vlen); version[vlen] = 0;
        snprintf(prefix, max, "/%.50s/%.50s", token, version);
    } else {
        snprintf(prefix, max, "/%.50s", token);
    }
}

static size_t append_meta_keys(const rwy_bls_slot_info_t *info, char *out, size_t pos, size_t max) {
    static const char *const keys[] = { "version", "machine-id", "sort-key", "architecture" };
    size_t k;
    for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        const char *val;
        size_t vlen;
        if (info->meta_len > 0 && rwy_meta_find(info->meta, info->meta_len, keys[k], &val, &vlen)) {
            char line[256];
            snprintf(line, sizeof(line), "%s %.*s\n", keys[k], (int)vlen, val);
            pos = append_str(out, pos, max, line);
            if (pos >= max) return max;
        }
    }
    return pos;
}

static size_t append_payload_lines(const rwy_bls_slot_info_t *info, const char *pfx,
                                   char *out, size_t pos, size_t max) {
    char line[512];

    snprintf(line, sizeof(line), "linux %s/linux\n", pfx);
    pos = append_str(out, pos, max, line);
    if (pos >= max) return max;

    if (info->has_initrd) {
        snprintf(line, sizeof(line), "initrd %s/initrd\n", pfx);
        pos = append_str(out, pos, max, line);
        if (pos >= max) return max;
    }

    if (info->has_dtb) {
        snprintf(line, sizeof(line), "devicetree %s/dtb\n", pfx);
        pos = append_str(out, pos, max, line);
        if (pos >= max) return max;
    }

    return pos;
}

int rwy_bls_render_type1(const rwy_bls_slot_info_t *info, char *out, size_t out_sz) {
    char pfx[256], line[512];
    size_t pos = 0;

    if (!info || !out || out_sz == 0 || !info->has_kernel) return RWY_ERR_INVAL;
    out[0] = 0;

    snprintf(line, sizeof(line), "title %s\n", info->has_label ? info->label : info->slot_name);
    pos = append_str(out, pos, out_sz, line);
    if (pos >= out_sz) return RWY_ERR_NOSPC;

    pos = append_meta_keys(info, out, pos, out_sz);
    if (pos >= out_sz) return RWY_ERR_NOSPC;

    rwy_bls_derive_prefix(info, pfx, sizeof(pfx));
    pos = append_payload_lines(info, pfx, out, pos, out_sz);
    if (pos >= out_sz) return RWY_ERR_NOSPC;

    if (info->has_args && info->args[0]) {
        pos = append_str(out, pos, out_sz, "options ");
        pos = append_str(out, pos, out_sz, info->args);
        pos = append_str(out, pos, out_sz, "\n");
        if (pos >= out_sz) return RWY_ERR_NOSPC;
    }

    return (int)pos;
}

static int read_slot_file(rwy_store_t *s, const char *slot, const char *name,
                          char *out, size_t out_sz, size_t *read_len) {
    char path[256];
    size_t n = out_sz - 1;
    snprintf(path, sizeof(path), "%s/%s", slot, name);
    if (rwy_read_path(s, path, 0, out, &n) != RWY_OK || n == 0) return 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) n--;
    out[n] = 0;
    if (read_len) *read_len = n;
    return 1;
}

static int check_file_exists(rwy_store_t *s, const char *slot, const char *name) {
    char path[256];
    rwy_attr_t a;
    snprintf(path, sizeof(path), "%s/%s", slot, name);
    return rwy_stat(s, path, &a) == RWY_OK;
}

static uint64_t parse_slot_num(const char *slot) {
    const char *base = strrchr(slot, '/');
    base = base ? base + 1 : slot;
    if (strncmp(base, "CONFIG", 6) == 0) {
        return strtoull(base + 6, NULL, 10);
    }
    return 0;
}

static void derive_entry_name(rwy_store_t *s, const char *slot,
                              rwy_bls_slot_info_t *info, int only_kernel) {
    char entry_val[RWY_ENTRY_MAX + 1];
    if (read_slot_file(s, slot, "ENTRY", entry_val, sizeof(entry_val), NULL)) {
        snprintf(info->entry_name, sizeof(info->entry_name), "%s", entry_val);
    } else {
        uint64_t num = parse_slot_num(slot);
        rwy_entry_default(info->entry_name, sizeof(info->entry_name), num, only_kernel);
    }
}

int rwy_bls_read_slot_info(rwy_store_t *s, const char *slot_path, rwy_bls_slot_info_t *info) {
    const char *base;
    char tries_str[32], rev_str[32];
    int only_kernel;

    if (!s || !slot_path || !info) return RWY_ERR_INVAL;
    memset(info, 0, sizeof(*info));

    base = strrchr(slot_path, '/');
    snprintf(info->slot_name, sizeof(info->slot_name), "%s", base ? base + 1 : slot_path);

    info->has_kernel = check_file_exists(s, slot_path, "KERNEL");
    info->has_initrd = check_file_exists(s, slot_path, "INITRD");
    info->has_dtb = check_file_exists(s, slot_path, "DTB");

    info->has_label = read_slot_file(s, slot_path, "LABEL", info->label, sizeof(info->label), NULL);
    info->has_args = read_slot_file(s, slot_path, "ARGS", info->args, sizeof(info->args), NULL);
    (void)read_slot_file(s, slot_path, "META", info->meta, sizeof(info->meta), &info->meta_len);

    if (info->meta_len > 0) info->meta[info->meta_len++] = '\n';

    only_kernel = info->has_kernel && !info->has_initrd && !info->has_dtb &&
                  !info->has_args && !info->has_label && (info->meta_len == 0);

    derive_entry_name(s, slot_path, info, only_kernel);

    size_t elen = strlen(info->entry_name);
    info->is_type2 = (elen >= 4 && !strcmp(info->entry_name + elen - 4, ".efi"));

    if (read_slot_file(s, slot_path, "STATE", info->state, sizeof(info->state), NULL) == 0) {
        snprintf(info->state, sizeof(info->state), "INSTALLED");
    }

    if (read_slot_file(s, slot_path, "TRIES", tries_str, sizeof(tries_str), NULL)) {
        info->tries = (uint32_t)strtoul(tries_str, NULL, 10);
    } else {
        info->tries = 1;
    }

    if (read_slot_file(s, slot_path, "REVISION", rev_str, sizeof(rev_str), NULL)) {
        info->revision = strtoull(rev_str, NULL, 10);
    }

    return RWY_OK;
}
