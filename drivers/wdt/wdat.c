/*
 * Watchdog driver application for platforms that describe their watchdog in
 * the ACPI WDAT table. It runs the table's actions to program the timer.
 * SPDX-License-Identifier: Apache-2.0
 * Load options: "arm <seconds>" or "stop".
 */
#include "wdt_common.h"
#include "efi_util.h"

#define ACPI20_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }

#define ACT_RESET               0x01
#define ACT_SET_COUNTDOWN       0x06
#define ACT_SET_RUNNING_STATE   0x09
#define ACT_SET_STOPPED_STATE   0x0b
#define ACT_SET_REBOOT          0x11

#define INST_READ_VALUE         0
#define INST_READ_COUNTDOWN     1
#define INST_WRITE_VALUE        2
#define INST_WRITE_COUNTDOWN    3
#define INST_OP_MASK            0x7f
#define INST_PRESERVE           0x80

#define SPACE_MEMORY            0
#define SPACE_IO                1

typedef struct {
    UINT8 signature[4];
    UINT32 length;
    UINT8 revision;
    UINT8 checksum;
    UINT8 oem_id[6];
    UINT8 oem_table_id[8];
    UINT32 oem_revision;
    UINT32 creator_id;
    UINT32 creator_revision;
} __attribute__((packed)) acpi_header_t;

typedef struct {
    UINT8 signature[8];
    UINT8 checksum;
    UINT8 oem_id[6];
    UINT8 revision;
    UINT32 rsdt_address;
    UINT32 length;
    UINT64 xsdt_address;
    UINT8 extended_checksum;
    UINT8 reserved[3];
} __attribute__((packed)) rsdp_t;

typedef struct {
    UINT8 space_id;
    UINT8 bit_width;
    UINT8 bit_offset;
    UINT8 access_size;
    UINT64 address;
} __attribute__((packed)) gas_t;

typedef struct {
    UINT8 action;
    UINT8 flags;
    UINT16 reserved;
    gas_t reg;
    UINT32 value;
    UINT32 mask;
} __attribute__((packed)) inst_t;

typedef struct {
    acpi_header_t header;
    UINT32 header_length;
    UINT16 pci_segment;
    UINT8 pci_bus;
    UINT8 pci_device;
    UINT8 pci_function;
    UINT8 reserved[3];
    UINT32 timer_period_ms;
    UINT32 max_count;
    UINT32 min_count;
    UINT8 flags;
    UINT8 reserved2[3];
    UINT32 entries;
} __attribute__((packed)) wdat_t;

static int sig_is(const UINT8 *p, const char *sig, UINTN n) {
    UINTN i;

    for (i = 0; i < n; i++)
        if (p[i] != (UINT8)sig[i]) return 0;
    return 1;
}

static int sum_is_zero(const void *p, UINTN n) {
    const UINT8 *b = p;
    UINT8 sum = 0;

    while (n--) sum += *b++;
    return sum == 0;
}

static const acpi_header_t *table_at(UINT64 addr) {
    const acpi_header_t *h = (const acpi_header_t *)(UINTN)addr;

    if (!addr || h->length < sizeof(*h) || !sum_is_zero(h, h->length)) return NULL;
    return h;
}

static const rsdp_t *find_rsdp(void) {
    EFI_GUID guid = ACPI20_GUID;
    UINTN i;

    for (i = 0; i < ST->NumberOfTableEntries; i++) {
        const rsdp_t *r = ST->ConfigurationTable[i].VendorTable;

        if (!guid_eq(&ST->ConfigurationTable[i].VendorGuid, &guid)) continue;
        if (sig_is(r->signature, "RSD PTR ", 8) && r->revision >= 2 &&
            sum_is_zero(r, 20) && sum_is_zero(r, r->length))
            return r;
    }
    return NULL;
}

/* The first WDAT table listed by the XSDT, checked for size and checksum. */
static const wdat_t *find_wdat(void) {
    const rsdp_t *rsdp = find_rsdp();
    const acpi_header_t *xsdt;
    const UINT64 *entry;
    UINTN i, n;

    if (!rsdp) return NULL;
    xsdt = table_at(rsdp->xsdt_address);
    if (!xsdt || !sig_is(xsdt->signature, "XSDT", 4)) return NULL;
    entry = (const UINT64 *)(xsdt + 1);
    n = (xsdt->length - sizeof(*xsdt)) / sizeof(UINT64);
    for (i = 0; i < n; i++) {
        const acpi_header_t *h = table_at(entry[i]);
        const wdat_t *w = (const wdat_t *)h;

        if (!h || !sig_is(h->signature, "WDAT", 4)) continue;
        if (h->length < sizeof(*w) ||
            (h->length - sizeof(*w)) / sizeof(inst_t) < w->entries)
            return NULL;
        return w;
    }
    return NULL;
}

static int reg_supported(const gas_t *g) {
    if (g->space_id != SPACE_MEMORY && g->space_id != SPACE_IO) return 0;
    if (g->bit_width != 8 && g->bit_width != 16 && g->bit_width != 32) return 0;
    return g->space_id == SPACE_MEMORY || g->address <= 0xffff;
}

static UINT32 reg_read(const gas_t *g) {
    UINT16 port = (UINT16)g->address;
    volatile void *m = (volatile void *)(UINTN)g->address;

    if (g->space_id == SPACE_IO)
        return g->bit_width == 8 ? inb(port) : g->bit_width == 16 ? inw(port) : inl(port);
    return g->bit_width == 8 ? *(volatile UINT8 *)m :
           g->bit_width == 16 ? *(volatile UINT16 *)m : *(volatile UINT32 *)m;
}

static void reg_write(const gas_t *g, UINT32 v) {
    UINT16 port = (UINT16)g->address;
    volatile void *m = (volatile void *)(UINTN)g->address;

    if (g->space_id == SPACE_IO) {
        if (g->bit_width == 8) outb(port, (UINT8)v);
        else if (g->bit_width == 16) outw(port, (UINT16)v);
        else outl(port, v);
    } else if (g->bit_width == 8) {
        *(volatile UINT8 *)m = (UINT8)v;
    } else if (g->bit_width == 16) {
        *(volatile UINT16 *)m = (UINT16)v;
    } else {
        *(volatile UINT32 *)m = v;
    }
}

static void exec_inst(const inst_t *in, UINT32 count) {
    UINT32 op = in->flags & INST_OP_MASK, x;

    if (op == INST_READ_VALUE || op == INST_READ_COUNTDOWN) {
        (void)reg_read(&in->reg);   /* arming has no use for the value */
        return;
    }
    x = (op == INST_WRITE_COUNTDOWN ? count : in->value) & in->mask;
    if (in->flags & INST_PRESERVE) x |= reg_read(&in->reg) & ~in->mask;
    reg_write(&in->reg, x);
}

/* EFI_NOT_FOUND: no entry for the action; EFI_UNSUPPORTED: an entry we cannot run. */
static EFI_STATUS check_action(const wdat_t *w, UINT8 action) {
    const inst_t *in = (const inst_t *)(w + 1);
    EFI_STATUS s = EFI_NOT_FOUND;
    UINT32 i;

    for (i = 0; i < w->entries; i++) {
        if (in[i].action != action) continue;
        if (!reg_supported(&in[i].reg)) return EFI_UNSUPPORTED;
        s = EFI_SUCCESS;
    }
    return s;
}

static void run_action(const wdat_t *w, UINT8 action, UINT32 count) {
    const inst_t *in = (const inst_t *)(w + 1);
    UINT32 i;

    for (i = 0; i < w->entries; i++)
        if (in[i].action == action) exec_inst(&in[i], count);
}

/* Required actions must exist; optional ones are skipped when absent. */
static EFI_STATUS check_actions(const wdat_t *w) {
    static const UINT8 required[] = { ACT_SET_COUNTDOWN, ACT_SET_RUNNING_STATE };
    static const UINT8 optional[] = { ACT_SET_REBOOT, ACT_RESET };
    UINTN i;
    EFI_STATUS s;

    for (i = 0; i < sizeof(required); i++) {
        s = check_action(w, required[i]);
        if (EFI_ERROR(s)) return s == EFI_NOT_FOUND ? EFI_UNSUPPORTED : s;
    }
    for (i = 0; i < sizeof(optional); i++) {
        s = check_action(w, optional[i]);
        if (s == EFI_UNSUPPORTED) return s;
    }
    return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE ih, EFI_SYSTEM_TABLE *st) {
    const wdat_t *w;
    UINT32 seconds = 0, count;
    UINT64 ticks;
    int is_stop = 0;
    EFI_STATUS s;

    util_init(ih, st);
    s = wdt_parse_options(ih, &is_stop, &seconds);
    if (EFI_ERROR(s)) return s;
    w = find_wdat();
    if (!w) return EFI_UNSUPPORTED;
    if (!w->timer_period_ms || !w->max_count) return EFI_DEVICE_ERROR;

    if (is_stop) {
        s = check_action(w, ACT_SET_STOPPED_STATE);
        if (EFI_ERROR(s)) return EFI_UNSUPPORTED;
        run_action(w, ACT_SET_STOPPED_STATE, 0);
        uprint("WDAT: stopped\n");
        return EFI_SUCCESS;
    }

    s = check_actions(w);
    if (EFI_ERROR(s)) return s;

    ticks = (UINT64)seconds * 1000 / w->timer_period_ms;
    if (ticks > w->max_count) ticks = w->max_count;
    if (ticks < w->min_count) ticks = w->min_count;
    count = (UINT32)ticks;

    run_action(w, ACT_SET_REBOOT, 0);
    run_action(w, ACT_SET_COUNTDOWN, count);
    run_action(w, ACT_SET_RUNNING_STATE, 0);
    run_action(w, ACT_RESET, 0);
    wdt_record_armed("WDAT");
    uprintf("WDAT: armed, %u x %u ms\n", (unsigned)count, (unsigned)w->timer_period_ms);
    return EFI_SUCCESS;
}
