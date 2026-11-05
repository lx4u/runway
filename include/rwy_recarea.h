/*
 * rwy_recarea.h — The plaintext recovery area of an encrypted store (ADR 0004).
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two copies of one bundle (KERNEL, INITRD, DTB, ARGS) in a fixed region of the partition. The code works on
 * the raw member device, never on the encrypted one, and needs no key. Link src/rwy_recarea.c with
 * the crypto sources (SHA-256).
 */
#ifndef RWY_RECAREA_H
#define RWY_RECAREA_H

#include "rwy_core.h"

/* Why a copy is not valid (`status` of rwy_ra_info_t). */
#define RWY_RA_OK          0
#define RWY_RA_EMPTY       1   /* no magic: never written */
#define RWY_RA_BAD_HEADER  2   /* version, CRC, entry count, kind or kernel */
#define RWY_RA_BAD_RANGE   3   /* an offset or length outside the copy */
#define RWY_RA_BAD_HASH    4   /* a payload does not match its SHA-256 */
#define RWY_RA_IO          5   /* a read failed */

/* The parts of a bundle to write. A part is absent when `data` is NULL; KERNEL is required. */
typedef struct {
    const void *data;
    uint32_t    len;
} rwy_ra_part_t;

typedef struct {
    rwy_ra_part_t part[RWY_RA_KINDS];   /* indexed by RWY_RA_* */
} rwy_ra_bundle_t;

/* Result of rwy_ra_pick. `status` tells for each copy why it was not used (RWY_RA_OK if it is valid). */
typedef struct {
    int             status[2];
    uint64_t        seq[2];     /* sequence of each copy that is valid, else 0 */
    unsigned        copy;       /* the winner, valid after RWY_OK */
    uint64_t        sequence;
    uint32_t        count;
    rwy_ra_entry_t entry[RWY_RA_KINDS];
} rwy_ra_info_t;

/* Blocks one copy needs for `b`, or 0 if the bundle is not valid (no kernel, an empty part, ARGS over 4 KiB). */
uint32_t rwy_ra_blocks(const rwy_ra_bundle_t *b);

/* Write the bundle into the copy that is not the newest valid one: payloads, flush, read back and verify,
 * then the header, flush. NOSPC if it does not fit `blocks`, INVAL if it is not valid. Nothing is written
 * in either case. */
int rwy_ra_write(const rwy_block_dev_t *raw, uint32_t start, uint32_t blocks, const rwy_ra_bundle_t *b);

/* Check both copies and pick the valid one with the highest sequence. RWY_OK, or CORRUPT when no copy is
 * valid (IO when a read failed on the way). `info` is filled in either case. */
int rwy_ra_pick(const rwy_block_dev_t *raw, uint32_t start, uint32_t blocks, rwy_ra_info_t *info);

/* Read one part of the winning copy of `info` into `buf` and check its SHA-256. NOENT if the bundle has no
 * such part, SIZE if `cap` is too small, CORRUPT if the hash no longer matches. */
int rwy_ra_read(const rwy_block_dev_t *raw, uint32_t start, uint32_t blocks, const rwy_ra_info_t *info,
                 uint32_t kind, void *buf, size_t cap, size_t *len);

/* True when two picked bundles hold the same parts: same kinds, lengths and SHA-256. Copy, sequence and offsets
 * do not count. */
int rwy_ra_same(const rwy_ra_info_t *a, const rwy_ra_info_t *b);

/* Short text for a RWY_RA_* status, for the boot log and the tools. */
const char *rwy_ra_reason(int status);

#endif /* RWY_RECAREA_H */
