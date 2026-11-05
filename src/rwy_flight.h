/*
 * rwy_flight.h — The plaintext flight recorder ring buffer.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_FLIGHT_H
#define RWY_FLIGHT_H

#include "rwy_block.h"
#include "rwy_format.h"

/* Why a flight copy is not valid (status in rwy_flight_info_t). */
#define RWY_FLIGHT_OK          0
#define RWY_FLIGHT_EMPTY       1   /* all zero: never written */
#define RWY_FLIGHT_BAD_HEADER  2   /* version, magic, kind, copy index, CRC, capacity or count */
#define RWY_FLIGHT_IO          3   /* read failed */

typedef struct {
    int      status[2];   /* RWY_FLIGHT_* for each copy */
    uint64_t seq[2];      /* sequence (generation) of each valid copy */
    unsigned copy;        /* winning copy (0 or 1), valid when rc == RWY_OK */
    uint64_t sequence;    /* sequence of winning copy */
    uint32_t count;       /* valid entries in winning copy */
    uint32_t head;        /* head ring slot in winning copy */
    uint32_t capacity;    /* capacity of winning copy */
} rwy_flight_info_t;

const char *rwy_flight_reason(int status);
const char *rwy_flight_event_name(uint8_t event);
uint32_t rwy_flight_capacity(uint32_t blocks);

/* Inspect both copies and pick the newest valid one.
 * Returns RWY_OK on success, RWY_ERR_CORRUPT if neither copy is valid,
 * or RWY_ERR_IO on I/O error. In all cases, info is populated. */
int rwy_flight_pick(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                     rwy_flight_info_t *info);

/* Append an entry to the flight recorder using A/B ping-pong update.
 * If neither copy was valid, initializes copy 0 with the entry.
 * If entry->seq is 0, advances sequentially from the newest entry in the active copy.
 * Wraps around when capacity is reached, overwriting the oldest entry.
 * Returns RWY_OK on success. */
int rwy_flight_append(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                       const rwy_flight_entry_t *entry);

/* Read entry at logical index (0 = oldest, count - 1 = newest) from the winning copy.
 * Returns RWY_OK on success, RWY_ERR_INVAL if index >= info->count. */
int rwy_flight_read_entry(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                           const rwy_flight_info_t *info, uint32_t index,
                           rwy_flight_entry_t *entry);

/* Count committed entries in winning copy, filtering uncommitted entries if max_rec_seq > 0.
 * If max_rec_seq == 0, returns info->count. */
uint32_t rwy_flight_count_committed(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                                     const rwy_flight_info_t *info, uint64_t max_rec_seq);

/* Read committed entry at logical index (0 = oldest committed, committed_count - 1 = newest).
 * If max_rec_seq > 0, skips any entries with entry.rec_seq > max_rec_seq.
 * Returns RWY_OK on success, RWY_ERR_NOENT if index >= committed count. */
int rwy_flight_read_committed(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                               const rwy_flight_info_t *info, uint64_t max_rec_seq,
                               uint32_t index, rwy_flight_entry_t *entry);

/* Copy the newest n committed entries of the ring to out, oldest first, with the filter of
 * rwy_flight_read_committed. Returns the number copied, 0 when the ring cannot be read. */
uint32_t rwy_flight_tail(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                         uint64_t max_rec_seq, uint32_t n, rwy_flight_entry_t *out);

/* Format a flight entry to textual representation. Returns total formatted length. */
size_t rwy_flight_format_entry(const rwy_flight_entry_t *e, char *buf, size_t bufsz);

/* Report severity of an event: RWY_FLIGHT_SEV_NORMAL, or an RFC 5424 severity. */
#define RWY_FLIGHT_SEV_NORMAL  (-1)
#define RWY_FLIGHT_SEV_ERROR   3
#define RWY_FLIGHT_SEV_WARNING 4
int rwy_flight_severity(uint8_t event, uint8_t reason);

/* RFC 5424 line of a not-normal entry, for one datagram. NULL timestamp or host is "-".
 * The line never exceeds RWY_FLIGHT_SYSLOG_MAX bytes: a long message is cut.
 * Returns the length written, or 0 for a normal event. */
#define RWY_FLIGHT_SYSLOG_MAX 480
size_t rwy_flight_format_syslog(const rwy_flight_entry_t *e, const char *timestamp, const char *host,
                                char buf[RWY_FLIGHT_SYSLOG_MAX]);

/* Record an event into the flight recorder area if configured. Non-blocking.
 * Not-normal events of this boot are also kept in the store for the report. */
int rwy_flight_record(rwy_store_t *s, const uint8_t *rec, const char *slot,
                       uint8_t event, uint8_t reason, uint64_t rec_seq);

/* Virtual file reader for EFI/RUNWAY/FLIGHT in reverse chronological order. */
int rwy_flight_vread(rwy_store_t *s, uint64_t pos, void *buf, size_t *len, size_t *total_sz);

#endif /* RWY_FLIGHT_H */
