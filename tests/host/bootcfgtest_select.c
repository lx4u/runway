/* Slot selection, recovery, fallback, condemn and the limits of a full store. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "bootcfgtest.h"
#include "rwy_bootcfg.h"

void test_select(void) {
    rwy_boot_choice_t c;
    uint64_t g, q;
    printf("== selection ==\n");
    /* Not engaged: no subtree, then a subtree without slots. */
    g = gen();
    EXPECT(sel(&c), OK, "select without subtree");
    CHECK(!c.engaged && !c.booted && gen() == g && !logbuf[0], "no subtree: legacy boot, store untouched, silent");
    fresh_subtree();
    g = gen();
    EXPECT(sel(&c), OK, "select without slots");
    CHECK(!c.engaged && !c.booted && gen() == g, "no slots: legacy boot, store untouched");
    EXPECT(rwy_bootcfg_select(&S, &c, logfn, NULL), PERM, "USER actor refused");

    /* OK top slot: boot as is; BOOTED/REASON written once, then no write at all. */
    fin(2, "OK");
    fin(1, "OK");
    EXPECT(sel(&c), OK, "select OK slot");
    CHECK(c.engaged && c.booted && !strcmp(c.slot, "CONFIG1") && c.revision == 2, "top revision wins, not top name");
    CHECK(has("RUNWAY: booting CONFIG1 (rev 2, OK)"), "message for OK");
    CHECK(booted_is("CONFIG1") && text_is(T "/REASON", "normal\n"), "BOOTED and REASON written");
    char flbuf[4096];
    rwy_flight_entry_t uncomm;
    CHECK(read_all(T "/FLIGHT", flbuf, sizeof(flbuf)) > 0, "FLIGHT virtual file readable");
    CHECK(strstr(flbuf, "event=BOOT slot=CONFIG1 reason=normal") != NULL, "FLIGHT records boot selection");
    g = gen(); q = rseq();
    EXPECT(sel(&c), OK, "select again");
    CHECK(c.booted && gen() == g && rseq() == q, "confirmed OK slot rebooting: no write");
    EXPECT(wr(T "/BOOTED/STATE", 0, "FAILED\n"), OK, "write through BOOTED reaches the slot");
    CHECK(text_is(T "/CONFIG1/STATE", "FAILED\n") && text_is(T "/CONFIG1/CAUSE", "operator\n"), "...CONFIG1, cause operator");
    as_sys(); EXPECT(wr(T "/CONFIG1/TRIES", 0, "3\n"), OK, "set tries"); as_user();
    CHECK(read_all(T "/FLIGHT", flbuf, sizeof(flbuf)) > 0, "FLIGHT readable after tries");
    CHECK(strstr(flbuf, "event=TRIES slot=CONFIG1") != NULL, "FLIGHT records tries change");
    CHECK(strstr(flbuf, "event=TRIES") < strstr(flbuf, "event=BOOT"), "FLIGHT is reverse chronological");
    memset(&uncomm, 0, sizeof(uncomm));
    uncomm.rec_seq = S.rec_seq + 10;
    uncomm.event = RWY_FLIGHT_EVENT_BOOT;
    strcpy(uncomm.booted, "GHOST");
    rwy_flight_append(&S.dev, S.sb.flight_start, S.sb.flight_blocks, &uncomm);
    CHECK(read_all(T "/FLIGHT", flbuf, sizeof(flbuf)) > 0, "FLIGHT readable with uncommitted entry");
    CHECK(strstr(flbuf, "GHOST") == NULL, "uncommitted entry filtered out");
    set_state(1, "OK\n");
    CHECK(rd(T "/CONFIG1/CAUSE") != OK, "a slot that is not FAILED has no CAUSE");

    /* INSTALLED -> TESTING; TRIES is the total number of boots (default 1). */
    fin(3, "INSTALLED");
    EXPECT(sel(&c), OK, "select INSTALLED");
    CHECK(!strcmp(c.slot, "CONFIG3") && c.revision == 3 && c.tries_left == 0 && c.trial == 1, "INSTALLED slot chosen, default 1 try");
    CHECK(text_is(T "/CONFIG3/STATE", "TESTING\n") && text_is(T "/CONFIG3/TRIES", "0\n"), "TESTING, TRIES consumed");
    CHECK(has("booting CONFIG3 (rev 3, TESTING, 0 tries left)") && booted_is("CONFIG3"), "message and BOOTED");
    g = gen(); q = rseq();
    EXPECT(sel(&c), OK, "unconfirmed TESTING without tries");
    CHECK(!strcmp(c.slot, "CONFIG1") && rseq() == q + 1 && gen() == g && c.trial == 0, "falls back to the next slot in ONE record write");
    CHECK(has("CONFIG3 FAILED, skipped") && text_is(T "/CONFIG3/STATE", "FAILED\n") && booted_is("CONFIG1"),
          "CONFIG3 FAILED, BOOTED moved");
    CHECK(has("CONFIG3 FAILED, skipped (tries_used_up)") && text_is(T "/CONFIG3/CAUSE", "tries_used_up\n"), "the cause is logged and kept");
    g = gen(); q = rseq();
    EXPECT(sel(&c), OK, "FAILED stays skipped");
    CHECK(!strcmp(c.slot, "CONFIG1") && rseq() == q && gen() == g && has("CONFIG3 FAILED, skipped (tries_used_up)"),
          "FAILED is skipped, no write, the cause is still logged");

    /* TRIES = 3 gives exactly three boots. */
    fin(4, "INSTALLED");
    set_file(4, "TRIES", "3\n");
    for (int i = 2; i >= 0; i--) {
        EXPECT(sel(&c), OK, "boot with tries");
        CHECK(!strcmp(c.slot, "CONFIG4") && (int)c.tries_left == i, "tries count down");
    }
    CHECK(has("TESTING, 0 tries left"), "last try message");
    EXPECT(sel(&c), OK, "fourth boot");
    CHECK(!strcmp(c.slot, "CONFIG1") && text_is(T "/CONFIG4/STATE", "FAILED\n"), "fourth boot: FAILED and fallback");
    fin(5, "INSTALLED");
    set_file(5, "TRIES", "2\n");
    EXPECT(sel(&c), OK, "boot");
    CHECK(has("TESTING, 1 try left"), "singular message");
    set_state(5, "OK\n");                  /* the OS confirmed */
    EXPECT(sel(&c), OK, "confirmed");
    CHECK(!strcmp(c.slot, "CONFIG5") && text_is(T "/CONFIG5/STATE", "OK\n") && text_is(T "/CONFIG5/TRIES", "1\n"), "OK slot boots, TRIES untouched");

    /* Invalid slots are ignored; the best valid one boots. */
    fin(6, "OK");
    EXPECT(rm(T "/CONFIG6/KERNEL"), OK, "remove KERNEL of a finalized slot");
    EXPECT(sel(&c), OK, "select with invalid top");
    CHECK(!strcmp(c.slot, "CONFIG5") && has("CONFIG6 invalid (KERNEL missing), ignored"), "missing KERNEL ignored");
    fin(7, "OK");
    EXPECT(rm(T "/CONFIG7/KERNEL"), OK, "remove KERNEL");
    priv(1); EXPECT(sy(T "/CONFIG7/KERNEL", "../nowhere"), OK, "dangling link"); priv(0);
    EXPECT(sel(&c), OK, "dangling");
    CHECK(!strcmp(c.slot, "CONFIG5") && has("CONFIG7 invalid (KERNEL invalid), ignored"), "dangling KERNEL ignored");
    slot(8);                               /* incomplete: no REVISION, silently ignored */
    fin(9, "OK");
    set_file(9, "REVISION", "junk\n");
    EXPECT(sel(&c), OK, "unparsable REVISION");
    CHECK(!strcmp(c.slot, "CONFIG5") && has("CONFIG9 invalid (REVISION invalid), ignored") && !has("CONFIG8"),
          "unparsable REVISION invalid, incomplete slot silent");

    /* Revision orders slots, not the directory name: CONFIG20 is older than CONFIG11. */
    fin(20, "OK");
    fin(11, "OK");
    EXPECT(sel(&c), OK, "order");
    CHECK(!strcmp(c.slot, "CONFIG11"), "highest REVISION wins over the name");

    /* Nothing bootable: transitions are still written, BOOTED stays. */
    for (int n = 1; n <= 20; n++) {
        char p[64];
        snprintf(p, sizeof(p), T "/CONFIG%d/STATE", n);
        if (num(p) != -1 || read_all(p, (char[4]){0}, 4) >= 0) set_state(n, "FAILED\n");
    }
    fin(12, "INSTALLED");
    set_file(12, "TRIES", "0\n");
    q = rseq();
    EXPECT(sel(&c), OK, "all failed");
    CHECK(c.engaged && !c.booted && rseq() == q + 1 && text_is(T "/CONFIG12/STATE", "FAILED\n"),
          "nothing bootable: not booted, FAILED written");
    CHECK(booted_is("CONFIG11"), "BOOTED untouched when nothing boots");
}

void test_recovery_unlock(void) {
    printf("== RECOVERY unlock ==\n");
    fresh_subtree();
    EXPECT(rwy_bootcfg_unlock_recovery(&S, 1), PERM, "USER cannot unlock");
    as_sys();
    EXPECT(mk(T "/RECOVERY"), PERM, "SYSTEM is sealed out without the unlock");
    EXPECT(rwy_bootcfg_unlock_recovery(&S, 1), OK, "SYSTEM unlocks");
    EXPECT(mk(T "/RECOVERY"), OK, "unlocked: create RECOVERY");
    EXPECT(cr(T "/RECOVERY/KERNEL"), OK, "unlocked: create KERNEL");
    EXPECT(wr(T "/RECOVERY/KERNEL", 0, "rk"), OK, "unlocked: write KERNEL");
    EXPECT(cr(T "/RECOVERY/STATE"), OK, "unlocked: create STATE");
    EXPECT(wr(T "/RECOVERY/STATE", 0, "OK\n"), OK, "unlocked: STATE takes any valid state");
    EXPECT(wr(T "/RECOVERY/STATE", 0, "bogus"), INV, "...but is still validated");
    EXPECT(cr(T "/RECOVERY/REVISION"), OK, "unlocked: REVISION allowed (ignored by selection)");
    EXPECT(wr(T "/RECOVERY/REVISION", 0, "x"), INV, "...and validated");
    EXPECT(cr(T "/RECOVERY/BOGUS"), INV, "the schema still applies");
    EXPECT(ch(T "/RECOVERY/KERNEL"), PERM, "chmod still refused");
    EXPECT(wr(T "/RECOVERY/KERNEL", 0, "rk2"), OK, "unlocked: rewrite KERNEL");
    EXPECT(ln(T "/RECOVERY/KERNEL", T "/BLOBS/rk"), OK, "share the payload");
    EXPECT(wr(T "/RECOVERY/KERNEL", 0, "x"), PERM, "a shared payload is immutable even unlocked");
    EXPECT(rm(T "/BLOBS/rk"), OK, "unshare");
    as_user();
    EXPECT(wr(T "/RECOVERY/KERNEL", 0, "x"), PERM, "USER stays sealed out while the flag is set");
    EXPECT(rm(T "/RECOVERY/KERNEL"), PERM, "...for namespace operations too");
    as_sys();
    EXPECT(rwy_bootcfg_unlock_recovery(&S, 0), OK, "lock again");
    EXPECT(wr(T "/RECOVERY/KERNEL", 0, "x"), PERM, "sealed again");
    EXPECT(rwy_bootcfg_unlock_recovery(&S, 1), OK, "unlock");
    EXPECT(rwy_remount(&S), OK, "remount");
    EXPECT(wr(T "/RECOVERY/KERNEL", 0, "x"), PERM, "remount drops the unlock");
    as_user();
}

void test_select_recovery(void) {
    rwy_boot_choice_t c;
    uint64_t g, q;
    printf("== selection: RECOVERY and RECOVER_NEXT ==\n");

    /* RECOVER_NEXT alone does not engage; without RECOVERY it is ignored and stays. */
    fresh_subtree();
    EXPECT(cr(T "/RECOVER_NEXT"), OK, "USER creates RECOVER_NEXT");
    EXPECT(sel(&c), OK, "RECOVER_NEXT alone");
    CHECK(!c.engaged && !logbuf[0], "RECOVER_NEXT alone: legacy boot, silent");
    fin(1, "OK");
    EXPECT(sel(&c), OK, "RECOVER_NEXT without RECOVERY");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG1") && has("RUNWAY: RECOVER_NEXT ignored, no RECOVERY"), "ignored with a message");
    CHECK(read_all(T "/RECOVER_NEXT", (char[2]){0}, 2) == 0, "...and left in place");

    /* RECOVER_NEXT with RECOVERY: RECOVERY boots once, the request is consumed in the same commit. */
    mkrecovery();
    EXPECT(sel(&c), OK, "requested");
    CHECK(c.engaged && c.booted && !strcmp(c.slot, "RECOVERY") && c.revision == 0, "RECOVERY chosen");
    CHECK(has("RUNWAY: recovery requested, booting RECOVERY"), "message");
    CHECK(booted_is("RECOVERY") && text_is(T "/REASON", "requested\n"), "BOOTED and REASON");
    CHECK(read_all(T "/RECOVER_NEXT", (char[2]){0}, 2) < 0, "RECOVER_NEXT consumed");
    CHECK(text_is(T "/CONFIG1/STATE", "OK\n") && text_is(T "/RECOVERY/STATE", "FAILED\n"), "no state touched");
    EXPECT(wr(T "/BOOTED/STATE", 0, "OK\n"), PERM, "the OS cannot confirm RECOVERY");
    EXPECT(sel(&c), OK, "next boot");
    CHECK(!strcmp(c.slot, "CONFIG1") && booted_is("CONFIG1") && text_is(T "/REASON", "normal\n"), "one-shot: back to the slot");
    EXPECT(cr(T "/RECOVER_NEXT"), OK, "USER requests again");
    EXPECT(sel(&c), OK, "requested again");
    CHECK(!strcmp(c.slot, "RECOVERY"), "a new request is honoured");
    EXPECT(sel(&c), OK, "and consumed again");
    CHECK(!strcmp(c.slot, "CONFIG1"), "...");

    /* All slots failed: RECOVERY with reason all_failed; nothing changes, nothing is committed. */
    set_state(1, "FAILED\n");
    EXPECT(sel(&c), OK, "all failed");
    CHECK(c.booted && !strcmp(c.slot, "RECOVERY") && has("CONFIG1 FAILED, skipped") &&
          has("RUNWAY: no bootable config (all_failed), booting RECOVERY"), "all_failed boots RECOVERY");
    CHECK(booted_is("RECOVERY") && text_is(T "/REASON", "all_failed\n"), "BOOTED and REASON");
    g = gen(); q = rseq();
    EXPECT(sel(&c), OK, "again");
    CHECK(!strcmp(c.slot, "RECOVERY") && gen() == g && rseq() == q, "unchanged: no write");
    set_state(1, "OK\n");
    EXPECT(sel(&c), OK, "a slot recovers");
    CHECK(!strcmp(c.slot, "CONFIG1") && text_is(T "/REASON", "normal\n"), "normal again");

    /* RECOVERY WATCHDOG, global fallback. */
    unlock(1); as_sys();
    EXPECT(cr(T "/RECOVERY/WATCHDOG"), OK, "fixture");
    EXPECT(wr(T "/RECOVERY/WATCHDOG", 0, "7\n"), OK, "fixture");
    as_user(); unlock(0);
    set_state(1, "FAILED\n");
    EXPECT(sel(&c), OK, "recovery watchdog");
    CHECK(c.watchdog == 7 && !has("WATCHDOG"), "RECOVERY/WATCHDOG carried, nothing logged");
}

void test_select_recovery_only(void) {
    rwy_boot_choice_t c;
    printf("== selection: RECOVERY without slots ==\n");
    fresh_subtree();
    mkrecovery();
    EXPECT(sel(&c), OK, "RECOVERY alone");
    CHECK(c.engaged && c.booted && !strcmp(c.slot, "RECOVERY") && has("(all_failed), booting RECOVERY"),
          "RECOVERY alone engages native mode");
    CHECK(text_is(T "/REASON", "all_failed\n") && booted_is("RECOVERY"), "REASON and BOOTED");
    unlock(1); as_sys();
    EXPECT(rm(T "/RECOVERY/KERNEL"), OK, "fixture");
    as_user(); unlock(0);
    EXPECT(sel(&c), OK, "invalid RECOVERY");
    CHECK(c.engaged && !c.booted && has("RECOVERY invalid (KERNEL missing), ignored"), "invalid RECOVERY counts as absent");
    EXPECT(cr(T "/RECOVER_NEXT"), OK, "request");
    EXPECT(sel(&c), OK, "request with invalid RECOVERY");
    CHECK(!c.booted && has("RUNWAY: RECOVER_NEXT ignored, no RECOVERY"), "ignored");
}

static int fb(rwy_boot_choice_t *c, const char *after) {
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    int rc = rwy_bootcfg_fallback(&S, c, after);
    rwy_set_actor(&S, RWY_ACTOR_USER);
    return rc;
}

void test_fallback(void) {
    rwy_boot_choice_t c;
    uint64_t g, q;
    printf("== fallback: boot without recording ==\n");
    fresh_subtree();
    EXPECT(fb(&c, NULL), OK, "no slots");
    CHECK(!c.engaged && !c.booted, "no slots: not engaged");
    fin(1, "OK");
    fin(2, "OK");
    fin(3, "TESTING");
    fin(4, "INSTALLED");
    g = gen(); q = rseq();
    EXPECT(fb(&c, NULL), OK, "fallback");
    CHECK(c.engaged && c.booted && !strcmp(c.slot, "CONFIG2") && c.revision == 2 && c.tries_left == 0,
          "highest OK slot, not the TESTING or INSTALLED ones above it");
    CHECK(gen() == g && rseq() == q && !rwy_in_txn(&S), "nothing written, no transaction left open");
    CHECK(read_all(T "/BOOTED", (char[2]){0}, 2) < 0 && read_all(T "/REASON", (char[2]){0}, 2) < 0,
          "BOOTED and REASON not created");
    EXPECT(fb(&c, "CONFIG2"), OK, "after CONFIG2");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG1"), "next lower OK slot");
    EXPECT(fb(&c, "CONFIG1"), OK, "after CONFIG1");
    CHECK(c.engaged && !c.booted, "no OK slot left, no RECOVERY");
    set_state(2, "FAILED\n");
    EXPECT(fb(&c, NULL), OK, "FAILED skipped");
    CHECK(!strcmp(c.slot, "CONFIG1"), "FAILED slot is not chosen");

    /* RECOVERY comes after the OK slots; a pending request is ignored. */
    mkrecovery();
    EXPECT(cr(T "/RECOVER_NEXT"), OK, "request");
    g = gen(); q = rseq();
    EXPECT(fb(&c, NULL), OK, "slot before RECOVERY");
    CHECK(!strcmp(c.slot, "CONFIG1"), "request ignored, OK slot first");
    EXPECT(fb(&c, "CONFIG1"), OK, "RECOVERY");
    CHECK(c.booted && !strcmp(c.slot, "RECOVERY") && c.revision == 0, "RECOVERY after the last OK slot");
    EXPECT(fb(&c, "RECOVERY"), OK, "after RECOVERY");
    CHECK(c.engaged && !c.booted, "nothing after RECOVERY");
    CHECK(read_all(T "/RECOVER_NEXT", (char[2]){0}, 2) == 0 && gen() == g && rseq() == q, "request still there, nothing written");
    EXPECT(fb(&c, "CONFIG99"), INV, "unknown slot");
    EXPECT(rwy_bootcfg_fallback(&S, &c, NULL), PERM, "USER actor refused");

    /* A global BOOT-DELAY and a slot WATCHDOG are carried. */
    set_file(1, "WATCHDOG", "9\n");
    cr(T "/BOOT-DELAY");
    EXPECT(wr(T "/BOOT-DELAY", 0, "4\n"), OK, "BOOT-DELAY");
    EXPECT(fb(&c, NULL), OK, "carry");
    CHECK(c.watchdog == 9 && c.boot_delay == 4, "WATCHDOG and BOOT-DELAY carried");

    /* Slot LABEL is carried by fallback; absent label clears has_label. */
    set_file(1, "LABEL", "slot1-fallback-label\n");
    EXPECT(fb(&c, NULL), OK, "carry label");
    CHECK(c.has_label && !strcmp(c.label, "slot1-fallback-label"), "LABEL carried in fallback");
    EXPECT(rm(T "/CONFIG1/LABEL"), OK, "remove LABEL");
    EXPECT(fb(&c, NULL), OK, "no label");
    CHECK(!c.has_label && !c.label[0], "no LABEL in fallback");
}

void test_preview(void) {
    rwy_boot_choice_t c, p;
    uint64_t g, q;
    printf("== selection preview ==\n");
    fresh_subtree();
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    EXPECT(rwy_bootcfg_preview(&S, &p), OK, "no slots");
    rwy_set_actor(&S, RWY_ACTOR_USER);
    CHECK(!p.engaged && !p.booted, "no slots: not engaged");
    fin(1, "OK");
    fin(2, "INSTALLED");
    set_file(2, "LABEL", "slot2-preview-label\n");
    g = gen(); q = rseq();
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    EXPECT(rwy_bootcfg_preview(&S, &p), OK, "preview");
    rwy_set_actor(&S, RWY_ACTOR_USER);
    CHECK(p.engaged && p.booted && !strcmp(p.slot, "CONFIG2") && p.revision == 2 && p.trial == 1, "the slot select would take");
    CHECK(p.has_label && !strcmp(p.label, "slot2-preview-label"), "preview carried label");
    CHECK(gen() == g && rseq() == q && !rwy_in_txn(&S), "nothing written, no transaction left open");
    CHECK(text_is(T "/CONFIG2/STATE", "INSTALLED\n") && text_is(T "/CONFIG2/TRIES", "1\n") &&
          read_all(T "/BOOTED", (char[2]){0}, 2) < 0, "STATE, TRIES and BOOTED untouched");
    EXPECT(rwy_bootcfg_preview(&S, &(rwy_boot_choice_t){0}), PERM, "USER actor refused");
    EXPECT(sel(&c), OK, "select");
    CHECK(!strcmp(c.slot, p.slot) && c.revision == p.revision && c.tries_left == p.tries_left && c.trial == p.trial, "select takes the same slot");
    CHECK(c.has_label && !strcmp(c.label, "slot2-preview-label"), "select carried label");

    /* A request for RECOVERY is previewed but not consumed. */
    mkrecovery();
    EXPECT(cr(T "/RECOVER_NEXT"), OK, "request");
    g = gen(); q = rseq();
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    EXPECT(rwy_bootcfg_preview(&S, &p), OK, "preview with a request");
    rwy_set_actor(&S, RWY_ACTOR_USER);
    CHECK(!strcmp(p.slot, "RECOVERY") && p.trial == 0 && gen() == g && rseq() == q && read_all(T "/RECOVER_NEXT", (char[2]){0}, 2) == 0,
          "RECOVERY previewed, request still there");
}

void test_select_watchdog_version(void) {
    rwy_boot_choice_t c;
    printf("== selection: WATCHDOG and VERSION ==\n");
    fresh_subtree();
    fin(1, "OK");
    EXPECT(sel(&c), OK, "no watchdog");
    CHECK(c.watchdog == 0 && !has("WATCHDOG"), "no WATCHDOG: nothing armed, no message");
    seed(T "/WATCHDOG", "30\n");
    EXPECT(sel(&c), OK, "global");
    CHECK(c.watchdog == 30 && !has("WATCHDOG"), "global WATCHDOG carried, nothing logged");
    set_file(1, "WATCHDOG", "5\n");
    EXPECT(sel(&c), OK, "slot overrides");
    CHECK(c.watchdog == 5 && !has("WATCHDOG"), "slot WATCHDOG overrides the global one");
    set_file(1, "WATCHDOG", "0\n");
    EXPECT(sel(&c), OK, "slot zero");
    CHECK(c.watchdog == 0 && !has("WATCHDOG"), "0 = off, even with a global value");
    priv(1); EXPECT(tr(T "/VERSION", 0), OK, "fixture"); EXPECT(wr(T "/VERSION", 0, "2\n"), OK, "fixture"); priv(0);
    EXPECT(sel(&c), OK, "newer layout");
    CHECK(c.booted && has("RUNWAY: layout v2 newer than supported, using known entries only"), "newer VERSION logged, still boots");
}

static int ballast(int n, size_t sz) {        /* file /Bn of sz bytes; the rc of the first failing call */
    static char blk[1 << 20];
    char p[32];
    size_t done = 0;
    int rc;
    snprintf(p, sizeof(p), "/B%d", n);
    rc = cr(p);
    if (rc) return rc;
    memset(blk, 'x', sizeof(blk));
    while (done < sz) {
        size_t k = sz - done < sizeof(blk) ? sz - done : sizeof(blk);
        rwy_begin(&S);
        rc = txn(rwy_write_path(&S, p, done, blk, k));
        if (rc) return rc;
        done += k;
    }
    return OK;
}
static int slot_clean(int n) {                /* incomplete: no REVISION, STATE untouched */
    char p[64];
    snprintf(p, sizeof(p), T "/CONFIG%d/REVISION", n);
    return rev(n) == -1 && read_all(p, (char[4]){0}, 4) < 0;
}

static int fill_inodes(void) {                /* empty files /Z0, /Z1 ... until the inode table is full; the count */
    char p[32];
    int n = 0;
    for (;; n++) {
        snprintf(p, sizeof(p), "/Z%d", n);
        if (cr(p) != OK) return n;
    }
}
static void fill_blocks_of(const char *p) {   /* grow an existing file to NOSPC, so no inode is needed */
    static char blk[1 << 20];
    uint64_t at = 0;
    memset(blk, 'x', sizeof(blk));
    for (size_t sz = 1 << 20; sz >= 1; sz >>= 2)
        for (;;) {
            rwy_begin(&S);
            if (txn(rwy_write_path(&S, p, at, blk, sz))) break;
            at += sz;
        }
}
static int fill_store(void) {                 /* fill to NOSPC, big files first, then the gaps; the file count */
    int nb = 0;
    for (size_t sz = 1 << 20; sz >= 1; sz >>= 2)
        while (ballast(nb++, sz) == OK)
            continue;
    return nb;
}

void test_full_store(void) {
    rwy_boot_choice_t c;
    int nb = 0, rc, tries = 0;
    long cnt;
    printf("== install on a full store ==\n");
    fresh_subtree();
    fin(1, "OK");                                /* the working slot */
    cnt = counter();
    slot(2);
    nb = fill_store();
    /* A payload copy that runs out of space fails with NOSPC, nothing else. */
    for (rc = OK, tries = 0; rc == OK && tries < 4096; tries++) {
        char big[4096];
        memset(big, 'k', sizeof(big));
        rwy_begin(&S);
        rc = txn(rwy_write_path(&S, T "/CONFIG2/KERNEL", 16 + (uint64_t)tries * sizeof(big), big, sizeof(big)));
    }
    CHECK(rc == RWY_ERR_NOSPC, "the copy ends with NOSPC");
    EXPECT(install(2), RWY_ERR_NOSPC, "INSTALLED on a full store reports NOSPC");

    CHECK(slot_clean(2) && counter() == cnt, "failed install: slot incomplete, counter untouched");

    /* The updater can clean up on a full store, then the space is back. */
    EXPECT(rm(T "/CONFIG2/KERNEL"), OK, "remove the partial KERNEL on a full store");
    EXPECT(rm(T "/CONFIG2/STATE"), OK, "remove STATE");
    EXPECT(rd(T "/CONFIG2"), OK, "remove the incomplete slot");

    /* A new slot needs transient room: every create rewrites its directory before the old copy is freed.
     * The reserve is not for installs, so a full store has none; give it one block back. */
    for (int k = nb - 1; k >= 0; k--) {          /* the newest ballast file that exists */
        char p[32];
        snprintf(p, sizeof(p), "/B%d", k);
        if (rm(p) == OK)
            break;
    }
    /* Retry at every amount of free space: any failure leaves the slot incomplete. */
    slot(3);
    for (int i = 0; i < 70; i++) {
        char p[32];
        rc = install(3);
        if (rc == OK) break;
        CHECK(rc == RWY_ERR_NOSPC, "failure is NOSPC");
        CHECK(slot_clean(3) && counter() == cnt, "failed finalize leaves slot and counter untouched");
        snprintf(p, sizeof(p), "/B%d", nb - 1 - i);
        rm(p);
    }
    CHECK(rev(3) == cnt + 1 && counter() == cnt + 1, "once there is room the install finalizes");
    for (int i = 70; i < 140; i++) {             /* selection needs room for its own commit (T1) */
        char p[32];
        snprintf(p, sizeof(p), "/B%d", nb - 1 - i);
        rm(p);
    }
    CHECK(sel(&c) == OK && c.booted && !strcmp(c.slot, "CONFIG3"), "the retried slot is selected");
}

static int condemn(const char *slot) {
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    int rc = rwy_bootcfg_condemn(&S, slot);
    rwy_set_actor(&S, RWY_ACTOR_USER);
    return rc;
}

/* A state change is always writable: the last blocks of a full store are kept for it. */
void test_state_on_full_store(void) {
    rwy_boot_choice_t c;
    uint64_t q;
    int n;
    printf("== state change on a full store ==\n");
    fresh_subtree();
    fin(1, "OK");
    fin(2, "OK");
    fin(3, "INSTALLED");
    n = fill_inodes();
    CHECK(n > 200 && cr("/Zlast") != OK && rwy_free_blocks(&S) > 0, "every inode is used and blocks are free");
    fill_blocks_of("/Z0");
    CHECK(rwy_free_blocks(&S) < 4 && cr("/Zlast") != OK, "the store has no inode and (almost) no block left");
    EXPECT(cr("/ORDINARY"), RWY_ERR_NOSPC, "an ordinary create is refused");
    /* The OS side: fail a slot, confirm one, ask for a recovery. */
    EXPECT(wr(T "/CONFIG1/STATE", 0, "FAILED\n"), OK, "USER writes STATE");
    EXPECT(cr(T "/RECOVER_NEXT"), OK, "USER creates RECOVER_NEXT");
    EXPECT(rm(T "/RECOVER_NEXT"), OK, "USER removes RECOVER_NEXT");
    /* RUNWAY.EFI: the selection commit and the condemn commit. */
    q = rseq();
    EXPECT(sel(&c), OK, "selection is written");
    CHECK(rseq() > q && c.booted && !strcmp(c.slot, "CONFIG3"), "the pending slot was selected");
    EXPECT(condemn("CONFIG3"), OK, "condemn commits");
    EXPECT(sel(&c), OK, "selection falls back");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG2"), "...to the next slot");
    /* An install step is not a state change. */
    EXPECT(mk(T "/CONFIG9"), RWY_ERR_NOSPC, "a new slot is refused");
}

/* A commit that fails closes its transaction: the next call starts clean instead of failing with EINVAL
 * (FUSE and the one-shot wrappers do not abort after a failed commit). */
void test_failed_commit(const char *base) {
    const char *img = tpath("rwy_fc.img");
    file_t fl;
    rwy_block_dev_t inner, dev, d2;
    fi_t fi;
    rwy_store_t s;
    printf("== failed commit closes its transaction ==\n");
    copy_file(base, img);
    open_store(&S, &d2, &fl, img);
    fclose(fl.f);
    fl.f = fopen(img, "r+b");
    inner.read = file_rd; inner.write = file_wr; inner.flush = file_fl; inner.ctx = &fl;
    inner.size = RWY_STORE_SIZE; inner.block_size = RWY_BLOCK_SIZE;
    dev.read = fi_rd; dev.write = fi_wr; dev.flush = fi_fl; dev.ctx = &fi;
    dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
    fi.inner = inner; fi.point = 0; fi.fail_at = -1;
    CHECK(rwy_mount(&s, &dev) == OK, "mount");
    { int r1 = rwy_begin(&s), r2 = rwy_mkdir(&s, "/FC", RWY_S_IFDIR | 0755, 0, 0); CHECK(r1 == OK && r2 == OK, "mkdir staged"); if (r1 || r2) printf("DBG %d %d\n", r1, r2); }
    fi.fail_at = fi.point + 1;                   /* the first I/O of the commit */
    CHECK(rwy_commit(&s) != OK, "the commit fails");
    CHECK(!rwy_in_txn(&s), "the failed commit closed its transaction");
    fi.fail_at = -1;
    CHECK(rwy_begin(&s) == OK, "a new transaction can start");
    CHECK(rwy_mkdir(&s, "/FC", RWY_S_IFDIR | 0755, 0, 0) == OK && rwy_commit(&s) == OK, "the retry commits");
    fclose(fl.f);
}

/* R2: which slot `rwyctl update` may recycle. */
static int pick(char *name) {
    int rc;
    rwy_begin(&S);
    rc = rwy_bootcfg_recycle_pick(&S, name);
    rwy_abort(&S);
    return rc;
}
static void booted_to(const char *slot) { set_booted(slot); }

void test_recycle_pick(void) {
    char n[32];
    printf("== slot to recycle ==\n");
    EXPECT(pick(n), INV, "no EFI/Runway");
    fresh_subtree();
    EXPECT(pick(n), RWY_ERR_NOENT, "no slot");
    fin(1, "OK");
    EXPECT(pick(n), RWY_ERR_NOENT, "the only OK slot is the one that boots");
    fin(2, "OK");
    EXPECT(pick(n), OK, "two OK slots, nothing booted yet");
    CHECK(!strcmp(n, "CONFIG1"), "the older one, the newest OK slot is kept");
    booted_to("CONFIG1");
    EXPECT(pick(n), RWY_ERR_NOENT, "BOOTED and the newest OK slot are both kept");
    booted_to("CONFIG2");
    EXPECT(pick(n), OK, "BOOTED is the newest");
    CHECK(!strcmp(n, "CONFIG1"), "...the other one is recycled");

    /* A pending slot is never recycled. */
    set_state(1, "FAILED\n");
    fin(3, "INSTALLED");
    EXPECT(pick(n), OK, "FAILED slot next to a pending one");
    CHECK(!strcmp(n, "CONFIG1"), "FAILED is recycled, INSTALLED is not");
    set_state(1, "OK\n");
    EXPECT(pick(n), OK, "an older OK slot next to a pending one");
    CHECK(!strcmp(n, "CONFIG1"), "...is recycled");
    drop(1);
    EXPECT(pick(n), RWY_ERR_NOENT, "BOOTED and a pending slot: nothing may go");
    set_file(3, "STATE", "TESTING\n");
    EXPECT(pick(n), RWY_ERR_NOENT, "TESTING is pending too");
    fin(1, "FAILED");

    /* Preference: incomplete, then FAILED, then the oldest OK; revision order, not name order. */
    set_state(1, "FAILED\n");
    set_file(3, "STATE", "FAILED\n");
    EXPECT(pick(n), OK, "two FAILED slots");
    CHECK(!strcmp(n, "CONFIG3"), "the lowest revision of the FAILED ones (CONFIG1 was installed again, so it is newer)");
    slot(9);
    EXPECT(pick(n), OK, "an incomplete slot");
    CHECK(!strcmp(n, "CONFIG9"), "incomplete first");
    drop(9);
    set_file(1, "STATE", "OK\n");
    set_file(3, "STATE", "OK\n");
    booted_to("CONFIG3");
    EXPECT(pick(n), OK, "three OK slots");
    CHECK(!strcmp(n, "CONFIG2"), "lowest revision of the unprotected ones (CONFIG3 is BOOTED, CONFIG1 the newest)");
    set_file(2, "PIN", "1\n");
    EXPECT(pick(n), RWY_ERR_NOENT, "a pinned slot is not recycled");
    rwy_begin(&S);
    EXPECT(rwy_rmdir(&S, T "/CONFIG2"), RWY_ERR_BUSY, "a pinned slot cannot be removed");
    rwy_abort(&S);
    set_file(2, "PIN", "0\n");
    EXPECT(pick(n), OK, "unpinned again");
    CHECK(!strcmp(n, "CONFIG2"), "...and recycled");

    /* Revision order, not name order: CONFIG5 is older than CONFIG4. */
    drop(1); drop(2); drop(3);
    fin(5, "OK");
    fin(4, "OK");
    booted_to("CONFIG4");
    EXPECT(pick(n), OK, "names out of order");
    CHECK(!strcmp(n, "CONFIG5"), "CONFIG5 has the lower revision");

    /* BOOTED naming RECOVERY or nothing keeps only the newest OK slot. */
    set_booted("RECOVERY");
    EXPECT(pick(n), OK, "BOOTED names RECOVERY");
    CHECK(!strcmp(n, "CONFIG5"), "the newest OK slot is still kept");
    set_state(5, "FAILED\n");
    set_state(4, "FAILED\n");
    EXPECT(pick(n), OK, "all FAILED");
    CHECK(!strcmp(n, "CONFIG5"), "lowest revision");

    /* A slot with an unparsable REVISION is garbage, like an incomplete one. */
    set_state(5, "OK\n");
    set_state(4, "OK\n");
    fin(6, "OK");
    set_file(6, "REVISION", "junk\n");
    EXPECT(pick(n), OK, "unparsable REVISION");
    CHECK(!strcmp(n, "CONFIG6"), "recycled first");

    /* The ordered walk lists every slot that may go, best first, and can be cut short. */
    {
        char names[4][32];
        size_t cnt = 0;
        rwy_begin(&S);
        CHECK(rwy_bootcfg_recycle_order(&S, names, 4, &cnt) == OK && cnt == 2, "two slots may go");
        CHECK(cnt == 2 && !strcmp(names[0], "CONFIG6") && !strcmp(names[1], "CONFIG5"), "garbage first, then the older OK slot");
        CHECK(rwy_bootcfg_recycle_order(&S, names, 1, &cnt) == OK && cnt == 1 && !strcmp(names[0], "CONFIG6"), "a short list keeps the best");
        CHECK(rwy_bootcfg_recycle_order(&S, names, 0, &cnt) == INV, "no room is refused");
        rwy_abort(&S);
    }
}

/* QE1: the blocks that removing slots frees, with a payload shared through BLOBS/. */
static uint32_t used_blocks(void) {
    uint32_t used, usable;
    rwy_pool_usage(&S, &used, &usable);
    return used;
}
static int freed_by(char names[][32], size_t n, uint32_t *freed) {
    int rc;
    rwy_begin(&S);
    rc = rwy_bootcfg_evict_blocks(&S, names, n, freed);
    rwy_abort(&S);
    return rc;
}

void test_evict_blocks(void) {
    static char big[3 * 4096 + 1];
    char one[1][32] = { "CONFIG1" }, other[1][32] = { "CONFIG2" }, two[2][32] = { "CONFIG1", "CONFIG2" };
    char bad[1][32] = { "BLOBS" };
    uint32_t used0, used1, used2, f1, f2, f12;
    printf("== blocks freed by removing slots ==\n");
    memset(big, 'x', sizeof(big) - 1);
    fresh_subtree();
    fin(1, "OK"); fin(2, "OK"); fin(3, "OK");
    CHECK(cr(T "/BLOBS/x") == OK && wr(T "/BLOBS/x", 0, big) == OK, "a three block blob");
    priv(1);
    rm(T "/CONFIG1/KERNEL"); rm(T "/CONFIG2/KERNEL");
    EXPECT(ln(T "/BLOBS/x", T "/CONFIG1/KERNEL"), OK, "CONFIG1 shares the blob");
    EXPECT(ln(T "/BLOBS/x", T "/CONFIG2/KERNEL"), OK, "CONFIG2 shares the blob");
    rm(T "/BLOBS/x");
    priv(0);

    used0 = used_blocks();
    EXPECT(freed_by(one, 1, &f1), OK, "one slot");
    EXPECT(freed_by(other, 1, &f2), OK, "the other slot");
    EXPECT(freed_by(two, 2, &f12), OK, "two slots");
    CHECK(f12 == f1 + f2 + 3, "the blob is freed only when both slots go");
    EXPECT(freed_by(bad, 1, &f2), RWY_ERR_NOENT, "BLOBS is not a slot");
    drop(1);
    used1 = used_blocks();
    CHECK(used0 - used1 == f1, "removing one slot frees what was counted");
    drop(2);
    used2 = used_blocks();
    CHECK(used0 - used2 == f12, "removing both frees what was counted");
}

/* QE3: the slots to remove before a new slot of `need` blocks is installed. */
static int plan_for(uint32_t need, rwy_evict_plan_t *plan) {
    int rc;
    rwy_begin(&S);
    rc = rwy_bootcfg_evict_plan(&S, need, plan);
    rwy_abort(&S);
    return rc;
}

/* Blocks the quota leaves free, from the current use. */
static uint32_t quota_room(unsigned pct) {
    uint32_t used, usable;
    rwy_pool_usage(&S, &used, &usable);
    return (uint32_t)((uint64_t)usable * pct / 100) - used;
}

void test_evict_plan(void) {
    static char big[3 * 4096 + 1];
    rwy_evict_plan_t pl;
    char one[1][32] = { "CONFIG1" };
    uint32_t room, f1, f12;
    printf("== eviction plan ==\n");
    memset(big, 'x', sizeof(big) - 1);
    EXPECT(plan_for(1, &pl), INV, "no EFI/Runway");
    fresh_subtree();
    fin(1, "OK"); fin(2, "OK"); fin(3, "OK"); fin(4, "OK");
    EXPECT(plan_for(1000000, &pl), OK, "neither QUOTA nor SLOTS: nothing to plan");
    CHECK(pl.count == 0, "...nothing goes");

    /* CONFIG1 and CONFIG2 share a three block blob. */
    CHECK(cr(T "/BLOBS/x") == OK && wr(T "/BLOBS/x", 0, big) == OK, "a three block blob");
    priv(1);
    rm(T "/CONFIG1/KERNEL"); rm(T "/CONFIG2/KERNEL");
    EXPECT(ln(T "/BLOBS/x", T "/CONFIG1/KERNEL"), OK, "CONFIG1 shares the blob");
    EXPECT(ln(T "/BLOBS/x", T "/CONFIG2/KERNEL"), OK, "CONFIG2 shares the blob");
    rm(T "/BLOBS/x");
    priv(0);
    EXPECT(cr(T "/QUOTA"), OK, "USER creates QUOTA");
    EXPECT(wr(T "/QUOTA", 0, "50\n"), OK, "QUOTA 50");
    EXPECT(freed_by(one, 1, &f1), OK, "one slot");
    {
        char two[2][32] = { "CONFIG1", "CONFIG2" };
        EXPECT(freed_by(two, 2, &f12), OK, "two slots");
    }
    CHECK(f12 >= f1 + 3, "the blob goes with the second slot");

    room = quota_room(50);
    EXPECT(plan_for(room, &pl), OK, "the new slot just fits");
    CHECK(pl.count == 0, "...nothing goes");
    EXPECT(plan_for(room + 1, &pl), OK, "one block too many");
    CHECK(pl.count == 1 && !strcmp(pl.names[0], "CONFIG1") && pl.freed == f1, "the oldest slot goes");
    EXPECT(plan_for(room + f1 + 1, &pl), OK, "more than the first slot frees");
    CHECK(pl.count == 2 && !strcmp(pl.names[1], "CONFIG2") && pl.freed == f12, "two go, the shared blob counted once");
    EXPECT(plan_for(room + f12 + 1, &pl), RWY_ERR_NOSPC, "the protected slots and the new one do not fit");
    CHECK(pl.count == 0, "...nothing goes");

    /* With RECOVERY/, only the newest OK slot is protected. */
    mkrecovery();
    EXPECT(plan_for(quota_room(50) + f12 + 1, &pl), OK, "RECOVERY exists");
    CHECK(pl.count == 3 && !strcmp(pl.names[2], "CONFIG3"), "the second newest OK slot may go");
    set_booted("CONFIG3");
    EXPECT(plan_for(quota_room(50) + f12 + 1, &pl), RWY_ERR_NOSPC, "BOOTED is kept");
    set_booted("CONFIG4");

    /* SLOTS alone counts slots, quota is off. */
    priv(1); rm(T "/QUOTA"); priv(0);
    EXPECT(cr(T "/SLOTS"), OK, "USER creates SLOTS");
    EXPECT(wr(T "/SLOTS", 0, "5\n"), OK, "SLOTS 5");
    EXPECT(plan_for(1000000, &pl), OK, "room for one more slot");
    CHECK(pl.count == 0, "...nothing goes");
    EXPECT(wr(T "/SLOTS", 0, "4\n"), OK, "SLOTS 4");
    EXPECT(plan_for(1000000, &pl), OK, "at the cap");
    CHECK(pl.count == 1 && !strcmp(pl.names[0], "CONFIG1"), "one slot goes");
    EXPECT(wr(T "/SLOTS", 0, "1\n"), OK, "SLOTS 1");
    EXPECT(plan_for(1, &pl), RWY_ERR_NOSPC, "no room even after the unprotected slots");
}

/* The newest journal entry. */
static int last_journal(rwy_journal_entry_t *e) {
    rwy_transplog_info_t info;
    int rc = rwy_transplog_pick(&S.dev, S.sb.journal_start, S.sb.journal_blocks, &info);
    return rc ? rc : rwy_transplog_read_entry(&S.dev, S.sb.journal_start, S.sb.journal_blocks, &info, info.count - 1, e);
}

/* QE4: removing the planned slots, with one EVICT record each. */
void test_evict_execute(void) {
    rwy_evict_plan_t pl;
    rwy_journal_entry_t e;
    uint32_t blocks;
    printf("== eviction execute ==\n");
    fresh_subtree();
    fin(1, "FAILED"); fin(2, "OK"); fin(3, "OK"); fin(4, "OK");
    mkrecovery();
    EXPECT(cr(T "/SLOTS"), OK, "USER creates SLOTS");
    EXPECT(wr(T "/SLOTS", 0, "3\n"), OK, "SLOTS 3");
    rwy_begin(&S);
    EXPECT(rwy_bootcfg_evict_plan(&S, 1, &pl), OK, "plan");
    CHECK(pl.count == 2 && !strcmp(pl.names[0], "CONFIG1") && !strcmp(pl.names[1], "CONFIG2"), "two slots go");
    CHECK(pl.reasons[0] == RWY_EVICT_REASON_SLOTS, "SLOTS asked for it");
    EXPECT(rwy_bootcfg_evict_execute(&S, &pl), OK, "execute");
    EXPECT(rwy_commit(&S), OK, "commit");
    CHECK(rm(T "/CONFIG1") != OK && rm(T "/CONFIG2") != OK, "both slots are gone");
    CHECK(last_journal(&e) == RWY_OK && e.event == RWY_JOURNAL_EVENT_EVICT && !strcmp(e.slot, "CONFIG2"), "EVICT recorded");
    blocks = e.reserved[RWY_EVICT_BLOCKS_OFF] | e.reserved[RWY_EVICT_BLOCKS_OFF + 1] << 8;
    CHECK(e.reserved[RWY_EVICT_STATE_OFF] == RWY_SS_OK && e.reserved[RWY_EVICT_REASON_OFF] == RWY_EVICT_REASON_SLOTS &&
          blocks == pl.slot_freed[1], "state, reason and blocks are kept");
}

/* R2: SLOTS holds the number of slots the owner wants (1..64). */
void test_slots_scalar(void) {
    printf("== SLOTS scalar ==\n");
    fresh_subtree();
    EXPECT(cr(T "/SLOTS"), OK, "USER creates SLOTS");
    EXPECT(wr(T "/SLOTS", 0, "2\n"), OK, "SLOTS 2");
    EXPECT(wr(T "/SLOTS", 0, "64"), OK, "SLOTS 64");
    EXPECT(wr(T "/SLOTS", 0, "1"), OK, "SLOTS 1");
    EXPECT(wr(T "/SLOTS", 0, "0"), INV, "SLOTS 0");
    EXPECT(wr(T "/SLOTS", 0, "65"), INV, "SLOTS 65");
    EXPECT(wr(T "/SLOTS", 0, "x"), INV, "SLOTS text");
    EXPECT(wr(T "/SLOTS", 0, "-1"), INV, "SLOTS -1");
    EXPECT(wr(T "/SLOTS", 0, "18446744073709551616"), INV, "SLOTS overflow");
    EXPECT(wr(T "/SLOTS", 1, "3"), INV, "scalar write must start at 0");
    same_text(T "/SLOTS", "1", "SLOTS keeps the last good value");
    EXPECT(tr(T "/SLOTS", 0), OK, "truncate(0) is a no-op success");
    same_text(T "/SLOTS", "1", "SLOTS survives truncate(0)");
    as_sys();
    EXPECT(wr(T "/SLOTS", 0, "5\n"), OK, "SYSTEM writes SLOTS");
    EXPECT(wr(T "/SLOTS", 0, "70"), INV, "SYSTEM is held to the range too");
    as_user();
    EXPECT(mv(T "/SLOTS", T "/CONFIG1"), INV, "SLOTS cannot become a slot");
    EXPECT(rm(T "/SLOTS"), OK, "SLOTS can be removed (the feature is optional)");
    priv(1); EXPECT(cr(T "/SLOTS"), OK, "fixture"); EXPECT(wr(T "/SLOTS", 0, "99\n"), OK, "bad value by bypass"); priv(0);
}

/* BD1: BOOT-DELAY holds a pause in seconds (0..30) before the slot starts; global only. */
void test_boot_delay(void) {
    rwy_boot_choice_t c;
    printf("== BOOT-DELAY scalar ==\n");
    fresh_subtree();
    EXPECT(cr(T "/BOOT-DELAY"), OK, "USER creates BOOT-DELAY");
    EXPECT(wr(T "/BOOT-DELAY", 0, "5\n"), OK, "BOOT-DELAY 5");
    EXPECT(wr(T "/BOOT-DELAY", 0, "30"), OK, "BOOT-DELAY 30");
    EXPECT(wr(T "/BOOT-DELAY", 0, "0"), OK, "BOOT-DELAY 0");
    EXPECT(wr(T "/BOOT-DELAY", 0, "31"), INV, "BOOT-DELAY 31");
    EXPECT(wr(T "/BOOT-DELAY", 0, "x"), INV, "BOOT-DELAY text");
    EXPECT(wr(T "/BOOT-DELAY", 0, "-1"), INV, "BOOT-DELAY -1");
    EXPECT(wr(T "/BOOT-DELAY", 0, "18446744073709551616"), INV, "BOOT-DELAY overflow");
    EXPECT(wr(T "/BOOT-DELAY", 1, "3"), INV, "scalar write must start at 0");
    same_text(T "/BOOT-DELAY", "0", "BOOT-DELAY keeps the last good value");
    as_sys();
    EXPECT(wr(T "/BOOT-DELAY", 0, "7\n"), OK, "SYSTEM writes BOOT-DELAY");
    EXPECT(wr(T "/BOOT-DELAY", 0, "99"), INV, "SYSTEM is held to the range too");
    as_user();
    EXPECT(mv(T "/BOOT-DELAY", T "/CONFIG1"), INV, "BOOT-DELAY cannot become a slot");

    /* Selection carries it, for a slot and for RECOVERY; a bad value is ignored. */
    EXPECT(rm(T "/BOOT-DELAY"), OK, "fixture");
    fin(1, "OK");
    EXPECT(sel(&c), OK, "no BOOT-DELAY");
    CHECK(c.boot_delay == 0, "absent: no pause");
    seed(T "/BOOT-DELAY", "4\n");
    EXPECT(sel(&c), OK, "BOOT-DELAY 4");
    CHECK(c.boot_delay == 4, "carried");
    mkrecovery();
    set_state(1, "FAILED\n");
    EXPECT(sel(&c), OK, "RECOVERY");
    CHECK(!strcmp(c.slot, "RECOVERY") && c.boot_delay == 4, "applies to RECOVERY too");
    set_state(1, "OK\n");
    priv(1); EXPECT(wr(T "/BOOT-DELAY", 0, "99\n"), OK, "bad value by bypass"); priv(0);
    EXPECT(sel(&c), OK, "bad value");
    CHECK(c.booted && c.boot_delay == 0, "a value above 30 is ignored");
    priv(1); EXPECT(wr(T "/BOOT-DELAY", 0, "soon\n"), OK, "bad text by bypass"); priv(0);
    EXPECT(sel(&c), OK, "bad text");
    CHECK(c.booted && c.boot_delay == 0, "unparsable is ignored");
    EXPECT(rm(T "/BOOT-DELAY"), OK, "BOOT-DELAY can be removed (the feature is optional)");
    priv(1); EXPECT(cr(T "/BOOT-DELAY"), OK, "fixture"); EXPECT(wr(T "/BOOT-DELAY", 0, "99\n"), OK, "bad value by bypass"); priv(0);
}

/* QE2: QUOTA holds the share of the usable pool the slots may use (10..95); global only, optional. */
static int quota_of(unsigned *pct) {
    int rc;
    rwy_begin(&S);
    rc = rwy_bootcfg_quota(&S, pct);
    rwy_abort(&S);
    return rc;
}

void test_quota_scalar(void) {
    unsigned pct = 0;
    printf("== QUOTA scalar ==\n");
    fresh_subtree();
    EXPECT(quota_of(&pct), RWY_ERR_NOENT, "absent: eviction is off");
    EXPECT(cr(T "/QUOTA"), OK, "USER creates QUOTA");
    EXPECT(wr(T "/QUOTA", 0, "80\n"), OK, "QUOTA 80");
    EXPECT(wr(T "/QUOTA", 0, "10"), OK, "QUOTA 10");
    EXPECT(wr(T "/QUOTA", 0, "95"), OK, "QUOTA 95");
    EXPECT(wr(T "/QUOTA", 0, "9"), INV, "QUOTA 9");
    EXPECT(wr(T "/QUOTA", 0, "96"), INV, "QUOTA 96");
    EXPECT(wr(T "/QUOTA", 0, "x"), INV, "QUOTA text");
    EXPECT(wr(T "/QUOTA", 0, "-1"), INV, "QUOTA -1");
    EXPECT(wr(T "/QUOTA", 0, "18446744073709551616"), INV, "QUOTA overflow");
    EXPECT(wr(T "/QUOTA", 1, "3"), INV, "scalar write must start at 0");
    same_text(T "/QUOTA", "95", "QUOTA keeps the last good value");
    EXPECT(quota_of(&pct), OK, "QUOTA 95 is read");
    CHECK(pct == 95, "the percentage");
    as_sys();
    EXPECT(wr(T "/QUOTA", 0, "60\n"), OK, "SYSTEM writes QUOTA");
    EXPECT(wr(T "/QUOTA", 0, "99"), INV, "SYSTEM is held to the range too");
    as_user();
    EXPECT(mv(T "/QUOTA", T "/CONFIG1"), INV, "QUOTA cannot become a slot");

    /* A bad value in an image turns eviction off, and the checker reports it. */
    priv(1); EXPECT(wr(T "/QUOTA", 0, "99\n"), OK, "bad value by bypass"); priv(0);
    EXPECT(quota_of(&pct), RWY_ERR_NOENT, "a value above 95 is ignored");
    priv(1); EXPECT(wr(T "/QUOTA", 0, "most\n"), OK, "bad text by bypass"); priv(0);
    EXPECT(quota_of(&pct), RWY_ERR_NOENT, "unparsable is ignored");
}

/* LOOP-GUARD, LOOP-LIMIT and LOOP-WINDOW: global scalars; the guard is off unless LOOP-GUARD says "on". */
static int guard_of(rwy_loop_guard_t *g) {
    int rc;
    rwy_begin(&S);
    rc = rwy_bootcfg_loop_guard(&S, g);
    rwy_abort(&S);
    return rc;
}

void test_loop_guard(void) {
    rwy_loop_guard_t g;
    printf("== LOOP-GUARD scalars ==\n");
    fresh_subtree();
    EXPECT(guard_of(&g), RWY_ERR_NOENT, "absent: guard is off");
    EXPECT(cr(T "/LOOP-GUARD"), OK, "USER creates LOOP-GUARD");
    EXPECT(wr(T "/LOOP-GUARD", 0, "off\n"), OK, "LOOP-GUARD off");
    EXPECT(guard_of(&g), RWY_ERR_NOENT, "off: guard is off");
    EXPECT(wr(T "/LOOP-GUARD", 0, "on"), OK, "LOOP-GUARD on");
    EXPECT(wr(T "/LOOP-GUARD", 0, "ON"), INV, "LOOP-GUARD ON");
    EXPECT(wr(T "/LOOP-GUARD", 0, "1"), INV, "LOOP-GUARD 1");
    EXPECT(wr(T "/LOOP-GUARD", 0, "onn"), INV, "LOOP-GUARD onn");
    EXPECT(wr(T "/LOOP-GUARD", 1, "on"), INV, "scalar write must start at 0");
    same_text(T "/LOOP-GUARD", "on", "LOOP-GUARD keeps the last good value");
    EXPECT(guard_of(&g), OK, "on without limit or window");
    CHECK(g.limit == 5 && g.window == 600, "defaults 5 and 600");

    EXPECT(cr(T "/LOOP-LIMIT"), OK, "USER creates LOOP-LIMIT");
    EXPECT(wr(T "/LOOP-LIMIT", 0, "2"), OK, "LOOP-LIMIT 2");
    EXPECT(wr(T "/LOOP-LIMIT", 0, "255\n"), OK, "LOOP-LIMIT 255");
    EXPECT(wr(T "/LOOP-LIMIT", 0, "1"), INV, "LOOP-LIMIT 1");
    EXPECT(wr(T "/LOOP-LIMIT", 0, "256"), INV, "LOOP-LIMIT 256");
    EXPECT(wr(T "/LOOP-LIMIT", 0, "x"), INV, "LOOP-LIMIT text");
    EXPECT(wr(T "/LOOP-LIMIT", 0, "18446744073709551616"), INV, "LOOP-LIMIT overflow");
    EXPECT(cr(T "/LOOP-WINDOW"), OK, "USER creates LOOP-WINDOW");
    EXPECT(wr(T "/LOOP-WINDOW", 0, "30"), OK, "LOOP-WINDOW 30");
    EXPECT(wr(T "/LOOP-WINDOW", 0, "86400\n"), OK, "LOOP-WINDOW 86400");
    EXPECT(wr(T "/LOOP-WINDOW", 0, "29"), INV, "LOOP-WINDOW 29");
    EXPECT(wr(T "/LOOP-WINDOW", 0, "86401"), INV, "LOOP-WINDOW 86401");
    EXPECT(wr(T "/LOOP-WINDOW", 0, "-1"), INV, "LOOP-WINDOW -1");
    EXPECT(guard_of(&g), OK, "on with both");
    CHECK(g.limit == 255 && g.window == 86400, "the values are read");
    as_sys();
    EXPECT(wr(T "/LOOP-LIMIT", 0, "3"), OK, "SYSTEM writes LOOP-LIMIT");
    EXPECT(wr(T "/LOOP-LIMIT", 0, "1"), INV, "SYSTEM is held to the range too");
    as_user();
    EXPECT(mv(T "/LOOP-GUARD", T "/CONFIG1"), INV, "LOOP-GUARD cannot become a slot");

    /* Bad values in an image: the guard stays on with the defaults, or goes off. */
    priv(1); EXPECT(wr(T "/LOOP-LIMIT", 0, "1\n"), OK, "bad limit by bypass");
    EXPECT(wr(T "/LOOP-WINDOW", 0, "soon\n"), OK, "bad window by bypass"); priv(0);
    EXPECT(guard_of(&g), OK, "bad limit and window do not turn the guard off");
    CHECK(g.limit == 5 && g.window == 600, "...they fall back to the defaults");
    priv(1); EXPECT(wr(T "/LOOP-GUARD", 0, "yes\n"), OK, "bad guard by bypass"); priv(0);
    EXPECT(guard_of(&g), RWY_ERR_NOENT, "an unknown LOOP-GUARD means off");
}

/* T2: a chosen slot that cannot be started is condemned (FAILED) and selection runs again. */

void test_condemn(void) {
    rwy_boot_choice_t c;
    uint64_t g, q;
    printf("== condemn a slot that cannot start ==\n");
    fresh_subtree();
    fin(1, "OK");
    fin(2, "INSTALLED");
    EXPECT(sel(&c), OK, "select");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG2") && text_is(T "/CONFIG2/STATE", "TESTING\n"), "CONFIG2 chosen, TESTING");
    EXPECT(rwy_bootcfg_condemn(&S, "CONFIG2"), PERM, "USER actor refused");
    EXPECT(condemn("RECOVERY"), INV, "RECOVERY is not a slot");
    EXPECT(condemn("CONFIG9"), RWY_ERR_NOENT, "unknown slot");
    g = gen(); q = rseq();
    EXPECT(condemn("CONFIG2"), OK, "condemn");
    CHECK(rseq() == q + 1 && gen() == g && text_is(T "/CONFIG2/STATE", "FAILED\n"), "one record write, FAILED");
    CHECK(text_is(T "/CONFIG2/CAUSE", "load_error\n"), "cause load_error");
    char flcond[4096];
    CHECK(read_all(T "/FLIGHT", flcond, sizeof(flcond)) > 0, "FLIGHT readable after condemn");
    CHECK(strstr(flcond, "event=CONDEMN slot=CONFIG2") != NULL, "FLIGHT records condemn");
    EXPECT(wr(T "/CONFIG2/CAUSE", 0, "x\n"), PERM, "CAUSE is read only");
    EXPECT(sel(&c), OK, "select again");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG1") && booted_is("CONFIG1") && text_is(T "/REASON", "normal\n"),
          "falls back to CONFIG1");
    EXPECT(condemn("CONFIG1"), OK, "condemn the last slot");
    EXPECT(sel(&c), OK, "select with nothing left");
    CHECK(c.engaged && !c.booted, "nothing bootable");
}

void test_select_label(void) {
    rwy_boot_choice_t c;
    sel_t x;
    char long_lbl[129], exp_sfx[130];

    printf("== selection label in boot messages ==\n");

    /* 1. Booting line with and without label */
    fresh_subtree();
    fin(1, "OK");
    EXPECT(sel(&c), OK, "select OK unlabelled");
    CHECK(has("RUNWAY: booting CONFIG1 (rev 1, OK)\n"), "unlabelled boot line has no suffix");

    set_file(1, "LABEL", "alpha-v1.0\n");
    EXPECT(sel(&c), OK, "select OK labelled");
    CHECK(has("RUNWAY: booting CONFIG1 (rev 1, OK) \"alpha-v1.0\"\n"), "labelled boot line has suffix");

    fin(2, "INSTALLED");
    set_file(2, "LABEL", "slot2-test\n");
    EXPECT(sel(&c), OK, "select INSTALLED labelled");
    CHECK(has("RUNWAY: booting CONFIG2 (rev 2, TESTING, 0 tries left) \"slot2-test\"\n"), "TESTING slot carries label");

    /* 2. Skipped line with and without label */
    EXPECT(sel(&c), OK, "second boot: CONFIG2 FAILED");
    CHECK(has("RUNWAY: CONFIG2 FAILED, skipped (tries_used_up) \"slot2-test\"\n"), "skipped slot carries label");

    EXPECT(rm(T "/CONFIG2/LABEL"), OK, "remove label");
    EXPECT(sel(&c), OK, "select with unlabelled FAILED slot");
    CHECK(has("RUNWAY: CONFIG2 FAILED, skipped (tries_used_up)\n"), "unlabelled skipped slot has no suffix");

    /* 3. RECOVERY boot lines with and without label */
    mkrecovery();
    set_state(1, "FAILED\n");
    EXPECT(sel(&c), OK, "all failed, recovery without label");
    CHECK(has("RUNWAY: no bootable config (all_failed), booting RECOVERY\n"), "unlabelled all_failed recovery");

    unlock(1); as_sys();
    CHECK(cr(T "/RECOVERY/LABEL") == OK && wr(T "/RECOVERY/LABEL", 0, "rescue-sys\n") == OK, "recovery label");
    as_user(); unlock(0);
    EXPECT(sel(&c), OK, "all failed, recovery with label");
    CHECK(has("RUNWAY: no bootable config (all_failed), booting RECOVERY \"rescue-sys\"\n"), "labelled all_failed recovery");

    EXPECT(cr(T "/RECOVER_NEXT"), OK, "recover_next");
    EXPECT(sel(&c), OK, "recovery requested with label");
    CHECK(has("RUNWAY: recovery requested, booting RECOVERY \"rescue-sys\"\n"), "labelled recovery requested");

    unlock(1); as_sys();
    EXPECT(rm(T "/RECOVERY/LABEL"), OK, "remove recovery label");
    as_user(); unlock(0);
    EXPECT(cr(T "/RECOVER_NEXT"), OK, "recover_next unlabelled");
    EXPECT(sel(&c), OK, "recovery requested unlabelled");
    CHECK(has("RUNWAY: recovery requested, booting RECOVERY\n"), "unlabelled recovery requested");

    /* 4. Condemn line */
    fin(3, "INSTALLED");
    set_file(3, "LABEL", "condemn-me\n");
    EXPECT(sel(&c), OK, "select INSTALLED");
    EXPECT(condemn("CONFIG3"), OK, "condemn slot with label");
    EXPECT(sel(&c), OK, "select after condemn");
    CHECK(has("RUNWAY: CONFIG3 FAILED, skipped (load_error) \"condemn-me\"\n"), "condemned slot skipped with label");

    /* 5. rwy_bootcfg_failed listing with and without label */
    logbuf[0] = 0;
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    CHECK(rwy_bootcfg_failed(&S, logfn, NULL) >= 1, "failed slots exist");
    rwy_set_actor(&S, RWY_ACTOR_USER);
    CHECK(has("CONFIG3 FAILED (load_error) \"condemn-me\"\n"), "failed listing with label");

    EXPECT(rm(T "/CONFIG3/LABEL"), OK, "remove label from failed slot");
    logbuf[0] = 0;
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    CHECK(rwy_bootcfg_failed(&S, logfn, NULL) >= 1, "failed slots exist");
    rwy_set_actor(&S, RWY_ACTOR_USER);
    CHECK(has("CONFIG3 FAILED (load_error)\n"), "failed listing unlabelled");

    /* 6. Truncation boundary tests on bc_cat_label_str */
    memset(&x, 0, sizeof(x));
    bc_cat_label_str(&x, NULL);
    CHECK(x.n == 0, "NULL label does not append");
    bc_cat_label_str(&x, "");
    CHECK(x.n == 0, "empty label does not append");

    memset(&x, 0, sizeof(x));
    x.n = 153;
    bc_cat_label_str(&x, "abcdef");
    CHECK(x.n == 153, "rem < 7 omits suffix");

    memset(&x, 0, sizeof(x));
    x.n = 152;
    bc_cat_label_str(&x, "abcdef");
    CHECK(x.n == 159 && !memcmp(x.b + 152, " \"a...\"", 7), "rem == 7 emits minimum truncated suffix");

    memset(&x, 0, sizeof(x));
    x.n = 151;
    bc_cat_label_str(&x, "abcdef");
    CHECK(x.n == 159 && !memcmp(x.b + 151, " \"ab...\"", 8), "rem == 8 emits 2 label chars with ellipsis");

    memset(&x, 0, sizeof(x));
    x.n = 150;
    bc_cat_label_str(&x, "abcdef");
    CHECK(x.n == 159 && !memcmp(x.b + 150, " \"abcdef\"", 9), "rem == len + 3 emits exact full suffix");

    memset(&x, 0, sizeof(x));
    x.n = 140;
    bc_cat_label_str(&x, "abcdef");
    CHECK(x.n == 149 && !memcmp(x.b + 140, " \"abcdef\"", 9), "rem > len + 3 emits full suffix");

    /* 7. Truncation in end-to-end selection with 128-byte label */
    memset(long_lbl, 'A', 128);
    long_lbl[128] = 0;
    fin(4, "OK");
    set_file(4, "LABEL", long_lbl);
    EXPECT(sel(&c), OK, "select with 128-byte label");
    /* "RUNWAY: booting CONFIG4 (rev 4, OK)" is 35 bytes.
     * rem = 159 - 35 = 124 bytes.
     * keep = 124 - 6 = 118 bytes of 'A', followed by "...\"" */
    memcpy(exp_sfx, " \"", 2);
    memset(exp_sfx + 2, 'A', 118);
    memcpy(exp_sfx + 120, "...\"\n", 5);
    exp_sfx[125] = 0;
    CHECK(has(exp_sfx), "128-byte label truncated with ellipsis in selection");
}

void test_select_entry(void) {
    rwy_boot_choice_t c, p;
    printf("== selection entry name ==\n");
    fresh_subtree();

    /* Slot with only KERNEL gets default .efi name */
    fin(1, "OK");
    EXPECT(sel(&c), OK, "select CONFIG1");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG1"), "CONFIG1 booted");
    CHECK(!strcmp(c.entry, "runway-1.efi"), "CONFIG1 only KERNEL gets runway-1.efi default");

    /* Slot with extra file (ARGS) gets default .conf name */
    fin(2, "INSTALLED");
    set_file(2, "ARGS", "console=ttyS0\n");
    EXPECT(sel(&c), OK, "select CONFIG2");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG2"), "CONFIG2 booted");
    CHECK(!strcmp(c.entry, "runway-2.conf"), "CONFIG2 with ARGS gets runway-2.conf default");

    /* Slot with valid ENTRY file */
    fin(3, "INSTALLED");
    set_file(3, "ENTRY", "custom-kernel-1.0.efi\n");

    /* Preview carries entry name before select */
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    EXPECT(rwy_bootcfg_preview(&S, &p), OK, "preview");
    rwy_set_actor(&S, RWY_ACTOR_USER);
    CHECK(!strcmp(p.entry, "custom-kernel-1.0.efi"), "preview carries entry name");

    EXPECT(sel(&c), OK, "select CONFIG3");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG3"), "CONFIG3 booted");
    CHECK(!strcmp(c.entry, "custom-kernel-1.0.efi"), "CONFIG3 reads ENTRY verbatim");

    /* Slot with invalid ENTRY (stem has counter) falls back to default name */
    fin(4, "INSTALLED");
    set_file(4, "ENTRY", "invalid+1.conf\n");
    EXPECT(sel(&c), OK, "select CONFIG4");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG4"), "CONFIG4 booted");
    CHECK(!strcmp(c.entry, "runway-4.efi"), "CONFIG4 invalid ENTRY falls back to default runway-4.efi");

    /* Remove invalid ENTRY so fsck.runway clean pass */
    EXPECT(rm(T "/CONFIG4/ENTRY"), OK, "remove invalid ENTRY");

    /* Fallback carries entry name */
    EXPECT(fb(&c, NULL), OK, "fallback");
    CHECK(!strcmp(c.entry, "runway-1.efi"), "fallback carries OK slot entry name");
}

static void collect_entry(void *ctx, const char *entry) {
    char *list = ctx;
    strcat(list, entry);
    strcat(list, " ");
}

void test_entries_list(void) {
    char list[256] = "";
    printf("== entry list ==\n");
    fresh_subtree();

    fin(1, "OK");
    fin(2, "INSTALLED");
    set_file(2, "ARGS", "console=ttyS0\n");
    fin(3, "FAILED");
    set_file(3, "ENTRY", "custom-kernel-1.0.efi\n");
    EXPECT(rwy_bootcfg_entries(&S, collect_entry, list), INV, "needs an open transaction");
    EXPECT(rwy_begin(&S), OK, "begin");
    EXPECT(rwy_bootcfg_entries(&S, collect_entry, list), OK, "list");
    EXPECT(rwy_abort(&S), OK, "abort");
    CHECK(!strcmp(list, "runway-1.efi runway-2.conf custom-kernel-1.0.efi "), "one name per slot, ENTRY or default");
}

