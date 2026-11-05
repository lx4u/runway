/*
 * rwy_recarea.c — The plaintext recovery area: build, write, verify and pick the bundle (ADR 0004).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"
#include "rwy_string.h"
#include "rwy_crypto.h"
#include "rwy_bundle.h"
#include "rwy_recarea.h"

static const rwy_bundle_kind_rule_t ra_rules[RWY_RA_KINDS] = {
    { RWY_RA_KERNEL, 1, 1, 0 },
    { RWY_RA_INITRD, 0, 1, 0 },
    { RWY_RA_DTB,    0, 1, 0 },
    { RWY_RA_ARGS,   0, 1, RWY_RA_ARGS_MAX },
};

static const rwy_bundle_spec_t ra_spec = {
    .magic = RWY_RA_MAGIC,
    .version = RWY_RA_VERSION,
    .max_entries = RWY_RA_KINDS,
    .num_rules = RWY_RA_KINDS,
    .rules = ra_rules,
};

static uint32_t ra_to_parts(const rwy_ra_bundle_t *b, rwy_bundle_payload_t parts[RWY_RA_KINDS]) {
    uint32_t count = 0;

    if (!b) return 0;
    for (uint32_t k = 0; k < RWY_RA_KINDS; k++) {
        if (!b->part[k].data) continue;
        parts[count].kind = k;
        parts[count].data = b->part[k].data;
        parts[count].len = b->part[k].len;
        count++;
    }
    return count;
}

uint32_t rwy_ra_blocks(const rwy_ra_bundle_t *b) {
    rwy_bundle_payload_t parts[RWY_RA_KINDS];
    uint32_t count;

    if (!b || !b->part[RWY_RA_KERNEL].data) return 0;
    count = ra_to_parts(b, parts);
    return rwy_bundle_blocks(&ra_spec, parts, count);
}

const char *rwy_ra_reason(int status) {
    return rwy_bundle_reason(status);
}

int rwy_ra_pick(const rwy_block_dev_t *raw, uint32_t start, uint32_t blocks, rwy_ra_info_t *info) {
    rwy_bundle_info_t bi;
    int rc;

    if (!raw || !info || !blocks) return RWY_ERR_INVAL;
    rc = rwy_bundle_pick(raw, start, blocks, &ra_spec, &bi);
    memset(info, 0, sizeof(*info));
    info->status[0] = bi.status[0];
    info->status[1] = bi.status[1];
    info->seq[0] = bi.seq[0];
    info->seq[1] = bi.seq[1];
    if (rc) return rc;
    info->copy = bi.copy;
    info->sequence = bi.sequence;
    info->count = bi.count;
    for (uint32_t i = 0; i < bi.count && i < RWY_RA_KINDS; i++) info->entry[i] = bi.entry[i];
    return RWY_OK;
}

int rwy_ra_read(const rwy_block_dev_t *raw, uint32_t start, uint32_t blocks, const rwy_ra_info_t *info,
                 uint32_t kind, void *buf, size_t cap, size_t *len) {
    rwy_bundle_info_t bi;

    if (!raw || !info || !buf || !len || info->copy > 1 || !blocks) return RWY_ERR_INVAL;
    memset(&bi, 0, sizeof(bi));
    bi.copy = info->copy;
    bi.sequence = info->sequence;
    bi.count = info->count;
    for (uint32_t i = 0; i < info->count && i < RWY_RA_KINDS; i++) bi.entry[i] = info->entry[i];
    return rwy_bundle_read(raw, start, blocks, &bi, kind, buf, cap, len);
}

int rwy_ra_same(const rwy_ra_info_t *a, const rwy_ra_info_t *b) {
    rwy_bundle_info_t bia, bib;

    if (!a || !b) return 0;
    memset(&bia, 0, sizeof(bia));
    memset(&bib, 0, sizeof(bib));
    bia.count = a->count;
    bib.count = b->count;
    for (uint32_t i = 0; i < a->count && i < RWY_RA_KINDS; i++) bia.entry[i] = a->entry[i];
    for (uint32_t i = 0; i < b->count && i < RWY_RA_KINDS; i++) bib.entry[i] = b->entry[i];
    return rwy_bundle_same(&bia, &bib);
}

int rwy_ra_write(const rwy_block_dev_t *raw, uint32_t start, uint32_t blocks, const rwy_ra_bundle_t *b) {
    rwy_bundle_payload_t parts[RWY_RA_KINDS];
    uint32_t count = ra_to_parts(b, parts);

    return rwy_bundle_write(raw, start, blocks, &ra_spec, parts, count);
}
