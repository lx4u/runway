/* The fsck.runway invariants and repair, the damaged record and the slot state. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "bootcfgtest.h"

#define CK      tpath("rwy_bootcfg_ck.img")
#define CK_BASE tpath("rwy_bootcfg_ck_base.img")

static int ck_unchanged(void) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cmp -s %s %s", CK, CK_BASE);
    return system(cmd);
}

/* Slots 1 (OK) and 2 (INSTALLED) finalized, 3 incomplete, a RECOVERY and a blob; counter = 2. */
static void ck_store(const char *base, void (*tweak)(void)) {
    file_t fl;
    rwy_block_dev_t dev;
    copy_file(base, CK_BASE);
    open_store(&S, &dev, &fl, CK_BASE);
    fresh_subtree();
    fin(1, "OK");
    fin(2, "INSTALLED");
    slot(3);
    set_file(3, "TRIES", "junk\n");                    /* values of an incomplete slot are not audited */
    CHECK(cr(T "/BLOBS/b") == OK && wr(T "/BLOBS/b", 0, "blob") == OK, "blob");
    mkrecovery();
    unlock(1); as_sys();
    CHECK(mk(T "/WDT") == OK && cr(T "/WDT/I6300ESB.EFI") == OK && wr(T "/WDT/I6300ESB.EFI", 0, "app") == OK, "WDT fixture");
    as_user(); unlock(0);
    if (tweak) tweak();
    fclose(fl.f);
    copy_file(CK_BASE, CK);
}

static long ck_counter(const char *img) {          /* also proves the image still mounts */
    file_t fl;
    rwy_block_dev_t dev;
    long c;
    open_store(&S, &dev, &fl, img);
    c = counter();
    fclose(fl.f);
    return c;
}

static void tw_behind(void)    { set_counter("1\n"); }
static void tw_missing(void)   { set_counter(""); }
static void tw_unparsable(void){ set_counter("abc\n"); }
static void tw_ahead(void)     { set_counter("99\n"); }
static void tw_revision(void)  { set_file(1, "REVISION", "junk\n"); }
static void tw_nokernel(void)  { rm(T "/CONFIG1/KERNEL"); }
static void tw_dangling(void)  { rm(T "/CONFIG1/KERNEL"); priv(1); sy(T "/CONFIG1/KERNEL", "../nowhere"); priv(0); }
static void tw_booted(void)    { set_booted("elsewhere"); }
static void tw_watchdog(void)  { cr(T "/WATCHDOG"); priv(1); wr(T "/WATCHDOG", 0, "soon\n"); priv(0); }
static void tw_norecovery(void){ unlock(1); as_sys(); rm(T "/RECOVERY/KERNEL"); as_user(); unlock(0); }
static void tw_wdtbig(void) {   /* a size the policy refuses, written the way a bypass would */
    unlock(1); as_sys();
    cr(T "/WDT/BIG.EFI");
    priv(1); wr(T "/WDT/BIG.EFI", 1024 * 1024, "x"); priv(0);
    as_user(); unlock(0);
}
static void tw_labellarge(void) {
    priv(1);
    cr(T "/CONFIG1/LABEL");
    wr(T "/CONFIG1/LABEL", 128, "x");
    wr(T "/CONFIG1/LABEL", 129, "y");
    priv(0);
}
static void tw_labelbad(void) {
    priv(1);
    cr(T "/CONFIG1/LABEL");
    wr(T "/CONFIG1/LABEL", 0, "bad\"quote\n");
    priv(0);
}
static void tw_hint(void)      { CHECK(rwy_bootcfg_wdt_set_hint(&S, "GONE.EFI") == RWY_OK, "record a hint"); }
static void tw_both(void)      { tw_behind(); tw_booted(); }

void test_check_repair(const char *base) {
    static const struct { const char *what; void (*tw)(void); const char *line; } viol[] = {
        { "unparsable REVISION", tw_revision, "CONFIG1: REVISION invalid" },
        { "KERNEL missing", tw_nokernel, "CONFIG1: KERNEL missing" },
        { "dangling KERNEL", tw_dangling, "CONFIG1: KERNEL invalid" },
        { "BOOTED names nothing", tw_booted, "BOOTED: does not name" },
        { "WATCHDOG not decimal", tw_watchdog, "WATCHDOG: not a decimal number" },
        { "RECOVERY without KERNEL", tw_norecovery, "RECOVERY: KERNEL missing" },
        { "WDT application over 1 MiB", tw_wdtbig, "WDT/BIG.EFI: larger than 1 MiB" },
        { "LABEL over 128 bytes", tw_labellarge, "CONFIG1/LABEL: larger than 128 bytes" },
        { "LABEL invalid characters", tw_labelbad, "CONFIG1/LABEL: invalid characters" },
    };
    static const struct { const char *what; void (*tw)(void); } cnt[] = {
        { "behind", tw_behind }, { "missing", tw_missing }, { "unparsable", tw_unparsable },
    };
    printf("== fsck.runway: EFI/RUNWAY invariants ==\n");
    ck_store(base, NULL);
    CHECK(run_checker_args("", CK, "") == 0 && chk_has("efi/runway: 3 slots, 1 incomplete"), "valid subtree passes");
    CHECK(run_checker_args("--repair", CK, "") == 0 && ck_counter(CK) == 2, "repair of a valid subtree changes nothing");
    CHECK(run_checker_args("", CK, "") == 0, "still clean after repair");
    {   /* The incomplete slot is reported and kept, never deleted. */
        file_t fl;
        rwy_block_dev_t dev;
        open_store(&S, &dev, &fl, CK);
        CHECK(rev(3) == -1 && read_all(T "/CONFIG3/KERNEL", (char[4]){0}, 4) == 1, "incomplete slot kept by repair");
        fclose(fl.f);
    }
    ck_store(base, tw_hint);
    CHECK(run_checker_args("", CK, "") == 0, "a WDT hint naming no file is fine");
    ck_store(base, tw_ahead);
    CHECK(run_checker_args("", CK, "") == 0, "counter ahead of the slots is fine");
    CHECK(run_checker_args("--repair", CK, "") == 0 && ck_counter(CK) == 99, "...and left alone");

    for (unsigned i = 0; i < sizeof(viol) / sizeof(viol[0]); i++) {
        printf("  violation: %s\n", viol[i].what);
        ck_store(base, viol[i].tw);
        CHECK(run_checker_args("", CK, "") == 4 && chk_has(viol[i].line), viol[i].what);
        CHECK(run_checker_args("--repair", CK, "") == 4 && chk_has("repair: REFUSED"), "repair refuses a violation");
        CHECK(ck_unchanged() == 0, "...and leaves the store untouched");
    }

    printf("== fsck.runway --repair: counter ==\n");
    for (unsigned i = 0; i < sizeof(cnt) / sizeof(cnt[0]); i++) {
        printf("  counter %s\n", cnt[i].what);
        ck_store(base, cnt[i].tw);
        CHECK(run_checker_args("", CK, "") == 4 && chk_has("EFI/RUNWAY/REVISION: "), "checker flags the counter");
        CHECK(run_checker_args("--repair", CK, "") == 1, "repair succeeds");
        CHECK(ck_counter(CK) == 2, "counter is the highest slot revision");
        CHECK(run_checker_args("", CK, "") == 0, "clean afterwards");
        CHECK(run_checker_args("--repair", CK, "") == 0 && chk_has("no leftovers, store unchanged"), "second repair is a no-op");
    }
    ck_store(base, tw_both);
    CHECK(run_checker_args("--repair", CK, "") == 4 && chk_has("repair: REFUSED") &&
          ck_unchanged() == 0,
          "a violation next to a bad counter: refused, counter not touched");

    /* Fault injection over every write/flush point of the repair. */
    printf("== fault injection: fsck.runway --repair (counter) ==\n");
    ck_store(base, tw_behind);
    CHECK(run_checker_args("--repair", CK, "RWY_CHECK_FI_VERBOSE=1") == 1, "counted run");
    {
        long pts = 0;
        char b[4096], *q;
        FILE *f = fopen(tpath("rwy_bootcfg_chk.log"), "r");
        size_t n = f ? fread(b, 1, sizeof(b) - 1, f) : 0;
        if (f) fclose(f);
        b[n] = 0;
        if ((q = strstr(b, "repair io points: ")) != NULL) pts = atol(q + 18);
        CHECK(pts > 0, "counted repair persistence points");
        for (long pt = 1; pt <= pts; pt++) {
            char env[64];
            long c;
            copy_file(CK_BASE, CK);
            snprintf(env, sizeof(env), "RWY_CHECK_FI_AT=%ld", pt);
            (void)run_checker_args("--repair", CK, env);
            c = ck_counter(CK);                    /* remounts: the store is valid */
            CHECK(c == 1 || c == 2, "counter is wholly old or wholly fixed");
            CHECK(run_checker_args("", CK, "") == (c == 2 ? 0 : 4), "checker agrees with the counter");
            CHECK(run_checker_args("--repair", CK, "") <= 1 && ck_counter(CK) == 2 && run_checker_args("", CK, "") == 0,
                  "a retry converges");
            if (failures) { printf("  at point %ld of %ld\n", pt, pts); break; }
        }
    }
}

static void tw_stale(void) {                       /* an entry of a revision no slot has */
    uint8_t r[RWY_REC_PAYLOAD];
    rwy_ss_entry_t e = { 1, RWY_SS_FAILED, RWY_SS_CAUSE_NONE, 0 };
    rwy_record_read(&S, r);
    CHECK(rwy_ss_put(r, 77, &e) == OK && rwy_record_write(&S, r) == OK, "stale entry");
}
/* Put the entry of slot 2 in the record: FAILED with `cause`, then patch the raw cause byte when `raw` is set. */
static void tw_entry3(int state, int cause, int raw) {
    uint8_t r[RWY_REC_PAYLOAD];
    rwy_ss_entry_t e = { 1, (uint8_t)state, (uint8_t)cause, 0 };
    uint64_t want = (uint64_t)rev(2);
    rwy_record_read(&S, r);
    CHECK(rwy_ss_put(r, want, &e) == OK, "entry of slot 2");
    for (unsigned i = 0; i < RWY_SS_ENTRIES; i++) {
        uint8_t *p = r + 32 + i * 24;
        uint64_t v = 0;
        for (unsigned k = 8; k--;) v = v << 8 | p[k];
        if (v == want && raw >= 0) p[17] = (uint8_t)raw;
    }
    CHECK(rwy_record_write(&S, r) == OK, "record write");
}
static void tw_failed_load(void)   { tw_entry3(RWY_SS_FAILED, RWY_SS_CAUSE_LOAD, -1); }
static void tw_failed_nocause(void){ tw_entry3(RWY_SS_FAILED, RWY_SS_CAUSE_NONE, -1); }
static void tw_cause_unknown(void) { tw_entry3(RWY_SS_FAILED, RWY_SS_CAUSE_LOAD, 99); }
static void tw_cause_not_failed(void) { tw_entry3(RWY_SS_OK, RWY_SS_CAUSE_NONE, 2); }
static void tw_booted_gone(void) { set_booted("CONFIG9"); }
static void tw_tree_state(void) {                  /* an old image: slot 3 is final and still has STATE in the tree */
    set_file(3, "STATE", "OK\n");
    set_file(3, "REVISION", "3\n");
    set_counter("3\n");
}
static void tw_slots_low(void)   { CHECK(cr(T "/SLOTS") == OK && wr(T "/SLOTS", 0, "1\n") == OK, "SLOTS"); }
static void tw_replayed(void) {                    /* a record that has seen revision 9, a tree that stops at 2 */
    uint8_t r[RWY_REC_PAYLOAD];
    rwy_record_read(&S, r);
    rwy_ss_set_rev_hw(r, 9);
    EXPECT(rwy_record_write(&S, r), OK, "record write");
}
static void tw_enrolled(void)    { EXPECT(rwy_anchor_set(&S, 4, 1), OK, "enroll"); }

/* Flip a byte in both copies of the record of the image CK_BASE, then publish it as CK. */
static void ck_damage_record(void) {
    for (int copy = 0; copy < 2; copy++) {
        FILE *f = fopen(CK_BASE, "r+b");
        long off = (long)(S.sb.rec_start + (uint32_t)copy) * RWY_BLOCK_SIZE + 100;
        int ch;
        if (!f || fseek(f, off, SEEK_SET)) exit(1);
        ch = fgetc(f);
        if (fseek(f, off, SEEK_SET) || fputc(ch ^ 0xff, f) == EOF) exit(1);
        fclose(f);
    }
    copy_file(CK_BASE, CK);
}

/* The audit looks at the record: a damaged one is an error that repair fixes, the rest are warnings. */
void test_check_record(const char *base) {
    printf("== fsck.runway: state record ==\n");
    ck_store(base, NULL);
    ck_damage_record();
    CHECK(run_checker_args("", CK, "") == 4 && chk_has("state record damaged"), "damaged record is an error");
    
    CHECK(run_checker_args("--repair", CK, "") == 1 && chk_has("repair: state record"), "repair writes an empty record");
    CHECK(run_checker_args("", CK, "") == 0 && !chk_has("damaged"), "clean afterwards");
    CHECK(run_checker_args("--repair", CK, "") == 0 && chk_has("no leftovers, store unchanged"), "second repair is a no-op");

    ck_store(base, tw_nokernel);
    ck_damage_record();
    CHECK(run_checker_args("--repair", CK, "") == 4 && chk_has("repair: REFUSED") &&
          ck_unchanged() == 0,
          "a violation next to a damaged record: refused");

    ck_store(base, tw_stale);
    CHECK(run_checker_args("", CK, "") == 0 && chk_has("warn: EFI/RUNWAY: state record entries of removed slots"), "stale entry is a warning");
    CHECK(run_checker_args("--repair", CK, "") == 1 && chk_has("repair: state record"), "repair drops it");
    CHECK(run_checker_args("", CK, "") == 0 && !chk_has("removed slots"), "no warning afterwards");

    ck_store(base, tw_failed_load);
    CHECK(run_checker_args("", CK, "") == 0 && chk_has("info: EFI/RUNWAY: CONFIG2 FAILED (load_error)"), "the cause of a FAILED slot is reported");
    ck_store(base, tw_failed_nocause);
    CHECK(run_checker_args("", CK, "") == 0 && chk_has("info: EFI/RUNWAY: CONFIG2 FAILED (cause unknown)"), "a FAILED slot with no cause is reported too");
    ck_store(base, tw_cause_unknown);
    CHECK(run_checker_args("", CK, "") == 4 && chk_has("EFI/RUNWAY/CONFIG2: record entry has an invalid cause"), "an unknown cause code: error");
    ck_store(base, tw_cause_not_failed);
    CHECK(run_checker_args("", CK, "") == 4 && chk_has("EFI/RUNWAY/CONFIG2: record entry has an invalid cause"), "a cause on a slot that is not FAILED: error");
    CHECK(run_checker_args("--repair", CK, "") == 4 && chk_has("repair: REFUSED"), "repair refuses it");

    ck_store(base, tw_booted_gone);
    CHECK(run_checker_args("", CK, "") == 0 && chk_has("warn: EFI/RUNWAY: BOOTED names a slot that does not exist"), "BOOTED names a missing slot: warning");
    CHECK(run_checker_args("--repair", CK, "") == 0 && chk_has("no leftovers, store unchanged"), "...and repair leaves it");

    ck_store(base, tw_tree_state);
    CHECK(run_checker_args("", CK, "") == 4 && chk_has("CONFIG3/STATE: is in the record"), "tree STATE in a final slot: error");
    CHECK(run_checker_args("--repair", CK, "") == 4 && chk_has("repair: REFUSED"), "repair refuses it");

    ck_store(base, tw_slots_low);
    CHECK(run_checker_args("", CK, "") == 0 && chk_has("warn: EFI/RUNWAY: 3 slots, more than SLOTS 1"), "more slots than SLOTS: warning");

    ck_store(base, tw_replayed);
    CHECK(run_checker_args("", CK, "") == 4 && chk_has("EFI/RUNWAY: the tree is older than the record (REVISION 2, rev_hw 9)"),
          "a tree below rev_hw: error");
    CHECK(run_checker_args("--repair", CK, "") == 4 && chk_has("repair: REFUSED") &&
          ck_unchanged() == 0,
          "repair refuses it and leaves the store untouched");

    ck_store(base, tw_enrolled);
    CHECK(run_checker_args("", CK, "") == 0 && chk_has("warn: EFI/RUNWAY: enrolled in the anchor, counter not checked (no --tpm)"),
          "enrolled, no TPM given: warning");
}

/* Both record copies damaged: the store mounts with an empty record, selection says so and starts the slots again. */
void test_record_damaged(const char *base) {
    const char *img = tpath("rwy_test_bootcfg_dmg.img");
    file_t fl;
    rwy_block_dev_t dev;
    rwy_boot_choice_t c;
    uint32_t rec;
    printf("== damaged state record ==\n");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    fresh_subtree();
    fin(1, "OK");
    fin(2, "OK");
    EXPECT(sel(&c), OK, "select");
    CHECK(!strcmp(c.slot, "CONFIG2") && text_is(T "/CONFIG2/STATE", "OK\n"), "CONFIG2 is OK and boots");
    rec = S.sb.rec_start;
    fclose(fl.f);
    for (int copy = 0; copy < 2; copy++) {
        FILE *f = fopen(img, "r+b");
        long off = (long)(rec + (uint32_t)copy) * RWY_BLOCK_SIZE + 100;
        int ch;
        if (!f || fseek(f, off, SEEK_SET)) exit(1);
        ch = fgetc(f);
        if (fseek(f, off, SEEK_SET) || fputc(ch ^ 0xff, f) == EOF) exit(1);
        fclose(f);
    }
    open_store(&S, &dev, &fl, img);
    CHECK(rwy_record_damaged(&S), "mount reports the damage");
    { uint8_t r[RWY_REC_PAYLOAD];
      CHECK(rwy_record_read(&S, r) == OK && rwy_ss_record_reset(r) == 1, "and the reset is counted"); }
    CHECK(text_is(T "/CONFIG2/STATE", "INSTALLED\n") && text_is(T "/CONFIG2/TRIES", "1\n"), "a slot with no entry is INSTALLED with 1 try");
    EXPECT(sel(&c), OK, "select");
    CHECK(has("RUNWAY: state record damaged") && c.booted && !strcmp(c.slot, "CONFIG2") && c.tries_left == 0,
          "selection says so and tries the top slot again");
    CHECK(!rwy_record_damaged(&S) && text_is(T "/CONFIG2/STATE", "TESTING\n"), "the write healed the record");
    { uint8_t r[RWY_REC_PAYLOAD];
      CHECK(rwy_record_read(&S, r) == OK && rwy_ss_record_reset(r) == 1, "the count is kept by the write"); }
    EXPECT(sel(&c), OK, "unconfirmed");
    CHECK(!has("state record damaged") && !strcmp(c.slot, "CONFIG1"), "no more message; CONFIG2 failed, CONFIG1 is next");
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after the damage was healed");
}

/* The record payload layout of ADR 0002 N4: header fields and the entries keyed by revision. */
void test_slotstate(void) {
    uint8_t r[RWY_REC_PAYLOAD] = { 0 }, zero[RWY_REC_PAYLOAD] = { 0 };
    rwy_ss_entry_t e = { 3, RWY_SS_TESTING, RWY_SS_CAUSE_NONE, 0 }, got;
    uint64_t live[2] = { 5, 7 };
    char name[32];
    printf("== slot state record ==\n");
    CHECK(!rwy_ss_recover_next(r) && rwy_ss_reason(r) == RWY_SS_REASON_NONE && rwy_ss_booted(r, name) == 0 &&
          !rwy_ss_get(r, 1, &got), "the zero payload is empty");
    rwy_ss_set_recover_next(r, 1);
    CHECK(rwy_ss_recover_next(r), "RECOVER_NEXT set");
    rwy_ss_set_recover_next(r, 0);
    CHECK(!rwy_ss_recover_next(r) && !memcmp(r, zero, sizeof(r)), "RECOVER_NEXT cleared");
    rwy_ss_set_reason(r, RWY_SS_REASON_ALL_FAILED);
    CHECK(rwy_ss_reason(r) == RWY_SS_REASON_ALL_FAILED, "reason");
    rwy_ss_set_reason(r, RWY_SS_REASON_BOOT_LOOP);
    CHECK(rwy_ss_reason(r) == RWY_SS_REASON_BOOT_LOOP, "reason boot_loop");
    rwy_ss_set_reason(r, RWY_SS_REASON_ALL_FAILED);
    EXPECT(rwy_ss_set_booted(r, "CONFIG18446744073709551615"), OK, "longest slot name");
    CHECK(rwy_ss_booted(r, name) == 26 && !strcmp(name, "CONFIG18446744073709551615"), "BOOTED name read back");
    EXPECT(rwy_ss_set_booted(r, "RECOVERY"), OK, "shorter name");
    CHECK(rwy_ss_booted(r, name) == 8 && !strcmp(name, "RECOVERY"), "a shorter name leaves no tail");
    EXPECT(rwy_ss_set_booted(r, "0123456789012345678901234567890"), INV, "name too long");
    EXPECT(rwy_ss_set_booted(r, 0), OK, "BOOTED cleared");
    CHECK(rwy_ss_booted(r, name) == 0, "BOOTED none");

    EXPECT(rwy_ss_put(r, 5, &e), OK, "put");
    CHECK(rwy_ss_get(r, 5, &got) && got.tries == 3 && got.state == RWY_SS_TESTING, "get");
    CHECK(!rwy_ss_get(r, 6, &got), "another revision has no entry");
    e.tries = 0; e.state = RWY_SS_FAILED;
    EXPECT(rwy_ss_put(r, 5, &e), OK, "update in place");
    CHECK(rwy_ss_get(r, 5, &got) && got.tries == 0 && got.state == RWY_SS_FAILED, "updated");
    e.cause = RWY_SS_CAUSE_LOAD;
    EXPECT(rwy_ss_put(r, 5, &e), OK, "put with a cause");
    CHECK(rwy_ss_get(r, 5, &got) && got.cause == RWY_SS_CAUSE_LOAD, "the cause of FAILED is kept");
    e.cause = RWY_SS_CAUSE_LOOP;
    EXPECT(rwy_ss_put(r, 5, &e), OK, "put with the boot loop cause");
    CHECK(rwy_ss_get(r, 5, &got) && got.cause == RWY_SS_CAUSE_LOOP, "the boot loop cause is kept");
    e.cause = RWY_SS_CAUSE_LOAD;
    r[32 + 17] = 99;
    CHECK(rwy_ss_get(r, 5, &got) && got.cause == RWY_SS_CAUSE_NONE, "an unknown cause code reads as none");
    e.pinned = 1;
    EXPECT(rwy_ss_put(r, 5, &e), OK, "put pinned");
    CHECK(rwy_ss_get(r, 5, &got) && got.pinned && r[32 + 18] == 1, "the pin is kept");
    r[32 + 18] = 7;
    CHECK(rwy_ss_get(r, 5, &got) && !got.pinned, "an unknown pin code reads as not pinned");
    e.pinned = 0;
    e.state = RWY_SS_TESTING;
    EXPECT(rwy_ss_put(r, 5, &e), OK, "leave FAILED");
    CHECK(rwy_ss_get(r, 5, &got) && got.cause == RWY_SS_CAUSE_NONE && r[32 + 17] == 0, "a state other than FAILED keeps no cause");
    e.state = RWY_SS_FAILED; e.cause = RWY_SS_CAUSE_NONE;
    EXPECT(rwy_ss_put(r, 5, &e), OK, "FAILED again");
    r[32 + 16] = 9;
    CHECK(rwy_ss_get(r, 5, &got) && got.state == RWY_SS_FAILED, "an unknown state code reads as FAILED");
    e.tries = 0; e.state = RWY_SS_FAILED;
    EXPECT(rwy_ss_put(r, 5, &e), OK, "and is overwritten");
    EXPECT(rwy_ss_put(r, 0, &e), INV, "revision 0 is not a key");
    EXPECT(rwy_ss_put(r, 9, &e), OK, "second entry");
    rwy_ss_prune(r, live, 2);
    CHECK(rwy_ss_get(r, 5, &got) && !rwy_ss_get(r, 9, &got), "prune keeps live revisions and drops the others");
    rwy_ss_prune(r, live, 0);
    rwy_ss_set_reason(r, 0);
    CHECK(!memcmp(r, zero, sizeof(r)), "prune leaves no residue");
    for (uint64_t v = 1; v <= RWY_SS_ENTRIES; v++) EXPECT(rwy_ss_put(r, v, &e), OK, "fill the table");
    EXPECT(rwy_ss_put(r, RWY_SS_ENTRIES + 1, &e), RWY_ERR_NOSPC, "one revision too many has no room");
    EXPECT(rwy_ss_put(r, RWY_SS_ENTRIES, &e), OK, "a known revision still updates");
}

/* The mirror health table and the reset counter of the record payload (ADR 0005). */
void test_slotstate_health(void) {
    uint8_t r[RWY_REC_PAYLOAD] = { 0 }, r2[RWY_REC_PAYLOAD], zero[RWY_REC_PAYLOAD] = { 0 };
    uint8_t a[16] = { 1 }, b[16] = { 2 }, none[16] = { 0 };
    rwy_ss_health_t h;
    printf("== mirror health in the record ==\n");
    CHECK(!rwy_ss_health_get(r, a, &h) && rwy_ss_record_reset(r) == 0, "the zero payload has no health");
    EXPECT(rwy_ss_health_note(r, none, RWY_SS_EV_LAGGING, 1), INV, "a member with no UUID is not a key");
    EXPECT(rwy_ss_health_note(r, a, 0, 1), INV, "event 0 is not an event");
    EXPECT(rwy_ss_health_note(r, a, RWY_SS_EV_LAST + 1, 1), INV, "an unknown event");
    EXPECT(rwy_ss_health_note(r, a, RWY_SS_EV_LAGGING, 4), OK, "note an event");
    EXPECT(rwy_ss_health_note(r, a, RWY_SS_EV_LAGGING, 5), OK, "and again");
    EXPECT(rwy_ss_health_note(r, a, RWY_SS_EV_SYNC_FAILED, 6), OK, "another kind");
    CHECK(rwy_ss_health_get(r, a, &h) && h.count[RWY_SS_EV_LAGGING] == 2 && h.count[RWY_SS_EV_SYNC_FAILED] == 1 &&
          h.count[RWY_SS_EV_WRITE_FAILED] == 0 && h.last_event == RWY_SS_EV_SYNC_FAILED && h.seq == 6, "counters, last event and sequence");
    CHECK(!rwy_ss_health_get(r, b, &h), "another member has none");
    memset(r2, 0, sizeof(r2));
    for (int i = 0; i < 3; i++) rwy_ss_health_note(r2, b, RWY_SS_EV_MISSING, 7);
    EXPECT(rwy_ss_health_note(r2, a, RWY_SS_EV_LAGGING, 9), OK, "second table");
    rwy_ss_note_record_reset(r2);
    rwy_ss_health_merge(r, r2);
    CHECK(rwy_ss_health_get(r, a, &h) && h.count[RWY_SS_EV_LAGGING] == 2 && h.last_event == RWY_SS_EV_LAGGING && h.seq == 9,
          "merge: the larger count, the event with the larger sequence");
    CHECK(rwy_ss_health_get(r, b, &h) && h.count[RWY_SS_EV_MISSING] == 3, "merge adds a member the table lacks");
    CHECK(rwy_ss_record_reset(r) == 1, "merge takes the larger reset count");
    rwy_ss_health_merge(r, zero);
    CHECK(rwy_ss_health_get(r, a, &h) && h.count[RWY_SS_EV_LAGGING] == 2, "merging an empty table changes nothing");
    rwy_ss_health_clear(r);
    CHECK(!rwy_ss_health_get(r, a, &h) && !rwy_ss_health_get(r, b, &h) && rwy_ss_record_reset(r) == 1, "clear drops the members, not the reset count");
    rwy_ss_health_clear(r);
    for (unsigned i = 0; i < RWY_SS_HEALTH_MEMBERS; i++) {
        uint8_t u[16] = { (uint8_t)(i + 1) };
        EXPECT(rwy_ss_health_note(r, u, RWY_SS_EV_MISSING, 1), OK, "fill the table");
    }
    a[0] = 0xee;
    EXPECT(rwy_ss_health_note(r, a, RWY_SS_EV_MISSING, 1), RWY_ERR_NOSPC, "a member too many has no room");
}

/* The member roster of the record payload (ADR 0005). */
void test_slotstate_roster(void) {
    uint8_t r[RWY_REC_PAYLOAD] = { 0 }, r2[RWY_REC_PAYLOAD] = { 0 }, zero[RWY_REC_PAYLOAD] = { 0 };
    uint8_t a[16] = { 1 }, b[16] = { 2 }, none[16] = { 0 }, u[16];
    printf("== member roster in the record ==\n");
    CHECK(!rwy_ss_roster_has(r, a) && !rwy_ss_roster_at(r, 0, u), "the zero payload expects no member");
    EXPECT(rwy_ss_roster_add(r, none), INV, "a member with no UUID is not listed");
    EXPECT(rwy_ss_roster_add(r, a), 1, "add a member");
    EXPECT(rwy_ss_roster_add(r, a), 0, "adding it again changes nothing");
    CHECK(rwy_ss_roster_has(r, a) && !rwy_ss_roster_has(r, b), "the roster lists it, and not another");
    EXPECT(rwy_ss_roster_add(r2, b), 1, "second roster");
    rwy_ss_roster_merge(r, r2);
    CHECK(rwy_ss_roster_has(r, a) && rwy_ss_roster_has(r, b), "merge is a union");
    rwy_ss_roster_merge(r, zero);
    CHECK(rwy_ss_roster_has(r, a) && rwy_ss_roster_has(r, b), "merging an empty roster changes nothing");
    CHECK(rwy_ss_roster_at(r, 0, u) && u[0] == 1 && rwy_ss_roster_at(r, 1, u) && u[0] == 2 &&
          !rwy_ss_roster_at(r, 2, u) && !rwy_ss_roster_at(r, RWY_SS_ROSTER_MEMBERS, u), "entries by index");
    CHECK(rwy_ss_roster_forget(r, a) == 1 && !rwy_ss_roster_has(r, a) && rwy_ss_roster_has(r, b), "forget drops one member");
    CHECK(rwy_ss_roster_forget(r, a) == 0 && rwy_ss_roster_forget(r, none) == 0, "forgetting an unknown member does nothing");
    memset(r, 0, sizeof(r));
    for (unsigned i = 0; i < RWY_SS_ROSTER_MEMBERS; i++) {
        uint8_t m[16] = { (uint8_t)(i + 1) };
        EXPECT(rwy_ss_roster_add(r, m), 1, "fill the roster");
    }
    a[0] = 0xee;
    EXPECT(rwy_ss_roster_add(r, a), RWY_ERR_NOSPC, "a member too many has no room");
    CHECK(rwy_ss_health_get(r, a, &(rwy_ss_health_t){ 0 }) == 0 && rwy_ss_record_reset(r) == 0, "the roster does not touch the health table");
}
