/*
 * forge.h — make a store carry marked-but-unreachable blocks (transaction leftovers).
 * SPDX-License-Identifier: Apache-2.0
 * A commit no longer leaves any (the images live in fixed areas), so tests that exercise
 * gc, repair and defrag on leftovers create them by publishing extra bitmap bits.
 */
#ifndef RWY_TEST_FORGE_H
#define RWY_TEST_FORGE_H

#include "rwy_core.h"

static inline int forge_leftovers(rwy_store_t *s, uint32_t n) {
    uint32_t b, done = 0;
    int rc = rwy_begin(s);
    if (rc) return rc;
    for (b = s->sb.pool_start; b < s->sb.total_blocks && done < n; b++) {
        if (s->w_bitmap[b >> 3] & (1u << (b & 7))) continue;
        s->w_bitmap[b >> 3] |= (uint8_t)(1u << (b & 7));
        done++;
    }
    return rwy_commit(s);
}

#endif
