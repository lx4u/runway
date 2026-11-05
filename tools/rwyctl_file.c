/* rwyctl: whole-file `put` and `cat` for host scripts and the benchmark. */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "rwyctl.h"

#define FILE_CHUNK (1024u * 1024u)

int cmd_put(int argc, char **argv) {
    char *buf;
    FILE *f;
    uint64_t off = 0;
    int rc;

    if (argc != 2) { warn("%s", "usage: put PATH SRC"); return X_USAGE; }
    f = fopen(argv[1], "rb");
    if (!f) { warn("cannot read %s", argv[1]); return X_IO; }
    buf = malloc(FILE_CHUNK);
    if (!buf) { fclose(f); return X_IO; }
    if (rwy_begin(&g_store)) { free(buf); fclose(f); return X_IO; }
    if (rwy_stat(&g_store, argv[0], &(rwy_attr_t){0}) == RWY_ERR_NOENT)
        rc = rwy_create(&g_store, argv[0], RWY_S_IFREG | 0644, 0, 0);
    else
        rc = rwy_truncate(&g_store, argv[0], 0);
    while (!rc) {
        size_t n = fread(buf, 1, FILE_CHUNK, f);
        if (!n) break;
        rc = rwy_write_path(&g_store, argv[0], off, buf, n);
        off += n;
    }
    if (!rc && ferror(f)) { warn("cannot read %s", argv[1]); rc = RWY_ERR_IO; }
    free(buf);
    fclose(f);
    return finish(rc);
}

int cmd_cat(int argc, char **argv) {
    rwy_attr_t a;
    char *buf;
    uint64_t off = 0;
    int rc = 0;

    if (argc != 1) { warn("%s", "usage: cat PATH"); return X_USAGE; }
    if ((rc = rwy_stat(&g_store, argv[0], &a))) { warn("cannot stat %s", rwy_strerror(rc)); return exit_of(rc); }
    if (RWY_S_ISDIR(a.mode)) { warn("%s", "is a directory"); return X_USAGE; }
    buf = malloc(FILE_CHUNK);
    if (!buf) return X_IO;
    while (off < a.size && !rc) {
        size_t n = a.size - off < FILE_CHUNK ? (size_t)(a.size - off) : FILE_CHUNK;
        rc = rwy_read_path(&g_store, argv[0], off, buf, &n);
        if (!rc && fwrite(buf, 1, n, stdout) != n) rc = RWY_ERR_IO;
        off += n;
    }
    free(buf);
    if (rc) { warn("cannot read %s", rwy_strerror(rc)); return exit_of(rc); }
    return fflush(stdout) ? X_IO : 0;
}
