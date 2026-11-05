/*
 * rwy_grpmount.c — Mount the volume groups and bind them to the boot handles.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_boot.h"
#include "rwy_tpm.h"
#include "../include/rwy_crypto.h"

/* The member with the highest generation is the master; returns that generation. */
static UINT64 pick_master(const vol_group_t *g, UINTN *master_idx) {
    UINT64 max_gen = 0;

    *master_idx = 0;
    for (UINTN i = 0; i < g->count; i++) {
        if (g->members[i].gen > max_gen) {
            max_gen = g->members[i].gen;
            *master_idx = i;
        }
    }
    RWY_DEBUG("mirror: authoritative master is member %u (generation %llu, slot %c)\n",
            (unsigned int)*master_idx, (unsigned long long)max_gen,
            g->members[*master_idx].slot == 0 ? 'A' : 'B');
    return max_gen;
}

/* Seal runtime DEK under PCR 7 and verify unseal round-trip. */
static EFI_STATUS seal_and_verify_dek(EFI_TCG2_PROTOCOL *tcg2, const UINT8 runtime_dek[32],
                                      rwy_keyslot_header_t *khdr) {
    UINT8 blob[364], pcr7_val[32], verify_dek[32];
    UINT32 blob_len = 0;
    EFI_STATUS s;

    s = rwy_tpm_seal(tcg2, 7, runtime_dek, blob, &blob_len, pcr7_val);
    if (EFI_ERROR(s)) return s;

    s = rwy_tpm_unseal(tcg2, 7, blob, blob_len, verify_dek);
    if (EFI_ERROR(s)) return s;

    if (CompareMem(verify_dek, runtime_dek, 32) != 0) {
        memset(verify_dek, 0, sizeof(verify_dek));
        return EFI_SECURITY_VIOLATION;
    }
    memset(verify_dek, 0, sizeof(verify_dek));

    if (rwy_keyslot_init_header(khdr) != RWY_CRYPTO_OK)
        return EFI_DEVICE_ERROR;
    if (rwy_keyslot_set_tpm2_blob(khdr, 1, blob, blob_len, pcr7_val) != RWY_CRYPTO_OK)
        return EFI_DEVICE_ERROR;
    return EFI_SUCCESS;
}

/* First-boot re-encryption: seals runtime DEK into TPM2, relocates store and cleans old keys. */
static EFI_STATUS reencrypt_and_enroll(vol_group_t *g, UINT8 active_dek[32]) {
    EFI_TCG2_PROTOCOL *tcg2 = NULL;
    rwy_keyslot_header_t khdr;
    UINT8 runtime_dek[32];
    EFI_STATUS s;
    int rc;

    for (UINTN i = 0; i < g->count; i++) {
        if (g->members[i].degraded) {
            RWY_WARN("first-boot re-encryption failed, retrying at next boot (mirror degraded)\n");
            return EFI_SUCCESS;
        }
    }
    if (EFI_ERROR(rwy_tpm_locate(&tcg2)) || !tcg2) {
        RWY_WARN("first-boot re-encryption failed, retrying at next boot (TPM unavailable)\n");
        return EFI_SUCCESS;
    }
    if (rwy_random(runtime_dek, sizeof(runtime_dek)) != 0) {
        RWY_WARN("first-boot re-encryption failed, retrying at next boot (random failed)\n");
        return EFI_SUCCESS;
    }
    s = seal_and_verify_dek(tcg2, runtime_dek, &khdr);
    if (EFI_ERROR(s)) {
        memset(runtime_dek, 0, sizeof(runtime_dek));
        RWY_WARN("first-boot re-encryption failed, retrying at next boot (seal failed: 0x%llx)\n",
                  (unsigned long long)s);
        return EFI_SUCCESS;
    }

    gBS->SetWatchdogTimer(0, 0, 0, NULL);
    g->inst->mirror_ctx.write_policy = RWY_MIRROR_WRITE_REQUIRE_ALL;
    if (rwy_in_txn(&g->inst->store)) rwy_abort(&g->inst->store);

    RWY_INFO("reencrypt: copying\n");
    UINT64 t0 = tsc_now();
    rc = rwy_reencrypt_relocate(&g->inst->store, &g->inst->block_dev, active_dek, runtime_dek, &khdr);
    UINT64 t1 = tsc_now();

    gBS->SetWatchdogTimer(300, 0, 0, NULL);
    g->inst->mirror_ctx.write_policy = RWY_MIRROR_WRITE_BEST_EFFORT;

    if (rc != RWY_OK) {
        (void)rwy_begin(&g->inst->store);
        memset(runtime_dek, 0, sizeof(runtime_dek));
        RWY_WARN("first-boot re-encryption failed, retrying at next boot (%s)\n", rwy_strerror(rc));
        return EFI_SUCCESS;
    }

    double copy_sec = tsc_to_seconds(t1 - t0);
    RWY_INFO("reencrypt: copy completed in %llu.%03llu s\n",
              (unsigned long long)(UINT64)copy_sec,
              (unsigned long long)((UINT64)(copy_sec * 1000.0) % 1000u));

    RWY_INFO("reencrypt: finalizing\n");
    rc = rwy_reencrypt_finish(&g->inst->store, active_dek);
    if (rc != RWY_OK) {
        (void)rwy_begin(&g->inst->store);
        memset(runtime_dek, 0, sizeof(runtime_dek));
        RWY_WARN("first-boot re-encryption finalize failed, retrying at next boot (%s)\n", rwy_strerror(rc));
        return EFI_SUCCESS;
    }

    memcpy(active_dek, runtime_dek, 32);
    static const char hex[] = "0123456789abcdef";
    char dek_hex[65];
    for (int i = 0; i < 32; i++) {
        dek_hex[2 * i] = hex[runtime_dek[i] >> 4];
        dek_hex[2 * i + 1] = hex[runtime_dek[i] & 15];
    }
    dek_hex[64] = 0;
    RWY_DEBUG("tpm2: runtime DEK = %s\n", dek_hex);
    memset(runtime_dek, 0, sizeof(runtime_dek));
    (void)rwy_begin(&g->inst->store);
    RWY_INFO("tpm2: enrolled DEK sealed in Keyslot 1, Slot 0 cleared across all members\n");
    rwy_tpm_print_pcr7(tcg2);
    return EFI_SUCCESS;
}

/* Resume pending finalize from an interrupted re-encryption. */
static void resume_pending_finalize(vol_group_t *g) {
    int pending = 0;
    if (!g->inst || rwy_reencrypt_pending(&g->inst->store, &pending) != RWY_OK || !pending)
        return;
    RWY_INFO("reencrypt: resuming pending finalize from previous boot\n");
    if (rwy_in_txn(&g->inst->store)) rwy_abort(&g->inst->store);
    (void)rwy_reencrypt_finish(&g->inst->store, NULL);
    (void)rwy_begin(&g->inst->store);
}

/* Put what happened to the members into the state record. A failure here does not stop the boot. */
static void note_member_events(vol_group_t *g, const UINT8 *dek) {
    rwy_backend_t bes[RWY_MIRROR_MAX_DEVS];
    UINT32 events[RWY_MIRROR_MAX_DEVS];
    UINTN n = g->count < RWY_MIRROR_MAX_DEVS ? g->count : RWY_MIRROR_MAX_DEVS;

    for (UINTN i = 0; i < n; i++) {
        bes[i] = g->members[i].backend;
        events[i] = g->members[i].events;
    }
    if (EFI_ERROR(rwy_fs_note_events(g->inst, bes, events, n, dek)))
        RWY_WARN("mirror: health events could not be recorded\n");
}

/* The newest state record may sit on any member; spread it before a catch-up overwrites one. */
static EFI_STATUS spread_records(vol_group_t *g, UINTN master_idx, const UINT8 *dek) {
    rwy_backend_t bes[RWY_MIRROR_MAX_DEVS];
    EFI_STATUS s;

    if (g->count < 2) return EFI_SUCCESS;
    for (UINTN i = 0; i < g->count && i < RWY_MIRROR_MAX_DEVS; i++) bes[i] = g->members[i].backend;
    s = rwy_fs_sync_records(bes, g->count, dek, g->members[master_idx].sb.rec_start);
    return EFI_ERROR(s) ? mfail("state record synchronization failed", s) : s;
}

/* Mount: reconcile generations, unlock crypto, and mount a
 * rwy_fs_instance_t over a mirror (>1 member) or single (1 member) device.
 * Does NOT touch the handle database. */
static EFI_STATUS mount_group(vol_group_t *g) {
    EFI_STATUS s;
    UINTN master_idx;
    UINT64 max_gen = pick_master(g, &master_idx);
    BOOLEAN is_encrypted = FALSE, needs_enroll = FALSE;
    UINT8 active_dek[32];
    rwy_backend_t backings[RWY_MIRROR_MAX_DEVS];
    EFI_HANDLE handles[RWY_MIRROR_MAX_DEVS];
    UINTN num_backings = 1;

    memset(active_dek, 0, sizeof(active_dek));
    s = unlock_group(g, master_idx, &is_encrypted, active_dek, &needs_enroll);
    if (EFI_ERROR(s)) goto out;

    s = spread_records(g, master_idx, is_encrypted ? active_dek : NULL);
    if (EFI_ERROR(s)) goto out;
    s = catch_up_members(g, master_idx, max_gen, is_encrypted, active_dek);
    if (EFI_ERROR(s)) goto out;

    /* The authoritative master is the primary backing */
    backings[0] = g->members[master_idx].backend;
    handles[0] = g->members[master_idx].handle;
    for (UINTN i = 0; i < g->count; i++) {
        if (i == master_idx || g->members[i].degraded) continue;
        handles[num_backings] = g->members[i].handle;
        backings[num_backings++] = g->members[i].backend;
    }
    RWY_DEBUG("mirror: active mirror array initialized with %u device(s) (primary: member %u)\n",
            (unsigned int)num_backings, (unsigned int)master_idx);

    s = rwy_fs_mount_group(backings, handles, num_backings,
                            is_encrypted ? active_dek : NULL, &g->inst);
    if (EFI_ERROR(s)) {
        s = mfail("rwy_fs_mount_group failed", s);
        goto out;
    }

    if (needs_enroll)
        s = reencrypt_and_enroll(g, active_dek);
    else if (is_encrypted)
        resume_pending_finalize(g);
    if (!EFI_ERROR(s)) note_member_events(g, is_encrypted ? active_dek : NULL);

out:
    memset(active_dek, 0, sizeof(active_dek));
    return s;
}

/* The member that gets the file protocol: the one on the loading ESP, else one on its disk (consumers such as
 * EFI Boot Guard only consider volumes on the boot medium), else the primary. */
static void bind_view(rwy_fs_instance_t *inst, EFI_HANDLE esp) {
    UINTN pick = 0;

    for (UINTN m = 0; m < inst->num_backings; m++) {
        if (inst->member_handles[m] == esp) {
            pick = m;
            break;
        }
        if (pick == 0 && same_disk(inst->member_handles[m], esp)) pick = m;
    }
    rwy_fs_select_view(inst, pick, same_disk(inst->member_handles[pick], esp) ? esp : NULL);
}

/* Say which backend the booted store runs on. */
static void log_backend(const vol_group_t *g) {
    if (g->members[0].backend.kind == RWY_BACKEND_BLOCK)
        RWY_INFO("store: partition, block backend\n");
    else {
        RWY_INFO("store: RUNWAY.DAT, file backend\n");
        RWY_INFO("store: size guard on RUNWAY.DAT at %llu bytes\n",
                 (unsigned long long)g->members[0].backend.dev.size);
    }
}

/* Mount an instance per volume group. Only the boot store is mandatory: when it fails, EFI_NOT_FOUND is
 * returned with its status in *boot_err and nothing is printed. */
EFI_STATUS mount_all_groups(EFI_HANDLE fat_fs_handle, EFI_STATUS *boot_err) {
    for (UINTN gi = 0; gi < gGroupCount; gi++) {
        EFI_STATUS s = mount_group(&gGroups[gi]);
        if (EFI_ERROR(s)) {
            /* The boot store is the only one we cannot do without: hand over to the firmware. */
            if (gGroups[gi].has_boot) {
                *boot_err = s;
                return EFI_NOT_FOUND;
            }
            RWY_WARN("volume group %u cannot be mounted (0x%llx), skipped\n", (unsigned int)gi,
                    (unsigned long long)s);
            gGroups[gi].inst = NULL;
            continue;
        }
        gGroups[gi].inst->is_boot_volume = gGroups[gi].has_boot;
        bind_view(gGroups[gi].inst, fat_fs_handle);
        if (gGroups[gi].has_boot) log_backend(&gGroups[gi]);
    }
    return EFI_SUCCESS;
}

/* Reinstall SFS on every enrolled partition handle. */
EFI_STATUS install_all_groups(void) {
    for (UINTN gi = 0; gi < gGroupCount; gi++) {
        EFI_STATUS s;
        if (!gGroups[gi].inst) continue;   /* skipped: could not be mounted */
        s = rwy_fs_install_group(gGroups[gi].inst);
        if (EFI_ERROR(s)) return ufail("rwy_fs_install_group failed", s);
        RWY_DEBUG("filesystem protocol: group %u installed on %u partition handle(s)\n",
                (unsigned int)gi, (unsigned int)gGroups[gi].inst->num_backings);
    }
    return EFI_SUCCESS;
}
