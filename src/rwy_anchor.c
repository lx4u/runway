/*
 * rwy_anchor.c — Check the record's epoch against an outside monotonic counter.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

/* A record that is not enrolled: a counter that reads anyway is stray, unless the store is plain. */
static int check_not_enrolled(const rwy_store_t *s, const rwy_anchor_ops_t *ops) {
    uint64_t counter;
    int r;
    if (!ops || !ops->read || s->sb.crypt_algo != RWY_CRYPT_AES_XTS_256) return RWY_ANCHOR_OFF;
    r = ops->read(ops->ctx, &counter);
    return r == RWY_OK || r == RWY_ANCHOR_READ_UNSET || r == RWY_ANCHOR_READ_DENIED ? RWY_ANCHOR_STRAY : RWY_ANCHOR_OFF;
}

int rwy_anchor_check(const rwy_store_t *s, const rwy_anchor_ops_t *ops) {
    uint64_t epoch = rwy_ss_epoch(s->rec), counter;
    int r;
    if (!rwy_ss_enrolled(s->rec)) return check_not_enrolled(s, ops);
    if (!ops || !ops->read) return RWY_ANCHOR_MISSING;
    r = ops->read(ops->ctx, &counter);
    if (r == RWY_ANCHOR_READ_UNSET) return RWY_ANCHOR_NEW;
    if (r) return RWY_ANCHOR_MISSING;
    if (epoch == counter) return RWY_ANCHOR_OK;
    if (epoch < counter) return RWY_ANCHOR_BEHIND;
    if (epoch > counter + 1) return RWY_ANCHOR_AHEAD;
    if (!ops->increment) return RWY_ANCHOR_MISSING;
    return ops->increment(ops->ctx) ? RWY_ERR_IO : RWY_ANCHOR_FINISHED;
}

int rwy_anchor_adopt(rwy_store_t *s, const rwy_anchor_ops_t *ops) {
    uint64_t v;
    int r = rwy_anchor_check(s, ops);
    if (r != RWY_ANCHOR_NEW || rwy_ss_epoch(s->rec) != 0) return r;
    if (!ops->increment || ops->increment(ops->ctx) || ops->read(ops->ctx, &v)) return RWY_ERR_IO;
    r = rwy_anchor_set(s, v, 1);
    return r ? r : RWY_ANCHOR_OK;
}

uint32_t rwy_anchor_index(const rwy_store_t *s) {
    return 0x01800000u | (rwy_crc32c(s->sb.uuid, 16) & 0x3FFFFFu);
}

void rwy_anchor_get(const rwy_store_t *s, uint64_t *epoch, int *enrolled) {
    *epoch = rwy_ss_epoch(s->rec);
    *enrolled = rwy_ss_enrolled(s->rec);
}

int rwy_anchor_set(rwy_store_t *s, uint64_t epoch, int enrolled) {
    uint8_t r[RWY_REC_PAYLOAD];
    (void)rwy_record_read(s, r);
    rwy_ss_set_epoch(r, epoch);
    rwy_ss_set_enrolled(r, enrolled);
    return rwy_record_write(s, r);
}

void rwy_anchor_attach(rwy_store_t *s, const rwy_anchor_ops_t *ops) {
    s->anchor_ops = ops;
}

/* 1 when the anchor says the store is not the one the counter vouches for. May finish an increment. */
int rwy_anchor_refused(const rwy_store_t *s) {
    int r = rwy_anchor_check(s, s->anchor_ops);
    return r != RWY_ANCHOR_OFF && r != RWY_ANCHOR_OK && r != RWY_ANCHOR_FINISHED;
}

/* Put epoch + 1 in the record image `rec` of an enrolled store; the counter follows once the record is written. */
int rwy_anchor_raise_rec(const rwy_store_t *s, uint8_t *rec) {
    if (!rwy_ss_enrolled(rec) || !s->anchor_ops) return 0;
    rwy_ss_set_epoch(rec, rwy_ss_epoch(rec) + 1);
    return 1;
}

/* Raise the counter after the record with the new epoch is on the disk. A failure is left to the next mount. */
void rwy_anchor_raised(const rwy_store_t *s) {
    if (s->anchor_ops && s->anchor_ops->increment) (void)s->anchor_ops->increment(s->anchor_ops->ctx);
}
