/*
 * rwy_bundle.c — Generic flat bundle support: build, write, verify and pick.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"
#include "rwy_string.h"
#include "rwy_crypto.h"
#include "rwy_bundle.h"

#define BS ((uint64_t)RWY_BLOCK_SIZE)

static uint32_t part_blocks(uint32_t len) { return (uint32_t)(((uint64_t)len + BS - 1) / BS); }

static const rwy_bundle_kind_rule_t *find_rule(const rwy_bundle_spec_t *spec, uint32_t kind) {
    for (uint32_t i = 0; i < spec->num_rules; i++) {
        if (spec->rules[i].kind == kind) return &spec->rules[i];
    }
    return NULL;
}

static int check_min_counts(const rwy_bundle_spec_t *spec, const uint32_t counts[RWY_BUNDLE_MAX_ENTRIES]) {
    for (uint32_t i = 0; i < spec->num_rules; i++) {
        if (counts[i] < spec->rules[i].min_count) return -1;
    }
    return 0;
}

static int count_part(const rwy_bundle_spec_t *spec, const rwy_bundle_payload_t *p,
                      uint32_t counts[RWY_BUNDLE_MAX_ENTRIES], uint64_t *blocks) {
    const rwy_bundle_kind_rule_t *r = NULL;
    uint32_t r_idx;

    if (!p->data || p->len == 0) return -1;
    for (r_idx = 0; r_idx < spec->num_rules; r_idx++) {
        if (spec->rules[r_idx].kind == p->kind) {
            r = &spec->rules[r_idx];
            break;
        }
    }
    if (!r || (r->max_len && p->len > r->max_len)) return -1;
    counts[r_idx]++;
    if (counts[r_idx] > r->max_count) return -1;
    *blocks += part_blocks(p->len);
    return 0;
}

uint32_t rwy_bundle_blocks(const rwy_bundle_spec_t *spec, const rwy_bundle_payload_t *parts, uint32_t count) {
    uint32_t counts[RWY_BUNDLE_MAX_ENTRIES];
    uint64_t n = 1;

    if (!spec || !parts || count == 0 || count > spec->max_entries) return 0;
    if (spec->num_rules > RWY_BUNDLE_MAX_ENTRIES) return 0;
    memset(counts, 0, sizeof(counts));
    for (uint32_t i = 0; i < count; i++) {
        if (count_part(spec, &parts[i], counts, &n) < 0) return 0;
    }
    if (check_min_counts(spec, counts) < 0) return 0;
    return (uint32_t)n;
}

const char *rwy_bundle_reason(int status) {
    switch (status) {
    case RWY_BUNDLE_OK: return "valid";
    case RWY_BUNDLE_EMPTY: return "empty";
    case RWY_BUNDLE_BAD_HEADER: return "bad header";
    case RWY_BUNDLE_BAD_RANGE: return "bad range";
    case RWY_BUNDLE_BAD_HASH: return "bad hash";
    case RWY_BUNDLE_IO: return "I/O error";
    }
    return "unknown";
}

static int check_entry_rule(const rwy_bundle_spec_t *spec, const rwy_bundle_entry_t *e,
                            uint32_t counts[RWY_BUNDLE_MAX_ENTRIES]) {
    const rwy_bundle_kind_rule_t *r = NULL;
    uint32_t r_idx;

    if (e->reserved) return RWY_BUNDLE_BAD_HEADER;
    for (r_idx = 0; r_idx < spec->num_rules; r_idx++) {
        if (spec->rules[r_idx].kind == e->kind) {
            r = &spec->rules[r_idx];
            break;
        }
    }
    if (!r) return RWY_BUNDLE_BAD_HEADER;
    counts[r_idx]++;
    if (counts[r_idx] > r->max_count) return RWY_BUNDLE_BAD_HEADER;
    return RWY_BUNDLE_OK;
}

int rwy_bundle_check_header(const uint8_t blk[RWY_BLOCK_SIZE], const rwy_bundle_spec_t *spec) {
    const rwy_bundle_header_t *h = (const rwy_bundle_header_t *)blk;
    uint32_t crc, counts[RWY_BUNDLE_MAX_ENTRIES];

    if (!blk || !spec || spec->num_rules > RWY_BUNDLE_MAX_ENTRIES) return RWY_BUNDLE_BAD_HEADER;
    if (h->magic != spec->magic) return RWY_BUNDLE_EMPTY;
    memcpy(&crc, blk + RWY_BUNDLE_CRC_OFFSET, sizeof(crc));
    if (crc != rwy_crc32c(blk, RWY_BUNDLE_CRC_OFFSET)) return RWY_BUNDLE_BAD_HEADER;
    if (h->version != spec->version || h->count == 0 || h->count > spec->max_entries)
        return RWY_BUNDLE_BAD_HEADER;
    memset(counts, 0, sizeof(counts));
    for (uint32_t i = 0; i < h->count; i++) {
        if (check_entry_rule(spec, &h->entry[i], counts) != RWY_BUNDLE_OK)
            return RWY_BUNDLE_BAD_HEADER;
    }
    if (check_min_counts(spec, counts) < 0) return RWY_BUNDLE_BAD_HEADER;
    return RWY_BUNDLE_OK;
}

int rwy_bundle_check_range(const rwy_bundle_header_t *h, uint32_t blocks, const rwy_bundle_spec_t *spec) {
    uint64_t size = (uint64_t)blocks * BS;

    if (!h || !spec || !blocks) return RWY_BUNDLE_BAD_RANGE;
    for (uint32_t i = 0; i < h->count; i++) {
        const rwy_bundle_entry_t *e = &h->entry[i];
        const rwy_bundle_kind_rule_t *r;

        if (e->offset % BS || e->offset < BS || e->offset >= size) return RWY_BUNDLE_BAD_RANGE;
        if (e->length == 0 || e->length > size - e->offset) return RWY_BUNDLE_BAD_RANGE;
        r = find_rule(spec, e->kind);
        if (r && r->max_len && e->length > r->max_len) return RWY_BUNDLE_BAD_RANGE;
    }
    return RWY_BUNDLE_OK;
}

static int entry_hash(const rwy_block_dev_t *dev, uint32_t base, const rwy_bundle_entry_t *e, uint8_t out[32]) {
    rwy_sha256_ctx_t c;
    uint8_t blk[RWY_BLOCK_SIZE];
    uint64_t left = e->length;
    uint32_t b = base + (uint32_t)(e->offset / BS);

    rwy_sha256_init(&c);
    while (left) {
        size_t n = left > BS ? (size_t)BS : (size_t)left;
        if (rwy_blk_read(dev, RWY_IO_META, b++, blk)) return RWY_BUNDLE_IO;
        rwy_sha256_update(&c, blk, n);
        left -= n;
    }
    rwy_sha256_final(&c, out);
    return RWY_BUNDLE_OK;
}

int rwy_bundle_check_hashes(const rwy_block_dev_t *dev, uint32_t base, const rwy_bundle_header_t *h) {
    for (uint32_t i = 0; i < h->count; i++) {
        uint8_t sum[32];
        int st = entry_hash(dev, base, &h->entry[i], sum);
        if (st) return st;
        if (CompareMem(sum, h->entry[i].sha256, sizeof(sum))) return RWY_BUNDLE_BAD_HASH;
    }
    return RWY_BUNDLE_OK;
}

int rwy_bundle_check_copy(const rwy_block_dev_t *dev, uint32_t base, uint32_t blocks,
                          const rwy_bundle_spec_t *spec, rwy_bundle_header_t *hdr) {
    uint8_t blk[RWY_BLOCK_SIZE];
    int st;

    if (rwy_blk_read(dev, RWY_IO_META, base, blk)) return RWY_BUNDLE_IO;
    st = rwy_bundle_check_header(blk, spec);
    if (st) return st;
    memcpy(hdr, blk, sizeof(*hdr));
    st = rwy_bundle_check_range(hdr, blocks, spec);
    if (st) return st;
    return rwy_bundle_check_hashes(dev, base, hdr);
}

typedef struct {
    const rwy_block_dev_t *dev;
    uint32_t start, blocks;
    const rwy_bundle_spec_t *spec;
    rwy_bundle_header_t hdr[2];
    int status[2];
} bundle_pick_ctx_t;

static int pick_copy(void *ctx, unsigned c, uint64_t *seq) {
    bundle_pick_ctx_t *p = ctx;
    p->status[c] = rwy_bundle_check_copy(p->dev, p->start + c * p->blocks, p->blocks, p->spec, &p->hdr[c]);
    if (p->status[c] == RWY_BUNDLE_IO) return RWY_ERR_IO;
    if (p->status[c]) return RWY_ERR_CORRUPT;
    *seq = p->hdr[c].sequence;
    return RWY_OK;
}

int rwy_bundle_pick(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                    const rwy_bundle_spec_t *spec, rwy_bundle_info_t *info) {
    bundle_pick_ctx_t p;
    unsigned best = 0;
    int rc;

    if (!dev || !info || !spec || !blocks) return RWY_ERR_INVAL;
    p.dev = dev;
    p.start = start;
    p.blocks = blocks;
    p.spec = spec;
    rc = rwy_pick_newest(2, pick_copy, &p, &best);
    memset(info, 0, sizeof(*info));
    info->status[0] = p.status[0];
    info->status[1] = p.status[1];
    for (unsigned c = 0; c < 2; c++) {
        if (p.status[c] == RWY_BUNDLE_OK) info->seq[c] = p.hdr[c].sequence;
    }
    if (rc) return rc;
    info->copy = best;
    info->sequence = p.hdr[best].sequence;
    info->count = p.hdr[best].count;
    memcpy(info->entry, p.hdr[best].entry, sizeof(info->entry));
    return RWY_OK;
}

int rwy_bundle_read_entry(const rwy_block_dev_t *dev, uint32_t base,
                          const rwy_bundle_entry_t *e, void *buf, size_t cap, size_t *len) {
    uint8_t blk[RWY_BLOCK_SIZE], sum[32];
    rwy_sha256_ctx_t c;
    uint64_t left;
    uint32_t b;
    size_t done = 0;

    if (!dev || !e || !buf || !len) return RWY_ERR_INVAL;
    if (e->length > cap) return RWY_ERR_SIZE;
    rwy_sha256_init(&c);
    left = e->length;
    b = base + (uint32_t)(e->offset / BS);
    while (left) {
        size_t n = left > BS ? (size_t)BS : (size_t)left;
        if (rwy_blk_read(dev, RWY_IO_META, b++, blk)) return RWY_ERR_IO;
        memcpy((uint8_t *)buf + done, blk, n);
        rwy_sha256_update(&c, blk, n);
        done += n;
        left -= n;
    }
    rwy_sha256_final(&c, sum);
    if (CompareMem(sum, e->sha256, sizeof(sum))) return RWY_ERR_CORRUPT;
    *len = done;
    return RWY_OK;
}

int rwy_bundle_read(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                    const rwy_bundle_info_t *info, uint32_t kind, void *buf, size_t cap, size_t *len) {
    const rwy_bundle_entry_t *e = NULL;

    if (!info || info->copy > 1 || !blocks) return RWY_ERR_INVAL;
    for (uint32_t i = 0; i < info->count; i++) {
        if (info->entry[i].kind == kind) {
            e = &info->entry[i];
            break;
        }
    }
    if (!e) return RWY_ERR_NOENT;
    return rwy_bundle_read_entry(dev, start + info->copy * blocks, e, buf, cap, len);
}

int rwy_bundle_read_at(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                       const rwy_bundle_info_t *info, unsigned idx, void *buf, size_t cap, size_t *len) {
    if (!info || info->copy > 1 || !blocks || idx >= info->count) return RWY_ERR_INVAL;
    return rwy_bundle_read_entry(dev, start + info->copy * blocks, &info->entry[idx], buf, cap, len);
}

int rwy_bundle_same(const rwy_bundle_info_t *a, const rwy_bundle_info_t *b) {
    if (!a || !b || a->count != b->count) return 0;
    for (uint32_t i = 0; i < a->count; i++) {
        if (a->entry[i].kind != b->entry[i].kind || a->entry[i].length != b->entry[i].length ||
            CompareMem(a->entry[i].sha256, b->entry[i].sha256, sizeof(a->entry[i].sha256)))
            return 0;
    }
    return 1;
}

static void header_build(uint8_t blk[RWY_BLOCK_SIZE], const rwy_bundle_spec_t *spec,
                         const rwy_bundle_payload_t *parts, uint32_t count, uint64_t seq) {
    rwy_bundle_header_t *h = (rwy_bundle_header_t *)blk;
    uint64_t off = BS;
    uint32_t crc;

    memset(blk, 0, RWY_BLOCK_SIZE);
    h->magic = spec->magic;
    h->version = spec->version;
    h->sequence = seq;
    h->count = count;
    for (uint32_t i = 0; i < count; i++) {
        rwy_bundle_entry_t *e = &h->entry[i];
        e->kind = parts[i].kind;
        e->offset = off;
        e->length = parts[i].len;
        rwy_sha256(parts[i].data, parts[i].len, e->sha256);
        off += (uint64_t)part_blocks(parts[i].len) * BS;
    }
    crc = rwy_crc32c(blk, RWY_BUNDLE_CRC_OFFSET);
    memcpy(blk + RWY_BUNDLE_CRC_OFFSET, &crc, sizeof(crc));
}

static int payloads_write(const rwy_block_dev_t *dev, uint32_t base, const rwy_bundle_header_t *h,
                          const rwy_bundle_payload_t *parts) {
    for (uint32_t i = 0; i < h->count; i++) {
        const rwy_bundle_payload_t *p = &parts[i];
        uint32_t blk = base + (uint32_t)(h->entry[i].offset / BS);
        for (uint32_t done = 0; done < p->len; done += RWY_BLOCK_SIZE) {
            uint8_t buf[RWY_BLOCK_SIZE];
            uint32_t n = p->len - done > RWY_BLOCK_SIZE ? RWY_BLOCK_SIZE : p->len - done;
            memset(buf, 0, sizeof(buf));
            memcpy(buf, (const uint8_t *)p->data + done, n);
            if (rwy_blk_write(dev, RWY_IO_META, blk++, buf)) return RWY_ERR_IO;
        }
    }
    return RWY_OK;
}

static int write_target(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                        const rwy_bundle_spec_t *spec, unsigned *copy, uint64_t *seq) {
    rwy_bundle_info_t info;
    int rc = rwy_bundle_pick(dev, start, blocks, spec, &info);
    if (rc == RWY_OK) {
        *copy = info.copy ? 0 : 1;
        *seq = info.sequence + 1;
        return RWY_OK;
    }
    if (rc == RWY_ERR_IO) return rc;
    *copy = 0;
    *seq = 1;
    return RWY_OK;
}

int rwy_bundle_write_copy(const rwy_block_dev_t *dev, uint32_t base, uint32_t blocks,
                          const rwy_bundle_spec_t *spec, const rwy_bundle_payload_t *parts,
                          uint32_t count, uint64_t seq) {
    uint8_t hdr_blk[RWY_BLOCK_SIZE];
    const rwy_bundle_header_t *h = (const rwy_bundle_header_t *)hdr_blk;
    uint32_t need = rwy_bundle_blocks(spec, parts, count);
    int rc;

    if (!dev || !spec || !need || !blocks) return RWY_ERR_INVAL;
    if (need > blocks) return RWY_ERR_NOSPC;
    header_build(hdr_blk, spec, parts, count, seq);
    rc = payloads_write(dev, base, h, parts);
    if (rc) return rc;
    rc = rwy_dev_flush(dev);
    if (rc) return rc;
    if (rwy_bundle_check_hashes(dev, base, h) != RWY_BUNDLE_OK) return RWY_ERR_IO;
    rc = rwy_blk_write(dev, RWY_IO_META, base, hdr_blk);
    if (rc) return rc;
    return rwy_dev_flush(dev);
}

int rwy_bundle_write(const rwy_block_dev_t *dev, uint32_t start, uint32_t blocks,
                     const rwy_bundle_spec_t *spec, const rwy_bundle_payload_t *parts, uint32_t count) {
    unsigned copy;
    uint64_t seq;
    int rc;

    if (!dev || !spec || !blocks) return RWY_ERR_INVAL;
    rc = write_target(dev, start, blocks, spec, &copy, &seq);
    if (rc) return rc;
    return rwy_bundle_write_copy(dev, start + copy * blocks, blocks, spec, parts, count, seq);
}

static const rwy_bundle_kind_rule_t loader_rules[2] = {
    { RWY_ENTRY_LOADER, 1, 1, 0 },
    { RWY_ENTRY_WDT,    0, 8, 0 },
};

const rwy_bundle_spec_t rwy_loader_bundle_spec = {
    .magic = RWY_LOADER_MAGIC,
    .version = RWY_BUNDLE_VERSION,
    .max_entries = 9,
    .num_rules = 2,
    .rules = loader_rules,
};
