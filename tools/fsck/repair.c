/*
 * Repair design (conservative):
 * SPDX-License-Identifier: Apache-2.0
 * - Repair fixes ONLY leftover accumulation on an otherwise consistent
 *   generation. It refuses on: unmountable store (no valid superblock),
 *   torn metadata headers, extent errors, double allocation,
 *   referenced-but-unmarked blocks, directory errors, orphaned inodes,
 *   nonzero reserved fields. No salvage, no inode surgery.
 * - Implementation reuses rwy_gc(): fresh table+bitmap images written
 *   CoW, flushed, then exactly one new superblock generation published.
 *   Crash before publication keeps the old generation; after keeps the
 *   new one. Identical visible contents either way.
 * - A no-op repair (zero leftovers) publishes nothing and exits 0:
 *   repair is not GC-on-demand; rwy_gc() covers proactive cleanup.
 * - The checker opens the store read-only by default. --repair opens
 *   read-write and takes an exclusive non-blocking flock, so two repairs
 *   cannot race; it cannot detect EFI/firmware mounts, so the operator
 *   must quiesce writers (unmount FUSE / halt EFI sessions) first.
 * - Physical reachability only: no keys/plaintext (same as GC).
 *
 * Test hook: RWY_CHECK_FI_AT=N fails the Nth repair-backend write/flush
 * (1-based; -1/ ... unset = never). RWY_CHECK_FI_VERBOSE=1 prints the
 * total backend write/flush count. Used by tests/repairtest.c for
 * deterministic fault injection over every repair persistence point.
 */

#include <stdio.h>
#include <stdlib.h>

#include "check.h"

static void print_io_points(const char *verb) {
    if (getenv("RWY_CHECK_FI_VERBOSE"))
        printf("%s io points: %ld\n", verb, file_io_points());
}

/*
 * Open for a repair or a defrag and refuse unless the store is clean
 * enough: encrypted without a key, unmountable, errors and orphans all
 * leave the store untouched. Returns 0 or the exit code.
 */
static int session_open_for_fix(check_ctx_t *c, session_t *ss, rwy_store_t *s, const opts_t *o, const char *verb,
                                                const char *lock_hint) {
    int rc = session_open(ss, o->store_path, o->key_pass, 1, lock_hint);

    if (rc) return rc;
    if (ss->is_enc && !o->key_pass) {
        printf("%s: REFUSED (store is encrypted; authentication required via --key) — store untouched\n", verb);
        file_close(&ss->fl);
        return FSCK_UNCORRECTED;
    }
    if (session_check(c, ss, s)) {
        printf("%s: REFUSED (unmountable store)\n", verb);
        file_close(&ss->fl);
        return FSCK_UNCORRECTED;
    }
    if (c->errors || c->orphans) {
        printf("%s: REFUSED (%d errors, %d orphans) — store untouched\n", verb, c->errors, c->orphans);
        file_close(&ss->fl);
        return FSCK_UNCORRECTED;
    }
    return 0;
}

/* Reopen the store read-only after a fix and check it again from disk. */
static int reopen_and_check(check_ctx_t *c, const opts_t *o, const char *verb, rwy_store_t *s) {
    session_t vs;
    int rc = session_open(&vs, o->store_path, o->key_pass, 0, "");

    if (rc) return rc;
    if (session_check(c, &vs, s)) {
        printf("%s: post-%s store unmountable (BUG)\n", verb, verb);
        rc = FSCK_UNCORRECTED;
    }
    file_close(&vs.fl);
    return rc;
}

/* The EFI/RUNWAY counter is the one policy violation repair fixes: one commit, as SYSTEM. */
static int repair_counter(rwy_store_t *s, int *fixed) {
    int rc;

    rwy_set_actor(s, RWY_ACTOR_SYSTEM);
    rc = rwy_bootcfg_repair_counter(s);
    if (rc < 0) {
        printf("repair: FAILED (%s) — store keeps a valid generation\n", rwy_strerror(rc));
        print_io_points("repair");
        return FSCK_ERROR;
    }
    *fixed = rc;
    if (rc) printf("repair: EFI/RUNWAY/REVISION fixed (gen %llu)\n", (unsigned long long)s->sb.generation);
    return 0;
}

static int repair_record(rwy_store_t *s, int *rec_fixed) {
    int rc;

    rwy_set_actor(s, RWY_ACTOR_SYSTEM);
    rc = rwy_bootcfg_repair_record(s);
    if (rc < 0) {
        printf("repair: FAILED (%s) — store keeps a valid generation\n", rwy_strerror(rc));
        return FSCK_ERROR;
    }
    *rec_fixed = rc;
    if (rc) printf("repair: state record written\n");
    return 0;
}

static int repair_policy(check_ctx_t *c, rwy_store_t *s, int *fixed, int *rec_fixed) {
    int rc;

    if (c->cfg_counter && (rc = repair_counter(s, fixed)))
        return rc;
    if ((c->cfg_record || c->cfg_stale) && (rc = repair_record(s, rec_fixed)))
        return rc;
    return 0;
}

int cmd_repair(check_ctx_t *c, const opts_t *o) {
    session_t ss;
    rwy_store_t s;
    uint64_t gen_before;
    int rc, fixed = 0, rec_fixed = 0;

    rc = session_open_for_fix(c, &ss, &s, o, "repair", "another repair or writer");
    if (rc) return rc;
    gen_before = s.sb.generation;
    rc = repair_policy(c, &s, &fixed, &rec_fixed);
    if (rc)
        goto out;
    if (c->leftovers == 0 && !fixed) {
        if (!rec_fixed)
            printf("repair: no leftovers, store unchanged (gen %llu)\n",
                   (unsigned long long)s.sb.generation);
        rc = rec_fixed ? FSCK_FIXED : FSCK_OK;
        goto out;
    }
    printf("repair: %s, publishing GC generation ...\n",
           fixed ? "the counter commit left blocks behind" : "leftover blocks found");
    rc = rwy_gc(&s);
    if (rc != RWY_OK) {
        printf("repair: FAILED (%s) — store keeps a valid generation\n", rwy_strerror(rc));
        print_io_points("repair");
        /* I/O or space failure mid-repair: old-or-new generation is valid, but repair did not converge. */
        rc = FSCK_ERROR;
        goto out;
    }
    print_io_points("repair");
    /* Re-verify from disk. */
    file_close(&ss.fl);
    rc = reopen_and_check(c, o, "repair", &s);
    if (rc) return rc;
    if (c->errors || c->cfg_counter || c->orphans || c->leftovers) {
        printf("repair: post-repair verification failed "
               "(%d errors, %d orphans, %d leftovers) (BUG)\n", c->errors, c->orphans, c->leftovers);
        return FSCK_UNCORRECTED;
    }
    if (s.sb.generation != gen_before + 1 + (uint64_t)fixed)
        NOTE(c, "repair published gen %llu (expected %llu)",
             (unsigned long long)s.sb.generation,
             (unsigned long long)(gen_before + 1 + (uint64_t)fixed));
    printf("repair: OK (gen %llu -> %llu, leftovers reclaimed)\n",
           (unsigned long long)gen_before, (unsigned long long)s.sb.generation);
    return FSCK_FIXED;
out:
    file_close(&ss.fl);
    return rc;
}

int cmd_defrag(check_ctx_t *c, const opts_t *o) {
    session_t ss;
    rwy_store_t s;
    uint64_t gen_before;
    int rc = session_open_for_fix(c, &ss, &s, o, "defrag", "another repair/defrag or writer");

    if (rc) return rc;
    if (c->frag_objects == 0 && c->leftovers == 0) {
        printf("defrag: store already compact (0 fragmented objects, 0 leftovers), store unchanged (gen %llu)\n",
               (unsigned long long)s.sb.generation);
        file_close(&ss.fl);
        return FSCK_OK;
    }
    gen_before = s.sb.generation;
    printf("defrag: %d fragmented objects, %d leftover blocks, compacting ...\n",
           c->frag_objects, c->leftovers);
    rc = rwy_defrag(&s);
    if (rc != RWY_OK) {
        printf("defrag: FAILED (%s) — store keeps a valid generation\n", rwy_strerror(rc));
        print_io_points("defrag");
        file_close(&ss.fl);
        return FSCK_ERROR;
    }
    print_io_points("defrag");
    /* Re-verify from disk. */
    file_close(&ss.fl);
    rc = reopen_and_check(c, o, "defrag", &s);
    if (rc) return rc;
    if (c->errors || c->orphans || c->leftovers || c->frag_objects) {
        printf("defrag: post-defrag verification failed "
               "(%d errors, %d orphans, %d leftovers, %d fragmented) (BUG)\n",
               c->errors, c->orphans, c->leftovers, c->frag_objects);
        return FSCK_UNCORRECTED;
    }
    if (s.sb.generation != gen_before + 1)
        NOTE(c, "defrag published gen %llu (expected %llu)",
             (unsigned long long)s.sb.generation, (unsigned long long)(gen_before + 1));
    printf("defrag: OK (gen %llu -> %llu, compacted to single runs)\n",
           (unsigned long long)gen_before, (unsigned long long)s.sb.generation);
    return 0;
}
