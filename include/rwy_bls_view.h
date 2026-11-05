/*
 * rwy_bls_view.h — A snapshot of the BLS projection of a store.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The view is built in one pass over EFI/RUNWAY and BLOBS and is keyed by the store UUID, the
 * superblock generation, the record sequence and the number of the mount. The core moves one of the
 * first two counters on every commit and every record write, and a new open of a store, which can
 * carry the same counters as the old one (a clone of the store file), has a new mount number, so a
 * stale view is found by comparing the key. The view answers
 * the questions of the FUSE layer with the answers of the scans it replaces. Host-only code.
 */
#ifndef RWY_BLS_VIEW_H
#define RWY_BLS_VIEW_H

#include <stddef.h>
#include <stdint.h>
#include "rwy_core.h"
#include "rwy_bls.h"

#define RWY_BLS_VIEW_SLOT_PATH 44   /* "EFI/RUNWAY/" + 32 characters + NUL */

typedef struct {
    char path[RWY_BLS_VIEW_SLOT_PATH];   /* EFI/RUNWAY/<slot dir>, cut as the projection cuts it */
    char *rendered;                      /* entry file name with state and counter; NULL if it does not render */
    char *prefix;                        /* directory of the payload files: /<token>[/<version>]; Type 1 only */
    int32_t conf_len;                    /* length of the rendered .conf; < 0 if it does not render */
    uint8_t is_type2, has_initrd, has_dtb;
} rwy_bls_view_slot_t;

typedef struct {
    int valid;
    uint8_t uuid[16];
    uint64_t generation, rec_seq, mount_id;   /* the key */
    rwy_bls_view_slot_t *slots;      /* in directory order */
    size_t nslots;
    char *blob_names;                /* names in BLOBS, NUL separated, in directory order */
    uint32_t *blob_off;
    size_t nblobs;
    unsigned long builds;            /* how many times the view was built */
} rwy_bls_view_t;

typedef void (*rwy_bls_view_name_fn)(void *ctx, const char *name);

void rwy_bls_view_init(rwy_bls_view_t *v);
void rwy_bls_view_free(rwy_bls_view_t *v);

/* Make `v` match the committed state of `s`, building it if the key moved.
 * Returns RWY_OK, or RWY_ERR_BUSY when the store is in a transaction or hides its virtual files:
 * the caller reads the store itself then. Another code means the build failed and v is not valid. */
int rwy_bls_view_sync(rwy_bls_view_t *v, rwy_store_t *s);

/* The store path behind a projected payload file (.../KERNEL, INITRD, DTB). First slot in directory order wins. */
int rwy_bls_view_payload(const rwy_bls_view_t *v, const char *path, char *out, size_t out_sz);
/* Same, but a file in BLOBS named after the path comes first. */
int rwy_bls_view_resolve(const rwy_bls_view_t *v, const char *path, char *out, size_t out_sz);
/* Whether `path` is a directory of the projection (a blob prefix, a slot prefix or its parent). */
int rwy_bls_view_is_dir(const rwy_bls_view_t *v, const char *path);
/* The slot behind an entry file name; *conf_len is the rendered .conf length (< 0: it does not render). */
int rwy_bls_view_entry(const rwy_bls_view_t *v, int type2, const char *file,
                       char *slot_out, size_t slot_sz, int *conf_len);
/* Entry file names of a type, in directory order. */
void rwy_bls_view_entries(const rwy_bls_view_t *v, int type2, rwy_bls_view_name_fn fn, void *ctx);
/* Children of a projected directory that come from BLOBS, and from the slots. */
void rwy_bls_view_blob_children(const rwy_bls_view_t *v, const char *path, rwy_bls_view_name_fn fn, void *ctx);
void rwy_bls_view_slot_children(const rwy_bls_view_t *v, const char *path, rwy_bls_view_name_fn fn, void *ctx);
/* Top-level directories that the slots add to the root. */
void rwy_bls_view_roots(const rwy_bls_view_t *v, rwy_bls_view_name_fn fn, void *ctx);

#endif /* RWY_BLS_VIEW_H */
