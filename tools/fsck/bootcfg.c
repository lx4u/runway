/*
 * EFI/RUNWAY: when the store has the subtree, the core's audit
 * (rwy_bootcfg_audit) is strict: schema violations are errors. A bad global
 * REVISION counter is the one error --repair fixes (SYSTEM actor, one
 * commit, then the GC generation); orphan incomplete slots only warn. A damaged
 * state record is an error that --repair fixes (an empty record, one in-place
 * write); entries of removed slots, a BOOTED that names no slot and more slots
 * than SLOTS only warn, and --repair drops the stale entries. Each FAILED slot is listed with its
 * cause; a cause code that is unknown, or set on a slot that is not FAILED, is an error --repair refuses.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "check.h"
#include "rwy_blob.h"
#include "rwy_format.h"
#include "rwy_tpm_cmd.h"
#include "tpm_linux.h"

/* EFI/RUNWAY audit lines: the counter is repairable, anything else is plain corruption. */
static void cfg_log(void *ctx, const char *line) {
    check_ctx_t *c = ctx;
    if (!strncmp(line, "EFI/RUNWAY/REVISION:", 20)) { printf("ERROR: %s\n", line); c->cfg_counter++; }
    else ERR(c, "%s", line);
}

static void cfg_failed(void *ctx, const char *line) { (void)ctx; printf("info: EFI/RUNWAY: %s\n", line); }

static void cfg_stale_blob(void *ctx, int kind, const char *path) {
    check_ctx_t *c = ctx;
    WARN(c, "EFI/RUNWAY: %s: %s", path, kind == RWY_BLOB_ORPHAN ? "orphan blob (no slot links it)" : "staging file left behind");
}

/* Anti-rollback anchor: the epoch of the record against the TPM counter. Never increments. */
#define ANCHOR_PCR 7
typedef struct {
    rwy_tpm_xport_t x;
    uint32_t nv_index;
} anchor_t;

static int anchor_read(void *ctx, uint64_t *v) {
    anchor_t *a = ctx;
    int r = rwy_tpmcmd_nv_read(&a->x, a->nv_index, ANCHOR_PCR, v);
    return r == RWY_TPM_EUNSET ? RWY_ANCHOR_READ_UNSET : r == RWY_TPM_ENOINDEX ? RWY_ANCHOR_READ_ABSENT :
           r == RWY_TPM_EPOLICY ? RWY_ANCHOR_READ_DENIED : r ? RWY_ERR_IO : RWY_OK;
}

static int anchor_no_increment(void *ctx) { (void)ctx; return 0; }

static void anchor_report(check_ctx_t *c, const rwy_store_t *s, uint64_t epoch) {
    anchor_t a;
    rwy_anchor_ops_t ops = { anchor_read, anchor_no_increment, &a };
    uint64_t counter = 0;
    int r;
    if (!c->tpm) { WARN(c, "EFI/RUNWAY: enrolled in the anchor, counter not checked (no --tpm)"); return; }
    if (tpm_linux_open(&a.x, c->tpm)) { WARN(c, "EFI/RUNWAY: enrolled in the anchor, cannot open the TPM %s", c->tpm); return; }
    a.nv_index = rwy_anchor_index(s);
    r = rwy_tpmcmd_nv_read(&a.x, a.nv_index, ANCHOR_PCR, &counter);
    if (r == RWY_TPM_ENOINDEX)
        ERR(c, "EFI/RUNWAY: enrolled in the anchor, but the NV counter 0x%08x is gone", a.nv_index);
    else if (r == RWY_TPM_EPOLICY)
        WARN(c, "EFI/RUNWAY: anchor counter not readable, PCR 7 differs from the value it was enrolled with");
    else if (r != RWY_TPM_OK && r != RWY_TPM_EUNSET)
        WARN(c, "EFI/RUNWAY: anchor counter not readable (TPM error %d)", r);
    else switch (rwy_anchor_check(s, &ops)) {
    case RWY_ANCHOR_OK: NOTE(c, "anchor: epoch %llu equals the counter", (unsigned long long)epoch); break;
    case RWY_ANCHOR_FINISHED: WARN(c, "EFI/RUNWAY: epoch %llu is one ahead of the counter (a cut before the increment; RUNWAY.EFI finishes it)", (unsigned long long)epoch); break;
    case RWY_ANCHOR_NEW: NOTE(c, "anchor: enrolled offline, the first increment is left to the device"); break;
    case RWY_ANCHOR_BEHIND:
    case RWY_ANCHOR_AHEAD: ERR(c, "EFI/RUNWAY: store rolled back (epoch %llu, counter %llu)", (unsigned long long)epoch, (unsigned long long)counter); break;
    default: WARN(c, "EFI/RUNWAY: anchor counter could not be checked"); break;
    }
    tpm_linux_close(&a.x);
}

/* Mirror health counters of the state record: any counter is a warning, `rwyctl mirror clear` drops them. */
void check_mirror_health(check_ctx_t *c, rwy_store_t *s) {
    uint8_t r[RWY_REC_PAYLOAD], uuid[16];
    rwy_ss_health_t h;
    if (rwy_record_read(s, r)) return;
    for (unsigned i = 0; i < RWY_MIRROR_MAX_DEVS; i++) {
        char text[37];
        if (!rwy_ss_health_at(r, i, uuid, &h)) continue;
        rwy_format_uuid(uuid, text);
        WARN(c, "mirror member %s: lagging %u, sync failed %u, write failed %u, missing %u (last: %s at record %llu)",
             text, h.count[RWY_SS_EV_LAGGING], h.count[RWY_SS_EV_SYNC_FAILED], h.count[RWY_SS_EV_WRITE_FAILED],
             h.count[RWY_SS_EV_MISSING], rwy_ss_event_name(h.last_event), (unsigned long long)h.seq);
    }
}

/* EFI/RUNWAY boot configuration: strict and read-only. */
void check_bootcfg(check_ctx_t *c, rwy_store_t *s) {
    rwy_bootcfg_audit_t au;
    int arc = rwy_bootcfg_audit(s, &au, cfg_log, c);

    if (arc) {
        printf("ERROR: EFI/RUNWAY: cannot read (%s)\n", rwy_strerror(arc));
        c->errors++;
        return;
    }
    if (!au.engaged)
        return;
    printf("efi/runway: %d slots, %d incomplete\n", au.slots, au.incomplete);
    if (au.incomplete) WARN(c, "EFI/RUNWAY: %d incomplete slot(s) (no REVISION), ignored by selection", au.incomplete);
    if (au.record_damaged) { printf("ERROR: EFI/RUNWAY: state record damaged (both copies), every slot reads INSTALLED\n"); c->cfg_record++; }
    if (au.stale) { WARN(c, "EFI/RUNWAY: state record entries of removed slots"); c->cfg_stale++; }
    if (au.booted_gone) WARN(c, "EFI/RUNWAY: BOOTED names a slot that does not exist");
    if (au.over_slots) WARN(c, "EFI/RUNWAY: %d slots, more than SLOTS %d", au.slots, au.over_slots);
    if (au.enrolled) anchor_report(c, s, au.epoch);
    (void)rwy_bootcfg_failed(s, cfg_failed, 0);
    (void)rwy_blob_stale(s, cfg_stale_blob, c);
}
