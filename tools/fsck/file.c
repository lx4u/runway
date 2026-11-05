/* SPDX-License-Identifier: Apache-2.0 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "hostdev.h"
#include "rwy_format.h"

typedef struct {
    uint64_t fat_off, data_off;
    uint32_t cl_sz, root_cl;
} fat_geom_t;

/* Read the geometry of a FAT32 volume from its boot sector; 0 if it is not one we can walk. */
static int fat32_geometry(const uint8_t *boot, uint64_t part_start, fat_geom_t *g) {
    uint16_t bps = (uint16_t)boot[11] | ((uint16_t)boot[12] << 8);
    uint8_t spc = boot[13];
    uint16_t res = (uint16_t)boot[14] | ((uint16_t)boot[15] << 8);
    uint8_t fats = boot[16];
    uint32_t spf = (uint32_t)boot[36] | ((uint32_t)boot[37] << 8) |
                   ((uint32_t)boot[38] << 16) | ((uint32_t)boot[39] << 24);

    g->root_cl = (uint32_t)boot[44] | ((uint32_t)boot[45] << 8) |
                 ((uint32_t)boot[46] << 16) | ((uint32_t)boot[47] << 24);
    if (boot[510] != 0x55 || boot[511] != 0xAA) return 0;
    if (bps != 512 || spc == 0 || res == 0 || fats == 0 || spf == 0 || g->root_cl < 2) return 0;
    g->fat_off = part_start + (uint64_t)res * bps;
    g->data_off = g->fat_off + (uint64_t)fats * spf * bps;
    g->cl_sz = (uint32_t)spc * bps;
    return 1;
}

/* The next cluster of a chain; 0 when the FAT cannot be read. */
static int fat32_next(rwy_block_dev_t *d, const fat_geom_t *g, uint32_t *cl) {
    uint8_t e[4];
    if (d->read(d->ctx, g->fat_off + (uint64_t)*cl * 4, e, sizeof(e))) return 0;
    *cl = ((uint32_t)e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16 | (uint32_t)e[3] << 24) & 0x0FFFFFFF;
    return 1;
}

/* Search the root directory for RUNWAY.DAT. */
static void fat32_find_store(rwy_block_dev_t *d, const fat_geom_t *g, uint32_t *start_cl, uint64_t *file_sz) {
    uint32_t cur_cl = g->root_cl;
    uint8_t *dir_buf = malloc(g->cl_sz);

    *start_cl = 0;
    *file_sz = 0;
    while (dir_buf && cur_cl >= 2 && cur_cl < 0x0FFFFFF8) {
        uint64_t cl_off = g->data_off + (uint64_t)(cur_cl - 2) * g->cl_sz;
        if (d->read(d->ctx, cl_off, dir_buf, g->cl_sz)) break;
        for (uint32_t d = 0; d < g->cl_sz; d += 32) {
            uint8_t *de = dir_buf + d;
            if (de[0] == 0x00) break;
            if (de[0] == 0xE5 || (de[11] & 0x0F) == 0x0F) continue;
            if (memcmp(de, "RUNWAY  DAT", 11) == 0 || memcmp(de, "RUNWAY~1DAT", 11) == 0) {
                *start_cl = ((uint32_t)*(uint16_t *)(de + 20) << 16) | (uint32_t)*(uint16_t *)(de + 26);
                *file_sz = *(uint32_t *)(de + 28);
                break;
            }
        }
        if (*start_cl != 0 || !fat32_next(d, g, &cur_cl)) break;
    }
    free(dir_buf);
}

/* Follow the cluster chain of the store; fills the file when the chain is complete. */
static void fat32_load_chain(file_t *fl, const fat_geom_t *g, uint32_t start_cl, uint64_t file_sz) {
    uint32_t needed_cls = (uint32_t)((file_sz + g->cl_sz - 1) / g->cl_sz);
    uint32_t *cls = malloc((size_t)needed_cls * sizeof(uint32_t));
    uint32_t c = start_cl, count = 0;

    if (!cls) return;
    while (count < needed_cls && c >= 2 && c < 0x0FFFFFF8) {
        cls[count++] = c;
        if (!fat32_next(&fl->bdev, g, &c)) break;
    }
    if (count != needed_cls) {
        free(cls);
        return;
    }
    fl->is_fat32 = 1;
    fl->fat_data_offset = g->data_off;
    fl->cluster_size = g->cl_sz;
    fl->clusters = cls;
    fl->cluster_count = needed_cls;
    fl->store_size = file_sz;
}

/* Find an EFI System Partition offset in GPT, or 0 if not GPT. */
static uint64_t gpt_find_esp_offset(rwy_block_dev_t *d) {
    static const uint8_t esp_guid[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
    };
    uint8_t h[92], entry[128];
    uint32_t n, esz, i;
    uint64_t lba = 0;

    if (d->read(d->ctx, 512, h, sizeof(h))) return 0;
    if (memcmp(h, "EFI PART", 8) != 0) return 0;
    n = (uint32_t)h[80] | ((uint32_t)h[81] << 8) | ((uint32_t)h[82] << 16) | ((uint32_t)h[83] << 24);
    esz = (uint32_t)h[84] | ((uint32_t)h[85] << 8) | ((uint32_t)h[86] << 16) | ((uint32_t)h[87] << 24);
    if (esz < 128 || esz > 4096 || n > 4096) return 0;
    for (i = 0; i < 8; i++) lba |= (uint64_t)h[72 + i] << (8 * i);
    for (i = 0; i < n; i++) {
        if (d->read(d->ctx, lba * 512 + (uint64_t)i * esz, entry, sizeof(entry))) return 0;
        if (memcmp(entry, esp_guid, 16) == 0) {
            uint64_t start_lba = 0;
            for (int k = 0; k < 8; k++) start_lba |= (uint64_t)entry[32 + k] << (8 * k);
            return start_lba * 512;
        }
    }
    return 0;
}

/* A write goes down in whole 4096-byte blocks, so each must sit inside one cluster at an aligned disk offset. */
static int fat32_write_safe(const file_t *fl) {
    return fl->cluster_size >= RWY_BLOCK_SIZE && fl->fat_data_offset % RWY_BLOCK_SIZE == 0;
}

int file_open(file_t *fl, const char *path, int rw) {
    uint8_t boot[512];
    fat_geom_t g;
    uint32_t start_cl;
    uint64_t file_sz;
    uint64_t part_start = 0;

    memset(fl, 0, sizeof(*fl));
    fl->path = path;
    if (hostdev_open(&fl->hd, path, rw ? O_RDWR : O_RDONLY)) return -1;
    hostdev_dev(&fl->hd, &fl->bdev);
    if (fl->hd.windowed) return 0;

    /* Detect if file is a bare FAT32 ESP image, or a GPT disk containing an ESP */
    if (fl->bdev.read(fl->bdev.ctx, 0, boot, sizeof(boot))) return 0;
    if (!fat32_geometry(boot, 0, &g)) {
        part_start = gpt_find_esp_offset(&fl->bdev);
        if (part_start == 0) return 0;
        if (fl->bdev.read(fl->bdev.ctx, part_start, boot, sizeof(boot))) return 0;
        if (!fat32_geometry(boot, part_start, &g)) return 0;
    }
    fat32_find_store(&fl->bdev, &g, &start_cl, &file_sz);
    if (start_cl >= 2 && file_sz >= RWY_MIN_STORE_SIZE)
        fat32_load_chain(fl, &g, start_cl, file_sz);
    if (rw && fl->is_fat32 && !fat32_write_safe(fl)) {
        fprintf(stderr, "%s: the store file is on FAT32 with %u-byte clusters at data offset %llu; "
                "writes need 4096-byte clusters and an aligned data area\n",
                path, fl->cluster_size, (unsigned long long)fl->fat_data_offset);
        file_close(fl);
        return -1;
    }
    return 0;
}

void file_close(file_t *fl) {
    hostdev_close(&fl->hd);
    if (fl->clusters) {
        free(fl->clusters);
        fl->clusters = NULL;
    }
}

uint64_t file_get_size(file_t *fl) {
    if (fl->is_fat32) return fl->store_size;
    return fl->bdev.size ? fl->bdev.size : RWY_STORE_SIZE;
}

int file_rd(void *ctx, uint64_t off, void *buf, size_t len) {
    file_t *fl = ctx;
    if (!fl->is_fat32) return fl->bdev.read(fl->bdev.ctx, off, buf, len);
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        uint32_t cl_idx = (uint32_t)(off / fl->cluster_size);
        uint32_t cl_off = (uint32_t)(off % fl->cluster_size);
        if (cl_idx >= fl->cluster_count) return -1;
        size_t chunk = fl->cluster_size - cl_off;
        if (chunk > len) chunk = len;
        uint64_t disk_off = fl->fat_data_offset + (uint64_t)(fl->clusters[cl_idx] - 2) * fl->cluster_size + cl_off;
        if (fl->bdev.read(fl->bdev.ctx, disk_off, p, chunk)) return -1;
        off += chunk;
        p += chunk;
        len -= chunk;
    }
    return 0;
}

/* Read-only backend: any write attempt fails loudly (plain check). */
int file_wr_ro(void *ctx, uint64_t off, const void *buf, size_t len) {
    (void)ctx; (void)off; (void)buf; (void)len;
    return -1;
}

/* Repair backend: counting writes/flushes with fault-injection hook. */
static long rw_point;
static long rw_fail_at = -1;
int file_wr_rw(void *ctx, uint64_t off, const void *buf, size_t len) {
    file_t *fl = ctx;
    rw_point++;
    if (rw_fail_at == rw_point) return -1;
    if (!fl->is_fat32) return fl->bdev.write(fl->bdev.ctx, off, buf, len);
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        uint32_t cl_idx = (uint32_t)(off / fl->cluster_size);
        uint32_t cl_off = (uint32_t)(off % fl->cluster_size);
        if (cl_idx >= fl->cluster_count) return -1;
        size_t chunk = fl->cluster_size - cl_off;
        if (chunk > len) chunk = len;
        uint64_t disk_off = fl->fat_data_offset + (uint64_t)(fl->clusters[cl_idx] - 2) * fl->cluster_size + cl_off;
        if (fl->bdev.write(fl->bdev.ctx, disk_off, p, chunk)) return -1;
        off += chunk;
        p += chunk;
        len -= chunk;
    }
    return 0;
}
int file_fl_rw(void *ctx) {
    file_t *fl = ctx;
    rw_point++;
    if (rw_fail_at == rw_point) return -1;
    return fl->bdev.flush(fl->bdev.ctx);
}

long file_io_points(void) {
    return rw_point;
}

void file_set_fail_at(long n) {
    rw_fail_at = n;
}
