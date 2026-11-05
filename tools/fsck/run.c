/*
 * Validates: both superblock slots (structure + CRC + reserved fields),
 * committed-generation selection, inode table / bitmap block headers
 * (magic+generation+index+kind+CRC, torn-write detection), inode sanity
 * (oid/type/mode/size + reserved fields), extent bounds, extent overlap
 * (double allocation), bitmap-vs-reachable consistency (via the core's own
 * reachability scan, cross-checked against the checker's own traversal),
 * directory tree (references, types, name validity, depth), orphans,
 * xattr/crypto reservations, symlink targets, timestamp ranges.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "rwy_format.h"

/* A partition larger than its store: say how much is left over (information, not an error). */
static void report_unused(const file_t *fl) {
    if (fl->hd.unused)
        printf("note: %llu bytes unused after the store (the partition is larger than the store)\n",
               (unsigned long long)fl->hd.unused);
}

/*
 * Full read-only validation pass over the store behind `fl`/`dev`.
 * Mounts a fresh rwy_store_t internally (so it can be re-run for
 * post-repair verification), prints findings, fills
 * the counters of `c`. Returns 0 on mount+scan success
 * (even with findings), nonzero if the store cannot be mounted at all.
 * `s_out` receives the mounted store on success (or NULL to discard).
 */
int run_checks(check_ctx_t *c, file_t *fl, rwy_block_dev_t *dev, rwy_store_t *s_out) {
    static rwy_store_t s;
    static scan_t sc;
    uint32_t i;
    int mountrc;

    memset(&sc, 0, sizeof(sc));
    c->cfg_counter = c->cfg_record = c->cfg_stale = 0;

    if (check_sb_slots(c, dev))
        return -1;
    mountrc = rwy_mount(&s, dev);
    s.bc_novirt = 1;   /* the virtual EFI/RUNWAY INDEX is not stored: the walk must not see it */
    if (mountrc != RWY_OK) {
        ERR(c, "mount failed: %s", rwy_strerror(mountrc));
        return -1;
    }
    check_sb_committed(c, &s);
    report_unused(fl);
    check_flight_area(c, fl, &s.sb);
    check_journal_area(c, dev, &s);
    check_recovery_area(c, fl, &s.sb);
    check_loader_area(c, fl, &s.sb);
    check_net_area(c, fl, &s.sb);

    c->total_blocks = s.sb.total_blocks;
    c->pool_start = s.sb.pool_start;
    c->owner = malloc(sizeof(int32_t) * c->total_blocks);
    if (!c->owner) { fprintf(stderr, "oom\n"); exit(2); }
    for (i = 0; i < c->total_blocks; i++) c->owner[i] = -1;
    for (i = 0; i < c->pool_start; i++) c->owner[i] = -3; /* superblocks and image areas */

    check_blk_headers(c, dev, &s, s.sb.inode_tbl_start, s.sb.inode_tbl_blocks, RWY_BLK_KIND_INODE, "inode-table");
    check_blk_headers(c, dev, &s, s.sb.bitmap_start, s.sb.bitmap_blocks, RWY_BLK_KIND_BITMAP, "bitmap");
    for (i = 0; i < RWY_MAX_INODES; i++)
        check_inode(c, fl, dev, &s, i, &sc);
    printf("inodes: %u live\n", sc.nlive);
    if (s.inodes[0].type != RWY_TYPE_DIR || s.inodes[0].oid != 1)
        ERR(c, "root inode missing");
    check_dir_tree(c, &s, &sc);
    check_bitmap(c, dev, &s, sc.referenced_data);
    report_fragmentation(c, &s);
    check_orphans(c, &sc);
    check_bootcfg(c, &s);
    check_mirror_health(c, &s);

    free(c->owner);
    c->owner = 0;
    if (s_out) *s_out = s;
    printf("check: %s (%d errors, %d warnings%s)\n",
           c->errors || c->cfg_counter || c->cfg_record ? "CORRUPT" : "OK", c->errors + c->cfg_counter + c->cfg_record, c->warnings,
           c->orphans ? ", orphans present" : "");
    return 0;
}

static int unauth_summary(check_ctx_t *c) {
    printf("check: %s (%d errors, %d warnings)\n", c->errors ? "CORRUPT" : "OK", c->errors, c->warnings);
    return c->errors ? FSCK_UNCORRECTED : FSCK_OK;
}

int run_unauthenticated_check(check_ctx_t *c, file_t *fl, rwy_block_dev_t *dev) {
    rwy_superblock_t a, b;
    uint8_t blk[RWY_BLOCK_SIZE];
    int va = 0, vb = 0;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    file_rd(fl, 0, blk, sizeof(blk));
    va = (rwy_validate_sb_block(blk, &a) == RWY_OK);
    file_rd(fl, RWY_BLOCK_SIZE, blk, sizeof(blk));
    vb = (rwy_validate_sb_block(blk, &b) == RWY_OK);

    printf("slot A: %s", va ? "valid" : "INVALID");
    if (va) printf(" (gen %llu)", (unsigned long long)a.generation);
    printf("\nslot B: %s", vb ? "valid" : "INVALID");
    if (vb) printf(" (gen %llu)", (unsigned long long)b.generation);
    printf("\n");
    if (!va && !vb) {
        printf("ERROR: no valid superblock\n");
        return FSCK_UNCORRECTED;
    }
    {
        const rwy_superblock_t *active = va ? &a : &b;
        if (va && vb && b.generation > a.generation) active = &b;
        char ustr[37];
        rwy_format_uuid(active->uuid, ustr);
        printf("uuid: %s\n", ustr);
        c->errors = c->warnings = 0;
        report_unused(fl);
        check_flight_area(c, fl, active);
        check_recovery_area(c, fl, active);
        check_loader_area(c, fl, active);
        check_net_area(c, fl, active);
    }

    rwy_keyslot_header_t khdr;
    int rc = rwy_keyslot_read_from_dev(dev, &khdr);
    if (rc != 0) {
        printf("ERROR: keyslot header corrupt or missing\n");
        return FSCK_UNCORRECTED;
    }

    printf("keyslot header: valid (magic=0x%llx, alg=AES-256-XTS, %u slots)\n",
           (unsigned long long)khdr.magic, khdr.slot_count);
    for (uint32_t i = 0; i < khdr.slot_count; i++) {
        printf("  slot %u: %s (type=%u)\n", i,
               khdr.slots[i].state == RWY_KEYSLOT_STATE_ACTIVE ? "active" : "empty",
               khdr.slots[i].slot_type);
    }

    printf("physical check: OK (store is encrypted with AES-256-XTS, %llu bytes)\n",
           (unsigned long long)dev->size);
    printf("note: use --key <passphrase> to verify decrypted filesystem contents\n");
    return unauth_summary(c);
}
