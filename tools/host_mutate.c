/*
 * host_mutate.c — Mutate a Runway store on the host to advance generation.
 * SPDX-License-Identifier: Apache-2.0
 * Usage: host_mutate <STORE_FILE> [FILE_PATH] [DATA]
 * STORE_FILE is a file or IMAGE@OFFSET[+LENGTH].
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_format.h"
#include "hostdev.h"

/* The bytes to write: the text itself, or the content of the host file when it starts with '@'. */
static const char *load_data(const char *data, size_t *dlen) {
    FILE *in;
    char *fbuf;

    *dlen = strlen(data);
    if (data[0] != '@') return data;
    in = fopen(data + 1, "rb");
    if (!in) { perror("fopen data"); exit(1); }
    fseek(in, 0, SEEK_END); *dlen = (size_t)ftell(in); rewind(in);
    fbuf = malloc(*dlen);
    if (!fbuf || fread(fbuf, 1, *dlen, in) != *dlen) { fprintf(stderr, "read data\n"); exit(1); }
    fclose(in);
    return fbuf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <store_path> [file_to_create] [text|@binfile]\n", argv[0]);
        return 1;
    }
    const char *store_path = argv[1];
    const char *create_path = (argc >= 3) ? argv[2] : "/HOST_MUTATION.TXT";
    const char *data = (argc >= 4) ? argv[3] : "Host write generation advance\n";

    hostdev_t fdev;
    if (hostdev_open(&fdev, store_path, O_RDWR)) { perror(store_path); return 1; }

    rwy_block_dev_t bdev;
    hostdev_dev(&fdev, &bdev);
    bdev.size = bdev.size ? bdev.size : RWY_STORE_SIZE;

    rwy_store_t store;
    int rc = rwy_mount(&store, &bdev);
    if (rc != RWY_OK) { fprintf(stderr, "rwy_mount failed: %d\n", rc); hostdev_close(&fdev); return 1; }

    rc = rwy_begin(&store);
    if (rc != RWY_OK) { fprintf(stderr, "rwy_begin failed: %d\n", rc); hostdev_close(&fdev); return 1; }

    rc = rwy_create(&store, create_path, RWY_S_IFREG | 0644, 0, 0);
    if (rc != RWY_OK && rc != RWY_ERR_EXIST) {
        fprintf(stderr, "rwy_create failed: %d\n", rc); hostdev_close(&fdev); return 1;
    }

    size_t dlen;
    data = load_data(data, &dlen);
    rc = rwy_write_path(&store, create_path, 0, data, dlen);
    if (rc != RWY_OK) { fprintf(stderr, "rwy_write_path failed: %d\n", rc); hostdev_close(&fdev); return 1; }

    rc = rwy_commit(&store);
    if (rc != RWY_OK) { fprintf(stderr, "rwy_commit failed: %d\n", rc); hostdev_close(&fdev); return 1; }
    bdev.flush(bdev.ctx);

    printf("host_mutate: committed generation %llu to %s\n",
           (unsigned long long)store.sb.generation, store_path);

    hostdev_close(&fdev);
    return 0;
}
