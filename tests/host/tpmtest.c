/*
 * tpmtest — the TPM command builders against a real TPM (swtpm on a unix socket or a device).
 * SPDX-License-Identifier: Apache-2.0
 *
 *   tpmtest TPM                       seal, unseal, then extend PCR 7: unseal must be refused
 *   tpmtest TPM enroll DEKFILE STORE  seal the key in DEKFILE into keyslot 1 of STORE (a fixture for rwyctl)
 *   tpmtest TPM enroll-only ...    same, and empty the passphrase keyslot 0 (a TPM-only store)
 *   tpmtest TPM extend                extend PCR 7 (makes every blob sealed before it unreadable)
 *   tpmtest TPM nv-read INDEX         print the NV counter at INDEX (hex)
 *   tpmtest TPM nv-incr INDEX         raise the NV counter at INDEX by one
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../include/rwy_crypto.h"
#include "../../tools/tpm_linux.h"

#define PCR 7
#define NV 0x01800123

static int g_fd;
static int fd_rd(void *c, uint64_t o, void *b, size_t n) { (void)c; return pread(g_fd, b, n, (off_t)o) == (ssize_t)n ? 0 : -1; }
/* A store takes whole blocks at block offsets only. */
static int fd_wr(void *c, uint64_t o, const void *b, size_t n) {
    (void)c;
    if (o % 4096 || n % 4096) return fprintf(stderr, "unaligned store write: %zu bytes at %llu\n", n, (unsigned long long)o), -1;
    return pwrite(g_fd, b, n, (off_t)o) == (ssize_t)n ? 0 : -1;
}

static int check(int cond, const char *what) {
    if (!cond) fprintf(stderr, "TPMTEST FAIL: %s\n", what);
    return !cond;
}

static int extend(rwy_tpm_xport_t *x) {
    uint8_t cmd[96], resp[64];
    uint32_t n = sizeof(resp), o = 0, i;
    cmd[o++] = 0x80; cmd[o++] = 0x02;                          /* TPM_ST_SESSIONS */
    o += 4;
    cmd[o++] = 0; cmd[o++] = 0; cmd[o++] = 0x01; cmd[o++] = 0x82;   /* PCR_Extend */
    cmd[o++] = 0; cmd[o++] = 0; cmd[o++] = 0; cmd[o++] = PCR;
    cmd[o++] = 0; cmd[o++] = 0; cmd[o++] = 0; cmd[o++] = 9;    /* auth area: empty password */
    cmd[o++] = 0x40; cmd[o++] = 0; cmd[o++] = 0; cmd[o++] = 9;
    cmd[o++] = 0; cmd[o++] = 0; cmd[o++] = 0; cmd[o++] = 0; cmd[o++] = 0;
    cmd[o++] = 0; cmd[o++] = 0; cmd[o++] = 0; cmd[o++] = 1;    /* one digest */
    cmd[o++] = 0; cmd[o++] = 0x0b;                             /* SHA-256 */
    for (i = 0; i < 32; i++) cmd[o++] = (uint8_t)(0xa0 + i);
    cmd[2] = 0; cmd[3] = 0; cmd[4] = (uint8_t)(o >> 8); cmd[5] = (uint8_t)o;
    return x->submit(x->ctx, cmd, o, resp, &n) || n < 10 || resp[6] || resp[7] || resp[8] || resp[9];
}

static int enroll(rwy_tpm_xport_t *x, const char *dekfile, const char *store, int tpm_only) {
    uint8_t dek[32], blob[364], pcr[32], mask[32] = { 0 };
    uint32_t len = 0;
    rwy_keyslot_header_t h;
    rwy_block_dev_t dev = { fd_rd, fd_wr, NULL, NULL, 0, 4096, NULL, NULL };
    int f = open(dekfile, O_RDONLY);
    if (f < 0 || read(f, dek, 32) != 32) return fprintf(stderr, "cannot read %s\n", dekfile), 1;
    close(f);
    g_fd = open(store, O_RDWR);
    if (g_fd < 0) return fprintf(stderr, "cannot open %s\n", store), 1;
    if (rwy_tpmcmd_seal(x, PCR, dek, blob, &len, pcr)) return fprintf(stderr, "seal failed\n"), 1;
    if (rwy_keyslot_read_from_dev(&dev, &h)) return fprintf(stderr, "no keyslot header\n"), 1;
    memcpy(mask, pcr, 32);
    if (tpm_only) h.slots[0].state = RWY_KEYSLOT_STATE_EMPTY;   /* before the call that updates the CRC */
    if (rwy_keyslot_set_tpm2_blob(&h, 1, blob, len, mask)) return fprintf(stderr, "set blob failed\n"), 1;
    if (rwy_keyslot_write_to_dev(&dev, &h)) return fprintf(stderr, "write header failed\n"), 1;
    close(g_fd);
    return 0;
}

static int unseal_slot(rwy_tpm_xport_t *x, const char *store) {
    uint8_t dek[32];
    rwy_keyslot_header_t h;
    rwy_block_dev_t dev = { fd_rd, fd_wr, NULL, NULL, 0, 4096, NULL, NULL };
    g_fd = open(store, O_RDONLY);
    if (g_fd < 0) return fprintf(stderr, "cannot open %s\n", store), 1;
    if (rwy_keyslot_read_from_dev(&dev, &h)) return fprintf(stderr, "no keyslot header\n"), 1;
    if (h.slots[1].state != RWY_KEYSLOT_STATE_ACTIVE || h.slots[1].slot_type != RWY_KEYSLOT_TYPE_TPM2)
        return fprintf(stderr, "keyslot 1 not active TPM2\n"), 1;
    if (rwy_tpmcmd_unseal(x, PCR, h.slots[1].blob, h.slots[1].blob_len, dek))
        return fprintf(stderr, "unseal failed\n"), 1;
    if (write(STDOUT_FILENO, dek, 32) != 32) return 1;
    close(g_fd);
    return 0;
}

int main(int argc, char **argv) {
    rwy_tpm_xport_t x;
    uint8_t dek[32], got[32], blob[364], pcr[32], pol[32], again[32];
    uint8_t rand_a[32], rand_b[64];
    uint32_t len = 0;
    uint64_t v = 0, v0;
    int bad = 0, i;

    if (argc < 2 || tpm_linux_open(&x, argv[1])) return fprintf(stderr, "usage: tpmtest TPM [enroll|enroll-only DEKFILE STORE | unseal-slot STORE | random [LEN] | extend | pcr | nv-read|nv-incr|nv-undef INDEX]\n"), 2;
    if (argc == 5 && !strcmp(argv[2], "enroll")) return enroll(&x, argv[3], argv[4], 0);
    if (argc == 5 && !strcmp(argv[2], "enroll-only")) return enroll(&x, argv[3], argv[4], 1);
    if (argc == 4 && !strcmp(argv[2], "unseal-slot")) return unseal_slot(&x, argv[3]);
    if (argc >= 3 && !strcmp(argv[2], "random")) {
        uint32_t n = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 10) : 32;
        uint8_t rbytes[128];
        if (n > sizeof(rbytes)) n = sizeof(rbytes);
        if (rwy_tpmcmd_get_random(&x, rbytes, n)) return 1;
        for (i = 0; i < (int)n; i++) printf("%02x", rbytes[i]);
        return printf("\n"), 0;
    }
    if (argc == 3 && !strcmp(argv[2], "extend")) return extend(&x);
    if (argc == 3 && !strcmp(argv[2], "pcr")) {
        if (rwy_tpmcmd_read_pcr(&x, PCR, pcr)) return 1;
        for (i = 0; i < 32; i++) printf("%02x", pcr[i]);
        return printf("\n"), 0;
    }
    if (argc == 4 && !strcmp(argv[2], "nv-read")) {
        uint64_t v;
        if (rwy_tpmcmd_nv_read(&x, (uint32_t)strtoul(argv[3], NULL, 16), PCR, &v)) return 1;
        return printf("%llu\n", (unsigned long long)v), 0;
    }
    if (argc == 4 && !strcmp(argv[2], "nv-undef")) return rwy_tpmcmd_nv_undefine(&x, (uint32_t)strtoul(argv[3], NULL, 16)) != 0;
    if (argc == 4 && !strcmp(argv[2], "nv-incr")) return rwy_tpmcmd_nv_increment(&x, (uint32_t)strtoul(argv[3], NULL, 16), PCR) != 0;

    bad |= check(rwy_tpmcmd_get_random(&x, rand_a, 32) == 0, "get_random 32 bytes");
    bad |= check(rwy_tpmcmd_get_random(&x, rand_b, 64) == 0, "get_random 64 bytes");
    bad |= check(memcmp(rand_a, rand_b, 32) != 0, "consecutive random draws differ");
    bad |= check(rwy_tpmcmd_get_random(&x, NULL, 32) == RWY_TPM_EARG, "get_random with null buffer");
    bad |= check(rwy_tpmcmd_get_random(NULL, rand_a, 32) == RWY_TPM_EARG, "get_random with null xport");

    for (i = 0; i < 32; i++) dek[i] = (uint8_t)(i * 7 + 1);
    bad |= check(rwy_tpmcmd_seal(&x, PCR, dek, blob, &len, pcr) == 0 && len > 4, "seal");
    rwy_tpmcmd_pcr_policy(PCR, pcr, pol);
    bad |= check(rwy_tpmcmd_read_pcr(&x, PCR, again) == 0 && !memcmp(again, pcr, 32), "read_pcr matches what was sealed");
    bad |= check(rwy_tpmcmd_unseal(&x, PCR, blob, len, got) == 0 && !memcmp(got, dek, 32), "unseal returns the key");
    bad |= check(rwy_tpmcmd_unseal(&x, PCR, blob, 3, got) == RWY_TPM_EARG, "short blob is an argument error");
    bad |= check(rwy_tpmcmd_unseal(&x, PCR, blob, len - 1, got) == RWY_TPM_ESIZE, "truncated blob is refused");
    bad |= check(rwy_tpmcmd_nv_define(&x, NV, PCR, NULL) == 0, "define the NV counter");
    bad |= check(rwy_tpmcmd_nv_define(&x, NV, PCR, NULL) == RWY_TPM_ERC && x.rc != 0, "a second define of the same index is refused");
    bad |= check(rwy_tpmcmd_nv_read(&x, NV, PCR, &v) == RWY_TPM_EUNSET, "a counter that was never incremented says so");
    bad |= check(rwy_tpmcmd_nv_increment(&x, NV, PCR) == 0 && rwy_tpmcmd_nv_read(&x, NV, PCR, &v0) == 0, "first increment, then read");
    bad |= check(rwy_tpmcmd_nv_increment(&x, NV, PCR) == 0 && rwy_tpmcmd_nv_read(&x, NV, PCR, &v) == 0 && v == v0 + 1, "increment adds one");
    bad |= check(extend(&x) == 0, "extend PCR 7");
    bad |= check(rwy_tpmcmd_nv_read(&x, NV, PCR, &v) == RWY_TPM_EPOLICY, "read after the PCR changed is refused");
    bad |= check(rwy_tpmcmd_nv_increment(&x, NV, PCR) == RWY_TPM_EPOLICY, "increment after the PCR changed is refused");
    bad |= check(rwy_tpmcmd_nv_undefine(&x, NV) == 0, "the owner removes the counter whatever the PCR");
    bad |= check(rwy_tpmcmd_nv_read(&x, NV, PCR, &v) == RWY_TPM_ENOINDEX, "the counter is gone");
    bad |= check(rwy_tpmcmd_unseal(&x, PCR, blob, len, got) == RWY_TPM_EPOLICY, "unseal after the PCR changed is refused");
    bad |= check(x.rc != 0, "the TPM response code is kept");
    tpm_linux_close(&x);
    puts(bad ? "TPMTEST: FAIL" : "TPMTEST: PASS");
    return bad;
}
