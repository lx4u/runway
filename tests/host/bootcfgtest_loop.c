/* The boot loop guard in selection: demotion, fallback and the cases where nothing is demoted. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "bootcfgtest.h"
#include "rwy_bootcfg.h"

typedef struct {
    uint64_t hit;        /* revision that reaches the limit, 0 for none */
    uint64_t asked[8];
    unsigned calls;
} guard_t;

static int guard_cb(void *ctx, uint64_t rev, const rwy_loop_guard_t *g) {
    guard_t *t = ctx;
    CHECK(g->limit == 5 && g->window == 600, "the callback gets the settings");
    if (t->calls < 8) t->asked[t->calls] = rev;
    t->calls++;
    return rev == t->hit;
}

static int sel_guarded(rwy_boot_choice_t *c, guard_t *t) {
    int rc;
    logbuf[0] = 0;
    t->calls = 0;
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    rc = rwy_bootcfg_select_guarded(&S, c, logfn, NULL, guard_cb, t);
    rwy_set_actor(&S, RWY_ACTOR_USER);
    return rc;
}

static uint64_t rev_of(int n) {
    char p[64];
    snprintf(p, sizeof(p), T "/CONFIG%d/REVISION", n);
    return (uint64_t)num(p);
}

static void guard_on(void) {
    EXPECT(cr(T "/LOOP-GUARD"), OK, "USER creates LOOP-GUARD");
    EXPECT(wr(T "/LOOP-GUARD", 0, "on"), OK, "LOOP-GUARD on");
}

static void flight_has(const char *what, const char *msg) {
    char fl[4096];
    CHECK(read_all(T "/FLIGHT", fl, sizeof(fl)) > 0 && strstr(fl, what) != NULL, msg);
}

void test_loop_demote_to_older(void) {
    printf("== boot loop: demote to older ==\n");
    rwy_boot_choice_t c;
    guard_t t = { 0 };
    uint64_t q;

    fresh_subtree();
    fin(1, "OK");
    fin(2, "OK");
    EXPECT(sel_guarded(&c, &t), OK, "guard off: select");
    CHECK(!t.calls && !strcmp(c.slot, "CONFIG2"), "LOOP-GUARD absent: the callback is not asked");
    guard_on();
    EXPECT(sel_guarded(&c, &t), OK, "guard on, no loop");
    CHECK(t.calls == 1 && t.asked[0] == rev_of(2) && !strcmp(c.slot, "CONFIG2") && !c.loop_slot[0], "asked once for the newest OK slot");
    CHECK(text_is(T "/CONFIG2/STATE", "OK\n") && text_is(T "/REASON", "normal\n"), "nothing demoted");

    t.hit = rev_of(2);
    q = rseq();
    EXPECT(sel_guarded(&c, &t), OK, "CONFIG2 reaches the limit");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG1") && !strcmp(c.loop_slot, "CONFIG2"), "CONFIG1 boots, CONFIG2 is named");
    CHECK(text_is(T "/CONFIG2/STATE", "FAILED\n") && text_is(T "/CONFIG2/CAUSE", "boot_loop\n"), "CONFIG2 FAILED, cause boot_loop");
    CHECK(booted_is("CONFIG1") && text_is(T "/REASON", "boot_loop\n") && rseq() == q + 1, "REASON boot_loop in one record write");
    CHECK(has("CONFIG2 FAILED, boot loop (more than 5 entries in 600 s), skipped"), "the log says why");
    CHECK(t.calls == 2 && t.asked[1] == rev_of(1), "the older slot is counted on its own");
    flight_has("event=BOOT slot=CONFIG1 reason=boot_loop", "the flight recorder has the fallback boot");

    EXPECT(sel_guarded(&c, &t), OK, "next boot");
    CHECK(!strcmp(c.slot, "CONFIG1") && !c.loop_slot[0] && text_is(T "/REASON", "normal\n"), "FAILED stays skipped, reason back to normal");
    set_state(2, "OK\n");
    EXPECT(sel_guarded(&c, &t), OK, "SYSTEM puts it back");
    CHECK(!strcmp(c.loop_slot, "CONFIG2"), "back to OK, it is demoted again at the limit");
}

void test_loop_demote_to_recovery(void) {
    printf("== boot loop: demote to recovery ==\n");
    rwy_boot_choice_t c;
    guard_t t = { 0 };

    fresh_subtree();
    fin(1, "OK");
    mkrecovery();
    guard_on();
    t.hit = rev_of(1);
    EXPECT(sel_guarded(&c, &t), OK, "the only OK slot reaches the limit");
    CHECK(c.booted && !strcmp(c.slot, "RECOVERY") && !strcmp(c.loop_slot, "CONFIG1"), "RECOVERY boots");
    CHECK(text_is(T "/CONFIG1/STATE", "FAILED\n") && text_is(T "/REASON", "boot_loop\n") && booted_is("RECOVERY"), "FAILED, reason boot_loop");
    CHECK(has("no OK slot left after a boot loop, booting RECOVERY"), "the log says why");
}

void test_loop_no_other_way(void) {
    printf("== boot loop: no other way ==\n");
    rwy_boot_choice_t c;
    guard_t t = { 0 };
    uint64_t q;

    fresh_subtree();
    fin(1, "OK");
    fin(2, "FAILED");
    guard_on();
    t.hit = rev_of(1);
    q = rseq();
    EXPECT(sel_guarded(&c, &t), OK, "no lower OK slot, no RECOVERY");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG1") && !c.loop_slot[0], "the slot boots as is");
    CHECK(text_is(T "/CONFIG1/STATE", "OK\n") && text_is(T "/REASON", "normal\n") && rseq() == q + 1, "not demoted");
    CHECK(has("CONFIG1 reached the boot loop limit (more than 5 entries in 600 s), no other slot, booting it"), "the log says so");
    flight_has("event=LOOP_LIMIT slot=CONFIG1", "the flight recorder says so");
}

void test_loop_not_for_trial_or_preview(void) {
    printf("== boot loop: not for trial or preview ==\n");
    rwy_boot_choice_t c;
    guard_t t = { 0 };
    uint64_t g;

    fresh_subtree();
    fin(1, "OK");
    fin(2, "INSTALLED");
    guard_on();
    t.hit = rev_of(1);
    EXPECT(sel_guarded(&c, &t), OK, "a slot under test");
    CHECK(!t.calls && !strcmp(c.slot, "CONFIG2") && c.trial, "TESTING slots are not guarded");
    set_state(2, "OK\n");
    g = gen();
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    EXPECT(rwy_bootcfg_preview(&S, &c), OK, "preview");
    rwy_set_actor(&S, RWY_ACTOR_USER);
    CHECK(!strcmp(c.slot, "CONFIG2") && !c.loop_slot[0] && gen() == g, "preview never asks the guard");
}
