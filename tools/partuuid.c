/* partuuid: find the partition of a GPT partition UUID, with or without udev. */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "partuuid.h"

/* The GPT form of a partition UUID text: the first three fields are stored little endian. */
static int guid_bytes(const char *uuid, uint8_t out[16]) {
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    unsigned v;
    int i, k = 0;
    for (i = 0; i < 16; i++) {
        if (uuid[k] == '-') k++;
        if (!isxdigit((unsigned char)uuid[k]) || !isxdigit((unsigned char)uuid[k + 1]) || sscanf(uuid + k, "%2x", &v) != 1) return 0;
        out[order[i]] = (uint8_t)v;
        k += 2;
    }
    return uuid[k] == 0;
}

/* The partition entry array of the GPT on `fd`: first entry offset, entry size and count. Sector size 512 or 4096. */
static int gpt_entries(int fd, off_t *first, uint32_t *esz, uint32_t *n) {
    static const int sectors[2] = {512, 4096};
    uint8_t h[92];
    uint64_t lba = 0;
    int s, i;
    for (s = 0; s < 2; s++) {
        if (pread(fd, h, sizeof(h), sectors[s]) != (ssize_t)sizeof(h) || memcmp(h, "EFI PART", 8)) continue;
        *n = (uint32_t)h[80] | h[81] << 8 | h[82] << 16 | (uint32_t)h[83] << 24;
        *esz = (uint32_t)h[84] | h[85] << 8 | h[86] << 16 | (uint32_t)h[87] << 24;
        if (*esz < 128 || *esz > 4096 || *n > 4096) return 0;
        for (i = 0; i < 8; i++) lba |= (uint64_t)h[72 + i] << (8 * i);
        *first = (off_t)(lba * sectors[s]);
        return 1;
    }
    return 0;
}

/* The number (1-based) of the partition of the GPT on `fd` that has GUID `guid`, or 0. */
static int gpt_find(int fd, const uint8_t guid[16]) {
    uint8_t g[16];
    uint32_t n, esz, i;
    off_t first;
    if (!gpt_entries(fd, &first, &esz, &n)) return 0;
    for (i = 0; i < n; i++)
        if (pread(fd, g, 16, first + (off_t)i * esz + 16) == 16 && !memcmp(g, guid, 16)) return (int)i + 1;
    return 0;
}

/* The sysfs name of partition number `num` of `disk` ("sda", 2 -> "sda2"), or 0. */
static int part_name(const char *root, const char *disk, int num, char *part) {
    char d[PATH_MAX], p[PATH_MAX];
    struct dirent *e;
    DIR *dir;
    FILE *f;
    int v, found = 0;
    snprintf(d, sizeof(d), "%s/sys/block/%s", root, disk);
    dir = opendir(d);
    if (!dir) return 0;
    while (!found && (e = readdir(dir))) {
        snprintf(p, sizeof(p), "%s/%s/partition", d, e->d_name);
        f = fopen(p, "r");
        if (!f) continue;
        if (fscanf(f, "%d", &v) == 1 && v == num) { snprintf(part, 64, "%s", e->d_name); found = 1; }
        fclose(f);
    }
    closedir(dir);
    return found;
}

/* No udev: read the GPT of every disk and match the partition GUID. */
static int part_of_uuid_gpt(const char *root, const char *uuid, char *part, char *disk) {
    char d[PATH_MAX], p[PATH_MAX];
    uint8_t guid[16];
    struct dirent *e;
    DIR *dir;
    int fd, num, found = 0;
    if (!guid_bytes(uuid, guid)) return 0;
    snprintf(d, sizeof(d), "%s/sys/block", root);
    dir = opendir(d);
    if (!dir) return 0;
    while (!found && (e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof(p), "%s/dev/%s", root, e->d_name);
        fd = open(p, O_RDONLY);
        if (fd < 0) continue;
        num = gpt_find(fd, guid);
        close(fd);
        if (num && part_name(root, e->d_name, num, part)) { snprintf(disk, 64, "%s", e->d_name); found = 1; }
    }
    closedir(dir);
    return found;
}

/* The partition with GPT UUID `uuid` and its disk: by-partuuid when udev made it, else the GPT. */
int partuuid_find(const char *root, const char *uuid, char *part, char *disk) {
    char p[PATH_MAX], real[PATH_MAX], *s;
    snprintf(p, sizeof(p), "%s/dev/disk/by-partuuid/%s", root, uuid);
    if (!realpath(p, real)) return part_of_uuid_gpt(root, uuid, part, disk);
    s = strrchr(real, '/');
    snprintf(part, 64, "%s", s ? s + 1 : real);
    snprintf(p, sizeof(p), "%s/sys/class/block/%s", root, part);
    if (!realpath(p, real)) return 0;
    *strrchr(real, '/') = 0;   /* .../sda/sda1 -> .../sda */
    s = strrchr(real, '/');
    snprintf(disk, 64, "%s", s ? s + 1 : real);
    return 1;
}


/* Whether partition `part` ("sda2") of `disk` has the GPT type `type` (a GUID text). 1 yes, 0 no, -1 when
 * the GPT of the disk cannot be read (not GPT, or no device node), so nothing is known. */
int partuuid_has_type(const char *root, const char *disk, const char *part, const char *type) {
    char p[PATH_MAX];
    uint8_t want[16], have[16];
    uint32_t n, esz;
    off_t first;
    int fd, num, ok;
    FILE *f;
    if (!guid_bytes(type, want)) return -1;
    snprintf(p, sizeof(p), "%s/sys/block/%s/%s/partition", root, disk, part);
    f = fopen(p, "r");
    if (!f) return -1;
    ok = fscanf(f, "%d", &num) == 1 && num > 0;
    fclose(f);
    snprintf(p, sizeof(p), "%s/dev/%s", root, disk);
    fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    ok = ok && gpt_entries(fd, &first, &esz, &n) && (uint32_t)num <= n;
    ok = ok && pread(fd, have, 16, first + (off_t)(num - 1) * esz) == 16;
    close(fd);
    return ok ? !memcmp(have, want, 16) : -1;
}
