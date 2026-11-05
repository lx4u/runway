/*
 * rwy_blob.h — Content names of blobs. Host-only code.
 * SPDX-License-Identifier: Apache-2.0
 *
 * A blob is named by the SHA-256 of its whole content: 64 lower case hex digits. This is the one place
 * that makes such a name. It hashes through the crypto table, so it uses OpenSSL where the tool installed
 * that table and the built-in code otherwise.
 */
#ifndef RWY_BLOB_H
#define RWY_BLOB_H

#include <stdint.h>

#include "rwy_core.h"

#define RWY_BLOB_NAME_LEN  64
#define RWY_BLOB_NAME_SIZE (RWY_BLOB_NAME_LEN + 1)

/* Hash a regular file from offset 0 to its end, in 1 MiB chunks. RWY_OK, RWY_ERR_IO or RWY_ERR_INVAL. */
int rwy_blob_hash_fd(int fd, uint8_t digest[32]);

/* Hash a file of the store from offset 0 to its end, in 1 MiB chunks. RWY_OK or the error of the read. */
int rwy_blob_hash_path(rwy_store_t *s, const char *path, uint8_t digest[32]);

/* Write the content name of a digest: 64 lower case hex digits and a NUL. */
void rwy_blob_name(const uint8_t digest[32], char out[RWY_BLOB_NAME_SIZE]);

/* The name of the file that `rwyctl` stages a new blob under, in BLOBS/, before it gets its content name. */
#define RWY_BLOB_STAGE_NAME "stage.rwyctl"

/* What `check` finds left over: a content blob that no slot links (it is swept with the next slot removal) and a
 * staging file of an install that did not finish: `BLOBS/stage.rwyctl`, or a file written to `/.staging/` of the mount
 * (the bridge names it `BLOBS/.staging%2F<name>`). `path` is "BLOBS/<name>". */
enum { RWY_BLOB_ORPHAN, RWY_BLOB_STAGED };
typedef void (*rwy_blob_stale_fn)(void *ctx, int kind, const char *path);

/* Tell `fn` about each orphan blob and staging file and return how many there are, < 0 on a read error. Writes nothing. */
int rwy_blob_stale(rwy_store_t *s, rwy_blob_stale_fn fn, void *ctx);

/* Remove only the staging files, in one commit; the actor needs no rights beyond USER. For `rwy-fuse` at start, when no
 * `rwyctl` can be copying. Returns how many were removed, 0 (and no commit) when none. */
int rwy_blob_staging_remove(rwy_store_t *s);

/* Remove them in one commit: SYSTEM only. Returns how many there were, 0 (and no commit) when none. */
int rwy_blob_stale_remove(rwy_store_t *s);

/* `fn` hears of a blob whose content is not its name (`slot` is NULL, `actual` is the hash of the content, or
 * "unreadable"), and, with repair, of each slot that links it and was just marked FAILED (`slot` is set). */
typedef void (*rwy_blob_bad_fn)(void *ctx, const char *name, const char *actual, const char *slot);

/* Hash every content blob and compare it with its name; this reads all the blobs. With `repair` (SYSTEM only) mark each
 * slot that links a bad blob FAILED with CAUSE bad_blob, one record write per bad blob. A blob is never renamed or
 * rewritten. Returns the number of bad blobs, < 0 on error. */
int rwy_blob_verify(rwy_store_t *s, int repair, rwy_blob_bad_fn fn, void *ctx);

#endif
