/*
 * rwy_string.h — String helpers that differ between the EFI build and the host build.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The EFI build has no memcmp, so core code calls CompareMem() in both builds.
 */
#ifndef RWY_STRING_H
#define RWY_STRING_H

#if defined(RWY_EFI) || defined(HAVE_USE_MS_ABI)
#include <efi.h>
#include <efilib.h>
#else
#include <stddef.h>
#include <string.h>

static inline int CompareMem(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
#endif

#endif /* RWY_STRING_H */
