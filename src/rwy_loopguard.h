/*
 * rwy_loopguard.h — The counting rule of the boot loop guard, without any firmware call. Internal.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_LOOPGUARD_H
#define RWY_LOOPGUARD_H

#include <stddef.h>
#include <stdint.h>
#include "rwy_core.h"

/* Payload of the RunwayBootGuard variable: version byte, REVISION (8, little endian), count, first (8). */
#define RWY_GUARD_VAR_LEN 18u

typedef struct {
    uint64_t revision;   /* REVISION of the slot that is counted */
    uint8_t  count;      /* entries seen inside the window */
    uint64_t first;      /* seconds of the first entry */
} rwy_guard_var_t;

/* 1 when `b` is a valid payload, else 0 (wrong size, other version). */
int rwy_guard_var_decode(const uint8_t *b, size_t len, rwy_guard_var_t *v);
void rwy_guard_var_encode(const rwy_guard_var_t *v, uint8_t b[RWY_GUARD_VAR_LEN]);

/* Seconds since 1970 for a UTC date and time; 0 outside 1970..2105 or for an impossible field. */
uint64_t rwy_guard_secs(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned min, unsigned sec);

enum {
    RWY_GUARD_BOOT,         /* boot the slot, the variable stays as it is */
    RWY_GUARD_BOOT_WRITE,   /* boot the slot after writing `next` */
    RWY_GUARD_DEMOTE        /* the limit is reached: demote the slot */
};

/* The rule for an entry into the OK slot `revision`. `old` is the variable, NULL when it is absent or damaged.
 * `now` is the clock in seconds, or 0 when it cannot be read: nothing is counted then. */
int rwy_guard_count(const rwy_guard_var_t *old, uint64_t revision, uint64_t now,
                    const rwy_loop_guard_t *g, rwy_guard_var_t *next);

#endif
