/*
 * freezetest — the freeze socket: path rules, listen, accept, reply, and close.
 * SPDX-License-Identifier: Apache-2.0
 * Not covered: the refusal of a peer that is another user. It needs a second user, so it cannot run here.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freeze_linux.h"
#include "testdir.h"

static int g_fails;

static void check(int cond, const char *what) {
    if (cond) return;
    fprintf(stderr, "FREEZETEST FAIL: %s\n", what);
    g_fails++;
}

static const uint8_t UUID[16] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                  0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe };
#define UUID_SOCK "01234567-89ab-cdef-1032-547698badcfe.sock"

static void test_path(void) {
    char p[256], want[256];

    unsetenv("RWY_RUN_DIR");
    check(!strcmp(freeze_dir(), "/run/rwy-fuse"), "default directory");
    setenv("RWY_RUN_DIR", "/var/tmp/x", 1);
    check(!strcmp(freeze_dir(), "/var/tmp/x"), "directory from RWY_RUN_DIR");
    setenv("RWY_RUN_DIR", "", 1);
    check(!strcmp(freeze_dir(), "/run/rwy-fuse"), "empty RWY_RUN_DIR is ignored");
    unsetenv("RWY_RUN_DIR");

    check(!freeze_path(p, sizeof(p), "/run/rwy-fuse", UUID), "build a path");
    snprintf(want, sizeof(want), "/run/rwy-fuse/%s", UUID_SOCK);
    check(!strcmp(p, want), "path is dir/uuid.sock");

    char longdir[200];
    memset(longdir, 'a', sizeof(longdir));
    longdir[0] = '/';
    longdir[sizeof(longdir) - 1] = 0;
    check(freeze_path(p, sizeof(p), longdir, UUID) == -1, "a too long path is refused");
    check(freeze_listen(longdir, UUID) == -1, "listen refuses a too long path");
    check(freeze_connect(longdir, UUID) == -1, "connect refuses a too long path");
}

static void test_socket(void) {
    const char *dir = tpath("run/sub");
    char sock[256], line[64];
    struct stat st;
    int l, c1, c2, a1, a2;
    char ch;

    snprintf(sock, sizeof(sock), "%s/%s", dir, UUID_SOCK);
    check(mkdir(tpath("run"), 0700) == 0, "make the parent directory");

    /* A stale socket file is replaced */
    check(mkdir(dir, 0755) == 0, "make the directory");
    FILE *f = fopen(sock, "w");
    check(f != NULL, "make a stale file");
    if (f) fclose(f);

    umask(022);
    l = freeze_listen(dir, UUID);
    check(l >= 0, "listen");
    check(!stat(sock, &st) && S_ISSOCK(st.st_mode), "the stale file became a socket");
    check((st.st_mode & 07777) == 0600, "socket mode is 0600");

    /* A missing directory is created with mode 0755 */
    const char *dir2 = tpath("run/new");
    int l2 = freeze_listen(dir2, UUID);
    check(l2 >= 0, "listen in a new directory");
    check(!stat(dir2, &st) && (st.st_mode & 07777) == 0755, "directory mode is 0755");
    freeze_close(l2);

    c1 = freeze_connect(dir, UUID);
    check(c1 >= 0, "connect");
    a1 = freeze_accept(l);
    check(a1 >= 0, "accept");
    check(a1 >= 0 && !freeze_reply(a1, "frozen"), "reply");
    check(freeze_read_answer(c1, line, sizeof(line), 2000) == 6 && !strcmp(line, "frozen"), "client reads the line");

    /* A second connection is accepted on its own */
    c2 = freeze_connect(dir, UUID);
    check(c2 >= 0, "second connect");
    a2 = freeze_accept(l);
    check(a2 >= 0 && a2 != a1, "second accept");
    check(a2 >= 0 && !freeze_reply(a2, "busy: frozen"), "second reply");
    check(freeze_read_answer(c2, line, sizeof(line), 2000) == 12 && !strcmp(line, "busy: frozen"), "client reads busy");

    /* A line that is too long, and no line at all */
    check(a2 >= 0 && !freeze_reply(a2, "0123456789"), "reply a long line");
    check(freeze_read_answer(c2, line, 5, 2000) == -1, "a line that does not fit is refused");
    check(freeze_read_answer(c1, line, sizeof(line), 100) == -1, "no answer ends after the wait");

    /* A client that goes away is seen as EOF */
    close(c1);
    check(a1 >= 0 && read(a1, &ch, 1) == 0, "daemon sees EOF when the client closes");
    close(a1);
    close(c2);
    close(a2);

    freeze_close(l);
    check(stat(sock, &st) == -1, "close removes the socket file");
    check(freeze_connect(dir, UUID) == -1, "no connect after close");
}

int main(void) {
    test_path();
    test_socket();
    if (g_fails) return 1;
    puts("freezetest: ok");
    return 0;
}
