/* hostdev: a store on the host, opened as a file, a window (offset and length) of an image, or a partition. */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HOSTDEV_H
#define HOSTDEV_H

#include <stddef.h>
#include <stdint.h>
#include "rwy_block.h"
#include "rwy_core.h"

/* A store spec is PATH, PATH@OFFSET or PATH@OFFSET+LENGTH (bytes, decimal or 0x). A PATH that exists wins.
 * PATH can be PARTUUID=<uuid> for a partition. The OFFSET and LENGTH of a window are multiples of 4096.
 * A write must be whole 4096-byte blocks at block offsets of the store; a read may be any size. */
typedef struct {
    int fd;
    uint64_t off, len;   /* the window start and size; len is 0 when it runs to the end of the file */
    int windowed;        /* the spec named a window: transfers outside it fail */
    int block;           /* a block device: opened with O_DIRECT */
    int direct;          /* O_DIRECT: transfers go through whole sectors of ssz bytes */
    uint32_t ssz;        /* sector size of a direct transfer */
    uint64_t span;       /* size of the file or device */
    uint64_t unused;     /* set by hostdev_dev: bytes of the window or device after the store */
    rwy_dev_stats_t stats;   /* system calls made since hostdev_open() */
} hostdev_t;

/* Prefix of /dev and /sys for PARTUUID= lookups ("" for the system; a test tree otherwise). */
extern const char *hostdev_root;

/* Split a spec into the file name and the window. 0 is done, -1 is a malformed window, -2 is a PARTUUID that is not found. */
int hostdev_parse(const char *spec, char *path, size_t max, uint64_t *off, uint64_t *len, int *windowed);
/* Open the file of a spec with open(2) flags. -1 and errno on failure; a window past the file end is EINVAL.
 * A block device always gets O_DIRECT; a file gets it only when flags has it. */
int hostdev_open(hostdev_t *h, const char *spec, int flags);
void hostdev_close(hostdev_t *h);
/* Fill a block device on h with the whole window, else the whole file (size 0 for an empty file). */
void hostdev_raw_dev(hostdev_t *h, rwy_block_dev_t *d);
/* Fill a block device on h for a store. On a block device or a window with a length, a valid superblock
 * gives the store size when it is not larger: the space after it stays unused (h->unused). */
void hostdev_dev(hostdev_t *h, rwy_block_dev_t *d);

/* Time stamps of the next changes come from the wall clock (the core has none). Call it before each operation. */
void hostdev_use_wall_clock(rwy_store_t *s);
/* SOURCE_DATE_EPOCH (reproducible builds): 1 and its seconds if set, 0 if not, -1 if it is not a decimal number. */
int hostdev_source_epoch(int64_t *sec);
/* For tools that build an image: SOURCE_DATE_EPOCH when set, else the wall clock. */
void hostdev_use_build_clock(rwy_store_t *s);

#endif
