/* tools/freeze_linux.h: the freeze socket of rwy-fuse, shared by rwy-fuse and rwyctl. */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FREEZE_LINUX_H
#define FREEZE_LINUX_H

#include <stddef.h>
#include <stdint.h>

/* The socket directory: $RWY_RUN_DIR, else /run/rwy-fuse. */
const char *freeze_dir(void);
/* Build "<dir>/<uuid>.sock" for the store UUID. Returns 0, or -1 when the path does not fit a socket address. */
int freeze_path(char *out, size_t n, const char *dir, const uint8_t uuid[16]);

/* Daemon side. Make the directory if missing, replace a stale socket and listen. Returns the fd or -1. */
int freeze_listen(const char *dir, const uint8_t uuid[16]);
/* Accept one connection. Returns its fd, or -1 on an error or when the peer is not the user of this process. */
int freeze_accept(int fd);
/* Close the listener and remove its socket file. */
void freeze_close(int fd);
/* Send one text line on a connection. Returns 0 or -1. */
int freeze_reply(int conn, const char *line);

/* The mount source of rwy-fuse is RWY_FUSE_FSNAME followed by the store UUID. */
#define RWY_FUSE_FSNAME "rwy-fuse:"

/* Client side. Connect to the socket of a store. The caller keeps the fd open for as long as it needs the lease. */
int freeze_connect(const char *dir, const uint8_t uuid[16]);
/* Read one answer line, without its newline, waiting at most timeout_ms. Returns its length or -1. */
int freeze_read_answer(int fd, char *buf, size_t n, int timeout_ms);

/* Find the mount point of the store with this UUID in /proc/self/mountinfo. Returns 0, or -1 when it is not mounted. */
int freeze_find_mount(const uint8_t uuid[16], char *out, size_t n);

#endif
