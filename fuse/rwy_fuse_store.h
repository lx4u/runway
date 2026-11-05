/* rwy_fuse_store.h — the store behind the mount of rwy-fuse. */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RWY_FUSE_STORE_H
#define RWY_FUSE_STORE_H

#include "rwy_core.h"

/* Open and lock the N stores named by SPECS (the members of one mirror), unlock them with the key, mount the newest
 * copy into S and attach the anchor. The file part of each spec is made absolute and remembered, since the daemon
 * changes directory. Returns 0, or -1 after printing why not. */
int rwy_fuse_store_open(rwy_store_t *s, const char **specs, int n, const char *key_pass);

/* Record the write failures, release the anchor, close every member (this drops the locks) and forget the mirror.
 * The data key stays in memory for rwy_fuse_store_reopen(). Does nothing when the store is not open. */
void rwy_fuse_store_close(rwy_store_t *s);

/* Open and lock the same stores again after a close, mount the newest copy and attach the anchor, as the first open
 * did. The key is not asked for again. Returns 0, or -1 after printing why not (the stores are then left closed). */
int rwy_fuse_store_reopen(rwy_store_t *s);

/* Record the write failures of the members in the state record. */
void rwy_fuse_store_note_failures(rwy_store_t *s);

#endif
