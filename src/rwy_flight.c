/*
 * rwy_flight.c — The plaintext flight recorder ring buffer.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "rwy_internal.h"
#include "rwy_flight.h"

const char *rwy_flight_reason(int status) {
    switch (status) {
    case RWY_FLIGHT_OK: return "valid";
    case RWY_FLIGHT_EMPTY: return "empty";
    case RWY_FLIGHT_BAD_HEADER: return "bad header";
    case RWY_FLIGHT_IO: return "I/O error";
    default: return "unknown";
    }
}

const char *rwy_flight_event_name(uint8_t event) {
    switch (event) {
    case RWY_FLIGHT_EVENT_BOOT: return "BOOT";
    case RWY_FLIGHT_EVENT_CONDEMN: return "CONDEMN";
    case RWY_FLIGHT_EVENT_CONFIRM: return "CONFIRM";
    case RWY_FLIGHT_EVENT_FAIL: return "FAIL";
    case RWY_FLIGHT_EVENT_TRIES: return "TRIES";
    case RWY_FLIGHT_EVENT_RECOVER_NEXT: return "RECOVER_NEXT";
    case RWY_FLIGHT_EVENT_LOOP_LIMIT: return "LOOP_LIMIT";
    case RWY_FLIGHT_EVENT_STORE: return "STORE";
    default: return "UNKNOWN";
    }
}

uint32_t rwy_flight_capacity(uint32_t blocks) {
    if (!blocks) return 0;
    return (uint32_t)(blocks * (RWY_BLOCK_SIZE / sizeof(rwy_flight_entry_t)) - 1);
}

static void entry_coords(uint32_t slot, uint32_t *block_idx, size_t *offset_in_block) {
    uint64_t off = (uint64_t)(slot + 1) * sizeof(rwy_flight_entry_t);
    *block_idx = (uint32_t)(off / RWY_BLOCK_SIZE);
    *offset_in_block = (size_t)(off % RWY_BLOCK_SIZE);
}

static int is_all_zero(const uint8_t blk[RWY_BLOCK_SIZE]) {
    for (size_t i = 0; i < RWY_BLOCK_SIZE; i++) {
        if (blk[i] != 0) return 0;
    }
    return 1;
}

static int copy_check(const uint8_t blk[RWY_BLOCK_SIZE], unsigned c, uint32_t blocks,
                      rwy_flight_header_t *hdr_out) {
    const rwy_blk_header_t *h = (const rwy_blk_header_t *)blk;
    const rwy_flight_header_t *fl = (const rwy_flight_header_t *)blk;

    if (is_all_zero(blk)) return RWY_FLIGHT_EMPTY;
    if (h->magic != RWY_BLK_MAGIC || h->kind != RWY_BLK_KIND_FLIGHT || h->index != c || h->generation == 0)
        return RWY_FLIGHT_BAD_HEADER;
    if (rwy_blk_compute_crc(blk) != h->crc32c)
        return RWY_FLIGHT_BAD_HEADER;
    if (fl->capacity != rwy_flight_capacity(blocks) || fl->count > fl->capacity ||
        (fl->capacity > 0 && fl->head >= fl->capacity))
        return RWY_FLIGHT_BAD_HEADER;
    memcpy(hdr_out, fl, sizeof(*hdr_out));
    return RWY_FLIGHT_OK;
}

static void scan_copies(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                        rwy_flight_info_t *info) {
    uint8_t blk[RWY_BLOCK_SIZE];
    rwy_flight_header_t hdr;

    for (unsigned c = 0; c < 2; c++) {
        if (rwy_blk_read(dev, RWY_IO_META, start + c * blocks, blk)) {
            info->status[c] = RWY_FLIGHT_IO;
            continue;
        }
        info->status[c] = copy_check(blk, c, blocks, &hdr);
        if (info->status[c] == RWY_FLIGHT_OK) info->seq[c] = hdr.hdr.generation;
    }
}

static unsigned choose_winner(const rwy_flight_info_t *info) {
    if (info->status[0] == RWY_FLIGHT_OK && info->status[1] == RWY_FLIGHT_OK)
        return info->seq[1] > info->seq[0] ? 1 : 0;
    return info->status[0] == RWY_FLIGHT_OK ? 0 : 1;
}

int rwy_flight_pick(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                     rwy_flight_info_t *info) {
    uint8_t blk[RWY_BLOCK_SIZE];
    rwy_flight_header_t hdr;
    unsigned winner;

    if (!dev || !info || !blocks) return RWY_ERR_INVAL;
    memset(info, 0, sizeof(*info));
    scan_copies(dev, start, blocks, info);

    if (info->status[0] != RWY_FLIGHT_OK && info->status[1] != RWY_FLIGHT_OK)
        return (info->status[0] == RWY_FLIGHT_IO || info->status[1] == RWY_FLIGHT_IO)
               ? RWY_ERR_IO : RWY_ERR_CORRUPT;

    winner = choose_winner(info);
    if (rwy_blk_read(dev, RWY_IO_META, start + winner * blocks, blk)) return RWY_ERR_IO;
    memcpy(&hdr, blk, sizeof(hdr));
    info->copy = winner;
    info->sequence = hdr.hdr.generation;
    info->count = hdr.count;
    info->head = hdr.head;
    info->capacity = hdr.capacity;
    return RWY_OK;
}

int rwy_flight_read_entry(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                           const rwy_flight_info_t *info, uint32_t index,
                           rwy_flight_entry_t *entry) {
    uint8_t blk[RWY_BLOCK_SIZE];
    uint32_t slot, b;
    size_t off;

    if (!dev || !info || !entry || index >= info->count || !info->capacity)
        return RWY_ERR_INVAL;

    slot = (info->head - info->count + index + info->capacity) % info->capacity;
    entry_coords(slot, &b, &off);
    if (rwy_blk_read(dev, RWY_IO_META, start + info->copy * blocks + b, blk)) return RWY_ERR_IO;
    memcpy(entry, blk + off, sizeof(*entry));
    return RWY_OK;
}

uint32_t rwy_flight_count_committed(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                                     const rwy_flight_info_t *info, uint64_t max_rec_seq) {
    uint32_t valid = 0;

    if (!dev || !info) return 0;
    if (max_rec_seq == 0) return info->count;
    for (uint32_t i = 0; i < info->count; i++) {
        rwy_flight_entry_t e;
        if (rwy_flight_read_entry(dev, start, blocks, info, i, &e) == RWY_OK) {
            if (e.rec_seq <= max_rec_seq) valid++;
        }
    }
    return valid;
}

int rwy_flight_read_committed(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                               const rwy_flight_info_t *info, uint64_t max_rec_seq,
                               uint32_t index, rwy_flight_entry_t *entry) {
    uint32_t match = 0;

    if (!dev || !info || !entry) return RWY_ERR_INVAL;
    if (max_rec_seq == 0) return rwy_flight_read_entry(dev, start, blocks, info, index, entry);
    for (uint32_t i = 0; i < info->count; i++) {
        rwy_flight_entry_t e;
        if (rwy_flight_read_entry(dev, start, blocks, info, i, &e) == RWY_OK) {
            if (e.rec_seq <= max_rec_seq) {
                if (match == index) { *entry = e; return RWY_OK; }
                match++;
            }
        }
    }
    return RWY_ERR_NOENT;
}

uint32_t rwy_flight_tail(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                         uint64_t max_rec_seq, uint32_t n, rwy_flight_entry_t *out) {
    rwy_flight_info_t info;
    uint32_t got = 0;

    if (!dev || !out || rwy_flight_pick(dev, start, blocks, &info) != RWY_OK) return 0;
    for (uint32_t i = info.count; i-- > 0 && got < n;) {
        rwy_flight_entry_t e;

        if (rwy_flight_read_entry(dev, start, blocks, &info, i, &e) != RWY_OK) continue;
        if (max_rec_seq && e.rec_seq > max_rec_seq) continue;
        out[got++] = e;
    }
    for (uint32_t a = 0; a < got / 2; a++) {
        rwy_flight_entry_t t = out[a];

        out[a] = out[got - 1 - a];
        out[got - 1 - a] = t;
    }
    return got;
}

static int write_payload_blocks(const rwy_block_dev_t *dev, uint32_t start, unsigned cur,
                                unsigned next, uint32_t blocks, int has_prev,
                                uint32_t write_slot, const rwy_flight_entry_t *e) {
    for (uint32_t b = 1; b < blocks; b++) {
        uint8_t blk[RWY_BLOCK_SIZE];
        uint32_t slot_b;
        size_t slot_off;

        if (has_prev) {
            if (rwy_blk_read(dev, RWY_IO_META, start + cur * blocks + b, blk)) return RWY_ERR_IO;
        } else {
            memset(blk, 0, sizeof(blk));
        }
        entry_coords(write_slot, &slot_b, &slot_off);
        if (slot_b == b) memcpy(blk + slot_off, e, sizeof(*e));
        if (rwy_blk_write(dev, RWY_IO_META, start + next * blocks + b, blk)) return RWY_ERR_IO;
    }
    if (blocks > 1) {
        if (rwy_dev_flush(dev)) return RWY_ERR_IO;
    }
    return RWY_OK;
}

static int write_header_block(const rwy_block_dev_t *dev, uint32_t start, unsigned cur,
                              unsigned next, uint32_t blocks, int has_prev,
                              uint32_t write_slot, const rwy_flight_entry_t *e,
                              uint64_t target_seq, uint32_t new_count, uint32_t new_head,
                              uint32_t cap) {
    uint8_t blk[RWY_BLOCK_SIZE];
    rwy_flight_header_t *h = (rwy_flight_header_t *)blk;
    uint32_t slot_b;
    size_t slot_off;

    if (has_prev) {
        if (rwy_blk_read(dev, RWY_IO_META, start + cur * blocks, blk)) return RWY_ERR_IO;
    } else {
        memset(blk, 0, sizeof(blk));
    }
    entry_coords(write_slot, &slot_b, &slot_off);
    if (slot_b == 0) memcpy(blk + slot_off, e, sizeof(*e));
    rwy_blk_hdr_make(&h->hdr, target_seq, next, RWY_BLK_KIND_FLIGHT);
    h->count = new_count;
    h->head = new_head;
    h->capacity = cap;
    memset(h->reserved, 0, sizeof(h->reserved));
    h->hdr.crc32c = rwy_blk_compute_crc(blk);
    if (rwy_blk_write(dev, RWY_IO_META, start + next * blocks, blk)) return RWY_ERR_IO;
    return rwy_dev_flush(dev);
}

int rwy_flight_append(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                       const rwy_flight_entry_t *entry) {
    rwy_flight_info_t info;
    rwy_flight_entry_t e;
    uint32_t cap, write_slot, new_count, new_head;
    uint64_t target_seq;
    unsigned cur, next;
    int rc, has_prev;

    if (!dev || !blocks || !entry) return RWY_ERR_INVAL;
    cap = rwy_flight_capacity(blocks);
    if (!cap) return RWY_ERR_NOSPC;

    rc = rwy_flight_pick(dev, start, blocks, &info);
    if (rc == RWY_OK) {
        has_prev = 1;
        cur = info.copy;
        next = cur ^ 1;
        target_seq = info.sequence + 1;
        write_slot = info.head;
        new_count = info.count < cap ? info.count + 1 : cap;
        new_head = (info.head + 1) % cap;
    } else {
        has_prev = 0;
        cur = 0;
        next = 0;
        target_seq = 1;
        write_slot = 0;
        new_count = 1;
        new_head = 1 % cap;
    }

    e = *entry;
    if (e.seq == 0) {
        if (has_prev && info.count > 0) {
            rwy_flight_entry_t last;
            if (rwy_flight_read_entry(dev, start, blocks, &info, info.count - 1, &last) == RWY_OK)
                e.seq = last.seq + 1;
            else
                e.seq = 1;
        } else {
            e.seq = 1;
        }
    }

    rc = write_payload_blocks(dev, start, cur, next, blocks, has_prev, write_slot, &e);
    if (rc) return rc;
    return write_header_block(dev, start, cur, next, blocks, has_prev, write_slot, &e,
                              target_seq, new_count, new_head, cap);
}

static char *fmt_u64(char *p, uint64_t v) {
    char tmp[24];
    int i = 0;
    do { tmp[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (i > 0) *p++ = tmp[--i];
    return p;
}

static char *fmt_str(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}

size_t rwy_flight_format_entry(const rwy_flight_entry_t *e, char *buf, size_t bufsz) {
    char tmp[160];
    char *p = tmp;
    const char *ev = rwy_flight_event_name(e->event);
    const char *reason;

    switch (e->reason) {
    case RWY_SS_REASON_NORMAL: reason = "normal"; break;
    case RWY_SS_REASON_REQUESTED: reason = "requested"; break;
    case RWY_SS_REASON_ALL_FAILED: reason = "all_failed"; break;
    case RWY_SS_REASON_INVALID_STORE: reason = "invalid_store"; break;
    case RWY_SS_REASON_BOOT_LOOP: reason = "boot_loop"; break;
    default: reason = "none"; break;
    }

    p = fmt_str(p, "seq=");
    p = fmt_u64(p, e->seq);
    p = fmt_str(p, " rec_seq=");
    p = fmt_u64(p, e->rec_seq);
    p = fmt_str(p, " event=");
    p = fmt_str(p, ev);
    p = fmt_str(p, " slot=");
    p = fmt_str(p, e->booted[0] ? e->booted : "-");
    p = fmt_str(p, " reason=");
    p = fmt_str(p, reason);
    p = fmt_str(p, " epoch=");
    p = fmt_u64(p, e->epoch);
    p = fmt_str(p, " rev_hw=");
    p = fmt_u64(p, e->rev_hw);
    *p++ = '\n';

    size_t len = (size_t)(p - tmp);
    if (buf && bufsz > 0) {
        size_t copy = len < bufsz ? len : bufsz;
        memcpy(buf, tmp, copy);
    }
    return len;
}

int rwy_flight_severity(uint8_t event, uint8_t reason) {
    switch (event) {
    case RWY_FLIGHT_EVENT_NONE:
    case RWY_FLIGHT_EVENT_CONFIRM:
        return RWY_FLIGHT_SEV_NORMAL;
    case RWY_FLIGHT_EVENT_BOOT:
        switch (reason) {
        case RWY_SS_REASON_NONE:
        case RWY_SS_REASON_NORMAL:
            return RWY_FLIGHT_SEV_NORMAL;
        case RWY_SS_REASON_ALL_FAILED:
        case RWY_SS_REASON_INVALID_STORE:
            return RWY_FLIGHT_SEV_ERROR;
        default:
            return RWY_FLIGHT_SEV_WARNING;
        }
    case RWY_FLIGHT_EVENT_CONDEMN:
    case RWY_FLIGHT_EVENT_FAIL:
    case RWY_FLIGHT_EVENT_STORE:
        return RWY_FLIGHT_SEV_ERROR;
    case RWY_FLIGHT_EVENT_TRIES:
    case RWY_FLIGHT_EVENT_RECOVER_NEXT:
    case RWY_FLIGHT_EVENT_LOOP_LIMIT:
        return RWY_FLIGHT_SEV_WARNING;
    default:
        return RWY_FLIGHT_SEV_ERROR;
    }
}

/* Facility local0 (16), as RFC 5424 PRI = facility * 8 + severity. */
#define SYSLOG_PRI_LOCAL0 (16U * 8U)

static char *put_bytes(char *p, const char *end, const char *s, size_t n) {
    while (n-- && p < end) *p++ = *s++;
    return p;
}

static char *put_str(char *p, const char *end, const char *s) {
    return put_bytes(p, end, s, strlen(s));
}

size_t rwy_flight_format_syslog(const rwy_flight_entry_t *e, const char *timestamp, const char *host,
                                char buf[RWY_FLIGHT_SYSLOG_MAX]) {
    char msg[160];
    char *end = buf + RWY_FLIGHT_SYSLOG_MAX;
    char *p = buf;
    int sev = rwy_flight_severity(e->event, e->reason);
    size_t len;

    if (sev == RWY_FLIGHT_SEV_NORMAL) return 0;
    len = rwy_flight_format_entry(e, msg, sizeof(msg));

    p = fmt_str(p, "<");
    p = fmt_u64(p, SYSLOG_PRI_LOCAL0 + (uint64_t)sev);
    p = fmt_str(p, ">1 ");
    p = put_str(p, end, timestamp ? timestamp : "-");
    p = put_str(p, end, " ");
    p = put_str(p, end, host ? host : "-");
    p = put_str(p, end, " runway - ");
    p = put_str(p, end, rwy_flight_event_name(e->event));
    p = put_str(p, end, " - ");
    /* The formatted entry ends with a newline, which a syslog message does not carry. */
    p = put_bytes(p, end, msg, len - 1);
    return (size_t)(p - buf);
}

int rwy_flight_record(rwy_store_t *s, const uint8_t *rec, const char *slot,
                       uint8_t event, uint8_t reason, uint64_t rec_seq) {
    rwy_flight_entry_t e;
    const uint8_t *r;
    size_t i = 0;

    int64_t sec = 0;
    uint32_t ns = 0;

    if (!s) return RWY_OK;
    memset(&e, 0, sizeof(e));
    e.rec_seq = rec_seq ? rec_seq : s->rec_seq;
    e.generation = s->in_txn ? s->w_generation : s->sb.generation;
    rwy_now_of(s, &sec, &ns);
    e.timestamp = sec > 0 ? (uint64_t)sec : 0;
    e.event = event;
    e.reason = reason;
    r = rec ? rec : s->rec;
    e.epoch = (uint32_t)rwy_ss_epoch(r);
    e.rev_hw = (uint32_t)rwy_ss_rev_hw(r);
    if (slot) {
        while (slot[i] && i < sizeof(e.booted) - 1) {
            e.booted[i] = slot[i];
            i++;
        }
        e.booted[i] = 0;
    }
    if (rwy_flight_severity(event, reason) != RWY_FLIGHT_SEV_NORMAL &&
        s->flight_boot_count < RWY_FLIGHT_BOOT_MAX)
        s->flight_boot[s->flight_boot_count++] = e;
    if (!s->sb.flight_blocks) return RWY_OK;
    return rwy_flight_append(&s->dev, s->sb.flight_start, s->sb.flight_blocks, &e);
}

static void vread_slice(const char *line, size_t line_len, uint64_t off,
                        uint64_t pos, void *buf, size_t req_len, size_t *got) {
    size_t start_in_line, copy_bytes;

    if (!buf || *got >= req_len || off + line_len <= pos || off >= pos + req_len) return;
    start_in_line = pos > off ? (size_t)(pos - off) : 0;
    copy_bytes = line_len - start_in_line;
    if (*got + copy_bytes > req_len) copy_bytes = req_len - *got;
    memcpy((char *)buf + *got, line + start_in_line, copy_bytes);
    *got += copy_bytes;
}

int rwy_flight_vread(rwy_store_t *s, uint64_t pos, void *buf, size_t *len, size_t *total_sz) {
    rwy_flight_info_t info;
    uint32_t count;
    uint64_t off = 0;
    size_t got = 0, req_len = (buf && len) ? *len : 0;

    if (!s || !s->sb.flight_blocks || rwy_flight_pick(&s->dev, s->sb.flight_start, s->sb.flight_blocks, &info)) {
        if (total_sz) *total_sz = 0;
        if (len) *len = 0;
        return RWY_OK;
    }

    count = rwy_flight_count_committed(&s->dev, s->sb.flight_start, s->sb.flight_blocks,
                                        &info, s->rec_seq);
    for (int32_t i = (int32_t)count - 1; i >= 0; i--) {
        rwy_flight_entry_t e;
        char line[160];
        size_t line_len;

        if (rwy_flight_read_committed(&s->dev, s->sb.flight_start, s->sb.flight_blocks,
                                       &info, s->rec_seq, (uint32_t)i, &e))
            continue;
        line_len = rwy_flight_format_entry(&e, line, sizeof(line));
        vread_slice(line, line_len, off, pos, buf, req_len, &got);
        off += line_len;
    }

    if (total_sz) *total_sz = (size_t)off;
    if (len) *len = got;
    return RWY_OK;
}
