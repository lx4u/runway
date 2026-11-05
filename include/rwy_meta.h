/*
 * rwy_meta.h — BLS Type 1 metadata file format and validation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * META is host-only key-value text lines, at most 1 KiB, Type 1 only:
 * version, machine-id, sort-key, architecture.
 */
#ifndef RWY_META_H
#define RWY_META_H

#include <stddef.h>
#include <stdint.h>

#define RWY_META_MAX 1024u

/* Validates that text is valid META format with known keys and size <= 1024. */
int rwy_meta_ok(const char *text, size_t len);

/* Finds a key in META text; returns 1 if found and valid, 0 if not found. */
int rwy_meta_find(const char *text, size_t len, const char *key, const char **val_out, size_t *val_len_out);

#endif /* RWY_META_H */
