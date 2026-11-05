/*
 * rwy_slotstate.c — Layout of the state record payload (ADR 0002 N4).
 * SPDX-License-Identifier: Apache-2.0
 *
 *   0       flags     bit 0: RECOVER_NEXT requested
 *   1       reason    RWY_SS_REASON_*, 0 = none
 *   2       booted_len, 0 = none
 *   3..31   booted    name of the slot or RECOVERY, no NUL
 *   32 ...  RWY_SS_ENTRIES entries of 24 bytes: revision (0 = free), TRIES left, state,
 *                     cause of FAILED, pinned (0 or 1), 5 spare
 *   3104    anchor: epoch (8), rev_hw (8), enrolled (1), 7 spare
 *   3128    wdt_len, 0 = none; then wdt: name of the WDT application that armed last, no NUL
 *   3168    record_reset (4), 4 spare; then RWY_SS_HEALTH_MEMBERS entries of 48 bytes: member UUID (all zero
 *                     = free), a counter (4) per event, last event, 7 spare, sequence of the last event (8)
 *   3560    roster: RWY_SS_ROSTER_MEMBERS UUIDs of 16 bytes, the members the store should have (all zero = free)
 *
 * An all-zero payload is the empty record. Functions work on a payload buffer, so a caller can
 * edit a copy and write it with one rwy_record_write().
 */
#include "rwy_internal.h"

#define SS_BOOTED_MAX 29u
#define SS_ENTRY_AT 32u
#define SS_ENTRY_SIZE 24u
#define SS_ANCHOR_AT (SS_ENTRY_AT + RWY_SS_ENTRIES * SS_ENTRY_SIZE)
#define SS_ANCHOR_SIZE 24u
#define SS_WDT_AT (SS_ANCHOR_AT + SS_ANCHOR_SIZE)

#define SS_HEALTH_AT 3168u
#define SS_HEALTH_ENTRY_AT (SS_HEALTH_AT + 8u)
#define SS_HEALTH_SIZE 48u
#define SS_ROSTER_AT (SS_HEALTH_ENTRY_AT + RWY_SS_HEALTH_MEMBERS * SS_HEALTH_SIZE)

typedef char ss_fits[SS_WDT_AT + 1 + RWY_SS_WDT_MAX <= SS_HEALTH_AT ? 1 : -1];
typedef char ss_health_fits[SS_ROSTER_AT <= RWY_REC_PAYLOAD ? 1 : -1];
typedef char ss_roster_fits[SS_ROSTER_AT + RWY_SS_ROSTER_MEMBERS * 16 <= RWY_REC_PAYLOAD ? 1 : -1];

static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 8; i--;) v = v << 8 | p[i];
    return v;
}

static void put64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8; i++, v >>= 8) p[i] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p) {
    uint32_t v = 0;
    for (unsigned i = 4; i--;) v = v << 8 | p[i];
    return v;
}

static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++, v >>= 8) p[i] = (uint8_t)v;
}

static uint8_t *entry_at(uint8_t *r, unsigned i) { return r + SS_ENTRY_AT + i * SS_ENTRY_SIZE; }

/* Entry of `rev`, or the first free one when `free_ok`; NULL if there is none. */
static uint8_t *find(uint8_t *r, uint64_t rev, int free_ok) {
    uint8_t *spare = 0;
    for (unsigned i = 0; i < RWY_SS_ENTRIES; i++) {
        uint8_t *e = entry_at(r, i);
        if (get64(e) == rev) return e;
        if (free_ok && !spare && get64(e) == 0) spare = e;
    }
    return free_ok ? spare : 0;
}

int rwy_ss_recover_next(const uint8_t *r) { return r[0] & 1; }

void rwy_ss_set_recover_next(uint8_t *r, int on) { r[0] = (uint8_t)(on ? r[0] | 1 : r[0] & ~1u); }

int rwy_ss_reason(const uint8_t *r) { return r[1] <= RWY_SS_REASON_LAST ? r[1] : 0; }

void rwy_ss_set_reason(uint8_t *r, int reason) { r[1] = (uint8_t)reason; }

size_t rwy_ss_booted(const uint8_t *r, char *out) {
    size_t n = r[2] <= SS_BOOTED_MAX ? r[2] : 0;
    for (size_t i = 0; i < n; i++) out[i] = (char)r[3 + i];
    out[n] = 0;
    return n;
}

int rwy_ss_set_booted(uint8_t *r, const char *name) {
    size_t n = name ? rwy_strlen(name) : 0;
    if (n > SS_BOOTED_MAX) return RWY_ERR_INVAL;
    r[2] = (uint8_t)n;
    for (size_t i = 0; i < SS_BOOTED_MAX; i++) r[3 + i] = i < n ? (uint8_t)name[i] : 0;
    return RWY_OK;
}

size_t rwy_ss_wdt(const uint8_t *r, char *out) {
    size_t n = r[SS_WDT_AT] <= RWY_SS_WDT_MAX ? r[SS_WDT_AT] : 0;
    for (size_t i = 0; i < n; i++) out[i] = (char)r[SS_WDT_AT + 1 + i];
    out[n] = 0;
    return n;
}

int rwy_ss_set_wdt(uint8_t *r, const char *name) {
    size_t n = name ? rwy_strlen(name) : 0;
    if (n > RWY_SS_WDT_MAX) return RWY_ERR_INVAL;
    r[SS_WDT_AT] = (uint8_t)n;
    for (size_t i = 0; i < RWY_SS_WDT_MAX; i++) r[SS_WDT_AT + 1 + i] = i < n ? (uint8_t)name[i] : 0;
    return RWY_OK;
}

int rwy_ss_get(const uint8_t *r, uint64_t rev, rwy_ss_entry_t *out) {
    const uint8_t *e = rev ? find((uint8_t *)r, rev, 0) : 0;
    if (!e) return 0;
    out->tries = get64(e + 8);
    out->state = e[16] <= RWY_SS_FAILED ? e[16] : RWY_SS_FAILED;
    out->cause = out->state == RWY_SS_FAILED && e[17] <= RWY_SS_CAUSE_LAST ? e[17] : RWY_SS_CAUSE_NONE;
    out->pinned = e[18] == 1;
    return 1;
}

int rwy_ss_put(uint8_t *r, uint64_t rev, const rwy_ss_entry_t *in) {
    uint8_t *e = rev ? find(r, rev, 1) : 0;
    if (!rev) return RWY_ERR_INVAL;
    if (!e) return RWY_ERR_NOSPC;
    put64(e, rev);
    put64(e + 8, in->tries);
    for (unsigned i = 16; i < SS_ENTRY_SIZE; i++) e[i] = 0;
    e[16] = (uint8_t)in->state;
    e[17] = in->state == RWY_SS_FAILED ? in->cause : 0;
    e[18] = in->pinned ? 1 : 0;
    return RWY_OK;
}

int rwy_ss_cause_ok(const uint8_t *r, uint64_t rev) {
    const uint8_t *e = rev ? find((uint8_t *)r, rev, 0) : 0;
    return !e || e[17] == 0 || (e[16] == RWY_SS_FAILED && e[17] <= RWY_SS_CAUSE_LAST);
}

void rwy_ss_prune(uint8_t *r, const uint64_t *live, size_t n) {
    for (unsigned i = 0; i < RWY_SS_ENTRIES; i++) {
        uint8_t *e = entry_at(r, i);
        size_t k;
        for (k = 0; k < n && live[k] != get64(e); k++) {}
        if (get64(e) && k == n)
            for (unsigned j = 0; j < SS_ENTRY_SIZE; j++) e[j] = 0;
    }
}

uint64_t rwy_ss_epoch(const uint8_t *r) { return get64(r + SS_ANCHOR_AT); }

void rwy_ss_set_epoch(uint8_t *r, uint64_t v) { put64(r + SS_ANCHOR_AT, v); }

uint64_t rwy_ss_rev_hw(const uint8_t *r) { return get64(r + SS_ANCHOR_AT + 8); }

void rwy_ss_set_rev_hw(uint8_t *r, uint64_t v) { put64(r + SS_ANCHOR_AT + 8, v); }

int rwy_ss_enrolled(const uint8_t *r) { return r[SS_ANCHOR_AT + 16] & 1; }

void rwy_ss_set_enrolled(uint8_t *r, int on) { r[SS_ANCHOR_AT + 16] = (uint8_t)(on ? 1 : 0); }

static int uuid_zero(const uint8_t *u) {
    uint8_t acc = 0;
    for (unsigned i = 0; i < 16; i++) acc |= u[i];
    return acc == 0;
}

static int uuid_eq(const uint8_t *a, const uint8_t *b) {
    for (unsigned i = 0; i < 16; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static uint8_t *health_at(uint8_t *r, unsigned i) { return r + SS_HEALTH_ENTRY_AT + i * SS_HEALTH_SIZE; }

/* Entry of the member, or the first free one when `free_ok`; NULL if there is none. */
static uint8_t *health_find(uint8_t *r, const uint8_t *uuid, int free_ok) {
    uint8_t *spare = 0;
    for (unsigned i = 0; i < RWY_SS_HEALTH_MEMBERS; i++) {
        uint8_t *e = health_at(r, i);
        if (uuid_eq(e, uuid)) return e;
        if (free_ok && !spare && uuid_zero(e)) spare = e;
    }
    return free_ok ? spare : 0;
}

static void health_decode(const uint8_t *e, rwy_ss_health_t *out) {
    for (unsigned k = 0; k <= RWY_SS_EV_LAST; k++) out->count[k] = k ? get32(e + 16 + 4 * (k - 1)) : 0;
    out->last_event = e[32] <= RWY_SS_EV_LAST ? e[32] : RWY_SS_EV_NONE;
    out->seq = get64(e + 40);
}

int rwy_ss_health_get(const uint8_t *r, const uint8_t uuid[16], rwy_ss_health_t *out) {
    const uint8_t *e = uuid_zero(uuid) ? 0 : health_find((uint8_t *)r, uuid, 0);
    if (!e) return 0;
    health_decode(e, out);
    return 1;
}

int rwy_ss_health_at(const uint8_t *r, unsigned i, uint8_t uuid[16], rwy_ss_health_t *out) {
    const uint8_t *e;
    if (i >= RWY_SS_HEALTH_MEMBERS) return 0;
    e = health_at((uint8_t *)r, i);
    if (uuid_zero(e)) return 0;
    memcpy(uuid, e, 16);
    health_decode(e, out);
    return 1;
}

const char *rwy_ss_event_name(int event) {
    static const char *const names[] = { "none", "lagging", "sync_failed", "write_failed", "missing" };
    return event >= 0 && event <= RWY_SS_EV_LAST ? names[event] : "none";
}

int rwy_ss_health_note(uint8_t *r, const uint8_t uuid[16], int event, uint64_t seq) {
    uint8_t *e;
    uint32_t n;
    if (uuid_zero(uuid) || event < 1 || event > RWY_SS_EV_LAST) return RWY_ERR_INVAL;
    e = health_find(r, uuid, 1);
    if (!e) return RWY_ERR_NOSPC;
    for (unsigned i = 0; i < 16; i++) e[i] = uuid[i];
    n = get32(e + 16 + 4 * (event - 1));
    if (n != 0xffffffffu) put32(e + 16 + 4 * (event - 1), n + 1);
    e[32] = (uint8_t)event;
    put64(e + 40, seq);
    return RWY_OK;
}

void rwy_ss_health_merge(uint8_t *dst, const uint8_t *src) {
    for (unsigned i = 0; i < RWY_SS_HEALTH_MEMBERS; i++) {
        const uint8_t *s = health_at((uint8_t *)src, i);
        uint8_t *d;
        if (uuid_zero(s)) continue;
        d = health_find(dst, s, 1);
        if (!d) continue;   /* no room: the member is left out */
        if (uuid_zero(d) || get64(s + 40) > get64(d + 40)) {
            d[32] = s[32];
            put64(d + 40, get64(s + 40));
        }
        for (unsigned k = 0; k < RWY_SS_EV_LAST; k++) {
            uint32_t a = get32(d + 16 + 4 * k), b = get32(s + 16 + 4 * k);
            put32(d + 16 + 4 * k, a > b ? a : b);
        }
        for (unsigned k = 0; k < 16; k++) d[k] = s[k];
    }
    put32(dst + SS_HEALTH_AT, get32(dst + SS_HEALTH_AT) > get32(src + SS_HEALTH_AT) ? get32(dst + SS_HEALTH_AT)
                                                                                  : get32(src + SS_HEALTH_AT));
}

void rwy_ss_health_clear(uint8_t *r) {
    for (unsigned i = 0; i < RWY_SS_HEALTH_MEMBERS * SS_HEALTH_SIZE; i++) r[SS_HEALTH_ENTRY_AT + i] = 0;
}

uint32_t rwy_ss_record_reset(const uint8_t *r) { return get32(r + SS_HEALTH_AT); }

void rwy_ss_note_record_reset(uint8_t *r) {
    uint32_t n = get32(r + SS_HEALTH_AT);
    if (n != 0xffffffffu) put32(r + SS_HEALTH_AT, n + 1);
}

static uint8_t *roster_at(uint8_t *r, unsigned i) { return r + SS_ROSTER_AT + i * 16; }

int rwy_ss_roster_at(const uint8_t *r, unsigned i, uint8_t uuid[16]) {
    const uint8_t *e;
    if (i >= RWY_SS_ROSTER_MEMBERS) return 0;
    e = roster_at((uint8_t *)r, i);
    if (uuid_zero(e)) return 0;
    memcpy(uuid, e, 16);
    return 1;
}

int rwy_ss_roster_has(const uint8_t *r, const uint8_t uuid[16]) {
    for (unsigned i = 0; i < RWY_SS_ROSTER_MEMBERS; i++)
        if (uuid_eq(roster_at((uint8_t *)r, i), uuid)) return 1;
    return 0;
}

int rwy_ss_roster_add(uint8_t *r, const uint8_t uuid[16]) {
    if (uuid_zero(uuid)) return RWY_ERR_INVAL;
    if (rwy_ss_roster_has(r, uuid)) return 0;
    for (unsigned i = 0; i < RWY_SS_ROSTER_MEMBERS; i++) {
        uint8_t *e = roster_at(r, i);
        if (!uuid_zero(e)) continue;
        memcpy(e, uuid, 16);
        return 1;
    }
    return RWY_ERR_NOSPC;
}

int rwy_ss_roster_forget(uint8_t *r, const uint8_t uuid[16]) {
    for (unsigned i = 0; !uuid_zero(uuid) && i < RWY_SS_ROSTER_MEMBERS; i++) {
        uint8_t *e = roster_at(r, i);
        if (!uuid_eq(e, uuid)) continue;
        memset(e, 0, 16);
        return 1;
    }
    return 0;
}

void rwy_ss_roster_merge(uint8_t *dst, const uint8_t *src) {
    for (unsigned i = 0; i < RWY_SS_ROSTER_MEMBERS; i++) {
        const uint8_t *s = roster_at((uint8_t *)src, i);
        if (!uuid_zero(s)) (void)rwy_ss_roster_add(dst, s);   /* no room: the member is left out */
    }
}
