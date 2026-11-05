/*
 * Watchdog driver application for the Intel 6300ESB (QEMU -device i6300esb).
 * SPDX-License-Identifier: Apache-2.0
 * Load options: "arm <seconds>" or "stop".
 */
#include "wdt_common.h"
#include "efi_util.h"

#define ESB_VENDOR      0x8086
#define ESB_DEVICE      0x25ab

#define PCI_COMMAND     0x04
#define PCI_CMD_MEMORY  0x0002
#define PCI_BAR0        0x10
#define ESB_CONFIG_REG  0x60
#define ESB_CONFIG_RESET 0x0003     /* no interrupt, reset on timeout, 33 MHz / 2^15 */
#define ESB_LOCK_REG    0x68
#define ESB_WDT_LOCK    0x01
#define ESB_WDT_ENABLE  0x02

#define ESB_TIMER1      0x00
#define ESB_TIMER2      0x04
#define ESB_RELOAD      0x0c
#define ESB_RELOAD_NOW  0x100
#define ESB_PRELOAD_MAX 0xfffff
/* Each of the two stages counts 2^24 ticks, about half a second, per unit. */
#define ESB_UNITS_PER_S 512

static void esb_unlock(volatile UINT8 *mmio) {
    mmio[ESB_RELOAD] = 0x80;
    mmio[ESB_RELOAD] = 0x86;
}

static void esb_write32(volatile UINT8 *mmio, UINTN reg, UINT32 v) {
    esb_unlock(mmio);
    *(volatile UINT32 *)(mmio + reg) = v;
}

EFI_STATUS efi_main(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st) {
    EFI_PCI_IO_PROTOCOL *pci;
    volatile UINT8 *mmio;
    UINT32 seconds = 0, bar, preload;
    UINT16 cmd, cfg = ESB_CONFIG_RESET;
    UINT8 lock;
    int is_stop = 0;
    EFI_STATUS s;

    util_init(ih, st);
    s = wdt_parse_options(ih, &is_stop, &seconds);
    if (EFI_ERROR(s)) return s;
    s = wdt_pci_find(ESB_VENDOR, ESB_DEVICE, &pci);
    if (EFI_ERROR(s)) return s;

    pci->Pci.Read(pci, EfiPciIoWidthUint8, ESB_LOCK_REG, 1, &lock);
    if (lock & ESB_WDT_LOCK) {
        uprint("I6300ESB: locked by firmware\n");
        return EFI_ACCESS_DENIED;
    }
    pci->Pci.Read(pci, EfiPciIoWidthUint32, PCI_BAR0, 1, &bar);
    bar &= ~0xfU;
    if (!bar) return EFI_DEVICE_ERROR;
    pci->Pci.Read(pci, EfiPciIoWidthUint16, PCI_COMMAND, 1, &cmd);
    cmd |= PCI_CMD_MEMORY;
    pci->Pci.Write(pci, EfiPciIoWidthUint16, PCI_COMMAND, 1, &cmd);

    mmio = (volatile UINT8 *)(UINTN)bar;
    if (is_stop) {
        esb_unlock(mmio);
        *(volatile UINT16 *)(mmio + ESB_RELOAD) = ESB_RELOAD_NOW;
        lock = 0;
        pci->Pci.Write(pci, EfiPciIoWidthUint8, ESB_LOCK_REG, 1, &lock);
        pci->Pci.Read(pci, EfiPciIoWidthUint8, ESB_LOCK_REG, 1, &lock);
        if (lock & ESB_WDT_ENABLE) return EFI_UNSUPPORTED;
        uprint("I6300ESB: stopped\n");
        return EFI_SUCCESS;
    }

    pci->Pci.Write(pci, EfiPciIoWidthUint16, ESB_CONFIG_REG, 1, &cfg);
    if (seconds > ESB_PRELOAD_MAX / ESB_UNITS_PER_S)
        seconds = ESB_PRELOAD_MAX / ESB_UNITS_PER_S;
    preload = seconds * ESB_UNITS_PER_S;
    esb_write32(mmio, ESB_TIMER1, preload);
    esb_write32(mmio, ESB_TIMER2, preload);
    esb_unlock(mmio);
    *(volatile UINT16 *)(mmio + ESB_RELOAD) = ESB_RELOAD_NOW;

    lock = ESB_WDT_ENABLE;
    pci->Pci.Write(pci, EfiPciIoWidthUint8, ESB_LOCK_REG, 1, &lock);
    wdt_record_armed("I6300ESB");
    uprintf("I6300ESB: armed, %u s\n", seconds);
    return EFI_SUCCESS;
}
