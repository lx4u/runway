/*
 * rwy_record.c — The state record: two blocks written in place, outside the CoW generations.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"
#include "rwy_string.h"

/* 0 if `blk` is a valid record block of `copy`. */
static int record_check(const uint8_t blk[RWY_BLOCK_SIZE], uint32_t copy) {
    const rwy_blk_header_t *h = (const rwy_blk_header_t *)blk;
    if (h->magic != RWY_BLK_MAGIC || h->kind != RWY_BLK_KIND_RECORD || h->index != copy ||
        h->generation == 0 || rwy_blk_compute_crc(blk) != h->crc32c)
        return RWY_ERR_CORRUPT;
    return RWY_OK;
}

typedef struct {
    const rwy_block_dev_t *dev;
    uint32_t rec_start;
    uint8_t blk[2][RWY_BLOCK_SIZE];
} record_scan_t;

static int record_copy_check(void *ctx, unsigned c, uint64_t *seq) {
    record_scan_t *sc = (record_scan_t *)ctx;
    int rc = rwy_blk_read(sc->dev, RWY_IO_META, sc->rec_start + c, sc->blk[c]);
    if (rc) return RWY_ERR_IO;
    rc = record_check(sc->blk[c], c);
    if (rc) return rc;
    *seq = ((rwy_blk_header_t *)sc->blk[c])->generation;
    return RWY_OK;
}

/* The newest valid copy of `dev` in `blk`, its copy in `*cur`. IO if a read failed and no copy is valid. */
static int record_best(const rwy_block_dev_t *dev, uint32_t rec_start, uint8_t blk[RWY_BLOCK_SIZE], int *cur) {
    record_scan_t sc;
    unsigned c = 0;
    int rc;
    sc.dev = dev;
    sc.rec_start = rec_start;
    rc = rwy_pick_newest(2, record_copy_check, &sc, &c);
    if (rc) return rc;
    for (size_t i = 0; i < RWY_BLOCK_SIZE; i++) blk[i] = sc.blk[c][i];
    *cur = (int)c;
    return RWY_OK;
}

static int record_load_from(rwy_store_t *s, const rwy_block_dev_t *dev) {
    uint8_t blk[RWY_BLOCK_SIZE];
    int cur = 0, rc = record_best(dev, s->sb.rec_start, blk, &cur);
    if (rc == RWY_ERR_IO) return rc;
    s->rec_damaged = 0;
    if (rc == RWY_OK) {
        s->rec_seq = ((rwy_blk_header_t *)blk)->generation;
        s->rec_cur = cur;
        for (size_t i = 0; i < RWY_REC_PAYLOAD; i++) s->rec[i] = blk[sizeof(rwy_blk_header_t) + i];
        return RWY_OK;
    }
    /* Damaged media must not make the kernels unreachable: go on with an empty record. */
    for (size_t i = 0; i < RWY_REC_PAYLOAD; i++) s->rec[i] = 0;
    rwy_ss_note_record_reset(s->rec);
    s->rec_seq = 0;
    s->rec_cur = 1;
    s->rec_damaged = 1;
    return RWY_OK;
}

int rwy_record_load(rwy_store_t *s) { return record_load_from(s, &s->dev); }

int rwy_record_adopt(rwy_store_t *s, const rwy_block_dev_t *m) {
    if (!s || !m) return RWY_ERR_INVAL;
    return record_load_from(s, m);
}

int rwy_record_damaged(const rwy_store_t *s) { return s && s->rec_damaged; }

int rwy_record_read(const rwy_store_t *s, void *buf) {
    if (!s || !buf) return RWY_ERR_INVAL;
    for (size_t i = 0; i < RWY_REC_PAYLOAD; i++) ((uint8_t *)buf)[i] = s->rec[i];
    return RWY_OK;
}

/* Write the copy that is not current and flush it. Only a flushed write is adopted. */
int rwy_record_write(rwy_store_t *s, const void *buf) {
    uint8_t blk[RWY_BLOCK_SIZE];
    rwy_blk_header_t *h = (rwy_blk_header_t *)blk;
    int next, rc;
    if (!s || !buf) return RWY_ERR_INVAL;
    next = s->rec_cur ? 0 : 1;
    for (size_t i = 0; i < sizeof(blk); i++) blk[i] = 0;
    rwy_blk_hdr_make(h, s->rec_seq + 1, (uint32_t)next, RWY_BLK_KIND_RECORD);
    for (size_t i = 0; i < RWY_REC_PAYLOAD; i++) blk[sizeof(*h) + i] = ((const uint8_t *)buf)[i];
    h->crc32c = rwy_blk_compute_crc(blk);
    rc = rwy_blk_write(&s->dev, RWY_IO_META, s->sb.rec_start + (uint32_t)next, blk);
    if (rc) return rc;
    rc = rwy_dev_flush(&s->dev);
    if (rc) return rc;
    s->rec_seq++;
    s->rec_cur = next;
    s->rec_damaged = 0;
    for (size_t i = 0; i < RWY_REC_PAYLOAD; i++) s->rec[i] = ((const uint8_t *)buf)[i];
    return RWY_OK;
}

/* Put the record `win` into the copy of `dev` that is not current, so a cut leaves the old or the new one. */
static void record_heal(const rwy_block_dev_t *dev, uint32_t rec_start, const uint8_t win[RWY_BLOCK_SIZE]) {
    uint8_t blk[RWY_BLOCK_SIZE], cur_blk[RWY_BLOCK_SIZE];
    rwy_blk_header_t *h = (rwy_blk_header_t *)blk;
    int cur = 1;
    if (record_best(dev, rec_start, cur_blk, &cur) == RWY_ERR_IO) return;
    for (size_t i = 0; i < sizeof(blk); i++) blk[i] = win[i];
    h->index = cur ? 0 : 1;
    h->crc32c = rwy_blk_compute_crc(blk);
    if (rwy_blk_write(dev, RWY_IO_META, rec_start + h->index, blk)) return;
    rwy_dev_flush(dev);
}

int rwy_record_sync(const rwy_block_dev_t *m, size_t n, uint32_t rec_start, int heal, size_t *newest) {
    uint8_t win[RWY_BLOCK_SIZE], blk[RWY_BLOCK_SIZE];
    uint64_t seq[RWY_MIRROR_MAX_DEVS], best = 0;
    size_t i, w = 0;
    if (!m || n == 0 || n > RWY_MIRROR_MAX_DEVS) return RWY_ERR_INVAL;
    for (i = 0; i < n; i++) {
        int cur;
        seq[i] = 0;
        if (record_best(&m[i], rec_start, blk, &cur)) continue;
        seq[i] = ((rwy_blk_header_t *)blk)->generation;
        if (seq[i] > best) {
            best = seq[i];
            w = i;
            for (size_t k = 0; k < sizeof(win); k++) win[k] = blk[k];
        }
    }
    if (newest) *newest = w;
    if (heal && best)
        for (i = 0; i < n; i++)
            if (seq[i] < best) record_heal(&m[i], rec_start, win);
    return RWY_OK;
}

/* Health counters and roster of every member go into the record of the store, so a member that lost the race does not lose them. */
int rwy_record_merge_health(rwy_store_t *s, const rwy_block_dev_t *m, size_t n) {
    uint8_t blk[RWY_BLOCK_SIZE];
    if (!s || !m || n == 0 || n > RWY_MIRROR_MAX_DEVS) return RWY_ERR_INVAL;
    for (size_t i = 0; i < n; i++) {
        int cur;
        if (record_best(&m[i], s->sb.rec_start, blk, &cur)) continue;
        rwy_ss_health_merge(s->rec, blk + sizeof(rwy_blk_header_t));
        rwy_ss_roster_merge(s->rec, blk + sizeof(rwy_blk_header_t));
    }
    return RWY_OK;
}

int rwy_record_note_health(rwy_store_t *s, const rwy_block_dev_t *devs, size_t n, const uint8_t uuid[16], int event) {
    uint8_t r[RWY_REC_PAYLOAD];
    int rc = RWY_OK;
    if (!s) return RWY_ERR_INVAL;
    if (devs) rc = rwy_record_merge_health(s, devs, n);
    if (!rc) rc = rwy_record_read(s, r);
    if (!rc) rc = rwy_ss_health_note(r, uuid, event, s->rec_seq + 1);
    return rc ? rc : rwy_record_write(s, r);
}

int rwy_record_note_roster(rwy_store_t *s, const rwy_block_dev_t *devs, size_t n, const uint8_t (*uuids)[16], size_t count) {
    uint8_t r[RWY_REC_PAYLOAD];
    int rc = RWY_OK, added = 0;
    if (!s) return RWY_ERR_INVAL;
    if (count < 2) return RWY_OK;
    if (devs) rc = rwy_record_merge_health(s, devs, n);
    if (!rc) rc = rwy_record_read(s, r);
    for (size_t i = 0; !rc && i < count; i++) {
        int a = rwy_ss_roster_add(r, uuids[i]);
        if (a < 0) rc = a;
        else added |= a;
    }
    return rc || !added ? rc : rwy_record_write(s, r);
}

int rwy_record_forget_member(rwy_store_t *s, const uint8_t uuid[16]) {
    uint8_t r[RWY_REC_PAYLOAD];
    int rc;
    if (!s) return RWY_ERR_INVAL;
    rc = rwy_record_read(s, r);
    if (rc) return rc;
    if (!rwy_ss_roster_forget(r, uuid)) return RWY_ERR_NOENT;
    return rwy_record_write(s, r);
}

int rwy_record_clear_health(rwy_store_t *s) {
    uint8_t r[RWY_REC_PAYLOAD];
    int rc;
    if (!s) return RWY_ERR_INVAL;
    rc = rwy_record_read(s, r);
    if (rc) return rc;
    rwy_ss_health_clear(r);
    return rwy_record_write(s, r);
}

static int uuid_named(const uint8_t u[16]) {
    uint8_t acc = 0;
    for (size_t i = 0; i < 16; i++) acc |= u[i];
    return acc != 0;
}

int rwy_mirror_note_failures(rwy_store_t *s, rwy_mirror_dev_t *m, const rwy_block_dev_t *devs, size_t n) {
    int hit[RWY_MIRROR_MAX_DEVS], first_err = 0;
    size_t i;
    if (!s || !m) return RWY_ERR_INVAL;
    for (i = 0; i < m->num_devs; i++) hit[i] = !m->dev_faulted[i] && m->write_errors[i] > m->write_noted[i];
    for (i = 0; i < m->num_devs; i++) {
        uint8_t uuid[16];
        int rc;
        if (!hit[i] || !rwy_block_member_uuid(&m->devs[i], uuid) || !uuid_named(uuid)) continue;
        rc = rwy_record_note_health(s, devs, n, uuid, RWY_SS_EV_WRITE_FAILED);
        if (rc && !first_err) first_err = rc;
    }
    for (i = 0; i < m->num_devs; i++) m->write_noted[i] = m->write_errors[i];
    return first_err;
}

static int uuid_listed(const uint8_t (*list)[16], size_t n, const uint8_t uuid[16]) {
    for (size_t i = 0; i < n; i++)
        if (!CompareMem(list[i], uuid, 16)) return 1;
    return 0;
}

int rwy_mirror_note_members(rwy_store_t *s, const rwy_mirror_dev_t *m, const rwy_block_dev_t *devs, size_t n) {
    uint8_t here[RWY_MIRROR_MAX_DEVS][16], r[RWY_REC_PAYLOAD], uuid[16];
    size_t count = 0;
    int rc, first_err;
    if (!s || !m) return RWY_ERR_INVAL;
    for (size_t i = 0; i < m->num_devs; i++)
        if (rwy_block_member_uuid(&m->devs[i], here[count]) && uuid_named(here[count])) count++;
    first_err = rwy_record_note_roster(s, devs, n, here, count);
    if (!first_err && devs) first_err = rwy_record_merge_health(s, devs, n);
    if (!first_err) first_err = rwy_record_read(s, r);
    if (first_err) return first_err;
    for (unsigned i = 0; i < RWY_SS_ROSTER_MEMBERS; i++) {
        if (!rwy_ss_roster_at(r, i, uuid) || uuid_listed(here, count, uuid)) continue;
        rc = rwy_record_note_health(s, devs, n, uuid, RWY_SS_EV_MISSING);
        if (rc && !first_err) first_err = rc;
    }
    return first_err;
}
