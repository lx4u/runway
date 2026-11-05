/*
 * rwy_fuse_virt.c — Synthetic BLS projection, staging, and virtual nodes for FUSE.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hostdev.h"
#include "rwy_fuse_virt.h"
#include "rwy_bls_view.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define MAX_STAGED_ENTRIES 16
#define MAX_OPEN_PAYLOADS 64
#define MAX_VIRT_DIRS 32

typedef struct {
    char entry_name[RWY_ENTRY_MAX + 1];
    char target_path[RWY_MAX_PATH + 1];
    char *conf_buf;
    size_t conf_len;
    size_t conf_cap;
    int is_type2;
    char blob_path[128];
    int open_handles;
    int active;
    int refused_logged;   /* the entry was refused for space and said so */
} staged_entry_t;

typedef struct {
    char path[RWY_MAX_PATH + 1];
    uint64_t oid;
    int open_handles;
    int active;
} open_payload_t;

static staged_entry_t g_staged[MAX_STAGED_ENTRIES];
static open_payload_t g_payloads[MAX_OPEN_PAYLOADS];
static char g_virt_dirs[MAX_VIRT_DIRS][RWY_MAX_PATH + 1];
static size_t g_virt_dirs_cnt;

#define virt_use_wall_clock hostdev_use_wall_clock

static rwy_bls_view_t g_view;

/* The view of the committed store. NULL when there is none (a transaction, hidden virtual files, no memory): the caller answers -EAGAIN. */
static const rwy_bls_view_t *view_of(rwy_store_t *s) {
    return rwy_bls_view_sync(&g_view, s) == RWY_OK ? &g_view : NULL;
}

static const rwy_bls_view_t *view_or_err(rwy_store_t *s, const char *path, int *err) {
    const rwy_bls_view_t *v = view_of(s);
    if (!v) {
        *err = (!strncmp(path, "/loader", 7) || !strncmp(path, "/EFI/Linux", 10)) ? -EAGAIN : -ENOENT;
        return NULL;
    }
    *err = 0;
    return v;
}

typedef struct { void *buf; fuse_fill_dir_t filler; } fill_ctx_t;

static void fill_name(void *ctx, const char *name) {
    fill_ctx_t *c = ctx;
    c->filler(c->buf, name, NULL, 0, 0);
}

int err_of(int rc) {
    static const int tbl[] = {
        [0] = 0,
        [-RWY_ERR_NOENT] = -ENOENT,
        [-RWY_ERR_EXIST] = -EEXIST,
        [-RWY_ERR_NOSPC] = -ENOSPC,
        [-RWY_ERR_INVAL] = -EINVAL,
        [-RWY_ERR_IO] = -EIO,
        [-RWY_ERR_NOTDIR] = -ENOTDIR,
        [-RWY_ERR_ISDIR] = -EISDIR,
        [-RWY_ERR_NAMETOOLONG] = -EINVAL,
        [-RWY_ERR_NOTEMPTY] = -ENOTEMPTY,
        [-RWY_ERR_FEAT] = -EPROTONOSUPPORT,
        [-RWY_ERR_XATTR] = -ENODATA,
        [-RWY_ERR_RANGE] = -ERANGE,
        [-RWY_ERR_LOOP] = -ELOOP,
        [-RWY_ERR_PERM] = -EPERM,
        [-RWY_ERR_BUSY] = -EBUSY,
    };
    if (rc == RWY_OK) return 0;
    if (rc < 0 && -rc < (int)(sizeof(tbl) / sizeof(tbl[0])) && tbl[-rc])
        return tbl[-rc];
    return -EIO;
}

static staged_entry_t *find_staged_by_path(const char *path) {
    size_t i;
    for (i = 0; i < MAX_STAGED_ENTRIES; i++) {
        if (!g_staged[i].active) continue;
        if (!strcmp(g_staged[i].target_path, path)) return &g_staged[i];
        if (!strncmp(path, "/loader/entries/", 16) &&
            !strcmp(g_staged[i].entry_name, path + 16))
            return &g_staged[i];
        if (!strncmp(path, "/EFI/Linux/", 11) &&
            !strcmp(g_staged[i].entry_name, path + 11))
            return &g_staged[i];
    }
    return NULL;
}

static staged_entry_t *alloc_staged(const char *path, const char *entry_name, int is_type2) {
    staged_entry_t *e = find_staged_by_path(path);
    size_t i;
    if (e) return e;
    for (i = 0; i < MAX_STAGED_ENTRIES; i++) {
        if (!g_staged[i].active) {
            e = &g_staged[i];
            memset(e, 0, sizeof(*e));
            e->active = 1;
            e->is_type2 = is_type2;
            snprintf(e->target_path, sizeof(e->target_path), "%s", path);
            snprintf(e->entry_name, sizeof(e->entry_name), "%s", entry_name);
            return e;
        }
    }
    return NULL;
}

static void payload_inc(const char *path, uint64_t oid) {
    size_t i;
    for (i = 0; i < MAX_OPEN_PAYLOADS; i++) {
        if (g_payloads[i].active && !strcmp(g_payloads[i].path, path)) {
            g_payloads[i].open_handles++;
            return;
        }
    }
    for (i = 0; i < MAX_OPEN_PAYLOADS; i++) {
        if (!g_payloads[i].active) {
            g_payloads[i].active = 1;
            g_payloads[i].oid = oid;
            g_payloads[i].open_handles = 1;
            snprintf(g_payloads[i].path, sizeof(g_payloads[i].path), "%s", path);
            return;
        }
    }
}

static void payload_dec_by_oid(uint64_t oid) {
    size_t i;
    for (i = 0; i < MAX_OPEN_PAYLOADS; i++) {
        if (g_payloads[i].active && g_payloads[i].oid == oid) {
            if (--g_payloads[i].open_handles <= 0)
                g_payloads[i].active = 0;
            return;
        }
    }
}

static int payload_is_open(const char *path) {
    size_t i;
    for (i = 0; i < MAX_OPEN_PAYLOADS; i++) {
        if (g_payloads[i].active && g_payloads[i].open_handles > 0 &&
            !strcmp(g_payloads[i].path, path))
            return 1;
    }
    return 0;
}

static int is_payload_path(const char *path) {
    if (!path || path[0] != '/') return 0;
    if (!strncmp(path, "/loader", 7) || !strncmp(path, "/EFI", 4)) return 0;
    return path[1] != '\0';
}

static void add_virt_dir(const char *path) {
    size_t i;
    for (i = 0; i < g_virt_dirs_cnt; i++) {
        if (!strcmp(g_virt_dirs[i], path)) return;
    }
    if (g_virt_dirs_cnt < MAX_VIRT_DIRS) {
        snprintf(g_virt_dirs[g_virt_dirs_cnt++], RWY_MAX_PATH + 1, "%s", path);
    }
}

static void remove_virt_dir(const char *path) {
    size_t i;
    for (i = 0; i < g_virt_dirs_cnt; i++) {
        if (!strcmp(g_virt_dirs[i], path)) {
            g_virt_dirs[i][0] = 0;
            break;
        }
    }
}

static int is_virt_dir(const char *path) {
    size_t i;
    for (i = 0; i < g_virt_dirs_cnt; i++) {
        if (g_virt_dirs[i][0] && !strcmp(g_virt_dirs[i], path)) return 1;
    }
    return 0;
}

static int is_temporary_name(const char *name) {
    size_t len;
    if (!name || !name[0]) return 1;
    if (name[0] == '.') return 1;
    len = strlen(name);
    if (len >= 4 && !strcmp(name + len - 4, ".tmp")) return 1;
    if (len >= 5 && !strcmp(name + len - 5, ".conf")) return 0;
    if (len >= 4 && !strcmp(name + len - 4, ".efi")) return 0;
    return 1;
}

/* The commit runs after close, so the writer cannot see a refusal: say it once. The daemon has no stderr, so syslog too. */
static void note_refused(staged_entry_t *e, int rc) {
    if (rc != RWY_ERR_NOSPC || e->refused_logged) return;
    e->refused_logged = 1;
    fprintf(stderr, "rwy-fuse: entry %s refused: no room (QUOTA, anchor or free space)\n", e->entry_name);
    syslog(LOG_WARNING, "entry %s refused: no room (QUOTA, anchor or free space)", e->entry_name);
}

static int try_commit_type1(rwy_store_t *s, staged_entry_t *e) {
    rwy_bls_entry_t parsed;
    int has_counter = 0;
    uint32_t tries_left = 3;
    int32_t tries_done = -1;
    char canon[RWY_ENTRY_MAX + 1];
    size_t k;
    int rc;

    if (!e->active || e->open_handles > 0 || !e->conf_buf || e->conf_len == 0) return 0;
    if (is_temporary_name(e->entry_name)) return 0;
    if (rwy_bls_parse(e->conf_buf, e->conf_len, &parsed) != RWY_OK) return 0;
    if (payload_is_open(parsed.linux_path)) return 0;
    for (k = 0; k < parsed.num_initrd; k++) {
        if (payload_is_open(parsed.initrd_paths[k])) return 0;
    }
    if (parsed.has_dtb && payload_is_open(parsed.dtb_path)) return 0;

    (void)rwy_bls_name_parse(e->entry_name, strlen(e->entry_name), canon, sizeof(canon),
                              &has_counter, &tries_left, &tries_done);
    virt_use_wall_clock(s);
    rc = rwy_bls_commit_type1(s, canon, e->conf_buf, e->conf_len,
                               has_counter ? tries_left : 3);
    if (rc == RWY_OK) {
        free(e->conf_buf);
        memset(e, 0, sizeof(*e));
    } else {
        note_refused(e, rc);
    }
    return rc;
}

static int try_commit_type2(rwy_store_t *s, staged_entry_t *e) {
    int has_counter = 0;
    uint32_t tries_left = 3;
    int32_t tries_done = -1;
    char canon[RWY_ENTRY_MAX + 1];
    int rc;

    if (!e->active || e->open_handles > 0 || !e->is_type2) return 0;
    if (is_temporary_name(e->entry_name)) return 0;
    (void)rwy_bls_name_parse(e->entry_name, strlen(e->entry_name), canon, sizeof(canon),
                              &has_counter, &tries_left, &tries_done);
    virt_use_wall_clock(s);
    rc = rwy_bls_commit_type2(s, canon, e->blob_path,
                               has_counter ? tries_left : 3);
    if (rc == RWY_OK) {
        virt_use_wall_clock(s);
        if (rwy_begin(s) == RWY_OK) {
            (void)rwy_unlink(s, e->blob_path);
            (void)rwy_commit(s);
        }
        memset(e, 0, sizeof(*e));
    } else {
        note_refused(e, rc);
    }
    return rc;
}

static void sweep_staged_type1(rwy_store_t *s) {
    size_t i;
    for (i = 0; i < MAX_STAGED_ENTRIES; i++) {
        if (g_staged[i].active && !g_staged[i].is_type2 && g_staged[i].open_handles == 0)
            try_commit_type1(s, &g_staged[i]);
    }
}

/* The .conf text of a Type 1 entry. */
static int render_conf(rwy_store_t *s, const rwy_bls_view_t *v, const char *file, char *conf, size_t conf_sz) {
    rwy_bls_slot_info_t info;
    char slot[300];
    int rc = rwy_bls_view_entry(v, 0, file, slot, sizeof(slot), NULL);
    if (rc) return rc;
    rc = rwy_bls_read_slot_info(s, slot, &info);
    if (rc) return rc;
    return rwy_bls_render_type1(&info, conf, conf_sz);
}

static int is_blob_dir(const rwy_bls_view_t *v, const char *path) {
    return is_virt_dir(path) || rwy_bls_view_is_dir(v, path);
}

static int getattr_static(const char *path, struct stat *st) {
    if (!strcmp(path, "/loader") || !strcmp(path, "/loader/entries") ||
        !strcmp(path, "/EFI/Linux")) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFDIR | 0777; st->st_nlink = 2; st->st_blksize = RWY_BLOCK_SIZE;
        st->st_uid = fuse_get_context()->uid;
        st->st_gid = fuse_get_context()->gid;
        return 0;
    }
    if (!strcmp(path, "/loader/entries.srel")) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFREG | 0444; st->st_nlink = 1; st->st_size = 6;
        st->st_blksize = RWY_BLOCK_SIZE;
        st->st_uid = fuse_get_context()->uid;
        st->st_gid = fuse_get_context()->gid;
        return 0;
    }
    return -ENOENT;
}

static int getattr_staged(rwy_store_t *s, const char *path, struct stat *st) {
    staged_entry_t *stg = find_staged_by_path(path);
    rwy_attr_t a;
    if (!stg) return -ENOENT;
    if (!stg->is_type2) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFREG | 0644; st->st_nlink = 1; st->st_size = (off_t)stg->conf_len;
        st->st_blksize = RWY_BLOCK_SIZE;
        st->st_uid = fuse_get_context()->uid;
        st->st_gid = fuse_get_context()->gid;
        return 0;
    }
    if (rwy_getattr_path(s, stg->blob_path, &a) == RWY_OK) {
        fill_stbuf(&a, st);
        return 0;
    }
    return -ENOENT;
}

static int getattr_entries(rwy_store_t *s, const rwy_bls_view_t *v, const char *path, struct stat *st) {
    char target[300];
    int conf_len;
    rwy_attr_t a;
    if (!strncmp(path, "/loader/entries/", 16)) {
        if (rwy_bls_view_entry(v, 0, path + 16, NULL, 0, &conf_len) == RWY_OK && conf_len >= 0) {
            memset(st, 0, sizeof(*st));
            st->st_mode = S_IFREG | 0444; st->st_nlink = 1; st->st_size = (off_t)conf_len;
            st->st_blksize = RWY_BLOCK_SIZE;
            return 0;
        }
        return -ENOENT;
    }
    if (!strncmp(path, "/EFI/Linux/", 11)) {
        if (rwy_bls_view_entry(v, 1, path + 11, target, sizeof(target), NULL) == RWY_OK) {
            char kpath[320];
            snprintf(kpath, sizeof(kpath), "%.300s/KERNEL", target);
            if (rwy_getattr_path(s, kpath, &a) == RWY_OK) { fill_stbuf(&a, st); return 0; }
        }
        return -ENOENT;
    }
    return -ENOENT;
}

int rwy_fuse_virt_getattr(rwy_store_t *s, const char *path, struct stat *st) {
    const rwy_bls_view_t *v;
    char target[300];
    rwy_attr_t a;

    if (getattr_static(path, st) == 0) return 0;
    if (getattr_staged(s, path, st) == 0) return 0;
    int err;
    v = view_or_err(s, path, &err);
    if (!v) return err;
    if (getattr_entries(s, v, path, st) == 0) return 0;

    if (rwy_bls_view_resolve(v, path, target, sizeof(target)) == RWY_OK) {
        if (rwy_getattr_path(s, target, &a) == RWY_OK) { fill_stbuf(&a, st); return 0; }
    }
    if (is_blob_dir(v, path)) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFDIR | 0777; st->st_nlink = 2; st->st_blksize = RWY_BLOCK_SIZE;
        st->st_uid = fuse_get_context()->uid;
        st->st_gid = fuse_get_context()->gid;
        return 0;
    }
    return -ENOENT;
}

static void readdir_virt_dirs(const char *path, void *buf, fuse_fill_dir_t filler) {
    size_t i;
    for (i = 0; i < g_virt_dirs_cnt; i++) {
        size_t plen = strlen(path);
        if (g_virt_dirs[i][0] && !strncmp(g_virt_dirs[i], path, plen) && g_virt_dirs[i][plen] == '/') {
            const char *sub = g_virt_dirs[i] + plen + 1;
            char item[64];
            const char *slash = strchr(sub, '/');
            if (slash) {
                size_t ilen = (size_t)(slash - sub);
                if (ilen >= sizeof(item)) ilen = sizeof(item) - 1;
                memcpy(item, sub, ilen); item[ilen] = 0;
            } else {
                snprintf(item, sizeof(item), "%s", sub);
            }
            filler(buf, item, NULL, 0, 0);
        }
    }
}

int rwy_fuse_virt_readdir(rwy_store_t *s, const char *path, void *buf,
                           fuse_fill_dir_t filler) {
    const rwy_bls_view_t *v;
    fill_ctx_t c = { buf, filler };
    if (!strcmp(path, "/loader")) {
        filler(buf, ".", NULL, 0, 0);
        filler(buf, "..", NULL, 0, 0);
        filler(buf, "entries", NULL, 0, 0);
        filler(buf, "entries.srel", NULL, 0, 0);
        return 0;
    }
    int err;
    v = view_or_err(s, path, &err);
    if (!v) return err;
    if (!strcmp(path, "/loader/entries")) {
        filler(buf, ".", NULL, 0, 0);
        filler(buf, "..", NULL, 0, 0);
        rwy_bls_view_entries(v, 0, fill_name, &c);
        return 0;
    }
    if (!strcmp(path, "/EFI/Linux")) {
        filler(buf, ".", NULL, 0, 0);
        filler(buf, "..", NULL, 0, 0);
        rwy_bls_view_entries(v, 1, fill_name, &c);
        return 0;
    }
    if (is_blob_dir(v, path)) {
        filler(buf, ".", NULL, 0, 0);
        filler(buf, "..", NULL, 0, 0);
        rwy_bls_view_blob_children(v, path, fill_name, &c);
        readdir_virt_dirs(path, buf, filler);
        rwy_bls_view_slot_children(v, path, fill_name, &c);
        return 0;
    }
    return -ENOENT;
}

/* List a virtual directory unless the store already has a real one. */
static void fill_virt_dir(rwy_store_t *s, const char *real, const char *name,
                          void *buf, fuse_fill_dir_t filler) {
    uint64_t oid;
    if (rwy_lookup(s, real, &oid) != RWY_OK) filler(buf, name, NULL, 0, 0);
}

int rwy_fuse_virt_readdir_extra(rwy_store_t *s, const char *path, void *buf,
                                 fuse_fill_dir_t filler) {
    if (!strcmp(path, "/")) {
        const rwy_bls_view_t *v = view_of(s);
        fill_ctx_t c = { buf, filler };
        if (!v) return -EAGAIN;
        fill_virt_dir(s, "loader", "loader", buf, filler);
        fill_virt_dir(s, "EFI", "EFI", buf, filler);
        rwy_bls_view_roots(v, fill_name, &c);
        return 0;
    }
    if (!strcmp(path, "/EFI")) {
        fill_virt_dir(s, "EFI/Linux", "Linux", buf, filler);
        return 0;
    }
    return -ENOENT;
}

static int open_bls_entries(rwy_store_t *s, const rwy_bls_view_t *v, const char *path, struct fuse_file_info *fi) {
    char slot[300], target[300];
    uint64_t oid;
    if (!strncmp(path, "/loader/entries/", 16)) {
        if (!strcmp(path + 16, "") || strchr(path + 16, '/')) return -ENOENT;
        if (rwy_bls_view_entry(v, 0, path + 16, slot, sizeof(slot), NULL) == RWY_OK) {
            fi->fh = RWY_FH_VIRT_CONF;
            return 0;
        }
        return -ENOENT;
    }
    if (!strncmp(path, "/EFI/Linux/", 11)) {
        if (rwy_bls_view_entry(v, 1, path + 11, target, sizeof(target), NULL) == RWY_OK) {
            char kpath[320];
            snprintf(kpath, sizeof(kpath), "%.300s/KERNEL", target);
            if (rwy_lookup(s, kpath, &oid) == RWY_OK) { fi->fh = oid; return 0; }
        }
        return -ENOENT;
    }
    return -ENOENT;
}

static int open_payload(rwy_store_t *s, const rwy_bls_view_t *v, const char *path, struct fuse_file_info *fi) {
    char target[300];
    uint64_t oid;
    if (rwy_bls_view_resolve(v, path, target, sizeof(target)) == RWY_OK) {
        if (rwy_lookup(s, target, &oid) == RWY_OK) {
            if ((fi->flags & O_TRUNC)) {
                virt_use_wall_clock(s);
                if (rwy_begin(s) == RWY_OK) {
                    (void)rwy_truncate_oid(s, oid, 0);
                    (void)rwy_commit(s);
                }
            }
            payload_inc(path, oid);
            fi->fh = oid;
            return 0;
        }
    }
    return -ENOENT;
}

int rwy_fuse_virt_open(rwy_store_t *s, const char *path, struct fuse_file_info *fi) {
    const rwy_bls_view_t *v;
    staged_entry_t *stg;

    if (!strcmp(path, "/loader/entries.srel")) {
        fi->fh = RWY_FH_VIRT_SREL;
        return 0;
    }
    stg = find_staged_by_path(path);
    if (stg && !stg->is_type2) {
        if ((fi->flags & O_ACCMODE) != O_RDONLY && (fi->flags & O_TRUNC)) {
            stg->conf_len = 0;
        }
        stg->open_handles++;
        fi->fh = RWY_FH_STAGED_BASE + (uint64_t)(stg - g_staged);
        return 0;
    }
    int err;
    v = view_or_err(s, path, &err);
    if (!v) return err;
    if (open_bls_entries(s, v, path, fi) == 0) return 0;
    return open_payload(s, v, path, fi);
}

/* Read from the .conf of a Type 1 entry. */
static int read_conf(rwy_store_t *s, const char *entry, char *buf, size_t size, off_t off) {
    const rwy_bls_view_t *v = view_of(s);
    char conf[4096];
    size_t len;
    if (!v) return -EAGAIN;
    if (render_conf(s, v, entry, conf, sizeof(conf)) < 0) return -EIO;
    len = strlen(conf);
    if (off >= (off_t)len) return 0;
    if (size > len - (size_t)off) size = len - (size_t)off;
    memcpy(buf, conf + off, size);
    return (int)size;
}

int rwy_fuse_virt_read(rwy_store_t *s, const char *path, char *buf,
                        size_t size, off_t off, struct fuse_file_info *fi) {
    if (fi->fh == RWY_FH_VIRT_SREL) {
        const char *srel = "type1\n";
        size_t len = 6;
        if (off >= (off_t)len) return 0;
        if (size > len - (size_t)off) size = len - (size_t)off;
        memcpy(buf, srel + off, size);
        return (int)size;
    }
    if (fi->fh >= RWY_FH_STAGED_BASE && fi->fh < RWY_FH_STAGED_BASE + MAX_STAGED_ENTRIES) {
        staged_entry_t *e = &g_staged[fi->fh - RWY_FH_STAGED_BASE];
        if (!e->active || !e->conf_buf || off >= (off_t)e->conf_len) return 0;
        size_t avail = e->conf_len - (size_t)off;
        if (size > avail) size = avail;
        memcpy(buf, e->conf_buf + off, size);
        return (int)size;
    }
    if (fi->fh == RWY_FH_VIRT_CONF && !strncmp(path, "/loader/entries/", 16))
        return read_conf(s, path + 16, buf, size, off);
    return -EIO;
}

static int create_type1_staged(const char *path, struct fuse_file_info *fi) {
    staged_entry_t *e = alloc_staged(path, path + 16, 0);
    if (!e) return -ENOSPC;
    e->open_handles++;
    fi->fh = RWY_FH_STAGED_BASE + (uint64_t)(e - g_staged);
    return 0;
}

static int create_type2_staged(rwy_store_t *s, const char *path, struct fuse_file_info *fi) {
    uint64_t oid;
    int rc;
    staged_entry_t *e = alloc_staged(path, path + 11, 1);
    if (!e) return -ENOSPC;
    snprintf(e->blob_path, sizeof(e->blob_path),
             "EFI/RUNWAY/BLOBS/staged_uki_%lu.efi", (unsigned long)(e - g_staged));
    virt_use_wall_clock(s);
    rc = rwy_begin(s);
    if (rc != RWY_OK) return err_of(rc);
    (void)rwy_mkdir(s, "EFI/RUNWAY/BLOBS", RWY_S_IFDIR | 0755, 0, 0);
    rc = rwy_create(s, e->blob_path, RWY_S_IFREG | 0644, 0, 0);
    if (rc != RWY_OK && rc != RWY_ERR_EXIST) { rwy_abort(s); return err_of(rc); }
    rc = rwy_commit(s);
    if (rc != RWY_OK) return err_of(rc);
    rc = rwy_lookup(s, e->blob_path, &oid);
    if (rc != RWY_OK) return err_of(rc);
    e->open_handles++;
    fi->fh = oid;
    return 0;
}

static int create_payload_blob(rwy_store_t *s, const char *path, struct fuse_file_info *fi) {
    char enc[256], bpath[300];
    uint64_t oid;
    int rc;
    rc = rwy_bls_encode_blob_name(path, enc, sizeof(enc));
    if (rc <= 0) return rc == RWY_ERR_PERM ? -EPERM : -EINVAL;
    snprintf(bpath, sizeof(bpath), "EFI/RUNWAY/BLOBS/%s", enc);
    virt_use_wall_clock(s);
    rc = rwy_begin(s);
    if (rc != RWY_OK) return err_of(rc);
    (void)rwy_mkdir(s, "EFI/RUNWAY/BLOBS", RWY_S_IFDIR | 0755, 0, 0);
    rc = rwy_create(s, bpath, RWY_S_IFREG | 0644, 0, 0);
    if (rc != RWY_OK && rc != RWY_ERR_EXIST) { rwy_abort(s); return err_of(rc); }
    rc = rwy_commit(s);
    if (rc != RWY_OK) return err_of(rc);
    rc = rwy_lookup(s, bpath, &oid);
    if (rc != RWY_OK) return err_of(rc);
    payload_inc(path, oid);
    fi->fh = oid;
    return 0;
}

int rwy_fuse_virt_create(rwy_store_t *s, const char *path, mode_t mode,
                          struct fuse_file_info *fi) {
    (void)mode;
    if (!strncmp(path, "/loader/entries/", 16)) return create_type1_staged(path, fi);
    if (!strncmp(path, "/EFI/Linux/", 11)) return create_type2_staged(s, path, fi);
    if (is_payload_path(path)) return create_payload_blob(s, path, fi);
    return -ENOENT;
}

int rwy_fuse_virt_write(rwy_store_t *s, const char *path, const char *buf,
                         size_t size, off_t off, struct fuse_file_info *fi) {
    (void)s; (void)path;
    if (fi->fh >= RWY_FH_STAGED_BASE && fi->fh < RWY_FH_STAGED_BASE + MAX_STAGED_ENTRIES) {
        staged_entry_t *e = &g_staged[fi->fh - RWY_FH_STAGED_BASE];
        if (!e->active) return -EBADF;
        if (off + size > 65536) return -EFBIG;
        if (off + size > e->conf_cap) {
            size_t ncap = (off + size + 4095) & ~4095ULL;
            char *nbuf = realloc(e->conf_buf, ncap);
            if (!nbuf) return -ENOMEM;
            e->conf_buf = nbuf;
            e->conf_cap = ncap;
        }
        memcpy(e->conf_buf + off, buf, size);
        if (off + size > e->conf_len) e->conf_len = off + size;
        return (int)size;
    }
    return -ENOENT;
}

static int count_slots(rwy_store_t *s) {
    char name[256];
    size_t i = 0;
    int count = 0;
    while (rwy_readdir_nth(s, "EFI/RUNWAY", i++, name, sizeof(name), NULL) == RWY_OK) {
        if (strncmp(name, "CONFIG", 6) == 0 && name[6] >= '0' && name[6] <= '9') {
            count++;
        }
    }
    return count;
}

int rwy_fuse_virt_flush(rwy_store_t *s, const char *path, struct fuse_file_info *fi) {
    (void)path;
    if (fi->fh >= RWY_FH_STAGED_BASE && fi->fh < RWY_FH_STAGED_BASE + MAX_STAGED_ENTRIES) {
        staged_entry_t *e = &g_staged[fi->fh - RWY_FH_STAGED_BASE];
        if (e->active && e->conf_buf && e->conf_len > 0) {
            int rc = rwy_bls_check_definite_faults(e->conf_buf, e->conf_len);
            if (rc != RWY_OK) return -EINVAL;
            if (count_slots(s) >= 64) return -ENOSPC;
        }
    }
    return 0;
}

int rwy_fuse_virt_release(rwy_store_t *s, const char *path, struct fuse_file_info *fi) {
    size_t i;
    (void)path;
    if (fi->fh >= RWY_FH_STAGED_BASE && fi->fh < RWY_FH_STAGED_BASE + MAX_STAGED_ENTRIES) {
        staged_entry_t *e = &g_staged[fi->fh - RWY_FH_STAGED_BASE];
        if (e->active) {
            if (--e->open_handles <= 0) {
                e->open_handles = 0;
                try_commit_type1(s, e);
            }
        }
        return 0;
    }
    payload_dec_by_oid(fi->fh);
    for (i = 0; i < MAX_STAGED_ENTRIES; i++) {
        if (g_staged[i].active && g_staged[i].is_type2) {
            uint64_t oid;
            if (rwy_lookup(s, g_staged[i].blob_path, &oid) == RWY_OK && oid == fi->fh) {
                if (--g_staged[i].open_handles <= 0) {
                    g_staged[i].open_handles = 0;
                    try_commit_type2(s, &g_staged[i]);
                }
                break;
            }
        }
    }
    sweep_staged_type1(s);
    return 0;
}

static int check_rename_noreplace(const rwy_bls_view_t *v, const char *to) {
    int is_t2 = !strncmp(to, "/EFI/Linux/", 11);
    if (find_staged_by_path(to)) return -EEXIST;
    if (!is_t2 && strncmp(to, "/loader/entries/", 16)) return 0;
    return rwy_bls_view_entry(v, is_t2, to + (is_t2 ? 11 : 16), NULL, 0, NULL) == RWY_OK ? -EEXIST : 0;
}

static int apply_bless_rename(rwy_store_t *s, const char *slot_dir, int target_state) {
    char state_path[320];
    int rc;
    virt_use_wall_clock(s);
    rc = rwy_begin(s);
    if (rc != RWY_OK) return err_of(rc);
    snprintf(state_path, sizeof(state_path), "%s/STATE", slot_dir);
    if (target_state == RWY_BLS_STATE_OK) {
        rc = rwy_write_path(s, state_path, 0, "OK\n", 3);
    } else if (target_state == RWY_BLS_STATE_FAILED) {
        rc = rwy_write_path(s, state_path, 0, "FAILED\n", 7);
    } else {
        rc = RWY_ERR_PERM;
    }
    if (rc == RWY_OK) rc = rwy_commit(s);
    else rwy_abort(s);
    return err_of(rc);
}

static int rename_staged(rwy_store_t *s, const rwy_bls_view_t *v, staged_entry_t *e, const char *to, unsigned flags) {
    if (flags & 1) {
        int chk = check_rename_noreplace(v, to);
        if (chk != 0) return chk;
    }
    if (!strncmp(to, "/loader/entries/", 16)) {
        snprintf(e->entry_name, sizeof(e->entry_name), "%s", to + 16);
        snprintf(e->target_path, sizeof(e->target_path), "%s", to);
        if (e->open_handles == 0) try_commit_type1(s, e);
        return 0;
    }
    if (!strncmp(to, "/EFI/Linux/", 11)) {
        snprintf(e->entry_name, sizeof(e->entry_name), "%s", to + 11);
        snprintf(e->target_path, sizeof(e->target_path), "%s", to);
        if (e->open_handles == 0) try_commit_type2(s, e);
        return 0;
    }
    return -EPERM;
}

static int rename_type1_bless(rwy_store_t *s, const rwy_bls_view_t *v, const char *from, const char *to, unsigned flags) {
    char slot_dir[300];
    int target_state = 0;
    if (strncmp(from, "/loader/entries/", 16) != 0) return -ENOENT;
    if (rwy_bls_view_entry(v, 0, from + 16, slot_dir, sizeof(slot_dir), NULL) != RWY_OK) return -ENOENT;
    if (strncmp(to, "/loader/entries/", 16) != 0) return -EPERM;
    if ((flags & 1) && check_rename_noreplace(v, to)) return -EEXIST;
    if (rwy_bls_name_match_rename(from + 16, strlen(from + 16),
                                   to + 16, strlen(to + 16),
                                   &target_state) != RWY_OK)
        return -EPERM;
    return apply_bless_rename(s, slot_dir, target_state);
}

static int rename_type2_bless(rwy_store_t *s, const rwy_bls_view_t *v, const char *from, const char *to, unsigned flags) {
    char slot_dir[300];
    int target_state = 0;
    if (strncmp(from, "/EFI/Linux/", 11) != 0) return -ENOENT;
    if (rwy_bls_view_entry(v, 1, from + 11, slot_dir, sizeof(slot_dir), NULL) != RWY_OK) return -ENOENT;
    if (strncmp(to, "/EFI/Linux/", 11) != 0) return -EPERM;
    if ((flags & 1) && check_rename_noreplace(v, to)) return -EEXIST;
    if (rwy_bls_name_match_rename(from + 11, strlen(from + 11),
                                   to + 11, strlen(to + 11),
                                   &target_state) != RWY_OK)
        return -EPERM;
    return apply_bless_rename(s, slot_dir, target_state);
}

static int rename_payload_blob(rwy_store_t *s, const char *from, const char *to) {
    char enc_from[256], enc_to[256], b_from[300], b_to[300];
    int rc;
    if (!is_payload_path(from) || !is_payload_path(to)) return -ENOENT;
    rc = rwy_bls_encode_blob_name(from, enc_from, sizeof(enc_from));
    if (rc > 0) rc = rwy_bls_encode_blob_name(to, enc_to, sizeof(enc_to));
    if (rc <= 0) return rc == RWY_ERR_PERM ? -EPERM : -ENOENT;
    snprintf(b_from, sizeof(b_from), "EFI/RUNWAY/BLOBS/%s", enc_from);
    snprintf(b_to, sizeof(b_to), "EFI/RUNWAY/BLOBS/%s", enc_to);
    virt_use_wall_clock(s);
    rc = rwy_begin(s);
    if (rc != RWY_OK) return err_of(rc);
    rc = rwy_rename(s, b_from, b_to);
    if (rc == RWY_OK) rc = rwy_commit(s);
    else rwy_abort(s);
    return err_of(rc);
}

int rwy_fuse_virt_rename_stage(rwy_store_t *s, const char *from, const char *to, unsigned flags) {
    const rwy_bls_view_t *v = view_of(s);
    staged_entry_t *e = find_staged_by_path(from);
    int rc;
    if (!v) return -EAGAIN;
    if (e) return rename_staged(s, v, e, to, flags);
    rc = rename_type1_bless(s, v, from, to, flags);
    if (rc != -ENOENT) return rc;
    rc = rename_type2_bless(s, v, from, to, flags);
    if (rc != -ENOENT) return rc;
    return rename_payload_blob(s, from, to);
}

int rwy_fuse_virt_truncate(rwy_store_t *s, const char *path, off_t size) {
    (void)s;
    staged_entry_t *stg = find_staged_by_path(path);
    if (stg) {
        if (!stg->is_type2) {
            if (size == 0) {
                stg->conf_len = 0;
                return 0;
            }
            if (size <= (off_t)stg->conf_cap) {
                stg->conf_len = (size_t)size;
                return 0;
            }
        }
    }
    return -ENOENT;
}

int rwy_fuse_virt_unlink(rwy_store_t *s, const char *path) {
    const rwy_bls_view_t *v;
    staged_entry_t *stg = find_staged_by_path(path);
    if (stg) {
        if (stg->conf_buf) free(stg->conf_buf);
        if (stg->is_type2 && stg->blob_path[0]) {
            virt_use_wall_clock(s);
            if (rwy_begin(s) == RWY_OK) {
                (void)rwy_unlink(s, stg->blob_path);
                (void)rwy_commit(s);
            }
        }
        memset(stg, 0, sizeof(*stg));
        return 0;
    }
    int err;
    v = view_or_err(s, path, &err);
    if (!v) return err;
    if (!strncmp(path, "/loader/entries/", 16)) {
        char slot[300];
        if (rwy_bls_view_entry(v, 0, path + 16, slot, sizeof(slot), NULL) == RWY_OK) {
            virt_use_wall_clock(s);
            return err_of(rwy_bls_remove_slot(s, slot));
        }
        return -ENOENT;
    }
    if (!strncmp(path, "/EFI/Linux/", 11)) {
        char slot[300];
        if (rwy_bls_view_entry(v, 1, path + 11, slot, sizeof(slot), NULL) == RWY_OK) {
            virt_use_wall_clock(s);
            return err_of(rwy_bls_remove_slot(s, slot));
        }
        return -ENOENT;
    }
    char target[300];
    if (rwy_bls_view_resolve(v, path, target, sizeof(target)) == RWY_OK) {
        virt_use_wall_clock(s);
        int rc = rwy_begin(s);
        if (rc == RWY_OK) {
            rc = rwy_unlink(s, target);
            if (rc == RWY_OK) (void)rwy_commit(s);
            else rwy_abort(s);
            return err_of(rc);
        }
        return err_of(rc);
    }
    return -ENOENT;
}

int rwy_fuse_virt_mkdir(rwy_store_t *s, const char *path, mode_t mode) {
    (void)s; (void)mode;
    if (is_payload_path(path)) {
        add_virt_dir(path);
        return 0;
    }
    return -ENOENT;
}

int rwy_fuse_virt_rmdir(rwy_store_t *s, const char *path) {
    (void)s;
    if (!strcmp(path, "/loader") || !strcmp(path, "/loader/entries") ||
        !strcmp(path, "/EFI/Linux") || !strcmp(path, "/EFI"))
        return -EBUSY;
    if (is_payload_path(path)) {
        remove_virt_dir(path);
        return 0;
    }
    return -ENOENT;
}

int rwy_fuse_virt_resolve_path(rwy_store_t *s, const char *path, char *out, size_t out_sz) {
    const rwy_bls_view_t *v;
    if (!path) return -ENOENT;
    if (!strncmp(path, "/loader", 7) || !strncmp(path, "/EFI/Linux", 10))
        return 1;
    int err;
    v = view_or_err(s, path, &err);
    if (!v) return err;
    if (rwy_bls_view_resolve(v, path, out, out_sz) == RWY_OK)
        return 0;
    if (is_payload_path(path) || is_blob_dir(v, path))
        return 0;
    return -ENOENT;
}

