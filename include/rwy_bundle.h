/*
 * rwy_bundle.h — Generic flat bundle support (build, write, verify, pick).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_BUNDLE_H
#define RWY_BUNDLE_H

#include "rwy_core.h"

/* Why a bundle copy is not valid (`status` of rwy_bundle_info_t). */
#define RWY_BUNDLE_OK          0
#define RWY_BUNDLE_EMPTY       1   /* no magic: never written */
#define RWY_BUNDLE_BAD_HEADER  2   /* version, CRC, entry count, kind constraints */
#define RWY_BUNDLE_BAD_RANGE   3   /* an offset or length outside the copy */
#define RWY_BUNDLE_BAD_HASH    4   /* a payload does not match its SHA-256 */
#define RWY_BUNDLE_IO          5   /* a read failed */

/* Rule for one kind in a bundle. */
typedef struct {
    uint32_t kind;
    uint32_t min_count;  /* minimum occurrences required */
    uint32_t max_count;  /* maximum occurrences permitted */
    uint64_t max_len;    /* maximum length in bytes (0 = unlimited) */
} rwy_bundle_kind_rule_t;

/* Specification defining bundle format and validation rules. */
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t max_entries;
    uint32_t num_rules;
    const rwy_bundle_kind_rule_t *rules;
} rwy_bundle_spec_t;

/* Standard specification for the loader area bundle. */
extern const rwy_bundle_spec_t rwy_loader_bundle_spec;

/* A payload part when building or writing a bundle. */
typedef struct {
    uint32_t    kind;
    const void *data;
    uint32_t    len;
} rwy_bundle_payload_t;

/* Result of rwy_bundle_pick. */
typedef struct {
    int                status[2];
    uint64_t           seq[2];
    unsigned           copy;
    uint64_t           sequence;
    uint32_t           count;
    rwy_bundle_entry_t entry[RWY_BUNDLE_MAX_ENTRIES];
} rwy_bundle_info_t;

/* Short text describing a bundle status code. */
const char *rwy_bundle_reason(int status);

/* Blocks needed for the bundle payloads, or 0 if payloads violate spec. */
uint32_t rwy_bundle_blocks(const rwy_bundle_spec_t *spec, const rwy_bundle_payload_t *parts, uint32_t count);

/* Check the fixed header block: magic, version, count, kinds vs spec. */
int rwy_bundle_check_header(const uint8_t blk[RWY_BLOCK_SIZE], const rwy_bundle_spec_t *spec);

/* Check entry ranges within the copy. */
int rwy_bundle_check_range(const rwy_bundle_header_t *h, uint32_t blocks, const rwy_bundle_spec_t *spec);

/* Verify hashes of all entries in copy at base. */
int rwy_bundle_check_hashes(const rwy_block_dev_t *dev, uint32_t base, const rwy_bundle_header_t *h);

/* Check one copy: header, range, and hashes. */
int rwy_bundle_check_copy(const rwy_block_dev_t *dev, uint32_t base, uint32_t blocks,
                          const rwy_bundle_spec_t *spec, rwy_bundle_header_t *hdr);

/* Check both copies and pick newest valid copy. */
int rwy_bundle_pick(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                    const rwy_bundle_spec_t *spec, rwy_bundle_info_t *info);

/* Read an entry payload from copy and verify SHA-256. */
int rwy_bundle_read_entry(const rwy_block_dev_t *dev, uint32_t base,
                          const rwy_bundle_entry_t *e, void *buf, size_t cap, size_t *len);

/* Read the first entry of kind from winning copy in info. */
int rwy_bundle_read(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                    const rwy_bundle_info_t *info, uint32_t kind, void *buf, size_t cap, size_t *len);

/* Read the entry at entry index from winning copy in info. */
int rwy_bundle_read_at(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                       const rwy_bundle_info_t *info, unsigned idx, void *buf, size_t cap, size_t *len);

/* True if two bundles hold identical parts (kind, length, and sha256). */
int rwy_bundle_same(const rwy_bundle_info_t *a, const rwy_bundle_info_t *b);

/* Write a bundle with an explicit sequence to a specific base block. */
int rwy_bundle_write_copy(const rwy_block_dev_t *dev, uint32_t base, uint32_t blocks,
                          const rwy_bundle_spec_t *spec, const rwy_bundle_payload_t *parts,
                          uint32_t count, uint64_t seq);

/* Write a bundle into the inactive copy. */
int rwy_bundle_write(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                     const rwy_bundle_spec_t *spec, const rwy_bundle_payload_t *parts, uint32_t count);

#endif /* RWY_BUNDLE_H */
