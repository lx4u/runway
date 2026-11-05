/*
 * fusestoretest — the store session of rwy-fuse: close and reopen it while another program changes the store.
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "anchor_linux.h"
#include "crypto_openssl.h"
#include "hostdev.h"
#include "rwy_core.h"
#include "../../fuse/rwy_fuse_store.h"
#include "testdir.h"

#define MIB (1024 * 1024)

static rwy_store_t g_s;
static int g_fails;

static void check(int cond, const char *what) {
    if (cond) return;
    fprintf(stderr, "FUSESTORETEST FAIL: %s\n", what);
    g_fails++;
}

static int mkstore(const char *spec, int mib, const char *opts) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s --size %d %s '%s' >/dev/null 2>&1", mkstore_path(), mib, opts, spec);
    return system(cmd);
}

static void copy(const char *from, const char *to) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", from, to);
    check(!system(cmd), "copy a store");
}

static int add_file(rwy_store_t *s, const char *path, const char *text) {
    return !rwy_begin(s) && !rwy_create(s, path, RWY_S_IFREG | 0644, 0, 0) &&
           !rwy_write_path(s, path, 0, text, strlen(text)) && !rwy_commit(s);
}

/* Add a file to a store that nobody has open, as another program would. */
static void put_closed(const char *spec, const char *path, const char *text) {
    hostdev_t h;
    rwy_block_dev_t dev;
    int ok = !hostdev_open(&h, spec, O_RDWR);
    if (ok) {
        hostdev_dev(&h, &dev);
        ok = rwy_mount(&g_s, &dev) == RWY_OK && add_file(&g_s, path, text);
        hostdev_close(&h);
    }
    check(ok, "change a closed store");
}

static void put_open(const char *path, const char *text) {
    check(add_file(&g_s, path, text), "change the open store");
}

static int has(const char *path, const char *text) {
    char buf[64] = "";
    size_t n = sizeof(buf);
    return !rwy_read_path(&g_s, path, 0, buf, &n) && n == strlen(text) && !memcmp(buf, text, n);
}

/* 1 when the file can be locked, that is when nobody holds it. */
static int lock_free(const char *path) {
    int fd = open(path, O_RDWR), ok = fd >= 0 && !flock(fd, LOCK_EX | LOCK_NB);
    if (fd >= 0) close(fd);
    return ok;
}

static void reopen_ok(const char *what) {
    check(!rwy_fuse_store_reopen(&g_s), what);
}

static void test_plain(void) {
    const char *f = tpath("plain.store");
    const char *specs[] = { f };
    check(!mkstore(f, 8, ""), "mkstore");
    check(!rwy_fuse_store_open(&g_s, specs, 1, NULL), "open");
    put_open("/a", "one");
    check(!lock_free(f), "locked while open");
    rwy_fuse_store_close(&g_s);
    check(lock_free(f), "lock dropped by close");
    rwy_fuse_store_close(&g_s);
    anchor_release();
    put_closed(f, "/b", "two");
    reopen_ok("reopen");
    check(!lock_free(f), "locked after reopen");
    check(has("/a", "one") && has("/b", "two"), "reopen sees the change");
    for (int i = 0; i < 3; i++) {
        rwy_fuse_store_close(&g_s);
        check(lock_free(f), "lock dropped again");
        reopen_ok("reopen again");
    }
    check(has("/a", "one") && has("/b", "two"), "cycles keep the content");
    rwy_fuse_store_close(&g_s);
}

static void test_relative(void) {
    char dir[512];
    const char *f = tpath("rel.store");
    const char *specs[] = { "rel.store" };
    snprintf(dir, sizeof(dir), "%s", f);
    check(!mkstore(f, 8, "") && !chdir(dirname(dir)), "relative: setup");
    check(!rwy_fuse_store_open(&g_s, specs, 1, NULL), "relative: open");
    check(!chdir("/"), "chdir /");
    rwy_fuse_store_close(&g_s);
    reopen_ok("relative: reopen after chdir");
    rwy_fuse_store_close(&g_s);
}

static void test_window(void) {
    const char *img = tpath("win.img");
    char spec[600];
    const char *specs[] = { spec };
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0 && !ftruncate(fd, 40 * MIB), "window: image");
    if (fd >= 0) close(fd);
    snprintf(spec, sizeof(spec), "%s@%d+%d", img, MIB, 32 * MIB);
    check(!mkstore(spec, 32, ""), "window: mkstore");
    check(!rwy_fuse_store_open(&g_s, specs, 1, NULL), "window: open");
    rwy_fuse_store_close(&g_s);
    check(lock_free(img), "window: lock dropped");
    put_closed(spec, "/w", "win");
    reopen_ok("window: reopen");
    check(has("/w", "win"), "window: sees the change");
    rwy_fuse_store_close(&g_s);
}

static void test_encrypted(void) {
    const char *f = tpath("enc.store");
    const char *specs[] = { f };
    check(!mkstore(f, 8, "--encrypt --passphrase P"), "encrypted: mkstore");
    check(!rwy_fuse_store_open(&g_s, specs, 1, "P"), "encrypted: open");
    put_open("/e", "secret");
    rwy_fuse_store_close(&g_s);
    reopen_ok("encrypted: reopen without the key");
    check(has("/e", "secret"), "encrypted: content");
    put_open("/e2", "more");
    rwy_fuse_store_close(&g_s);
    reopen_ok("encrypted: reopen again");
    check(has("/e2", "more"), "encrypted: written after reopen");
    rwy_fuse_store_close(&g_s);
}

/* How many lagging events the state record counts over all members. */
static unsigned lagging_total(void) {
    uint8_t r[RWY_REC_PAYLOAD], uuid[16];
    rwy_ss_health_t h;
    unsigned total = 0;
    if (rwy_record_read(&g_s, r)) return ~0u;
    for (unsigned i = 0; i < RWY_MIRROR_MAX_DEVS; i++)
        if (rwy_ss_health_at(r, i, uuid, &h)) total += h.count[RWY_SS_EV_LAGGING];
    return total;
}

static void test_mirror(void) {
    const char *a = tpath("m-a.store"), *b = tpath("m-b.store");
    const char *specs[] = { a, b };
    check(!mkstore(a, 8, ""), "mirror: mkstore");
    copy(a, b);
    put_closed(a, "/m0", "zero");
    check(!rwy_fuse_store_open(&g_s, specs, 2, NULL), "mirror: open");
    check(lagging_total() == 1, "mirror: the first open counts the lagging member");
    put_open("/m1", "one");
    check(!lock_free(a) && !lock_free(b), "mirror: both locked");
    rwy_fuse_store_close(&g_s);
    check(lock_free(a) && lock_free(b), "mirror: both released");
    put_closed(a, "/m2", "two");
    reopen_ok("mirror: reopen");
    check(has("/m1", "one") && has("/m2", "two"), "mirror: newest copy is mounted");
    check(!lock_free(a) && !lock_free(b), "mirror: both locked again");
    check(lagging_total() == 1, "mirror: a reopen does not count the lagging member again");
    rwy_fuse_store_close(&g_s);
}

int main(void) {
    rwy_crypto_host_init();
    test_plain();
    test_relative();
    test_window();
    test_encrypted();
    test_mirror();
    if (!g_fails) printf("fusestoretest: ok\n");
    return g_fails ? 1 : 0;
}
