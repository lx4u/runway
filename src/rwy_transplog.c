/*
 * rwy_transplog.c — The encrypted rolling update journal.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "rwy_core.h"
#include "rwy_crypto.h"
#include "rwy_format.h"
#include "rwy_internal.h"
#include "rwy_string.h"
#include "rwy_bootcfg.h"
#include "rwy_transplog.h"

const char *rwy_transplog_reason(int status) {
    switch (status) {
    case RWY_TRANSPLOG_OK: return "valid";
    case RWY_TRANSPLOG_EMPTY: return "empty";
    case RWY_TRANSPLOG_BAD_HEADER: return "bad header";
    case RWY_TRANSPLOG_IO: return "I/O error";
    default: return "unknown";
    }
}

const char *rwy_transplog_event_name(uint8_t event) {
    switch (event) {
    case RWY_JOURNAL_EVENT_INSTALL: return "INSTALL";
    case RWY_JOURNAL_EVENT_UPDATE: return "UPDATE";
    case RWY_JOURNAL_EVENT_CONFIRM: return "CONFIRM";
    case RWY_JOURNAL_EVENT_FAIL: return "FAIL";
    case RWY_JOURNAL_EVENT_REMOVE: return "REMOVE";
    case RWY_JOURNAL_EVENT_EVICT: return "EVICT";
    default: return "UNKNOWN";
    }
}

uint32_t rwy_transplog_capacity(uint32_t blocks) {
    return blocks * 10;
}

static void entry_coords(uint32_t slot, uint32_t *block_idx, size_t *offset_in_block) {
    *block_idx = slot / 10;
    *offset_in_block = 256 + (size_t)(slot % 10) * sizeof(rwy_journal_entry_t);
}

static int is_all_zero(const uint8_t blk[RWY_BLOCK_SIZE]) {
    for (size_t i = 0; i < RWY_BLOCK_SIZE; i++) {
        if (blk[i] != 0) return 0;
    }
    return 1;
}

static int copy_check(const uint8_t blk[RWY_BLOCK_SIZE], unsigned c, uint32_t blocks,
                      rwy_journal_header_t *hdr_out) {
    const rwy_blk_header_t *h = (const rwy_blk_header_t *)blk;
    const rwy_journal_header_t *jh = (const rwy_journal_header_t *)blk;

    if (is_all_zero(blk)) return RWY_TRANSPLOG_EMPTY;
    if (h->magic != RWY_BLK_MAGIC || h->kind != RWY_BLK_KIND_JOURNAL ||
        h->index != c || h->generation == 0)
        return RWY_TRANSPLOG_BAD_HEADER;
    if (rwy_blk_compute_crc(blk) != h->crc32c)
        return RWY_TRANSPLOG_BAD_HEADER;
    if (jh->capacity != rwy_transplog_capacity(blocks) || jh->count > jh->capacity ||
        (jh->capacity > 0 && jh->head >= jh->capacity))
        return RWY_TRANSPLOG_BAD_HEADER;
    memcpy(hdr_out, jh, sizeof(*hdr_out));
    return RWY_TRANSPLOG_OK;
}

static void scan_copies(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                        rwy_transplog_info_t *info) {
    uint8_t blk[RWY_BLOCK_SIZE];
    rwy_journal_header_t hdr;

    for (unsigned c = 0; c < 2; c++) {
        if (rwy_blk_read(dev, RWY_IO_META, start + c * blocks, blk)) {
            info->status[c] = RWY_TRANSPLOG_IO;
            continue;
        }
        info->status[c] = copy_check(blk, c, blocks, &hdr);
        if (info->status[c] == RWY_TRANSPLOG_OK) info->seq[c] = hdr.hdr.generation;
    }
}

static unsigned choose_winner(const rwy_transplog_info_t *info) {
    if (info->status[0] == RWY_TRANSPLOG_OK && info->status[1] == RWY_TRANSPLOG_OK)
        return info->seq[1] > info->seq[0] ? 1 : 0;
    return info->status[0] == RWY_TRANSPLOG_OK ? 0 : 1;
}

int rwy_transplog_pick(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                        rwy_transplog_info_t *info) {
    uint8_t blk[RWY_BLOCK_SIZE];
    rwy_journal_header_t hdr;
    unsigned winner;

    if (!dev || !info || !blocks) return RWY_ERR_INVAL;
    memset(info, 0, sizeof(*info));
    scan_copies(dev, start, blocks, info);

    if (info->status[0] != RWY_TRANSPLOG_OK && info->status[1] != RWY_TRANSPLOG_OK)
        return (info->status[0] == RWY_TRANSPLOG_IO || info->status[1] == RWY_TRANSPLOG_IO)
               ? RWY_ERR_IO : RWY_ERR_CORRUPT;

    winner = choose_winner(info);
    if (rwy_blk_read(dev, RWY_IO_META, start + winner * blocks, blk)) return RWY_ERR_IO;
    memcpy(&hdr, blk, sizeof(hdr));
    info->copy = winner;
    info->sequence = hdr.hdr.generation;
    info->count = hdr.count;
    info->head = hdr.head;
    info->capacity = hdr.capacity;
    memcpy(info->head_hash, hdr.head_hash, sizeof(info->head_hash));
    return RWY_OK;
}

static void calc_entry_hash(const rwy_journal_entry_t *entry, uint8_t out[32]) {
    rwy_sha256(entry, offsetof(rwy_journal_entry_t, entry_hash), out);
}

static int verify_entry_hash(const rwy_journal_entry_t *entry) {
    uint8_t expected[32];
    calc_entry_hash(entry, expected);
    return CompareMem(expected, entry->entry_hash, sizeof(expected)) == 0;
}

int rwy_transplog_read_entry(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                              const rwy_transplog_info_t *info, uint32_t index,
                              rwy_journal_entry_t *entry) {
    uint8_t blk[RWY_BLOCK_SIZE];
    uint32_t slot, b;
    size_t off;

    if (!dev || !info || !entry || index >= info->count || !info->capacity)
        return RWY_ERR_INVAL;

    slot = (info->head - info->count + index + info->capacity) % info->capacity;
    entry_coords(slot, &b, &off);
    if (rwy_blk_read(dev, RWY_IO_META, start + info->copy * blocks + b, blk)) return RWY_ERR_IO;
    memcpy(entry, blk + off, sizeof(*entry));
    if (!verify_entry_hash(entry)) return RWY_ERR_CORRUPT;
    return RWY_OK;
}

uint32_t rwy_transplog_count_committed(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                                        const rwy_transplog_info_t *info,
                                        uint64_t max_gen, uint64_t max_rec_seq) {
    uint32_t valid = 0;

    if (!dev || !info) return 0;
    for (uint32_t i = 0; i < info->count; i++) {
        rwy_journal_entry_t e;
        if (rwy_transplog_read_entry(dev, start, blocks, info, i, &e) == RWY_OK) {
            if (max_gen > 0 && e.generation > max_gen) continue;
            if (max_rec_seq > 0 && e.rec_seq > max_rec_seq) continue;
            valid++;
        }
    }
    return valid;
}

int rwy_transplog_read_committed(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                                  const rwy_transplog_info_t *info,
                                  uint64_t max_gen, uint64_t max_rec_seq,
                                  uint32_t index, rwy_journal_entry_t *entry) {
    uint32_t match = 0;

    if (!dev || !info || !entry) return RWY_ERR_INVAL;
    for (uint32_t i = 0; i < info->count; i++) {
        rwy_journal_entry_t e;
        if (rwy_transplog_read_entry(dev, start, blocks, info, i, &e) == RWY_OK) {
            if (max_gen > 0 && e.generation > max_gen) continue;
            if (max_rec_seq > 0 && e.rec_seq > max_rec_seq) continue;
            if (match == index) { *entry = e; return RWY_OK; }
            match++;
        }
    }
    return RWY_ERR_NOENT;
}

int rwy_transplog_verify_chain(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                                const rwy_transplog_info_t *info, uint64_t max_epoch) {
    rwy_journal_entry_t prev_e;
    uint8_t zero_hash[32];

    if (!dev || !info) return RWY_ERR_INVAL;
    memset(zero_hash, 0, sizeof(zero_hash));
    if (info->count == 0)
        return CompareMem(info->head_hash, zero_hash, 32) == 0 ? RWY_OK : RWY_ERR_CORRUPT;

    for (uint32_t i = 0; i < info->count; i++) {
        rwy_journal_entry_t e;
        int rc = rwy_transplog_read_entry(dev, start, blocks, info, i, &e);
        if (rc) return rc;
        if (max_epoch > 0 && e.epoch > max_epoch) return RWY_ERR_RANGE;
        if (i > 0) {
            if (CompareMem(e.prev_hash, prev_e.entry_hash, 32) != 0) return RWY_ERR_CORRUPT;
        }
        if (i == info->count - 1) {
            if (CompareMem(info->head_hash, e.entry_hash, 32) != 0) return RWY_ERR_CORRUPT;
        }
        prev_e = e;
    }
    return RWY_OK;
}

static int write_payload_blocks(const rwy_block_dev_t *dev, uint32_t start, unsigned cur,
                                unsigned next, uint32_t blocks, int has_prev,
                                uint32_t write_slot, const rwy_journal_entry_t *e) {
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
                              uint32_t write_slot, const rwy_journal_entry_t *e,
                              uint64_t target_seq, uint32_t new_count, uint32_t new_head,
                              uint32_t cap) {
    uint8_t blk[RWY_BLOCK_SIZE];
    rwy_journal_header_t *h = (rwy_journal_header_t *)blk;
    uint32_t slot_b;
    size_t slot_off;

    if (has_prev) {
        if (rwy_blk_read(dev, RWY_IO_META, start + cur * blocks, blk)) return RWY_ERR_IO;
    } else {
        memset(blk, 0, sizeof(blk));
    }
    entry_coords(write_slot, &slot_b, &slot_off);
    if (slot_b == 0) memcpy(blk + slot_off, e, sizeof(*e));
    rwy_blk_hdr_make(&h->hdr, target_seq, next, RWY_BLK_KIND_JOURNAL);
    h->count = new_count;
    h->head = new_head;
    h->capacity = cap;
    memcpy(h->head_hash, e->entry_hash, sizeof(h->head_hash));
    memset(h->reserved, 0, sizeof(h->reserved));
    h->hdr.crc32c = rwy_blk_compute_crc(blk);
    if (rwy_blk_write(dev, RWY_IO_META, start + next * blocks, blk)) return RWY_ERR_IO;
    return rwy_dev_flush(dev);
}

int rwy_transplog_append(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                          rwy_journal_entry_t *entry) {
    rwy_transplog_info_t info;
    rwy_journal_entry_t last;
    uint32_t cap, write_slot, new_count, new_head;
    uint64_t target_seq;
    unsigned cur, next;
    int rc, has_prev;

    if (!dev || !blocks || !entry) return RWY_ERR_INVAL;
    cap = rwy_transplog_capacity(blocks);
    if (!cap) return RWY_ERR_NOSPC;

    rc = rwy_transplog_pick(dev, start, blocks, &info);
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

    if (has_prev && info.count > 0) {
        if (rwy_transplog_read_entry(dev, start, blocks, &info, info.count - 1, &last) == RWY_OK) {
            if (entry->seq == 0) entry->seq = last.seq + 1;
            memcpy(entry->prev_hash, last.entry_hash, 32);
        } else {
            if (entry->seq == 0) entry->seq = 1;
            memset(entry->prev_hash, 0, 32);
        }
    } else {
        if (entry->seq == 0) entry->seq = 1;
        memset(entry->prev_hash, 0, 32);
    }

    calc_entry_hash(entry, entry->entry_hash);

    rc = write_payload_blocks(dev, start, cur, next, blocks, has_prev, write_slot, entry);
    if (rc) return rc;
    return write_header_block(dev, start, cur, next, blocks, has_prev, write_slot, entry,
                              target_seq, new_count, new_head, cap);
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    return c - 'a' + 10;
}

/* If `oid` has a content name in BLOBS/, its digest is that name: no need to read the data. */
static int blob_digest(rwy_store_t *s, uint64_t oid, uint8_t out[32]) {
    char bn[RWY_NAME_MAX + 1];
    uint64_t blobs, f;

    if (rwy_lookup_follow(s, "EFI/RUNWAY/BLOBS", 1, &blobs) != RWY_OK) return 0;
    for (size_t j = 0; rwy_readdir_nth(s, "EFI/RUNWAY/BLOBS", j, bn, sizeof(bn), 0) == RWY_OK; j++) {
        size_t l = strlen(bn);
        if (!rwy_bootcfg_content_name(bn, l)) continue;
        if (rwy_dir_lookup(s, blobs, bn, (uint32_t)l, &f, 0) != RWY_OK || f != oid) continue;
        for (size_t k = 0; k < 32; k++) out[k] = (uint8_t)(hex_val(bn[2 * k]) << 4 | hex_val(bn[2 * k + 1]));
        return 1;
    }
    return 0;
}

static int hash_one_payload(rwy_store_t *s, uint64_t dir, const char *name,
                            rwy_sha256_ctx_t *ctx, int *found_any) {
    uint8_t buf[RWY_BLOCK_SIZE];
    uint8_t digest[32];
    uint64_t file_oid, fsz, off = 0;
    const rwy_inode_t *in;
    uint32_t nl = (uint32_t)strlen(name);

    if (rwy_dir_lookup(s, dir, name, nl, &file_oid, 0) != RWY_OK) return RWY_OK;
    in = rwy_inode_of(s, file_oid, s->in_txn);
    if (!in || in->type != RWY_TYPE_FILE) return RWY_OK;

    *found_any = 1;
    rwy_sha256_update(ctx, name, (size_t)nl + 1);
    fsz = in->size;
    rwy_sha256_update(ctx, &fsz, sizeof(fsz));

    if (in->nlink > 1 && blob_digest(s, file_oid, digest)) {
        rwy_sha256_update(ctx, digest, sizeof(digest));
        return RWY_OK;
    }

    while (off < fsz) {
        size_t chunk = (size_t)(fsz - off);
        size_t got = 0;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        if (rwy_inode_pread(s, in, off, buf, chunk, &got) != RWY_OK || got != chunk)
            return RWY_ERR_IO;
        rwy_sha256_update(ctx, buf, got);
        off += got;
    }
    return RWY_OK;
}

static void make_slot_full_path(char *full, size_t full_sz, const char *slot_path) {
    size_t i = 0, j = 0;
    if (rwy_strncmp(slot_path, "EFI/RUNWAY/", 11) != 0 && slot_path[0] != '/') {
        const char *pfx = "EFI/RUNWAY/";
        while (pfx[j] && i + 1 < full_sz) full[i++] = pfx[j++];
    }
    j = 0;
    while (slot_path[j] && i + 1 < full_sz) full[i++] = slot_path[j++];
    full[i] = 0;
}

int rwy_transplog_calc_payload_hash(rwy_store_t *s, const char *slot_path, uint8_t out[32]) {
    static const char *const PAYLOAD_NAMES[] = { "ARGS", "DTB", "ENTRY", "INITRD", "KERNEL", "LABEL", "META" };
    char full[RWY_MAX_PATH + 1];
    rwy_sha256_ctx_t ctx;
    uint64_t dir;
    int found_any = 0, rc;

    if (!s || !slot_path || !out) return RWY_ERR_INVAL;
    memset(out, 0, 32);

    make_slot_full_path(full, sizeof(full), slot_path);

    if (rwy_lookup_follow(s, full, 1, &dir) != RWY_OK)
        return RWY_OK;

    rwy_sha256_init(&ctx);
    for (size_t i = 0; i < sizeof(PAYLOAD_NAMES) / sizeof(PAYLOAD_NAMES[0]); i++) {
        rc = hash_one_payload(s, dir, PAYLOAD_NAMES[i], &ctx, &found_any);
        if (rc) return rc;
    }

    if (found_any) rwy_sha256_final(&ctx, out);
    return RWY_OK;
}

static void copy_field(char *dst, size_t dst_sz, const char *src) {
    size_t i = 0;
    if (!dst || dst_sz == 0) return;
    if (src) {
        while (src[i] && i < dst_sz - 1) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = 0;
}

static int record_entry(rwy_store_t *s, const char *slot, uint8_t event,
                        uint64_t revision, const char *label, const uint8_t payload_hash[32],
                        uint64_t rec_seq, const char *caller, const uint8_t extra[6]) {
    rwy_journal_entry_t e;
    int64_t sec = 0;
    uint32_t ns = 0;

    if (!s || !s->sb.journal_blocks) return RWY_OK;
    memset(&e, 0, sizeof(e));
    e.generation = s->in_txn ? s->w_generation : s->sb.generation;
    e.rec_seq = rec_seq ? rec_seq : s->rec_seq;
    e.event = event;
    e.revision = revision;
    memcpy(e.store_uuid, s->sb.uuid, sizeof(e.store_uuid));
    e.epoch = rwy_ss_epoch(s->rec);
    rwy_now_of(s, &sec, &ns);
    e.timestamp = sec > 0 ? (uint64_t)sec : 0;

    copy_field(e.slot, sizeof(e.slot), slot);
    copy_field(e.label, sizeof(e.label), label);
    copy_field(e.caller, sizeof(e.caller), caller ? caller : "rwyctl");
    if (payload_hash) memcpy(e.payload_hash, payload_hash, sizeof(e.payload_hash));
    if (extra) memcpy(e.reserved, extra, 6);

    return rwy_transplog_append(&s->dev, s->sb.journal_start, s->sb.journal_blocks, &e);
}

int rwy_transplog_record(rwy_store_t *s, const char *slot, uint8_t event,
                          uint64_t revision, const char *label, const uint8_t payload_hash[32],
                          uint64_t rec_seq, const char *caller) {
    return record_entry(s, slot, event, revision, label, payload_hash, rec_seq, caller, NULL);
}

int rwy_transplog_record_evict(rwy_store_t *s, const char *slot, uint64_t revision, const char *label,
                               const uint8_t payload_hash[32], uint8_t state, uint8_t reason, uint32_t blocks) {
    uint8_t x[6] = { 0 };
    x[RWY_EVICT_STATE_OFF] = state;
    x[RWY_EVICT_REASON_OFF] = reason;
    for (unsigned i = 0; i < 4; i++) x[RWY_EVICT_BLOCKS_OFF + i] = (uint8_t)(blocks >> (8 * i));
    return record_entry(s, slot, RWY_JOURNAL_EVENT_EVICT, revision, label, payload_hash, s->rec_seq, NULL, x);
}

static int zero_journal_blocks(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks) {
    uint8_t zero[RWY_BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    for (uint32_t b = 0; b < blocks; b++) {
        int rc = rwy_blk_write(dev, RWY_IO_META, start + b, zero);
        if (rc) return rc;
    }
    return RWY_OK;
}

static int copy_journal_blocks(const rwy_block_dev_t *from, const rwy_block_dev_t *to,
                               uint32_t from_start, uint32_t to_start, uint32_t blocks,
                               unsigned new_idx) {
    uint8_t blk[RWY_BLOCK_SIZE];
    for (uint32_t b = 0; b < blocks; b++) {
        int rc = rwy_blk_read(from, RWY_IO_META, from_start + b, blk);
        if (rc) return rc;
        if (b == 0) {
            rwy_journal_header_t *h = (rwy_journal_header_t *)blk;
            h->hdr.index = new_idx;
            h->hdr.crc32c = rwy_blk_compute_crc(blk);
        }
        rc = rwy_blk_write(to, RWY_IO_META, to_start + b, blk);
        if (rc) return rc;
    }
    return RWY_OK;
}

int rwy_transplog_relocate(rwy_store_t *s, const rwy_block_dev_t *to) {
    rwy_transplog_info_t info;
    uint32_t jstart = s->sb.journal_start, jblocks = s->sb.journal_blocks;
    int rc;

    if (!jblocks) return RWY_OK;

    rc = rwy_transplog_pick(&s->dev, jstart, jblocks, &info);
    if (rc == RWY_OK) {
        unsigned cur = info.copy;
        unsigned inactive = cur ^ 1;
        rc = copy_journal_blocks(&s->dev, to, jstart + cur * jblocks,
                                 jstart + inactive * jblocks, jblocks, inactive);
        if (rc) return rc;
    } else {
        rc = zero_journal_blocks(to, jstart + jblocks, jblocks);
        if (rc) return rc;
    }
    return rwy_dev_flush(to);
}

int rwy_transplog_scrub_inactive(rwy_store_t *s) {
    rwy_transplog_info_t info;
    uint32_t jstart = s->sb.journal_start, jblocks = s->sb.journal_blocks;
    int rc;

    if (!jblocks) return RWY_OK;

    rc = rwy_transplog_pick(&s->dev, jstart, jblocks, &info);
    if (rc == RWY_OK) {
        unsigned inactive = info.copy ^ 1;
        rc = zero_journal_blocks(&s->dev, jstart + inactive * jblocks, jblocks);
        if (rc) return rc;
    } else {
        rc = zero_journal_blocks(&s->dev, jstart, jblocks);
        if (rc) return rc;
    }
    return rwy_dev_flush(&s->dev);
}
