/*
 * rwy_dbglog.h — debug builds keep the console text in a file on the ESP.
 * SPDX-License-Identifier: Apache-2.0
 *
 * RUNWAY_DEBUG.EFI writes \RUNWAY.LOG on the volume it was loaded from, so a failure can be read after
 * the machine reboots. Release builds get empty functions.
 */
#ifndef RWY_DBGLOG_H
#define RWY_DBGLOG_H

#include <efi.h>

#if defined(RWY_DEBUG_LOG)
/* Open the ESP root, start keeping the console text and hook the failure path of ufail(). Call it before
 * the store is mounted: later, the loaded image's handle serves the store, not the ESP. */
void dbglog_start(EFI_HANDLE image);
/* Write the text kept so far to \RUNWAY.LOG. Errors are ignored: a log must never stop a boot. */
void dbglog_flush(void);
/* Print what the firmware shows (disks, partitions, their types) once, so that the log says why a store was not found. */
void diag_firmware(void);
/* Flush, then wait for a key (60 s at most) so the operator can read the screen. */
void dbglog_pause(void);
#else
static inline void dbglog_start(EFI_HANDLE image) { (void)image; }
static inline void dbglog_flush(void) {}
static inline void diag_firmware(void) {}
static inline void dbglog_pause(void) {}
#endif

#endif
