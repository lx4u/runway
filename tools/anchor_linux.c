/* The anti-rollback anchor on a Linux TPM: the counter callbacks, its state and the gate for epoch raising commits. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "anchor_linux.h"
#include "rwyctl_open.h"

int anchor_tpm_read(void *ctx, uint64_t *v) {
    anchor_tpm_t *a = ctx;
    int r = rwy_tpmcmd_nv_read(&a->x, a->index, ANCHOR_PCR, v);
    return r == RWY_TPM_EUNSET ? RWY_ANCHOR_READ_UNSET : r == RWY_TPM_ENOINDEX ? RWY_ANCHOR_READ_ABSENT :
           r == RWY_TPM_EPOLICY ? RWY_ANCHOR_READ_DENIED : r ? RWY_ERR_IO : RWY_OK;
}

int anchor_tpm_increment(void *ctx) {
    anchor_tpm_t *a = ctx;
    return rwy_tpmcmd_nv_increment(&a->x, a->index, ANCHOR_PCR) ? RWY_ERR_IO : RWY_OK;
}

/* Status only looks: the increment of a record one ahead is left to RUNWAY.EFI. */
int anchor_tpm_peek(void *ctx) { (void)ctx; return 0; }

/* Indexed by RWY_ANCHOR_*. */
static const char *const ANCHOR_WORD[] = { "off", "ok", "pending", "rolled-back", "rolled-back", "unavailable", "new", "stray" };

const char *anchor_word(int r) { return r >= 0 && r <= RWY_ANCHOR_STRAY ? ANCHOR_WORD[r] : "unavailable"; }

const char *anchor_state(rwy_store_t *s) {
    anchor_tpm_t *act = anchor_active();
    anchor_tpm_t local_a = { .index = rwy_anchor_index(s) };
    anchor_tpm_t *a = act ? act : &local_a;
    rwy_anchor_ops_t ops = { anchor_tpm_read, anchor_tpm_peek, a };
    uint64_t epoch;
    int enrolled, r;
    rwy_anchor_get(s, &epoch, &enrolled);
    if (!enrolled) return anchor_word(RWY_ANCHOR_OFF);
    if (!act && tpm_linux_open(&local_a.x, tpm_dev())) return anchor_word(RWY_ANCHOR_MISSING);
    r = rwy_anchor_check(s, &ops);
    if (!act) tpm_linux_close(&local_a.x);
    return anchor_word(r);
}

static anchor_tpm_t g_anchor;
static int g_anchor_open;   /* the TPM of g_anchor is open */
static const rwy_anchor_ops_t g_anchor_ops = { anchor_tpm_read, anchor_tpm_increment, &g_anchor };

anchor_tpm_t *anchor_active(void) {
    return g_anchor_open ? &g_anchor : NULL;
}

void anchor_release(void) {
    if (!g_anchor_open) return;
    tpm_linux_close(&g_anchor.x);
    g_anchor_open = 0;
}

int anchor_gate(rwy_store_t *s) {
    uint64_t epoch;
    int enrolled, r;
    rwy_anchor_get(s, &epoch, &enrolled);
    if (!enrolled) return 0;
    g_anchor.index = rwy_anchor_index(s);
    if (tpm_linux_open(&g_anchor.x, tpm_dev())) return -1;
    g_anchor_open = 1;
    r = rwy_anchor_adopt(s, &g_anchor_ops);
    if (r == RWY_ANCHOR_OK || r == RWY_ANCHOR_FINISHED) {
        rwy_anchor_attach(s, &g_anchor_ops);
        return 0;
    }
    anchor_release();
    return r < 0 ? RWY_ANCHOR_MISSING : r;
}
