/*
 * rwy_ns.c — Namespace and metadata operations: create, link, rename, attributes.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

int rwy_getattr(rwy_store_t *s, uint64_t oid, rwy_attr_t *out) {
    const rwy_inode_t *in;
    if (!s || !out) return RWY_ERR_INVAL;
    if (oid & RWY_OID_VIRT) return rwy_virt_attr(s, oid, out);
    in = rwy_inode_of(s, oid, s->in_txn);
    if (!in) return RWY_ERR_NOENT;
    rwy_fill_attr(in, out);
    return RWY_OK;
}

int rwy_stat(rwy_store_t *s, const char *path, rwy_attr_t *out) {
    uint64_t o;
    int rc;
    if (!s || !path || !out) return RWY_ERR_INVAL;
    rc = rwy_resolve_ext(s, path, s->in_txn, 1, &o);
    if (rc) return rc;
    return rwy_getattr(s, o, out);
}

int rwy_lstat(rwy_store_t *s, const char *path, rwy_attr_t *out) {
    uint64_t o;
    int rc;
    if (!s || !path || !out) return RWY_ERR_INVAL;
    rc = rwy_resolve_ext(s, path, s->in_txn, 0, &o);
    if (rc) return rc;
    return rwy_getattr(s, o, out);
}

int rwy_getattr_path(rwy_store_t *s, const char *path, rwy_attr_t *out) {
    return rwy_lstat(s, path, out);
}

int rwy_need_txn(const rwy_store_t *s) {
    return s->in_txn ? RWY_OK : RWY_ERR_NOTXN;
}

int rwy_create(rwy_store_t *s, const char *path, uint32_t mode,
                uint32_t uid, uint32_t gid) {
    uint64_t parent, oid;
    const char *base;
    uint32_t blen;
    rwy_inode_t *in;
    int rc, done;
    if (!s || !path) return RWY_ERR_INVAL;
    rc = rwy_need_txn(s);
    if (rc) return rc;
    if (!(mode & RWY_S_IFMT)) mode |= RWY_S_IFREG;
    rc = rwy_resolve_parent(s, path, 1, &parent, &base, &blen);
    if (rc) return rc;
    rc = rwy_virt_create(s, parent, base, blen, RWY_TYPE_FILE, &done);
    if (rc || done) return rc;
    rc = rwy_bootcfg_check_create(s, parent, base, blen, RWY_TYPE_FILE, 0, &mode, &uid, &gid);
    if (rc) return rc;
    in = rwy_alloc_inode(s, &oid);
    if (!in) return RWY_ERR_NOSPC;
    rwy_inode_init(s, in, oid, RWY_TYPE_FILE, mode, uid, gid);
    rc = rwy_parent_insert(s, parent, oid, RWY_TYPE_FILE, base, blen);
    if (rc) { in->type = RWY_TYPE_FREE; in->oid = 0; return rc; }
    return RWY_OK;
}

int rwy_mkdir(rwy_store_t *s, const char *path, uint32_t mode,
               uint32_t uid, uint32_t gid) {
    uint64_t parent, oid;
    const char *base;
    uint32_t blen;
    rwy_inode_t *in;
    int rc;
    if (!s || !path) return RWY_ERR_INVAL;
    rc = rwy_need_txn(s);
    if (rc) return rc;
    if (!(mode & RWY_S_IFMT)) mode |= RWY_S_IFDIR;
    rc = rwy_resolve_parent(s, path, 1, &parent, &base, &blen);
    if (rc) return rc;
    rc = rwy_bootcfg_check_create(s, parent, base, blen, RWY_TYPE_DIR, 0, &mode, &uid, &gid);
    if (rc) return rc;
    in = rwy_alloc_inode(s, &oid);
    if (!in) return RWY_ERR_NOSPC;
    rwy_inode_init(s, in, oid, RWY_TYPE_DIR, mode, uid, gid);
    rc = rwy_parent_insert(s, parent, oid, RWY_TYPE_DIR, base, blen);
    if (rc) { in->type = RWY_TYPE_FREE; in->oid = 0; return rc; }
    return rwy_bootcfg_mkdir_done(s, oid);
}

int rwy_symlink(rwy_store_t *s, const char *path, const char *target,
                 uint32_t uid, uint32_t gid) {
    uint64_t parent, oid;
    const char *base;
    uint32_t blen;
    rwy_inode_t *in;
    size_t tl;
    int rc;
    if (!s || !path || !target) return RWY_ERR_INVAL;
    rc = rwy_need_txn(s);
    if (rc) return rc;
    rc = rwy_resolve_parent(s, path, 1, &parent, &base, &blen);
    if (rc) return rc;
    rc = rwy_bootcfg_check_create(s, parent, base, blen, RWY_TYPE_SYMLINK, 0, 0, &uid, &gid);
    if (rc) return rc;
    in = rwy_alloc_inode(s, &oid);
    if (!in) return RWY_ERR_NOSPC;
    rwy_inode_init(s, in, oid, RWY_TYPE_SYMLINK, RWY_S_IFLNK | 0777u, uid, gid);
    tl = rwy_strlen(target);
    if (tl > 4096) { in->type = RWY_TYPE_FREE; in->oid = 0; return RWY_ERR_NAMETOOLONG; }
    rc = rwy_cow_write(s, in, 0, (const uint8_t *)target, tl);
    if (rc) { in->type = RWY_TYPE_FREE; in->oid = 0; return rc; }
    rc = rwy_parent_insert(s, parent, oid, RWY_TYPE_SYMLINK, base, blen);
    if (rc) {
        rwy_winode_free_data(s, in);
        in->type = RWY_TYPE_FREE; in->oid = 0;
        return rc;
    }
    return RWY_OK;
}

int rwy_readlink(rwy_store_t *s, const char *path, char *out, size_t outsz) {
    uint64_t o;
    const rwy_inode_t *in;
    size_t got = 0;
    int rc;
    if (!s || !path || !out || !outsz) return RWY_ERR_INVAL;
    rc = rwy_resolve(s, path, s->in_txn, &o);
    if (rc) return rc;
    if (o & RWY_OID_VIRT) return rwy_virt_readlink(s, o, out, outsz);
    in = rwy_inode_of(s, o, s->in_txn);
    if (!in || in->type != RWY_TYPE_SYMLINK) return RWY_ERR_INVAL;
    if (in->size + 1 > outsz) return RWY_ERR_NAMETOOLONG;
    rc = rwy_inode_pread(s, in, 0, out, (size_t)in->size, &got);
    if (rc) return rc;
    out[got] = 0;
    return RWY_OK;
}

static int free_inode_data(rwy_store_t *s, uint64_t oid) {
    rwy_inode_t *in = rwy_winode_of(s, oid);
    uint32_t i;
    if (!in) return RWY_ERR_NOENT;
    rwy_winode_free_data(s, in);
    if (in->xattr_root) {
        uint32_t xb = (uint32_t)in->xattr_root;
        uint32_t guard = 0;
        while (xb != 0 && guard++ < 64) {
            uint8_t blk[RWY_BLOCK_SIZE];
            const rwy_xattr_header_t *xh;
            rwy_w_free(s, xb);
            if (rwy_blk_read(&s->dev, RWY_IO_META, xb, blk)) break;
            xh = (const rwy_xattr_header_t *)blk;
            if (xh->hdr.magic != RWY_BLK_MAGIC || xh->hdr.kind != RWY_BLK_KIND_XATTR) break;
            xb = xh->next;
        }
        in->xattr_root = 0;
    }
    for (i = 0; i < sizeof(*in); i++) ((uint8_t *)in)[i] = 0;
    in->type = RWY_TYPE_FREE;
    return RWY_OK;
}

static int drop_inode_link(rwy_store_t *s, uint64_t oid) {
    rwy_inode_t *in = rwy_winode_of(s, oid);
    if (!in) return RWY_ERR_NOENT;
    if (in->nlink > 1) {
        int64_t sec; uint32_t ns;
        in->nlink--;
        if (in->nlink == 1) s->bc_sweep = 1;
        rwy_now_of(s, &sec, &ns);
        in->ctime_sec = sec;
        in->ctime_nsec = ns;
        in->generation = s->w_generation;
        return RWY_OK;
    }
    in->nlink = 0;
    return free_inode_data(s, oid);
}

int rwy_link(rwy_store_t *s, const char *oldpath, const char *newpath) {
    uint64_t old_oid, new_parent;
    const char *base;
    uint32_t blen;
    rwy_inode_t *old_in, *p;
    int rc;

    if (!s || !oldpath || !newpath) return RWY_ERR_INVAL;
    rc = rwy_need_txn(s);
    if (rc) return rc;

    rc = rwy_resolve_ext(s, oldpath, 1, 0, &old_oid);
    if (rc) return rc;

    rc = rwy_mut_inode(s, old_oid, &old_in);
    if (rc) return rc;
    if (old_in->type == RWY_TYPE_DIR) return RWY_ERR_INVAL;

    rc = rwy_resolve_parent(s, newpath, 1, &new_parent, &base, &blen);
    if (rc) return rc;

    p = rwy_winode_of(s, new_parent);
    if (!p || p->type != RWY_TYPE_DIR) return RWY_ERR_NOTDIR;
    rc = rwy_bootcfg_check_link(s, old_oid, new_parent, base, blen);
    if (rc) return rc;

    rc = rwy_parent_insert(s, new_parent, old_oid, old_in->type, base, blen);
    if (rc) return rc;

    old_in->nlink++;
    {
        int64_t sec; uint32_t ns;
        rwy_now_of(s, &sec, &ns);
        old_in->ctime_sec = sec;
        old_in->ctime_nsec = ns;
    }
    old_in->generation = s->w_generation;
    return RWY_OK;
}

static int unlink_path(rwy_store_t *s, const char *path) {
    uint64_t parent, child = 0;
    const char *base;
    uint32_t blen;
    const rwy_inode_t *ci;
    rwy_inode_t *w;
    int rc;
    if (!s || !path) return RWY_ERR_INVAL;
    rc = rwy_need_txn(s);
    if (rc) return rc;
    rc = rwy_resolve_parent(s, path, 1, &parent, &base, &blen);
    if (rc) return rc;
    /* peek type: do not follow terminal symlink */
    {
        uint64_t o;
        rc = rwy_resolve_ext(s, path, 1, 0, &o);
        if (rc) return rc;
        if (o & RWY_OID_VIRT) return rwy_virt_unlink(s, o);
        rc = rwy_mut_inode(s, o, &w);
        if (rc) return rc;
        ci = w;
        if (ci->type == RWY_TYPE_DIR) return RWY_ERR_ISDIR;
        child = o;
    }
    rc = rwy_bootcfg_check_remove(s, parent, base, blen);
    if (rc) return rc < 0 ? rc : RWY_OK;
    rc = rwy_parent_remove(s, parent, base, blen, 0);
    if (rc) return rc;
    return drop_inode_link(s, child);
}

static int rmdir_path(rwy_store_t *s, const char *path) {
    uint64_t parent, child = 0;
    const char *base;
    uint32_t blen;
    const rwy_inode_t *ci;
    rwy_inode_t *w;
    int rc;
    if (!s || !path) return RWY_ERR_INVAL;
    rc = rwy_need_txn(s);
    if (rc) return rc;
    rc = rwy_resolve_parent(s, path, 1, &parent, &base, &blen);
    if (rc) return rc;
    {
        uint64_t o;
        rc = rwy_resolve(s, path, 1, &o);
        if (rc) return rc;
        rc = rwy_mut_inode(s, o, &w);
        if (rc) return rc;
        ci = w;
        if (ci->type != RWY_TYPE_DIR) return RWY_ERR_NOTDIR;
        rc = rwy_bootcfg_check_remove(s, parent, base, blen);
        if (rc) return rc;
        rc = rwy_bootcfg_empty_slot(s, o, path);
        if (rc) return rc;
        rc = rwy_mut_inode(s, o, &w);
        if (rc) return rc;
        ci = w;
        if (ci->size != 0) return RWY_ERR_NOTEMPTY;
        child = o;
    }
    rc = rwy_parent_remove(s, parent, base, blen, 0);
    if (rc) return rc;
    return free_inode_data(s, child);
}

/* An operation that frees space may take the state-change reserve for the directory rewrite. */
int rwy_unlink(rwy_store_t *s, const char *path) {
    int rc;
    if (!s) return RWY_ERR_INVAL;
    s->alloc_reserve++;
    rc = unlink_path(s, path);
    s->alloc_reserve--;
    return rc;
}

int rwy_rmdir(rwy_store_t *s, const char *path) {
    int rc;
    if (!s) return RWY_ERR_INVAL;
    s->alloc_reserve++;
    rc = rmdir_path(s, path);
    s->alloc_reserve--;
    return rc;
}

/* If dst exists: POSIX replace (same-type file replace; dir<->dir only if dst empty; never
 * file<->dir). The new object keeps ITS OWN metadata. *doc is dst, or 0 when it does not exist. */
static int rename_replace(rwy_store_t *s, uint8_t stype, const char *dst, uint64_t *doc,
                          const rwy_inode_t **di) {
    rwy_inode_t *w;
    int rc;

    *doc = 0;
    *di = 0;
    rc = rwy_resolve(s, dst, 1, doc);
    if (rc == RWY_ERR_NOENT) return RWY_OK;
    if (rc) return rc;
    rc = rwy_mut_inode(s, *doc, &w);
    if (rc) return rc == RWY_ERR_NOENT ? RWY_ERR_CORRUPT : rc;
    *di = w;
    if (stype == RWY_TYPE_DIR || w->type == RWY_TYPE_DIR) {
        if (stype != w->type) return RWY_ERR_ISDIR;
        if (w->size != 0) return RWY_ERR_NOTEMPTY;
    }
    return RWY_OK;
}

typedef struct {
    uint64_t sp, dp, so;           /* parents of source and destination, the source */
    const char *sb, *db;           /* base names */
    uint32_t sl, dl;
    uint8_t stype;
} rename_t;

static int rename_lookup(rwy_store_t *s, const char *src, const char *dst, rename_t *r) {
    rwy_inode_t *w;
    int rc = rwy_resolve_parent(s, src, 1, &r->sp, &r->sb, &r->sl);

    if (rc) return rc;
    rc = rwy_resolve_parent(s, dst, 1, &r->dp, &r->db, &r->dl);
    if (rc) return rc;
    rc = rwy_resolve(s, src, 1, &r->so);
    if (rc) return rc;
    rc = rwy_mut_inode(s, r->so, &w);
    if (rc) return rc;
    r->stype = w->type;
    return RWY_OK;
}

int rwy_rename(rwy_store_t *s, const char *src, const char *dst) {
    rename_t r;
    uint64_t doc = 0;
    const rwy_inode_t *di = 0;
    int rc;
    if (!s || !src || !dst) return RWY_ERR_INVAL;
    rc = rwy_need_txn(s);
    if (rc) return rc;
    rc = rename_lookup(s, src, dst, &r);
    if (rc) return rc;
    rc = rename_replace(s, r.stype, dst, &doc, &di);
    if (rc) return rc;
    if (r.so == doc) return RWY_OK;
    rc = rwy_bootcfg_check_rename(s, r.sp, r.sb, r.sl, r.dp, r.db, r.dl, r.stype, di != 0);
    if (rc) return rc;
    if (di) {
        rc = rwy_parent_remove(s, r.dp, r.db, r.dl, 0);
        if (rc) return rc;
        rc = drop_inode_link(s, doc);
        if (rc) return rc;
    }
    rc = rwy_parent_remove(s, r.sp, r.sb, r.sl, 0);
    if (rc) return rc;
    return rwy_parent_insert(s, r.dp, r.so, r.stype, r.db, r.dl);
}

int rwy_chmod(rwy_store_t *s, const char *path, uint32_t mode) {
    uint64_t o;
    rwy_inode_t *in;
    int64_t sec; uint32_t ns;
    int rc;
    if (!s || !path) return RWY_ERR_INVAL;
    rc = rwy_need_txn(s);
    if (rc) return rc;
    rc = rwy_resolve_ext(s, path, 1, 1, &o);
    if (rc) return rc;
    if (o & RWY_OID_VIRT) return rwy_virt_setattr(s, o);
    rc = rwy_mut_inode(s, o, &in);
    if (rc) return rc;
    rc = rwy_bootcfg_check_attr(s, o);
    if (rc) return rc;
    /* preserve file-type bits from inode, take perm/suid/sgid/sticky from arg */
    in->mode = (in->mode & RWY_S_IFMT) | (mode & ~RWY_S_IFMT);
    rwy_now_of(s, &sec, &ns);
    in->ctime_sec = sec; in->ctime_nsec = ns;
    in->generation = s->w_generation;
    return RWY_OK;
}

int rwy_chown(rwy_store_t *s, const char *path, uint32_t uid, uint32_t gid) {
    uint64_t o;
    rwy_inode_t *in;
    int64_t sec; uint32_t ns;
    int rc;
    if (!s || !path) return RWY_ERR_INVAL;
    rc = rwy_need_txn(s);
    if (rc) return rc;
    rc = rwy_resolve(s, path, 1, &o);
    if (rc) return rc;
    if (o & RWY_OID_VIRT) return rwy_virt_setattr(s, o);
    rc = rwy_mut_inode(s, o, &in);
    if (rc) return rc;
    rc = rwy_bootcfg_check_attr(s, o);
    if (rc) return rc;
    in->uid = uid; in->gid = gid;
    rwy_now_of(s, &sec, &ns);
    in->ctime_sec = sec; in->ctime_nsec = ns;
    in->generation = s->w_generation;
    return RWY_OK;
}

int rwy_utimens(rwy_store_t *s, const char *path,
                 int64_t a_sec, uint32_t a_nsec,
                 int64_t m_sec, uint32_t m_nsec) {
    uint64_t o;
    rwy_inode_t *in;
    int64_t sec; uint32_t ns;
    int rc;
    if (!s || !path) return RWY_ERR_INVAL;
    rc = rwy_need_txn(s);
    if (rc) return rc;
    rc = rwy_resolve(s, path, 1, &o);
    if (rc) return rc;
    if (o & RWY_OID_VIRT) return rwy_virt_setattr(s, o);
    rc = rwy_mut_inode(s, o, &in);
    if (rc) return rc;
    rc = rwy_bootcfg_check_attr(s, o);
    if (rc) return rc;
    in->atime_sec = a_sec; in->atime_nsec = a_nsec;
    in->mtime_sec = m_sec; in->mtime_nsec = m_nsec;
    rwy_now_of(s, &sec, &ns);
    in->ctime_sec = sec; in->ctime_nsec = ns;
    in->generation = s->w_generation;
    return RWY_OK;
}

int rwy_set_times(rwy_store_t *s, const char *path,
                  int64_t m_sec, uint32_t m_nsec,
                  int64_t c_sec, uint32_t c_nsec) {
    uint64_t o;
    rwy_inode_t *in;
    int rc, bc;
    if (!s || !path) return RWY_ERR_INVAL;
    rc = rwy_need_txn(s);
    if (rc) return rc;
    rc = rwy_resolve(s, path, 1, &o);
    if (rc) return rc;
    if (o & RWY_OID_VIRT) return rwy_virt_setattr(s, o);
    rc = rwy_mut_inode(s, o, &in);
    if (rc) return rc;
    rc = rwy_bootcfg_check_write(s, o, 0, "", 0, &bc);   /* allowed where a write is */
    if (rc) return rc;
    in->mtime_sec = m_sec; in->mtime_nsec = m_nsec;
    in->ctime_sec = c_sec; in->ctime_nsec = c_nsec;
    in->generation = s->w_generation;
    return RWY_OK;
}
