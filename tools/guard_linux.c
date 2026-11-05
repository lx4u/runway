/* The boot loop guard counter seen from Linux: who started, and the clear at the health point. */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "guard_linux.h"

#define EFIVARS "/sys/firmware/efi/efivars/"
#define VENDOR "b371a076-b7ef-4e6e-b98a-8d0d2c493c7b"

int guard_started(const char *root, char *slot, size_t cap) {
    char path[PATH_MAX], v[64];
    FILE *f;
    size_t n;
    char *sp;
    snprintf(path, sizeof(path), "%s" EFIVARS "RunwayBoot-" VENDOR, root);
    if (!(f = fopen(path, "rb"))) return 0;
    n = fread(v, 1, sizeof(v) - 1, f);
    fclose(f);
    if (n <= 4) return 0;
    v[n] = 0;
    if ((sp = strchr(v + 4, ' '))) *sp = 0;
    snprintf(slot, cap, "%s", v + 4);
    return 1;
}

int guard_is_booted(rwy_store_t *s, const char *name, char *booted, size_t cap) {
    char link[64], *b;
    if (rwy_readlink(s, "EFI/RUNWAY/BOOTED", link, sizeof(link)) != RWY_OK) return 0;
    b = strrchr(link, '/');
    snprintf(booted, cap, "%s", b ? b + 1 : link);
    return !strcmp(name, "BOOTED") || !strcmp(name, booted);
}

/* efivarfs files are immutable until the flag is dropped. */
int guard_clear(const char *root) {
    char path[PATH_MAX];
    int fd, flags;
    snprintf(path, sizeof(path), "%s" EFIVARS "RunwayBootGuard-" VENDOR, root);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0) {
        if (!ioctl(fd, FS_IOC_GETFLAGS, &flags) && (flags & FS_IMMUTABLE_FL)) {
            flags &= ~FS_IMMUTABLE_FL;
            (void)ioctl(fd, FS_IOC_SETFLAGS, &flags);
        }
        close(fd);
    }
    return unlink(path) && errno != ENOENT ? -errno : 0;
}

int guard_healthy(rwy_store_t *s, const char *root, const char *name) {
    char booted[64], started[64];
    if (!guard_is_booted(s, name, booted, sizeof(booted))) return 0;
    if (guard_started(root, started, sizeof(started)) && strcmp(started, booted)) return 0;
    return guard_clear(root);
}
