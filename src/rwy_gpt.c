/*
 * rwy_gpt.c — read the type of a partition from the GPT of its disk (see rwy_gpt.h).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "rwy_gpt.h"
#include "rwy_string.h"

#define HEADER_READ 512u            /* enough for any header, whatever the block size */
#define HEADER_MIN 92u
#define ENTRIES_MAX (128u * 1024u)  /* the table is 16 KiB on any disk we know */
#define ENTRY_MIN 128u
#define ENTRY_MAX 512u
#define CHUNK 4096u

typedef struct {
    uint64_t entries_lba;
    uint32_t count;
    uint32_t size;
    uint32_t crc;
} gpt_header_t;

static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p) {
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

/* CRC-32 as the GPT uses it (the one of zlib), continued from `crc`. */
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* The header at block `lba`, if it is a valid one that sits there. */
static int read_header(rwy_gpt_read_fn rd, void *ctx, uint32_t block_size, uint64_t lba, gpt_header_t *h) {
    uint8_t b[HEADER_READ];
    uint32_t size, crc;

    if (rd(ctx, lba * block_size, b, sizeof(b))) return -1;
    if (CompareMem(b, "EFI PART", 8)) return -1;
    size = le32(b + 12);
    if (size < HEADER_MIN || size > sizeof(b) || le64(b + 24) != lba) return -1;
    crc = le32(b + 16);
    memset(b + 16, 0, 4);
    if (crc32_update(0, b, size) != crc) return -1;
    h->entries_lba = le64(b + 72);
    h->count = le32(b + 80);
    h->size = le32(b + 84);
    h->crc = le32(b + 88);
    if (h->size < ENTRY_MIN || h->size > ENTRY_MAX || h->size & (h->size - 1)) return -1;
    if (!h->count || (uint64_t)h->count * h->size > ENTRIES_MAX) return -1;
    return 0;
}

/*
 * Check the CRC of the whole table and keep entry `number`. The table is read in pieces: the entry can start
 * in one piece and end in the next.
 */
static int read_entry(rwy_gpt_read_fn rd, void *ctx, uint32_t block_size, const gpt_header_t *h, uint32_t number,
                      uint8_t *entry) {
    uint8_t chunk[CHUNK];
    uint64_t total = (uint64_t)h->count * h->size, base = h->entries_lba * block_size;
    uint64_t want = (uint64_t)(number - 1) * h->size;
    uint32_t crc = 0;

    for (uint64_t off = 0; off < total; off += sizeof(chunk)) {
        size_t n = total - off < sizeof(chunk) ? (size_t)(total - off) : sizeof(chunk);

        if (rd(ctx, base + off, chunk, n)) return -1;
        crc = crc32_update(crc, chunk, n);
        for (uint32_t i = 0; i < ENTRY_MIN; i++) {
            uint64_t at = want + i;

            if (at >= off && at < off + n) entry[i] = chunk[at - off];
        }
    }
    return crc == h->crc ? 0 : -1;
}

static int type_in_table(rwy_gpt_read_fn rd, void *ctx, uint32_t block_size, uint64_t lba, uint32_t number,
                         const uint8_t unique[16], uint8_t type[16]) {
    static const uint8_t unused[16];
    uint8_t entry[ENTRY_MIN];
    gpt_header_t h;

    if (read_header(rd, ctx, block_size, lba, &h) || number > h.count) return -1;
    if (read_entry(rd, ctx, block_size, &h, number, entry)) return -1;
    if (!CompareMem(entry, unused, 16) || CompareMem(entry + 16, unique, 16)) return -1;
    memcpy(type, entry, 16);
    return 0;
}

int rwy_gpt_partition_type(rwy_gpt_read_fn rd, void *ctx, uint32_t block_size, uint64_t last_lba, uint32_t number,
                           const uint8_t unique[16], uint8_t type[16]) {
    if (!number || block_size < 512 || block_size & (block_size - 1)) return -1;
    if (!type_in_table(rd, ctx, block_size, 1, number, unique, type)) return 0;
    return last_lba > 1 ? type_in_table(rd, ctx, block_size, last_lba, number, unique, type) : -1;
}
