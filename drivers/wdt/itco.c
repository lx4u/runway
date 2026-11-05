/*
 * Watchdog driver application for the Intel ICH6+ TCO timer (iTCO).
 * SPDX-License-Identifier: Apache-2.0
 * Load options: "arm <seconds>" or "stop".
 */
#include "wdt_common.h"
#include "efi_util.h"

#define LPC_VENDOR      0x8086

#define LPC_PMBASE      0x40
#define LPC_RCBA        0xf0
#define PMBASE_MASK     0xff80U
#define RCBA_ENABLE     0x1U
#define RCBA_MASK       0xffffc000U
#define RCBA_GCS        0x3410
#define GCS_NO_REBOOT   0x20

#define TCO_BASE        0x60
#define TCO_RLD         0x00
#define TCO1_STS        0x04
#define TCO2_STS        0x06
#define TCO1_CNT        0x08
#define TCO_TMR         0x12
#define TCO1_TIMEOUT    0x0008
#define TCO2_SECOND_TO  0x0002
#define TCO2_BOOT_STS   0x0004
#define TCO1_LOCK       0x1000
#define TCO1_HLT        0x0800
#define TCO_TMR_MASK    0x03ff
#define TCO_TMR_MIN     4           /* values below 4 are ignored by the hardware */
#define TCO_TICK_DECI_S 6           /* one tick is 0.6 s */

/* LPC bridges with TCO version 2 and RCBA: ICH6 to ICH10 */
static const UINT16 lpc_ids[] = {
    0x2640, 0x2641, 0x2642,                             /* ICH6 */
    0x27b0, 0x27b8, 0x27b9, 0x27bc, 0x27bd,            /* ICH7, NM10 */
    0x2810, 0x2811, 0x2812, 0x2814, 0x2815,            /* ICH8 */
    0x2912, 0x2914, 0x2916, 0x2917, 0x2918, 0x2919,    /* ICH9 */
    0x3a14, 0x3a16, 0x3a18, 0x3a1a,                    /* ICH10 */
};

static EFI_STATUS find_lpc(EFI_PCI_IO_PROTOCOL **pci) {
    UINTN i;

    for (i = 0; i < sizeof(lpc_ids) / sizeof(lpc_ids[0]); i++)
        if (!EFI_ERROR(wdt_pci_find(LPC_VENDOR, lpc_ids[i], pci))) return EFI_SUCCESS;
    return EFI_UNSUPPORTED;
}

/* Allow the TCO to reset the machine; firmware may have set NO_REBOOT. */
static EFI_STATUS tco_allow_reboot(EFI_PCI_IO_PROTOCOL *pci) {
    volatile UINT32 *gcs;
    UINT32 rcba;

    pci->Pci.Read(pci, EfiPciIoWidthUint32, LPC_RCBA, 1, &rcba);
    if (!(rcba & RCBA_ENABLE)) return EFI_DEVICE_ERROR;
    gcs = (volatile UINT32 *)(UINTN)((rcba & RCBA_MASK) + RCBA_GCS);
    *gcs &= ~(UINT32)GCS_NO_REBOOT;
    return (*gcs & GCS_NO_REBOOT) ? EFI_ACCESS_DENIED : EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st) {
    EFI_PCI_IO_PROTOCOL *pci;
    UINT32 seconds = 0, pmbase;
    UINT16 tco, ticks, cnt;
    int is_stop = 0;
    EFI_STATUS s;

    util_init(ih, st);
    s = wdt_parse_options(ih, &is_stop, &seconds);
    if (EFI_ERROR(s)) return s;
    s = find_lpc(&pci);
    if (EFI_ERROR(s)) return s;

    pci->Pci.Read(pci, EfiPciIoWidthUint32, LPC_PMBASE, 1, &pmbase);
    pmbase &= PMBASE_MASK;
    if (!pmbase) return EFI_DEVICE_ERROR;
    tco = (UINT16)(pmbase + TCO_BASE);

    if (inw(tco + TCO1_CNT) & TCO1_LOCK) {
        uprint("ITCO: locked by firmware\n");
        return EFI_ACCESS_DENIED;
    }

    if (is_stop) {
        cnt = inw(tco + TCO1_CNT);
        outw(tco + TCO1_CNT, (UINT16)(cnt | TCO1_HLT));
        outw(tco + TCO_RLD, 1);
        cnt = inw(tco + TCO1_CNT);
        if (!(cnt & TCO1_HLT)) return EFI_UNSUPPORTED;
        uprint("ITCO: stopped\n");
        return EFI_SUCCESS;
    }

    s = tco_allow_reboot(pci);
    if (EFI_ERROR(s)) {
        uprint("ITCO: cannot clear NO_REBOOT\n");
        return s;
    }

    if (seconds > TCO_TMR_MASK * TCO_TICK_DECI_S / 10) ticks = TCO_TMR_MASK;
    else ticks = (UINT16)(seconds * 10 / TCO_TICK_DECI_S);
    if (ticks < TCO_TMR_MIN) ticks = TCO_TMR_MIN;

    outw(tco + TCO_TMR, (UINT16)((inw(tco + TCO_TMR) & ~TCO_TMR_MASK) | ticks));
    outw(tco + TCO1_STS, TCO1_TIMEOUT);
    outw(tco + TCO2_STS, TCO2_SECOND_TO | TCO2_BOOT_STS);
    outw(tco + TCO_RLD, 1);
    cnt = inw(tco + TCO1_CNT);
    outw(tco + TCO1_CNT, cnt & ~TCO1_HLT);
    wdt_record_armed("ITCO");
    uprintf("ITCO: armed, %u ticks\n", (unsigned)ticks);
    return EFI_SUCCESS;
}
