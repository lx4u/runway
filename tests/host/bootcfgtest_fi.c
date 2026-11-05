/* Fault injection over subtree creation, finalize, selection and condemn. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "bootcfgtest.h"

void test_fault_injection(const char *base) {
    const char *img = tpath("rwy_fi_bootcfg_base.img"), *cr_img = tpath("rwy_fi_bootcfg_crash.img");
    file_t fl;
    rwy_block_dev_t inner, dev;
    fi_t fi;
    long pts = 0;
    printf("== fault injection: subtree creation ==\n");
    copy_file(base, img);
    fl.f = fopen(img, "r+b");
    inner.read = file_rd; inner.write = file_wr; inner.flush = file_fl; inner.ctx = &fl;
    inner.size = RWY_STORE_SIZE; inner.block_size = RWY_BLOCK_SIZE;
    dev.read = fi_rd; dev.write = fi_wr; dev.flush = fi_fl; dev.ctx = &fi;
    dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
    fclose(fl.f);
    for (long pt = 0; pt <= 64; pt++) {         /* pt 0 counts the points */
        rwy_store_t s;
        char b[16];
        uint64_t o;
        size_t n = sizeof(b);
        copy_file(img, cr_img);
        fl.f = fopen(cr_img, "r+b");
        fi.inner = inner; fi.point = 0; fi.fail_at = pt ? pt : -1;
        if (rwy_mount(&s, &dev) == OK) (void)rwy_txn_mkdir(&s, T, RWY_S_IFDIR | 0755, 0, 0);
        if (!pt) pts = fi.point;
        fclose(fl.f);
        if (pt > pts && pts) break;
        fl.f = fopen(cr_img, "r+b");
        fi.fail_at = -1;
        CHECK(rwy_mount(&s, &inner) == OK, "remount after fault");
        if (rwy_resolve(&s, T, 0, &o) == OK) {
            CHECK(rwy_resolve(&s, T "/VERSION", 0, &o) == OK, "subtree without VERSION after a fault");
            if (rwy_resolve(&s, T "/VERSION", 0, &o)) printf("  at point %ld of %ld\n", pt, pts);
            CHECK(rwy_read(&s, o, 0, b, &n) == OK && n == 2 && !memcmp(b, "1\n", 2), "VERSION content after a fault");
        }
        fclose(fl.f);
        CHECK(run_checker(cr_img) == 0, "checker passes on crash state");
    }
    CHECK(pts > 0, "counted persistence points");
}

/* Fault injection over finalize: after any failed write/flush the slot is either
 * untouched or complete, and counter, slot revision and state agree. */
void fi_finalize(const char *base, const char *what, int with_counter) {
    const char *img = tpath("rwy_fi_fin_base.img"), *cr_img = tpath("rwy_fi_fin_crash.img");
    file_t fl;
    rwy_block_dev_t inner, dev, d2;
    fi_t fi;
    long pts = 0, old = with_counter ? 41 : -1;
    int untouched = 0;
    printf("== fault injection: finalize (%s) ==\n", what);
    copy_file(base, img);
    open_store(&S, &d2, &fl, img);
    CHECK(mk(T) == OK && mk(T "/BLOBS") == OK, "subtree");
    slot(1);
    EXPECT(install(1), OK, "first slot");           /* counter = 1 */
    if (with_counter) set_counter("41\n"); else set_counter("");
    slot(2);                                        /* the slot under test */
    fclose(fl.f);
    inner.read = file_rd; inner.write = file_wr; inner.flush = file_fl; inner.ctx = &fl;
    inner.size = RWY_STORE_SIZE; inner.block_size = RWY_BLOCK_SIZE;
    dev.read = fi_rd; dev.write = fi_wr; dev.flush = fi_fl; dev.ctx = &fi;
    dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
    for (long pt = 0; pt <= 400; pt++) {
        rwy_store_t s;
        uint64_t o;
        copy_file(img, cr_img);
        fl.f = fopen(cr_img, "r+b");
        fi.inner = inner; fi.point = 0; fi.fail_at = pt ? pt : -1;
        if (rwy_mount(&s, &dev) == OK && rwy_begin(&s) == OK) {
            if (rwy_lookup(&s, T "/CONFIG2/STATE", &o) == OK && rwy_write(&s, o, 0, "INSTALLED\n", 10) == OK)
                (void)rwy_commit(&s);
            else
                (void)rwy_abort(&s);
        }
        if (!pt) pts = fi.point;
        fclose(fl.f);
        if (pt > pts && pts) break;
        fl.f = fopen(cr_img, "r+b");
        fi.fail_at = -1;
        CHECK(rwy_mount(&S, &inner) == OK, "remount after fault");
        {
            long r = rev(2), c = counter();
            char b[16];
            int n = read_all(T "/CONFIG2/STATE", b, sizeof(b));
            untouched = r == -1;
            if (r == -1) CHECK(n == 0 && c == old, "slot untouched: no REVISION, empty STATE, counter unchanged");
            else CHECK(n == 10 && !memcmp(b, "INSTALLED\n", 10) && r == c && c == (with_counter ? 42 : 2),
                       "slot complete: STATE, REVISION and counter agree");
            if (r == -1 && !(n == 0 && c == old)) printf("  at point %ld of %ld: n=%d counter=%ld\n", pt, pts, n, c);
        }
        fclose(fl.f);
        /* A counter-less store with a finalized slot is flagged (and repairable); every other state is clean. */
        CHECK(run_checker(cr_img) == (!with_counter && untouched ? 4 : 0), "checker verdict on crash state");
    }
    CHECK(pts > 0, "counted persistence points");
}

/* Fault injection over a selection transaction: after any failed write/flush the state is
 * wholly the old one or wholly the new one (STATE, TRIES, BOOTED and REASON together). */
void fi_select(const char *base, int fallback) {
    const char *img = tpath("rwy_fi_sel_base.img"), *cr_img = tpath("rwy_fi_sel_crash.img");
    file_t fl;
    rwy_block_dev_t inner, dev, d2;
    fi_t fi;
    long pts = 0;
    printf("== fault injection: selection (%s) ==\n", fallback ? "TESTING exhausted, fallback" : "INSTALLED -> TESTING");
    copy_file(base, img);
    open_store(&S, &d2, &fl, img);
    fresh_subtree();
    fin(1, "OK");
    fin(2, "INSTALLED");
    if (fallback) {
        rwy_boot_choice_t c;
        EXPECT(sel(&c), OK, "first boot of CONFIG2");   /* TESTING, TRIES 0 */
    }
    fclose(fl.f);
    inner.read = file_rd; inner.write = file_wr; inner.flush = file_fl; inner.ctx = &fl;
    inner.size = RWY_STORE_SIZE; inner.block_size = RWY_BLOCK_SIZE;
    dev.read = fi_rd; dev.write = fi_wr; dev.flush = fi_fl; dev.ctx = &fi;
    dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
    for (long pt = 0; pt <= 400; pt++) {
        rwy_store_t s;
        rwy_boot_choice_t c;
        copy_file(img, cr_img);
        fl.f = fopen(cr_img, "r+b");
        fi.inner = inner; fi.point = 0; fi.fail_at = pt ? pt : -1;
        if (rwy_mount(&s, &dev) == OK && sel_with(&s, &c) != OK) {
            CHECK(!has("RUNWAY: booting"), "no booting line when the commit fails");
            fi.fail_at = -1;   /* the retry of RUNWAY.EFI: same handle, the medium works again */
            CHECK(sel_with(&s, &c) == OK && c.engaged && c.booted && has("RUNWAY: booting"), "retry on the same handle commits");
        }
        if (!pt) pts = fi.point;
        fclose(fl.f);
        if (pt > pts && pts) break;
        fl.f = fopen(cr_img, "r+b");
        fi.fail_at = -1;
        CHECK(rwy_mount(&S, &inner) == OK, "remount after fault");
        {
            int old_st = text_is(T "/CONFIG2/STATE", fallback ? "TESTING\n" : "INSTALLED\n");
            int new_st = text_is(T "/CONFIG2/STATE", fallback ? "FAILED\n" : "TESTING\n");
            int old_b = fallback ? booted_is("CONFIG2") : text_is(T "/CONFIG2/TRIES", "1\n");
            int new_b = fallback ? booted_is("CONFIG1") && text_is(T "/REASON", "normal\n")
                                 : booted_is("CONFIG2") && text_is(T "/CONFIG2/TRIES", "0\n") && text_is(T "/REASON", "normal\n");
            if (!fallback) old_b = old_b && rwy_readlink(&S, T "/BOOTED", (char[8]){0}, 8) != OK;
            CHECK((old_st && old_b) || (new_st && new_b), "selection is wholly old or wholly new");
            if (!((old_st && old_b) || (new_st && new_b))) printf("  at point %ld of %ld\n", pt, pts);
        }
        fclose(fl.f);
        CHECK(run_checker(cr_img) == 0, "checker passes on crash state");
    }
    CHECK(pts > 0, "counted persistence points");
}

/* Selection of a requested recovery (and the all_failed shape): wholly old or wholly new. A request whose
 * commit fails must be ignored and the normal path taken (§2.6). */
void fi_recovery(const char *base, int requested) {
    const char *img = tpath("rwy_fi_rec_base.img"), *cr_img = tpath("rwy_fi_rec_crash.img");
    file_t fl;
    rwy_block_dev_t inner, dev, d2;
    fi_t fi;
    long pts = 0;
    printf("== fault injection: recovery (%s) ==\n", requested ? "RECOVER_NEXT" : "all_failed");
    copy_file(base, img);
    open_store(&S, &d2, &fl, img);
    fresh_subtree();
    fin(1, "OK");
    mkrecovery();
    if (requested) CHECK(cr(T "/RECOVER_NEXT") == OK, "request"); else set_state(1, "FAILED\n");
    fclose(fl.f);
    inner.read = file_rd; inner.write = file_wr; inner.flush = file_fl; inner.ctx = &fl;
    inner.size = RWY_STORE_SIZE; inner.block_size = RWY_BLOCK_SIZE;
    dev.read = fi_rd; dev.write = fi_wr; dev.flush = fi_fl; dev.ctx = &fi;
    dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
    for (long pt = 0; pt <= 400; pt++) {
        rwy_store_t s;
        rwy_boot_choice_t c;
        int rc = -1, ran = 0, want_rec;
        copy_file(img, cr_img);
        fl.f = fopen(cr_img, "r+b");
        fi.inner = inner; fi.point = 0; fi.fail_at = pt ? pt : -1;
        if (rwy_mount(&s, &dev) == OK) { rc = sel_with(&s, &c); ran = 1; }
        if (ran && rc != OK) CHECK(!has("booting RECOVERY"), "no recovery line when the commit fails");
        if (!pt) pts = fi.point;
        fclose(fl.f);
        if (pt > pts && pts) break;
        fl.f = fopen(cr_img, "r+b");
        fi.fail_at = -1;
        CHECK(rwy_mount(&S, &inner) == OK, "remount after fault");
        want_rec = booted_is("RECOVERY");
        if (requested) {
            int pending = read_all(T "/RECOVER_NEXT", (char[2]){0}, 2) == 0;
            /* wholly old: request pending, nothing booted yet; wholly new: consumed, RECOVERY, requested. */
            int old_s = pending && !want_rec;
            int new_s = !pending && want_rec && text_is(T "/REASON", "requested\n");
            /* the request failed to commit and the normal path ran: request pending, CONFIG1 booted. */
            int ign = pending && booted_is("CONFIG1") && text_is(T "/REASON", "normal\n");
            CHECK(old_s || new_s || ign, "request is wholly old, wholly new or ignored");
            if (ran && rc == OK && c.booted) CHECK(!strcmp(c.slot, "RECOVERY") ? new_s : ign, "the choice matches the disk");
            if (!(old_s || new_s || ign)) printf("  at point %ld of %ld\n", pt, pts);
        } else {
            int old_s = !want_rec && !booted_is("CONFIG1");
            int new_s = want_rec && text_is(T "/REASON", "all_failed\n");
            CHECK(old_s || new_s, "all_failed is wholly old or wholly new");
            if (!(old_s || new_s)) printf("  at point %ld of %ld\n", pt, pts);
        }
        fclose(fl.f);
        CHECK(run_checker(cr_img) == 0, "checker passes on crash state");
    }
    CHECK(pts > 0, "counted persistence points");
}

/* Condemning a slot is one record write: wholly old (TESTING, still BOOTED) or wholly new (FAILED). */
void fi_condemn(const char *base) {
    const char *img = tpath("rwy_fi_cnd_base.img"), *cr_img = tpath("rwy_fi_cnd_crash.img");
    file_t fl;
    rwy_block_dev_t inner, dev, d2;
    fi_t fi;
    rwy_boot_choice_t c;
    long pts = 0;
    printf("== fault injection: condemn ==\n");
    copy_file(base, img);
    open_store(&S, &d2, &fl, img);
    fresh_subtree();
    fin(1, "OK");
    fin(2, "INSTALLED");
    EXPECT(sel(&c), OK, "first boot of CONFIG2");
    fclose(fl.f);
    inner.read = file_rd; inner.write = file_wr; inner.flush = file_fl; inner.ctx = &fl;
    inner.size = RWY_STORE_SIZE; inner.block_size = RWY_BLOCK_SIZE;
    dev.read = fi_rd; dev.write = fi_wr; dev.flush = fi_fl; dev.ctx = &fi;
    dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
    for (long pt = 0; pt <= 400; pt++) {
        rwy_store_t s;
        copy_file(img, cr_img);
        fl.f = fopen(cr_img, "r+b");
        fi.inner = inner; fi.point = 0; fi.fail_at = pt ? pt : -1;
        if (rwy_mount(&s, &dev) == OK) {
            rwy_set_actor(&s, RWY_ACTOR_SYSTEM);
            (void)rwy_bootcfg_condemn(&s, "CONFIG2");
        }
        if (!pt) pts = fi.point;
        fclose(fl.f);
        if (pt > pts && pts) break;
        fl.f = fopen(cr_img, "r+b");
        fi.fail_at = -1;
        CHECK(rwy_mount(&S, &inner) == OK, "remount after fault");
        CHECK(text_is(T "/CONFIG2/STATE", "TESTING\n") || text_is(T "/CONFIG2/STATE", "FAILED\n"), "STATE is wholly old or new");
        fclose(fl.f);
        CHECK(run_checker(cr_img) == 0, "checker passes on crash state");
    }
    CHECK(pts > 0, "counted persistence points");
}

static int fail_reads;

static int rd_maybe_fail(void *c, uint64_t o, void *b, size_t l) {
    return fail_reads ? -1 : fi_rd(c, o, b, l);
}

/* A read error on EFI/Runway is an error, not a store without EFI/Runway. */
void test_read_error(const char *base) {
    const char *img = tpath("rwy_fi_bootcfg_rderr.img");
    file_t fl;
    rwy_block_dev_t inner, dev, d2;
    rwy_boot_choice_t c;
    rwy_store_t s;
    fi_t fi;
    printf("== read error on EFI/Runway ==\n");
    copy_file(base, img);
    open_store(&S, &d2, &fl, img);
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    EXPECT(rwy_bootcfg_select(&S, &c, logfn, NULL), OK, "store without EFI/Runway");
    rwy_set_actor(&S, RWY_ACTOR_USER);
    CHECK(!c.engaged, "store without EFI/RUNWAY: not engaged");
    fresh_subtree();
    fin(1, "OK");
    fclose(fl.f);
    fl.f = fopen(img, "r+b");
    inner.read = file_rd; inner.write = file_wr; inner.flush = file_fl; inner.ctx = &fl;
    inner.size = RWY_STORE_SIZE; inner.block_size = RWY_BLOCK_SIZE;
    dev.read = rd_maybe_fail; dev.write = fi_wr; dev.flush = fi_fl; dev.ctx = &fi;
    dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
    fi.inner = inner; fi.point = 0; fi.fail_at = -1; fail_reads = 0;
    EXPECT(rwy_mount(&s, &dev), OK, "mount");
    rwy_set_actor(&s, RWY_ACTOR_SYSTEM);
    fail_reads = 1;
    CHECK(rwy_bootcfg_select(&s, &c, logfn, NULL) != OK, "select: read error is an error");
    CHECK(rwy_bootcfg_preview(&s, &c) != OK, "preview: read error is an error");
    CHECK(rwy_bootcfg_fallback(&s, &c, NULL) != OK, "fallback: read error is an error");
    fail_reads = 0;
    EXPECT(rwy_bootcfg_select(&s, &c, logfn, NULL), OK, "select after the error");
    CHECK(c.engaged && c.booted && !strcmp(c.slot, "CONFIG1"), "the error left nothing behind");
    fclose(fl.f);
}

static long reads_left;

static int rd_after(void *c, uint64_t o, void *b, size_t l) {
    return reads_left-- <= 0 ? -1 : fi_rd(c, o, b, l);
}

/* A read error anywhere in selection or in the audit is an error, never a quieter answer. */
void test_read_error_sweep(const char *base) {
    const char *img = tpath("rwy_fi_bootcfg_rdsweep.img");
    file_t fl;
    rwy_block_dev_t inner, dev, d2;
    rwy_boot_choice_t c;
    rwy_bootcfg_audit_t au;
    fi_t fi;
    long n, failed = 0, max_reads;
    printf("== read error inside a slot ==\n");
    copy_file(base, img);
    open_store(&S, &d2, &fl, img);
    fresh_subtree();
    fin(1, "OK");
    fin(2, "INSTALLED");
    fclose(fl.f);
    fl.f = fopen(img, "r+b");
    inner.read = file_rd; inner.write = file_wr; inner.flush = file_fl; inner.ctx = &fl;
    inner.size = RWY_STORE_SIZE; inner.block_size = RWY_BLOCK_SIZE;
    dev.read = rd_after; dev.write = fi_wr; dev.flush = fi_fl; dev.ctx = &fi;
    dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
    fi.inner = inner; fi.point = 0; fi.fail_at = -1;
    {
        rwy_store_t s;
        reads_left = 1000000;
        EXPECT(rwy_mount(&s, &dev), OK, "mount");
        rwy_set_actor(&s, RWY_ACTOR_SYSTEM);
        EXPECT(rwy_bootcfg_audit(&s, &au, NULL, NULL), OK, "audit without faults");
        max_reads = 1000000 - reads_left;
        CHECK(au.engaged && au.slots == 2, "audit without faults sees both slots");
    }
    for (n = 0; n <= max_reads; n++) {
        rwy_store_t s;
        int rc;
        reads_left = 1000000;
        EXPECT(rwy_mount(&s, &dev), OK, "mount");
        rwy_set_actor(&s, RWY_ACTOR_SYSTEM);
        reads_left = n;
        memset(&c, 0, sizeof(c));
        rc = rwy_bootcfg_preview(&s, &c);
        if (rc == OK) {
            CHECK(c.engaged && c.booted && !strcmp(c.slot, "CONFIG2"), "preview: the right slot or an error");
            if (!(c.engaged && c.booted && !strcmp(c.slot, "CONFIG2"))) printf("  after %ld reads\n", n);
        } else failed++;
        reads_left = 1000000;
        EXPECT(rwy_mount(&s, &dev), OK, "remount");
        reads_left = n;
        rc = rwy_bootcfg_audit(&s, &au, NULL, NULL);
        if (rc == OK) {
            CHECK(au.engaged && au.slots == 2 && !au.violations, "audit: clean or an error");
            if (!(au.engaged && au.slots == 2 && !au.violations)) printf("  after %ld reads\n", n);
        } else failed++;
    }
    CHECK(failed > 0, "some read point failed");
    fclose(fl.f);
}
