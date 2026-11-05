/*
 * rwy_fuse_virt.h — Synthetic BLS projection and virtual node declarations for FUSE.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_FUSE_VIRT_H
#define RWY_FUSE_VIRT_H

#include <sys/stat.h>
#define FUSE_USE_VERSION 34
#include <fuse.h>
#include "rwy_core.h"
#include "rwy_bls.h"

#define RWY_FH_VIRT_SREL 0xFFFFFFFFFFFFFFF1ULL
#define RWY_FH_VIRT_CONF 0xFFFFFFFFFFFFFFF2ULL
#define RWY_FH_STAGED_BASE 0xFFFFFFFFFFFF0000ULL

int err_of(int rc);
void fill_stbuf(const rwy_attr_t *a, struct stat *st);

int rwy_fuse_virt_getattr(rwy_store_t *s, const char *path, struct stat *st);
int rwy_fuse_virt_readdir(rwy_store_t *s, const char *path, void *buf,
                           fuse_fill_dir_t filler);
int rwy_fuse_virt_readdir_extra(rwy_store_t *s, const char *path, void *buf,
                                 fuse_fill_dir_t filler);
int rwy_fuse_virt_open(rwy_store_t *s, const char *path, struct fuse_file_info *fi);
int rwy_fuse_virt_read(rwy_store_t *s, const char *path, char *buf,
                        size_t size, off_t off, struct fuse_file_info *fi);
int rwy_fuse_virt_create(rwy_store_t *s, const char *path, mode_t mode,
                          struct fuse_file_info *fi);
int rwy_fuse_virt_write(rwy_store_t *s, const char *path, const char *buf,
                         size_t size, off_t off, struct fuse_file_info *fi);
int rwy_fuse_virt_flush(rwy_store_t *s, const char *path, struct fuse_file_info *fi);
int rwy_fuse_virt_release(rwy_store_t *s, const char *path, struct fuse_file_info *fi);
int rwy_fuse_virt_truncate(rwy_store_t *s, const char *path, off_t size);
int rwy_fuse_virt_unlink(rwy_store_t *s, const char *path);
int rwy_fuse_virt_mkdir(rwy_store_t *s, const char *path, mode_t mode);
int rwy_fuse_virt_rmdir(rwy_store_t *s, const char *path);
int rwy_fuse_virt_rename_stage(rwy_store_t *s, const char *from, const char *to, unsigned flags);
int rwy_fuse_virt_resolve_path(rwy_store_t *s, const char *path, char *out, size_t out_sz);

#endif /* RWY_FUSE_VIRT_H */
