/* hostdev: a store on the host, opened as a file or as a window of an image. */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "hostdev.h"
#include "partuuid.h"
#include "rwy_core.h"
#include "rwy_format.h"

#define PARTUUID_PREFIX "PARTUUID="
#define DIRECT_MAX (1u << 20)   /* the largest piece moved through the aligned buffer */
#define FILE_SECTOR 4096        /* sector size assumed for a regular file opened with O_DIRECT */

const char *hostdev_root = "";

/* Read a number that fills [s, end); -1 if it does not. */
static int number(const char *s, const char *end, uint64_t *v) {
    char *e;
    if (s == end || *s < '0' || *s > '9') return -1;
    errno = 0;
    *v = strtoull(s, &e, 0);
    return errno || e != end ? -1 : 0;
}

/* Replace PARTUUID=<uuid> in path by the device node. 0 is done (or not a PARTUUID), -2 is not found. */
static int resolve_partuuid(char *path, size_t max) {
    char uuid[40], part[64], disk[64];
    size_t i, n = strlen(path) - strlen(PARTUUID_PREFIX);
    if (strncmp(path, PARTUUID_PREFIX, strlen(PARTUUID_PREFIX))) return 0;
    if (n >= sizeof(uuid)) return -2;
    for (i = 0; i < n; i++) uuid[i] = (char)tolower((unsigned char)path[strlen(PARTUUID_PREFIX) + i]);
    uuid[n] = 0;
    if (!partuuid_find(hostdev_root, uuid, part, disk)) return -2;
    return snprintf(path, max, "%s/dev/%s", hostdev_root, part) < (int)max ? 0 : -2;
}

int hostdev_parse(const char *spec, char *path, size_t max, uint64_t *off, uint64_t *len, int *windowed) {
    const char *at = strrchr(spec, '@'), *plus;
    *off = *len = 0;
    *windowed = 0;
    if (!at || !access(spec, F_OK)) {
        if (strlen(spec) >= max) return -1;
        strcpy(path, spec);
        return resolve_partuuid(path, max);
    }
    plus = strchr(at, '+');
    if (number(at + 1, plus ? plus : at + strlen(at), off) || (plus && number(plus + 1, plus + strlen(plus), len)) ||
        (plus && !*len) || (size_t)(at - spec) >= max)
        return -1;
    memcpy(path, spec, (size_t)(at - spec));
    path[at - spec] = 0;
    *windowed = 1;
    return resolve_partuuid(path, max);
}

/* The size and sector size of what h->fd names, and whether it is a block device. */
static int probe_target(hostdev_t *h, int flags) {
    struct stat st;
    uint64_t sz = 0;
    int ssz = 0;
    if (fstat(h->fd, &st)) return -1;
    h->block = S_ISBLK(st.st_mode);
    if (h->block) {
        if (ioctl(h->fd, BLKGETSIZE64, &sz) || ioctl(h->fd, BLKSSZGET, &ssz) || ssz <= 0 || (uint32_t)ssz > RWY_BLOCK_SIZE) return -1;
        flags |= O_DIRECT;
        if (!(fcntl(h->fd, F_GETFL) & O_DIRECT) && fcntl(h->fd, F_SETFL, fcntl(h->fd, F_GETFL) | O_DIRECT)) return -1;
        h->ssz = (uint32_t)ssz;
        h->span = sz;
    } else {
        h->ssz = FILE_SECTOR;
        h->span = (uint64_t)st.st_size;
    }
    h->direct = !!(flags & O_DIRECT);
    return 0;
}

/* A window is whole store blocks: its start and length are multiples of the block size, and it is inside the file. */
static int window_bad(const hostdev_t *h) {
    if (h->off % RWY_BLOCK_SIZE || h->len % RWY_BLOCK_SIZE) return 1;
    return h->off > h->span || (h->len ? h->len > h->span - h->off : h->off == h->span);
}

int hostdev_open(hostdev_t *h, const char *spec, int flags) {
    char path[4096];
    int rc;
    memset(h, 0, sizeof(*h));
    h->fd = -1;
    rc = hostdev_parse(spec, path, sizeof(path), &h->off, &h->len, &h->windowed);
    if (rc) { errno = rc == -2 ? ENOENT : EINVAL; return -1; }
    h->fd = open(path, flags);
    if (h->fd < 0) return -1;
    if (probe_target(h, flags) || (h->windowed && window_bad(h))) {
        int e = errno ? errno : EINVAL;
        close(h->fd);
        h->fd = -1;
        errno = e;
        return -1;
    }
    return 0;
}

void hostdev_close(hostdev_t *h) {
    if (h->fd >= 0) close(h->fd);
    h->fd = -1;
}

static int span_ok(const hostdev_t *h, uint64_t off, size_t n) {
    return !h->windowed || !h->len || (off <= h->len && n <= h->len - off);
}

/* One piece of a transfer through whole sectors of an aligned buffer; a read may start and end inside a sector. */
static int direct_piece(hostdev_t *h, uint64_t abs, uint8_t *b, size_t cap, void *buf, size_t n, int wr, size_t *used) {
    uint64_t start = abs / h->ssz * h->ssz;
    size_t head = (size_t)(abs - start), span = (head + n + h->ssz - 1) / h->ssz * h->ssz, take, done = 0;
    if (span > cap) span = cap;
    take = span - head < n ? span - head : n;
    if (wr) {
        memcpy(b, buf, take);
        while (done < span) {
            ssize_t r = pwrite(h->fd, b + done, span - done, (off_t)(start + done));
            if (r <= 0) return -1;
            h->stats.writes++;
            h->stats.write_bytes += (uint64_t)r;
            done += (size_t)r;
        }
    } else {
        while (done < head + take) {
            ssize_t r = pread(h->fd, b + done, span - done, (off_t)(start + done));
            if (r <= 0) return -1;
            h->stats.reads++;
            h->stats.read_bytes += (uint64_t)r;
            done += (size_t)r;
        }
        memcpy(buf, b + head, take);
    }
    *used = take;
    return 0;
}

/* O_DIRECT needs aligned memory; the core does not give it. */
static int direct_io(hostdev_t *h, uint64_t abs, void *buf, size_t n, int wr) {
    size_t cap = (size_t)((abs % h->ssz + n + h->ssz - 1) / h->ssz * h->ssz), used;
    void *b;
    int rc = 0;
    if (cap > DIRECT_MAX) cap = DIRECT_MAX;
    if (posix_memalign(&b, 4096, cap)) return -1;
    while (n && !rc) {
        rc = direct_piece(h, abs, b, cap, buf, n, wr, &used);
        abs += used;
        buf = (char *)buf + used;
        n -= used;
    }
    free(b);
    return rc;
}

static int hd_read(void *c, uint64_t off, void *buf, size_t n) {
    hostdev_t *h = c;
    size_t done = 0;
    if (!span_ok(h, off, n)) return -1;
    if (h->direct) return direct_io(h, h->off + off, buf, n, 0);
    while (done < n) {
        ssize_t r = pread(h->fd, (char *)buf + done, n - done, (off_t)(h->off + off + done));
        if (r <= 0) return -1;
        h->stats.reads++;
        h->stats.read_bytes += (uint64_t)r;
        done += (size_t)r;
    }
    return 0;
}

static int hd_write(void *c, uint64_t off, const void *buf, size_t n) {
    hostdev_t *h = c;
    size_t done = 0;
    if (!span_ok(h, off, n) || off % RWY_BLOCK_SIZE || n % RWY_BLOCK_SIZE) return -1;
    if (h->direct) return direct_io(h, h->off + off, (void *)(uintptr_t)buf, n, 1);
    while (done < n) {
        ssize_t r = pwrite(h->fd, (const char *)buf + done, n - done, (off_t)(h->off + off + done));
        if (r <= 0) return -1;
        h->stats.writes++;
        h->stats.write_bytes += (uint64_t)r;
        done += (size_t)r;
    }
    return 0;
}

static int hd_flush(void *c) {
    hostdev_t *h = c;

    if (fsync(h->fd)) return -1;
    h->stats.flushes++;
    return 0;
}

void hostdev_raw_dev(hostdev_t *h, rwy_block_dev_t *d) {
    struct stat st;
    uint64_t size = 0;
    if (!h->block && !fstat(h->fd, &st)) h->span = (uint64_t)st.st_size;   /* a file can grow after open */
    if (h->windowed && h->len) size = h->len;
    else if (h->span > h->off) size = h->span - h->off;
    d->read = hd_read; d->write = hd_write; d->flush = hd_flush;
    d->ctx = h; d->size = size; d->block_size = RWY_BLOCK_SIZE; d->stats = NULL; d->dircache = NULL;
}

void hostdev_dev(hostdev_t *h, rwy_block_dev_t *d) {
    rwy_superblock_t sb, best;
    uint8_t blk[RWY_BLOCK_SIZE];
    int i, found = 0;
    hostdev_raw_dev(h, d);
    h->unused = 0;
    if (!h->block && !(h->windowed && h->len)) return;
    for (i = 0; i < 2; i++) {
        if (d->read(h, (uint64_t)i * RWY_BLOCK_SIZE, blk, sizeof(blk)) || rwy_validate_sb_block(blk, &sb) != RWY_OK) continue;
        if (!found || sb.generation > best.generation) { best = sb; found = 1; }
    }
    if (!found || best.total_size > d->size) return;
    h->unused = d->size - best.total_size;
    d->size = best.total_size;
}

void hostdev_use_wall_clock(rwy_store_t *s) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    rwy_set_fixed_time(s, (int64_t)ts.tv_sec, (uint32_t)ts.tv_nsec);
}

int hostdev_source_epoch(int64_t *sec) {
    const char *e = getenv("SOURCE_DATE_EPOCH");
    char *end;
    long long v;
    if (!e || !*e) return 0;
    errno = 0;
    v = strtoll(e, &end, 10);
    if (errno || *end || v < 0) return -1;
    *sec = v;
    return 1;
}

void hostdev_use_build_clock(rwy_store_t *s) {
    int64_t sec;
    if (hostdev_source_epoch(&sec) == 1) rwy_set_fixed_time(s, sec, 0);
    else hostdev_use_wall_clock(s);
}
