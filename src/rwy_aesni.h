/*
 * src/rwy_aesni.h — AES-NI helpers (x86_64 inline assembly). Private.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_AESNI_H
#define RWY_AESNI_H

#include <stdint.h>

#if defined(__x86_64__) || defined(_M_X64)
static inline int rwy_has_aesni(void) {
    static int cached = -1;

    if (cached < 0) {
        uint32_t eax, ebx, ecx, edx;
        __asm__ volatile ("cpuid"
                          : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                          : "a"(1), "c"(0));
        cached = (ecx & (1U << 25)) != 0;
    }
    return cached;
}

static inline void rwy_aesni_gf_mul(uint8_t t[16]) {
    __asm__ volatile (
        "movdqu (%0), %%xmm0\n\t"
        "pshufd $0xff, %%xmm0, %%xmm1\n\t"
        "psrad $31, %%xmm1\n\t"
        "movl $0x87, %%eax\n\t"
        "movd %%eax, %%xmm3\n\t"
        "pand %%xmm1, %%xmm3\n\t"
        "movdqa %%xmm0, %%xmm4\n\t"
        "psrlq $63, %%xmm4\n\t"
        "pslldq $8, %%xmm4\n\t"
        "psllq $1, %%xmm0\n\t"
        "por %%xmm4, %%xmm0\n\t"
        "pxor %%xmm3, %%xmm0\n\t"
        "movdqu %%xmm0, (%0)\n\t"
        :
        : "r"(t)
        : "rax", "xmm0", "xmm1", "xmm3", "xmm4", "memory"
    );
}

static inline void rwy_aesni_enc_block(const void *rk, const uint8_t in[16], uint8_t out[16]) {
    __asm__ volatile (
        "movdqu (%1), %%xmm0\n\t"
        "movdqu (%0), %%xmm1\n\t"
        "pxor %%xmm1, %%xmm0\n\t"
        "movdqu 16(%0), %%xmm1\n\t"
        "aesenc %%xmm1, %%xmm0\n\t"
        "movdqu 32(%0), %%xmm1\n\t"
        "aesenc %%xmm1, %%xmm0\n\t"
        "movdqu 48(%0), %%xmm1\n\t"
        "aesenc %%xmm1, %%xmm0\n\t"
        "movdqu 64(%0), %%xmm1\n\t"
        "aesenc %%xmm1, %%xmm0\n\t"
        "movdqu 80(%0), %%xmm1\n\t"
        "aesenc %%xmm1, %%xmm0\n\t"
        "movdqu 96(%0), %%xmm1\n\t"
        "aesenc %%xmm1, %%xmm0\n\t"
        "movdqu 112(%0), %%xmm1\n\t"
        "aesenc %%xmm1, %%xmm0\n\t"
        "movdqu 128(%0), %%xmm1\n\t"
        "aesenc %%xmm1, %%xmm0\n\t"
        "movdqu 144(%0), %%xmm1\n\t"
        "aesenc %%xmm1, %%xmm0\n\t"
        "movdqu 160(%0), %%xmm1\n\t"
        "aesenc %%xmm1, %%xmm0\n\t"
        "movdqu 176(%0), %%xmm1\n\t"
        "aesenc %%xmm1, %%xmm0\n\t"
        "movdqu 192(%0), %%xmm1\n\t"
        "aesenc %%xmm1, %%xmm0\n\t"
        "movdqu 208(%0), %%xmm1\n\t"
        "aesenc %%xmm1, %%xmm0\n\t"
        "movdqu 224(%0), %%xmm1\n\t"
        "aesenclast %%xmm1, %%xmm0\n\t"
        "movdqu %%xmm0, (%2)\n\t"
        :
        : "r"(rk), "r"(in), "r"(out)
        : "xmm0", "xmm1", "memory"
    );
}

static inline void rwy_aesni_dec_block(const void *rk, const uint8_t in[16], uint8_t out[16]) {
    __asm__ volatile (
        "movdqu (%1), %%xmm0\n\t"
        "movdqu (%0), %%xmm1\n\t"
        "pxor %%xmm1, %%xmm0\n\t"
        "movdqu 16(%0), %%xmm1\n\t"
        "aesdec %%xmm1, %%xmm0\n\t"
        "movdqu 32(%0), %%xmm1\n\t"
        "aesdec %%xmm1, %%xmm0\n\t"
        "movdqu 48(%0), %%xmm1\n\t"
        "aesdec %%xmm1, %%xmm0\n\t"
        "movdqu 64(%0), %%xmm1\n\t"
        "aesdec %%xmm1, %%xmm0\n\t"
        "movdqu 80(%0), %%xmm1\n\t"
        "aesdec %%xmm1, %%xmm0\n\t"
        "movdqu 96(%0), %%xmm1\n\t"
        "aesdec %%xmm1, %%xmm0\n\t"
        "movdqu 112(%0), %%xmm1\n\t"
        "aesdec %%xmm1, %%xmm0\n\t"
        "movdqu 128(%0), %%xmm1\n\t"
        "aesdec %%xmm1, %%xmm0\n\t"
        "movdqu 144(%0), %%xmm1\n\t"
        "aesdec %%xmm1, %%xmm0\n\t"
        "movdqu 160(%0), %%xmm1\n\t"
        "aesdec %%xmm1, %%xmm0\n\t"
        "movdqu 176(%0), %%xmm1\n\t"
        "aesdec %%xmm1, %%xmm0\n\t"
        "movdqu 192(%0), %%xmm1\n\t"
        "aesdec %%xmm1, %%xmm0\n\t"
        "movdqu 208(%0), %%xmm1\n\t"
        "aesdec %%xmm1, %%xmm0\n\t"
        "movdqu 224(%0), %%xmm1\n\t"
        "aesdeclast %%xmm1, %%xmm0\n\t"
        "movdqu %%xmm0, (%2)\n\t"
        :
        : "r"(rk), "r"(in), "r"(out)
        : "xmm0", "xmm1", "memory"
    );
}

static inline void rwy_aesni_expand_enc_keys(const uint8_t key[32], uint8_t rkeys[240]) {
    __asm__ volatile (
        "movdqu (%0), %%xmm1\n\t"
        "movdqu 16(%0), %%xmm3\n\t"
        "movdqu %%xmm1, (%1)\n\t"
        "movdqu %%xmm3, 16(%1)\n\t"

        #define EXPAND_ROUND_STEP(rcon, off1, off2) \
            "aeskeygenassist $" #rcon ", %%xmm3, %%xmm2\n\t" \
            "pshufd $0xff, %%xmm2, %%xmm2\n\t" \
            "movdqa %%xmm1, %%xmm4\n\t" \
            "pslldq $4, %%xmm4\n\t" \
            "pxor %%xmm4, %%xmm1\n\t" \
            "pslldq $4, %%xmm4\n\t" \
            "pxor %%xmm4, %%xmm1\n\t" \
            "pslldq $4, %%xmm4\n\t" \
            "pxor %%xmm4, %%xmm1\n\t" \
            "pxor %%xmm2, %%xmm1\n\t" \
            "movdqu %%xmm1, " #off1 "(%1)\n\t" \
            "aeskeygenassist $0x00, %%xmm1, %%xmm4\n\t" \
            "pshufd $0xaa, %%xmm4, %%xmm2\n\t" \
            "movdqa %%xmm3, %%xmm4\n\t" \
            "pslldq $4, %%xmm4\n\t" \
            "pxor %%xmm4, %%xmm3\n\t" \
            "pslldq $4, %%xmm4\n\t" \
            "pxor %%xmm4, %%xmm3\n\t" \
            "pslldq $4, %%xmm4\n\t" \
            "pxor %%xmm4, %%xmm3\n\t" \
            "pxor %%xmm2, %%xmm3\n\t" \
            "movdqu %%xmm3, " #off2 "(%1)\n\t"

        EXPAND_ROUND_STEP(0x01, 32, 48)
        EXPAND_ROUND_STEP(0x02, 64, 80)
        EXPAND_ROUND_STEP(0x04, 96, 112)
        EXPAND_ROUND_STEP(0x08, 128, 144)
        EXPAND_ROUND_STEP(0x10, 160, 176)
        EXPAND_ROUND_STEP(0x20, 192, 208)
        #undef EXPAND_ROUND_STEP

        "aeskeygenassist $0x40, %%xmm3, %%xmm2\n\t"
        "pshufd $0xff, %%xmm2, %%xmm2\n\t"
        "movdqa %%xmm1, %%xmm4\n\t"
        "pslldq $4, %%xmm4\n\t"
        "pxor %%xmm4, %%xmm1\n\t"
        "pslldq $4, %%xmm4\n\t"
        "pxor %%xmm4, %%xmm1\n\t"
        "pslldq $4, %%xmm4\n\t"
        "pxor %%xmm4, %%xmm1\n\t"
        "pxor %%xmm2, %%xmm1\n\t"
        "movdqu %%xmm1, 224(%1)\n\t"
        :
        : "r"(key), "r"(rkeys)
        : "xmm1", "xmm2", "xmm3", "xmm4", "memory"
    );
}

static inline void rwy_aesni_expand_dec_keys(const uint8_t enc_keys[240], uint8_t dec_keys[240]) {
    __asm__ volatile (
        "movdqu 224(%0), %%xmm0\n\t"
        "movdqu %%xmm0, (%1)\n\t"

        #define INV_KEY_ROUND(in_off, out_off) \
            "movdqu " #in_off "(%0), %%xmm0\n\t" \
            "aesimc %%xmm0, %%xmm0\n\t" \
            "movdqu %%xmm0, " #out_off "(%1)\n\t"

        INV_KEY_ROUND(208, 16)
        INV_KEY_ROUND(192, 32)
        INV_KEY_ROUND(176, 48)
        INV_KEY_ROUND(160, 64)
        INV_KEY_ROUND(144, 80)
        INV_KEY_ROUND(128, 96)
        INV_KEY_ROUND(112, 112)
        INV_KEY_ROUND(96, 128)
        INV_KEY_ROUND(80, 144)
        INV_KEY_ROUND(64, 160)
        INV_KEY_ROUND(48, 176)
        INV_KEY_ROUND(32, 192)
        INV_KEY_ROUND(16, 208)
        #undef INV_KEY_ROUND

        "movdqu (%0), %%xmm0\n\t"
        "movdqu %%xmm0, 224(%1)\n\t"
        :
        : "r"(enc_keys), "r"(dec_keys)
        : "xmm0", "memory"
    );
}

static inline void rwy_aesni_xts_enc_8blocks(const uint8_t *rk, const uint8_t in[128], uint8_t out[128], const uint8_t t[128]) {
    __asm__ volatile (
        "movdqu 0(%3), %%xmm8\n\t"
        "movdqu 16(%3), %%xmm9\n\t"
        "movdqu 32(%3), %%xmm10\n\t"
        "movdqu 48(%3), %%xmm11\n\t"
        "movdqu 64(%3), %%xmm12\n\t"
        "movdqu 80(%3), %%xmm13\n\t"
        "movdqu 96(%3), %%xmm14\n\t"
        "movdqu 112(%3), %%xmm15\n\t"

        "movdqu 0(%1), %%xmm0\n\t"
        "pxor %%xmm8, %%xmm0\n\t"
        "movdqu 16(%1), %%xmm1\n\t"
        "pxor %%xmm9, %%xmm1\n\t"
        "movdqu 32(%1), %%xmm2\n\t"
        "pxor %%xmm10, %%xmm2\n\t"
        "movdqu 48(%1), %%xmm3\n\t"
        "pxor %%xmm11, %%xmm3\n\t"
        "movdqu 64(%1), %%xmm4\n\t"
        "pxor %%xmm12, %%xmm4\n\t"
        "movdqu 80(%1), %%xmm5\n\t"
        "pxor %%xmm13, %%xmm5\n\t"
        "movdqu 96(%1), %%xmm6\n\t"
        "pxor %%xmm14, %%xmm6\n\t"
        "movdqu 112(%1), %%xmm7\n\t"
        "pxor %%xmm15, %%xmm7\n\t"

        "movdqu 0(%0), %%xmm8\n\t"
        "pxor %%xmm8, %%xmm0\n\t"
        "pxor %%xmm8, %%xmm1\n\t"
        "pxor %%xmm8, %%xmm2\n\t"
        "pxor %%xmm8, %%xmm3\n\t"
        "pxor %%xmm8, %%xmm4\n\t"
        "pxor %%xmm8, %%xmm5\n\t"
        "pxor %%xmm8, %%xmm6\n\t"
        "pxor %%xmm8, %%xmm7\n\t"

        #define AES_ENC_ROUND(offset) \
            "movdqu " #offset "(%0), %%xmm8\n\t" \
            "aesenc %%xmm8, %%xmm0\n\t" \
            "aesenc %%xmm8, %%xmm1\n\t" \
            "aesenc %%xmm8, %%xmm2\n\t" \
            "aesenc %%xmm8, %%xmm3\n\t" \
            "aesenc %%xmm8, %%xmm4\n\t" \
            "aesenc %%xmm8, %%xmm5\n\t" \
            "aesenc %%xmm8, %%xmm6\n\t" \
            "aesenc %%xmm8, %%xmm7\n\t"

        AES_ENC_ROUND(16)
        AES_ENC_ROUND(32)
        AES_ENC_ROUND(48)
        AES_ENC_ROUND(64)
        AES_ENC_ROUND(80)
        AES_ENC_ROUND(96)
        AES_ENC_ROUND(112)
        AES_ENC_ROUND(128)
        AES_ENC_ROUND(144)
        AES_ENC_ROUND(160)
        AES_ENC_ROUND(176)
        AES_ENC_ROUND(192)
        AES_ENC_ROUND(208)
        #undef AES_ENC_ROUND

        "movdqu 224(%0), %%xmm8\n\t"
        "aesenclast %%xmm8, %%xmm0\n\t"
        "aesenclast %%xmm8, %%xmm1\n\t"
        "aesenclast %%xmm8, %%xmm2\n\t"
        "aesenclast %%xmm8, %%xmm3\n\t"
        "aesenclast %%xmm8, %%xmm4\n\t"
        "aesenclast %%xmm8, %%xmm5\n\t"
        "aesenclast %%xmm8, %%xmm6\n\t"
        "aesenclast %%xmm8, %%xmm7\n\t"

        "movdqu 0(%3), %%xmm8\n\t"
        "pxor %%xmm8, %%xmm0\n\t"
        "movdqu %%xmm0, 0(%2)\n\t"

        "movdqu 16(%3), %%xmm9\n\t"
        "pxor %%xmm9, %%xmm1\n\t"
        "movdqu %%xmm1, 16(%2)\n\t"

        "movdqu 32(%3), %%xmm10\n\t"
        "pxor %%xmm10, %%xmm2\n\t"
        "movdqu %%xmm2, 32(%2)\n\t"

        "movdqu 48(%3), %%xmm11\n\t"
        "pxor %%xmm11, %%xmm3\n\t"
        "movdqu %%xmm3, 48(%2)\n\t"

        "movdqu 64(%3), %%xmm12\n\t"
        "pxor %%xmm12, %%xmm4\n\t"
        "movdqu %%xmm4, 64(%2)\n\t"

        "movdqu 80(%3), %%xmm13\n\t"
        "pxor %%xmm13, %%xmm5\n\t"
        "movdqu %%xmm5, 80(%2)\n\t"

        "movdqu 96(%3), %%xmm14\n\t"
        "pxor %%xmm14, %%xmm6\n\t"
        "movdqu %%xmm6, 96(%2)\n\t"

        "movdqu 112(%3), %%xmm15\n\t"
        "pxor %%xmm15, %%xmm7\n\t"
        "movdqu %%xmm7, 112(%2)\n\t"
        :
        : "r"(rk), "r"(in), "r"(out), "r"(t)
        : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
          "memory"
    );
}

static inline void rwy_aesni_xts_dec_8blocks(const uint8_t *rk, const uint8_t in[128], uint8_t out[128], const uint8_t t[128]) {
    __asm__ volatile (
        "movdqu 0(%3), %%xmm8\n\t"
        "movdqu 16(%3), %%xmm9\n\t"
        "movdqu 32(%3), %%xmm10\n\t"
        "movdqu 48(%3), %%xmm11\n\t"
        "movdqu 64(%3), %%xmm12\n\t"
        "movdqu 80(%3), %%xmm13\n\t"
        "movdqu 96(%3), %%xmm14\n\t"
        "movdqu 112(%3), %%xmm15\n\t"

        "movdqu 0(%1), %%xmm0\n\t"
        "pxor %%xmm8, %%xmm0\n\t"
        "movdqu 16(%1), %%xmm1\n\t"
        "pxor %%xmm9, %%xmm1\n\t"
        "movdqu 32(%1), %%xmm2\n\t"
        "pxor %%xmm10, %%xmm2\n\t"
        "movdqu 48(%1), %%xmm3\n\t"
        "pxor %%xmm11, %%xmm3\n\t"
        "movdqu 64(%1), %%xmm4\n\t"
        "pxor %%xmm12, %%xmm4\n\t"
        "movdqu 80(%1), %%xmm5\n\t"
        "pxor %%xmm13, %%xmm5\n\t"
        "movdqu 96(%1), %%xmm6\n\t"
        "pxor %%xmm14, %%xmm6\n\t"
        "movdqu 112(%1), %%xmm7\n\t"
        "pxor %%xmm15, %%xmm7\n\t"

        "movdqu 0(%0), %%xmm8\n\t"
        "pxor %%xmm8, %%xmm0\n\t"
        "pxor %%xmm8, %%xmm1\n\t"
        "pxor %%xmm8, %%xmm2\n\t"
        "pxor %%xmm8, %%xmm3\n\t"
        "pxor %%xmm8, %%xmm4\n\t"
        "pxor %%xmm8, %%xmm5\n\t"
        "pxor %%xmm8, %%xmm6\n\t"
        "pxor %%xmm8, %%xmm7\n\t"

        #define AES_DEC_ROUND(offset) \
            "movdqu " #offset "(%0), %%xmm8\n\t" \
            "aesdec %%xmm8, %%xmm0\n\t" \
            "aesdec %%xmm8, %%xmm1\n\t" \
            "aesdec %%xmm8, %%xmm2\n\t" \
            "aesdec %%xmm8, %%xmm3\n\t" \
            "aesdec %%xmm8, %%xmm4\n\t" \
            "aesdec %%xmm8, %%xmm5\n\t" \
            "aesdec %%xmm8, %%xmm6\n\t" \
            "aesdec %%xmm8, %%xmm7\n\t"

        AES_DEC_ROUND(16)
        AES_DEC_ROUND(32)
        AES_DEC_ROUND(48)
        AES_DEC_ROUND(64)
        AES_DEC_ROUND(80)
        AES_DEC_ROUND(96)
        AES_DEC_ROUND(112)
        AES_DEC_ROUND(128)
        AES_DEC_ROUND(144)
        AES_DEC_ROUND(160)
        AES_DEC_ROUND(176)
        AES_DEC_ROUND(192)
        AES_DEC_ROUND(208)
        #undef AES_DEC_ROUND

        "movdqu 224(%0), %%xmm8\n\t"
        "aesdeclast %%xmm8, %%xmm0\n\t"
        "aesdeclast %%xmm8, %%xmm1\n\t"
        "aesdeclast %%xmm8, %%xmm2\n\t"
        "aesdeclast %%xmm8, %%xmm3\n\t"
        "aesdeclast %%xmm8, %%xmm4\n\t"
        "aesdeclast %%xmm8, %%xmm5\n\t"
        "aesdeclast %%xmm8, %%xmm6\n\t"
        "aesdeclast %%xmm8, %%xmm7\n\t"

        "movdqu 0(%3), %%xmm8\n\t"
        "pxor %%xmm8, %%xmm0\n\t"
        "movdqu %%xmm0, 0(%2)\n\t"

        "movdqu 16(%3), %%xmm9\n\t"
        "pxor %%xmm9, %%xmm1\n\t"
        "movdqu %%xmm1, 16(%2)\n\t"

        "movdqu 32(%3), %%xmm10\n\t"
        "pxor %%xmm10, %%xmm2\n\t"
        "movdqu %%xmm2, 32(%2)\n\t"

        "movdqu 48(%3), %%xmm11\n\t"
        "pxor %%xmm11, %%xmm3\n\t"
        "movdqu %%xmm3, 48(%2)\n\t"

        "movdqu 64(%3), %%xmm12\n\t"
        "pxor %%xmm12, %%xmm4\n\t"
        "movdqu %%xmm4, 64(%2)\n\t"

        "movdqu 80(%3), %%xmm13\n\t"
        "pxor %%xmm13, %%xmm5\n\t"
        "movdqu %%xmm5, 80(%2)\n\t"

        "movdqu 96(%3), %%xmm14\n\t"
        "pxor %%xmm14, %%xmm6\n\t"
        "movdqu %%xmm6, 96(%2)\n\t"

        "movdqu 112(%3), %%xmm15\n\t"
        "pxor %%xmm15, %%xmm7\n\t"
        "movdqu %%xmm7, 112(%2)\n\t"
        :
        : "r"(rk), "r"(in), "r"(out), "r"(t)
        : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
          "memory"
    );
}

static inline void rwy_aesni_xts_enc_sector(const uint8_t *k1_enc, const uint8_t *k2_enc,
                                      uint64_t sector_idx, const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t tweak[16] = {0};
    uint8_t t[16];
    for (int i = 0; i < 8; ++i)
        tweak[i] = (uint8_t)(sector_idx >> (i * 8));

    rwy_aesni_enc_block(k2_enc, tweak, t);

    size_t blocks = len / 16;
    size_t b = 0;

    while (b + 8 <= blocks) {
        uint8_t t_vec[128];
        for (int i = 0; i < 8; ++i) {
            __builtin_memcpy(t_vec + i * 16, t, 16);
            rwy_aesni_gf_mul(t);
        }
        rwy_aesni_xts_enc_8blocks(k1_enc, in + b * 16, out + b * 16, t_vec);
        b += 8;
    }

    while (b < blocks) {
        uint8_t tmp[16];
        for (int i = 0; i < 16; ++i) tmp[i] = in[b * 16 + i] ^ t[i];
        rwy_aesni_enc_block(k1_enc, tmp, tmp);
        for (int i = 0; i < 16; ++i) out[b * 16 + i] = tmp[i] ^ t[i];
        rwy_aesni_gf_mul(t);
        b++;
    }
}

static inline void rwy_aesni_xts_dec_sector(const uint8_t *k1_dec, const uint8_t *k2_enc,
                                      uint64_t sector_idx, const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t tweak[16] = {0};
    uint8_t t[16];
    for (int i = 0; i < 8; ++i)
        tweak[i] = (uint8_t)(sector_idx >> (i * 8));

    rwy_aesni_enc_block(k2_enc, tweak, t);

    size_t blocks = len / 16;
    size_t b = 0;

    while (b + 8 <= blocks) {
        uint8_t t_vec[128];
        for (int i = 0; i < 8; ++i) {
            __builtin_memcpy(t_vec + i * 16, t, 16);
            rwy_aesni_gf_mul(t);
        }
        rwy_aesni_xts_dec_8blocks(k1_dec, in + b * 16, out + b * 16, t_vec);
        b += 8;
    }

    while (b < blocks) {
        uint8_t tmp[16];
        for (int i = 0; i < 16; ++i) tmp[i] = in[b * 16 + i] ^ t[i];
        rwy_aesni_dec_block(k1_dec, tmp, tmp);
        for (int i = 0; i < 16; ++i) out[b * 16 + i] = tmp[i] ^ t[i];
        rwy_aesni_gf_mul(t);
        b++;
    }
}
#endif

#endif
