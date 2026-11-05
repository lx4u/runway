/* rwyctl: `check`, the audit of EFI/RUNWAY and its repair. */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include "rwyctl.h"
#include "rwy_blob.h"

static int g_stale;   /* the last audit found stale record entries */

static void audit_line(void *ctx, const char *line) { (void)ctx; printf("ERROR: %s\n", line); }

static void failed_line(void *ctx, const char *line) { (void)ctx; printf("info: %s\n", line); }

static int audit_report(int *counter_bad, int *record_bad) {
    rwy_bootcfg_audit_t au;
    int rc = rwy_bootcfg_audit(&g_store, &au, audit_line, 0);
    if (rc) { warn("cannot audit: %s", rwy_strerror(rc)); return -1; }
    if (au.incomplete) printf("efi/runway: %d slots, %d incomplete\n", au.slots, au.incomplete);
    else printf("efi/runway: %d slots\n", au.slots);
    if (au.counter_bad) puts("ERROR: EFI/RUNWAY/REVISION is behind the slots");
    if (au.record_damaged) puts("ERROR: state record damaged (both copies), every slot reads INSTALLED");
    if (au.stale) puts("warn: state record has entries of removed slots");
    if (au.booted_gone) puts("warn: BOOTED names a slot that does not exist");
    if (au.over_slots) printf("warn: %d slots, more than SLOTS %d\n", au.slots, au.over_slots);
    (void)rwy_bootcfg_failed(&g_store, failed_line, 0);
    *counter_bad = au.counter_bad;
    *record_bad = au.record_damaged;
    g_stale = au.stale;
    return au.violations + au.counter_bad + au.record_damaged;
}

/* Fix the counter and the state record; the caller has checked that nothing else is wrong. */
static int repair_bootcfg(int counter_bad) {
    int rc;
    rwy_set_actor(&g_store, RWY_ACTOR_SYSTEM);
    rc = counter_bad ? rwy_bootcfg_repair_counter(&g_store) : 0;
    if (rc < 0) { warn("cannot repair: %s", rwy_strerror(rc)); return exit_of(rc); }
    if (rc) puts("repair: EFI/RUNWAY/REVISION fixed");
    rc = rwy_bootcfg_repair_record(&g_store);
    if (rc < 0) { warn("cannot repair: %s", rwy_strerror(rc)); return exit_of(rc); }
    if (rc) puts("repair: state record written");
    return 0;
}

static void stale_line(void *ctx, int kind, const char *path) {
    (void)ctx;
    printf("warn: %s: %s\n", path, kind == RWY_BLOB_ORPHAN ? "orphan blob (no slot links it)" : "staging file left behind");
}

/* Orphan blobs and staging files. With --repair, remove them in one commit. */
static int stale_blobs(int repair) {
    int n = rwy_blob_stale(&g_store, stale_line, 0);

    if (n < 0) { warn("cannot look for orphan blobs: %s", rwy_strerror(n)); return exit_of(n); }
    if (!n || !repair) return 0;
    rwy_set_actor(&g_store, RWY_ACTOR_SYSTEM);
    n = rwy_blob_stale_remove(&g_store);
    if (n < 0) { warn("cannot repair: %s", rwy_strerror(n)); return exit_of(n); }
    printf("repair: %d orphan blobs and staging files removed\n", n);
    return 0;
}

static void bad_blob_line(void *ctx, const char *name, const char *actual, const char *slot) {
    (void)ctx;
    if (slot) printf("repair: %s FAILED (bad_blob), it links BLOBS/%s\n", slot, name);
    else printf("ERROR: BLOBS/%s: content is %s\n", name, actual);
}

/* Options of check: --blobs, and --repair and --unlock, which go together. */
static int parse_check(int argc, char **argv, int *repair, int *blobs) {
    int i, unlock = 0;
    *repair = *blobs = 0;
    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--repair")) *repair = 1;
        else if (!strcmp(argv[i], "--unlock")) unlock = 1;
        else if (!strcmp(argv[i], "--blobs")) *blobs = 1;
        else { warn("usage: %s", "check [--blobs] [--repair --unlock]"); return X_USAGE; }
    }
    if (*repair != unlock) { warn("%s", "--repair and --unlock go together"); return X_USAGE; }
    return 0;
}

/* Fix the counter and the record when --repair asks, then audit again. */
static int repair_and_audit(int repair, int *bad, int *counter_bad, int *record_bad) {
    int rc;
    if (!repair || (!*bad && !g_stale)) return 0;
    if (*bad != *counter_bad + *record_bad) { puts("repair: REFUSED (violations other than the counter and the record) - store untouched"); return X_CHECK; }
    if ((rc = repair_bootcfg(*counter_bad))) return rc;
    return (*bad = audit_report(counter_bad, record_bad)) < 0 ? X_IO : 0;
}

/* Orphans and staging files always; with --blobs, the content of every blob. */
static int check_blobs(int repair, int hash, int *bad) {
    int rc = stale_blobs(repair);
    if (rc || !hash) return rc;
    if (repair) rwy_set_actor(&g_store, RWY_ACTOR_SYSTEM);
    if ((rc = rwy_blob_verify(&g_store, repair, bad_blob_line, 0)) < 0) { warn("cannot check the blobs: %s", rwy_strerror(rc)); return exit_of(rc); }
    *bad += rc;
    return 0;
}

int cmd_check(int argc, char **argv) {
    int repair, blobs, bad, counter_bad, record_bad, rc;
    if ((rc = parse_check(argc, argv, &repair, &blobs))) return rc;
    if (repair && (rc = system_allowed())) return rc;
    if (!have_efi_runway()) { warn("%s", "EFI/RUNWAY is absent (nothing to check)"); return 0; }
    if ((bad = audit_report(&counter_bad, &record_bad)) < 0) return X_IO;
    if ((rc = repair_and_audit(repair, &bad, &counter_bad, &record_bad))) return rc;
    if ((rc = check_blobs(repair, blobs, &bad))) return rc;
    printf("check: %s (%d errors)\n", bad ? "CORRUPT" : "OK", bad);
    return bad ? X_CHECK : 0;
}
