/*
 * rwy_unlock.c — Unlock the DEK of a volume group: unseal it, or enroll it on the first boot.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_boot.h"
#include "rwy_tpm.h"
#include "rwy_fs.h"
#include "../include/efi_tcg2.h"
#include "../include/rwy_crypto.h"

/* Read a member's keyslot header from the newest valid superblock slot.
 * Returns 1 = valid encrypted header, 0 = no keyslot header (plaintext),
 * -1 = header corrupt or no valid superblock slot. */
static int read_keyslot_hdr(const mirror_member_t *m, rwy_keyslot_header_t *out) {
    int rc = rwy_keyslot_read_from_dev(&m->backend.dev, out);
    if (rc == RWY_CRYPTO_OK) return 1;
    if (rc == RWY_CRYPTO_ERR_EMPTY) return 0;
    return -1;
}



/* Does the key of one member match the master's? *same is set when its sealed blob is the master's. */
static BOOLEAN member_dek_matches(const rwy_keyslot_header_t *h, const rwy_keyslot_header_t *mhdr,
                                  const UINT8 *dek, EFI_TCG2_PROTOCOL *tcg2) {
    static const char transport_pass[] = "runway-test-passphrase";
    UINT8 mdek[32];
    BOOLEAN have = FALSE, match;

    if (h->slots[0].state == RWY_KEYSLOT_STATE_ACTIVE) {
        if (rwy_keyslot_unwrap(h, transport_pass, mdek) == RWY_CRYPTO_OK) have = TRUE;
    } else if (h->slots[1].state == RWY_KEYSLOT_STATE_ACTIVE &&
               h->slots[1].slot_type == RWY_KEYSLOT_TYPE_TPM2) {
        if (mhdr->slots[1].state == RWY_KEYSLOT_STATE_ACTIVE &&
            mhdr->slots[1].blob_len == h->slots[1].blob_len &&
            CompareMem(mhdr->slots[1].blob, h->slots[1].blob, h->slots[1].blob_len) == 0)
            return TRUE; /* same sealed blob as the master */
        if (tcg2 && !EFI_ERROR(rwy_tpm_unseal(tcg2, 7, h->slots[1].blob, h->slots[1].blob_len, mdek)))
            have = TRUE;
    }
    match = have && CompareMem(mdek, dek, 32) == 0;
    memset(mdek, 0, sizeof(mdek));
    return match;
}

/* Every mirror member must hold the same DEK as the master (or none, if the
 * master is plaintext). Checked before enrollment rewrites member headers, so
 * a foreign member is rejected instead of having its keyslots overwritten. */

static EFI_STATUS check_member_keys(vol_group_t *g, UINTN master_idx, BOOLEAN master_encrypted,
                                    const rwy_keyslot_header_t *mhdr, const UINT8 *dek,
                                    EFI_TCG2_PROTOCOL *tcg2) {
    for (UINTN m = 0; m < g->count; m++) {
        rwy_keyslot_header_t h;
        int r;
        if (m == master_idx) continue;
        r = read_keyslot_hdr(&g->members[m], &h);
        if (r < 0) return mfail("mirror member keyslot header corrupt", EFI_SECURITY_VIOLATION);
        if ((r == 1) != master_encrypted)
            return mfail("mirror member encryption differs from master", EFI_SECURITY_VIOLATION);
        if (g->members[m].gen < g->members[master_idx].gen)
            continue;
        if (master_encrypted && !member_dek_matches(&h, mhdr, dek, tcg2))
            return mfail("mirror member has a different DEK than master", EFI_SECURITY_VIOLATION);
    }
    return EFI_SUCCESS;
}

/* Normal boot: the DEK is sealed in keyslot 1 and comes back only if PCR 7 is unchanged. */
static EFI_STATUS unseal_dek(vol_group_t *g, UINTN master_idx, const rwy_keyslot_header_t *khdr,
                             EFI_TCG2_PROTOCOL *tcg2, EFI_STATUS tpm_stat, UINT8 *dek) {
    EFI_STATUS s;

    if (EFI_ERROR(tpm_stat) || !tcg2)
        return mfail("TPM 2.0 required for unsealing but not available", EFI_SECURITY_VIOLATION);
    RWY_DEBUG("tpm2: unsealing DEK using PCR 7 policy...\n");
    s = rwy_tpm_unseal(tcg2, 7, khdr->slots[1].blob, khdr->slots[1].blob_len, dek);
    if (EFI_ERROR(s)) {
        RWY_ERROR("SECURITY ALERT: TPM 2.0 unseal failed! PCR 7 mismatch / tamper detected.\n");
        return mfail("TPM 2.0 unseal failed (tamper detected)", EFI_SECURITY_VIOLATION);
    }
    RWY_DEBUG("tpm2: hardware unseal successful (PCR 7 verified)\n");
    rwy_tpm_print_pcr7(tcg2);
    return check_member_keys(g, master_idx, TRUE, khdr, dek, tcg2);
}

/* Find out whether the master is encrypted and get its DEK (sealed in the TPM, or the factory key
 * on the first boot). Every other member must agree. */
EFI_STATUS unlock_group(vol_group_t *g, UINTN master_idx, BOOLEAN *is_encrypted, UINT8 *dek,
                        BOOLEAN *needs_enroll) {
    rwy_keyslot_header_t khdr;
    EFI_TCG2_PROTOCOL *tcg2 = NULL;
    EFI_STATUS tpm_stat;
    int kr = read_keyslot_hdr(&g->members[master_idx], &khdr);

    *is_encrypted = FALSE;
    *needs_enroll = FALSE;
    if (kr < 0)
        return mfail("keyslot header corrupt (refusing to treat store as plaintext)", EFI_SECURITY_VIOLATION);
    if (kr == 0)
        return check_member_keys(g, master_idx, FALSE, NULL, NULL, NULL);
    *is_encrypted = TRUE;
    RWY_DEBUG("crypto: store encrypted with AES-256-XTS\n");
    tpm_stat = rwy_tpm_locate(&tcg2);
    if (khdr.slots[1].state == RWY_KEYSLOT_STATE_ACTIVE && khdr.slots[1].slot_type == RWY_KEYSLOT_TYPE_TPM2)
        return unseal_dek(g, master_idx, &khdr, tcg2, tpm_stat, dek);
    if (khdr.slots[0].state == RWY_KEYSLOT_STATE_ACTIVE) {
        static const char transport_pass[] = "runway-test-passphrase";
        RWY_DEBUG("tpm2: enrollment phase: unwrapping factory transport key...\n");
        if (rwy_keyslot_unwrap(&khdr, transport_pass, dek) != RWY_CRYPTO_OK)
            return mfail("factory transport passphrase rejected", EFI_ACCESS_DENIED);
        if (EFI_ERROR(tpm_stat) || !tcg2)
            return mfail("TPM 2.0 required for enrollment but not available", EFI_DEVICE_ERROR);
        EFI_STATUS s = check_member_keys(g, master_idx, TRUE, &khdr, dek, tcg2);
        if (EFI_ERROR(s)) return s;
        *needs_enroll = TRUE;
        return EFI_SUCCESS;
    }
    return mfail("no usable keyslot found for encrypted volume", EFI_SECURITY_VIOLATION);
}
