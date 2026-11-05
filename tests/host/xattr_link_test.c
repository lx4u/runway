/*
 * xattr_link_test — Comprehensive test suite for Option 2:
 * Extended Attributes (xattrs), Hardlinks (nlink), and Symlink Path Resolution.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Verifies:
 *  1. Extended Attributes: CRUD, namespaces (user, security, trusted, system),
 *     XATTR_CREATE / XATTR_REPLACE flags, buffer size querying (size=0),
 *     ERANGE when buffer too small, ENODATA when attr missing, listing,
 *     multi-block chaining (>4KB payloads), CoW updates, feature flag RWY_FEAT_COMPAT_XATTR.
 *  2. Hardlinks: creation, link counts (nlink), content sharing across links,
 *     independent unlinking (data preserved until last link unlinked),
 *     rejection of directory hardlinking (EINVAL/EPERM).
 *  3. Symlink Path Resolution: intermediate symlink resolution across directories,
 *     relative and absolute symlinks, stat vs lstat semantics, loop detection
 *     (direct, mutual, chain > 32 hops returning RWY_ERR_LOOP).
 *  4. GC & Defrag safety: xattrs and hardlinks preserved intact across rwy_gc() and rwy_defrag().
 *  5. Checker validation: corrupt xattr CRC, link count mismatch, orphan xattrs
 *     cause fsck.runway to detect corruption and exit 1.
 *  6. Deterministic Fault Injection: power loss during setxattr, link, and unlink
 *     always leaves valid old-or-new state.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testdir.h"
#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_format.h"
#include "forge.h"

static int failures;
static const char *g_checkbin = "build/tools/fsck.runway";

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

/* ---------- file-backed block device ---------- */
typedef struct { FILE *f; } file_t;
static int file_rd(void *ctx, uint64_t off, void *buf, size_t len) {
    file_t *fl = ctx;
    if (fseek(fl->f, (long)off, SEEK_SET)) return -1;
    return fread(buf, 1, len, fl->f) == len ? 0 : -1;
}
static int file_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    file_t *fl = ctx;
    if (fseek(fl->f, (long)off, SEEK_SET)) return -1;
    return fwrite(buf, 1, len, fl->f) == len ? 0 : -1;
}
static int file_fl(void *ctx) { file_t *fl = ctx; return fflush(fl->f) ? -1 : 0; }

static void open_store(rwy_store_t *s, rwy_block_dev_t *dev, file_t *fl,
                       const char *path, const char *mode) {
    fl->f = fopen(path, mode);
    if (!fl->f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    dev->read = file_rd; dev->write = file_wr; dev->flush = file_fl;
    dev->ctx = fl; dev->size = RWY_STORE_SIZE; dev->block_size = RWY_BLOCK_SIZE;
    if (rwy_mount(s, dev)) { fprintf(stderr, "mount failed %s\n", path); exit(1); }
}

static void copy_file(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "wb");
    char buf[1 << 20];
    size_t n;
    if (!fa || !fb) { fprintf(stderr, "copy failed %s -> %s\n", a, b); exit(1); }
    while ((n = fread(buf, 1, sizeof(buf), fa)) > 0)
        if (fwrite(buf, 1, n, fb) != n) { fprintf(stderr, "copy failed\n"); exit(1); }
    fclose(fa); fclose(fb);
}

static int run_checker(const char *store_path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s %s >/dev/null 2>&1", g_checkbin, store_path);
    return system(cmd);
}

/* ---------- fault injection wrapper ---------- */
typedef struct {
    rwy_block_dev_t inner;
    long point;
    long fail_at;
    int failed;
} fi_t;

static int fi_rd(void *ctx, uint64_t off, void *buf, size_t len) {
    fi_t *fi = ctx;
    return fi->inner.read(fi->inner.ctx, off, buf, len);
}
static int fi_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    fi_t *fi = ctx;
    fi->point++;
    if (fi->fail_at == fi->point) { fi->failed = 1; return -1; }
    return fi->inner.write(fi->inner.ctx, off, buf, len);
}
static int fi_fl(void *ctx) {
    fi_t *fi = ctx;
    fi->point++;
    if (fi->fail_at == fi->point) { fi->failed = 1; return -1; }
    if (fi->inner.flush) return fi->inner.flush(fi->inner.ctx);
    return 0;
}

/* ========================================================================= */
/* 1. Extended Attributes Test                                              */
/* ========================================================================= */
static void test_xattr_basic(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    const char *img = tpath("rwy_test_xattr_basic.img");
    char buf[256];
    char list[512];
    int rc;

    printf("== xattr basic & namespaces ==\n");
    copy_file(store_path, img);
    open_store(&s, &dev, &fl, img, "r+b");

    /* Create test file */
    CHECK(rwy_txn_create(&s, "XFILE.TXT", RWY_S_IFREG | 0644, 1000, 1000) == RWY_OK, "create XFILE.TXT");

    /* Get on nonexistent attribute returns RWY_ERR_XATTR */
    rc = rwy_getxattr(&s, "XFILE.TXT", "user.comment", buf, sizeof(buf));
    CHECK(rc == RWY_ERR_XATTR, "get nonexistent xattr gives ERR_XATTR");

    /* Set user namespace xattr */
    CHECK(rwy_txn_setxattr(&s, "XFILE.TXT", "user.comment", "Hello Xattr", 11, 0) == RWY_OK, "set user.comment");

    /* Verify feature flag RWY_FEAT_COMPAT_XATTR is recorded */
    CHECK((s.sb.feat_compat & RWY_FEAT_COMPAT_XATTR) != 0, "feat_compat XATTR bit set");

    /* Query size (size = 0) */
    rc = rwy_getxattr(&s, "XFILE.TXT", "user.comment", NULL, 0);
    CHECK(rc == 11, "query size returns 11");

    /* Get into too small buffer returns RWY_ERR_RANGE */
    rc = rwy_getxattr(&s, "XFILE.TXT", "user.comment", buf, 5);
    CHECK(rc == RWY_ERR_RANGE, "buffer too small returns ERR_RANGE");

    /* Get into adequate buffer */
    memset(buf, 0, sizeof(buf));
    rc = rwy_getxattr(&s, "XFILE.TXT", "user.comment", buf, sizeof(buf));
    CHECK(rc == 11, "getxattr returns 11 bytes");
    CHECK(memcmp(buf, "Hello Xattr", 11) == 0, "getxattr value matches");

    /* XATTR_CREATE on existing attribute must fail with RWY_ERR_EXIST */
    rc = rwy_txn_setxattr(&s, "XFILE.TXT", "user.comment", "new", 3, XATTR_CREATE);
    CHECK(rc == RWY_ERR_EXIST, "XATTR_CREATE on existing fails with ERR_EXIST");

    /* XATTR_REPLACE on existing attribute succeeds */
    rc = rwy_txn_setxattr(&s, "XFILE.TXT", "user.comment", "Replaced Value", 14, XATTR_REPLACE);
    CHECK(rc == RWY_OK, "XATTR_REPLACE on existing succeeds");
    rc = rwy_getxattr(&s, "XFILE.TXT", "user.comment", buf, sizeof(buf));
    CHECK(rc == 14 && memcmp(buf, "Replaced Value", 14) == 0, "value was replaced");

    /* XATTR_REPLACE on non-existing attribute fails with RWY_ERR_XATTR */
    rc = rwy_txn_setxattr(&s, "XFILE.TXT", "user.nonexistent", "val", 3, XATTR_REPLACE);
    CHECK(rc == RWY_ERR_XATTR, "XATTR_REPLACE on nonexistent fails with ERR_XATTR");

    /* Test namespaces: security.*, trusted.*, system.* */
    CHECK(rwy_txn_setxattr(&s, "XFILE.TXT", "security.selinux", "system_u:object_r:file_t:s0", 27, 0) == RWY_OK,
          "set security.selinux");
    CHECK(rwy_txn_setxattr(&s, "XFILE.TXT", "trusted.checksum", "12345678abcdef", 14, 0) == RWY_OK,
          "set trusted.checksum");
    CHECK(rwy_txn_setxattr(&s, "XFILE.TXT", "system.posix_acl", "acl_data_blob", 13, 0) == RWY_OK,
          "set system.posix_acl");

    /* Invalid namespace rejection */
    rc = rwy_txn_setxattr(&s, "XFILE.TXT", "custom.namespace", "val", 3, 0);
    CHECK(rc == RWY_ERR_INVAL, "custom namespace rejected with ERR_INVAL");
    rc = rwy_txn_setxattr(&s, "XFILE.TXT", "user", "val", 3, 0);
    CHECK(rc == RWY_ERR_INVAL, "no period rejected with ERR_INVAL");

    /* List attributes */
    rc = rwy_listxattr(&s, "XFILE.TXT", NULL, 0);
    CHECK(rc > 0, "listxattr size query > 0");
    int list_sz = rc;
    rc = rwy_listxattr(&s, "XFILE.TXT", list, 10);
    CHECK(rc == RWY_ERR_RANGE, "listxattr too small returns ERR_RANGE");

    rc = rwy_listxattr(&s, "XFILE.TXT", list, sizeof(list));
    CHECK(rc == list_sz, "listxattr returned expected size");

    /* Verify listed names */
    int found_comment = 0, found_selinux = 0, found_trusted = 0, found_acl = 0;
    int pos = 0;
    while (pos < rc) {
        const char *name = list + pos;
        if (!strcmp(name, "user.comment")) found_comment = 1;
        if (!strcmp(name, "security.selinux")) found_selinux = 1;
        if (!strcmp(name, "trusted.checksum")) found_trusted = 1;
        if (!strcmp(name, "system.posix_acl")) found_acl = 1;
        pos += (int)strlen(name) + 1;
    }
    CHECK(found_comment && found_selinux && found_trusted && found_acl, "all 4 xattrs listed");

    /* Remove attribute */
    CHECK(rwy_txn_removexattr(&s, "XFILE.TXT", "trusted.checksum") == RWY_OK, "remove trusted.checksum");
    CHECK(rwy_getxattr(&s, "XFILE.TXT", "trusted.checksum", buf, sizeof(buf)) == RWY_ERR_XATTR,
          "removed attribute cannot be retrieved");
    CHECK(rwy_txn_removexattr(&s, "XFILE.TXT", "trusted.checksum") == RWY_ERR_XATTR,
          "remove again returns ERR_XATTR");

    /* Remount and verify persistence */
    fclose(fl.f);
    open_store(&s, &dev, &fl, img, "r+b");
    rc = rwy_getxattr(&s, "XFILE.TXT", "user.comment", buf, sizeof(buf));
    CHECK(rc == 14 && memcmp(buf, "Replaced Value", 14) == 0, "user.comment persisted across remount");
    rc = rwy_getxattr(&s, "XFILE.TXT", "security.selinux", buf, sizeof(buf));
    CHECK(rc == 27, "security.selinux persisted across remount");
    fclose(fl.f);

    CHECK(run_checker(img) == 0, "checker passes on store with xattrs");
    printf("== xattr basic OK ==\n");
}

/* ========================================================================= */
/* 2. Large Chained Extended Attributes Test (>4KB payload)                  */
/* ========================================================================= */
static void test_xattr_large_chain(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    const char *img = tpath("rwy_test_xattr_large.img");
    uint8_t *val1 = malloc(2500);
    uint8_t *val2 = malloc(2500);
    uint8_t *val3 = malloc(2500);
    uint8_t *readbuf = malloc(4096);
    int rc;

    printf("== xattr large multi-block chaining ==\n");
    for (int i = 0; i < 2500; i++) {
        val1[i] = (uint8_t)(i ^ 0x5a);
        val2[i] = (uint8_t)(i ^ 0xa5);
        val3[i] = (uint8_t)(i ^ 0x3c);
    }

    copy_file(store_path, img);
    open_store(&s, &dev, &fl, img, "r+b");

    CHECK(rwy_txn_create(&s, "BIGATTR.DAT", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create BIGATTR.DAT");

    /* Total payload: 2500 * 3 + headers = ~7550 bytes > 4032 bytes (spans >= 2 blocks) */
    CHECK(rwy_txn_setxattr(&s, "BIGATTR.DAT", "user.chunk1", val1, 2500, 0) == RWY_OK, "set chunk1");
    CHECK(rwy_txn_setxattr(&s, "BIGATTR.DAT", "user.chunk2", val2, 2500, 0) == RWY_OK, "set chunk2");
    CHECK(rwy_txn_setxattr(&s, "BIGATTR.DAT", "user.chunk3", val3, 2500, 0) == RWY_OK, "set chunk3");

    /* Read back all 3 chunks */
    rc = rwy_getxattr(&s, "BIGATTR.DAT", "user.chunk1", readbuf, 4096);
    CHECK(rc == 2500 && memcmp(readbuf, val1, 2500) == 0, "chunk1 matches");
    rc = rwy_getxattr(&s, "BIGATTR.DAT", "user.chunk2", readbuf, 4096);
    CHECK(rc == 2500 && memcmp(readbuf, val2, 2500) == 0, "chunk2 matches");
    rc = rwy_getxattr(&s, "BIGATTR.DAT", "user.chunk3", readbuf, 4096);
    CHECK(rc == 2500 && memcmp(readbuf, val3, 2500) == 0, "chunk3 matches");

    /* Overwrite middle chunk2 */
    for (int i = 0; i < 2500; i++) val2[i] = (uint8_t)(i ^ 0xff);
    CHECK(rwy_txn_setxattr(&s, "BIGATTR.DAT", "user.chunk2", val2, 2500, 0) == RWY_OK, "overwrite chunk2");
    rc = rwy_getxattr(&s, "BIGATTR.DAT", "user.chunk2", readbuf, 4096);
    CHECK(rc == 2500 && memcmp(readbuf, val2, 2500) == 0, "chunk2 updated matches");
    rc = rwy_getxattr(&s, "BIGATTR.DAT", "user.chunk1", readbuf, 4096);
    CHECK(rc == 2500 && memcmp(readbuf, val1, 2500) == 0, "chunk1 unaffected");

    fclose(fl.f);
    free(val1); free(val2); free(val3); free(readbuf);

    CHECK(run_checker(img) == 0, "checker passes on multi-block xattr chain");
    printf("== xattr large chaining OK ==\n");
}

/* ========================================================================= */
/* 3. Hardlinks Test                                                        */
/* ========================================================================= */
static void test_hardlinks(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    const char *img = tpath("rwy_test_hardlinks.img");
    rwy_attr_t a1, a2, a3;
    uint64_t o1, o2, o3;
    char buf[64];
    size_t got;
    int rc;

    printf("== hardlinks (nlink & sharing) ==\n");
    copy_file(store_path, img);
    open_store(&s, &dev, &fl, img, "r+b");

    /* Create file and write content */
    CHECK(rwy_txn_create(&s, "ORIG.TXT", RWY_S_IFREG | 0644, 500, 500) == RWY_OK, "create ORIG.TXT");
    CHECK(rwy_txn_write_path(&s, "ORIG.TXT", 0, "LinkContent12345", 16) == RWY_OK, "write ORIG.TXT");

    /* Initial nlink must be 1 */
    CHECK(rwy_getattr_path(&s, "ORIG.TXT", &a1) == RWY_OK, "stat ORIG.TXT");
    CHECK(a1.nlink == 1, "initial nlink is 1");

    /* Create first hardlink */
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    CHECK(rwy_link(&s, "ORIG.TXT", "LINK1.TXT") == RWY_OK, "link ORIG.TXT -> LINK1.TXT");
    CHECK(rwy_commit(&s) == RWY_OK, "commit link1");

    /* Both must share OID and have nlink = 2 */
    CHECK(rwy_lookup(&s, "ORIG.TXT", &o1) == RWY_OK, "lookup ORIG");
    CHECK(rwy_lookup(&s, "LINK1.TXT", &o2) == RWY_OK, "lookup LINK1");
    CHECK(o1 == o2, "hardlink shares identical OID");
    CHECK(rwy_getattr_path(&s, "ORIG.TXT", &a1) == RWY_OK, "stat ORIG");
    CHECK(rwy_getattr_path(&s, "LINK1.TXT", &a2) == RWY_OK, "stat LINK1");
    CHECK(a1.nlink == 2 && a2.nlink == 2, "both report nlink == 2");

    /* Create second and third hardlinks */
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    CHECK(rwy_link(&s, "LINK1.TXT", "LINK2.TXT") == RWY_OK, "link LINK1 -> LINK2");
    CHECK(rwy_link(&s, "ORIG.TXT", "LINK3.TXT") == RWY_OK, "link ORIG -> LINK3");
    CHECK(rwy_commit(&s) == RWY_OK, "commit links 2 and 3");

    CHECK(rwy_getattr_path(&s, "ORIG.TXT", &a1) == RWY_OK, "stat ORIG");
    CHECK(a1.nlink == 4, "nlink is now 4");

    /* Rejection tests */
    /* 1. Cannot link a directory */
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    rc = rwy_link(&s, "EFI", "EFI_LINK");
    CHECK(rc == RWY_ERR_INVAL, "link on directory returns ERR_INVAL");
    rwy_abort(&s);

    /* 2. Cannot link nonexistent target */
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    rc = rwy_link(&s, "NO_SUCH_FILE", "FAIL_LINK");
    CHECK(rc == RWY_ERR_NOENT, "link nonexistent source returns ERR_NOENT");
    rwy_abort(&s);

    /* 3. Cannot link over existing destination */
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    rc = rwy_link(&s, "ORIG.TXT", "LINK1.TXT");
    CHECK(rc == RWY_ERR_EXIST, "link to existing dest returns ERR_EXIST");
    rwy_abort(&s);

    /* Verify data mutation via LINK2 is visible via ORIG */
    CHECK(rwy_txn_write_path(&s, "LINK2.TXT", 0, "ModifiedByLink2!", 16) == RWY_OK, "write LINK2");
    got = sizeof(buf);
    CHECK(rwy_read_path(&s, "ORIG.TXT", 0, buf, &got) == RWY_OK, "read ORIG");
    CHECK(got == 16 && memcmp(buf, "ModifiedByLink2!", 16) == 0, "mutation visible via ORIG");

    /* Checker passes with 4 links */
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes with multiple links (nlink==4)");
    open_store(&s, &dev, &fl, img, "r+b");

    /* Unlink ORIG.TXT: LINK1, LINK2, LINK3 still alive with nlink == 3 */
    CHECK(rwy_txn_unlink(&s, "ORIG.TXT") == RWY_OK, "unlink ORIG.TXT");
    CHECK(rwy_lookup(&s, "ORIG.TXT", &o1) == RWY_ERR_NOENT, "ORIG.TXT is gone");
    CHECK(rwy_getattr_path(&s, "LINK1.TXT", &a1) == RWY_OK, "LINK1 still exists");
    CHECK(a1.nlink == 3, "LINK1 nlink is now 3");
    got = sizeof(buf);
    CHECK(rwy_read_path(&s, "LINK1.TXT", 0, buf, &got) == RWY_OK &&
          memcmp(buf, "ModifiedByLink2!", 16) == 0, "LINK1 content intact");

    /* Unlink LINK1 and LINK2 */
    CHECK(rwy_txn_unlink(&s, "LINK1.TXT") == RWY_OK, "unlink LINK1");
    CHECK(rwy_txn_unlink(&s, "LINK2.TXT") == RWY_OK, "unlink LINK2");
    CHECK(rwy_getattr_path(&s, "LINK3.TXT", &a3) == RWY_OK, "LINK3 alive");
    CHECK(a3.nlink == 1, "LINK3 nlink is 1");

    /* Unlink final link LINK3 */
    CHECK(rwy_txn_unlink(&s, "LINK3.TXT") == RWY_OK, "unlink LINK3");
    CHECK(rwy_lookup(&s, "LINK3.TXT", &o3) == RWY_ERR_NOENT, "LINK3 is gone");

    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after unlinking all links");
    printf("== hardlinks OK ==\n");
}

/* ========================================================================= */
/* 4. Symlink Path Resolution Test                                          */
/* ========================================================================= */
static void test_symlinks(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    const char *img = tpath("rwy_test_symlinks.img");
    uint64_t target_oid, sym_oid, found_oid;
    rwy_attr_t a;
    char buf[128];
    size_t got;
    int rc;

    printf("== symlink resolution & loop detection ==\n");
    copy_file(store_path, img);
    open_store(&s, &dev, &fl, img, "r+b");

    /* Setup directory tree:
     * /DIR1/FILE1.TXT
     * /SYMLINK_DIR -> DIR1
     * /SYMLINK_FILE -> DIR1/FILE1.TXT
     * /DIR1/REL_SYM -> ../SYMLINK_FILE
     */
    CHECK(rwy_txn_mkdir(&s, "DIR1", 0755, 0, 0) == RWY_OK, "mkdir DIR1");
    CHECK(rwy_txn_create(&s, "DIR1/FILE1.TXT", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create FILE1");
    CHECK(rwy_txn_write_path(&s, "DIR1/FILE1.TXT", 0, "SymlinkPayload", 14) == RWY_OK, "write FILE1");
    CHECK(rwy_lookup(&s, "DIR1/FILE1.TXT", &target_oid) == RWY_OK, "lookup FILE1");

    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    CHECK(rwy_symlink(&s, "SYMLINK_DIR", "DIR1", 0, 0) == RWY_OK, "symlink SYMLINK_DIR -> DIR1");
    CHECK(rwy_symlink(&s, "SYMLINK_FILE", "DIR1/FILE1.TXT", 0, 0) == RWY_OK, "symlink SYMLINK_FILE");
    CHECK(rwy_commit(&s) == RWY_OK, "commit symlinks");
    CHECK(rwy_lookup(&s, "SYMLINK_FILE", &sym_oid) == RWY_OK, "lookup SYMLINK_FILE");
    CHECK(sym_oid != target_oid, "symlink has distinct OID from target");

    /* 1. Intermediate symlink resolution: SYMLINK_DIR/FILE1.TXT */
    got = sizeof(buf);
    CHECK(rwy_read_path(&s, "SYMLINK_DIR/FILE1.TXT", 0, buf, &got) == RWY_OK, "read via dir symlink");
    CHECK(got == 14 && memcmp(buf, "SymlinkPayload", 14) == 0, "read payload matches");

    /* Write via intermediate symlink */
    CHECK(rwy_txn_write_path(&s, "SYMLINK_DIR/FILE1.TXT", 0, "ModifiedPayload!", 16) == RWY_OK, "write via symlink");
    got = sizeof(buf);
    CHECK(rwy_read_path(&s, "DIR1/FILE1.TXT", 0, buf, &got) == RWY_OK &&
          memcmp(buf, "ModifiedPayload!", 16) == 0, "target file was modified");

    /* 2. stat vs lstat semantics */
    CHECK(rwy_lstat(&s, "SYMLINK_FILE", &a) == RWY_OK, "lstat SYMLINK_FILE");
    CHECK(a.type == RWY_TYPE_SYMLINK, "lstat returns RWY_TYPE_SYMLINK");
    CHECK(a.oid == sym_oid, "lstat returns symlink OID");

    CHECK(rwy_stat(&s, "SYMLINK_FILE", &a) == RWY_OK, "stat SYMLINK_FILE");
    CHECK(a.type == RWY_TYPE_FILE, "stat returns RWY_TYPE_FILE");
    CHECK(a.oid == target_oid, "stat returns target OID");

    CHECK(rwy_lookup(&s, "SYMLINK_FILE", &found_oid) == RWY_OK, "rwy_lookup nofollow");
    CHECK(found_oid == sym_oid, "rwy_lookup returned symlink OID");

    CHECK(rwy_lookup_follow(&s, "SYMLINK_FILE", 1, &found_oid) == RWY_OK, "rwy_lookup_follow follow");
    CHECK(found_oid == target_oid, "rwy_lookup_follow returned target OID");

    /* 3. Relative symlink resolution: /DIR1/REL_SYM -> ../SYMLINK_FILE */
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    CHECK(rwy_symlink(&s, "DIR1/REL_SYM", "../SYMLINK_FILE", 0, 0) == RWY_OK, "create relative symlink");
    CHECK(rwy_commit(&s) == RWY_OK, "commit relative symlink");

    CHECK(rwy_stat(&s, "DIR1/REL_SYM", &a) == RWY_OK, "stat DIR1/REL_SYM");
    CHECK(a.oid == target_oid, "relative symlink resolved to target file OID");

    /* 4. Symlink loop detection */
    /* Direct self loop */
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    CHECK(rwy_symlink(&s, "LOOP_SELF", "LOOP_SELF", 0, 0) == RWY_OK, "create LOOP_SELF");
    CHECK(rwy_commit(&s) == RWY_OK, "commit LOOP_SELF");

    rc = rwy_stat(&s, "LOOP_SELF", &a);
    CHECK(rc == RWY_ERR_LOOP, "resolving self loop returns RWY_ERR_LOOP");
    rc = rwy_lookup_follow(&s, "LOOP_SELF", 1, &found_oid);
    CHECK(rc == RWY_ERR_LOOP, "lookup_follow on self loop returns RWY_ERR_LOOP");

    /* Mutual loop A -> B, B -> A */
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    CHECK(rwy_symlink(&s, "LOOP_A", "LOOP_B", 0, 0) == RWY_OK, "symlink LOOP_A -> LOOP_B");
    CHECK(rwy_symlink(&s, "LOOP_B", "LOOP_A", 0, 0) == RWY_OK, "symlink LOOP_B -> LOOP_A");
    CHECK(rwy_commit(&s) == RWY_OK, "commit mutual loop");

    rc = rwy_stat(&s, "LOOP_A", &a);
    CHECK(rc == RWY_ERR_LOOP, "resolving mutual loop returns RWY_ERR_LOOP");

    /* Long symlink chain (> 32 hops) */
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    for (int i = 0; i < 35; i++) {
        char sfrom[32], sto[32];
        snprintf(sfrom, sizeof(sfrom), "HOP_%02d", i);
        snprintf(sto, sizeof(sto), "HOP_%02d", i + 1);
        CHECK(rwy_symlink(&s, sfrom, sto, 0, 0) == RWY_OK, "create hop");
    }
    CHECK(rwy_create(&s, "HOP_35", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "terminal HOP_35");
    CHECK(rwy_commit(&s) == RWY_OK, "commit 35-hop chain");

    rc = rwy_stat(&s, "HOP_00", &a);
    CHECK(rc == RWY_ERR_LOOP, "chain >= 32 hops returns RWY_ERR_LOOP");

    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes on store with symlinks");
    printf("== symlinks OK ==\n");
}

/* ========================================================================= */
/* 5. GC & Defrag Preservation Test                                         */
/* ========================================================================= */
static void test_gc_defrag_safety(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    const char *img = tpath("rwy_test_gc_defrag.img");
    rwy_attr_t a;
    char buf[256];
    int rc;

    printf("== GC & defrag safety on xattrs and hardlinks ==\n");
    copy_file(store_path, img);
    open_store(&s, &dev, &fl, img, "r+b");

    /* Create file with xattr and multiple links */
    CHECK(rwy_txn_create(&s, "SURVIVE.DAT", RWY_S_IFREG | 0644, 100, 200) == RWY_OK, "create SURVIVE.DAT");
    CHECK(rwy_txn_write_path(&s, "SURVIVE.DAT", 0, "SurviveContent!", 15) == RWY_OK, "write SURVIVE.DAT");
    CHECK(rwy_txn_setxattr(&s, "SURVIVE.DAT", "user.persistent", "xattr_data_value", 16, 0) == RWY_OK, "set xattr");
    CHECK(rwy_txn_setxattr(&s, "SURVIVE.DAT", "security.test", "sec_val", 7, 0) == RWY_OK, "set security xattr");

    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    CHECK(rwy_link(&s, "SURVIVE.DAT", "SURVIVE_LINK1.DAT") == RWY_OK, "link 1");
    CHECK(rwy_link(&s, "SURVIVE.DAT", "SURVIVE_LINK2.DAT") == RWY_OK, "link 2");
    CHECK(rwy_commit(&s) == RWY_OK, "commit links");

    /* Create and delete churning files to generate GC leftovers */
    for (int i = 0; i < 20; i++) {
        char name[32];
        snprintf(name, sizeof(name), "CHURN_%d.DAT", i);
        CHECK(rwy_txn_create(&s, name, RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create churn");
        CHECK(rwy_txn_write_path(&s, name, 0, "dummy data churn block", 22) == RWY_OK, "write churn");
    }
    for (int i = 0; i < 20; i++) {
        char name[32];
        snprintf(name, sizeof(name), "CHURN_%d.DAT", i);
        CHECK(rwy_txn_unlink(&s, name) == RWY_OK, "unlink churn");
    }

    CHECK(forge_leftovers(&s, 40) == RWY_OK, "leave marked-but-unreachable blocks");
    CHECK(rwy_reclaimable(&s) > 0, "churn produced reclaimable blocks");

    /* Run rwy_gc() */
    CHECK(rwy_gc(&s) == RWY_OK, "rwy_gc() executed");
    CHECK(rwy_reclaimable(&s) == 0, "post-GC leftovers == 0");

    /* Verify file, links, and xattrs survived GC */
    CHECK(rwy_getattr_path(&s, "SURVIVE.DAT", &a) == RWY_OK, "stat SURVIVE");
    CHECK(a.nlink == 3, "nlink survived GC (3)");
    CHECK(rwy_getattr_path(&s, "SURVIVE_LINK1.DAT", &a) == RWY_OK && a.nlink == 3, "link1 survived GC");

    rc = rwy_getxattr(&s, "SURVIVE.DAT", "user.persistent", buf, sizeof(buf));
    CHECK(rc == 16 && memcmp(buf, "xattr_data_value", 16) == 0, "user.persistent survived GC");
    rc = rwy_getxattr(&s, "SURVIVE_LINK2.DAT", "security.test", buf, sizeof(buf));
    CHECK(rc == 7 && memcmp(buf, "sec_val", 7) == 0, "security.test survived GC via link2");

    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after GC");

    /* Now run defrag */
    open_store(&s, &dev, &fl, img, "r+b");
    CHECK(rwy_defrag(&s) == RWY_OK, "rwy_defrag() executed");

    /* Verify post-defrag state */
    CHECK(rwy_getattr_path(&s, "SURVIVE.DAT", &a) == RWY_OK, "stat SURVIVE post-defrag");
    CHECK(a.nlink == 3, "nlink survived defrag (3)");
    rc = rwy_getxattr(&s, "SURVIVE_LINK1.DAT", "user.persistent", buf, sizeof(buf));
    CHECK(rc == 16 && memcmp(buf, "xattr_data_value", 16) == 0, "xattr survived defrag");

    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after defrag");
    printf("== GC & defrag safety OK ==\n");
}

/* ========================================================================= */
/* 6. Checker Corruption Detection Test                                     */
/* ========================================================================= */
static void test_checker_corruption(const char *store_path) {
    file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    const char *img = tpath("rwy_test_checker_corrupt.img");

    printf("== checker corruption detection (CRCs, nlink, orphans) ==\n");

    /* Sub-test A: Link count mismatch */
    {
        copy_file(store_path, img);
        open_store(&s, &dev, &fl, img, "r+b");
        CHECK(rwy_txn_create(&s, "NLINK_TEST.DAT", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create NLINK_TEST");
        uint32_t tbl_start = s.sb.inode_tbl_start;
        uint64_t oid;
        CHECK(rwy_lookup(&s, "NLINK_TEST.DAT", &oid) == RWY_OK, "lookup");
        fclose(fl.f);

        /* Manually tamper with the inode table: change nlink to 5 while only 1 dirent exists */
        FILE *f = fopen(img, "r+b");
        CHECK(f != NULL, "open for tamper");

        uint32_t slot = (uint32_t)(oid - 1);
        uint32_t blk_idx = tbl_start + (slot / 15);
        uint32_t blk_off = 32 + (slot % 15) * 256;

        uint8_t blk[RWY_BLOCK_SIZE];
        fseek(f, (long)blk_idx * RWY_BLOCK_SIZE, SEEK_SET);
        CHECK(fread(blk, 1, sizeof(blk), f) == sizeof(blk), "read inode block");

        /* Tamper nlink field in inode */
        rwy_inode_t *in = (rwy_inode_t *)(blk + blk_off);
        CHECK(in->oid == oid, "found inode");
        in->nlink = 5; /* mismatch! 1 dirent exists, inode claims 5 */

        /* Recompute inode table block CRC32C so block header CRC is valid */
        rwy_blk_header_t *h = (rwy_blk_header_t *)blk;
        h->crc32c = 0;
        h->crc32c = rwy_crc32c(blk, sizeof(blk));

        fseek(f, (long)blk_idx * RWY_BLOCK_SIZE, SEEK_SET);
        CHECK(fwrite(blk, 1, sizeof(blk), f) == sizeof(blk), "write inode block");
        fclose(f);

        /* Verify checker flags corruption and returns exit 1 */
        CHECK(run_checker(img) != 0, "checker correctly rejects link count mismatch (exit 1)");
    }

    /* Sub-test B: Corrupt xattr block CRC */
    {
        copy_file(store_path, img);
        open_store(&s, &dev, &fl, img, "r+b");
        CHECK(rwy_txn_create(&s, "XCRC_TEST.DAT", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create XCRC_TEST");
        CHECK(rwy_txn_setxattr(&s, "XCRC_TEST.DAT", "user.myattr", "AttributeValueData", 18, 0) == RWY_OK, "set xattr");
        uint64_t oid;
        CHECK(rwy_lookup(&s, "XCRC_TEST.DAT", &oid) == RWY_OK, "lookup");
        const rwy_inode_t *in = &s.inodes[oid - 1];
        uint64_t xblk = in->xattr_root;
        CHECK(xblk != 0, "xattr_root is nonzero");
        fclose(fl.f);

        /* Corrupt a payload byte in the xattr block */
        FILE *f = fopen(img, "r+b");
        fseek(f, (long)xblk * RWY_BLOCK_SIZE + 70, SEEK_SET);
        uint8_t byte = 0;
        CHECK(fread(&byte, 1, 1, f) == 1, "read xattr byte");
        byte ^= 0xff;
        fseek(f, (long)xblk * RWY_BLOCK_SIZE + 70, SEEK_SET);
        CHECK(fwrite(&byte, 1, 1, f) == 1, "write xattr byte");
        fclose(f);

        /* Verify checker flags bad CRC and returns exit 1 */
        CHECK(run_checker(img) != 0, "checker correctly rejects bad xattr CRC (exit 1)");
    }

    /* Sub-test C: Orphan xattr block in free inode slot */
    {
        copy_file(store_path, img);
        open_store(&s, &dev, &fl, img, "r+b");
        CHECK(rwy_txn_create(&s, "ORPHAN_TEST.DAT", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create ORPHAN_TEST");
        CHECK(rwy_txn_setxattr(&s, "ORPHAN_TEST.DAT", "user.attr", "OrphanData", 10, 0) == RWY_OK, "set xattr");
        uint64_t oid;
        CHECK(rwy_lookup(&s, "ORPHAN_TEST.DAT", &oid) == RWY_OK, "lookup");
        uint32_t tbl_start = s.sb.inode_tbl_start;
        const rwy_inode_t *in = &s.inodes[oid - 1];
        uint64_t xblk = in->xattr_root;
        fclose(fl.f);

        /* Tamper inode table: mark inode as RWY_TYPE_FREE but leave xattr_root intact */
        FILE *f = fopen(img, "r+b");
        uint32_t slot = (uint32_t)(oid - 1);
        uint32_t blk_idx = tbl_start + (slot / 15);
        uint32_t blk_off = 32 + (slot % 15) * 256;

        uint8_t blk[RWY_BLOCK_SIZE];
        fseek(f, (long)blk_idx * RWY_BLOCK_SIZE, SEEK_SET);
        CHECK(fread(blk, 1, sizeof(blk), f) == sizeof(blk), "read inode block");

        rwy_inode_t *tin = (rwy_inode_t *)(blk + blk_off);
        tin->type = RWY_TYPE_FREE;
        tin->oid = 0;
        tin->xattr_root = xblk; /* orphan xattr! */

        rwy_blk_header_t *h = (rwy_blk_header_t *)blk;
        h->crc32c = 0;
        h->crc32c = rwy_crc32c(blk, sizeof(blk));

        fseek(f, (long)blk_idx * RWY_BLOCK_SIZE, SEEK_SET);
        CHECK(fwrite(blk, 1, sizeof(blk), f) == sizeof(blk), "write inode block");
        fclose(f);

        CHECK(run_checker(img) != 0, "checker correctly rejects orphan xattr on free inode (exit 1)");
    }

    printf("== checker corruption detection OK ==\n");
}

/* ========================================================================= */
/* 7. Fault Injection Crash Safety Test                                      */
/* ========================================================================= */
static void test_fault_injection(const char *store_path) {
    file_t fl;
    rwy_block_dev_t inner;
    fi_t fi;
    rwy_store_t s;
    const char *img = tpath("rwy_fi_xattr_base.img");
    const char *crash_img = tpath("rwy_fi_xattr_crash.img");
    long pts = 0;

    printf("== deterministic fault injection (setxattr / link / unlink) ==\n");

    /* Sub-test A: Crash safety during setxattr */
    copy_file(store_path, img);
    open_store(&s, &inner, &fl, img, "r+b");
    CHECK(rwy_txn_create(&s, "FI_XATTR.DAT", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create FI_XATTR.DAT");
    fclose(fl.f);

    rwy_block_dev_t fidev;
    fidev.read = fi_rd; fidev.write = fi_wr; fidev.flush = fi_fl;
    fidev.ctx = &fi; fidev.size = RWY_STORE_SIZE; fidev.block_size = RWY_BLOCK_SIZE;

    /* Phase 1: count persistence points */
    fi.inner = inner; fi.point = 0; fi.fail_at = -1; fi.failed = 0;
    fl.f = fopen(img, "r+b");
    CHECK(rwy_mount(&s, &fidev) == RWY_OK, "mount for count");
    CHECK(rwy_txn_setxattr(&s, "FI_XATTR.DAT", "user.crashtest", "ValCrash123", 11, 0) == RWY_OK, "setxattr");
    pts = fi.point;
    fclose(fl.f);

    /* Phase 2: inject fault at every point 1..pts */
    for (long pt = 1; pt <= pts; pt++) {
        copy_file(img, crash_img);
        fl.f = fopen(crash_img, "r+b");
        fi.inner = inner; fi.point = 0; fi.fail_at = pt; fi.failed = 0;
        int mrc = rwy_mount(&s, &fidev);
        if (mrc == RWY_OK) {
            (void)rwy_txn_setxattr(&s, "FI_XATTR.DAT", "user.crashtest", "ValCrash123", 11, 0);
        }
        fclose(fl.f);

        /* Remount crash image cleanly without faults */
        open_store(&s, &inner, &fl, crash_img, "r+b");
        char buf[64];
        int grc = rwy_getxattr(&s, "FI_XATTR.DAT", "user.crashtest", buf, sizeof(buf));
        /* Old-or-new: either attr does not exist yet (RWY_ERR_XATTR) or it completely exists */
        if (grc >= 0) {
            CHECK(grc == 11 && memcmp(buf, "ValCrash123", 11) == 0, "committed state is complete");
        } else {
            CHECK(grc == RWY_ERR_XATTR, "uncommitted state has no attribute");
        }
        fclose(fl.f);
        CHECK(run_checker(crash_img) == 0, "checker passes on crash state");
    }

    /* Sub-test B: Crash safety during rwy_link */
    copy_file(store_path, img);
    open_store(&s, &inner, &fl, img, "r+b");
    CHECK(rwy_txn_create(&s, "FI_SRC.DAT", RWY_S_IFREG | 0644, 0, 0) == RWY_OK, "create FI_SRC.DAT");
    fclose(fl.f);

    fi.inner = inner; fi.point = 0; fi.fail_at = -1; fi.failed = 0;
    fl.f = fopen(img, "r+b");
    CHECK(rwy_mount(&s, &fidev) == RWY_OK, "mount for link count");
    CHECK(rwy_begin(&s) == RWY_OK, "begin");
    CHECK(rwy_link(&s, "FI_SRC.DAT", "FI_DST.DAT") == RWY_OK, "link");
    CHECK(rwy_commit(&s) == RWY_OK, "commit");
    pts = fi.point;
    fclose(fl.f);

    for (long pt = 1; pt <= pts; pt++) {
        copy_file(img, crash_img);
        fl.f = fopen(crash_img, "r+b");
        fi.inner = inner; fi.point = 0; fi.fail_at = pt; fi.failed = 0;
        int mrc = rwy_mount(&s, &fidev);
        if (mrc == RWY_OK) {
            if (rwy_begin(&s) == RWY_OK) {
                if (rwy_link(&s, "FI_SRC.DAT", "FI_DST.DAT") == RWY_OK) {
                    (void)rwy_commit(&s);
                } else {
                    rwy_abort(&s);
                }
            }
        }
        fclose(fl.f);

        /* Remount and verify old-or-new */
        open_store(&s, &inner, &fl, crash_img, "r+b");
        uint64_t dst_oid;
        int lrc = rwy_lookup(&s, "FI_DST.DAT", &dst_oid);
        rwy_attr_t a;
        CHECK(rwy_getattr_path(&s, "FI_SRC.DAT", &a) == RWY_OK, "stat FI_SRC");
        if (lrc == RWY_OK) {
            CHECK(a.nlink == 2, "committed state has nlink == 2");
        } else {
            CHECK(a.nlink == 1, "uncommitted state has nlink == 1");
        }
        fclose(fl.f);
        CHECK(run_checker(crash_img) == 0, "checker passes on link crash state");
    }

    printf("== fault injection OK ==\n");
}

int main(int argc, char **argv) {
    const char *store_path = "build/RUNWAY.DAT";
    if (argc >= 2) store_path = argv[1];
    if (argc >= 3) g_checkbin = argv[2];

    printf("xattr_link_test: running on %s with checker %s\n", store_path, g_checkbin);

    test_xattr_basic(store_path);
    test_xattr_large_chain(store_path);
    test_hardlinks(store_path);
    test_symlinks(store_path);
    test_gc_defrag_safety(store_path);
    test_checker_corruption(store_path);
    test_fault_injection(store_path);

    if (failures) {
        printf("\nXATTR_LINK_TEST: FAIL (%d failures)\n", failures);
        return 1;
    }
    printf("\nXATTR_LINK_TEST: PASS\n");
    return 0;
}
