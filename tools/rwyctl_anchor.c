/* rwyctl: the anti-rollback anchor (TPM NV counter): its state, the gate for epoch raising commands and `anchor`. */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rwyctl.h"
#include "anchor_linux.h"

static uint32_t anchor_index(void) { return rwy_anchor_index(&g_store); }

/* A command that finalizes, removes or fails a slot raises the epoch. On an enrolled store whose counter cannot
 * be trusted (or reached) it is refused. */
int anchor_refuse(void) {
    int r;
    if (anchor_active()) return 0;   /* the daemon passed the gate when it mounted the store */
    r = anchor_gate(&g_store);
    if (!r) return 0;
    if (r < 0) warn("refused: the anchor needs the TPM %s", tpm_dev());
    else warn("refused: the anchor says %s (see `anchor status`; `anchor reset` after a restored copy)", anchor_word(r));
    return X_POLICY;
}

static int cmd_anchor_status(void) {
    anchor_tpm_t *act = anchor_active();
    anchor_tpm_t local_a = { .index = anchor_index() };
    anchor_tpm_t *a = act ? act : &local_a;
    rwy_anchor_ops_t ops = { anchor_tpm_read, anchor_tpm_peek, a };
    uint64_t epoch, counter;
    int enrolled, r, have_tpm = act ? 1 : !tpm_linux_open(&local_a.x, tpm_dev());
    rwy_anchor_get(&g_store, &epoch, &enrolled);
    printf("ENROLLED=%s\n", enrolled ? "yes" : "no");
    printf("EPOCH=%llu\n", (unsigned long long)epoch);
    printf("NV-INDEX=0x%08x\n", anchor_index());
    r = have_tpm ? rwy_anchor_check(&g_store, &ops) : enrolled ? RWY_ANCHOR_MISSING : RWY_ANCHOR_OFF;
    printf("COUNTER=");
    if (have_tpm && anchor_tpm_read(a, &counter) == RWY_OK) printf("%llu\n", (unsigned long long)counter);
    else printf("-\n");
    printf("ANCHOR=%s\n", anchor_word(r));
    if (have_tpm && !act) tpm_linux_close(&local_a.x);
    return 0;
}

static int define_counter(rwy_tpm_xport_t *x, int replace, const uint8_t *pcr7) {
    uint64_t v;
    if (replace) (void)rwy_tpmcmd_nv_undefine(x, anchor_index());
    int dr = rwy_tpmcmd_nv_define(x, anchor_index(), ANCHOR_PCR, pcr7);
    if (dr && !replace && rwy_tpmcmd_nv_read(x, anchor_index(), ANCHOR_PCR, &v) == RWY_TPM_EUNSET) {
        puts("the counter was defined by an enroll that did not finish; finishing it");
        return 0;
    }
    return dr;
}

static int enroll_increment(rwy_tpm_xport_t *x, uint64_t *out_v) {
    if (rwy_tpmcmd_nv_increment(x, anchor_index(), ANCHOR_PCR) ||
        rwy_tpmcmd_nv_read(x, anchor_index(), ANCHOR_PCR, out_v)) {
        fprintf(stderr, "rwyctl: cannot start the NV counter (TPM rc 0x%x)\n", x->rc);
        return X_IO;
    }
    return 0;
}

/* Define the counter, raise it once so it has a value, and record that value as the epoch. */
static int anchor_enroll(int replace, const uint8_t *pcr7) {
    anchor_tpm_t *act = anchor_active();
    rwy_tpm_xport_t local_x;
    rwy_tpm_xport_t *x = act ? &act->x : &local_x;
    uint64_t v, epoch;
    int enrolled, rc, dr;
    if (g_store.sb.crypt_algo != RWY_CRYPT_AES_XTS_256) { warn("%s", "the anchor needs an encrypted store"); return X_POLICY; }
    rwy_anchor_get(&g_store, &epoch, &enrolled);
    if (enrolled && !replace) { warn("%s", "already enrolled (use `anchor reset` to start again)"); return X_POLICY; }
    if (!act && tpm_linux_open(&local_x, tpm_dev())) { warn("cannot open the TPM %s", tpm_dev()); return X_IO; }
    dr = define_counter(x, replace, pcr7);
    if (dr) {
        fprintf(stderr, "rwyctl: cannot define the NV counter 0x%08x (TPM rc 0x%x); is it taken? try `anchor reset`\n", anchor_index(), x->rc);
        rc = X_IO;
    } else if (pcr7) {
        rc = rwy_anchor_set(&g_store, 0, 1);
        if (rc) { warn("cannot write the record: %s", rwy_strerror(rc)); rc = X_IO; }
        else printf("enrolled: NV index 0x%08x, the first increment is left to the device\n", anchor_index());
    } else if ((rc = enroll_increment(x, &v))) {
        /* error already reported */
    } else if ((rc = rwy_anchor_set(&g_store, v, 1))) {
        warn("cannot write the record: %s", rwy_strerror(rc));
        rc = X_IO;
    } else printf("enrolled: NV index 0x%08x, epoch %llu\n", anchor_index(), (unsigned long long)v);
    if (!act) tpm_linux_close(&local_x);
    return rc;
}

/* 64 hex digits into 32 bytes. */
static int parse_pcr7(const char *h, uint8_t out[32]) {
    int i;
    if (strlen(h) != 64) return -1;
    for (i = 0; i < 32; i++) {
        unsigned v;
        char b[3] = { h[2 * i], h[2 * i + 1], 0 }, *e;
        v = (unsigned)strtoul(b, &e, 16);
        if (*e || !isxdigit((unsigned char)b[0]) || !isxdigit((unsigned char)b[1])) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

int cmd_anchor(int argc, char **argv) {
    uint8_t pcr7[32];
    int enroll = argc >= 1 && !strcmp(argv[0], "enroll"), reset = argc >= 1 && !strcmp(argv[0], "reset");
    if (argc == 1 && !strcmp(argv[0], "status")) return cmd_anchor_status();
    if ((enroll || reset) && argc == 1) return anchor_enroll(reset, NULL);
    if ((enroll || reset) && argc == 3 && !strcmp(argv[1], "--pcr7") && !parse_pcr7(argv[2], pcr7)) return anchor_enroll(reset, pcr7);
    warn("%s", "usage: anchor status | enroll [--pcr7 HEX] | reset [--pcr7 HEX]");
    return X_USAGE;
}
