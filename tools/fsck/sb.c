/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "rwy_format.h"
#include "rwy_flight.h"
#include "rwy_netset.h"
#include "rwy_transplog.h"
#include "rwy_internal.h"
#include "rwy_recarea.h"
#include "rwy_bundle.h"

static void check_sb_reserved(check_ctx_t *c, const rwy_superblock_t *sb, char slot) {
    if (sb->key_gen != 0 || sb->snapshot_gen != 0 || sb->snapshot_root != 0)
        ERR(c, "slot %c: nonzero reserved generation fields", slot);
    if (sb->crypt_algo == RWY_CRYPT_NONE && !all_zero(sb->reserved, sizeof(sb->reserved)))
        ERR(c, "slot %c: nonzero reserved bytes", slot);
}

/* Read a raw slot for reporting (mount picks internally). */
static int read_sb_slot(rwy_block_dev_t *dev, uint64_t off, rwy_superblock_t *sb) {
    uint8_t blk[RWY_BLOCK_SIZE];
    memset(sb, 0, sizeof(*sb));
    if (dev->read(dev->ctx, off, blk, sizeof(blk)) != 0) return 0;
    return rwy_validate_sb_block(blk, sb) == RWY_OK;
}

static void print_sb_slot(char slot, int valid, const rwy_superblock_t *sb) {
    printf("slot %c: %s", slot, valid ? "valid" : "INVALID");
    if (valid) printf(" (gen %llu)", (unsigned long long)sb->generation);
    printf("\n");
    if (valid) {
        char u[37];
        rwy_format_uuid(sb->member_uuid, u);
        printf("slot %c: member uuid %s\n", slot, u);
    }
}

/* The geometry of the flight area is written once, so both slots must agree on it. */
static void check_flight_geometry(check_ctx_t *c, const rwy_superblock_t *a, const rwy_superblock_t *b) {
    if (a->flight_start != b->flight_start || a->flight_blocks != b->flight_blocks)
        ERR(c, "slot A and slot B flight area differ (the geometry never changes)");
}

/* The geometry of the journal area is written once, so both slots must agree on it. */
static void check_journal_geometry(check_ctx_t *c, const rwy_superblock_t *a, const rwy_superblock_t *b) {
    if (a->journal_start != b->journal_start || a->journal_blocks != b->journal_blocks)
        ERR(c, "slot A and slot B journal area differ (the geometry never changes)");
}

/* The geometry of the recovery area is written once, so both slots must agree on it. */
static void check_recovery_geometry(check_ctx_t *c, const rwy_superblock_t *a, const rwy_superblock_t *b) {
    if (a->recovery_start != b->recovery_start || a->recovery_blocks != b->recovery_blocks)
        ERR(c, "slot A and slot B recovery area differ (the geometry never changes)");
}

/* The geometry of the loader area is written once, so both slots must agree on it. */
static void check_loader_geometry(check_ctx_t *c, const rwy_superblock_t *a, const rwy_superblock_t *b) {
    if (a->loader_start != b->loader_start || a->loader_blocks != b->loader_blocks)
        ERR(c, "slot A and slot B loader area differ (the geometry never changes)");
}

/* The geometry of the plaintext settings area is written once, so both slots must agree on it. */
static void check_net_geometry(check_ctx_t *c, const rwy_superblock_t *a, const rwy_superblock_t *b) {
    if (a->net_start != b->net_start || a->net_blocks != b->net_blocks)
        ERR(c, "slot A and slot B settings area differ (the geometry never changes)");
}

/* The area is plaintext: read it from the file itself, with or without the key. */
static void raw_dev(rwy_block_dev_t *d, file_t *fl) {
    memset(d, 0, sizeof(*d));
    d->read = file_rd;
    d->ctx = fl;
    d->size = file_get_size(fl);
    d->block_size = RWY_BLOCK_SIZE;
}

/* Pick the bundle of one store file; 0 when it has a valid one. */
static int member_pick(file_t *fl, rwy_ra_info_t *info) {
    rwy_block_dev_t raw;
    rwy_superblock_t a, b;
    const rwy_superblock_t *sb;
    int va, vb;
    raw_dev(&raw, fl);
    va = read_sb_slot(&raw, 0, &a);
    vb = read_sb_slot(&raw, RWY_BLOCK_SIZE, &b);
    sb = !vb || (va && a.generation >= b.generation) ? &a : &b;
    if (!va && !vb) return -1;
    if (!sb->recovery_blocks) return -1;
    return rwy_ra_pick(&raw, sb->recovery_start, sb->recovery_blocks, info);
}

/* Compare the bundle with the one of each --member file. */
static void check_recovery_members(check_ctx_t *c, const rwy_ra_info_t *mine) {
    for (int i = 0; i < c->nmember; i++) {
        file_t mf;
        rwy_ra_info_t other;
        int rc;
        if (file_open(&mf, c->member[i], 0)) { ERR(c, "cannot open member %s", c->member[i]); continue; }
        rc = member_pick(&mf, &other);
        file_close(&mf);
        if (rc) WARN(c, "recovery area: member %s has no valid bundle", c->member[i]);
        else if (!rwy_ra_same(mine, &other)) WARN(c, "recovery area differs from the one of %s", c->member[i]);
        else printf("recovery area: same bundle as %s\n", c->member[i]);
    }
}

static void check_flight_missing(check_ctx_t *c, const rwy_flight_info_t *info) {
    if (info->status[0] == RWY_FLIGHT_EMPTY && info->status[1] == RWY_FLIGHT_EMPTY) {
        NOTE(c, "flight area is empty");
    } else if (info->status[0] == RWY_FLIGHT_EMPTY || info->status[1] == RWY_FLIGHT_EMPTY) {
        int empty_idx = (info->status[0] == RWY_FLIGHT_EMPTY) ? 0 : 1;
        int bad_idx = 1 - empty_idx;
        WARN(c, "flight area copy %c is %s, copy %c is empty",
             'A' + bad_idx, rwy_flight_reason(info->status[bad_idx]), 'A' + empty_idx);
    } else {
        ERR(c, "flight area has no valid copy (copy A: %s, copy B: %s)",
            rwy_flight_reason(info->status[0]), rwy_flight_reason(info->status[1]));
    }
}

void check_flight_area(check_ctx_t *c, file_t *fl, const rwy_superblock_t *sb) {
    rwy_block_dev_t raw;
    rwy_flight_info_t info;
    int rc;
    if (!sb->flight_blocks) return;
    raw_dev(&raw, fl);
    rc = rwy_flight_pick(&raw, sb->flight_start, sb->flight_blocks, &info);
    for (unsigned k = 0; k < 2; k++) {
        if (info.status[k] == RWY_FLIGHT_OK)
            printf("flight area: copy %c: valid, sequence %llu\n", 'A' + k, (unsigned long long)info.seq[k]);
        else
            printf("flight area: copy %c: %s\n", 'A' + k, rwy_flight_reason(info.status[k]));
    }
    if (rc) {
        check_flight_missing(c, &info);
        return;
    }
    for (unsigned k = 0; k < 2; k++)
        if (info.status[k] != RWY_FLIGHT_OK && info.status[k] != RWY_FLIGHT_EMPTY)
            WARN(c, "flight area copy %c is %s, copy %c is used", 'A' + k,
                 rwy_flight_reason(info.status[k]), 'A' + info.copy);
}

static void check_journal_missing(check_ctx_t *c, const rwy_transplog_info_t *info) {
    if (info->status[0] == RWY_TRANSPLOG_EMPTY && info->status[1] == RWY_TRANSPLOG_EMPTY) {
        NOTE(c, "journal area is empty");
    } else if (info->status[0] == RWY_TRANSPLOG_EMPTY || info->status[1] == RWY_TRANSPLOG_EMPTY) {
        int empty_idx = (info->status[0] == RWY_TRANSPLOG_EMPTY) ? 0 : 1;
        int bad_idx = 1 - empty_idx;
        WARN(c, "journal area copy %c is %s, copy %c is empty",
             'A' + bad_idx, rwy_transplog_reason(info->status[bad_idx]), 'A' + empty_idx);
    } else {
        ERR(c, "journal area has no valid copy (copy A: %s, copy B: %s)",
            rwy_transplog_reason(info->status[0]), rwy_transplog_reason(info->status[1]));
    }
}

static void check_journal_chain(check_ctx_t *c, rwy_block_dev_t *dev, const rwy_store_t *s,
                                const rwy_transplog_info_t *info) {
    uint64_t max_epoch = rwy_ss_epoch(s->rec);
    int vrc = rwy_transplog_verify_chain(dev, s->sb.journal_start, s->sb.journal_blocks, info, max_epoch);
    if (vrc == RWY_ERR_CORRUPT)
        ERR(c, "journal hash chain broken or corrupted");
    else if (vrc == RWY_ERR_RANGE)
        ERR(c, "journal entry epoch exceeds store anchor epoch (%llu)", (unsigned long long)max_epoch);
    else if (vrc == RWY_OK && info->count > 0)
        NOTE(c, "journal: hash chain verified (%u entries)", info->count);
}

void check_net_area(check_ctx_t *c, file_t *fl, const rwy_superblock_t *sb) {
    rwy_block_dev_t raw;
    rwy_log_info_t info;
    char text[512];
    int rc;

    if (!sb->net_blocks) return;
    raw_dev(&raw, fl);
    rc = rwy_log_pick(&raw, sb->net_start, sb->net_blocks, &info);
    for (unsigned k = 0; k < 2; k++) {
        if (info.status[k] == RWY_LOG_OK)
            printf("settings area: copy %c: valid, sequence %llu\n", 'A' + k, (unsigned long long)info.seq[k]);
        else
            printf("settings area: copy %c: %s\n", 'A' + k, rwy_log_reason(info.status[k]));
    }
    if (rc) {
        if (info.status[0] == RWY_LOG_EMPTY && info.status[1] == RWY_LOG_EMPTY)
            NOTE(c, "settings area is empty");
        else
            ERR(c, "settings area has no valid copy (copy A: %s, copy B: %s)",
                rwy_log_reason(info.status[0]), rwy_log_reason(info.status[1]));
        return;
    }
    for (unsigned k = 0; k < 2; k++)
        if (info.status[k] != RWY_LOG_OK && info.status[k] != RWY_LOG_EMPTY)
            WARN(c, "settings area copy %c is %s, copy %c is used", 'A' + k,
                 rwy_log_reason(info.status[k]), 'A' + info.copy);
    rwy_log_format(&info.settings, text, sizeof(text));
    fputs(text, stdout);
}

void check_journal_area(check_ctx_t *c, rwy_block_dev_t *dev, const rwy_store_t *s) {
    rwy_transplog_info_t info;
    int rc;
    if (!s->sb.journal_blocks) return;
    rc = rwy_transplog_pick(dev, s->sb.journal_start, s->sb.journal_blocks, &info);
    for (unsigned k = 0; k < 2; k++) {
        if (info.status[k] == RWY_TRANSPLOG_OK)
            printf("journal area: copy %c: valid, sequence %llu\n", 'A' + k, (unsigned long long)info.seq[k]);
        else
            printf("journal area: copy %c: %s\n", 'A' + k, rwy_transplog_reason(info.status[k]));
    }
    if (rc) {
        check_journal_missing(c, &info);
        return;
    }
    for (unsigned k = 0; k < 2; k++)
        if (info.status[k] != RWY_TRANSPLOG_OK && info.status[k] != RWY_TRANSPLOG_EMPTY)
            WARN(c, "journal area copy %c is %s, copy %c is used", 'A' + k,
                 rwy_transplog_reason(info.status[k]), 'A' + info.copy);
    check_journal_chain(c, dev, s, &info);
}

/* Both copies of the area: a bad copy next to a good one warns, no good copy is an error. */
void check_recovery_area(check_ctx_t *c, file_t *fl, const rwy_superblock_t *sb) {
    rwy_block_dev_t raw;
    rwy_ra_info_t info;
    int rc;
    if (!sb->recovery_blocks) return;
    raw_dev(&raw, fl);
    rc = rwy_ra_pick(&raw, sb->recovery_start, sb->recovery_blocks, &info);
    for (unsigned k = 0; k < 2; k++) {
        if (info.status[k] == RWY_RA_OK) printf("recovery area: copy %c: valid, sequence %llu\n", 'A' + k, (unsigned long long)info.seq[k]);
        else printf("recovery area: copy %c: %s\n", 'A' + k, rwy_ra_reason(info.status[k]));
    }
    if (rc) {
        if (info.status[0] == RWY_RA_EMPTY && info.status[1] == RWY_RA_EMPTY) NOTE(c, "recovery area is empty");
        else ERR(c, "recovery area has no valid bundle (copy A: %s, copy B: %s)", rwy_ra_reason(info.status[0]), rwy_ra_reason(info.status[1]));
        return;
    }
    for (unsigned k = 0; k < 2; k++)
        if (info.status[k] != RWY_RA_OK && info.status[k] != RWY_RA_EMPTY)
            WARN(c, "recovery area copy %c is %s, copy %c is used", 'A' + k, rwy_ra_reason(info.status[k]), 'A' + info.copy);
    check_recovery_members(c, &info);
}

static int check_loader_record(const rwy_block_dev_t *dev, uint32_t blk, rwy_loader_record_t *rec) {
    if (dev->read(dev->ctx, (uint64_t)blk * RWY_BLOCK_SIZE, rec, sizeof(*rec)) != 0)
        return -1;
    if (rec->magic != RWY_BLK_MAGIC || rec->version != RWY_LOADER_RECORD_VERSION)
        return -1;
    if (rwy_crc32c(rec, offsetof(rwy_loader_record_t, crc32c)) != rec->crc32c)
        return -1;
    return 0;
}

void check_loader_area(check_ctx_t *c, file_t *fl, const rwy_superblock_t *sb) {
    rwy_block_dev_t raw;
    rwy_bundle_info_t info;
    rwy_loader_record_t r0, r1;
    int v0, v1, rc;

    if (!sb->loader_blocks) return;
    raw_dev(&raw, fl);
    v0 = check_loader_record(&raw, sb->loader_start, &r0);
    v1 = check_loader_record(&raw, sb->loader_start + 1, &r1);

    if (v0 && v1)
        ERR(c, "loader area has no valid state record");
    else if (v0 || v1)
        NOTE(c, "loader area state record copy %u is invalid", v0 ? 0 : 1);

    rc = rwy_bundle_pick(&raw, sb->loader_start + 2, sb->loader_blocks, &rwy_loader_bundle_spec, &info);
    for (unsigned k = 0; k < 2; k++) {
        if (info.status[k] == RWY_BUNDLE_OK)
            printf("loader area: copy %c: valid, sequence %llu\n", 'A' + k, (unsigned long long)info.seq[k]);
        else
            printf("loader area: copy %c: %s\n", 'A' + k, rwy_bundle_reason(info.status[k]));
    }
    if (rc) {
        if (info.status[0] == RWY_BUNDLE_EMPTY && info.status[1] == RWY_BUNDLE_EMPTY)
            NOTE(c, "loader area is empty");
        else
            ERR(c, "loader area has no valid bundle (copy A: %s, copy B: %s)",
                rwy_bundle_reason(info.status[0]), rwy_bundle_reason(info.status[1]));
        return;
    }
    for (unsigned k = 0; k < 2; k++)
        if (info.status[k] != RWY_BUNDLE_OK && info.status[k] != RWY_BUNDLE_EMPTY)
            WARN(c, "loader area copy %c is %s, copy %c is used", 'A' + k,
                 rwy_bundle_reason(info.status[k]), 'A' + info.copy);
}

/* Compare fixed layout geometries between slot A and slot B. */
static void check_slot_geometries(check_ctx_t *c, const rwy_superblock_t *a, const rwy_superblock_t *b) {
    check_flight_geometry(c, a, b);
    check_journal_geometry(c, a, b);
    check_recovery_geometry(c, a, b);
    check_loader_geometry(c, a, b);
    check_net_geometry(c, a, b);
}

/* Compare two valid superblock slots for agreement. */
static void compare_slots(check_ctx_t *c, const rwy_superblock_t *a, const rwy_superblock_t *b) {
    if (memcmp(a->uuid, b->uuid, 16) != 0)
        ERR(c, "slot A and slot B UUID mismatch");
    if (a->generation == b->generation && a->inode_tbl_start != b->inode_tbl_start)
        NOTE(c, "both slots share generation %llu with different roots (clean shutdown image)",
             (unsigned long long)a->generation);
    check_slot_geometries(c, a, b);
}

int check_sb_slots(check_ctx_t *c, rwy_block_dev_t *dev) {
    rwy_superblock_t a, b;
    int va = read_sb_slot(dev, 0, &a);
    int vb = read_sb_slot(dev, RWY_BLOCK_SIZE, &b);

    print_sb_slot('A', va, &a);
    print_sb_slot('B', vb, &b);
    if (!va && !vb) { ERR(c, "no valid superblock"); return -1; }
    if (va && vb) compare_slots(c, &a, &b);
    /* Reserved fields must be zero on every valid slot, so a nonzero one blocks repair before mount. */
    if (va) check_sb_reserved(c, &a, 'A');
    if (vb) check_sb_reserved(c, &b, 'B');
    return 0;
}

static void print_area_info(const char *name, uint32_t blocks, uint32_t start) {
    if (blocks)
        printf("%s area: 2 x %u blocks at %u\n", name, blocks, start);
    else
        printf("%s area: none\n", name);
}

void check_sb_committed(check_ctx_t *c, const rwy_store_t *s) {
    char ustr[37];
    uint32_t unknown = s->sb.feat_compat & ~(RWY_FEAT_COMPAT_XATTR | RWY_FEAT_COMPAT_SNAPSHOT);

    rwy_format_uuid(s->sb.uuid, ustr);
    printf("committed: slot %c gen %llu table@%u bitmap@%u uuid=%s\n",
           s->sb_slot ? 'B' : 'A', (unsigned long long)s->sb.generation,
           s->sb.inode_tbl_start, s->sb.bitmap_start, ustr);
    print_area_info("flight", s->sb.flight_blocks, s->sb.flight_start);
    print_area_info("journal", s->sb.journal_blocks, s->sb.journal_start);
    print_area_info("recovery", s->sb.recovery_blocks, s->sb.recovery_start);
    print_area_info("loader", s->sb.loader_blocks, s->sb.loader_start);
    print_area_info("settings", s->sb.net_blocks, s->sb.net_start);
    if (unknown)
        WARN(c, "unknown compatible feature bits 0x%x (ignored)", unknown);
    if (s->sb.crypt_algo != RWY_CRYPT_NONE && s->sb.crypt_algo != RWY_CRYPT_AES_XTS_256)
        ERR(c, "unknown crypto algorithm %u (mount should have refused)", s->sb.crypt_algo);
    if (s->sb.auth_algo != 0 || s->sb.key_id != 0)
        ERR(c, "nonzero crypto reservation (auth_algo/key_id)");
    if (s->sb.key_gen != 0 || s->sb.snapshot_gen != 0 || s->sb.snapshot_root != 0)
        ERR(c, "nonzero reserved superblock fields");
    if (s->sb.crypt_algo == RWY_CRYPT_NONE && !all_zero(s->sb.reserved, sizeof(s->sb.reserved)))
        ERR(c, "nonzero reserved superblock bytes");
}
