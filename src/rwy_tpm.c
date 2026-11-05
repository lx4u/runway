/*
 * src/rwy_tpm.c — TPM 2.0 PCR 7 sealing and unsealing for RUNWAY.EFI.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Finds EFI_TCG2_PROTOCOL and carries the commands of rwy_tpm_cmd.c to the TPM through it.
 */
#include <string.h>
#include "rwy_tpm.h"
#include "efi_util.h"
#include "../include/efi_tcg2.h"
#include "../include/rwy_tpm_cmd.h"

static int tcg2_submit(void *ctx, const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t *out_len) {
    EFI_TCG2_PROTOCOL *tcg2 = ctx;
    EFI_STATUS s = tcg2->SubmitCommand(tcg2, in_len, (UINT8 *)in, *out_len, out);
    uint32_t n;

    if (EFI_ERROR(s) || *out_len < 10) return -1;
    n = ((uint32_t)out[2] << 24) | ((uint32_t)out[3] << 16) | ((uint32_t)out[4] << 8) | out[5];
    if (n < 10 || n > *out_len) return -1;
    *out_len = n;
    if ((((uint32_t)out[6] << 24) | ((uint32_t)out[7] << 16) | ((uint32_t)out[8] << 8) | out[9]) == TPM_RC_RETRY)
        gBS->Stall(50000); /* 50ms before the caller tries again */
    return 0;
}

static EFI_STATUS to_efi(int r) {
    switch (r) {
    case RWY_TPM_OK:      return EFI_SUCCESS;
    case RWY_TPM_EARG:    return EFI_INVALID_PARAMETER;
    case RWY_TPM_ESIZE:   return EFI_BAD_BUFFER_SIZE;
    case RWY_TPM_EPOLICY: return EFI_SECURITY_VIOLATION;
    case RWY_TPM_EDATA:   return EFI_COMPROMISED_DATA;
    case RWY_TPM_EUNSET:  return EFI_NOT_READY;
    case RWY_TPM_ENOINDEX: return EFI_NOT_FOUND;
    default:               return EFI_DEVICE_ERROR;
    }
}

EFI_STATUS rwy_tpm_locate(EFI_TCG2_PROTOCOL **out_tcg2) {
    EFI_STATUS s;
    EFI_GUID tcg2_guid = EFI_TCG2_PROTOCOL_GUID;
    EFI_TCG2_PROTOCOL *tcg2 = NULL;
    EFI_TCG2_BOOT_SERVICE_CAPABILITY cap;

    if (!out_tcg2) return EFI_INVALID_PARAMETER;
    *out_tcg2 = NULL;

    s = gBS->LocateProtocol(&tcg2_guid, NULL, (void **)&tcg2);
    if (EFI_ERROR(s) || !tcg2) {
        return EFI_NOT_FOUND;
    }

    memset(&cap, 0, sizeof(cap));
    cap.Size = sizeof(cap);
    s = tcg2->GetCapability(tcg2, &cap);
    if (EFI_ERROR(s) || !cap.TPMPresentFlag) {
        return EFI_NOT_FOUND;
    }

    *out_tcg2 = tcg2;
    return EFI_SUCCESS;
}

EFI_STATUS rwy_tpm_read_pcr(EFI_TCG2_PROTOCOL *tcg2, UINT32 pcr_idx, UINT8 out_digest[32]) {
    rwy_tpm_xport_t x = { tcg2_submit, tcg2, 0 };
    return to_efi(rwy_tpmcmd_read_pcr(&x, pcr_idx, out_digest));
}

void rwy_tpm_compute_pcr_policy(UINT32 pcr_idx, const UINT8 pcr_val[32], UINT8 out_policy[32]) {
    rwy_tpmcmd_pcr_policy(pcr_idx, pcr_val, out_policy);
}

EFI_STATUS rwy_tpm_seal(EFI_TCG2_PROTOCOL *tcg2, UINT32 pcr_idx,
                         const UINT8 dek[32], UINT8 *out_blob, UINT32 *out_blob_len,
                         UINT8 out_pcr_val[32]) {
    rwy_tpm_xport_t x = { tcg2_submit, tcg2, 0 };
    return to_efi(rwy_tpmcmd_seal(&x, pcr_idx, dek, out_blob, out_blob_len, out_pcr_val));
}

EFI_STATUS rwy_tpm_unseal(EFI_TCG2_PROTOCOL *tcg2, UINT32 pcr_idx,
                           const UINT8 *blob, UINT32 blob_len, UINT8 out_dek[32]) {
    rwy_tpm_xport_t x = { tcg2_submit, tcg2, 0 };
    return to_efi(rwy_tpmcmd_unseal(&x, pcr_idx, blob, blob_len, out_dek));
}

EFI_STATUS rwy_tpm_nv_read(EFI_TCG2_PROTOCOL *tcg2, UINT32 index, UINT32 pcr_idx, UINT64 *value) {
    rwy_tpm_xport_t x = { tcg2_submit, tcg2, 0 };
    return to_efi(rwy_tpmcmd_nv_read(&x, index, pcr_idx, value));
}

EFI_STATUS rwy_tpm_nv_increment(EFI_TCG2_PROTOCOL *tcg2, UINT32 index, UINT32 pcr_idx) {
    rwy_tpm_xport_t x = { tcg2_submit, tcg2, 0 };
    return to_efi(rwy_tpmcmd_nv_increment(&x, index, pcr_idx));
}

EFI_STATUS rwy_tpm_get_random(EFI_TCG2_PROTOCOL *tcg2, void *buf, size_t len) {
    rwy_tpm_xport_t x = { tcg2_submit, tcg2, 0 };
    return to_efi(rwy_tpmcmd_get_random(&x, buf, len));
}

int rwy_tpm_random(void *buf, size_t len) {
    EFI_TCG2_PROTOCOL *tcg2 = NULL;
    if (EFI_ERROR(rwy_tpm_locate(&tcg2)) || !tcg2) return -1;
    return EFI_ERROR(rwy_tpm_get_random(tcg2, buf, len)) ? -1 : 0;
}

void rwy_tpm_print_pcr7(EFI_TCG2_PROTOCOL *tcg2) {
    static const char hex[] = "0123456789abcdef";
    UINT8 v[32];
    char h[65];
    if (!tcg2 || EFI_ERROR(rwy_tpm_read_pcr(tcg2, 7, v))) return;
    for (int i = 0; i < 32; i++) {
        h[2 * i] = hex[v[i] >> 4];
        h[2 * i + 1] = hex[v[i] & 15];
    }
    h[64] = 0;
    RWY_DEBUG("tpm2: PCR 7 = %s\n", h);
}
