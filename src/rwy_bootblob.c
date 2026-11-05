/*
 * rwy_bootblob.c — Orphans, staging files and bad content of EFI/RUNWAY/BLOBS (the work of `rwyctl check`).
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-only code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rwy_blob.h"
#include "rwy_bootcfg.h"
#include "rwy_crypto.h"

#define BLOBS_PATH ROOT_PATH "/BLOBS"
#define STAGING_PREFIX ".staging%2F"   /* what the bridge makes of a file written to /.staging/ */
#define BLOB_CHUNK (1U << 20)

static int stage_name(const char *name) {
    return !strcmp(name, RWY_BLOB_STAGE_NAME) || !strncmp(name, STAGING_PREFIX, sizeof(STAGING_PREFIX) - 1);
}

/* Tell `fn` about each orphan blob and each staging file of BLOBS/; return how many there are. */
static int stale_visit(rwy_store_t *s, rwy_blob_stale_fn fn, void *ctx) {
    char name[RWY_NAME_MAX + 1], path[RWY_MAX_PATH + 1];
    uint64_t t = bc_active(s), blobs;
    int n = 0;

    if (!t) return 0;
    s->bc_novirt = 1;
    if (rwy_dir_lookup(s, t, "BLOBS", 5, &blobs, 0) == RWY_OK) {
        for (size_t i = 0; rwy_readdir_nth(s, BLOBS_PATH, i, name, sizeof(name), 0) == RWY_OK; i++) {
            int orphan = bc_blob_orphan(s, blobs, name);

            if (!orphan && !stage_name(name)) continue;
            snprintf(path, sizeof(path), "BLOBS/%s", name);
            if (fn) fn(ctx, orphan ? RWY_BLOB_ORPHAN : RWY_BLOB_STAGED, path);
            n++;
        }
    }
    s->bc_novirt = 0;
    return n;
}

int rwy_blob_stale(rwy_store_t *s, rwy_blob_stale_fn fn, void *ctx) {
    int n, rc;

    if (!s) return RWY_ERR_INVAL;
    if ((rc = rwy_begin(s))) return rc;
    s->bc_err = 0;
    n = stale_visit(s, fn, ctx);
    rc = s->bc_err;
    (void)rwy_abort(s);
    return rc ? rc : n;
}

/* Unlink the staging files of BLOBS/, one at a time: the index of the next entry moves with each unlink. */
static int unlink_staging(rwy_store_t *s, int *count) {
    char name[RWY_NAME_MAX + 1], path[RWY_MAX_PATH + 1];
    size_t i = 0;
    int rc = RWY_OK;

    while (!rc && rwy_readdir_nth(s, BLOBS_PATH, i, name, sizeof(name), 0) == RWY_OK) {
        if (!stage_name(name)) {
            i++;
            continue;
        }
        snprintf(path, sizeof(path), BLOBS_PATH "/%s", name);
        rc = rwy_unlink(s, path);
        if (!rc && count) (*count)++;
        i = 0;
    }
    return rc;
}

int rwy_blob_staging_remove(rwy_store_t *s) {
    int n = 0, rc;

    if (!s) return RWY_ERR_INVAL;
    if ((rc = rwy_begin(s))) return rc;
    s->bc_novirt = 1;
    rc = unlink_staging(s, &n);
    s->bc_novirt = 0;
    if (rc || !n) {
        (void)rwy_abort(s);
        return rc;
    }
    rc = rwy_commit(s);
    return rc ? rc : n;
}

int rwy_blob_stale_remove(rwy_store_t *s) {
    int n, rc;

    if (!s) return RWY_ERR_INVAL;
    if (!bc_is_system(s)) return RWY_ERR_PERM;
    if ((rc = rwy_begin(s))) return rc;
    s->bc_err = 0;
    n = stale_visit(s, 0, 0);
    rc = s->bc_err;
    if (!rc && n) {
        rc = unlink_staging(s, NULL);
        s->bc_sweep = 1;   /* the commit unlinks the orphans */
    }
    if (rc || !n) {
        (void)rwy_abort(s);
        return rc;
    }
    rc = rwy_commit(s);
    return rc ? rc : n;
}

/* Hash the object `oid` of `size` bytes. RWY_OK, or the error of the read. */
static int digest_object(rwy_store_t *s, uint64_t oid, uint64_t size, uint8_t *buf, uint8_t digest[32]) {
    rwy_sha256_stream_t h;
    uint64_t pos = 0;
    int rc = RWY_OK;

    if (rwy_sha256_stream_init(&h) != RWY_CRYPTO_OK) return RWY_ERR_IO;
    while (!rc && pos < size) {
        size_t n = size - pos < BLOB_CHUNK ? (size_t)(size - pos) : BLOB_CHUNK;

        if ((rc = rwy_read(s, oid, pos, buf, &n)) || !n) break;
        rwy_sha256_stream_update(&h, buf, n);
        pos += n;
    }
    rwy_sha256_stream_final(&h, digest);
    return rc;
}

static int slot_links(rwy_store_t *s, uint64_t slot, uint64_t blob) {
    static const char *const payloads[] = { "KERNEL", "INITRD", "DTB" };
    uint64_t o;

    for (size_t i = 0; i < sizeof(payloads) / sizeof(payloads[0]); i++)
        if (rwy_dir_lookup(s, slot, payloads[i], (uint32_t)strlen(payloads[i]), &o, 0) == RWY_OK && o == blob) return 1;
    return 0;
}

/* FAILED with CAUSE bad_blob, one record write and one journal line, like `rwyctl fail`. */
static int fail_slot(rwy_store_t *s, const char *name, uint64_t dir, uint64_t rev) {
    uint8_t r[RWY_REC_PAYLOAD], hash[32] = { 0 };
    char label[RWY_LABEL_MAX + 1] = { 0 };
    rwy_ss_entry_t e;
    int rc, raise;

    bc_record_load(s, r);
    e = bc_ss_entry(r, rev);
    if (e.state == RWY_SS_FAILED) return RWY_OK;
    e.state = RWY_SS_FAILED;
    e.cause = RWY_SS_CAUSE_BLOB;
    if ((rc = rwy_ss_put(r, rev, &e))) return rc;
    raise = rwy_anchor_raise_rec(s, r);
    (void)bc_read_label(s, dir, label);
    (void)rwy_transplog_calc_payload_hash(s, name, hash);
    (void)rwy_transplog_record(s, name, RWY_JOURNAL_EVENT_FAIL, rev, label, hash, s->rec_seq + 1, NULL);
    rc = rwy_record_write(s, r);
    if (!rc && raise) rwy_anchor_raised(s);
    return rc;
}

/* Fail each slot that links the blob `blob`; tell `fn` about each. */
static int fail_linkers(rwy_store_t *s, const char *bname, uint64_t blob, rwy_blob_bad_fn fn, void *ctx) {
    char name[RWY_NAME_MAX + 1];
    uint64_t t = bc_active(s), o, rev;
    uint32_t l;
    size_t i = 0;
    int rc = RWY_OK;

    while (!rc && bc_next_dir(s, t, &i, name, &l, &o)) {
        if (bc_dir_kind(name, l) != K_DIR_SLOT || !bc_read_u64(s, o, "REVISION", &rev) || !slot_links(s, o, blob)) continue;
        if (bc_ss_entry(s->rec, rev).state == RWY_SS_FAILED) continue;
        if ((rc = fail_slot(s, name, o, rev)) == RWY_OK && fn) fn(ctx, bname, 0, name);
    }
    return rc;
}

/* Hash the blob `name` and compare. 1 if its content is not the name (`actual` says what it is), 0 if it is, < 0 on error. */
static int blob_bad(rwy_store_t *s, uint64_t blobs, const char *name, uint8_t *buf, uint64_t *oid, char actual[RWY_BLOB_NAME_SIZE]) {
    const rwy_inode_t *in;
    uint8_t digest[32], type;
    int rc;

    if (rwy_dir_lookup(s, blobs, name, (uint32_t)strlen(name), oid, &type) != RWY_OK || type != RWY_TYPE_FILE) return 0;
    if (!(in = rwy_inode_of(s, *oid, 1))) return RWY_ERR_IO;
    rc = digest_object(s, *oid, in->size, buf, digest);
    if (rc) {
        snprintf(actual, RWY_BLOB_NAME_SIZE, "unreadable");
        return 1;
    }
    rwy_blob_name(digest, actual);
    return strcmp(actual, name) != 0;
}

/* Check the blob `name`: 1 if it is bad (told to `fn`, and with `repair` its slots are failed), 0 if not, < 0 on error. */
static int verify_one(rwy_store_t *s, uint64_t blobs, const char *name, uint8_t *buf, int repair, rwy_blob_bad_fn fn, void *ctx) {
    char actual[RWY_BLOB_NAME_SIZE];
    uint64_t oid;
    int rc = blob_bad(s, blobs, name, buf, &oid, actual);

    if (rc <= 0) return rc;
    if (fn) fn(ctx, name, actual, 0);
    if (repair && (rc = fail_linkers(s, name, oid, fn, ctx))) return rc;
    return 1;
}

int rwy_blob_verify(rwy_store_t *s, int repair, rwy_blob_bad_fn fn, void *ctx) {
    char name[RWY_NAME_MAX + 1];
    uint64_t t, blobs;
    uint8_t *buf;
    int bad = 0, rc;

    if (!s) return RWY_ERR_INVAL;
    if (repair && !bc_is_system(s)) return RWY_ERR_PERM;
    if (!(buf = malloc(BLOB_CHUNK))) return RWY_ERR_IO;
    if ((rc = rwy_begin(s))) {
        free(buf);
        return rc;
    }
    s->bc_err = 0;
    t = bc_active(s);
    s->bc_novirt = 1;
    if (t && rwy_dir_lookup(s, t, "BLOBS", 5, &blobs, 0) == RWY_OK) {
        for (size_t i = 0; rc >= 0 && rwy_readdir_nth(s, BLOBS_PATH, i, name, sizeof(name), 0) == RWY_OK; i++) {
            if (!rwy_bootcfg_content_name(name, strlen(name))) continue;
            rc = verify_one(s, blobs, name, buf, repair, fn, ctx);
            bad += rc > 0;
        }
    }
    s->bc_novirt = 0;
    if (rc >= 0) rc = s->bc_err;
    (void)rwy_abort(s);
    free(buf);
    return rc < 0 ? rc : bad;
}
