/*
 * rwy_entry.h — BLS entry name validation, split, and default naming.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_ENTRY_H
#define RWY_ENTRY_H

#include <stddef.h>
#include <stdint.h>

#define RWY_ENTRY_MAX 255u

/* Validate entry name: 1..255 chars in [A-Za-z0-9._+-], ends in .conf or .efi,
 * stem does not end in counter (+<digits>[-<digits>]). */
int rwy_entry_name_ok(const char *name, size_t len);

/* Split valid entry into stem length and whether extension is .efi. */
int rwy_entry_split(const char *name, size_t len, size_t *stem_len, int *is_efi);

/* Derive default entry name: runway-<slot_num>.efi if only_kernel, else runway-<slot_num>.conf.
 * Returns length or -1 on buffer overflow. */
int rwy_entry_default(char *buf, size_t buf_sz, uint64_t slot_num, int only_kernel);

/* Check if stem ends in +<digits> or +<digits>-<digits>. */
int rwy_stem_has_counter(const char *stem, size_t len);

#endif /* RWY_ENTRY_H */
