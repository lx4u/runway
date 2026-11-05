/*
 * src/rwy_aesarm.h — AES with the ARMv8 Crypto Extensions (NEON intrinsics). Private.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Round keys are 15 x 16 bytes in cipher order. Little-endian only.
 */
#ifndef RWY_AESARM_H
#define RWY_AESARM_H

#include <stdint.h>
#include <string.h>

#if defined(__aarch64__)

#include <arm_neon.h>
#ifndef RWY_EFI
#include <sys/auxv.h>
#endif

#pragma GCC push_options
#pragma GCC target("+crypto")

static inline int rwy_has_armaes(void) {
    static int cached = -1;

    if (cached < 0) {
#ifdef RWY_EFI
        uint64_t isar0;

        __asm__ volatile ("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
        cached = ((isar0 >> 4) & 0xf) != 0;
#else
        cached = (getauxval(AT_HWCAP) & (1UL << 3)) != 0; /* HWCAP_AES */
#endif
    }
    return cached;
}

static inline uint32_t rwy_armaes_sub_word(uint32_t w) {
    uint8x16_t s = vaeseq_u8(vreinterpretq_u8_u32(vdupq_n_u32(w)), vdupq_n_u8(0));

    return vgetq_lane_u32(vreinterpretq_u32_u8(s), 0);
}

static inline void rwy_armaes_expand_enc_keys(const uint8_t key[32], uint8_t rkeys[240]) {
    static const uint8_t rcon[7] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40 };
    uint32_t w[60];
    int i;

    memcpy(w, key, 32);
    for (i = 8; i < 60; ++i) {
        uint32_t t = w[i - 1];

        if (i % 8 == 0)
            t = rwy_armaes_sub_word((t >> 8) | (t << 24)) ^ rcon[i / 8 - 1];
        else if (i % 8 == 4)
            t = rwy_armaes_sub_word(t);
        w[i] = w[i - 8] ^ t;
    }
    memcpy(rkeys, w, 240);
}

/* Equivalent inverse cipher: reversed keys, InvMixColumns on the middle ones */
static inline void rwy_armaes_expand_dec_keys(const uint8_t enc_keys[240], uint8_t dec_keys[240]) {
    int i;

    vst1q_u8(dec_keys, vld1q_u8(enc_keys + 224));
    for (i = 1; i < 14; ++i)
        vst1q_u8(dec_keys + i * 16, vaesimcq_u8(vld1q_u8(enc_keys + (14 - i) * 16)));
    vst1q_u8(dec_keys + 224, vld1q_u8(enc_keys));
}

static inline uint8x16_t rwy_armaes_enc(const uint8_t *rk, uint8x16_t s) {
    int i;

    for (i = 0; i < 13; ++i)
        s = vaesmcq_u8(vaeseq_u8(s, vld1q_u8(rk + i * 16)));
    s = vaeseq_u8(s, vld1q_u8(rk + 208));
    return veorq_u8(s, vld1q_u8(rk + 224));
}

static inline uint8x16_t rwy_armaes_dec(const uint8_t *rk, uint8x16_t s) {
    int i;

    for (i = 0; i < 13; ++i)
        s = vaesimcq_u8(vaesdq_u8(s, vld1q_u8(rk + i * 16)));
    s = vaesdq_u8(s, vld1q_u8(rk + 208));
    return veorq_u8(s, vld1q_u8(rk + 224));
}

static inline void rwy_armaes_enc_block(const void *rk, const uint8_t in[16], uint8_t out[16]) {
    vst1q_u8(out, rwy_armaes_enc(rk, vld1q_u8(in)));
}

static inline void rwy_armaes_dec_block(const void *rk, const uint8_t in[16], uint8_t out[16]) {
    vst1q_u8(out, rwy_armaes_dec(rk, vld1q_u8(in)));
}

/* Multiply the tweak by x in GF(2^128) (IEEE 1619, little-endian) */
static inline void rwy_armaes_gf_mul(uint64_t t[2]) {
    uint64_t carry = t[1] >> 63;

    t[1] = (t[1] << 1) | (t[0] >> 63);
    t[0] = (t[0] << 1) ^ (carry ? 0x87 : 0);
}

static inline void rwy_armaes_xts_sector(const uint8_t *k1, const uint8_t *k2_enc, uint64_t sector_idx,
                                         const uint8_t *in, uint8_t *out, size_t len, int encrypt) {
    uint64_t t[2] = { sector_idx, 0 };
    size_t b, blocks = len / 16;

    vst1q_u8((uint8_t *)t, rwy_armaes_enc(k2_enc, vld1q_u8((const uint8_t *)t)));
    for (b = 0; b < blocks; ++b) {
        uint8x16_t tw = vld1q_u8((const uint8_t *)t);
        uint8x16_t s = veorq_u8(vld1q_u8(in + b * 16), tw);

        s = encrypt ? rwy_armaes_enc(k1, s) : rwy_armaes_dec(k1, s);
        vst1q_u8(out + b * 16, veorq_u8(s, tw));
        rwy_armaes_gf_mul(t);
    }
}

#pragma GCC pop_options

#endif

#endif
