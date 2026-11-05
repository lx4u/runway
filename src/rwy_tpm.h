/*
 * src/rwy_tpm.h — UEFI TPM 2.0 interface for Runway keyslot sealing/unsealing.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_TPM_H
#define RWY_TPM_H

#include <efi.h>
#include <efilib.h>
#include "../include/efi_tcg2.h"
#include "../include/rwy_crypto.h"
#include "../include/rwy_format.h"

/* Locate EFI_TCG2_PROTOCOL if available and verify TPM is active. */
EFI_STATUS rwy_tpm_locate(EFI_TCG2_PROTOCOL **out_tcg2);

/* Read 32-byte SHA-256 value of a PCR (e.g. PCR 7). */
EFI_STATUS rwy_tpm_read_pcr(EFI_TCG2_PROTOCOL *tcg2, UINT32 pcr_idx, UINT8 out_digest[32]);

/* Compute PCR policy digest in software for a single SHA-256 PCR value. */
void rwy_tpm_compute_pcr_policy(UINT32 pcr_idx, const UINT8 pcr_val[32], UINT8 out_policy[32]);

/* Seal 32-byte DEK against specified PCR index (e.g. PCR 7) policy.
 * Generates/uses primary SRK under TPM_RH_OWNER, creates keyedhash object,
 * and writes serialized { priv_len (2B), priv, pub_len (2B), pub } into out_blob. */
EFI_STATUS rwy_tpm_seal(EFI_TCG2_PROTOCOL *tcg2, UINT32 pcr_idx,
                         const UINT8 dek[32], UINT8 *out_blob, UINT32 *out_blob_len,
                         UINT8 out_pcr_val[32]);

/* Unseal 32-byte DEK using sealed blob and asserting current PCR state.
 * Returns EFI_SUCCESS and out_dek on match, or EFI_SECURITY_VIOLATION / error on tamper. */
EFI_STATUS rwy_tpm_unseal(EFI_TCG2_PROTOCOL *tcg2, UINT32 pcr_idx,
                           const UINT8 *blob, UINT32 blob_len, UINT8 out_dek[32]);

/* Read and increment the NV counter `index` under a policy session over `pcr_idx`. nv_read returns
 * EFI_NOT_READY for a counter that was defined but never incremented and EFI_NOT_FOUND when the index is not defined. */
EFI_STATUS rwy_tpm_nv_read(EFI_TCG2_PROTOCOL *tcg2, UINT32 index, UINT32 pcr_idx, UINT64 *value);
EFI_STATUS rwy_tpm_nv_increment(EFI_TCG2_PROTOCOL *tcg2, UINT32 index, UINT32 pcr_idx);

/* Fetch random bytes from the TPM RNG. */
EFI_STATUS rwy_tpm_get_random(EFI_TCG2_PROTOCOL *tcg2, void *buf, size_t len);
int rwy_tpm_random(void *buf, size_t len);

/* Log PCR 7 value. */
void rwy_tpm_print_pcr7(EFI_TCG2_PROTOCOL *tcg2);

#endif /* RWY_TPM_H */
