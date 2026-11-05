/*
 * rwy_mount.c — Superblock validation, mount/remount and time source.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

/* Magic, version and sizes; the checks run in this order so that the first fault names the error. */
static int sb_check_size(const rwy_superblock_t *sb) {
    if (rwy_strncmp((const char *)sb->magic, "RUNWAYSB", 8) != 0) return RWY_ERR_MAGIC;
    if (sb->version != RWY_VERSION) return RWY_ERR_VERSION;
    if (sb->block_size != RWY_BLOCK_SIZE) return RWY_ERR_SIZE;
    if (sb->total_size < RWY_MIN_STORE_SIZE || sb->total_size > RWY_MAX_STORE_SIZE)
        return RWY_ERR_SIZE;
    if (sb->total_blocks != (uint32_t)(sb->total_size / RWY_BLOCK_SIZE))
        return RWY_ERR_SIZE;
    if (sb->total_blocks > RWY_MAX_BLOCKS) return RWY_ERR_SIZE;
    if (sb->max_inodes != RWY_MAX_INODES) return RWY_ERR_SIZE;
    return RWY_OK;
}

/* The flight area follows the state record and takes at most a quarter of the store. */
static int sb_check_flight(const rwy_superblock_t *sb) {
    if (sb->flight_start != sb->rec_start + RWY_REC_BLOCKS) return RWY_ERR_SIZE;
    if (sb->flight_blocks > sb->total_blocks / 4) return RWY_ERR_SIZE;
    return RWY_OK;
}

/* The journal area follows the flight area and takes at most a quarter of the store. */
static int sb_check_journal(const rwy_superblock_t *sb) {
    if (sb->journal_start != sb->flight_start + 2 * sb->flight_blocks) return RWY_ERR_SIZE;
    if (sb->journal_blocks > sb->total_blocks / 4) return RWY_ERR_SIZE;
    return RWY_OK;
}

/* The recovery area follows the journal area and takes at most a quarter of the store. */
static int sb_check_recovery(const rwy_superblock_t *sb) {
    if (sb->recovery_start != sb->journal_start + 2 * sb->journal_blocks) return RWY_ERR_SIZE;
    if (sb->recovery_blocks > sb->total_blocks / 4) return RWY_ERR_SIZE;
    return RWY_OK;
}

/* The loader area follows the recovery area: two state record blocks plus two bundle copies. */
static int sb_check_loader(const rwy_superblock_t *sb) {
    if (sb->loader_start != sb->recovery_start + 2 * sb->recovery_blocks) return RWY_ERR_SIZE;
    if (sb->loader_blocks > sb->total_blocks / 4) return RWY_ERR_SIZE;
    uint32_t exp_net = sb->loader_blocks ? sb->loader_start + 2 + 2 * sb->loader_blocks : sb->loader_start;
    if (sb->net_start != exp_net) return RWY_ERR_SIZE;
    return RWY_OK;
}

/* The plaintext settings area follows the loader area and holds two copies. */
static int sb_check_net(const rwy_superblock_t *sb) {
    if (sb->net_blocks > sb->total_blocks / 4) return RWY_ERR_SIZE;
    if (sb->pool_start != sb->net_start + 2 * sb->net_blocks) return RWY_ERR_SIZE;
    return RWY_OK;
}

/* The two image areas hold the inode table and bitmap copies. */
static int sb_check_images(const rwy_superblock_t *sb) {
    uint32_t bm_bytes = (sb->total_blocks + 7) / 8;
    uint32_t exp_bm_blocks = (bm_bytes + (RWY_BLOCK_SIZE - 32) - 1) / (RWY_BLOCK_SIZE - 32);

    if (sb->inode_tbl_blocks != RWY_INODE_TBL_BLOCKS) return RWY_ERR_SIZE;
    if (sb->bitmap_blocks != exp_bm_blocks) return RWY_ERR_SIZE;
    if (sb->area_blocks != sb->inode_tbl_blocks + sb->bitmap_blocks) return RWY_ERR_SIZE;
    if (sb->rec_start != RWY_SB_BLOCKS + 2 * sb->area_blocks) return RWY_ERR_SIZE;
    if (sb->inode_tbl_start != RWY_SB_BLOCKS && sb->inode_tbl_start != RWY_SB_BLOCKS + sb->area_blocks)
        return RWY_ERR_CORRUPT;
    if (sb->bitmap_start != sb->inode_tbl_start + sb->inode_tbl_blocks) return RWY_ERR_CORRUPT;
    return RWY_OK;
}

/* The image areas, the record, flight, journal, recovery area and the pool must follow each other as mkfs lays them out. */
static int sb_check_layout(const rwy_superblock_t *sb) {
    int rc;

    if (sb->generation == 0) return RWY_ERR_CORRUPT;
    if (sb->root_oid != 1) return RWY_ERR_CORRUPT;
    rc = sb_check_images(sb);
    if (rc) return rc;
    rc = sb_check_flight(sb);
    if (rc) return rc;
    rc = sb_check_journal(sb);
    if (rc) return rc;
    rc = sb_check_recovery(sb);
    if (rc) return rc;
    rc = sb_check_loader(sb);
    if (rc) return rc;
    rc = sb_check_net(sb);
    if (rc) return rc;
    if (sb->pool_start >= sb->total_blocks || sb->data_blocks != sb->total_blocks - sb->pool_start)
        return RWY_ERR_SIZE;
    return RWY_OK;
}

int rwy_validate_sb(const rwy_superblock_t *sb) {
    int rc;
    if (!sb) return RWY_ERR_INVAL;
    rc = sb_check_size(sb);
    if (rc) return rc;
    rc = sb_check_layout(sb);
    if (rc) return rc;
    if ((sb->crypt_algo != RWY_CRYPT_NONE && sb->crypt_algo != RWY_CRYPT_AES_XTS_256) ||
        sb->auth_algo != 0)
        return RWY_ERR_FEAT;
    if (rwy_crc32c(sb, offsetof(rwy_superblock_t, crc32c)) != sb->crc32c) return RWY_ERR_CORRUPT;
    return RWY_OK;
}

void rwy_sb_block_set_member(uint8_t *blk, const uint8_t uuid[16]) {
    rwy_superblock_t *sb = (rwy_superblock_t *)blk;
    size_t i;
    for (i = 0; i < 16; i++) sb->member_uuid[i] = uuid[i];
    sb->crc32c = rwy_crc32c(sb, offsetof(rwy_superblock_t, crc32c));
}

int rwy_validate_sb_block(const void *blk, rwy_superblock_t *out) {
    if (!blk) return RWY_ERR_INVAL;
    rwy_superblock_t sb;
    const uint8_t *p = (const uint8_t *)blk;
    uint8_t *q = (uint8_t *)&sb;
    for (size_t i = 0; i < sizeof(sb); i++) q[i] = p[i];
    int rc = rwy_validate_sb(&sb);
    if (rc != RWY_OK) return rc;
    if (sb.crypt_algo == RWY_CRYPT_AES_XTS_256) {
        rwy_keyslot_header_t khdr;
        const uint8_t *kp = p + RWY_KEYSLOT_OFFSET;
        uint8_t *kq = (uint8_t *)&khdr;
        for (size_t i = 0; i < sizeof(khdr); i++) kq[i] = kp[i];
        if (khdr.magic != RWY_KEYSLOT_MAGIC || khdr.crypt_algo != RWY_CRYPT_AES_XTS_256)
            return RWY_ERR_CORRUPT;
        rwy_keyslot_header_t tmp = khdr;
        tmp.crc32c = 0;
        if (rwy_crc32c(&tmp, sizeof(tmp)) != khdr.crc32c)
            return RWY_ERR_CORRUPT;
    }
    if (out) *out = sb;
    return RWY_OK;
}

static int read_sb_slot(const rwy_block_dev_t *dev, int slot, rwy_superblock_t *out) {
    uint8_t blk[RWY_BLOCK_SIZE];
    if (slot != 0 && slot != 1) return RWY_ERR_INVAL;
    if (rwy_dev_pread(dev, RWY_IO_META, (uint64_t)(slot ? RWY_SB_SLOT_B : RWY_SB_SLOT_A) * RWY_BLOCK_SIZE,
                      blk, sizeof(blk)))
        return RWY_ERR_IO;
    return rwy_validate_sb_block(blk, out);
}

int rwy_sb_slot_valid(rwy_store_t *s, int slot, rwy_superblock_t *out) {
    rwy_superblock_t sb;
    int rc = read_sb_slot(&s->dev, slot, &sb);
    if (out && rc == RWY_OK) *out = sb;
    return rc;
}

static int load_committed(rwy_store_t *s) {
    uint8_t blk[RWY_BLOCK_SIZE];
    uint32_t i;
    /* Load bitmap image. */
    for (i = 0; i < s->sb.bitmap_blocks; i++) {
        int rc = rwy_blk_read(&s->dev, RWY_IO_META, s->sb.bitmap_start + i, blk);
        if (rc) return rc;
        rc = rwy_blk_verify(blk, s->sb.generation, i, RWY_BLK_KIND_BITMAP);
        if (rc) return rc;
        {
            size_t copy = sizeof(s->bitmap) - (size_t)i * (RWY_BLOCK_SIZE - sizeof(rwy_blk_header_t));
            size_t payload = RWY_BLOCK_SIZE - sizeof(rwy_blk_header_t);
            if (copy > payload) copy = payload;
            if ((size_t)i * payload < sizeof(s->bitmap)) {
                const uint8_t *src = blk + sizeof(rwy_blk_header_t);
                uint8_t *dst = s->bitmap + (size_t)i * payload;
                size_t k;
                for (k = 0; k < copy; k++) dst[k] = src[k];
            }
        }
    }
    /* Load inode table image. */
    for (i = 0; i < s->sb.inode_tbl_blocks; i++) {
        int rc = rwy_blk_read(&s->dev, RWY_IO_META, s->sb.inode_tbl_start + i, blk);
        uint32_t per = RWY_BLOCK_SIZE - (uint32_t)sizeof(rwy_blk_header_t);
        uint32_t nino = per / RWY_INODE_SIZE;
        uint32_t base = i * nino, k;
        if (rc) return rc;
        rc = rwy_blk_verify(blk, s->sb.generation, i, RWY_BLK_KIND_INODE);
        if (rc) return rc;
        for (k = 0; k < nino && base + k < RWY_MAX_INODES; k++) {
            const uint8_t *src = blk + sizeof(rwy_blk_header_t) + k * RWY_INODE_SIZE;
            uint8_t *dst = (uint8_t *)&s->inodes[base + k];
            uint32_t j;
            for (j = 0; j < RWY_INODE_SIZE; j++) dst[j] = src[j];
        }
    }
    return RWY_OK;
}

/* Take the valid superblock slot with the higher generation. */
static int pick_superblock(rwy_store_t *s) {
    rwy_superblock_t a, b;
    int ra = read_sb_slot(&s->dev, 0, &a);
    int rb = read_sb_slot(&s->dev, 1, &b);

    if (ra == RWY_ERR_FEAT || rb == RWY_ERR_FEAT) return RWY_ERR_FEAT;
    if (ra != RWY_OK && rb != RWY_OK)
        return (ra == RWY_ERR_IO || rb == RWY_ERR_IO) ? RWY_ERR_IO : RWY_ERR_CORRUPT;
    if (ra == RWY_OK && rb == RWY_OK) {
        if (a.generation >= b.generation) { s->sb = a; s->sb_slot = 0; }
        else { s->sb = b; s->sb_slot = 1; }
    } else if (ra == RWY_OK) { s->sb = a; s->sb_slot = 0; }
    else { s->sb = b; s->sb_slot = 1; }
    return RWY_OK;
}

static int dev_size_ok(const rwy_block_dev_t *dev) {
    return dev->size >= RWY_MIN_STORE_SIZE && dev->size <= RWY_MAX_STORE_SIZE &&
           dev->block_size == RWY_BLOCK_SIZE;
}

static void reset_state(rwy_store_t *s, const rwy_block_dev_t *dev) {
    s->dev = *dev;
    memset(&s->io, 0, sizeof(s->io));
    s->dev.stats = &s->io;
    s->dev.dircache = &s->dircache;
    rwy_dircache_drop(&s->dev);
    s->in_txn = 0;
    s->alloc_reserve = 0;
    s->have_fixed_now = 0;
    s->flight_boot_count = 0;
    s->actor = RWY_ACTOR_USER;
    s->bc_priv = 0;
    s->bc_unlock = 0;
    s->bc_novirt = 0;
    s->anchor_ops = NULL;
    rwy_bootcfg_invalidate(s);
}

static uint64_t g_mounts;

int rwy_mount(rwy_store_t *s, const rwy_block_dev_t *dev) {
    uint32_t b;
    int rc;
    if (!s || !dev || !dev->read || !dev->write) return RWY_ERR_INVAL;
    if (!dev_size_ok(dev)) return RWY_ERR_SIZE;
    reset_state(s, dev);
    s->mount_id = ++g_mounts;
    rc = pick_superblock(s);
    if (rc) return rc;
    if (s->sb.total_size != dev->size)
        return RWY_ERR_SIZE;
    s->next_hint = s->sb.pool_start;
    rc = load_committed(s);
    if (rc) return rc;
    rc = rwy_record_load(s);
    if (rc) return rc;
    for (b = 0; b < s->sb.pool_start; b++)
        if (!rwy_bmp_get(s->bitmap, b)) return RWY_ERR_CORRUPT;
    if (s->inodes[0].oid != 1 || s->inodes[0].type != RWY_TYPE_DIR)
        return RWY_ERR_CORRUPT;
    return RWY_OK;
}

int rwy_remount(rwy_store_t *s) {
    rwy_block_dev_t d;
    int had_txn;
    if (!s) return RWY_ERR_INVAL;
    d = s->dev;
    had_txn = s->in_txn;
    (void)had_txn;
    s->in_txn = 0;
    {
        const struct rwy_anchor_ops *ops = s->anchor_ops;
        int actor = s->actor, novirt = s->bc_novirt;
        uint64_t mount_id = s->mount_id;
        int rc = rwy_mount(s, &d);
        s->mount_id = mount_id;
        s->actor = actor;
        s->bc_novirt = novirt;
        s->anchor_ops = ops;
        return rc;
    }
}

void rwy_set_actor(rwy_store_t *s, int actor) { if (s) s->actor = actor; }

void rwy_set_fixed_time(rwy_store_t *s, int64_t sec, uint32_t nsec) {
    if (!s) return;
    s->fixed_now_sec = sec; s->fixed_now_nsec = nsec; s->have_fixed_now = 1;
}

void rwy_clear_fixed_time(rwy_store_t *s) { if (s) s->have_fixed_now = 0; }

void rwy_now_of(const rwy_store_t *s, int64_t *sec, uint32_t *nsec) {
    if (s->have_fixed_now) { *sec = s->fixed_now_sec; *nsec = s->fixed_now_nsec; return; }
    *sec = (int64_t)(s->in_txn ? s->w_generation : s->sb.generation);
    *nsec = 0;
}
