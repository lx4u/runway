/*
 * src/rwy_keyslot.c — keyslot header: init, wrap and unwrap of the data key.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "rwy_crypto.h"
#include "rwy_core.h"
#include "rwy_format.h"

int rwy_keyslot_init_header(rwy_keyslot_header_t *hdr) {
    if (!hdr) return RWY_CRYPTO_ERR_INVAL;
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = RWY_KEYSLOT_MAGIC;
    hdr->version = 1;
    hdr->crypt_algo = RWY_CRYPT_AES_XTS_256;
    hdr->slot_count = RWY_KEYSLOT_COUNT;
    for (uint32_t i = 0; i < RWY_KEYSLOT_COUNT; ++i) {
        hdr->slots[i].slot_type = i;
        hdr->slots[i].state = RWY_KEYSLOT_STATE_EMPTY;
    }
    hdr->crc32c = rwy_crc32c(hdr, sizeof(*hdr));
    return RWY_CRYPTO_OK;
}

static void derive_kek(const char *passphrase, const uint8_t *salt, uint32_t iterations,
                       uint8_t kek1[32], uint8_t kek2[32]) {
    uint8_t kek[64];
    rwy_pbkdf2_sha256(passphrase, strlen(passphrase), salt, RWY_KEY_SALT_SIZE,
                       iterations, kek, 64);
    memcpy(kek1, kek, 32);
    memcpy(kek2, kek + 32, 32);
}

int rwy_keyslot_set_password(rwy_keyslot_header_t *hdr, uint32_t slot_idx,
                             const char *passphrase, const uint8_t dek[32],
                             uint32_t iterations) {
    if (!hdr || !passphrase || !dek || slot_idx >= RWY_KEYSLOT_COUNT)
        return RWY_CRYPTO_ERR_INVAL;

    rwy_keyslot_t *slot = &hdr->slots[slot_idx];
    memset(slot, 0, sizeof(*slot));
    slot->slot_type = RWY_KEYSLOT_TYPE_PASSWORD;
    slot->state = RWY_KEYSLOT_STATE_ACTIVE;
    slot->kdf_type = RWY_KDF_PBKDF2_SHA256;
    slot->kdf_iterations = iterations ? iterations : 1000;

    /* Generate pseudo-random salt for salt determinism or test uniqueness */
    for (size_t i = 0; i < sizeof(slot->salt); ++i)
        slot->salt[i] = (uint8_t)((slot_idx * 37 + i * 53 + 0xa5) & 0xff);

    uint8_t kek1[32], kek2[32];
    derive_kek(passphrase, slot->salt, slot->kdf_iterations, kek1, kek2);

    /* Encrypt DEK with KEK1 */
    for (int i = 0; i < 32; ++i)
        slot->wrapped_key[i] = dek[i] ^ kek1[i];

    /* Authenticate DEK with HMAC-SHA256(KEK2, DEK) */
    rwy_hmac_sha256(kek2, 32, dek, 32, slot->wrapped_key + 32);

    hdr->crc32c = 0;
    hdr->crc32c = rwy_crc32c(hdr, sizeof(*hdr));
    return RWY_CRYPTO_OK;
}

int rwy_keyslot_set_tpm2_blob(rwy_keyslot_header_t *hdr, uint32_t slot_idx,
                              const uint8_t *blob, uint32_t blob_len,
                              const uint8_t pcr_mask[32]) {
    if (!hdr || slot_idx >= RWY_KEYSLOT_COUNT) return RWY_CRYPTO_ERR_INVAL;
    rwy_keyslot_t *slot = &hdr->slots[slot_idx];
    memset(slot, 0, sizeof(*slot));
    slot->slot_type = RWY_KEYSLOT_TYPE_TPM2;
    slot->state = (blob && blob_len > 0) ? RWY_KEYSLOT_STATE_ACTIVE : RWY_KEYSLOT_STATE_EMPTY;
    slot->kdf_type = RWY_KDF_NONE;
    if (pcr_mask) memcpy(slot->pcr_mask, pcr_mask, sizeof(slot->pcr_mask));
    if (blob && blob_len <= sizeof(slot->blob)) {
        slot->blob_len = blob_len;
        memcpy(slot->blob, blob, blob_len);
    }
    hdr->crc32c = 0;
    hdr->crc32c = rwy_crc32c(hdr, sizeof(*hdr));
    return RWY_CRYPTO_OK;
}

int rwy_keyslot_set_recovery_key(rwy_keyslot_header_t *hdr, uint32_t slot_idx,
                                 const char *recovery_pass, const uint8_t dek[32]) {
    if (!hdr || !recovery_pass || !dek || slot_idx >= RWY_KEYSLOT_COUNT)
        return RWY_CRYPTO_ERR_INVAL;

    rwy_keyslot_t *slot = &hdr->slots[slot_idx];
    memset(slot, 0, sizeof(*slot));
    slot->slot_type = RWY_KEYSLOT_TYPE_RECOVERY;
    slot->state = RWY_KEYSLOT_STATE_ACTIVE;
    slot->kdf_type = RWY_KDF_PBKDF2_SHA256;
    slot->kdf_iterations = 1000;

    for (size_t i = 0; i < sizeof(slot->salt); ++i)
        slot->salt[i] = (uint8_t)((slot_idx * 79 + i * 17 + 0x5a) & 0xff);

    uint8_t kek1[32], kek2[32];
    derive_kek(recovery_pass, slot->salt, slot->kdf_iterations, kek1, kek2);

    for (int i = 0; i < 32; ++i)
        slot->wrapped_key[i] = dek[i] ^ kek1[i];
    rwy_hmac_sha256(kek2, 32, dek, 32, slot->wrapped_key + 32);

    hdr->crc32c = 0;
    hdr->crc32c = rwy_crc32c(hdr, sizeof(*hdr));
    return RWY_CRYPTO_OK;
}

int rwy_keyslot_unwrap(const rwy_keyslot_header_t *hdr, const char *passphrase,
                        uint8_t out_dek[32]) {
    if (!hdr || !passphrase || !out_dek) return RWY_CRYPTO_ERR_INVAL;
    if (hdr->magic != RWY_KEYSLOT_MAGIC) return RWY_CRYPTO_ERR_CORRUPT;

    /* Verify header CRC */
    rwy_keyslot_header_t tmp = *hdr;
    tmp.crc32c = 0;
    if (rwy_crc32c(&tmp, sizeof(tmp)) != hdr->crc32c)
        return RWY_CRYPTO_ERR_CORRUPT;

    /* Try password slot (0) then recovery slot (2) */
    for (uint32_t i = 0; i < hdr->slot_count; ++i) {
        const rwy_keyslot_t *slot = &hdr->slots[i];
        if (slot->state != RWY_KEYSLOT_STATE_ACTIVE) continue;
        if (slot->slot_type != RWY_KEYSLOT_TYPE_PASSWORD &&
            slot->slot_type != RWY_KEYSLOT_TYPE_RECOVERY) continue;

        uint8_t kek1[32], kek2[32];
        derive_kek(passphrase, slot->salt, slot->kdf_iterations, kek1, kek2);

        uint8_t cand_dek[32];
        for (int j = 0; j < 32; ++j)
            cand_dek[j] = slot->wrapped_key[j] ^ kek1[j];

        uint8_t exp_mac[32];
        rwy_hmac_sha256(kek2, 32, cand_dek, 32, exp_mac);

        int match = 1;
        for (int j = 0; j < 32; ++j) {
            if (exp_mac[j] != slot->wrapped_key[32 + j])
                match = 0;
        }

        if (match) {
            memcpy(out_dek, cand_dek, 32);
            return RWY_CRYPTO_OK;
        }
    }
    return RWY_CRYPTO_ERR_AUTH;
}

static int read_slot_info(const rwy_block_dev_t *dev, int slot,
                          uint64_t *gen, int *enc, rwy_keyslot_header_t *khdr) {
    uint8_t blk[RWY_BLOCK_SIZE];
    uint64_t off = (uint64_t)(slot ? RWY_SB_SLOT_B : RWY_SB_SLOT_A) * RWY_BLOCK_SIZE;
    rwy_superblock_t sb;
    if (dev->read(dev->ctx, off, blk, sizeof(blk)) != 0) return 0;
    if (rwy_validate_sb_block(blk, &sb) != RWY_OK) return 0;
    *gen = sb.generation;
    *enc = (sb.crypt_algo == RWY_CRYPT_AES_XTS_256);
    if (*enc) memcpy(khdr, blk + RWY_KEYSLOT_OFFSET, sizeof(*khdr));
    return 1;
}

int rwy_keyslot_read_from_dev(const rwy_block_dev_t *dev, rwy_keyslot_header_t *out_hdr) {
    if (!dev || !dev->read || !out_hdr) return RWY_CRYPTO_ERR_INVAL;

    int valid[2], enc[2] = {0, 0};
    uint64_t gen[2] = {0, 0};
    rwy_keyslot_header_t khdr[2];

    for (int i = 0; i < 2; i++)
        valid[i] = read_slot_info(dev, i, &gen[i], &enc[i], &khdr[i]);

    int picked = -1;
    if (valid[0] && valid[1])
        picked = (gen[0] >= gen[1]) ? 0 : 1;
    else if (valid[0])
        picked = 0;
    else if (valid[1])
        picked = 1;

    if (picked < 0) return RWY_CRYPTO_ERR_CORRUPT;
    if (!enc[picked]) return RWY_CRYPTO_ERR_EMPTY;
    *out_hdr = khdr[picked];
    return RWY_CRYPTO_OK;
}

/* Replace the keyslot header in one superblock slot; the slot is read and written back as a whole block. */
static int write_slot_header(const rwy_block_dev_t *dev, uint32_t blk, const rwy_keyslot_header_t *hdr) {
    uint8_t buf[RWY_BLOCK_SIZE];
    uint64_t off = (uint64_t)blk * RWY_BLOCK_SIZE;

    if (dev->read(dev->ctx, off, buf, sizeof(buf))) return RWY_CRYPTO_ERR_IO;
    memcpy(buf + RWY_KEYSLOT_OFFSET, hdr, sizeof(*hdr));
    return dev->write(dev->ctx, off, buf, sizeof(buf)) ? RWY_CRYPTO_ERR_IO : RWY_CRYPTO_OK;
}

int rwy_keyslot_write_to_dev(const rwy_block_dev_t *dev, const rwy_keyslot_header_t *hdr) {
    int rc;

    if (!dev || !dev->read || !dev->write || !hdr) return RWY_CRYPTO_ERR_INVAL;
    rc = write_slot_header(dev, RWY_SB_SLOT_A, hdr);
    if (rc) return rc;
    rc = write_slot_header(dev, RWY_SB_SLOT_B, hdr);
    if (rc) return rc;

    if (dev->flush) dev->flush(dev->ctx);
    return RWY_CRYPTO_OK;
}
