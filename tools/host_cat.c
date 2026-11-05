/* host_cat — dump a file from a Runway store to stdout. Usage: host_cat STORE /PATH
 * SPDX-License-Identifier: Apache-2.0
 * STORE is a file or IMAGE@OFFSET[+LENGTH]. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_format.h"
#include "hostdev.h"

static int wr(void *ctx, uint64_t o, const void *b, size_t l) { (void)ctx; (void)o; (void)b; (void)l; return -1; }
static int fl(void *ctx) { (void)ctx; return 0; }

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s STORE /PATH\n", argv[0]); return 2; }
    hostdev_t hd;
    rwy_block_dev_t bdev;
    if (hostdev_open(&hd, argv[1], O_RDONLY)) { perror(argv[1]); return 2; }
    hostdev_dev(&hd, &bdev);
    bdev.write = wr; bdev.flush = fl;
    rwy_store_t st;
    if (rwy_mount(&st, &bdev) != RWY_OK) { fprintf(stderr, "mount failed\n"); return 1; }
    static char buf[262144]; size_t len = sizeof buf;
    if (rwy_read_path(&st, argv[2], 0, buf, &len) != RWY_OK) { fprintf(stderr, "read failed\n"); return 1; }
    fwrite(buf, 1, len, stdout);
    return 0;
}
