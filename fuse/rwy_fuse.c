/*
 * rwy_fuse — Linux FUSE adapter around the portable Runway core.
 * SPDX-License-Identifier: Apache-2.0
 *
 * This layer is a protocol adapter, NOT a second filesystem implementation:
 * every operation translates straight into rwy_core calls. The same core
 * serves the EFI adapter (src/rwy_fs.c) and the host tools.
 *
 * Transaction model: each mutating FUSE request runs as exactly one RUNWAY
 * transaction (begin/op/commit). Consequences (documented):
 *  - write()/create()/rename()/etc. are durable when they return: the new
 *    filesystem generation has been published (atomic root publication)
 *    and flushed to the backing file before the syscall returns.
 *  - fsync()/flush() therefore have nothing left to do and succeed as
 *    no-ops. Durability column: fsync guarantees "all prior ops durable",
 *    which holds trivially.
 *  - rename-over-existing is atomic: after a crash the name holds either
 *    the complete old or the complete new object (see UKI rotation test).
 *
 * Permission enforcement: mount with `-o default_permissions` so the
 * kernel performs standard UNIX permission checks from the uid/gid/mode
 * we report. The core faithfully stores 32-bit uid/gid and full mode
 * bits (incl. suid/sgid/sticky); the adapter never second-guesses them.
 *
 * Time: wall-clock via clock_gettime, installed as the core's fixed time
 * at each request entry, so mtime/ctime/atime are real times while image
 * builds can still force deterministic timestamps via rwy_set_fixed_time.
 *
 * Build: make fuse
 * Run (single-threaded; the core is single-threaded by design):
 *   build/rwy-fuse -s -o default_permissions STORE MOUNTPOINT
 */
#define _GNU_SOURCE
#define FUSE_USE_VERSION 34

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <fuse.h>
#include <fuse_lowlevel.h>

#include "hostdev.h"
#include "rwy_core.h"
#include "crypto_openssl.h"
#include "rwy_fuse_virt.h"
#include "guard_linux.h"
#include "freeze_linux.h"
#include "rwy_fuse_store.h"
#include "rwy_blob.h"
#include "rwyctl_open.h"
#include "rwyctl.h"
#include "rwyctl_rpc.h"

rwy_store_t g_store;
static int g_mounted;
const char *g_root = "";
int g_rw = 1;
int g_verbose = 0;
member_t g_mem[MAXG * MAXM];
int g_nmem;
group_t g_grp[MAXG];
int g_ngrp;
char g_tmpmnt[8][PATH_MAX];
int g_ntmp;
char g_why[PATH_MAX + 128];
static int g_freeze_wait;

void note_write_failures(void) {
    rwy_fuse_store_note_failures(&g_store);
}

/* The answer of an operation when there is no store to work on. */
static int no_store(void) {
    return -EIO;
}

#define use_wall_clock() hostdev_use_wall_clock(&g_store)

/* FUSE paths are absolute ("/a/b"); the core takes them as-is. */

void fill_stbuf(const rwy_attr_t *a, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_ino = (ino_t)a->oid;
    st->st_mode = (mode_t)a->mode;
    st->st_uid = (uid_t)a->uid;
    st->st_gid = (gid_t)a->gid;
    st->st_size = (off_t)a->size;
    st->st_blksize = RWY_BLOCK_SIZE;
    st->st_blocks = (blkcnt_t)((a->size + 511) / 512);
    st->st_atim.tv_sec = (time_t)a->atime_sec;
    st->st_atim.tv_nsec = (long)a->atime_nsec;
    st->st_mtim.tv_sec = (time_t)a->mtime_sec;
    st->st_mtim.tv_nsec = (long)a->mtime_nsec;
    st->st_ctim.tv_sec = (time_t)a->ctime_sec;
    st->st_ctim.tv_nsec = (long)a->ctime_nsec;
    st->st_nlink = (nlink_t)(a->nlink ? a->nlink : 1);
}

static int tgetattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    rwy_attr_t a;
    int rc;
    (void)fi;
    if (!g_mounted) return no_store();
    rc = rwy_fuse_virt_getattr(&g_store, path, st);
    if (rc != -ENOENT) return rc;
    rc = rwy_getattr_path(&g_store, path, &a);
    if (rc) return err_of(rc);
    fill_stbuf(&a, st);
    return 0;
}

#define RWY_FUSE_DIRTY_LIMIT (8 * 1024 * 1024)

static size_t g_dirty_bytes;

int store_commit_active(void) {
    int rc;
    g_dirty_bytes = 0;
    if (!g_store.in_txn) return 0;
    rc = rwy_commit(&g_store);
    return err_of(rc);
}

static int begin_op(void) {
    int rc = store_commit_active();
    if (rc) return rc;
    use_wall_clock();
    rc = rwy_begin(&g_store);
    return err_of(rc);
}

static int tchmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    char res[300];
    int rc, vrc;
    (void)fi;
    if (!g_mounted) return no_store();
    vrc = rwy_fuse_virt_resolve_path(&g_store, path, res, sizeof(res));
    if (vrc >= 0) return 0;

    rc = begin_op();
    if (rc) return rc;
    rc = rwy_chmod(&g_store, path, (uint32_t)mode);
    if (rc) { rwy_abort(&g_store); return err_of(rc); }
    return err_of(rwy_commit(&g_store));
}

static int tchown(const char *path, uid_t uid, gid_t gid,
                  struct fuse_file_info *fi) {
    char res[300];
    int rc, vrc;
    (void)fi;
    if (!g_mounted) return no_store();
    vrc = rwy_fuse_virt_resolve_path(&g_store, path, res, sizeof(res));
    if (vrc >= 0) return 0;
    rc = begin_op();
    if (rc) return rc;
    rc = rwy_chown(&g_store, path, (uint32_t)uid, (uint32_t)gid);
    if (rc) { rwy_abort(&g_store); return err_of(rc); }
    return err_of(rwy_commit(&g_store));
}

static int tutimens(const char *path, const struct timespec tv[2],
                    struct fuse_file_info *fi) {
    char res[300];
    rwy_attr_t cur;
    int rc, vrc;
    int64_t as, ms;
    uint32_t an, mn;
    (void)fi;
    if (!g_mounted) return no_store();
    vrc = rwy_fuse_virt_resolve_path(&g_store, path, res, sizeof(res));
    if (vrc >= 0) return 0;

    rc = rwy_getattr_path(&g_store, path, &cur);
    if (rc) return err_of(rc);
    if (tv) { as = tv[0].tv_sec; an = tv[0].tv_nsec; ms = tv[1].tv_sec; mn = tv[1].tv_nsec; }
    else {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        as = ms = now.tv_sec; an = mn = now.tv_nsec;
    }
    rc = begin_op();
    if (rc) return rc;
    rc = rwy_utimens(&g_store, path, as, an, ms, mn);
    if (rc) { rwy_abort(&g_store); return err_of(rc); }
    return err_of(rwy_commit(&g_store));
}

static int treaddir(const char *path, void *buf, fuse_fill_dir_t filler,
                    off_t off, struct fuse_file_info *fi,
                    enum fuse_readdir_flags flags) {
    static char names[16 * RWY_BLOCK_SIZE];
    static uint8_t types[2048];
    size_t cnt = 2048;
    size_t i, o = 0;
    int rc;
    (void)off; (void)fi; (void)flags;
    if (!g_mounted) return no_store();
    rc = rwy_fuse_virt_readdir(&g_store, path, buf, filler);
    if (rc != -ENOENT) return rc;
    rc = rwy_readdir(&g_store, path, names, sizeof(names), &cnt, types);
    if (rc && rc != RWY_ERR_NOENT) return err_of(rc);
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    if (rc == RWY_OK) {
        for (i = 0; i < cnt; i++) {
            struct stat st;
            const char *nm = names + o;
            char full[RWY_MAX_PATH + 2];
            size_t nmlen = strlen(nm);
            o += nmlen + 1;
            if (nmlen > RWY_NAME_MAX) continue; /* core never emits these */
            if (!strcmp(path, "/")) snprintf(full, sizeof(full), "/%.*s",
                                              RWY_NAME_MAX, nm);
            else snprintf(full, sizeof(full), "%s/%.*s", path, RWY_NAME_MAX, nm);
            memset(&st, 0, sizeof(st));
            if (rwy_getattr_path(&g_store, full, &(rwy_attr_t){0}) == RWY_OK) {
                rwy_attr_t a;
                rwy_getattr_path(&g_store, full, &a);
                fill_stbuf(&a, &st);
            }
            if (filler(buf, nm, &st, 0, 0)) break;
        }
    }
    rwy_fuse_virt_readdir_extra(&g_store, path, buf, filler);
    return 0;
}

static int topen(const char *path, struct fuse_file_info *fi) {
    uint64_t oid;
    int rc;
    if (!g_mounted) return no_store();
    rc = rwy_fuse_virt_open(&g_store, path, fi);
    if (rc != -ENOENT) return rc;
    if ((fi->flags & O_ACCMODE) == O_RDONLY) {
        rc = rwy_lookup(&g_store, path, &oid);
        if (rc) return err_of(rc);
        fi->fh = oid;
        return 0;
    }
    rc = rwy_lookup(&g_store, path, &oid);
    if (rc) return err_of(rc);
    if ((fi->flags & O_TRUNC)) {
        rc = begin_op();
        if (rc) return rc;
        rc = rwy_truncate_oid(&g_store, oid, 0);
        if (rc) { rwy_abort(&g_store); return err_of(rc); }
        rc = rwy_commit(&g_store);
        if (rc) return err_of(rc);
    }
    fi->fh = oid;
    return 0;
}

static int tcreate(const char *path, mode_t mode, struct fuse_file_info *fi) {
    uint64_t oid;
    int rc;
    if (!g_mounted) return no_store();
    rc = store_commit_active();
    if (rc) return rc;
    rc = rwy_fuse_virt_create(&g_store, path, mode, fi);
    if (rc != -ENOENT) return rc;
    rc = begin_op();
    if (rc) return rc;
    rc = rwy_create(&g_store, path, (uint32_t)mode,
                     (uint32_t)fuse_get_context()->uid,
                     (uint32_t)fuse_get_context()->gid);
    if (rc) { rwy_abort(&g_store); return err_of(rc); }
    rc = rwy_commit(&g_store);
    if (rc) return err_of(rc);
    rc = rwy_lookup(&g_store, path, &oid);
    if (rc) return err_of(rc);
    fi->fh = oid;
    return 0;
}

static int tmkdir(const char *path, mode_t mode) {
    int rc;
    if (!g_mounted) return no_store();
    rc = store_commit_active();
    if (rc) return rc;
    rc = rwy_fuse_virt_mkdir(&g_store, path, mode);
    if (rc != -ENOENT) return rc;
    rc = begin_op();
    if (rc) return rc;
    rc = rwy_mkdir(&g_store, path, (uint32_t)mode,
                    (uint32_t)fuse_get_context()->uid,
                    (uint32_t)fuse_get_context()->gid);
    if (rc) { rwy_abort(&g_store); return err_of(rc); }
    return err_of(rwy_commit(&g_store));
}

static int tsymlink(const char *target, const char *path) {
    int rc;
    if (!g_mounted) return no_store();
    rc = begin_op();
    if (rc) return rc;
    rc = rwy_symlink(&g_store, path, target,
                      (uint32_t)fuse_get_context()->uid,
                      (uint32_t)fuse_get_context()->gid);
    if (rc) { rwy_abort(&g_store); return err_of(rc); }
    return err_of(rwy_commit(&g_store));
}

static int treadlink(const char *path, char *out, size_t sz) {
    int rc;
    if (!g_mounted) return no_store();
    rc = rwy_readlink(&g_store, path, out, sz);
    return err_of(rc);
}

static int tlink(const char *oldpath, const char *newpath) {
    int rc;
    if (!g_mounted) return no_store();
    rc = begin_op();
    if (rc) return rc;
    rc = rwy_link(&g_store, oldpath, newpath);
    if (rc) {
        rwy_abort(&g_store);
        if (rc == RWY_ERR_INVAL) return -EPERM;
        return err_of(rc);
    }
    return err_of(rwy_commit(&g_store));
}

static int tunlink(const char *path) {
    int rc;
    if (!g_mounted) return no_store();
    rc = store_commit_active();
    if (rc) return rc;
    rc = rwy_fuse_virt_unlink(&g_store, path);
    if (rc != -ENOENT) return rc;
    rc = begin_op();
    if (rc) return rc;
    rc = rwy_unlink(&g_store, path);
    if (rc) { rwy_abort(&g_store); return err_of(rc); }
    return err_of(rwy_commit(&g_store));
}

static int trmdir(const char *path) {
    int rc;
    if (!g_mounted) return no_store();
    rc = store_commit_active();
    if (rc) return rc;
    rc = rwy_fuse_virt_rmdir(&g_store, path);
    if (rc != -ENOENT) return rc;
    rc = begin_op();
    if (rc) return rc;
    rc = rwy_rmdir(&g_store, path);
    if (rc) { rwy_abort(&g_store); return err_of(rc); }
    return err_of(rwy_commit(&g_store));
}

static int trename(const char *from, const char *to, unsigned flags) {
    int rc;
    if (flags && flags != 1 /* RENAME_NOREPLACE */) return -EINVAL;
    if (!g_mounted) return no_store();
    rc = store_commit_active();
    if (rc) return rc;
    rc = rwy_fuse_virt_rename_stage(&g_store, from, to, flags);
    if (rc != -ENOENT) return rc;
    if (flags & 1) {
        uint64_t to_oid;
        if (rwy_lookup(&g_store, to, &to_oid) == RWY_OK) return -EEXIST;
    }
    rc = begin_op();
    if (rc) return rc;
    rc = rwy_rename(&g_store, from, to);
    if (rc) { rwy_abort(&g_store); return err_of(rc); }
    return err_of(rwy_commit(&g_store));
}

static int ttruncate(const char *path, off_t size, struct fuse_file_info *fi) {
    int rc;
    (void)fi;
    if (size < 0) return -EINVAL;
    if (!g_mounted) return no_store();
    rc = store_commit_active();
    if (rc) return rc;
    rc = rwy_fuse_virt_truncate(&g_store, path, size);
    if (rc != -ENOENT) return rc;
    rc = begin_op();
    if (rc) return rc;
    rc = rwy_truncate(&g_store, path, (uint64_t)size);
    if (rc) { rwy_abort(&g_store); return err_of(rc); }
    return err_of(rwy_commit(&g_store));
}

static int tread(const char *path, char *buf, size_t size, off_t off,
                 struct fuse_file_info *fi) {
    uint64_t oid;
    size_t got = size;
    int rc;
    if (!g_mounted) return no_store();
    if (off < 0) return -EINVAL;
    if (fi->fh == RWY_FH_VIRT_SREL || fi->fh == RWY_FH_VIRT_CONF ||
        (fi->fh >= RWY_FH_STAGED_BASE && fi->fh < RWY_FH_STAGED_BASE + 32))
        return rwy_fuse_virt_read(&g_store, path, buf, size, off, fi);
    (void)path;
    oid = fi->fh ? fi->fh : 0;
    if (!oid) {
        rc = rwy_lookup(&g_store, path, &oid);
        if (rc) return err_of(rc);
    }
    rc = rwy_read(&g_store, oid, (uint64_t)off, buf, &got);
    if (rc) return err_of(rc);
    return (int)got;
}

/* The health point: OK written to the STATE of the slot that started clears the boot loop counter.
 * RWYCTL_ROOT moves the efivarfs path, as for rwyctl. */
static void health_point(const char *path, const char *buf, size_t size, off_t off) {
    const char *slash, *root = getenv("RWYCTL_ROOT");
    char name[RWY_NAME_MAX + 1];
    size_t n;
    int rc;
    if (off != 0 || size < 2 || strncmp(path, "/EFI/RUNWAY/", 12) || strncmp(buf, "OK", 2) || (size > 2 && buf[2] != '\n')) return;
    if (!(slash = strchr(path + 12, '/')) || strcmp(slash, "/STATE")) return;
    n = (size_t)(slash - (path + 12));
    if (n > RWY_NAME_MAX) return;
    memcpy(name, path + 12, n);
    name[n] = 0;
    if ((rc = guard_healthy(&g_store, root ? root : "", name)))
        fprintf(stderr, "rwy-fuse: confirmed %s, but cannot clear the boot loop counter: %s\n", name, strerror(-rc));
}

static int is_health_point(const char *path, const char *buf, size_t size, off_t off) {
    const char *slash;
    if (off != 0 || size < 2 || !path || strncmp(path, "/EFI/RUNWAY/", 12) ||
        strncmp(buf, "OK", 2) || (size > 2 && buf[2] != '\n')) return 0;
    if (!(slash = strchr(path + 12, '/')) || strcmp(slash, "/STATE")) return 0;
    return 1;
}

static int twrite(const char *path, const char *buf, size_t size, off_t off,
                  struct fuse_file_info *fi) {
    uint64_t oid;
    int rc;
    (void)path;
    if (!g_mounted) return no_store();
    if (off < 0) return -EINVAL;
    if (size == 0) return 0;
    rc = rwy_fuse_virt_write(&g_store, path, buf, size, off, fi);
    if (rc != -ENOENT) return rc;
    oid = fi->fh ? fi->fh : 0;
    if (!oid) {
        rc = rwy_lookup(&g_store, path, &oid);
        if (rc) return err_of(rc);
    }
    use_wall_clock();
    if (!g_store.in_txn) {
        rc = rwy_begin(&g_store);
        if (rc) return err_of(rc);
    }
    rc = rwy_write(&g_store, oid, (uint64_t)off, buf, size);
    if (rc) {
        g_dirty_bytes = 0;
        rwy_abort(&g_store);
        return err_of(rc);
    }
    g_dirty_bytes += size;
    if (is_health_point(path, buf, size, off) || g_dirty_bytes >= RWY_FUSE_DIRTY_LIMIT) {
        rc = store_commit_active();
        if (rc) return rc;
        health_point(path, buf, size, off);
    }
    return (int)size;
}

static int tflush(const char *path, struct fuse_file_info *fi) {
    int vrc, crc;
    if (!g_mounted) return no_store();
    vrc = rwy_fuse_virt_flush(&g_store, path, fi);
    crc = store_commit_active();
    return vrc ? vrc : crc;
}

/* Count the writes a member failed. Checked when a file closes, on fsync and at unmount, not on every request. */
static int trelease(const char *path, struct fuse_file_info *fi) {
    int rc, crc;
    if (!g_mounted) return no_store();
    crc = store_commit_active();
    rc = rwy_fuse_virt_release(&g_store, path, fi);
    rwy_fuse_store_note_failures(&g_store);
    return crc ? crc : rc;
}

static int tfsync(const char *path, int datasync, struct fuse_file_info *fi) {
    int rc;
    (void)path; (void)datasync; (void)fi;
    if (!g_mounted) return no_store();
    rc = store_commit_active();
    rwy_fuse_store_note_failures(&g_store);
    return rc;
}

static void tdestroy(void *private_data) {
    (void)private_data;
    if (g_mounted) {
        store_commit_active();
        rwy_fuse_store_note_failures(&g_store);
    }
}

static int tstatfs(const char *path, struct statvfs *st) {
    uint32_t free;
    (void)path;
    if (!g_mounted) return no_store();
    free = rwy_free_blocks(&g_store);
    memset(st, 0, sizeof(*st));
    st->f_bsize = RWY_BLOCK_SIZE;
    st->f_frsize = RWY_BLOCK_SIZE;
    st->f_blocks = g_store.sb.total_blocks;
    st->f_bfree = free;
    st->f_bavail = free;
    st->f_files = g_store.sb.max_inodes;
    st->f_namemax = RWY_NAME_MAX;
    return 0;
}

static void *tinit(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn;
    cfg->kernel_cache = 0; /* keep the kernel honest across remount tests */
    /* The store can change while the mount is closed: keep no names or attributes */
    cfg->entry_timeout = 0;
    cfg->attr_timeout = 0;
    cfg->negative_timeout = 0;
    return NULL;
}

static int tsetxattr(const char *path, const char *name, const char *value,
                        size_t size, int flags) {
    char res[300];
    int rc, vrc;
    if (!g_mounted) return no_store();
    vrc = rwy_fuse_virt_resolve_path(&g_store, path, res, sizeof(res));
    if (vrc >= 0) return -ENOTSUP;
    rc = begin_op();
    if (rc) return rc;
    rc = rwy_setxattr(&g_store, path, name, value, size, flags);
    if (rc) { rwy_abort(&g_store); return err_of(rc); }
    return err_of(rwy_commit(&g_store));
}

static int tgetxattr(const char *path, const char *name, char *value, size_t size) {
    char res[300];
    int rc, vrc;
    if (!g_mounted) return no_store();
    vrc = rwy_fuse_virt_resolve_path(&g_store, path, res, sizeof(res));
    if (vrc == 1) return -ENODATA;
    if (vrc == 0) path = res;
    rc = rwy_getxattr(&g_store, path, name, value, size);
    if (rc < 0) return err_of(rc);
    return rc;
}

static int tlistxattr(const char *path, char *list, size_t size) {
    char res[300];
    int rc, vrc;
    if (!g_mounted) return no_store();
    vrc = rwy_fuse_virt_resolve_path(&g_store, path, res, sizeof(res));
    if (vrc == 1) return 0;
    if (vrc == 0) path = res;
    rc = rwy_listxattr(&g_store, path, list, size);
    if (rc < 0) return err_of(rc);
    return rc;
}

static int tremovexattr(const char *path, const char *name) {
    char res[300];
    int rc, vrc;
    if (!g_mounted) return no_store();
    vrc = rwy_fuse_virt_resolve_path(&g_store, path, res, sizeof(res));
    if (vrc >= 0) return -ENODATA;
    rc = begin_op();
    if (rc) return rc;
    rc = rwy_removexattr(&g_store, path, name);
    if (rc) { rwy_abort(&g_store); return err_of(rc); }
    return err_of(rwy_commit(&g_store));
}

static const struct fuse_operations rwy_ops = {
    .getattr = tgetattr,
    .chmod = tchmod,
    .chown = tchown,
    .utimens = tutimens,
    .readdir = treaddir,
    .open = topen,
    .create = tcreate,
    .mkdir = tmkdir,
    .symlink = tsymlink,
    .readlink = treadlink,
    .link = tlink,
    .unlink = tunlink,
    .rmdir = trmdir,
    .rename = trename,
    .truncate = ttruncate,
    .read = tread,
    .write = twrite,
    .flush = tflush,
    .release = trelease,
    .fsync = tfsync,
    .statfs = tstatfs,
    .init = tinit,
    .destroy = tdestroy,
    .setxattr = tsetxattr,
    .getxattr = tgetxattr,
    .listxattr = tlistxattr,
    .removexattr = tremovexattr,
};

#define USAGE_FMT "usage: %s [fuse opts] [--key PASS] [--key-file FILE] [--dek-file FILE] [--tpm DEV] STORE... MOUNT\n" \
                  "example: %s -s -o default_permissions build/RUNWAY.DAT /mnt/runway\n" \
                  "Several STOREs are the members of one mirror; each is locked.\n" \
                  "A STORE is a file, a partition (/dev/sda2 or PARTUUID=UUID), or IMAGE@OFFSET[+LENGTH] for a store inside an image.\n"

static const char *g_key_pass;
static const char *g_freeze_wait_arg;

/* Read the --freeze-wait value. Returns 0 or an exit code. */
static int parse_freeze_wait(void) {
    char *end;
    long v;

    if (!g_freeze_wait_arg) return 0;
    v = strtol(g_freeze_wait_arg, &end, 10);
    if (*g_freeze_wait_arg == '\0' || *end != '\0' || v < 0 || v > 86400) {
        fprintf(stderr, "rwy-fuse: bad --freeze-wait value '%s'\n", g_freeze_wait_arg);
        return 2;
    }
    g_freeze_wait = (int)v;
    return 0;
}

static int parse_string_opt(const char *arg, const char *next, int *consumed) {
    static const struct { const char *prefix; const char **target; } opts[] = {
        { "--key", &g_key_pass },
        { "-k", &g_key_pass },
        { "--key-file", &g_passfile },
        { "--passphrase-file", &g_passfile },
        { "--dek-file", &g_dekfile },
        { "--tpm", &g_tpm },
        { "--freeze-wait", &g_freeze_wait_arg },
    };
    size_t i;
    *consumed = 0;
    for (i = 0; i < sizeof(opts) / sizeof(opts[0]); i++) {
        size_t len = strlen(opts[i].prefix);
        if (!strcmp(arg, opts[i].prefix) && next) {
            *opts[i].target = next;
            *consumed = 1;
            return 1;
        }
        if (!strncmp(arg, opts[i].prefix, len) && arg[len] == '=') {
            *opts[i].target = arg + len + 1;
            return 1;
        }
    }
    return 0;
}

/* Extract --key/-k, --key-file, --dek-file, --tpm, --freeze-wait, forward options to fuse; the rest is STORE... MOUNTPOINT. Returns 0 or an exit code. */
static int split_args(int argc, char **argv, char **fuse_argv, int *fuse_argc, const char **pos, int *npos,
                      const char **key_pass) {
    int i;

    for (i = 1; i < argc; i++) {
        int consumed = 0;
        if (parse_string_opt(argv[i], i + 1 < argc ? argv[i + 1] : NULL, &consumed)) {
            i += consumed;
            continue;
        }
        if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            fuse_argv[(*fuse_argc)++] = argv[i++];
            fuse_argv[(*fuse_argc)++] = argv[i];
            continue;
        }
        if (argv[i][0] == '-') { fuse_argv[(*fuse_argc)++] = argv[i]; continue; }
        if (*npos == RWY_MIRROR_MAX_DEVS + 1) { fprintf(stderr, "rwy-fuse: too many stores\n"); return 2; }
        pos[(*npos)++] = argv[i];
    }
    if (*npos < 2) {
        fprintf(stderr, USAGE_FMT, argv[0], argv[0]);
        return 2;
    }
    *key_pass = g_key_pass;
    return parse_freeze_wait();
}

/* The loop state: the FUSE session and the RPC listener. */
struct loop {
    struct fuse_session *se;
    struct fuse_buf buf;
    int listener;
    int64_t retry_ms;      /* when to try to open the store again, while it is not open */
};

#define RETRY_MS 1000

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Receive and process one request. Returns 0 to go on, 1 on a normal end, -1 on an error. */
static int serve_request(struct loop *l) {
    int res = fuse_session_receive_buf(l->se, &l->buf);

    if (res == -EINTR) return 0;
    if (res == 0) return 1;
    if (res < 0) return -1;
    fuse_session_process_buf(l->se, &l->buf);
    return 0;
}

/* Open the store again when it was lost, once the retry time has come. */
static void retry_open(struct loop *l) {
    if (g_mounted || now_ms() < l->retry_ms) return;
    g_mounted = rwy_fuse_store_reopen(&g_store) == 0;
    if (g_mounted) syslog(LOG_INFO, "store opened again");
    else l->retry_ms = now_ms() + RETRY_MS;
}

/* Wait for one event and handle it. Returns 0 to go on, 1 on a normal end, -1 on an error. */
static int serve_once(struct loop *l, const sigset_t *wait_mask) {
    struct pollfd p[2];
    struct timespec ts;
    const struct timespec *to = NULL;
    int rc;

    retry_open(l);
    p[0] = (struct pollfd){ .fd = fuse_session_fd(l->se), .events = POLLIN };
    p[1] = (struct pollfd){ .fd = l->listener, .events = POLLIN };

    if (!g_mounted) {
        int64_t left = l->retry_ms - now_ms();
        if (left < 0) left = 0;
        ts.tv_sec = (time_t)(left / 1000);
        ts.tv_nsec = (long)(left % 1000) * 1000000;
        to = &ts;
    }

    if (ppoll(p, 2, to, wait_mask) < 0) return errno == EINTR ? 0 : -1;
    if (p[0].revents) {
        rc = serve_request(l);
        if (rc) return rc;
    }
    if (p[1].revents & POLLIN) {
        int conn = freeze_accept(l->listener);
        if (conn >= 0) {
            rwyctl_rpc_server_handle(conn);
            close(conn);
        }
    }
    return 0;
}

/* Serve requests until unmount or signal. The signals fuse catches are blocked, and only ppoll() lets them in. */
static int serve(struct fuse_session *se, int listener) {
    struct loop l = { .se = se, .listener = listener };
    sigset_t block, orig;
    int res = 0;

    sigemptyset(&block);
    sigaddset(&block, SIGHUP);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigprocmask(SIG_BLOCK, &block, &orig);
    while (!fuse_session_exited(se) && (res = serve_once(&l, &orig)) == 0)
        ;
    sigprocmask(SIG_SETMASK, &orig, NULL);
    free(l.buf.mem);
    fuse_session_reset(se);
    return res < 0 ? 1 : 0;
}

/* The same steps as fuse_main() for a single threaded daemon. Returns the exit code. */
static int run_fuse(int argc, char **argv) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_cmdline_opts opts;
    struct fuse *fuse;
    struct fuse_session *se;
    int rc = 1;

    if (fuse_parse_cmdline(&args, &opts)) { fuse_opt_free_args(&args); return 1; }
    if (opts.show_help) {
        printf("usage: %s [options] <mountpoint>\n\n", argv[0]);
        fuse_cmdline_help();
        fuse_lib_help(&args);
        rc = 0;
    } else if (opts.show_version) {
        printf("FUSE library version %s\n", fuse_pkgversion());
        fuse_lowlevel_version();
        rc = 0;
    } else if (!opts.mountpoint) {
        fprintf(stderr, "rwy-fuse: no mount point\n");
    } else if ((fuse = fuse_new(&args, &rwy_ops, sizeof(rwy_ops), NULL)) != NULL) {
        se = fuse_get_session(fuse);
        if (fuse_mount(fuse, opts.mountpoint) == 0) {
            if (fuse_daemonize(opts.foreground) == 0 && fuse_set_signal_handlers(se) == 0) {
                int listener = freeze_listen(freeze_dir(), g_store.sb.uuid);

                if (listener < 0) fprintf(stderr, "rwy-fuse: warning: no freeze socket\n");
                rc = serve(se, listener);
                freeze_close(listener);
                fuse_remove_signal_handlers(se);
            }
            fuse_unmount(fuse);
        }
        fuse_destroy(fuse);
    }
    free(opts.mountpoint);
    fuse_opt_free_args(&args);
    return rc;
}

/* No rwyctl can be copying yet, so the staging files in the store belong to a killed one. */
static void remove_stale_staging(void) {
    int n = rwy_blob_staging_remove(&g_store);

    if (n > 0) syslog(LOG_INFO, "removed %d staging files of a killed rwyctl", n);
    else if (n < 0) syslog(LOG_WARNING, "cannot remove the staging files (%d)", n);
}

/* Name the mount after the store, so that rwyctl finds the mount point of a store in /proc/self/mountinfo. */
static int set_fsname(char **fuse_argv, int *fuse_argc) {
    char u[37];
    char *opt;

    rwy_format_uuid(g_store.sb.uuid, u);
    if (asprintf(&opt, "-ofsname=" RWY_FUSE_FSNAME "%s", u) < 0) return 1;
    fuse_argv[(*fuse_argc)++] = opt;
    fuse_argv[(*fuse_argc)++] = "-osubtype=rwy-fuse";   /* fsname alone makes the type plain "fuse" */
    return 0;
}

int main(int argc, char **argv) {
    const char *pos[RWY_MIRROR_MAX_DEVS + 1];
    const char *key_pass = NULL;
    char **fuse_argv;
    int fuse_argc = 0, npos = 0, rc;

    rwy_crypto_host_init();
    openlog("rwy-fuse", LOG_PID, LOG_DAEMON);

    if (argc < 3) {
        fprintf(stderr, USAGE_FMT, argv[0], argv[0]);
        return 2;
    }
    fuse_argv = calloc((size_t)argc + 5, sizeof(char *));
    if (!fuse_argv) return 1;
    fuse_argv[fuse_argc++] = argv[0];
    fuse_argv[fuse_argc++] = "-s";

    rc = split_args(argc, argv, fuse_argv, &fuse_argc, pos, &npos, &key_pass);
    if (rc) { free(fuse_argv); return rc; }
    fuse_argv[fuse_argc++] = (char *)pos[--npos];   /* the mount point */
    if (!key_pass) key_pass = getenv("RWY_KEY");

    if (rwy_fuse_store_open(&g_store, pos, npos, key_pass)) { free(fuse_argv); return 1; }
    g_mounted = 1;
    remove_stale_staging();
    if (set_fsname(fuse_argv, &fuse_argc)) { free(fuse_argv); return 1; }

    rc = run_fuse(fuse_argc, fuse_argv);
    free(fuse_argv);
    return rc;
}
