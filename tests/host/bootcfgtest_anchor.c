/* bootcfgtest: the anti-rollback fields of the state record and the tree bind. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "bootcfgtest.h"

static uint64_t rec_rev_hw(void) {
    uint8_t r[RWY_REC_PAYLOAD];
    rwy_record_read(&S, r);
    return rwy_ss_rev_hw(r);
}

static void rec_anchor(int enrolled, uint64_t epoch, uint64_t rev_hw) {
    uint8_t r[RWY_REC_PAYLOAD];
    rwy_record_read(&S, r);
    rwy_ss_set_enrolled(r, enrolled);
    rwy_ss_set_epoch(r, epoch);
    rwy_ss_set_rev_hw(r, rev_hw);
    EXPECT(rwy_record_write(&S, r), OK, "record write");
}

/* Put the image `from` over `to`, except the two record blocks of `to`. */
static void replay_tree(const char *from, const char *to, uint32_t rec_start) {
    uint8_t blk[2 * RWY_BLOCK_SIZE];
    FILE *f = fopen(to, "r+b");
    long off = (long)rec_start * RWY_BLOCK_SIZE;
    if (!f || fseek(f, off, SEEK_SET) || fread(blk, 1, sizeof(blk), f) != sizeof(blk)) exit(1);
    fclose(f);
    copy_file(from, to);
    f = fopen(to, "r+b");
    if (!f || fseek(f, off, SEEK_SET) || fwrite(blk, 1, sizeof(blk), f) != sizeof(blk)) exit(1);
    fclose(f);
}

typedef struct { uint64_t value; int missing, reads, incs, unset, fail_inc, absent, denied; } fake_t;
static int fake_read(void *c, uint64_t *v) {
    fake_t *f = c;
    f->reads++;
    if (f->missing) return RWY_ERR_NOENT;
    if (f->absent) return RWY_ANCHOR_READ_ABSENT;
    if (f->denied) return RWY_ANCHOR_READ_DENIED;
    if (f->unset) return RWY_ANCHOR_READ_UNSET;
    *v = f->value;
    return OK;
}
/* Like fake_read, but the counter reads as "never incremented" only until the first increment. */
static int fake_read_after_inc(void *c, uint64_t *v) {
    fake_t *f = c;
    if (f->incs) f->unset = 0;
    return fake_read(c, v);
}
static int fake_inc(void *c) {
    fake_t *f = c;
    if (f->fail_inc) return RWY_ERR_IO;
    f->incs++;
    f->value++;
    return OK;
}

static int anchor(fake_t *f, int enrolled, uint64_t epoch) {
    rwy_anchor_ops_t ops = { fake_read, fake_inc, f };
    rec_anchor(enrolled, epoch, 0);
    return rwy_anchor_check(&S, &ops);
}

static void test_anchor_rules(const char *base) {
    const char *img = tpath("rwy_test_bootcfg_anc.img");
    file_t fl;
    rwy_block_dev_t dev;
    fake_t f = { 5, 0, 0, 0, 0, 0, 0, 0 };
    printf("== anchor: epoch against the counter ==\n");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    EXPECT(anchor(&f, 0, 9), RWY_ANCHOR_OFF, "not enrolled");
    CHECK(f.reads == 0 && f.incs == 0, "not enrolled: the counter is not touched");
    EXPECT(anchor(&f, 1, 5), RWY_ANCHOR_OK, "epoch equals the counter");
    CHECK(f.incs == 0, "equal: nothing to finish");
    EXPECT(anchor(&f, 1, 6), RWY_ANCHOR_FINISHED, "epoch one ahead: a cut between the record and the increment");
    CHECK(f.incs == 1 && f.value == 6, "the increment is finished");
    EXPECT(anchor(&f, 1, 5), RWY_ANCHOR_BEHIND, "epoch behind the counter");
    CHECK(f.incs == 1, "behind: the counter is not raised");
    EXPECT(anchor(&f, 1, 8), RWY_ANCHOR_AHEAD, "epoch two or more ahead");
    CHECK(f.incs == 1, "far ahead: the counter is not raised");
    f.unset = 1;
    EXPECT(anchor(&f, 1, 0), RWY_ANCHOR_NEW, "enrolled offline: the counter was never incremented");
    CHECK(f.incs == 1, "new: the caller does the first increment");
    f.unset = 0;
    f.missing = 1;
    EXPECT(anchor(&f, 1, 6), RWY_ANCHOR_MISSING, "enrolled and the counter is gone");
    f.reads = 0;
    EXPECT(anchor(&f, 0, 6), RWY_ANCHOR_OFF, "not enrolled and the counter is gone");
    CHECK(f.reads == 0, "not enrolled: no read");
    rec_anchor(1, 6, 0);
    EXPECT(rwy_anchor_check(&S, NULL), RWY_ANCHOR_MISSING, "enrolled and no TPM callbacks");
    fclose(fl.f);
}

/* A copy of the store made before enrollment says "not enrolled"; the counter of the store still exists. */
static void test_anchor_stray(const char *base) {
    const char *img = tpath("rwy_test_bootcfg_ans.img");
    file_t fl;
    rwy_block_dev_t dev;
    rwy_boot_choice_t c;
    fake_t f = { 5, 0, 0, 0, 0, 0, 0, 0 };
    rwy_anchor_ops_t ops = { fake_read, fake_inc, &f };
    uint64_t seq;
    printf("== anchor: a counter for a store that is not enrolled ==\n");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    fresh_subtree();
    fin(1, "OK");
    EXPECT(anchor(&f, 0, 0), RWY_ANCHOR_OFF, "unencrypted: the counter is not read");
    CHECK(f.reads == 0, "unencrypted: no read");
    S.sb.crypt_algo = RWY_CRYPT_AES_XTS_256;
    f.absent = 1;
    EXPECT(anchor(&f, 0, 0), RWY_ANCHOR_OFF, "encrypted, not enrolled, no counter");
    CHECK(f.reads == 1, "...the counter was looked up");
    f.absent = 0;
    EXPECT(anchor(&f, 0, 0), RWY_ANCHOR_STRAY, "not enrolled but the counter exists");
    CHECK(f.incs == 0, "stray: the counter is not raised");
    f.unset = 1;
    EXPECT(anchor(&f, 0, 0), RWY_ANCHOR_STRAY, "the counter exists and was never incremented (a cut inside enroll)");
    f.unset = 0;
    f.denied = 1;
    EXPECT(anchor(&f, 0, 0), RWY_ANCHOR_STRAY, "the counter exists but PCR 7 differs");
    f.denied = 0;
    f.missing = 1;
    EXPECT(anchor(&f, 0, 0), RWY_ANCHOR_OFF, "a TPM that cannot be read does not turn a plain store off");
    f.missing = 0;
    f.absent = 1;
    EXPECT(anchor(&f, 1, 5), RWY_ANCHOR_MISSING, "enrolled and the counter is absent");
    rec_anchor(0, 0, 0);
    EXPECT(rwy_anchor_check(&S, NULL), RWY_ANCHOR_OFF, "not enrolled and no callbacks");
    f.absent = 0;
    rwy_anchor_attach(&S, &ops);
    seq = rseq();
    EXPECT(sel(&c), RWY_ERR_ROLLBACK, "select refuses a store whose counter exists");
    CHECK(!c.engaged && !c.booted && rseq() == seq, "nothing engaged, nothing written");
    f.absent = 1;
    EXPECT(sel(&c), OK, "no counter: select works as before");
    fclose(fl.f);
    remove(img);
}

static void test_tree_bind(const char *base) {
    const char *img = tpath("rwy_test_bootcfg_rbk.img"), *old = tpath("rwy_test_bootcfg_rbk_old.img");
    file_t fl;
    rwy_block_dev_t dev;
    rwy_boot_choice_t c;
    uint32_t rec;
    uint64_t seq;
    printf("== rev_hw and the tree bind ==\n");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    CHECK(rec_rev_hw() == 0, "a new store has rev_hw 0");
    fresh_subtree();
    fin(1, "OK");
    CHECK(counter() == 1 && rec_rev_hw() == 1, "finalize raises rev_hw after the commit");
    fclose(fl.f);
    copy_file(img, old);
    open_store(&S, &dev, &fl, img);
    fin(2, "OK");
    CHECK(counter() == 2 && rec_rev_hw() == 2, "second finalize raises it again");
    rec = S.sb.rec_start;
    fclose(fl.f);

    replay_tree(old, img, rec);
    open_store(&S, &dev, &fl, img);
    CHECK(counter() == 1 && rec_rev_hw() == 2, "the tree is old, the record is new");
    seq = rseq();
    EXPECT(sel(&c), RWY_ERR_ROLLBACK, "select refuses a replayed tree");
    CHECK(!c.engaged && !c.booted && rseq() == seq, "nothing engaged, nothing written");
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    EXPECT(rwy_bootcfg_preview(&S, &c), RWY_ERR_ROLLBACK, "preview refuses it");
    EXPECT(rwy_bootcfg_fallback(&S, &c, NULL), RWY_ERR_ROLLBACK, "fallback refuses it");
    rwy_set_actor(&S, RWY_ACTOR_USER);
    CHECK(rseq() == seq && gen() > 0, "the store is still readable and unchanged");
    fclose(fl.f);

    /* A cut between the tree commit and the record write leaves the counter above rev_hw. */
    open_store(&S, &dev, &fl, img);
    rec_anchor(0, 0, 0);
    set_counter("2");
    fclose(fl.f);
    open_store(&S, &dev, &fl, img);
    EXPECT(sel(&c), OK, "counter above rev_hw is normal");
    CHECK(c.engaged && rec_rev_hw() == 2, "selection raises rev_hw to the highest revision");
    fclose(fl.f);
    remove(old);
}

static void test_anchor_set(const char *base) {
    const char *img = tpath("rwy_test_bootcfg_anc2.img");
    file_t fl;
    rwy_block_dev_t dev;
    uint64_t epoch = 99, seq;
    int enrolled = 1;
    printf("== anchor: enroll and reset the record fields ==\n");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    rwy_anchor_get(&S, &epoch, &enrolled);
    CHECK(epoch == 0 && enrolled == 0, "a new store is not enrolled");
    rec_anchor(0, 0, 7);
    seq = rseq();
    EXPECT(rwy_anchor_set(&S, 12, 1), OK, "enroll with epoch 12");
    rwy_anchor_get(&S, &epoch, &enrolled);
    CHECK(epoch == 12 && enrolled == 1, "epoch and flag are read back");
    CHECK(rseq() == seq + 1 && rec_rev_hw() == 7, "one record write, rev_hw untouched");
    fclose(fl.f);
    open_store(&S, &dev, &fl, img);
    rwy_anchor_get(&S, &epoch, &enrolled);
    CHECK(epoch == 12 && enrolled == 1, "the fields survive a remount");
    fclose(fl.f);
}

static uint64_t epoch_of(void) {
    uint64_t e;
    int en;
    rwy_anchor_get(&S, &e, &en);
    return e;
}

static void test_anchor_bind(const char *base) {
    const char *img = tpath("rwy_test_bootcfg_anb.img");
    file_t fl;
    rwy_block_dev_t dev;
    rwy_boot_choice_t c;
    fake_t f = { 5, 0, 0, 0, 0, 0, 0, 0 };
    rwy_anchor_ops_t ops = { fake_read, fake_inc, &f };
    uint64_t seq;
    printf("== anchor: selection checks and raises the epoch ==\n");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    fresh_subtree();
    fin(1, "OK");
    fin(2, "INSTALLED");
    EXPECT(rwy_anchor_set(&S, 5, 1), OK, "enroll with epoch 5");
    rwy_anchor_attach(&S, &ops);
    EXPECT(sel(&c), OK, "epoch equals the counter: select");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG2") && f.incs == 0, "CONFIG2 starts, the counter is not touched");
    EXPECT(sel(&c), OK, "CONFIG2 has no try left: select");
    CHECK(!strcmp(c.slot, "CONFIG1"), "CONFIG1 starts");
    CHECK(epoch_of() == 6 && f.value == 6 && f.incs == 1, "a slot became FAILED: epoch and counter are raised");
    seq = rseq();
    EXPECT(sel(&c), OK, "CONFIG1 is OK: select");
    CHECK(epoch_of() == 6 && f.incs == 1, "a plain boot does not raise the epoch");

    f.value = 7;
    seq = rseq();
    EXPECT(sel(&c), RWY_ERR_ROLLBACK, "counter above the epoch: select refuses");
    CHECK(!c.engaged && !c.booted && rseq() == seq, "nothing engaged, nothing written");
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    EXPECT(rwy_bootcfg_preview(&S, &c), RWY_ERR_ROLLBACK, "preview refuses it");
    EXPECT(rwy_bootcfg_fallback(&S, &c, NULL), RWY_ERR_ROLLBACK, "fallback refuses it");
    CHECK(!c.booted, "no RECOVERY: nothing to boot");
    rwy_set_actor(&S, RWY_ACTOR_USER);
    mkrecovery();
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    EXPECT(rwy_bootcfg_fallback(&S, &c, NULL), RWY_ERR_ROLLBACK, "fallback still reports the rollback");
    CHECK(c.booted && !strcmp(c.slot, "RECOVERY") && rseq() == seq, "...and offers RECOVERY, writing nothing");
    rwy_set_actor(&S, RWY_ACTOR_USER);

    f.value = 6;
    rwy_anchor_attach(&S, NULL);
    EXPECT(sel(&c), RWY_ERR_ROLLBACK, "enrolled and no counter callbacks: select refuses");
    rwy_anchor_attach(&S, &ops);

    fin(3, "INSTALLED");   /* finalize raises the epoch to 7 */
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    f.fail_inc = 1;
    EXPECT(rwy_bootcfg_condemn(&S, "CONFIG3"), OK, "condemn with the increment failing");
    CHECK(epoch_of() == 8 && f.value == 7, "a cut after the record write: epoch one ahead of the counter");
    f.fail_inc = 0;
    rwy_set_actor(&S, RWY_ACTOR_USER);
    EXPECT(sel(&c), OK, "epoch one ahead: select finishes the increment");
    CHECK(f.value == 8 && epoch_of() == 8 && !strcmp(c.slot, "CONFIG1"), "counter caught up, CONFIG1 starts");
    fin(4, "INSTALLED");   /* finalize raises the epoch to 9 */
    rwy_set_actor(&S, RWY_ACTOR_SYSTEM);
    EXPECT(rwy_bootcfg_condemn(&S, "CONFIG4"), OK, "condemn");
    rwy_set_actor(&S, RWY_ACTOR_USER);
    CHECK(epoch_of() == 10 && f.value == 10, "condemn raises the epoch and the counter");
    fclose(fl.f);
    remove(img);
}

static void test_anchor_adopt(const char *base) {
    const char *img = tpath("rwy_test_bootcfg_ana.img");
    file_t fl;
    rwy_block_dev_t dev;
    fake_t f = { 0, 0, 0, 0, 1, 0, 0, 0 };
    rwy_anchor_ops_t ops = { fake_read, fake_inc, &f };
    uint64_t seq;
    printf("== anchor: first increment of a counter enrolled offline ==\n");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    EXPECT(rwy_anchor_adopt(&S, &ops), RWY_ANCHOR_OFF, "not enrolled: nothing to do");
    EXPECT(rwy_anchor_set(&S, 0, 1), OK, "enrolled offline: epoch 0");
    seq = rseq();
    f.fail_inc = 1;
    EXPECT(rwy_anchor_adopt(&S, &ops), RWY_ERR_IO, "the increment fails");
    CHECK(epoch_of() == 0 && rseq() == seq, "the record is untouched");
    f.fail_inc = 0;
    f.value = 40;   /* the TPM starts a new counter at its high-water mark */
    f.unset = 1;
    {
        /* read answers "never incremented" until the increment, then the value */
        rwy_anchor_ops_t two = { fake_read_after_inc, fake_inc, &f };
        EXPECT(rwy_anchor_adopt(&S, &two), RWY_ANCHOR_OK, "adopt");
    }
    CHECK(epoch_of() == 41 && f.value == 41 && f.incs == 1, "one increment, the epoch is the counter");
    EXPECT(rwy_anchor_adopt(&S, &ops), RWY_ANCHOR_OK, "adopt again: nothing to do");
    CHECK(f.incs == 1, "no second increment");
    fclose(fl.f);
    remove(img);
}

static void test_anchor_raise(const char *base) {
    const char *img = tpath("rwy_test_bootcfg_anr.img");
    file_t fl;
    rwy_block_dev_t dev;
    fake_t f = { 5, 0, 0, 0, 0, 0, 0, 0 };
    rwy_anchor_ops_t ops = { fake_read, fake_inc, &f };
    printf("== anchor: finalize, remove and FAILED raise the epoch ==\n");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    fresh_subtree();
    EXPECT(rwy_anchor_set(&S, 5, 1), OK, "enroll with epoch 5");
    rwy_anchor_attach(&S, &ops);
    fin(1, "INSTALLED");
    CHECK(epoch_of() == 6 && f.value == 6 && f.incs == 1, "finalize raises the epoch and the counter once");
    set_state(1, "OK\n");
    CHECK(epoch_of() == 6 && f.incs == 1, "OK does not raise it");
    set_state(1, "FAILED\n");
    CHECK(epoch_of() == 7 && f.value == 7 && f.incs == 2, "a slot becoming FAILED raises it");
    set_state(1, "FAILED\n");
    CHECK(epoch_of() == 7 && f.incs == 2, "FAILED again does not");
    drop(1);
    CHECK(epoch_of() == 8 && f.value == 8 && f.incs == 3, "removing a slot raises it once");
    f.fail_inc = 1;
    fin(2, "INSTALLED");
    CHECK(epoch_of() == 9 && f.value == 8, "a cut after the record write: epoch one ahead of the counter");
    f.fail_inc = 0;
    rwy_anchor_attach(&S, NULL);
    fin(3, "INSTALLED");
    CHECK(epoch_of() == 9, "no counter callbacks: the epoch stays");
    EXPECT(rwy_anchor_set(&S, 9, 0), OK, "not enrolled");
    rwy_anchor_attach(&S, &ops);
    fin(4, "INSTALLED");
    CHECK(epoch_of() == 9 && f.incs == 3, "not enrolled: nothing is raised");
    fclose(fl.f);
    remove(img);
}

void test_anchor(const char *base) {
    test_anchor_raise(base);
    test_anchor_adopt(base);
    test_anchor_bind(base);
    test_anchor_rules(base);
    test_anchor_stray(base);
    test_anchor_set(base);
    test_tree_bind(base);
}
