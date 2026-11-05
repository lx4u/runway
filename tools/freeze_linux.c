/* tools/freeze_linux.c: the freeze socket of rwy-fuse, shared by rwy-fuse and rwyctl. */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "freeze_linux.h"
#include "rwy_core.h"

#define FREEZE_DEFAULT_DIR "/run/rwy-fuse"

const char *freeze_dir(void) {
    const char *d = getenv("RWY_RUN_DIR");
    return d && *d ? d : FREEZE_DEFAULT_DIR;
}

int freeze_path(char *out, size_t n, const char *dir, const uint8_t uuid[16]) {
    char u[37];
    struct sockaddr_un sa;
    int len;

    rwy_format_uuid(uuid, u);
    len = snprintf(out, n, "%s/%s.sock", dir, u);
    if (len < 0 || (size_t)len >= n || (size_t)len >= sizeof(sa.sun_path)) {
        fprintf(stderr, "freeze: socket path too long: %s/%s.sock\n", dir, u);
        return -1;
    }
    return 0;
}

static int make_addr(struct sockaddr_un *sa, const char *dir, const uint8_t uuid[16]) {
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    return freeze_path(sa->sun_path, sizeof(sa->sun_path), dir, uuid);
}

int freeze_listen(const char *dir, const uint8_t uuid[16]) {
    struct sockaddr_un sa;
    int fd;

    if (make_addr(&sa, dir, uuid)) return -1;
    if (mkdir(dir, 0755) && errno != EEXIST) {
        fprintf(stderr, "freeze: cannot create %s: %s\n", dir, strerror(errno));
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "freeze: socket: %s\n", strerror(errno));
        return -1;
    }
    unlink(sa.sun_path);
    /* Set the mode before listen(): nobody can connect while it is still open to others. */
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) || chmod(sa.sun_path, 0600) || listen(fd, 4)) {
        fprintf(stderr, "freeze: cannot listen on %s: %s\n", sa.sun_path, strerror(errno));
        unlink(sa.sun_path);
        close(fd);
        return -1;
    }
    return fd;
}

int freeze_accept(int fd) {
    struct ucred cred;
    socklen_t len = sizeof(cred);
    int c;

    do c = accept4(fd, NULL, NULL, SOCK_CLOEXEC); while (c < 0 && errno == EINTR);
    if (c < 0) return -1;
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) || cred.uid != geteuid()) {
        close(c);
        return -1;
    }
    return c;
}

void freeze_close(int fd) {
    struct sockaddr_un sa;
    socklen_t len = sizeof(sa);

    if (fd < 0) return;
    if (!getsockname(fd, (struct sockaddr *)&sa, &len) && len > sizeof(sa.sun_family) && sa.sun_path[0])
        unlink(sa.sun_path);
    close(fd);
}

int freeze_reply(int conn, const char *line) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s\n", line);
    ssize_t w;

    if (n < 0 || (size_t)n >= sizeof(buf)) return -1;
    do w = send(conn, buf, (size_t)n, MSG_NOSIGNAL); while (w < 0 && errno == EINTR);
    return w == n ? 0 : -1;
}

int freeze_connect(const char *dir, const uint8_t uuid[16]) {
    struct sockaddr_un sa;
    int fd, rc;

    if (make_addr(&sa, dir, uuid)) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    do rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa)); while (rc < 0 && errno == EINTR);
    if (rc) {
        close(fd);
        return -1;
    }
    return fd;
}

static long ms_left(const struct timespec *end) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (end->tv_sec - now.tv_sec) * 1000 + (end->tv_nsec - now.tv_nsec) / 1000000;
}

int freeze_read_answer(int fd, char *buf, size_t n, int timeout_ms) {
    struct timespec end;
    size_t len = 0;

    if (n < 2) return -1;
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += timeout_ms / 1000;
    end.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (end.tv_nsec >= 1000000000L) { end.tv_sec++; end.tv_nsec -= 1000000000L; }

    while (len < n - 1) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        long left = ms_left(&end);
        char ch;
        int rc;

        if (left <= 0) return -1;
        rc = poll(&p, 1, (int)left);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return -1;
        rc = (int)read(fd, &ch, 1);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return -1;
        if (ch == '\n') {
            buf[len] = 0;
            return (int)len;
        }
        buf[len++] = ch;
    }
    return -1;
}

/* Decode the \ooo escapes of a mountinfo field, up to the space that ends it. */
static void unescape_field(const char *in, char *out, size_t n) {
    size_t len = 0;

    while (*in && *in != ' ' && len + 1 < n) {
        if (in[0] == '\\' && in[1] >= '0' && in[1] <= '3' && in[2] >= '0' && in[2] <= '7' && in[3] >= '0' && in[3] <= '7') {
            out[len++] = (char)((in[1] - '0') * 64 + (in[2] - '0') * 8 + (in[3] - '0'));
            in += 4;
        } else {
            out[len++] = *in++;
        }
    }
    out[len] = 0;
}

/* The mount point of a mountinfo line whose source is `source`, or NULL. */
static const char *line_mount_point(const char *line, const char *source, size_t slen) {
    const char *sep = strstr(line, " - "), *p = line;
    int i;

    if (!sep) return NULL;
    sep += 3;
    sep = strchr(sep, ' ');
    if (!sep || strncmp(sep + 1, source, slen) || (sep[1 + slen] != ' ' && sep[1 + slen] != '\n' && sep[1 + slen] != 0)) return NULL;
    for (i = 0; i < 4 && p; i++) p = strchr(p, ' '), p = p ? p + 1 : NULL;
    return p;
}

int freeze_find_mount(const uint8_t uuid[16], char *out, size_t n) {
    char line[4096], source[64];
    FILE *f = fopen("/proc/self/mountinfo", "r");
    int found = -1;

    if (!f) return -1;
    rwy_format_uuid(uuid, source + sizeof(RWY_FUSE_FSNAME) - 1);
    memcpy(source, RWY_FUSE_FSNAME, sizeof(RWY_FUSE_FSNAME) - 1);
    while (found && fgets(line, sizeof(line), f)) {
        const char *mp = line_mount_point(line, source, strlen(source));
        if (mp) {
            unescape_field(mp, out, n);
            found = 0;
        }
    }
    fclose(f);
    return found;
}
