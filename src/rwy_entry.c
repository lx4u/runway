/*
 * rwy_entry.c — BLS entry name validation, split, and default naming.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared between host core and RUNWAY.EFI. Entry names are at most 255 bytes,
 * characters [A-Za-z0-9._+-], ending in .conf or .efi, without a trailing
 * counter (+<digits>[-<digits>]) on the stem.
 */
#include "rwy_entry.h"
#include "rwy_core.h"

static int is_valid_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' || c == '-';
}

/* Check if stem ends in +<digits> or +<digits>-<digits>. */
int rwy_stem_has_counter(const char *stem, size_t len) {
    size_t i = len;
    size_t d1 = 0, d2 = 0;

    while (i > 0 && stem[i - 1] >= '0' && stem[i - 1] <= '9') {
        d1++;
        i--;
    }
    if (d1 == 0 || i == 0) return 0;
    if (stem[i - 1] == '+') return 1;
    if (stem[i - 1] != '-') return 0;
    i--;
    while (i > 0 && stem[i - 1] >= '0' && stem[i - 1] <= '9') {
        d2++;
        i--;
    }
    return (d2 > 0 && i > 0 && stem[i - 1] == '+');
}

int rwy_entry_name_ok(const char *name, size_t len) {
    size_t i, stem_len;

    if (!name || len == 0 || len > RWY_ENTRY_MAX) return 0;
    for (i = 0; i < len; i++) {
        if (!is_valid_char(name[i])) return 0;
    }
    if (len >= 5 && rwy_strncmp(name + len - 4, ".efi", 4) == 0) {
        stem_len = len - 4;
    } else if (len >= 6 && rwy_strncmp(name + len - 5, ".conf", 5) == 0) {
        stem_len = len - 5;
    } else {
        return 0;
    }
    if (stem_len == 0) return 0;
    return !rwy_stem_has_counter(name, stem_len);
}

int rwy_entry_split(const char *name, size_t len, size_t *stem_len, int *is_efi) {
    if (!rwy_entry_name_ok(name, len)) return 0;
    if (name[len - 1] == 'i') {
        if (is_efi) *is_efi = 1;
        if (stem_len) *stem_len = len - 4;
    } else {
        if (is_efi) *is_efi = 0;
        if (stem_len) *stem_len = len - 5;
    }
    return 1;
}

int rwy_entry_default(char *buf, size_t buf_sz, uint64_t slot_num, int only_kernel) {
    char num[24];
    char *p = num + sizeof(num) - 1;
    uint64_t v = slot_num;
    size_t nl, total;
    const char *ext = only_kernel ? ".efi" : ".conf";
    size_t el = only_kernel ? 4 : 5;
    size_t i;

    *p = 0;
    do {
        *--p = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    nl = (size_t)((num + sizeof(num) - 1) - p);
    total = 7 + nl + el;
    if (buf_sz < total + 1) return -1;
    for (i = 0; i < 7; i++) buf[i] = "runway-"[i];
    for (i = 0; i < nl; i++) buf[7 + i] = p[i];
    for (i = 0; i < el; i++) buf[7 + nl + i] = ext[i];
    buf[total] = 0;
    return (int)total;
}
