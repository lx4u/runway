/* check of BLOBS/: orphans, staging files, content that is not the name, and the slots marked bad_blob. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "bootcfgtest.h"
#include "rwy_blob.h"
#include "rwy_crypto.h"

#define FAKE "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
#define UPPER "00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF"

static char lines[1024];

static void note(const char *s) {
    strncat(lines, s, sizeof(lines) - strlen(lines) - 1);
    strncat(lines, "\n", sizeof(lines) - strlen(lines) - 1);
}

static void on_stale(void *ctx, int kind, const char *path) {
    char b[160];
    (void)ctx;
    snprintf(b, sizeof(b), "%s %s", kind == RWY_BLOB_ORPHAN ? "orphan" : "staged", path);
    note(b);
}

static void on_bad(void *ctx, const char *name, const char *actual, const char *slot) {
    char b[200];
    (void)ctx;
    if (slot) snprintf(b, sizeof(b), "slot %s %s", slot, name);
    else snprintf(b, sizeof(b), "bad %s is %s", name, actual);
    note(b);
}

static uint32_t nlinks_of(const char *p) {
    rwy_attr_t a;
    return rwy_stat(&S, p, &a) == OK ? a.nlink : 0;
}

static int seen(const char *sub) { return strstr(lines, sub) != NULL; }

/* The content name of `data`, the way an install makes it. */
static void name_of(const char *data, char out[RWY_BLOB_NAME_SIZE]) {
    rwy_sha256_stream_t h;
    uint8_t d[32];
    rwy_sha256_stream_init(&h);
    rwy_sha256_stream_update(&h, data, strlen(data));
    rwy_sha256_stream_final(&h, d);
    rwy_blob_name(d, out);
}

/* A blob under `name` with `data`: staged and renamed by SYSTEM. */
static void make_blob(const char *name, const char *data) {
    char p[160];
    snprintf(p, sizeof(p), T "/BLOBS/%s", name);
    as_sys();
    CHECK(cr(T "/BLOBS/stage") == OK && wr(T "/BLOBS/stage", 0, data) == OK && mv(T "/BLOBS/stage", p) == OK, "blob made");
    as_user();
}

/* A finalized slot whose KERNEL links the blob `name`. */
static void blob_slot(int n, const char *name) {
    char d[64], k[64], st[64], b[160];
    snprintf(d, sizeof(d), T "/CONFIG%d", n);
    snprintf(k, sizeof(k), T "/CONFIG%d/KERNEL", n);
    snprintf(st, sizeof(st), T "/CONFIG%d/STATE", n);
    snprintf(b, sizeof(b), T "/BLOBS/%s", name);
    CHECK(mk(d) == OK && ln(b, k) == OK && cr(st) == OK && install(n) == OK, "slot with a linked kernel");
}

static void verify_cases(void) {
    char good[RWY_BLOB_NAME_SIZE], again[RWY_BLOB_NAME_SIZE];
    uint64_t g;

    name_of("good kernel", good);
    name_of("not what the name says", again);
    make_blob(good, "good kernel");
    make_blob(FAKE, "not what the name says");
    blob_slot(1, good);
    blob_slot(2, FAKE);
    blob_slot(3, FAKE);
    make_blob(UPPER, "a plain name, not a content name");

    lines[0] = 0;
    CHECK(rwy_blob_verify(&S, 0, on_bad, 0) == 1, "one bad blob");
    CHECK(seen("bad " FAKE " is ") && seen(again) && !seen("slot ") && !seen(good), "the blob is named with its real hash");
    g = gen();
    CHECK(text_is(T "/CONFIG2/STATE", "INSTALLED\n"), "a check without repair leaves the slot");
    EXPECT(rwy_blob_verify(&S, 1, on_bad, 0), PERM, "USER cannot repair");

    as_sys();
    lines[0] = 0;
    CHECK(rwy_blob_verify(&S, 1, on_bad, 0) == 1, "repair finds the same blob");
    as_user();
    CHECK(seen("slot CONFIG2 " FAKE) && seen("slot CONFIG3 " FAKE) && !seen("slot CONFIG1"), "both slots of the blob are marked");
    CHECK(text_is(T "/CONFIG2/STATE", "FAILED\n") && text_is(T "/CONFIG2/CAUSE", "bad_blob\n"), "CONFIG2 FAILED, cause bad_blob");
    CHECK(text_is(T "/CONFIG3/CAUSE", "bad_blob\n") && text_is(T "/CONFIG1/STATE", "INSTALLED\n"), "the slot of the good blob is left");
    CHECK(gen() == g, "the tree did not change");
    CHECK(text_is(T "/BLOBS/" FAKE, "not what the name says"), "the blob is not rewritten");

    as_sys();
    lines[0] = 0;
    CHECK(rwy_blob_verify(&S, 1, on_bad, 0) == 1 && !seen("slot "), "a second repair marks nothing new");
    as_user();
    set_state(3, "INSTALLED\n");
}

static void stale_cases(void) {
    char live[RWY_BLOB_NAME_SIZE];
    uint64_t g;

    name_of("a live kernel", live);
    make_blob(live, "a live kernel");
    blob_slot(1, live);
    make_blob(UPPER, "a plain name, not a content name");
    lines[0] = 0;
    CHECK(rwy_blob_stale(&S, on_stale, 0) == 0 && !lines[0], "a linked blob and a plain name are not stale");

    make_blob(FAKE, "nobody links this");
    CHECK(cr(T "/BLOBS/stage.rwyctl") == OK && cr(T "/BLOBS/.staging%2Fa.1.x") == OK, "a stage file and a bridge staging file");
    lines[0] = 0;
    CHECK(rwy_blob_stale(&S, on_stale, 0) == 3, "three stale things");
    CHECK(seen("orphan BLOBS/" FAKE) && seen("staged BLOBS/stage.rwyctl") && seen("staged BLOBS/.staging%2Fa.1.x") && !seen(live),
          "each one is named");

    EXPECT(rwy_blob_stale_remove(&S), PERM, "USER cannot remove them");
    CHECK(rwy_blob_staging_remove(&S) == 2, "USER removes the two staging files and leaves the orphan");
    CHECK(rwy_blob_stale(&S, 0, 0) == 1, "the orphan is left");
    CHECK(cr(T "/BLOBS/stage.rwyctl") == OK && cr(T "/BLOBS/.staging%2Fa.1.x") == OK, "two staging files again");
    as_sys();
    CHECK(rwy_blob_stale_remove(&S) == 3, "SYSTEM removes the three in one commit");
    CHECK(rwy_blob_stale(&S, 0, 0) == 0, "nothing is left");
    g = gen();
    CHECK(rwy_blob_stale_remove(&S) == 0 && gen() == g, "nothing to remove: no commit");
    as_user();
    CHECK(nlinks_of(T "/BLOBS/" FAKE) == 0, "the orphan is gone");
}

typedef void (*fixture_fn)(void);
typedef int (*repair_fn)(void);
typedef void (*state_fn)(void);

/* A store with an orphan blob, a stage file and a bridge staging file. */
static void stale_fixture(void) {
    make_blob(FAKE, "nobody links this");
    CHECK(cr(T "/BLOBS/stage.rwyctl") == OK && cr(T "/BLOBS/.staging%2Fa.1.x") == OK, "staging fixture");
}

static int op_stale_remove(void) { return rwy_blob_stale_remove(&S); }

static void state_stale_remove(void) {
    int left = rwy_blob_stale(&S, 0, 0);
    CHECK(left == 0 || left == 3, "all the stale things are there, or none");
}

/* A store with a blob that is not its name, linked by two slots, and a good slot. */
static void bad_fixture(void) {
    char good[RWY_BLOB_NAME_SIZE];

    name_of("good kernel", good);
    make_blob(good, "good kernel");
    make_blob(FAKE, "not what the name says");
    blob_slot(1, good);
    blob_slot(2, FAKE);
    blob_slot(3, FAKE);
}

static int op_verify_repair(void) { return rwy_blob_verify(&S, 1, 0, 0); }

/* Each slot of the bad blob is INSTALLED without a cause, or FAILED with bad_blob; the good slot is never touched. */
static void state_verify_repair(void) {
    int n;

    for (n = 2; n <= 3; n++) {
        char st[64], ca[64];
        snprintf(st, sizeof(st), T "/CONFIG%d/STATE", n);
        snprintf(ca, sizeof(ca), T "/CONFIG%d/CAUSE", n);
        CHECK((text_is(st, "INSTALLED\n") && nlinks_of(ca) == 0) || (text_is(st, "FAILED\n") && text_is(ca, "bad_blob\n")),
              "a slot of the bad blob is untouched or marked whole");
    }
    CHECK(text_is(T "/CONFIG1/STATE", "INSTALLED\n"), "the slot of the good blob is left");
    CHECK(text_is(T "/BLOBS/" FAKE, "not what the name says"), "the blob is not rewritten");
}

/* Run `op` as SYSTEM with the Nth write or flush failing; `state` looks at every crash state. */
static void fi_repair(const char *base, const char *what, fixture_fn fixture, repair_fn op, state_fn state) {
    const char *img = tpath("rwy_fi_blob_base.img"), *crash = tpath("rwy_fi_blob_crash.img");
    file_t fl;
    rwy_block_dev_t inner, dev, d;
    fi_t fi;
    long pts = 0;

    printf("== fault injection: %s ==\n", what);
    copy_file(base, img);
    open_store(&S, &d, &fl, img);
    fresh_subtree();
    fixture();
    fclose(fl.f);
    inner.read = file_rd; inner.write = file_wr; inner.flush = file_fl; inner.ctx = &fl;
    inner.size = RWY_STORE_SIZE; inner.block_size = RWY_BLOCK_SIZE;
    dev.read = fi_rd; dev.write = fi_wr; dev.flush = fi_fl; dev.ctx = &fi;
    dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
    for (long pt = 0; pt <= 400; pt++) {
        copy_file(img, crash);
        fl.f = fopen(crash, "r+b");
        fi.inner = inner; fi.point = 0; fi.fail_at = pt ? pt : -1;
        if (rwy_mount(&S, &dev) == OK) {
            as_sys();
            (void)op();
            as_user();
        }
        if (!pt) pts = fi.point;
        fclose(fl.f);
        if (pt > pts && pts) break;
        fl.f = fopen(crash, "r+b");
        fi.fail_at = -1;
        CHECK(rwy_mount(&S, &inner) == OK, "remount after fault");
        state();
        fclose(fl.f);
        CHECK(run_checker(crash) == 0, "checker passes on crash state");
    }
    CHECK(pts > 0, "counted persistence points");
}

static void on_file(const char *base) {
    const char *img = tpath("rwy_test_blobcheck.img");
    file_t fl;
    rwy_block_dev_t d;

    printf("== check of BLOBS/ ==\n");
    copy_file(base, img);
    open_store(&S, &d, &fl, img);
    fresh_subtree();
    verify_cases();
    fclose(fl.f);
    copy_file(base, img);
    open_store(&S, &d, &fl, img);
    fresh_subtree();
    stale_cases();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes");
    fi_repair(base, "removal of orphans and staging files", stale_fixture, op_stale_remove, state_stale_remove);
    fi_repair(base, "marking of the slots of a bad blob", bad_fixture, op_verify_repair, state_verify_repair);
}

void test_blob_check(const char *base) {
    on_file(base);
}
