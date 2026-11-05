/*
 * rwy_oneshot.c — Single-transaction convenience wrappers (begin + op + commit).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

int rwy_txn_link(rwy_store_t *s, const char *oldpath, const char *newpath) {
    int rc;
    if (!s) return RWY_ERR_INVAL;
    rc = rwy_begin(s);
    if (rc) return rc;
    rc = rwy_link(s, oldpath, newpath);
    if (rc) { rwy_abort(s); return rc; }
    return rwy_commit(s);
}

#define WRAP_BEGIN(s) do { int _r = rwy_begin(s); if (_r) return _r; } while (0)

#define WRAP_END(s, rc) do { int _r = (rc); if (_r) { rwy_abort(s); return _r; } return rwy_commit(s); } while (0)

int rwy_txn_create(rwy_store_t *s, const char *p, uint32_t m, uint32_t u, uint32_t g) {
    int rc;
    WRAP_BEGIN(s);
    rc = rwy_create(s, p, m, u, g);
    WRAP_END(s, rc);
}

int rwy_txn_mkdir(rwy_store_t *s, const char *p, uint32_t m, uint32_t u, uint32_t g) {
    int rc;
    WRAP_BEGIN(s);
    rc = rwy_mkdir(s, p, m, u, g);
    WRAP_END(s, rc);
}

int rwy_txn_write_path(rwy_store_t *s, const char *p, uint64_t pos, const void *b, size_t l) {
    int rc;
    WRAP_BEGIN(s);
    rc = rwy_write_path(s, p, pos, b, l);
    WRAP_END(s, rc);
}

int rwy_txn_rename(rwy_store_t *s, const char *a, const char *b_) {
    int rc;
    WRAP_BEGIN(s);
    rc = rwy_rename(s, a, b_);
    WRAP_END(s, rc);
}

int rwy_txn_unlink(rwy_store_t *s, const char *p) {
    int rc;
    WRAP_BEGIN(s);
    rc = rwy_unlink(s, p);
    WRAP_END(s, rc);
}

int rwy_txn_rmdir(rwy_store_t *s, const char *p) {
    int rc;
    WRAP_BEGIN(s);
    rc = rwy_rmdir(s, p);
    WRAP_END(s, rc);
}

int rwy_txn_truncate(rwy_store_t *s, const char *p, uint64_t sz) {
    int rc;
    WRAP_BEGIN(s);
    rc = rwy_truncate(s, p, sz);
    WRAP_END(s, rc);
}

int rwy_txn_chmod(rwy_store_t *s, const char *p, uint32_t m) {
    int rc;
    WRAP_BEGIN(s);
    rc = rwy_chmod(s, p, m);
    WRAP_END(s, rc);
}

int rwy_txn_chown(rwy_store_t *s, const char *p, uint32_t u, uint32_t g) {
    int rc;
    WRAP_BEGIN(s);
    rc = rwy_chown(s, p, u, g);
    WRAP_END(s, rc);
}

int rwy_txn_setxattr(rwy_store_t *s, const char *path, const char *name,
                      const void *value, size_t size, int flags) {
    int rc;
    WRAP_BEGIN(s);
    rc = rwy_setxattr(s, path, name, value, size, flags);
    WRAP_END(s, rc);
}

int rwy_txn_removexattr(rwy_store_t *s, const char *path, const char *name) {
    int rc;
    WRAP_BEGIN(s);
    rc = rwy_removexattr(s, path, name);
    WRAP_END(s, rc);
}
