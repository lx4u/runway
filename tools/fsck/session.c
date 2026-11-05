/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>
#include <sys/file.h>

#include "check.h"

/* Returns 0, FSCK_UNCORRECTED (bad keyslot header) or FSCK_ERROR (cannot unlock). */
static int check_crypto_setup(file_t *fl, rwy_block_dev_t *base_dev,
                              const char *key_pass,
                              rwy_block_dev_t *out_dev,
                              rwy_crypto_dev_ctx_t *cctx,
                              int *is_encrypted) {
    (void)fl;
    *is_encrypted = 0;

    rwy_keyslot_header_t khdr;
    int krc = rwy_keyslot_read_from_dev(base_dev, &khdr);
    if (krc == RWY_CRYPTO_ERR_EMPTY) {
        *out_dev = *base_dev;
        return 0;
    }
    if (krc != RWY_CRYPTO_OK) {
        fprintf(stderr, "check: keyslot header corrupt or missing\n");
        return FSCK_UNCORRECTED;
    }

    *is_encrypted = 1;
    if (!key_pass) {
        *out_dev = *base_dev;
        return 0;
    }

    uint8_t dek[32];
    int rc = rwy_keyslot_unwrap(&khdr, key_pass, dek);
    if (rc != 0) {
        fprintf(stderr, "check: authentication failed (incorrect key/passphrase)\n");
        return FSCK_ERROR;
    }

    rc = rwy_crypto_dev_wrap(out_dev, cctx, base_dev, dek, RWY_SB_BLOCKS);
    if (rc != 0) {
        fprintf(stderr, "check: failed to wrap crypto block device\n");
        return FSCK_ERROR;
    }

    return 0;
}

/*
 * Open the store. A writable session takes an exclusive lock; lock_hint
 * names who else may hold it. Returns 0, FSCK_ERROR if the store cannot
 * be opened, locked or unlocked, or FSCK_UNCORRECTED for a bad keyslot header.
 */
int session_open(session_t *ss, const char *path, const char *key_pass, int rw,
                        const char *lock_hint) {
    int rc;

    if (file_open(&ss->fl, path, rw) != 0) {
        fprintf(stderr, "cannot open %s\n", path);
        return FSCK_ERROR;
    }
    if (rw && flock(ss->fl.hd.fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "cannot lock %s (%s holds it?)\n", ss->fl.path, lock_hint);
        file_close(&ss->fl);
        return FSCK_ERROR;
    }
    ss->dev.read = file_rd;
    ss->dev.write = rw ? file_wr_rw : file_wr_ro;
    ss->dev.flush = rw ? file_fl_rw : 0;
    ss->dev.ctx = &ss->fl;
    ss->dev.size = file_get_size(&ss->fl);
    ss->dev.block_size = RWY_BLOCK_SIZE;
    ss->dev.stats = NULL;
    ss->dev.dircache = NULL;
    ss->is_enc = 0;
    rc = check_crypto_setup(&ss->fl, &ss->dev, key_pass, &ss->chk_dev, &ss->cctx, &ss->is_enc);
    if (rc)
        file_close(&ss->fl);
    return rc;
}

int session_check(check_ctx_t *c, session_t *ss, rwy_store_t *s) {
    c->errors = c->warnings = c->orphans = c->leftovers = c->frag_objects = 0;
    return run_checks(c, &ss->fl, &ss->chk_dev, s);
}
