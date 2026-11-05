/*
 * Helpers shared by the watchdog driver applications.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WDT_COMMON_H
#define WDT_COMMON_H

#include <efi.h>
#include <efilib.h>
#include <efipciio.h>

static inline void outw(UINT16 port, UINT16 v) {
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline UINT16 inw(UINT16 port) {
    UINT16 v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(UINT16 port, UINT8 v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline UINT8 inb(UINT16 port) {
    UINT8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(UINT16 port, UINT32 v) {
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline UINT32 inl(UINT16 port) {
    UINT32 v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Parse the load options: "arm <seconds>" or "stop". */
EFI_STATUS wdt_parse_options(EFI_HANDLE ih, int *is_stop, UINT32 *seconds);

/* Parse the load options "arm <seconds>"; seconds must be > 0. */
EFI_STATUS wdt_parse_seconds(EFI_HANDLE ih, UINT32 *seconds);

/* Record the driver name that armed the watchdog. */
void wdt_record_armed(const char *name);

/* Find the first PCI function with this ID; EFI_UNSUPPORTED if absent. */
EFI_STATUS wdt_pci_find(UINT16 vendor, UINT16 device, EFI_PCI_IO_PROTOCOL **out);

#endif
