/* Reads of directory blocks: a lookup or a listing must not read the same blocks again and again. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "bootcfgtest.h"

#define SLOTS_N 30
#define BLOBS_N 40

static uint64_t reads(void) {
    uint64_t n = 0;
    for (int k = 0; k < RWY_IO_KINDS; k++)
        for (int b = 0; b < RWY_IO_BUCKETS; b++) n += S.io.calls[0][k][b];
    return n;
}

static void fill(void) {
    char p[96];
    for (int n = 1; n <= SLOTS_N; n++) slot(n);
    for (int n = 0; n < BLOBS_N; n++) {
        snprintf(p, sizeof(p), T "/BLOBS/b%d", n);
        CHECK(cr(p) == OK, "blob made");
    }
}

/* The reads of one pass: a stat of every slot file, then a listing of the subtree and of BLOBS/. */
static uint64_t pass(void) {
    char p[96], name[RWY_NAME_MAX + 1];
    rwy_attr_t a;
    uint64_t r0 = reads();
    for (int n = 1; n <= SLOTS_N; n++) {
        snprintf(p, sizeof(p), T "/CONFIG%d/KERNEL", n);
        CHECK(rwy_stat(&S, p, &a) == OK, "stat of a slot file");
    }
    for (size_t i = 0; rwy_readdir_nth(&S, T, i, name, sizeof(name), 0) == OK; i++) {}
    for (size_t i = 0; rwy_readdir_nth(&S, T "/BLOBS", i, name, sizeof(name), 0) == OK; i++) {}
    return reads() - r0;
}

void test_dir_reads(void) {
    char name[RWY_NAME_MAX + 1];
    uint64_t first, again;
    int seen = 0;

    printf("== directory reads ==\n");
    fresh_subtree();
    fill();
    first = pass();
    again = pass();
    printf("   reads: first pass %llu, second pass %llu\n", (unsigned long long)first, (unsigned long long)again);
    CHECK(again < 400, "a second pass reads few blocks");

    /* A new name shows at once, in the transaction and after the commit */
    rwy_begin(&S);
    CHECK(rwy_create(&S, T "/BLOBS/late", RWY_S_IFREG | 0600, 1000, 1000) == OK, "late blob created");
    for (size_t i = 0; rwy_readdir_nth(&S, T "/BLOBS", i, name, sizeof(name), 0) == OK; i++) seen += !strcmp(name, "late");
    CHECK(seen == 1, "the new name is listed in the transaction");
    CHECK(rwy_commit(&S) == OK, "commit");
    seen = 0;
    for (size_t i = 0; rwy_readdir_nth(&S, T "/BLOBS", i, name, sizeof(name), 0) == OK; i++) seen += !strcmp(name, "late");
    CHECK(seen == 1, "the new name is listed after the commit");

    /* A removed name is gone, also after a remount */
    EXPECT(rm(T "/BLOBS/late"), OK, "late blob removed");
    CHECK(rwy_remount(&S) == OK, "remount");
    seen = 0;
    for (size_t i = 0; rwy_readdir_nth(&S, T "/BLOBS", i, name, sizeof(name), 0) == OK; i++) seen += !strcmp(name, "late");
    CHECK(seen == 0, "the removed name is gone after the remount");
}
