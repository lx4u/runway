/*
 * rwy_diag.c — the machine as the firmware shows it, for the log of a debug build that cannot boot (see rwy_dbglog.h):
 * the firmware, the SMBIOS strings, the storage and USB controllers, and every disk and partition with its type.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_dbglog.h"
#include "rwy_partinfo.h"
#include "rwy_tpm.h"

#if defined(RWY_DEBUG_LOG)

#define GPT_NAME_OFFSET 56
#define GPT_NAME_CHARS 36

#define DISK_INFO_GUID \
    { 0xd432a67f, 0x14dc, 0x484b, { 0xb3, 0xbb, 0x3f, 0x02, 0x91, 0x84, 0x93, 0x27 } }
#define DISK_INFO_AHCI_GUID \
    { 0x9e498932, 0x4abc, 0x45af, { 0xa3, 0x4d, 0x02, 0x47, 0x78, 0x7b, 0xe7, 0xc6 } }
#define DISK_INFO_IDE_GUID \
    { 0x5e948fe3, 0x26d3, 0x42b5, { 0xaf, 0x17, 0x61, 0x02, 0x87, 0x18, 0x8d, 0xec } }
#define DISK_INFO_NVME_GUID \
    { 0x3ab14680, 0x5d3f, 0x4a4d, { 0xbc, 0xdc, 0xcc, 0x38, 0x00, 0x00, 0xc7, 0xf9 } }
#define DISK_INFO_USB_GUID \
    { 0xcb871572, 0xc11a, 0x47b5, { 0xb4, 0x92, 0x67, 0x5e, 0xaf, 0xa7, 0x77, 0x27 } }
#define IDENTIFY_MAX 4096

/* EFI_DISK_INFO_PROTOCOL */
typedef struct disk_info disk_info_t;
struct disk_info {
    EFI_GUID interface;
    EFI_STATUS (EFIAPI *inquiry)(disk_info_t *, VOID *, UINT32 *);
    EFI_STATUS (EFIAPI *identify)(disk_info_t *, VOID *, UINT32 *);
    EFI_STATUS (EFIAPI *sense)(disk_info_t *, VOID *, UINT32 *, UINT8 *);
    EFI_STATUS (EFIAPI *which_ide)(disk_info_t *, UINT32 *, UINT32 *);
};

/* A GPT GUID is stored with its first three fields little endian. */
static void guid_text(const UINT8 *g, char out[37]) {
    static const char hex[] = "0123456789ABCDEF";
    static const UINT8 order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    UINTN n = 0;

    for (UINTN i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[n++] = '-';
        out[n++] = hex[g[order[i]] >> 4];
        out[n++] = hex[g[order[i]] & 15];
    }
    out[n] = 0;
}

static void path_text(EFI_HANDLE h, char *out, UINTN cap) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH *dp = NULL;
    CHAR16 *w = NULL;
    UINTN i;

    out[0] = 0;
    if (EFI_ERROR(gBS->HandleProtocol(h, &dp_guid, (void **)&dp)) || !dp) return;
    w = DevicePathToStr(dp);
    if (!w) return;
    for (i = 0; w[i] && i + 1 < cap; i++) out[i] = w[i] < 128 ? (char)w[i] : '?';
    out[i] = 0;
    gBS->FreePool(w);
}

static void name_text(const UINT8 *entry, char out[GPT_NAME_CHARS + 1]) {
    UINTN i;

    for (i = 0; i < GPT_NAME_CHARS; i++) {
        UINT16 c = (UINT16)(entry[GPT_NAME_OFFSET + 2 * i] | (entry[GPT_NAME_OFFSET + 2 * i + 1] << 8));

        if (!c) break;
        out[i] = c < 128 ? (char)c : '?';
    }
    out[i] = 0;
}

static void print_partition_info(EFI_HANDLE h) {
    EFI_GUID info_guid = PARTITION_INFO_GUID;
    partition_info_t *info = NULL;
    char guid[37], name[GPT_NAME_CHARS + 1];

    if (EFI_ERROR(gBS->HandleProtocol(h, &info_guid, (void **)&info)) || !info) {
        EFI_GUID type;

        if (!rwy_partition_type(h, &type)) {
            uprint("RUNWAY: diag:   no partition info protocol, and no type in the GPT\n");
            return;
        }
        guid_text((const UINT8 *)&type, guid);
        uprintf("RUNWAY: diag:   no partition info protocol; GPT type %s (read from the table)\n", guid);
        return;
    }
    if (info->type != PARTITION_INFO_GPT) {
        uprintf("RUNWAY: diag:   partition info: %s\n", info->type == PARTITION_INFO_MBR ? "MBR" : "other");
        return;
    }
    guid_text(info->entry, guid);
    name_text(info->entry, name);
    uprintf("RUNWAY: diag:   partition info: GPT type %s name \"%s\"\n", guid, name);
}

static void text_field(const UINT8 *src, UINTN len, int swap, char *out) {
    UINTN n = 0;

    for (UINTN i = 0; i < len; i++) {
        UINT8 c = swap ? src[i ^ 1] : src[i];

        if (c >= ' ' && c < 127 && !(c == ' ' && (n == 0 || out[n - 1] == ' '))) out[n++] = (char)c;
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
}

/* Model and serial of the disk, from the identify data of the interface it is on. */
static void print_disk_info(EFI_HANDLE h) {
    EFI_GUID di_guid = DISK_INFO_GUID, ahci = DISK_INFO_AHCI_GUID, ide = DISK_INFO_IDE_GUID;
    EFI_GUID nvme = DISK_INFO_NVME_GUID, usb = DISK_INFO_USB_GUID;
    UINT8 buf[IDENTIFY_MAX];
    char model[64] = "", serial[32] = "", guid[37];
    const char *name = NULL;
    UINT32 size = sizeof(buf);
    disk_info_t *di = NULL;
    int ata, nv;

    if (EFI_ERROR(gBS->HandleProtocol(h, &di_guid, (void **)&di)) || !di) return;
    ata = !CompareMem(&di->interface, &ahci, sizeof(ahci)) || !CompareMem(&di->interface, &ide, sizeof(ide));
    nv = !CompareMem(&di->interface, &nvme, sizeof(nvme));
    if (ata) name = "SATA";
    else if (nv) name = "NVMe";
    else if (!CompareMem(&di->interface, &usb, sizeof(usb))) name = "USB";
    if ((ata || nv) && di->identify && !EFI_ERROR(di->identify(di, buf, &size))) {
        text_field(buf + (ata ? 54 : 24), 40, ata, model);
        text_field(buf + (ata ? 20 : 4), 20, ata, serial);
    }
    guid_text((const UINT8 *)&di->interface, guid);
    uprintf("RUNWAY: diag:   interface %s\n", name ? name : guid);
    if (model[0]) uprintf("RUNWAY: diag:   model \"%s\" serial \"%s\"\n", model, serial);
}

static void print_device(EFI_HANDLE h) {
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID, sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_BLOCK_IO_PROTOCOL *bio = NULL;
    VOID *sfs = NULL;
    char path[200];

    if (EFI_ERROR(gBS->HandleProtocol(h, &bio_guid, (void **)&bio)) || !bio || !bio->Media) return;
    path_text(h, path, sizeof(path));
    uprintf("RUNWAY: diag: %s\n", path);
    uprintf("RUNWAY: diag:   %s, block %u, %llu blocks, io align %u, %s%s\n",
            bio->Media->LogicalPartition ? "partition" : "disk", (unsigned)bio->Media->BlockSize,
            (unsigned long long)(bio->Media->LastBlock + 1), (unsigned)bio->Media->IoAlign,
            bio->Media->ReadOnly ? "read-only" : "writable", bio->Media->MediaPresent ? "" : ", no media");
    if (bio->Media->LogicalPartition) print_partition_info(h);
    else print_disk_info(h);
    uprintf("RUNWAY: diag:   file system: %s\n",
            EFI_ERROR(gBS->HandleProtocol(h, &sfs_guid, &sfs)) ? "no" : "yes");
}

static void hex_text(UINT32 v, int digits, char *out) {
    static const char hex[] = "0123456789abcdef";

    for (int i = 0; i < digits; i++) out[i] = hex[(v >> (4 * (digits - 1 - i))) & 15];
    out[digits] = 0;
}

static void print_firmware(void) {
    char vendor[64];
    UINTN i = 0;

    for (; gST->FirmwareVendor && gST->FirmwareVendor[i] && i < sizeof(vendor) - 1; i++)
        vendor[i] = gST->FirmwareVendor[i] < 128 ? (char)gST->FirmwareVendor[i] : '?';
    vendor[i] = 0;
    uprintf("RUNWAY: diag: firmware \"%s\" revision 0x%x, UEFI %u.%u\n", vendor, (unsigned)gST->FirmwareRevision,
            (unsigned)(gST->Hdr.Revision >> 16), (unsigned)((gST->Hdr.Revision & 0xffff) / 10));
}

/* The string `idx` of the SMBIOS structure `hdr` (they follow its formatted part, each ended by a NUL). */
static const char *smbios_string(const UINT8 *hdr, UINT8 idx) {
    const char *p = (const char *)hdr + hdr[1];

    if (!idx) return "";
    while (--idx && *p) p += strlen(p) + 1;
    return *p ? p : "";
}

/* The walk ends at the end-of-table structure or at the end of the table. */
static const UINT8 *smbios_next(const UINT8 *hdr, const UINT8 *end) {
    const UINT8 *p = hdr + hdr[1];

    while (p + 1 < end && (p[0] || p[1])) p++;
    return p + 2;
}

static void print_smbios_table(const UINT8 *p, const UINT8 *end) {
    while (p + 4 < end && p[0] != 127) {
        if (p[0] == 0)
            uprintf("RUNWAY: diag: BIOS \"%s\" version \"%s\"\n", smbios_string(p, p[4]), smbios_string(p, p[5]));
        if (p[0] == 1)
            uprintf("RUNWAY: diag: system \"%s\" \"%s\" version \"%s\"\n", smbios_string(p, p[4]),
                    smbios_string(p, p[5]), smbios_string(p, p[6]));
        p = smbios_next(p, end);
    }
}

static void print_smbios(void) {
    EFI_GUID g3 = SMBIOS3_TABLE_GUID, g2 = SMBIOS_TABLE_GUID;

    for (UINTN i = 0; i < gST->NumberOfTableEntries; i++) {
        const UINT8 *ep = gST->ConfigurationTable[i].VendorTable;
        const UINT8 *table;
        UINT32 len;

        if (!CompareMem(&gST->ConfigurationTable[i].VendorGuid, &g3, sizeof(g3))) {
            table = (const UINT8 *)(UINTN)(*(const UINT64 *)(ep + 0x10));
            len = *(const UINT32 *)(ep + 0x0c);
        } else if (!CompareMem(&gST->ConfigurationTable[i].VendorGuid, &g2, sizeof(g2))) {
            table = (const UINT8 *)(UINTN)(*(const UINT32 *)(ep + 0x18));
            len = *(const UINT16 *)(ep + 0x16);
        } else {
            continue;
        }
        print_smbios_table(table, table + len);
        return;
    }
    uprint("RUNWAY: diag: no SMBIOS table\n");
}

/* Storage (class 01) and USB (class 0C03) controllers: what the disks hang on. */
static void print_pci(void) {
    EFI_GUID pci_guid = EFI_PCI_IO_PROTOCOL_GUID;
    EFI_HANDLE *hs = NULL;
    UINTN n = 0;

    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &pci_guid, NULL, &n, &hs)) || !hs) return;
    for (UINTN i = 0; i < n; i++) {
        EFI_PCI_IO_PROTOCOL *pci = NULL;
        UINT32 id = 0, cls = 0;
        UINTN seg, bus, dev, fn;
        char ven[5], did[5], cc[5];

        if (EFI_ERROR(gBS->HandleProtocol(hs[i], &pci_guid, (void **)&pci)) || !pci) continue;
        if (EFI_ERROR(pci->Pci.Read(pci, EfiPciIoWidthUint32, 0, 1, &id)) ||
            EFI_ERROR(pci->Pci.Read(pci, EfiPciIoWidthUint32, 8, 1, &cls)))
            continue;
        cls >>= 16; /* base class and sub class */
        if ((cls >> 8) != 0x01 && cls != 0x0c03) continue;
        if (EFI_ERROR(pci->GetLocation(pci, &seg, &bus, &dev, &fn))) seg = bus = dev = fn = 0;
        hex_text(id & 0xffff, 4, ven);
        hex_text(id >> 16, 4, did);
        hex_text(cls, 4, cc);
        uprintf("RUNWAY: diag: pci %u:%u.%u %s:%s class %s\n", (unsigned)bus, (unsigned)dev, (unsigned)fn, ven, did,
                cc);
    }
    gBS->FreePool(hs);
}

static void print_tpm(void) {
    EFI_TCG2_PROTOCOL *tcg2 = NULL;
    EFI_TCG2_BOOT_SERVICE_CAPABILITY cap;
    EFI_PHYSICAL_ADDRESS log_loc = 0, log_last = 0;
    BOOLEAN log_trunc = FALSE;
    static const UINT32 pcrs[] = { 0, 1, 4, 7, 11 };

    if (EFI_ERROR(rwy_tpm_locate(&tcg2)) || !tcg2) {
        uprint("RUNWAY: diag: TPM: not present\n");
        return;
    }
    memset(&cap, 0, sizeof(cap));
    cap.Size = sizeof(cap);
    if (!EFI_ERROR(tcg2->GetCapability(tcg2, &cap))) {
        uprintf("RUNWAY: diag: TPM 2.0 active (mfg 0x%x, banks 0x%x)\n",
                (unsigned)cap.ManufacturerID, (unsigned)cap.ActivePcrBanks);
    } else {
        uprint("RUNWAY: diag: TPM 2.0 active\n");
    }
    if (!EFI_ERROR(tcg2->GetEventLog(tcg2, EFI_TCG2_EVENT_LOG_FORMAT_TCG_2,
                                     &log_loc, &log_last, &log_trunc))) {
        UINT64 len = (log_last >= log_loc) ? (log_last - log_loc) : 0;
        uprintf("RUNWAY: diag: TPM event log at 0x%llx (last 0x%llx, %llu bytes%s)\n",
                (unsigned long long)log_loc, (unsigned long long)log_last,
                (unsigned long long)len, log_trunc ? ", truncated" : "");
    }
    for (UINTN i = 0; i < sizeof(pcrs) / sizeof(pcrs[0]); i++) {
        UINT8 v[32];
        char h[65];

        if (EFI_ERROR(rwy_tpm_read_pcr(tcg2, pcrs[i], v))) continue;
        for (UINTN j = 0; j < 32; j++) {
            hex_text(v[j] >> 4, 1, &h[j * 2]);
            hex_text(v[j] & 15, 1, &h[j * 2 + 1]);
        }
        h[64] = 0;
        uprintf("RUNWAY: diag: PCR %u: %s\n", (unsigned)pcrs[i], h);
    }
}

void diag_firmware(void) {
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE *hs = NULL;
    UINTN n = 0;

    print_firmware();
    print_smbios();
    print_pci();
    print_tpm();
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &n, &hs)) || !hs) {
        uprint("RUNWAY: diag: no block device at all\n");
        return;
    }
    uprintf("RUNWAY: diag: %u block device handle(s)\n", (unsigned)n);
    for (UINTN i = 0; i < n; i++) print_device(hs[i]);
    gBS->FreePool(hs);
}

#endif
