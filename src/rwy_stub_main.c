/*
 * rwy_stub_main.c — Loader stub entry point (RUNWAY.EFI).
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal, frozen boot stub. Discovers the store partition or file on the booted
 * storage device, arbitrates loader bundle copies through the state machine,
 * arms trial watchdogs, sets the handshake variable, and runs the loader from
 * memory.
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_partscan.h"
#include "rwy_loader_fsm.h"
#include "rwy_bundle.h"
#include "rwy_format.h"

typedef struct {
    EFI_HANDLE            self;
    EFI_HANDLE            esp;
    const rwy_block_dev_t *dev;
} stub_ctx_t;

static UINTN wdt_arm_options(CHAR16 *out, uint32_t seconds) {
    char digits[16];
    UINTN n = 0, d = 0;
    const char *p = "arm ";

    while (*p) out[n++] = (CHAR16)*p++;
    do digits[d++] = (char)('0' + seconds % 10); while ((seconds /= 10));
    while (d) out[n++] = (CHAR16)digits[--d];
    out[n++] = 0;
    return n * sizeof(CHAR16);
}

static int probe_one_wdt(EFI_HANDLE self, const rwy_block_dev_t *dev, uint32_t base,
                         const rwy_bundle_entry_t *e, uint32_t timeout, char *out_name) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID var_guid = RWY_BOOT_VAR_GUID;
    EFI_LOADED_IMAGE *li;
    EFI_HANDLE child = NULL;
    CHAR16 opts[32];
    void *buf = NULL;
    size_t len = 0;
    UINTN name_sz = 32;
    EFI_STATUS s;

    if (gBS->AllocatePool(EfiBootServicesData, (UINTN)e->length, &buf) != EFI_SUCCESS)
        return 0;
    if (rwy_bundle_read_entry(dev, base, e, buf, (size_t)e->length, &len) != RWY_OK) {
        gBS->FreePool(buf);
        return 0;
    }

    s = gBS->LoadImage(FALSE, self, NULL, buf, (UINTN)len, &child);
    gBS->FreePool(buf);
    if (EFI_ERROR(s)) return 0;

    s = gBS->HandleProtocol(child, &li_guid, (void **)&li);
    if (!EFI_ERROR(s)) {
        li->LoadOptionsSize = (UINT32)wdt_arm_options(opts, timeout);
        li->LoadOptions = opts;
        s = gBS->StartImage(child, NULL, NULL);
    }
    if (child) gBS->UnloadImage(child);
    if (EFI_ERROR(s)) return 0;

    out_name[0] = '\0';
    (void)gRT->GetVariable(L"RunwayWdtArmed", &var_guid, NULL, &name_sz, out_name);
    return 1;
}

static void arm_trial_watchdogs(EFI_HANDLE self, const rwy_block_dev_t *dev,
                                const rwy_loader_fsm_plan_t *plan, char *out_name) {
    out_name[0] = '\0';
    gBS->SetWatchdogTimer(plan->timeout, 0x0000, 0, NULL);
    for (uint32_t i = 0; i < plan->header.count; i++) {
        if (plan->header.entry[i].kind == RWY_ENTRY_WDT) {
            if (probe_one_wdt(self, dev, plan->bundle_base, &plan->header.entry[i],
                              plan->timeout, out_name))
                break;
        }
    }
}

static int run_child_loader(EFI_HANDLE self, EFI_HANDLE esp, const rwy_block_dev_t *dev,
                            const rwy_loader_fsm_plan_t *plan) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID var_guid = RWY_BOOT_VAR_GUID;
    EFI_LOADED_IMAGE *child_li = NULL;
    EFI_HANDLE child = NULL;
    rwy_loader_handshake_t hs = plan->handshake;
    const rwy_bundle_entry_t *loader_entry = &plan->header.entry[0];
    void *buf = NULL;
    size_t len = 0;
    EFI_STATUS s;

    if (gBS->AllocatePool(EfiBootServicesData, (UINTN)loader_entry->length, &buf) != EFI_SUCCESS)
        return RWY_ERR_IO;
    if (rwy_bundle_read_entry(dev, plan->bundle_base, loader_entry, buf,
                              (size_t)loader_entry->length, &len) != RWY_OK) {
        gBS->FreePool(buf);
        return RWY_ERR_CORRUPT;
    }

    s = gBS->LoadImage(FALSE, self, NULL, buf, (UINTN)len, &child);
    gBS->FreePool(buf);
    if (EFI_ERROR(s)) return (int)s;

    s = gBS->HandleProtocol(child, &li_guid, (void **)&child_li);
    if (EFI_ERROR(s)) {
        gBS->UnloadImage(child);
        return (int)s;
    }
    child_li->DeviceHandle = esp;

    if (plan->is_trial) {
        char armed[32];
        arm_trial_watchdogs(self, dev, plan, armed);
        memcpy(hs.wdt_armed, armed, sizeof(hs.wdt_armed));
    }

    s = gRT->SetVariable(RWY_LOADER_HANDSHAKE_VAR, &var_guid,
                         EFI_VARIABLE_BOOTSERVICE_ACCESS, sizeof(hs), &hs);
    if (EFI_ERROR(s)) {
        gBS->UnloadImage(child);
        return (int)s;
    }

    s = gBS->StartImage(child, NULL, NULL);
    if (child) gBS->UnloadImage(child);
    return EFI_ERROR(s) ? (int)s : 0;
}

static int stub_child_launcher(void *ctx, const rwy_loader_fsm_plan_t *plan) {
    stub_ctx_t *sc = (stub_ctx_t *)ctx;
    return run_child_loader(sc->self, sc->esp, sc->dev, plan);
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *li = NULL;
    rwy_backend_t be;
    rwy_superblock_t sb;
    rwy_loader_fsm_t fsm;
    stub_ctx_t ctx;
    UINT64 gen = 0;
    int slot = 0, rc;
    EFI_STATUS s;

    util_init(ImageHandle, SystemTable);
    uprint("RUNWAY STUB\n");

    s = gBS->HandleProtocol(ImageHandle, &li_guid, (void **)&li);
    if (EFI_ERROR(s)) return ufail("RUNWAY STUB: no LoadedImage protocol", s);

    if (!rwy_scan_boot_store(li->DeviceHandle, &be, &sb, &slot, &gen))
        return ufail("RUNWAY STUB: boot store not found or invalid", EFI_NOT_FOUND);

    if (!sb.loader_blocks)
        return ufail("RUNWAY STUB: store has no loader area", EFI_NOT_FOUND);

    rc = rwy_loader_fsm_init(&fsm, &be.dev, sb.loader_start, sb.loader_blocks);
    if (rc != RWY_OK) return EFI_LOAD_ERROR;

    ctx.self = ImageHandle;
    ctx.esp = li->DeviceHandle;
    ctx.dev = &be.dev;

    rc = rwy_loader_fsm_run(&fsm, stub_child_launcher, &ctx);
    if (rc != 0) return EFI_LOAD_ERROR;
    return EFI_SUCCESS;
}
