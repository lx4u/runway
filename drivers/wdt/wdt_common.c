/*
 * Helpers shared by the watchdog driver applications.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wdt_common.h"
#include "efi_util.h"
#include "rwy_format.h"

static int is_verb(const CHAR16 *p, UINTN n, const CHAR16 *verb, UINTN vlen) {
    UINTN i;

    if (n < vlen) return 0;
    for (i = 0; i < vlen; i++)
        if (p[i] != verb[i]) return 0;
    return 1;
}

static int is_blank_tail(const CHAR16 *p, UINTN n, UINTN start) {
    UINTN i;

    for (i = start; i < n; i++)
        if (p[i] != 0 && p[i] != ' ') return 0;
    return 1;
}

static int parse_seconds(const CHAR16 *p, UINTN n, UINT32 *out) {
    UINT64 v = 0;
    UINTN i;

    for (i = 4; i < n && p[i] >= '0' && p[i] <= '9'; i++) {
        v = v * 10 + (p[i] - '0');
        if (v > 0xffffffffULL) return 0;
    }
    if (i == 4 || v == 0) return 0;
    if (!is_blank_tail(p, n, i)) return 0;
    if (out) *out = (UINT32)v;
    return 1;
}

EFI_STATUS wdt_parse_options(EFI_HANDLE ih, int *is_stop, UINT32 *seconds) {
    static const CHAR16 arm_verb[] = { 'a', 'r', 'm', ' ' };
    static const CHAR16 stop_verb[] = { 's', 't', 'o', 'p' };
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *li;
    const CHAR16 *p;
    UINTN n;
    EFI_STATUS s = gBS->HandleProtocol(ih, &li_guid, (VOID **)&li);

    if (EFI_ERROR(s)) return s;
    p = li->LoadOptions;
    n = li->LoadOptionsSize / sizeof(CHAR16);
    if (!p || n == 0) return EFI_INVALID_PARAMETER;

    if (is_verb(p, n, stop_verb, 4) && is_blank_tail(p, n, 4)) {
        if (is_stop) *is_stop = 1;
        return EFI_SUCCESS;
    }
    if (is_verb(p, n, arm_verb, 4) && parse_seconds(p, n, seconds)) {
        if (is_stop) *is_stop = 0;
        return EFI_SUCCESS;
    }
    return EFI_INVALID_PARAMETER;
}

EFI_STATUS wdt_parse_seconds(EFI_HANDLE ih, UINT32 *seconds) {
    int is_stop = 0;
    EFI_STATUS s = wdt_parse_options(ih, &is_stop, seconds);

    if (EFI_ERROR(s)) return s;
    return is_stop ? EFI_INVALID_PARAMETER : EFI_SUCCESS;
}

void wdt_record_armed(const char *name) {
    EFI_GUID guid = RWY_BOOT_VAR_GUID;
    UINTN len = 0;

    if (!name) return;
    while (name[len]) len++;
    (void)gRT->SetVariable(L"RunwayWdtArmed", &guid, EFI_VARIABLE_BOOTSERVICE_ACCESS, len + 1, (void *)name);
}

EFI_STATUS wdt_pci_find(UINT16 vendor, UINT16 device, EFI_PCI_IO_PROTOCOL **out) {
    EFI_HANDLE *h = NULL;
    UINTN n = 0, i;
    EFI_STATUS s = gBS->LocateHandleBuffer(ByProtocol, &PciIoProtocol, NULL, &n, &h);

    if (EFI_ERROR(s)) return EFI_UNSUPPORTED;
    s = EFI_UNSUPPORTED;
    for (i = 0; i < n && EFI_ERROR(s); i++) {
        EFI_PCI_IO_PROTOCOL *pci;
        UINT16 id[2];

        if (EFI_ERROR(gBS->HandleProtocol(h[i], &PciIoProtocol, (VOID **)&pci))) continue;
        if (EFI_ERROR(pci->Pci.Read(pci, EfiPciIoWidthUint16, 0, 2, id))) continue;
        if (id[0] == vendor && id[1] == device) {
            *out = pci;
            s = EFI_SUCCESS;
        }
    }
    gBS->FreePool(h);
    return s;
}
