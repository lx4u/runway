/*
 * efi_util — tiny freestanding helpers for the Runway EFI binaries.
 * SPDX-License-Identifier: Apache-2.0
 *
 * No libc: everything here uses only BootServices/SystemTable calls plus
 * our own string/printf routines. Output goes to ConOut AND the serial
 * port (COM1 via EFI_SERIAL_IO_PROTOCOL) so `qemu -serial stdio` always
 * shows the log even if ConOut is not wired to serial by OVMF.
 */
#ifndef EFI_UTIL_H
#define EFI_UTIL_H

#include <efi.h>
#include <efilib.h>

/* Note: efilib.h provides the ST/BS globals (with gST/gBS aliases).
 * We use those directly; the image handle is cached here.
 * util_init does NOT call InitializeLib: Debian's libefi is built with
 * EFI_FUNCTION_WRAPPER (SysV app side), while our firmware calls are
 * ms_abi (HAVE_USE_MS_ABI) — mixing them corrupts arguments. Manual init
 * (ST/BS/LibImageHandle) is all this code needs from the library. */
extern EFI_HANDLE gImageHandle;

/* Must be called first in efi_main (wraps InitializeLib + caches globals). */
void util_init(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st);

/* Raw ASCII output (ConOut + serial). */
void uprint(const char *s);
void uprintf(const char *fmt, ...);
/* Like uprint(), with the console foreground set to attr (an EFI_* color) for the text. */
void uprint_color(UINTN attr, const char *s);
/* Keep a copy of the last `cap` bytes that uprint() writes, without the CR added for the console. NULL stops it. */
void uprint_capture(char *buf, UINTN cap);
/* The text kept so far and its length. */
UINTN uprint_captured(const char **text);

/* Boot log lines, each with the "RUNWAY: " prefix. The level is the one place to
 * add colors later. RWY_DEBUG prints only when built with -DRWY_DEBUG_LOG; otherwise the compiler still checks it and then drops it. */
#define RWY_ERROR(fmt, ...) uprintf("RUNWAY: " fmt, ##__VA_ARGS__)
#define RWY_WARN(fmt, ...)  uprintf("RUNWAY: " fmt, ##__VA_ARGS__)
#define RWY_INFO(fmt, ...)  uprintf("RUNWAY: " fmt, ##__VA_ARGS__)
#if defined(RWY_DEBUG_LOG)
#define RWY_DEBUG(fmt, ...) uprintf("RUNWAY: " fmt, ##__VA_ARGS__)
#else
#define RWY_DEBUG(fmt, ...) do { if (0) uprintf("RUNWAY: " fmt, ##__VA_ARGS__); } while (0)
#endif


/* Run fn in ufail() before the power-off (debug builds keep a log there). NULL removes it. */
void ufail_hook(void (*fn)(void));

/* Fatal helper: print + Stall a moment (lets serial flush) + return code. */
EFI_STATUS ufail(const char *msg, EFI_STATUS st);

/* ---- FAT helpers (firmware filesystem) ---- */

/* Open the volume our image was loaded from (LoadedImage.DeviceHandle). */
EFI_STATUS fat_open_our_root(EFI_HANDLE ih, EFI_FILE_HANDLE *out_root);

/* Read a whole file (e.g. CHAIN.EFI, RUNWAY.DAT probing) into pool memory. */
EFI_STATUS fat_read_file(EFI_FILE_HANDLE root, const CHAR16 *path,
                         UINT8 **out_buf, UINTN *out_size);

/* ---- TSC timing ---- */
void tsc_calibrate(void);          /* uses BS->Stall(1s); call once */
UINT64 tsc_now(void);              /* rdtsc */
double tsc_to_seconds(UINT64 d);   /* via calibration */
UINT64 tsc_hz(void);               /* the calibrated tick rate */

/* Register TPM random provider fallback. */
void efi_register_tpm_random(int (*fn)(void *buf, size_t len));

/* Random bytes combining EFI_RNG_PROTOCOL and TPM 2.0; nonzero when neither is available. */
int efi_random(void *buf, size_t len);

/* Compare GUIDs (EFI has no memcmp in freestanding). */
int guid_eq(const EFI_GUID *a, const EFI_GUID *b);

/* Stall-and-shutdown helpers for the automated harness. */
void flush_and_stall(UINTN microseconds);

#endif
