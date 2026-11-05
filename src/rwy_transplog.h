/*
 * rwy_transplog.h — The encrypted rolling update journal.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_TRANSPLOG_H
#define RWY_TRANSPLOG_H

#include "rwy_block.h"
#include "rwy_format.h"

/* Why a journal copy is not valid (status in rwy_transplog_info_t). */
#define RWY_TRANSPLOG_OK          0
#define RWY_TRANSPLOG_EMPTY       1   /* all zero: never written */
#define RWY_TRANSPLOG_BAD_HEADER  2   /* version, magic, kind, copy index, CRC, capacity or count */
#define RWY_TRANSPLOG_IO          3   /* read failed */

typedef struct {
    int      status[2];       /* RWY_TRANSPLOG_* for each copy */
    uint64_t seq[2];          /* sequence (generation) of each valid copy */
    unsigned copy;            /* winning copy (0 or 1), valid when rc == RWY_OK */
    uint64_t sequence;        /* sequence of winning copy */
    uint32_t count;           /* valid entries in winning copy */
    uint32_t head;            /* head ring slot in winning copy */
    uint32_t capacity;        /* capacity of winning copy */
    uint8_t  head_hash[32];   /* entry_hash of newest entry (or 0) */
} rwy_transplog_info_t;

const char *rwy_transplog_reason(int status);
const char *rwy_transplog_event_name(uint8_t event);
uint32_t rwy_transplog_capacity(uint32_t blocks);

/* Inspect both copies and pick the newest valid one.
 * Returns RWY_OK on success, RWY_ERR_CORRUPT if neither copy is valid,
 * or RWY_ERR_IO on I/O error. In all cases, info is populated. */
int rwy_transplog_pick(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                        rwy_transplog_info_t *info);

/* Append an entry to the journal using A/B ping-pong update.
 * If neither copy was valid, initializes copy 0 with the entry.
 * If entry->seq is 0, advances sequentially from the newest entry in the active copy.
 * Wraps around when capacity is reached, overwriting the oldest entry.
 * Automatically computes prev_hash and entry_hash.
 * Returns RWY_OK on success. */
int rwy_transplog_append(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                          rwy_journal_entry_t *entry);

/* Read entry at logical index (0 = oldest, count - 1 = newest) from the winning copy.
 * Returns RWY_OK on success, RWY_ERR_INVAL if index >= info->count, RWY_ERR_CORRUPT if entry hash fails. */
int rwy_transplog_read_entry(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                              const rwy_transplog_info_t *info, uint32_t index,
                              rwy_journal_entry_t *entry);

/* Count committed entries in winning copy, filtering uncommitted entries:
 * If max_gen > 0, skips entry if entry.generation > max_gen.
 * If max_rec_seq > 0, skips entry if entry.rec_seq > max_rec_seq. */
uint32_t rwy_transplog_count_committed(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                                        const rwy_transplog_info_t *info,
                                        uint64_t max_gen, uint64_t max_rec_seq);

/* Read committed entry at logical index (0 = oldest committed, committed_count - 1 = newest).
 * Skips uncommitted entries using max_gen and max_rec_seq.
 * Returns RWY_OK on success, RWY_ERR_NOENT if index >= committed count. */
int rwy_transplog_read_committed(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                                  const rwy_transplog_info_t *info,
                                  uint64_t max_gen, uint64_t max_rec_seq,
                                  uint32_t index, rwy_journal_entry_t *entry);

/* Verify the hash chain and entry integrity across all entries in the winning copy.
 * Checks each entry's entry_hash, checks that prev_hash links consecutive entries,
 * and checks that head_hash matches the newest entry's entry_hash.
 * If max_epoch > 0, also checks that no entry's epoch exceeds max_epoch.
 * Returns RWY_OK on success, RWY_ERR_CORRUPT on broken chain/hash, RWY_ERR_RANGE on epoch violation. */
int rwy_transplog_verify_chain(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                                const rwy_transplog_info_t *info, uint64_t max_epoch);

/* Compute payload digest for a slot over sorted payload files:
 * ARGS, DTB, INITRD, KERNEL, LABEL.
 * Sets out to 32-byte SHA-256 digest. */
int rwy_transplog_calc_payload_hash(rwy_store_t *s, const char *slot_path, uint8_t out[32]);

/* Record an administrative state change into the journal area if configured. Non-blocking. */
int rwy_transplog_record(rwy_store_t *s, const char *slot, uint8_t event,
                          uint64_t revision, const char *label, const uint8_t payload_hash[32],
                          uint64_t rec_seq, const char *caller);

/* Record an EVICT event: the slot's state, why it went (RWY_EVICT_REASON_*) and the blocks its removal frees. */
int rwy_transplog_record_evict(rwy_store_t *s, const char *slot, uint64_t revision, const char *label,
                               const uint8_t payload_hash[32], uint8_t state, uint8_t reason, uint32_t blocks);

/* Re-encrypt the journal area to the target device during relocation. */
int rwy_transplog_relocate(rwy_store_t *s, const rwy_block_dev_t *to);

/* Scrub the inactive copy of the journal after re-encryption finalization. */
int rwy_transplog_scrub_inactive(rwy_store_t *s);

#endif /* RWY_TRANSPLOG_H */
