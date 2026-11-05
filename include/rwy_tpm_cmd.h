/*
 * rwy_tpm_cmd — TPM 2.0 PCR sealing commands, independent of how they reach the TPM.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The caller supplies a transport (EFI_TCG2_PROTOCOL in RUNWAY.EFI, a device or a socket on Linux).
 */
#ifndef RWY_TPM_CMD_H
#define RWY_TPM_CMD_H

#include <stdint.h>

/* TPM 2.0 command and handle constants */
#define TPM_ST_NO_SESSIONS             0x8001
#define TPM_ST_SESSIONS                0x8002

#define TPM_CC_Startup                 0x00000144
#define TPM_CC_CreatePrimary           0x00000131
#define TPM_CC_Create                  0x00000153
#define TPM_CC_CreateLoaded            0x00000191
#define TPM_CC_Load                    0x00000157
#define TPM_CC_Unseal                  0x0000015E
#define TPM_CC_GetRandom               0x0000017B
#define TPM_CC_PCR_Read                0x0000017E
#define TPM_CC_PCR_Extend              0x00000182
#define TPM_CC_StartAuthSession        0x00000176
#define TPM_CC_PolicyPCR               0x0000017F
#define TPM_CC_FlushContext            0x00000165
#define TPM_CC_NV_UndefineSpace        0x00000122
#define TPM_CC_NV_DefineSpace          0x0000012A
#define TPM_CC_NV_Increment            0x00000134
#define TPM_CC_NV_Read                 0x0000014E

#define TPMA_NV_POLICYWRITE            0x00000008
#define TPMA_NV_COUNTER                0x00000010   /* type counter */
#define TPMA_NV_POLICYREAD             0x00080000

#define TPM_RC_SUCCESS                 0x00000000
#define TPM_RC_RETRY                   0x00000922
#define TPM_RC_NV_UNINITIALIZED        0x0000014A   /* the counter was defined but never incremented */
#define TPM_RC_HANDLE                  0x0000008B   /* format-one code: no such NV index */
#define TPM_RC_POLICY_FAIL             0x0000009D   /* format-one code: the parameter and session bits are masked */

#define TPM_ALG_ERROR                  0x0000
#define TPM_ALG_RSA                    0x0001
#define TPM_ALG_SHA1                   0x0004
#define TPM_ALG_HMAC                   0x0005
#define TPM_ALG_AES                    0x0006
#define TPM_ALG_KEYEDHASH              0x0008
#define TPM_ALG_SHA256                 0x000B
#define TPM_ALG_NULL                   0x0010
#define TPM_ALG_ECC                    0x0023
#define TPM_ALG_CFB                    0x0043

#define TPM_RH_OWNER                   0x40000001
#define TPM_RH_NULL                    0x40000007
#define TPM_RS_PW                      0x40000009

#define TPM_SE_POLICY                  0x01
#define TPM_SE_TRIAL                   0x03

/* Results of the functions below */
#define RWY_TPM_OK        0
#define RWY_TPM_EARG     (-1)   /* bad argument */
#define RWY_TPM_EIO      (-2)   /* transport failed, or the TPM kept asking to retry */
#define RWY_TPM_ERC      (-3)   /* the TPM refused a command (code in xport->rc) */
#define RWY_TPM_ESIZE    (-4)   /* a blob or a reply does not fit */
#define RWY_TPM_EPOLICY  (-5)   /* Unseal refused: PCR policy or authorization failed */
#define RWY_TPM_EUNSET   (-7)   /* an NV counter that was defined but never incremented */
#define RWY_TPM_ENOINDEX (-8)   /* an NV command named an index that is not defined */
#define RWY_TPM_EDATA    (-6)   /* Unseal returned something that is not a 32-byte key */

/*
 * Send in[0..in_len) and read the reply into out; *out_len is the room on entry and the reply
 * length on return. Returns 0, or non-zero when the TPM could not be reached.
 * A TPM_RC_RETRY reply is passed up as is; the transport may wait before returning it.
 */
typedef struct rwy_tpm_xport {
    int (*submit)(void *ctx, const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t *out_len);
    void *ctx;
    uint32_t rc;                 /* first error code the TPM returned in the last call below, else 0 */
} rwy_tpm_xport_t;

/* 32-byte SHA-256 value of one PCR. */
int rwy_tpmcmd_read_pcr(rwy_tpm_xport_t *x, uint32_t pcr_idx, uint8_t out_digest[32]);

/* Fetch random bytes from the TPM RNG. */
int rwy_tpmcmd_get_random(rwy_tpm_xport_t *x, void *buf, size_t len);

/* PolicyPCR digest for one SHA-256 PCR, in software. */
void rwy_tpmcmd_pcr_policy(uint32_t pcr_idx, const uint8_t pcr_val[32], uint8_t out_policy[32]);

/* Seal a 32-byte key under an SRK with the PCR policy; the blob is { priv_len, priv, pub_len, pub }. */
int rwy_tpmcmd_seal(rwy_tpm_xport_t *x, uint32_t pcr_idx, const uint8_t dek[32],
                     uint8_t *out_blob, uint32_t *out_blob_len, uint8_t out_pcr_val[32]);

/* Unseal with a policy session over the current PCR value. */
int rwy_tpmcmd_unseal(rwy_tpm_xport_t *x, uint32_t pcr_idx, const uint8_t *blob, uint32_t blob_len,
                       uint8_t out_dek[32]);

/*
 * NV counter (type counter, 8 bytes) at `index`. Read and increment need a policy session over `pcr_idx`;
 * the index has no policy delete, so only the owner can remove it. Define and undefine use the owner
 * hierarchy with an empty password. A new counter has no value until its first increment.
 */
/* Define binds the policy to `pcr_given` (32 bytes), or to the live PCR value when it is NULL. */
int rwy_tpmcmd_nv_define(rwy_tpm_xport_t *x, uint32_t index, uint32_t pcr_idx, const uint8_t *pcr_given);
int rwy_tpmcmd_nv_undefine(rwy_tpm_xport_t *x, uint32_t index);
int rwy_tpmcmd_nv_increment(rwy_tpm_xport_t *x, uint32_t index, uint32_t pcr_idx);
/* RWY_TPM_EUNSET: defined but never incremented. RWY_TPM_EPOLICY: the PCR no longer matches the policy of the index.
 * RWY_TPM_ENOINDEX: no such index. */
int rwy_tpmcmd_nv_read(rwy_tpm_xport_t *x, uint32_t index, uint32_t pcr_idx, uint64_t *value);

#endif /* RWY_TPM_CMD_H */
