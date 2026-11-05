/*
 * src/rwy_tpm_cmd.c — TPM 2.0 PCR sealing and unsealing commands for Runway.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Builds the commands and parses the replies; a transport (rwy_tpm_xport_t) carries them to the TPM.
 * Used by RUNWAY.EFI (through EFI_TCG2_PROTOCOL) and by Linux tools (through a device or a socket).
 */
#include <string.h>
#include "../include/rwy_tpm_cmd.h"
#include "../include/rwy_crypto.h"

static inline void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Send one command; TPM_RC_RETRY is tried again. RWY_TPM_ERC when the TPM answers with an error. */
static int tpm_cmd(rwy_tpm_xport_t *x, const uint8_t *cmd, uint32_t len,
                   uint8_t *resp, uint32_t cap, uint32_t *rlen) {
    for (int retry = 0; retry < 20; retry++) {
        uint32_t n = cap;
        if (x->submit(x->ctx, cmd, len, resp, &n) || n < 10) return RWY_TPM_EIO;
        uint32_t rc = get_u32(resp + 6);
        if (rc == TPM_RC_RETRY) continue;
        if (rc && !x->rc) x->rc = rc;
        if (rlen) *rlen = n;
        return rc ? RWY_TPM_ERC : RWY_TPM_OK;
    }
    return RWY_TPM_EIO;
}

/* Bit of a PCR in the 3-byte selection mask. */
static void pcr_select(uint8_t sel[3], uint32_t pcr_idx) {
    sel[0] = (pcr_idx < 8) ? (uint8_t)(1 << pcr_idx) : 0;
    sel[1] = (pcr_idx >= 8 && pcr_idx < 16) ? (uint8_t)(1 << (pcr_idx - 8)) : 0;
    sel[2] = (pcr_idx >= 16 && pcr_idx < 24) ? (uint8_t)(1 << (pcr_idx - 16)) : 0;
}

int rwy_tpmcmd_read_pcr(rwy_tpm_xport_t *x, uint32_t pcr_idx, uint8_t out_digest[32]) {
    uint8_t cmd[64];
    uint8_t resp[128];
    uint32_t rlen = 0;
    int r;

    if (!x || !out_digest) return RWY_TPM_EARG;
    x->rc = 0;

    /* TPM2_PCR_Read: pcrSelectionIn: count=1, alg=SHA256, sizeOfSelect=3 */
    put_u16(cmd + 0, TPM_ST_NO_SESSIONS);
    put_u32(cmd + 2, 20);
    put_u32(cmd + 6, TPM_CC_PCR_Read);
    put_u32(cmd + 10, 1);
    put_u16(cmd + 14, TPM_ALG_SHA256);
    cmd[16] = 3;
    pcr_select(cmd + 17, pcr_idx);

    r = tpm_cmd(x, cmd, 20, resp, sizeof(resp), &rlen);
    if (r) return r;

    /* 10B header, 4B pcrUpdateCounter, 10B pcrSelectionOut, 4B count + 2B size + digest */
    if (rlen < 10 + 4 + 10 + 6 + 32) return RWY_TPM_ESIZE;

    memcpy(out_digest, resp + rlen - 32, 32);
    return RWY_TPM_OK;
}

int rwy_tpmcmd_get_random(rwy_tpm_xport_t *x, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    uint8_t cmd[12];
    uint8_t resp[128];
    uint32_t rlen;
    int r;

    if (!x || (!buf && len > 0)) return RWY_TPM_EARG;
    x->rc = 0;

    put_u16(cmd + 0, TPM_ST_NO_SESSIONS);
    put_u32(cmd + 2, 12);
    put_u32(cmd + 6, TPM_CC_GetRandom);

    while (len > 0) {
        uint16_t req = (len > 32) ? 32 : (uint16_t)len;
        put_u16(cmd + 10, req);
        rlen = 0;
        r = tpm_cmd(x, cmd, 12, resp, sizeof(resp), &rlen);
        if (r) return r;
        if (rlen < 12) return RWY_TPM_ESIZE;
        uint16_t got = get_u16(resp + 10);
        if (got == 0 || (uint32_t)(12 + got) > rlen) return RWY_TPM_EDATA;
        if (got > len) got = (uint16_t)len;
        memcpy(p, resp + 12, got);
        p += got;
        len -= got;
    }
    return RWY_TPM_OK;
}

void rwy_tpmcmd_pcr_policy(uint32_t pcr_idx, const uint8_t pcr_val[32], uint8_t out_policy[32]) {
    /* policyDigest_new = SHA256(policyDigest_old || TPM_CC_PolicyPCR || pcrSelection || pcrDigest) */
    uint8_t pcr_digest[32];
    rwy_sha256(pcr_val, 32, pcr_digest);

    uint8_t buffer[32 + 4 + 10 + 32];
    memset(buffer, 0, 32); /* policyDigest_old: 32 zeroes */

    put_u32(buffer + 32, TPM_CC_PolicyPCR);
    put_u32(buffer + 36, 1);
    put_u16(buffer + 40, TPM_ALG_SHA256);
    buffer[42] = 3;
    pcr_select(buffer + 43, pcr_idx);

    memcpy(buffer + 46, pcr_digest, 32);
    rwy_sha256(buffer, sizeof(buffer), out_policy);
}

static void flush_handle(rwy_tpm_xport_t *x, uint32_t handle) {
    uint8_t cmd[14];
    uint8_t resp[16];
    put_u16(cmd + 0, TPM_ST_NO_SESSIONS);
    put_u32(cmd + 2, 14);
    put_u32(cmd + 6, TPM_CC_FlushContext);
    put_u32(cmd + 10, handle);
    tpm_cmd(x, cmd, 14, resp, sizeof(resp), NULL);
}

static int create_primary_srk(rwy_tpm_xport_t *x, uint32_t *out_srk_handle) {
    uint8_t cmd[128];
    uint8_t resp[512];
    uint32_t rlen = 0;
    int r;

    *out_srk_handle = 0;

    /* TPM2_CreatePrimary under TPM_RH_OWNER: standard RSA 2048 SRK template */
    static const uint8_t in_pub[] = {
        0x00, 0x1a, /* 26 bytes public area */
        0x00, 0x01, /* type: RSA */
        0x00, 0x0b, /* nameAlg: SHA256 */
        0x00, 0x03, 0x00, 0x72, /* attrs: fixedTPM | fixedParent | sensDataOrigin | userWithAuth | restricted | decrypt */
        0x00, 0x00, /* authPolicy: 0 */
        0x00, 0x06, 0x00, 0x80, 0x00, 0x43, /* sym: AES-128-CFB */
        0x00, 0x10, /* scheme: NULL */
        0x08, 0x00, /* keyBits: 2048 */
        0x00, 0x00, 0x00, 0x00, /* exponent: 0 */
        0x00, 0x00  /* unique: 0 */
    };

    uint32_t off = 0;
    put_u16(cmd + off, TPM_ST_SESSIONS); off += 2;
    off += 4; /* skip size for now */
    put_u32(cmd + off, TPM_CC_CreatePrimary); off += 4;
    put_u32(cmd + off, TPM_RH_OWNER); off += 4;

    /* Auth area: 9 bytes password auth */
    put_u32(cmd + off, 9); off += 4;
    put_u32(cmd + off, TPM_RS_PW); off += 4;
    put_u16(cmd + off, 0); off += 2;
    cmd[off++] = 0; /* sessionAttributes = 0 */
    put_u16(cmd + off, 0); off += 2;

    /* inSensitive: 4 bytes empty userAuth + data */
    put_u16(cmd + off, 4); off += 2;
    put_u16(cmd + off, 0); off += 2;
    put_u16(cmd + off, 0); off += 2;

    memcpy(cmd + off, in_pub, sizeof(in_pub)); off += sizeof(in_pub);

    /* outsideInfo: 0, creationPCR: 0 */
    put_u16(cmd + off, 0); off += 2;
    put_u32(cmd + off, 0); off += 4;

    put_u32(cmd + 2, off); /* fill total command size */

    r = tpm_cmd(x, cmd, off, resp, sizeof(resp), &rlen);
    if (r) return r;
    if (rlen < 14) return RWY_TPM_ESIZE;

    *out_srk_handle = get_u32(resp + 10);
    return RWY_TPM_OK;
}

int rwy_tpmcmd_seal(rwy_tpm_xport_t *x, uint32_t pcr_idx, const uint8_t dek[32],
                     uint8_t *out_blob, uint32_t *out_blob_len, uint8_t out_pcr_val[32]) {
    uint8_t pcr_val[32];
    uint8_t pcr_policy[32];
    uint32_t srk_handle = 0;
    uint8_t cmd[512];
    uint8_t resp[512];
    uint32_t rlen = 0;
    int r;

    if (!x || !dek || !out_blob || !out_blob_len) return RWY_TPM_EARG;
    x->rc = 0;

    r = rwy_tpmcmd_read_pcr(x, pcr_idx, pcr_val);
    if (r) return r;
    if (out_pcr_val) memcpy(out_pcr_val, pcr_val, 32);

    rwy_tpmcmd_pcr_policy(pcr_idx, pcr_val, pcr_policy);

    r = create_primary_srk(x, &srk_handle);
    if (r) return r;

    /* TPM2_Create: keyedhash object sealed with pcr_policy */
    uint32_t off = 0;
    put_u16(cmd + off, TPM_ST_SESSIONS); off += 2;
    off += 4; /* skip size */
    put_u32(cmd + off, TPM_CC_Create); off += 4;
    put_u32(cmd + off, srk_handle); off += 4;

    /* Auth area: 9 bytes password auth */
    put_u32(cmd + off, 9); off += 4;
    put_u32(cmd + off, TPM_RS_PW); off += 4;
    put_u16(cmd + off, 0); off += 2;
    cmd[off++] = 0;
    put_u16(cmd + off, 0); off += 2;

    /* inSensitive: 2B userAuth(0) + 2B data_len(32) + 32B DEK = 36B */
    put_u16(cmd + off, 36); off += 2;
    put_u16(cmd + off, 0); off += 2;
    put_u16(cmd + off, 32); off += 2;
    memcpy(cmd + off, dek, 32); off += 32;

    /* inPublic: KeyedHash sealed object (46 bytes) */
    put_u16(cmd + off, 46); off += 2;
    put_u16(cmd + off, TPM_ALG_KEYEDHASH); off += 2;
    put_u16(cmd + off, TPM_ALG_SHA256); off += 2;
    put_u32(cmd + off, 0x00000012); off += 4;        /* fixedTPM | fixedParent */
    put_u16(cmd + off, 32); off += 2;
    memcpy(cmd + off, pcr_policy, 32); off += 32;
    put_u16(cmd + off, TPM_ALG_NULL); off += 2;
    put_u16(cmd + off, 0); off += 2;                 /* unique len = 0 */

    /* outsideInfo: 0, creationPCR: 0 */
    put_u16(cmd + off, 0); off += 2;
    put_u32(cmd + off, 0); off += 4;

    put_u32(cmd + 2, off);

    r = tpm_cmd(x, cmd, off, resp, sizeof(resp), &rlen);
    flush_handle(x, srk_handle);
    if (r) return r;

    /* Response has parameterSize (uint32) followed by outPrivate and outPublic */
    uint32_t roff = 10 + 4; /* skip 10B header + 4B param_size */
    if (rlen < roff + 2) return RWY_TPM_ESIZE;
    uint16_t priv_len = get_u16(resp + roff);
    const uint8_t *priv_2b = resp + roff;
    roff += 2 + priv_len;

    if (rlen < roff + 2) return RWY_TPM_ESIZE;
    uint16_t pub_len = get_u16(resp + roff);
    const uint8_t *pub_2b = resp + roff;
    roff += 2 + pub_len;
    if (rlen < roff) return RWY_TPM_ESIZE;

    uint32_t total_blob = (2 + priv_len) + (2 + pub_len);
    if (total_blob > 364) return RWY_TPM_ESIZE;

    memcpy(out_blob, priv_2b, 2 + priv_len);
    memcpy(out_blob + 2 + priv_len, pub_2b, 2 + pub_len);
    *out_blob_len = total_blob;

    return RWY_TPM_OK;
}

/* Start a policy session and bind it to the current value of one PCR; the session is flushed on failure. */
static int policy_pcr_session(rwy_tpm_xport_t *x, uint32_t pcr_idx, uint32_t *out_sess) {
    uint8_t cmd[64];
    uint8_t resp[64];
    uint32_t rlen = 0, sess, off = 0;
    int r;

    /* TPM2_StartAuthSession (policy session) */
    put_u16(cmd + off, TPM_ST_NO_SESSIONS); off += 2;
    off += 4;
    put_u32(cmd + off, TPM_CC_StartAuthSession); off += 4;
    put_u32(cmd + off, TPM_RH_NULL); off += 4; /* tpmKey: NULL */
    put_u32(cmd + off, TPM_RH_NULL); off += 4; /* bind: NULL */
    put_u16(cmd + off, 16); off += 2;          /* nonceCaller: 16 bytes */
    for (int i = 0; i < 16; i++) cmd[off++] = 0x5a;
    put_u16(cmd + off, 0); off += 2;          /* encryptedSalt: 0 */
    cmd[off++] = TPM_SE_POLICY;
    put_u16(cmd + off, TPM_ALG_NULL); off += 2;   /* symmetric = NULL */
    put_u16(cmd + off, TPM_ALG_SHA256); off += 2; /* authHash */

    put_u32(cmd + 2, off);
    r = tpm_cmd(x, cmd, off, resp, sizeof(resp), &rlen);
    if (r) return r;
    if (rlen < 14) return RWY_TPM_ESIZE;
    sess = get_u32(resp + 10);

    /* TPM2_PolicyPCR */
    off = 0;
    put_u16(cmd + off, TPM_ST_NO_SESSIONS); off += 2;
    off += 4;
    put_u32(cmd + off, TPM_CC_PolicyPCR); off += 4;
    put_u32(cmd + off, sess); off += 4;
    put_u16(cmd + off, 0); off += 2; /* pcrDigest len = 0 (TPM reads current PCR) */
    put_u32(cmd + off, 1); off += 4; /* pcrSelection: count = 1 */
    put_u16(cmd + off, TPM_ALG_SHA256); off += 2;
    cmd[off++] = 3; /* sizeOfSelect = 3 */
    pcr_select(cmd + off, pcr_idx); off += 3;

    put_u32(cmd + 2, off);
    r = tpm_cmd(x, cmd, off, resp, sizeof(resp), NULL);
    if (r) {
        flush_handle(x, sess);
        return r;
    }
    *out_sess = sess;
    return RWY_TPM_OK;
}

/* Authorization area with one session that has no nonce, attributes or hmac. */
static uint32_t put_auth(uint8_t *cmd, uint32_t off, uint32_t session) {
    put_u32(cmd + off, 9);
    put_u32(cmd + off + 4, session);
    put_u16(cmd + off + 8, 0);
    cmd[off + 10] = 0;
    put_u16(cmd + off + 11, 0);
    return off + 13;
}

/* A sealed blob is a TPM2B private followed by a TPM2B public. */
static int sealed_blob_ok(const uint8_t *blob, uint32_t blob_len) {
    uint16_t priv_len = get_u16(blob), pub_len;

    if (blob_len < (uint32_t)(2 + priv_len + 2)) return RWY_TPM_ESIZE;
    pub_len = get_u16(blob + 2 + priv_len);
    if (blob_len < (uint32_t)(2 + priv_len + 2 + pub_len)) return RWY_TPM_ESIZE;
    return RWY_TPM_OK;
}

/* TPM2_Load the sealed object under the SRK. */
static int load_sealed(rwy_tpm_xport_t *x, uint32_t srk_handle, const uint8_t *blob, uint32_t *loaded) {
    uint8_t cmd[512], resp[512];
    uint32_t rlen = 0, off = 0;
    uint16_t priv_len = get_u16(blob), pub_len = get_u16(blob + 2 + priv_len);
    int r;

    put_u16(cmd + off, TPM_ST_SESSIONS); off += 2;
    off += 4;
    put_u32(cmd + off, TPM_CC_Load); off += 4;
    put_u32(cmd + off, srk_handle); off += 4;
    off = put_auth(cmd, off, TPM_RS_PW);
    /* inPrivate and inPublic */
    memcpy(cmd + off, blob, 2 + priv_len); off += 2 + priv_len;
    memcpy(cmd + off, blob + 2 + priv_len, 2 + pub_len); off += 2 + pub_len;
    put_u32(cmd + 2, off);

    r = tpm_cmd(x, cmd, off, resp, sizeof(resp), &rlen);
    if (r || rlen < 14) return r ? r : RWY_TPM_ESIZE;
    *loaded = get_u32(resp + 10);
    return RWY_TPM_OK;
}

/* TPM2_Unseal with the policy session; the DEK comes back in the response. */
static int unseal_loaded(rwy_tpm_xport_t *x, uint32_t loaded_handle, uint32_t sess_handle, uint8_t out_dek[32]) {
    uint8_t cmd[512], resp[512];
    uint32_t rlen = 0, off = 0;
    int r;

    put_u16(cmd + off, TPM_ST_SESSIONS); off += 2;
    off += 4;
    put_u32(cmd + off, TPM_CC_Unseal); off += 4;
    put_u32(cmd + off, loaded_handle); off += 4;
    off = put_auth(cmd, off, sess_handle);
    put_u32(cmd + 2, off);
    r = tpm_cmd(x, cmd, off, resp, sizeof(resp), &rlen);
    if (r == RWY_TPM_ERC) return RWY_TPM_EPOLICY;  /* policy mismatch (tamper) or bad auth */
    if (r) return r;

    /* 10B header, 4B parameterSize, 2B data_len, 32B DEK */
    if (rlen < 10 + 4 + 2 || get_u16(resp + 10 + 4) != 32 || rlen < 10 + 4 + 2 + 32) return RWY_TPM_EDATA;
    memcpy(out_dek, resp + 10 + 4 + 2, 32);
    return RWY_TPM_OK;
}

int rwy_tpmcmd_unseal(rwy_tpm_xport_t *x, uint32_t pcr_idx, const uint8_t *blob, uint32_t blob_len,
                       uint8_t out_dek[32]) {
    uint32_t srk_handle = 0, loaded_handle = 0, sess_handle = 0;
    int r;

    if (!x || !blob || blob_len < 4 || !out_dek) return RWY_TPM_EARG;
    x->rc = 0;

    r = sealed_blob_ok(blob, blob_len);
    if (r) return r;
    r = create_primary_srk(x, &srk_handle);
    if (r) return r;
    r = load_sealed(x, srk_handle, blob, &loaded_handle);
    if (r) {
        flush_handle(x, srk_handle);
        return r;
    }
    r = policy_pcr_session(x, pcr_idx, &sess_handle);
    if (r) {
        flush_handle(x, loaded_handle);
        flush_handle(x, srk_handle);
        return r;
    }
    r = unseal_loaded(x, loaded_handle, sess_handle, out_dek);
    flush_handle(x, sess_handle);
    flush_handle(x, loaded_handle);
    flush_handle(x, srk_handle);
    return r;
}

int rwy_tpmcmd_nv_define(rwy_tpm_xport_t *x, uint32_t index, uint32_t pcr_idx, const uint8_t *pcr_given) {
    uint8_t cmd[128], resp[64], pcr_val[32];
    uint32_t off = 0, rlen = 0;
    int r;

    if (!x) return RWY_TPM_EARG;
    x->rc = 0;
    if (pcr_given) memcpy(pcr_val, pcr_given, 32);
    else if ((r = rwy_tpmcmd_read_pcr(x, pcr_idx, pcr_val))) return r;

    put_u16(cmd + off, TPM_ST_SESSIONS); off += 2;
    off += 4;
    put_u32(cmd + off, TPM_CC_NV_DefineSpace); off += 4;
    put_u32(cmd + off, TPM_RH_OWNER); off += 4;
    off = put_auth(cmd, off, TPM_RS_PW);
    put_u16(cmd + off, 0); off += 2;                 /* auth value of the index: empty */
    put_u16(cmd + off, 46); off += 2;                /* TPM2B_NV_PUBLIC */
    put_u32(cmd + off, index); off += 4;
    put_u16(cmd + off, TPM_ALG_SHA256); off += 2;
    put_u32(cmd + off, TPMA_NV_POLICYWRITE | TPMA_NV_COUNTER | TPMA_NV_POLICYREAD); off += 4;
    put_u16(cmd + off, 32); off += 2;
    rwy_tpmcmd_pcr_policy(pcr_idx, pcr_val, cmd + off); off += 32;
    put_u16(cmd + off, 8); off += 2;                 /* dataSize */
    put_u32(cmd + 2, off);
    return tpm_cmd(x, cmd, off, resp, sizeof(resp), &rlen);
}

int rwy_tpmcmd_nv_undefine(rwy_tpm_xport_t *x, uint32_t index) {
    uint8_t cmd[64], resp[64];
    uint32_t off = 0, rlen = 0;

    if (!x) return RWY_TPM_EARG;
    x->rc = 0;
    put_u16(cmd + off, TPM_ST_SESSIONS); off += 2;
    off += 4;
    put_u32(cmd + off, TPM_CC_NV_UndefineSpace); off += 4;
    put_u32(cmd + off, TPM_RH_OWNER); off += 4;
    put_u32(cmd + off, index); off += 4;
    off = put_auth(cmd, off, TPM_RS_PW);
    put_u32(cmd + 2, off);
    return tpm_cmd(x, cmd, off, resp, sizeof(resp), &rlen);
}

/* NV_Increment or NV_Read of `index`, authorized by a policy session over the PCR. `params` follow the auth area. */
static int nv_policy_cmd(rwy_tpm_xport_t *x, uint32_t cc, uint32_t index, uint32_t pcr_idx,
                         const uint8_t *params, uint32_t plen, uint8_t *resp, uint32_t cap, uint32_t *rlen) {
    uint8_t cmd[64];
    uint32_t sess, off = 0;
    int r = policy_pcr_session(x, pcr_idx, &sess);

    if (r) return r;
    put_u16(cmd + off, TPM_ST_SESSIONS); off += 2;
    off += 4;
    put_u32(cmd + off, cc); off += 4;
    put_u32(cmd + off, index); off += 4;             /* authHandle */
    put_u32(cmd + off, index); off += 4;             /* nvIndex */
    off = put_auth(cmd, off, sess);
    memcpy(cmd + off, params, plen); off += plen;
    put_u32(cmd + 2, off);
    r = tpm_cmd(x, cmd, off, resp, cap, rlen);
    flush_handle(x, sess);
    if (r == RWY_TPM_ERC && (x->rc & 0xBF) == TPM_RC_POLICY_FAIL) return RWY_TPM_EPOLICY;
    if (r == RWY_TPM_ERC && (x->rc & 0xBF) == TPM_RC_HANDLE) return RWY_TPM_ENOINDEX;
    return r;
}

int rwy_tpmcmd_nv_increment(rwy_tpm_xport_t *x, uint32_t index, uint32_t pcr_idx) {
    uint8_t resp[64];
    uint32_t rlen = 0;

    if (!x) return RWY_TPM_EARG;
    x->rc = 0;
    return nv_policy_cmd(x, TPM_CC_NV_Increment, index, pcr_idx, NULL, 0, resp, sizeof(resp), &rlen);
}

int rwy_tpmcmd_nv_read(rwy_tpm_xport_t *x, uint32_t index, uint32_t pcr_idx, uint64_t *value) {
    static const uint8_t params[4] = { 0, 8, 0, 0 };   /* size 8, offset 0 */
    uint8_t resp[64];
    uint32_t rlen = 0;
    int r;

    if (!x || !value) return RWY_TPM_EARG;
    x->rc = 0;
    r = nv_policy_cmd(x, TPM_CC_NV_Read, index, pcr_idx, params, sizeof(params), resp, sizeof(resp), &rlen);
    if (r == RWY_TPM_ERC && x->rc == TPM_RC_NV_UNINITIALIZED) return RWY_TPM_EUNSET;
    if (r) return r;
    /* 10B header, 4B parameterSize, 2B size, 8B value */
    if (rlen < 10 + 4 + 2 + 8 || get_u16(resp + 14) != 8) return RWY_TPM_EDATA;
    *value = ((uint64_t)get_u32(resp + 16) << 32) | get_u32(resp + 20);
    return RWY_TPM_OK;
}
