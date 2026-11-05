/* tools/rwyctl_rpc.c: client/server RPC protocol between rwyctl and rwy-fuse. */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>

#include "rwyctl.h"
#include "rwyctl_cmd.h"
#include "rwyctl_rpc.h"
#include "freeze_linux.h"

#define MAX_RPC_FDS 16
#define MAX_RPC_PAYLOAD 65536
#define RPC_MAGIC 0x52575932

struct rpc_hdr {
    uint32_t magic;
    uint32_t num_fds;
    uint32_t argc;
    uint32_t total_len;
};

extern int store_commit_active(void);

int rwyctl_rpc_available(const uint8_t uuid[16]) {
    char path[PATH_MAX];
    struct stat st;

    return !freeze_path(path, sizeof(path), freeze_dir(), uuid) && !stat(path, &st) && S_ISSOCK(st.st_mode);
}

int rwyctl_rpc_connect(const uint8_t uuid[16]) {
    return freeze_connect(freeze_dir(), uuid);
}

static int on_rwy_fuse(dev_t dev) {
    char line[PATH_MAX + 256];
    FILE *f = fopen("/proc/self/mountinfo", "r");
    int found = 0;

    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char fs[64];
        unsigned maj, min;
        char *sep = strstr(line, " - ");
        if (!sep) continue;
        if (sscanf(line, "%*d %*d %u:%u", &maj, &min) != 2) continue;
        if (sscanf(sep + 3, "%63s", fs) == 1 && !strcmp(fs, "fuse.rwy-fuse") && makedev(maj, min) == dev) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static int scratch_open(void) {
    const char *dir = getenv("TMPDIR");
    int fd = open(dir && dir[0] ? dir : "/var/tmp", O_TMPFILE | O_RDWR, 0600);

    return fd >= 0 ? fd : memfd_create("rwyctl", 0);
}

static int copy_to_scratch(const char *p, int *fd_out) {
    char buf[65536];
    int in = open(p, O_RDONLY), out;
    ssize_t n;

    if (in < 0) { warn("cannot read %s", p); return X_IO; }
    out = scratch_open();
    if (out < 0) { close(in); warn("no scratch file for %s", p); return X_IO; }
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        char *at = buf;
        while (n > 0) {
            ssize_t w = write(out, at, (size_t)n);
            if (w < 0 && errno == EINTR) continue;
            if (w < 0) {
                close(in); close(out);
                warn("cannot write the scratch copy of %s", p);
                return X_IO;
            }
            at += w;
            n -= w;
        }
    }
    close(in);
    if (n < 0) { close(out); warn("cannot read %s", p); return X_IO; }
    lseek(out, 0, SEEK_SET);
    *fd_out = out;
    return 0;
}

static const char *const path_options[] = { "--kernel", "--initrd", "--dtb", NULL };
static const char *const text_options[] = { "--args", "--label", "--meta", "--entry", NULL };

static int is_one_of(const char *s, const char *const *list) {
    for (; *list; list++) if (!strcmp(s, *list)) return 1;
    return 0;
}

static int is_wdt_input(const char *cmd, int argc, char **argv, int i) {
    const char *a = argv[i];

    if (strcmp(cmd, "wdt") || argc <= 1 || strcmp(argv[1], "set")) return 0;
    return i >= 2 && strcmp(a, "--unlock") && strncmp(a, "blob:", 5) ? 1 : 0;
}

static int is_install_input(const char *cmd, int argc, char **argv, int i) {
    const char *a = argv[i];

    if (strcmp(cmd, "install") && strcmp(cmd, "update")) {
        if (strcmp(cmd, "recovery") || argc < 2 || strcmp(argv[1], "set")) return 0;
    }
    if (i > 0 && is_one_of(argv[i - 1], path_options))
        return strncmp(a, "blob:", 5) ? 1 : 0;
    if (i > 0 && is_one_of(argv[i - 1], text_options))
        return a[0] == '@' ? 2 : 0;
    return 0;
}

static int input_kind(const char *cmd, int argc, char **argv, int i) {
    if (!strcmp(cmd, "put")) return i == 2;
    if (!strcmp(cmd, "set")) return i == argc - 1 && i >= 2 && argv[i][0] == '@' ? 2 : 0;
    if (is_wdt_input(cmd, argc, argv, i)) return 1;
    return is_install_input(cmd, argc, argv, i);
}

static int prepare_one_input(const char *p, int *out_fd) {
    struct stat st;

    if (stat(p, &st)) { warn("cannot read %s", p); return X_IO; }
    if (on_rwy_fuse(st.st_dev)) return copy_to_scratch(p, out_fd);
    *out_fd = open(p, O_RDONLY | O_CLOEXEC);
    if (*out_fd < 0) { warn("cannot read %s", p); return X_IO; }
    return 0;
}

static int prepare_client_fds(int argc, char **argv, int *fds, size_t *num_fds, char **rpc_argv) {
    int i, rc;

    fds[0] = dup(STDOUT_FILENO);
    fds[1] = dup(STDERR_FILENO);
    *num_fds = 2;
    rpc_argv[0] = argv[0];

    for (i = 1; i < argc; i++) {
        int kind = input_kind(argv[0], argc, argv, i);
        if (kind) {
            int in_fd;
            char tag[32];
            rc = prepare_one_input(argv[i] + (kind == 2), &in_fd);
            if (rc) return rc;
            if (*num_fds >= MAX_RPC_FDS) { close(in_fd); return X_USAGE; }
            snprintf(tag, sizeof(tag), "%s__FD_%u__", kind == 2 ? "@" : "", (unsigned)*num_fds);
            fds[(*num_fds)++] = in_fd;
            rpc_argv[i] = strdup(tag);
        } else {
            rpc_argv[i] = argv[i];
        }
    }
    return 0;
}

static int serialize_args(int argc, char **rpc_argv, char *buf, size_t max_len, uint32_t *out_len) {
    size_t off = 0;
    int i;

    for (i = 0; i < argc; i++) {
        size_t len = strlen(rpc_argv[i]) + 1;
        if (off + len > max_len) return X_USAGE;
        memcpy(buf + off, rpc_argv[i], len);
        off += len;
    }
    *out_len = (uint32_t)off;
    return 0;
}

static int send_rpc_msg(int sock_fd, const struct rpc_hdr *hdr, const char *payload,
                        const int *fds, size_t num_fds) {
    struct msghdr msg = { 0 };
    struct iovec iov[2];
    char cmsg_buf[CMSG_SPACE(MAX_RPC_FDS * sizeof(int))];
    ssize_t total = (ssize_t)(sizeof(*hdr) + hdr->total_len), sent;

    iov[0].iov_base = (void *)hdr;
    iov[0].iov_len = sizeof(*hdr);
    iov[1].iov_base = (void *)payload;
    iov[1].iov_len = hdr->total_len;
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = CMSG_SPACE(num_fds * sizeof(int));

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(num_fds * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, num_fds * sizeof(int));

    sent = sendmsg(sock_fd, &msg, MSG_NOSIGNAL);
    return sent == total ? 0 : -1;
}

static int check_disruptive(int argc, char **argv) {
    if (argc < 1) return 0;
    if (!strcmp(argv[0], "reencrypt") || !strcmp(argv[0], "init")) return 1;
    if (!strcmp(argv[0], "check") && has_arg(argc - 1, argv + 1, "--repair")) return 1;
    return 0;
}

static void cleanup_client(size_t num_fds, const int *fds, int argc, char **argv, char **rpc_argv) {
    size_t i;
    for (i = 0; i < num_fds; i++) close(fds[i]);
    for (i = 1; i < (size_t)argc; i++) if (rpc_argv[i] != argv[i]) free(rpc_argv[i]);
}

static void log_mount_info(const uint8_t uuid[16]) {
    char mnt[PATH_MAX];
    if (freeze_find_mount(uuid, mnt, sizeof(mnt))) mnt[0] = 0;
    warn("store mounted at %s", mnt[0] ? mnt : "(not found)");
    warn("%s", "running command via rwy-fuse RPC");
}

static int exchange_rpc(int sock_fd, int argc, char **rpc_argv, const int *fds, size_t num_fds) {
    char payload[MAX_RPC_PAYLOAD];
    struct rpc_hdr hdr = { .magic = RPC_MAGIC, .argc = (uint32_t)argc, .num_fds = (uint32_t)num_fds };
    int32_t exit_code = X_IO;
    int rc = serialize_args(argc, rpc_argv, payload, sizeof(payload), &hdr.total_len);
    if (!rc && send_rpc_msg(sock_fd, &hdr, payload, fds, num_fds) < 0) rc = X_IO;
    if (!rc) {
        ssize_t r = recv(sock_fd, &exit_code, sizeof(exit_code), 0);
        rc = r == sizeof(exit_code) ? (int)exit_code : X_IO;
    }
    return rc;
}

int rwyctl_rpc_client(const uint8_t uuid[16], int argc, char **argv) {
    int fds[MAX_RPC_FDS];
    char *rpc_argv[argc > 0 ? argc : 1];
    size_t num_fds = 0;

    if (!rwyctl_rpc_available(uuid)) return -1;
    if (check_disruptive(argc, argv)) {
        if (g_verbose) log_mount_info(uuid);
        warn("%s", "store busy: it is mounted by rwy-fuse, unmount it first");
        return X_BUSY;
    }

    int rc = prepare_client_fds(argc, argv, fds, &num_fds, rpc_argv);
    if (rc) return rc;
    int sock_fd = rwyctl_rpc_connect(uuid);
    if (sock_fd < 0) {
        cleanup_client(num_fds, fds, argc, argv, rpc_argv);
        return rwyctl_rpc_available(uuid) ? -1 : X_IO;
    }
    if (g_verbose) log_mount_info(uuid);

    rc = exchange_rpc(sock_fd, argc, rpc_argv, fds, num_fds);
    close(sock_fd);
    cleanup_client(num_fds, fds, argc, argv, rpc_argv);
    return rc;
}

static int recv_all(int fd, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t r = recv(fd, (char *)buf + got, len - got, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int recv_rpc_msg(int conn, struct rpc_hdr *hdr, char *payload, size_t max_payload,
                        int *fds, size_t *num_fds) {
    struct msghdr msg = { 0 };
    struct iovec iov[1];
    char cmsg_buf[CMSG_SPACE(MAX_RPC_FDS * sizeof(int))];

    iov[0].iov_base = hdr;
    iov[0].iov_len = sizeof(*hdr);
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    if (recvmsg(conn, &msg, 0) != sizeof(*hdr)) return -1;
    if (hdr->magic != RPC_MAGIC || hdr->num_fds < 2 || hdr->num_fds > MAX_RPC_FDS) return -1;
    if (hdr->total_len > max_payload) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) return -1;
    *num_fds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    if (*num_fds != hdr->num_fds) return -1;
    memcpy(fds, CMSG_DATA(cmsg), (*num_fds) * sizeof(int));

    return recv_all(conn, payload, hdr->total_len);
}

static int unpack_args(int argc, char *payload, size_t total_len, const int *fds, size_t num_fds,
                       char **out_argv, char fd_paths[MAX_RPC_FDS][64]) {
    size_t off = 0;
    int i;

    for (i = 0; i < argc; i++) {
        if (off >= total_len) return -1;
        char *str = payload + off;
        size_t len = strlen(str);
        off += len + 1;
        int at = str[0] == '@';
        if (!strncmp(str + at, "__FD_", 5)) {
            unsigned idx = (unsigned)atoi(str + at + 5);
            if (idx >= num_fds) return -1;
            snprintf(fd_paths[idx], 64, "%s/proc/self/fd/%d", at ? "@" : "", fds[idx]);
            out_argv[i] = fd_paths[idx];
        } else {
            out_argv[i] = str;
        }
    }
    out_argv[argc] = NULL;
    return 0;
}

static int is_disruptive(int argc, char **argv) {
    if (argc < 1) return 0;
    if (!strcmp(argv[0], "reencrypt") || !strcmp(argv[0], "init")) return 1;
    if (!strcmp(argv[0], "check") && has_arg(argc - 1, argv + 1, "--repair")) return 1;
    return 0;
}

void rwyctl_rpc_server_handle(int conn) {
    struct rpc_hdr hdr;
    char payload[MAX_RPC_PAYLOAD];
    int fds[MAX_RPC_FDS];
    size_t num_fds = 0, i;
    char fd_paths[MAX_RPC_FDS][64];

    if (recv_rpc_msg(conn, &hdr, payload, sizeof(payload), fds, &num_fds)) {
        for (i = 0; i < num_fds; i++) close(fds[i]);
        return;
    }
    char *cmd_argv[hdr.argc + 1];
    if (unpack_args((int)hdr.argc, payload, hdr.total_len, fds, num_fds, cmd_argv, fd_paths)) {
        for (i = 0; i < num_fds; i++) close(fds[i]);
        return;
    }

    int saved_out = dup(STDOUT_FILENO);
    int saved_err = dup(STDERR_FILENO);
    dup2(fds[0], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);

    int rc;
    if (is_disruptive((int)hdr.argc, cmd_argv)) {
        warn("%s", "store busy: it is mounted by rwy-fuse, unmount it first");
        rc = X_BUSY;
    } else {
        store_commit_active();
        rc = rwyctl_exec_command((int)hdr.argc, cmd_argv);
    }

    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);

    for (i = 0; i < num_fds; i++) close(fds[i]);
    int32_t ret = (int32_t)rc;
    send(conn, &ret, sizeof(ret), MSG_NOSIGNAL);
}
