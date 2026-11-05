/* rwyctl: content-named blobs. Hash a source, find or store BLOBS/<hash>, link it where it is wanted. */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rwyctl.h"
#include "rwy_blob.h"

#define BLOBS_DIR EFI_ROOT "/BLOBS"
#define STAGE_PATH BLOBS_DIR "/" RWY_BLOB_STAGE_NAME
#define COPY_CHUNK (1u << 20)

static void blob_path(char *out, const char *name) {
    snprintf(out, PATH_MAX, BLOBS_DIR "/%s", name);
}

int blob_exists(const char *name) {
    char p[PATH_MAX];
    rwy_attr_t a;

    blob_path(p, name);
    return rwy_stat(&g_store, p, &a) == RWY_OK;
}

int blob_available(const char *name) {
    return blob_exists(name);
}

int blob_changed(const char *name) {
    warn("the file of blob %s changed: run the command again", name);
    return RWY_ERR_IO;
}

int blob_missing(const char *name) {
    warn("no such blob: %s", name);
    return RWY_ERR_NOENT;
}

int blob_hash_file(const char *src, char name[RWY_BLOB_NAME_SIZE], uint64_t *size) {
    uint8_t digest[32];
    struct stat st;
    int fd = open(src, O_RDONLY), rc;

    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        if (fd >= 0) close(fd);
        warn("cannot read %s", src);
        return RWY_ERR_NOENT;
    }
    rc = rwy_blob_hash_fd(fd, digest);
    close(fd);
    if (rc) { warn("cannot read %s", src); return rc; }
    rwy_blob_name(digest, name);
    *size = (uint64_t)st.st_size;
    return RWY_OK;
}

/* Copy the source into the stage file and hash it again: the name must still be the one of pass one. */
static int stage_file(const char *src, const char *name) {
    static char chunk[COPY_CHUNK];
    rwy_sha256_stream_t h;
    uint8_t digest[32];
    char again[RWY_BLOB_NAME_SIZE];
    uint64_t pos = 0;
    ssize_t n = 0;
    struct stat st;
    int fd = open(src, O_RDONLY), rc;

    if (fd < 0) { warn("cannot read %s", src); return RWY_ERR_NOENT; }
    rc = rwy_create(&g_store, STAGE_PATH, RWY_S_IFREG | 0644, 0, 0);
    rwy_sha256_stream_init(&h);
    while (!rc && (n = read(fd, chunk, sizeof(chunk))) > 0) {
        rwy_sha256_stream_update(&h, chunk, (size_t)n);
        rc = rwy_write_path(&g_store, STAGE_PATH, pos, chunk, (size_t)n);
        pos += (uint64_t)n;
    }
    if (!rc && n < 0) { warn("cannot read %s", src); rc = RWY_ERR_IO; }
    if (!rc) rc = fstat(fd, &st) ? RWY_ERR_IO : stamp_times(STAGE_PATH, &st);
    close(fd);
    rwy_sha256_stream_final(&h, digest);
    rwy_blob_name(digest, again);
    if (!rc && strcmp(again, name)) { warn("%s changed while it was read: run the command again", src); rc = RWY_ERR_IO; }
    return rc;
}

/* Give the stage file its content name. Only SYSTEM makes one, so the actor is raised for this call alone. */
static int adopt_stage(const char *name) {
    char blob[PATH_MAX];
    int actor = g_store.actor, rc;

    blob_path(blob, name);
    rwy_set_actor(&g_store, RWY_ACTOR_SYSTEM);
    rc = rwy_rename(&g_store, STAGE_PATH, blob);
    rwy_set_actor(&g_store, actor);
    return rc;
}

/* Make BLOBS/name exist: store the source. */
static int blob_make(const char *name, const char *src) {
    rwy_attr_t a;
    int rc;

    if (rwy_stat(&g_store, BLOBS_DIR, &a) && (rc = rwy_mkdir(&g_store, BLOBS_DIR, RWY_S_IFDIR | 0755, 0, 0))) return rc;
    if (!src) return blob_missing(name);
    if ((rc = stage_file(src, name))) return rc;
    return adopt_stage(name);
}

/* With SOURCE_DATE_EPOCH the times are capped by it, and the change time is it. */
int stamp_times(const char *dst, const struct stat *st) {
    int64_t epoch, m = st->st_mtim.tv_sec, c = st->st_ctim.tv_sec;
    uint32_t mn = (uint32_t)st->st_mtim.tv_nsec, cn = (uint32_t)st->st_ctim.tv_nsec;
    if (hostdev_source_epoch(&epoch) == 1) {
        if (m > epoch) { m = epoch; mn = 0; }
        c = epoch; cn = 0;
    }
    return rwy_set_times(&g_store, dst, m, mn, c, cn);
}

int blob_link(const char *name, const char *src, const char *dst) {
    char blob[PATH_MAX];
    int rc;

    if (!blob_exists(name) && (rc = blob_make(name, src))) return rc;
    blob_path(blob, name);
    return rwy_link(&g_store, blob, dst);
}

int blob_copy(const char *name, const char *dst) {
    static char chunk[COPY_CHUNK];
    char blob[PATH_MAX];
    uint64_t pos = 0;
    size_t n;
    rwy_attr_t a;
    int rc;

    if (!blob_exists(name)) return blob_missing(name);
    blob_path(blob, name);
    if ((rc = rwy_create(&g_store, dst, RWY_S_IFREG | 0644, 0, 0))) return rc;
    do {
        n = sizeof(chunk);
        if ((rc = rwy_read_path(&g_store, blob, pos, chunk, &n)) || !n) break;
        rc = rwy_write_path(&g_store, dst, pos, chunk, n);
        pos += n;
    } while (!rc);
    if (rc || (rc = rwy_stat(&g_store, blob, &a))) return rc;
    return rwy_set_times(&g_store, dst, a.mtime_sec, a.mtime_nsec, a.ctime_sec, a.ctime_nsec);
}
