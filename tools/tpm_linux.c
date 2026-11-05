/*
 * tools/tpm_linux.c — see tpm_linux.h.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "tpm_linux.h"

static int connect_socket(const char *path) {
    struct sockaddr_un a;
    int fd;
    if (strlen(path) >= sizeof(a.sun_path)) { errno = ENAMETOOLONG; return -1; }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, path);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a))) { int e = errno; close(fd); errno = e; return -1; }
    return fd;
}

/* A device returns the whole reply to one read; a socket may split it, so read up to the size in the header. */
static int submit(void *ctx, const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t *out_len) {
    int fd = (int)(long)ctx;
    uint32_t have = 0, want = 10;
    ssize_t n;

    if (write(fd, in, in_len) != (ssize_t)in_len) return -1;
    while (have < want) {
        n = read(fd, out + have, *out_len - have);
        if (n <= 0) return -1;
        have += (uint32_t)n;
        if (have >= 10) {
            want = ((uint32_t)out[2] << 24) | ((uint32_t)out[3] << 16) | ((uint32_t)out[4] << 8) | out[5];
            if (want < 10 || want > *out_len) return -1;
        }
    }
    *out_len = have;
    if ((((uint32_t)out[6] << 24) | ((uint32_t)out[7] << 16) | ((uint32_t)out[8] << 8) | out[9]) == TPM_RC_RETRY)
        nanosleep(&(struct timespec){ 0, 50000000 }, NULL);
    return 0;
}

int tpm_linux_open(rwy_tpm_xport_t *x, const char *dev) {
    struct stat st;
    int fd;
    if (stat(dev, &st)) return -1;
    fd = S_ISSOCK(st.st_mode) ? connect_socket(dev) : open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    x->submit = submit;
    x->ctx = (void *)(long)fd;
    x->rc = 0;
    return 0;
}

void tpm_linux_close(rwy_tpm_xport_t *x) {
    close((int)(long)x->ctx);
}
