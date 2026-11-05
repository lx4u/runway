/* gpttest — read the type of a partition from a GPT built in memory. */
/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/rwy_gpt.h"

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1; \
        } \
    } while (0)

#define COUNT 128u
#define DISK_BLOCKS 4096u

typedef struct {
    uint8_t *data;
    uint32_t block_size;
    uint32_t entry_size;
    uint64_t last_lba;
    uint64_t reads;
} disk_t;

static uint32_t crc32(const uint8_t *p, size_t len) {
    uint32_t crc = ~0u;

    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static int read_disk(void *ctx, uint64_t off, void *buf, size_t len) {
    disk_t *d = ctx;

    d->reads++;
    if (off + len > (uint64_t)(d->last_lba + 1) * d->block_size) return -1;
    memcpy(buf, d->data + off, len);
    return 0;
}

static void guid(uint8_t g[16], uint8_t seed) {
    for (int i = 0; i < 16; i++) g[i] = (uint8_t)(seed * 16 + i + 1);
}

/* One header and its table: the primary at block 1, the backup in the last block. */
static void write_table(disk_t *d, int backup) {
    uint32_t table = COUNT * d->entry_size;
    uint64_t hdr_lba = backup ? d->last_lba : 1;
    uint64_t ent_lba = backup ? d->last_lba - table / d->block_size : 2;
    uint8_t *h = d->data + hdr_lba * d->block_size;
    uint8_t *e = d->data + ent_lba * d->block_size;

    memset(h, 0, 92);
    memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x00010000);
    put32(h + 12, 92);
    put64(h + 24, hdr_lba);
    put64(h + 32, backup ? 1 : d->last_lba);
    put64(h + 72, ent_lba);
    put32(h + 80, COUNT);
    put32(h + 84, d->entry_size);
    put32(h + 88, crc32(e, table));
    put32(h + 16, crc32(h, 92));
}

static void make_disk(disk_t *d, uint32_t block_size, uint32_t entry_size) {
    memset(d, 0, sizeof(*d));
    d->block_size = block_size;
    d->entry_size = entry_size;
    d->last_lba = DISK_BLOCKS - 1;
    d->data = calloc(DISK_BLOCKS, block_size);
    for (uint8_t n = 0; n < 3; n++) {
        uint8_t *e = d->data + 2 * (uint64_t)block_size + (uint64_t)n * entry_size;

        guid(e, n + 1);       /* type */
        guid(e + 16, n + 9);  /* unique */
    }
    /* The backup table is a copy of the primary one. */
    memcpy(d->data + (d->last_lba - COUNT * entry_size / block_size) * block_size, d->data + 2 * (uint64_t)block_size,
           COUNT * entry_size);
    write_table(d, 0);
    write_table(d, 1);
}

static int lookup(disk_t *d, uint32_t number, uint8_t unique_seed, uint8_t type[16]) {
    uint8_t unique[16];

    guid(unique, unique_seed);
    return rwy_gpt_partition_type(read_disk, d, d->block_size, d->last_lba, number, unique, type);
}

static int check_lookup(uint32_t block_size, uint32_t entry_size) {
    disk_t d;
    uint8_t type[16], want[16];

    make_disk(&d, block_size, entry_size);
    for (uint32_t n = 1; n <= 3; n++) {
        CHECK(lookup(&d, n, (uint8_t)(n + 8), type) == 0);
        guid(want, (uint8_t)n);
        CHECK(!memcmp(type, want, 16));
    }
    CHECK(lookup(&d, 2, 9, type) != 0);   /* the unique GUID of another partition */
    CHECK(lookup(&d, 0, 9, type) != 0);   /* numbers start at 1 */
    CHECK(lookup(&d, COUNT + 1, 9, type) != 0);
    CHECK(lookup(&d, 4, 12, type) != 0);  /* an unused entry */
    free(d.data);
    return 0;
}

static int check_backup(void) {
    disk_t d;
    uint8_t type[16], want[16];

    guid(want, 2);
    make_disk(&d, 512, 128);
    d.data[1 * 512 + 40] ^= 1;  /* the primary header changes after its CRC */
    CHECK(lookup(&d, 2, 10, type) == 0 && !memcmp(type, want, 16));
    free(d.data);

    make_disk(&d, 512, 128);
    d.data[2 * 512 + 3 * 128 + 5] ^= 1;  /* the primary table changes after its CRC */
    CHECK(lookup(&d, 2, 10, type) == 0 && !memcmp(type, want, 16));
    free(d.data);

    make_disk(&d, 512, 128);
    d.data[1 * 512 + 40] ^= 1;
    d.data[d.last_lba * 512 + 40] ^= 1;  /* both headers are bad */
    CHECK(lookup(&d, 2, 10, type) != 0);
    free(d.data);

    make_disk(&d, 512, 128);
    d.data[2 * 512 + 3 * 128 + 5] ^= 1;
    d.data[(d.last_lba - 32) * 512 + 3 * 128 + 5] ^= 1;  /* both tables are bad */
    CHECK(lookup(&d, 2, 10, type) != 0);
    free(d.data);
    return 0;
}

static int check_limits(void) {
    disk_t d;
    uint8_t type[16], unique[16];

    make_disk(&d, 512, 128);
    d.last_lba = 10;  /* the reader refuses everything past 5.5 KiB: the table does not fit */
    guid(unique, 10);
    CHECK(rwy_gpt_partition_type(read_disk, &d, 512, 1, 2, unique, type) != 0);
    d.last_lba = DISK_BLOCKS - 1;
    CHECK(rwy_gpt_partition_type(read_disk, &d, 100, d.last_lba, 2, unique, type) != 0);
    CHECK(rwy_gpt_partition_type(read_disk, &d, 0, d.last_lba, 2, unique, type) != 0);
    free(d.data);
    return 0;
}

int main(void) {
    if (check_lookup(512, 128) || check_lookup(4096, 128) || check_lookup(512, 256) || check_backup() ||
        check_limits())
        return 1;
    printf("gpt: partition types read from the primary and the backup table\n");
    return 0;
}
