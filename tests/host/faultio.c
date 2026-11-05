/*
 * LD_PRELOAD shim for test_rwyctl.sh, so root is not needed.
 * SPDX-License-Identifier: Apache-2.0
 *   FAULTIO_FAIL_PWRITE=SUFFIX  pwrite on a file whose path ends in SUFFIX fails with EIO
 *   FAULTIO_CUT=SUFFIX:N        the process dies (power cut) at the N+1th pwrite on a file ending in SUFFIX
 *   FAULTIO_EROFS=PATH          open(PATH, O_RDWR) fails with EROFS until mount() is called
 *   FAULTIO_LOG=FILE            mount() appends "target data" to FILE and succeeds
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int remounted;

static int open_any(const char *name, const char *path, int flags, mode_t mode) {
    int (*real)(const char *, int, ...) = dlsym(RTLD_NEXT, name);
    const char *erofs = getenv("FAULTIO_EROFS");
    if (erofs && !remounted && (flags & O_ACCMODE) != O_RDONLY && !strcmp(path, erofs)) {
        errno = EROFS;
        return -1;
    }
    return real(path, flags, mode);
}

int open(const char *path, int flags, ...) {
    va_list ap;
    mode_t mode;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
    return open_any("open", path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    va_list ap;
    mode_t mode;
    va_start(ap, flags);
    mode = va_arg(ap, mode_t);
    va_end(ap);
    return open_any("open64", path, flags, mode);
}

int mount(const char *src, const char *target, const char *type, unsigned long flags, const void *data) {
    const char *log = getenv("FAULTIO_LOG");
    FILE *f;
    (void)src; (void)type; (void)flags;
    if (log && (f = fopen(log, "a"))) {
        fprintf(f, "%s %s\n", target, data ? (const char *)data : "");
        fclose(f);
    }
    remounted = 1;
    return 0;
}

static int path_ends_with(int fd, const char *suffix) {
    char link[64], path[PATH_MAX];
    ssize_t len;
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    len = readlink(link, path, sizeof(path) - 1);
    if (len <= 0) return 0;
    path[len] = 0;
    return (size_t)len >= strlen(suffix) && !strcmp(path + len - strlen(suffix), suffix);
}

static ssize_t pwrite_any(const char *name, int fd, const void *buf, size_t n, off_t off) {
    ssize_t (*real)(int, const void *, size_t, off_t) = dlsym(RTLD_NEXT, name);
    const char *suffix = getenv("FAULTIO_FAIL_PWRITE");
    const char *cut = getenv("FAULTIO_CUT");
    static long seen;
    if (suffix && path_ends_with(fd, suffix)) {
        errno = EIO;
        return -1;
    }
    if (cut) {
        const char *colon = strrchr(cut, ':');
        char sfx[PATH_MAX];
        if (colon && (size_t)(colon - cut) < sizeof(sfx)) {
            memcpy(sfx, cut, colon - cut);
            sfx[colon - cut] = 0;
            if (path_ends_with(fd, sfx) && seen++ >= atol(colon + 1)) _exit(137);
        }
    }
    return real(fd, buf, n, off);
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return pwrite_any("pwrite", fd, buf, n, off); }
ssize_t pwrite64(int fd, const void *buf, size_t n, off_t off) { return pwrite_any("pwrite64", fd, buf, n, off); }
