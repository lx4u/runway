/* Shared by the files of fsck.runway. */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CHECK_H
#define CHECK_H

#include <stdint.h>
#include <stdio.h>

#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_crypto.h"
#include "hostdev.h"

/* A store file, a window of an image, or inside the FAT32 volume of an ESP image. */
typedef struct {
    hostdev_t hd;
    rwy_block_dev_t bdev;   /* the store file or window, as the core sees it */
    const char *path;
    int is_fat32;
    uint64_t fat_data_offset;
    uint32_t cluster_size;
    uint32_t *clusters;
    uint32_t cluster_count;
    uint64_t store_size;
} file_t;

/* The findings of one run, the block owner map and the options that shape the checks. */
typedef struct {
    int errors, warnings, orphans, leftovers, frag_objects;
    int cfg_counter;   /* EFI/RUNWAY/REVISION is bad: an error that --repair fixes */
    int cfg_record;    /* the state record is damaged: an error that --repair fixes */
    int cfg_stale;     /* the state record has entries of removed slots: --repair drops them */
    int32_t *owner;    /* per block: -1 free-expected, else oid / -2 metadata */
    uint32_t total_blocks, pool_start;
    const char *tpm;   /* --tpm DEV or RWY_TPM: where the counter is read */
    const char *member[8];   /* --member FILE: other stores of the mirror, for the recovery area compare */
    int nmember;
} check_ctx_t;

#define ERR(c, fmt, ...) do { printf("ERROR: " fmt "\n", ##__VA_ARGS__); (c)->errors++; } while (0)
#define WARN(c, fmt, ...) do { printf("warn: " fmt "\n", ##__VA_ARGS__); (c)->warnings++; } while (0)
#define NOTE(c, fmt, ...) do { printf("note: " fmt "\n", ##__VA_ARGS__); } while (0)

typedef struct {
    uint64_t live_oids[RWY_MAX_INODES];
    uint32_t dirent_refs[RWY_MAX_INODES + 1];
    uint32_t nlive;
    uint32_t referenced_data;
} scan_t;

/* Exit codes, as in fsck(8). */
enum {
    FSCK_OK = 0,
    FSCK_FIXED = 1,
    FSCK_UNCORRECTED = 4,
    FSCK_ERROR = 8,
    FSCK_USAGE = 16,
};

typedef struct {
    const char *store_path;
    const char *key_pass;
    int repair, defrag;
} opts_t;

/* One opened store: the file, the block device on it and the decrypting view used for checks. */
typedef struct {
    file_t fl;
    rwy_block_dev_t dev, chk_dev;
    rwy_crypto_dev_ctx_t cctx;
    int is_enc;
} session_t;

static inline int all_zero(const uint8_t *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++)
        if (p[i]) return 0;
    return 1;
}

/* file.c */
int file_open(file_t *fl, const char *path, int rw);
void file_close(file_t *fl);
uint64_t file_get_size(file_t *fl);
int file_rd(void *ctx, uint64_t off, void *buf, size_t len);
int file_wr_ro(void *ctx, uint64_t off, const void *buf, size_t len);
int file_wr_rw(void *ctx, uint64_t off, const void *buf, size_t len);
int file_fl_rw(void *ctx);
long file_io_points(void);
void file_set_fail_at(long n);

/* sb.c */
void check_flight_area(check_ctx_t *c, file_t *fl, const rwy_superblock_t *sb);
void check_journal_area(check_ctx_t *c, rwy_block_dev_t *dev, const rwy_store_t *s);
void check_recovery_area(check_ctx_t *c, file_t *fl, const rwy_superblock_t *sb);
void check_loader_area(check_ctx_t *c, file_t *fl, const rwy_superblock_t *sb);
void check_net_area(check_ctx_t *c, file_t *fl, const rwy_superblock_t *sb);
int check_sb_slots(check_ctx_t *c, rwy_block_dev_t *dev);
void check_sb_committed(check_ctx_t *c, const rwy_store_t *s);

/* meta.c */
void check_blk_headers(check_ctx_t *c, rwy_block_dev_t *dev, const rwy_store_t *s, uint32_t start, uint32_t count, uint32_t kind, const char *what);
void check_inode(check_ctx_t *c, file_t *fl, rwy_block_dev_t *dev, const rwy_store_t *s, uint32_t i, scan_t *sc);

/* tree.c */
void check_dir_tree(check_ctx_t *c, rwy_store_t *s, scan_t *sc);
void check_bitmap(check_ctx_t *c, rwy_block_dev_t *dev, rwy_store_t *s, uint32_t referenced_data);
void report_fragmentation(check_ctx_t *c, rwy_store_t *s);
void check_orphans(check_ctx_t *c, const scan_t *sc);

/* bootcfg.c */
void check_bootcfg(check_ctx_t *c, rwy_store_t *s);
void check_mirror_health(check_ctx_t *c, rwy_store_t *s);   /* a warning for each member with a counter */

/* run.c */
int run_checks(check_ctx_t *c, file_t *fl, rwy_block_dev_t *dev, rwy_store_t *s_out);
int run_unauthenticated_check(check_ctx_t *c, file_t *fl, rwy_block_dev_t *dev);

/* session.c */
int session_open(session_t *ss, const char *path, const char *key_pass, int rw, const char *lock_hint);
int session_check(check_ctx_t *c, session_t *ss, rwy_store_t *s);

/* repair.c */
int cmd_repair(check_ctx_t *c, const opts_t *o);
int cmd_defrag(check_ctx_t *c, const opts_t *o);

#endif
