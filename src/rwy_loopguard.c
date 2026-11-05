/*
 * rwy_loopguard.c — Counting rule and payload of the boot loop guard variable.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_loopguard.h"

#define VAR_VERSION 1u

static void put64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

int rwy_guard_var_decode(const uint8_t *b, size_t len, rwy_guard_var_t *v) {
    if (!b || len != RWY_GUARD_VAR_LEN || b[0] != VAR_VERSION) return 0;
    v->revision = get64(b + 1);
    v->count = b[9];
    v->first = get64(b + 10);
    return 1;
}

void rwy_guard_var_encode(const rwy_guard_var_t *v, uint8_t b[RWY_GUARD_VAR_LEN]) {
    b[0] = VAR_VERSION;
    put64(b + 1, v->revision);
    b[9] = v->count;
    put64(b + 10, v->first);
}

static unsigned is_leap(unsigned y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static unsigned month_days(unsigned year, unsigned month) {
    static const unsigned mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return mdays[month - 1] + (month == 2 && is_leap(year));
}

uint64_t rwy_guard_secs(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned min, unsigned sec) {
    uint64_t days = 0;
    unsigned i;
    if (year < 1970 || year > 2105 || month < 1 || month > 12) return 0;
    if (day < 1 || day > month_days(year, month) || hour > 23 || min > 59 || sec > 59) return 0;
    for (i = 1970; i < year; i++) days += 365 + is_leap(i);
    for (i = 1; i < month; i++) days += month_days(year, i);
    days += day - 1;
    return days * 86400 + hour * 3600u + min * 60u + sec;
}

int rwy_guard_count(const rwy_guard_var_t *old, uint64_t revision, uint64_t now,
                    const rwy_loop_guard_t *g, rwy_guard_var_t *next) {
    if (!now) return RWY_GUARD_BOOT;
    if (old && old->revision == revision && now >= old->first && now - old->first < g->window) {
        if (old->count >= g->limit) return RWY_GUARD_DEMOTE;
        *next = *old;
        next->count++;
        return RWY_GUARD_BOOT_WRITE;
    }
    next->revision = revision;
    next->count = 1;
    next->first = now;
    return RWY_GUARD_BOOT_WRITE;
}
