/*
 * loopguardtest.c — Counting rule and payload of the boot loop guard.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include "rwy_loopguard.h"

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

static const rwy_loop_guard_t G = { 5, 600 };

/* Enter the slot at `now` with the variable `*v`; apply the write the rule asks for. */
static int enter(rwy_guard_var_t *v, int *have, uint64_t rev, uint64_t now) {
    rwy_guard_var_t next;
    int r = rwy_guard_count(*have ? v : NULL, rev, now, &G, &next);
    if (r == RWY_GUARD_BOOT_WRITE) { *v = next; *have = 1; }
    return r;
}

static void test_payload(void) {
    rwy_guard_var_t v = { 0x0102030405060708ull, 7, 1700000000ull }, o;
    uint8_t b[RWY_GUARD_VAR_LEN];
    rwy_guard_var_encode(&v, b);
    CHECK(rwy_guard_var_decode(b, sizeof(b), &o), "decode");
    CHECK(o.revision == v.revision && o.count == 7 && o.first == v.first, "round trip");
    CHECK(!rwy_guard_var_decode(b, sizeof(b) - 1, &o), "short payload");
    CHECK(!rwy_guard_var_decode(b, sizeof(b) + 1, &o), "long payload");
    b[0] = 2;
    CHECK(!rwy_guard_var_decode(b, sizeof(b), &o), "other version");
}

static void test_secs(void) {
    CHECK(rwy_guard_secs(1970, 1, 1, 0, 0, 1) == 1, "epoch");
    CHECK(rwy_guard_secs(2000, 3, 1, 0, 0, 0) == 951868800ull, "after a leap day");
    CHECK(rwy_guard_secs(2024, 2, 29, 12, 30, 15) == 1709209815ull, "leap day");
    CHECK(rwy_guard_secs(2023, 2, 29, 0, 0, 0) == 0, "no leap day in 2023");
    CHECK(rwy_guard_secs(1900, 1, 1, 0, 0, 0) == 0, "before 1970");
    CHECK(rwy_guard_secs(2024, 13, 1, 0, 0, 0) == 0, "month 13");
}

static void test_count_to_limit(void) {
    rwy_guard_var_t v = { 0 };
    int have = 0, i;
    for (i = 1; i <= 5; i++) {
        CHECK(enter(&v, &have, 9, 1000 + 10u * i) == RWY_GUARD_BOOT_WRITE, "entry up to the limit boots");
        CHECK(v.count == i && v.first == 1010, "count rises, first stays");
    }
    CHECK(enter(&v, &have, 9, 1100) == RWY_GUARD_DEMOTE, "entry LIMIT + 1 demotes");
    CHECK(v.count == 5, "demotion leaves the count to the caller");
}

static void test_restart(void) {
    rwy_guard_var_t v = { 0 };
    int have = 0;
    enter(&v, &have, 9, 1000);
    enter(&v, &have, 9, 1010);
    CHECK(enter(&v, &have, 9, 1000 + 600) == RWY_GUARD_BOOT_WRITE && v.count == 1 && v.first == 1600, "window over");
    enter(&v, &have, 9, 1610);
    CHECK(enter(&v, &have, 10, 1620) == RWY_GUARD_BOOT_WRITE && v.count == 1 && v.revision == 10, "other REVISION");
    CHECK(enter(&v, &have, 10, 1000) == RWY_GUARD_BOOT_WRITE && v.count == 1 && v.first == 1000, "clock went back");
    CHECK(enter(&v, &have, 10, 1599) == RWY_GUARD_BOOT_WRITE && v.count == 2, "last second of the window");
    have = 0;
    CHECK(enter(&v, &have, 10, 1700) == RWY_GUARD_BOOT_WRITE && v.count == 1, "variable absent");
}

static void test_no_clock(void) {
    rwy_guard_var_t v = { 9, 5, 1000 }, next;
    CHECK(rwy_guard_count(&v, 9, 0, &G, &next) == RWY_GUARD_BOOT, "no clock, no demotion");
    CHECK(rwy_guard_count(NULL, 9, 0, &G, &next) == RWY_GUARD_BOOT, "no clock, no variable");
}

static void test_lowered_limit(void) {
    rwy_guard_var_t v = { 9, 200, 1000 }, next;
    CHECK(rwy_guard_count(&v, 9, 1010, &G, &next) == RWY_GUARD_DEMOTE, "count above a lowered limit");
}

static void test_max_limit(void) {
    static const rwy_loop_guard_t big = { 255, 600 };
    rwy_guard_var_t v = { 9, 254, 1000 }, next;
    CHECK(rwy_guard_count(&v, 9, 1010, &big, &next) == RWY_GUARD_BOOT_WRITE && next.count == 255, "entry 255 boots");
    v.count = 255;
    CHECK(rwy_guard_count(&v, 9, 1010, &big, &next) == RWY_GUARD_DEMOTE, "entry 256 demotes, no wrap");
    CHECK(rwy_guard_count(&v, 9, 1000, &big, &next) == RWY_GUARD_DEMOTE, "an entry at the same second counts");
}

int main(void) {
    test_payload();
    test_secs();
    test_count_to_limit();
    test_restart();
    test_no_clock();
    test_lowered_limit();
    test_max_limit();
    if (failures) {
        printf("FAILED (%d errors)\n", failures);
        return 1;
    }
    printf("ALL LOOP GUARD TESTS PASSED\n");
    return 0;
}
