/*
 * rwy_native.h — reading from the boot store in RUNWAY.EFI. Internal.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_NATIVE_H
#define RWY_NATIVE_H

#include <efi.h>
#include "../include/rwy_core.h"
#include "../include/rwy_format.h"
#include "../include/rwy_recarea.h"

typedef struct { UINT8 *buf; UINTN size; } blob_t;

/* Read EFI/RUNWAY/<dir>/<file> (following links) into pool memory. Absent is EFI_NOT_FOUND. */
EFI_STATUS native_read(rwy_store_t *st, const char *dir, const char *file, blob_t *b);

/* Load the recovery bundle (ADR 0004) of the first member with a valid copy, one blob per RWY_RA_* kind.
 * EFI_NOT_FOUND: no member has an area. EFI_LOAD_ERROR: an area, but no valid copy; the message is printed. */
EFI_STATUS recboot_load(const rwy_block_dev_t devs[], const rwy_superblock_t sbs[], UINTN count, EFI_STATUS mount_err,
                        blob_t part[RWY_RA_KINDS]);

/* Probe the watchdog applications of EFI/RUNWAY/WDT and let one arm the hardware for `seconds`. */
void wdt_arm(rwy_store_t *st, EFI_HANDLE self, EFI_HANDLE fat_fs_handle, uint64_t seconds);

/* Stop the armed watchdog application, or re-arm if the hardware cannot be stopped. */
void wdt_stop(rwy_store_t *st, EFI_HANDLE self, EFI_HANDLE fat_fs_handle, const char *name,
              uint32_t timeout, uint64_t slot_wdt);

#endif
