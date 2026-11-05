/*
 * rwy_mirror.c — Multi-device mirror block layer (rwy_block_dev_t wrapper).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

int rwy_mirror_init(rwy_mirror_dev_t *mdev) {
    uint8_t *p;
    size_t i;
    if (!mdev) return RWY_MIRROR_ERR_INVAL;
    p = (uint8_t *)mdev;
    for (i = 0; i < sizeof(*mdev); i++) p[i] = 0;
    mdev->primary_idx = 0;
    mdev->write_policy = RWY_MIRROR_WRITE_BEST_EFFORT;
    return RWY_MIRROR_OK;
}

int rwy_mirror_add(rwy_mirror_dev_t *mdev, const rwy_block_dev_t *dev) {
    if (!mdev || !dev) return RWY_MIRROR_ERR_INVAL;
    if (mdev->num_devs >= RWY_MIRROR_MAX_DEVS) return RWY_MIRROR_ERR_FULL;
    mdev->devs[mdev->num_devs] = *dev;
    mdev->num_devs++;
    return RWY_MIRROR_OK;
}

int rwy_mirror_set_primary(rwy_mirror_dev_t *mdev, size_t primary_idx) {
    if (!mdev) return RWY_MIRROR_ERR_INVAL;
    if (primary_idx >= mdev->num_devs) return RWY_MIRROR_ERR_INVAL;
    mdev->primary_idx = primary_idx;
    return RWY_MIRROR_OK;
}

int rwy_mirror_set_faulted(rwy_mirror_dev_t *mdev, size_t idx, int faulted) {
    if (!mdev) return RWY_MIRROR_ERR_INVAL;
    if (idx >= mdev->num_devs) return RWY_MIRROR_ERR_INVAL;
    mdev->dev_faulted[idx] = faulted ? 1 : 0;
    return RWY_MIRROR_OK;
}

int rwy_mirror_read(void *ctx, uint64_t offset, void *buf, size_t len) {
    rwy_mirror_dev_t *m = (rwy_mirror_dev_t *)ctx;
    size_t prim, i;
    int rc;

    if (!m || m->num_devs == 0) return RWY_MIRROR_ERR_NODEV;

    /* 1. Try reading from primary / master device */
    prim = m->primary_idx;
    if (prim < m->num_devs && !m->dev_faulted[prim] && m->devs[prim].read) {
        m->read_ops[prim]++;
        rc = m->devs[prim].read(m->devs[prim].ctx, offset, buf, len);
        if (rc == 0) return 0;
        m->read_errors[prim]++;
    }

    /* 2. Resilient failover to alternate replicas */
    for (i = 0; i < m->num_devs; i++) {
        if (i == prim) continue;
        if (m->dev_faulted[i] || !m->devs[i].read) continue;

        m->read_ops[i]++;
        rc = m->devs[i].read(m->devs[i].ctx, offset, buf, len);
        if (rc == 0) return 0;
        m->read_errors[i]++;
    }

    return RWY_MIRROR_ERR_IO;
}

/* The outcome of a write or flush on all members, as the write policy sees it. */
static int mirror_verdict(const rwy_mirror_dev_t *m, int success_count, int primary_success, int first_err) {
    int ok;

    if (m->write_policy == RWY_MIRROR_WRITE_REQUIRE_ALL) ok = (size_t)success_count >= m->num_devs;
    else if (m->write_policy == RWY_MIRROR_WRITE_REQUIRE_PRIMARY) ok = primary_success;
    else ok = success_count > 0;
    if (ok) return 0;
    return first_err ? first_err : RWY_MIRROR_ERR_IO;
}

/* The superblock slots hold the member UUID, which differs between members. */
static int sb_slot_offset(uint64_t offset) {
    return offset == 0 || offset == RWY_BLOCK_SIZE;
}

static int sb_block_valid(const uint8_t *blk) {
    rwy_superblock_t sb;
    size_t i;
    for (i = 0; i < sizeof(sb); i++) ((uint8_t *)&sb)[i] = blk[i];
    return rwy_validate_sb(&sb) == RWY_OK;
}

static int uuid_is_zero(const uint8_t u[16]) {
    uint8_t acc = 0;
    size_t i;
    for (i = 0; i < 16; i++) acc |= u[i];
    return acc == 0;
}

static int uuid_same(const uint8_t a[16], const uint8_t b[16]) {
    size_t i;
    for (i = 0; i < 16; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int rwy_block_member_uuid(const rwy_block_dev_t *d, uint8_t out[16]) {
    uint8_t blk[RWY_BLOCK_SIZE];
    uint64_t best = 0;
    int slot, found = 0;

    for (slot = 0; slot < 2; slot++) {
        const rwy_superblock_t *sb = (const rwy_superblock_t *)blk;
        size_t i;
        if (!d->read || d->read(d->ctx, (uint64_t)slot * RWY_BLOCK_SIZE, blk, sizeof(blk))) continue;
        if (!sb_block_valid(blk) || (found && sb->generation < best)) continue;
        best = sb->generation;
        for (i = 0; i < 16; i++) out[i] = sb->member_uuid[i];
        found = 1;
    }
    return found;
}

/* Write to one member and count it. */
static int write_member(rwy_mirror_dev_t *m, size_t i, uint64_t offset, const void *buf, size_t len) {
    int rc;

    if (m->dev_faulted[i] || !m->devs[i].write) {
        m->write_errors[i]++;
        return RWY_MIRROR_ERR_IO;
    }
    m->write_ops[i]++;
    rc = m->devs[i].write(m->devs[i].ctx, offset, buf, len);
    if (rc) m->write_errors[i]++;
    return rc;
}

/* A superblock block as one member should hold it: with that member's own UUID, if it has one. */
static const uint8_t *member_block(const rwy_mirror_dev_t *m, size_t i, const uint8_t *buf, uint8_t *own) {
    uint8_t uuid[16];
    size_t k;

    if (m->dev_faulted[i] || !rwy_block_member_uuid(&m->devs[i], uuid) || uuid_is_zero(uuid)) return buf;
    for (k = 0; k < RWY_BLOCK_SIZE; k++) own[k] = buf[k];
    rwy_sb_block_set_member(own, uuid);
    return own;
}

int rwy_mirror_write(void *ctx, uint64_t offset, const void *buf, size_t len) {
    rwy_mirror_dev_t *m = (rwy_mirror_dev_t *)ctx;
    uint8_t own[RWY_BLOCK_SIZE];
    size_t i;
    int sb_write, success_count = 0;
    int primary_success = 0;
    int first_err = 0;

    if (!m || m->num_devs == 0) return RWY_MIRROR_ERR_NODEV;

    sb_write = sb_slot_offset(offset) && len == RWY_BLOCK_SIZE && sb_block_valid(buf);

    /* Sequential writes across all registered mirror devices */
    for (i = 0; i < m->num_devs; i++) {
        int rc = write_member(m, i, offset, sb_write ? member_block(m, i, buf, own) : buf, len);
        if (rc == 0) {
            success_count++;
            if (i == m->primary_idx) primary_success = 1;
        } else if (!first_err) {
            first_err = rc;
        }
    }

    return mirror_verdict(m, success_count, primary_success, first_err);
}

/* Give a member UUID to every slot of a member that holds a valid superblock. */
static int member_uuid_store(rwy_mirror_dev_t *m, size_t i, const uint8_t uuid[16]) {
    uint8_t blk[RWY_BLOCK_SIZE];
    int slot, rc = 0;

    for (slot = 0; slot < 2 && !rc; slot++) {
        uint64_t off = (uint64_t)slot * RWY_BLOCK_SIZE;
        if (m->devs[i].read(m->devs[i].ctx, off, blk, sizeof(blk)) || !sb_block_valid(blk)) continue;
        rwy_sb_block_set_member(blk, uuid);
        rc = write_member(m, i, off, blk, sizeof(blk));
    }
    if (!rc && m->devs[i].flush) rc = m->devs[i].flush(m->devs[i].ctx);
    return rc;
}

/* Does member i share its UUID with an earlier named member? */
static int uuid_shared(uint8_t uuids[][16], const int *named, size_t i) {
    size_t j;
    for (j = 0; j < i; j++) if (named[j] && uuid_same(uuids[i], uuids[j])) return 1;
    return 0;
}

int rwy_mirror_name_members(rwy_mirror_dev_t *m, int (*random)(void *buf, size_t len)) {
    uint8_t uuids[RWY_MIRROR_MAX_DEVS][16];
    int named[RWY_MIRROR_MAX_DEVS];
    size_t i;
    int first_err = 0;

    if (!m || !random) return RWY_MIRROR_ERR_INVAL;
    for (i = 0; i < m->num_devs; i++) {
        int rc;
        named[i] = !m->dev_faulted[i] && rwy_block_member_uuid(&m->devs[i], uuids[i]) && !uuid_is_zero(uuids[i]);
        if (m->dev_faulted[i] || (named[i] && !uuid_shared(uuids, named, i))) continue;
        rc = random(uuids[i], 16);
        if (!rc && !uuid_is_zero(uuids[i])) rc = member_uuid_store(m, i, uuids[i]);
        named[i] = rc == 0;
        if (rc && !first_err) first_err = rc;
    }
    return first_err;
}

int rwy_mirror_flush(void *ctx) {
    rwy_mirror_dev_t *m = (rwy_mirror_dev_t *)ctx;
    size_t i;
    int success_count = 0;
    int primary_success = 0;
    int first_err = 0;

    if (!m || m->num_devs == 0) return RWY_MIRROR_ERR_NODEV;

    /* Sequential flushes across all registered mirror devices */
    for (i = 0; i < m->num_devs; i++) {
        int rc;
        if (m->dev_faulted[i]) {
            m->flush_errors[i]++;
            if (!first_err) first_err = RWY_MIRROR_ERR_IO;
            continue;
        }

        if (!m->devs[i].flush) {
            m->flush_ops[i]++;
            success_count++;
            if (i == m->primary_idx) primary_success = 1;
            continue;
        }

        m->flush_ops[i]++;
        rc = m->devs[i].flush(m->devs[i].ctx);
        if (rc == 0) {
            success_count++;
            if (i == m->primary_idx) primary_success = 1;
        } else {
            m->flush_errors[i]++;
            if (!first_err) first_err = rc;
        }
    }

    return mirror_verdict(m, success_count, primary_success, first_err);
}

int rwy_mirror_wrap(rwy_block_dev_t *out_dev, rwy_mirror_dev_t *mdev) {
    size_t prim;
    if (!out_dev || !mdev || mdev->num_devs == 0) return RWY_MIRROR_ERR_INVAL;

    prim = mdev->primary_idx < mdev->num_devs ? mdev->primary_idx : 0;
    out_dev->read = rwy_mirror_read;
    out_dev->write = rwy_mirror_write;
    out_dev->flush = rwy_mirror_flush;
    out_dev->ctx = mdev;
    out_dev->size = mdev->devs[prim].size;
    out_dev->block_size = mdev->devs[prim].block_size ? mdev->devs[prim].block_size : RWY_BLOCK_SIZE;
    out_dev->stats = NULL;
    out_dev->dircache = NULL;

    return RWY_MIRROR_OK;
}
