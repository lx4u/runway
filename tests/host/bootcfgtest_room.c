/* Power cut at every write of the make-room sequence: the eviction commit, then the install commit. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "bootcfgtest.h"

#define PAYLOAD_BLOCKS 700u
#define NSLOTS         12
#define PAYLOAD_BYTES  (PAYLOAD_BLOCKS * 4096u)

static char payload[PAYLOAD_BYTES];
static uint64_t old_size;   /* the KERNEL size of the slots made by the fixture */

/* A slot with a big KERNEL, finalized, then set to `state`. */
static void big_slot(int n, const char *state) {
    char k[64];
    slot(n);
    snprintf(k, sizeof(k), T "/CONFIG%d/KERNEL", n);
    EXPECT(wr(k, 1, payload), OK, "payload");
    EXPECT(install(n), OK, "install");
    if (strcmp(state, "INSTALLED")) { char v[16]; snprintf(v, sizeof(v), "%s\n", state); set_state(n, v); }
}

/* The largest QUOTA that leaves less room than the new slot needs. */
static unsigned tight_quota(uint32_t need) {
    uint32_t used, usable;
    unsigned pct;
    rwy_pool_usage(&S, &used, &usable);
    for (pct = 95; pct > 10; pct--)
        if ((uint64_t)usable * pct / 100 < (uint64_t)used + need) break;
    return pct;
}

/* What `rwyctl update` does with QUOTA: make room, then stage the new slot in one commit. */
static int update_slot(rwy_store_t *s, int n) {
    char d[64], k[64], st[64];
    int rc;
    snprintf(d, sizeof(d), T "/CONFIG%d", n);
    snprintf(k, sizeof(k), T "/CONFIG%d/KERNEL", n);
    snprintf(st, sizeof(st), T "/CONFIG%d/STATE", n);
    if ((rc = rwy_begin(s))) return rc;
    if ((rc = rwy_bootcfg_make_room(s, PAYLOAD_BLOCKS + RWY_SLOT_META_BLOCKS, PAYLOAD_BLOCKS))) return rc;
    if ((rc = rwy_mkdir(s, d, RWY_S_IFDIR | 0700, 1000, 1000)) || (rc = rwy_create(s, k, RWY_S_IFREG | 0600, 1000, 1000)) ||
        (rc = rwy_write_path(s, k, 0, payload, sizeof(payload))) || (rc = rwy_create(s, st, RWY_S_IFREG | 0600, 1000, 1000)) ||
        (rc = rwy_write_path(s, st, 0, "INSTALLED\n", 10))) {
        rwy_abort(s);
        return rc;
    }
    return rwy_commit(s);
}

static int kernel_size(int n, uint64_t *size) {
    char k[64];
    rwy_attr_t a;
    int rc;
    snprintf(k, sizeof(k), T "/CONFIG%d/KERNEL", n);
    rc = rwy_stat(&S, k, &a);
    *size = a.size;
    return rc;
}

static void check_after_cut(long pt, long pts, int newest) {
    rwy_boot_choice_t c;
    uint64_t size, new_size;
    int new_slot = kernel_size(NSLOTS + 1, &new_size) == OK;
    CHECK(kernel_size(newest, &size) == OK && size == old_size, "the newest known good slot is intact");
    CHECK(!new_slot || new_size == PAYLOAD_BYTES, "a new slot is complete or absent");
    CHECK(sel(&c) == OK, "a boot selection works after the cut");
    CHECK(booted_is(new_slot ? "CONFIG13" : "CONFIG12"), "the boot picks the new slot or the newest known good one");
    if (failures) printf("  at point %ld of %ld\n", pt, pts);
}

void fi_make_room(const char *base) {
    const char *img = tpath("rwy_fi_room_base.img"), *cr_img = tpath("rwy_fi_room_crash.img");
    file_t fl;
    rwy_block_dev_t inner, dev, d2;
    fi_t fi;
    long pts = 0;
    char q[16];
    printf("== fault injection: make room and install under QUOTA ==\n");
    memset(payload, 'x', sizeof(payload));
    copy_file(base, img);
    open_store(&S, &d2, &fl, img);
    fresh_subtree();
    for (int n = 1; n <= NSLOTS; n++) big_slot(n, "OK");
    snprintf(q, sizeof(q), "%u\n", tight_quota(PAYLOAD_BLOCKS + RWY_SLOT_META_BLOCKS));
    CHECK(kernel_size(NSLOTS, &old_size) == OK, "fixture kernel");
    CHECK(cr(T "/QUOTA") == OK && wr(T "/QUOTA", 0, q) == OK, "QUOTA");
    fclose(fl.f);
    inner.read = file_rd; inner.write = file_wr; inner.flush = file_fl; inner.ctx = &fl;
    inner.size = RWY_STORE_SIZE; inner.block_size = RWY_BLOCK_SIZE;
    dev.read = fi_rd; dev.write = fi_wr; dev.flush = fi_fl; dev.ctx = &fi;
    dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
    for (long pt = 0; pt <= 600; pt++) {
        rwy_store_t s;
        uint64_t size;
        int rc;
        copy_file(img, cr_img);
        fl.f = fopen(cr_img, "r+b");
        fi.inner = inner; fi.point = 0; fi.fail_at = pt ? pt : -1;
        if (rwy_mount(&s, &dev) == OK) {
            rwy_set_actor(&s, RWY_ACTOR_USER);
            rc = update_slot(&s, NSLOTS + 1);
            if (!pt) EXPECT(rc, OK, "the sequence works without a cut");
        }
        if (!pt) pts = fi.point;
        fclose(fl.f);
        if (!pts || pt > pts) break;
        fl.f = fopen(cr_img, "r+b");
        fi.fail_at = -1;
        CHECK(rwy_mount(&S, &inner) == OK, "remount after the cut");
        if (!pt) CHECK(kernel_size(1, &size) != OK && kernel_size(NSLOTS + 1, &size) == OK, "the oldest slot went and the new one is in");
        check_after_cut(pt, pts, NSLOTS);
        fclose(fl.f);
        CHECK(run_checker(cr_img) == 0, "checker passes on crash state");
    }
    CHECK(pts > 0, "counted persistence points");
}

#define SPLIT_MAX_BYTES (650u * 4096u)

static char split_data[SPLIT_MAX_BYTES + 8];

/* A slot with a KERNEL of `blocks` blocks of one letter per slot, then set to `state`. */
static void split_slot(int n, uint32_t blocks, const char *state) {
    char k[64];
    slot(n);
    snprintf(k, sizeof(k), T "/CONFIG%d/KERNEL", n);
    memset(split_data, 'a' + n, blocks * 4096u);
    split_data[blocks * 4096u] = 0;
    EXPECT(wr(k, 1, split_data), OK, "payload");
    EXPECT(install(n), OK, "install");
    if (strcmp(state, "INSTALLED")) { char v[16]; snprintf(v, sizeof(v), "%s\n", state); set_state(n, v); }
}

/* Take every free block but a few, from the tail run. */
static void fill_store(void) {
    rwy_frag_info_t fi;
    char *f;
    size_t n;
    CHECK(rwy_frag_info(&S, &fi) == OK && fi.free_blocks > 1000, "frag info before the filler");
    n = ((size_t)fi.free_blocks - 150u) * 4096u;
    f = malloc(n + 1);
    memset(f, 'f', n);
    f[n] = 0;
    seed(T "/BLOBS/filler", f);
    free(f);
}

/* The KERNEL of slot n still has `blocks` blocks of the letter split_slot wrote. */
static int split_kernel_intact(int n, uint32_t blocks) {
    char k[64];
    size_t len = sizeof(split_data);
    uint64_t o;
    snprintf(k, sizeof(k), T "/CONFIG%d/KERNEL", n);
    if (rwy_resolve_ext(&S, k, 0, 1, &o) || rwy_read(&S, o, 1, split_data, &len) || len != blocks * 4096u) return 0;
    for (size_t i = 0; i < len; i++)
        if (split_data[i] != 'a' + n) return 0;
    return 1;
}

/* The slot that stays is split in two extents, around the one that goes: make_room moves it into the hole. */
void test_make_room_compacts(const char *base) {
    const char *img = tpath("rwy_room_split.img");
    file_t fl;
    rwy_block_dev_t dev;
    rwy_frag_info_t before, after;
    int rc;

    printf("== make room: compact a split slot after an eviction ==\n");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    fresh_subtree();
    split_slot(1, 300, "FAILED");
    split_slot(2, 650, "FAILED");
    split_slot(3, 300, "FAILED");
    fill_store();
    drop(1);
    drop(3);
    split_slot(4, 600, "OK");   /* first fit: one extent in each hole */
    EXPECT(cr(T "/SLOTS"), OK, "SLOTS");
    EXPECT(wr(T "/SLOTS", 0, "2\n"), OK, "SLOTS value");
    CHECK(rwy_frag_info(&S, &before) == OK && before.frag_objects == 1, "the slot that stays is split");

    rwy_begin(&S);
    rc = rwy_bootcfg_make_room(&S, PAYLOAD_BLOCKS + RWY_SLOT_META_BLOCKS, PAYLOAD_BLOCKS);
    EXPECT(rc, OK, "make room");
    if (!rc) rwy_commit(&S);

    CHECK(rwy_frag_info(&S, &after) == OK, "frag info after");
    CHECK(after.frag_objects == 0, "the split slot is in one run");
    CHECK(after.free_blocks >= PAYLOAD_BLOCKS + RWY_SLOT_META_BLOCKS, "the eviction made room");
    CHECK(!split_kernel_intact(2, 650), "the evicted slot is gone");
    CHECK(split_kernel_intact(4, 600), "the moved slot is intact");
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after the compaction");
}
