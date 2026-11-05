/* rwyctl: what the files of the tool share: the store state and the command entry points. */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RWYCTL_H
#define RWYCTL_H

#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_crypto.h"
#include "rwy_format.h"
#include "hostdev.h"
#include "rwyctl_open.h"

/* Exit codes of the commands. */
enum { X_OK_ = 0, X_IO = 1, X_USAGE = 2, X_NOSTORE = 3, X_LOCKED = 4, X_BUSY = 5, X_POLICY = 6, X_NOSPC = 7, X_CHECK = 8 };

#define EFI_ROOT "EFI/RUNWAY"
#define MAXG 16
#define MAXM RWY_MIRROR_MAX_DEVS

typedef struct {
    char path[PATH_MAX];   /* a file, a partition, or IMAGE@OFFSET[+LENGTH] */
    char node[64];   /* partition name (sda1) when found by discovery */
    hostdev_t hd;
    int enc, ok, stale;   /* stale: the store is not among the members RUNWAY.EFI used at boot */
    uint8_t uuid[16];
    uint32_t recovery_start, recovery_blocks;   /* geometry of the plaintext recovery area, 0 blocks = none */
    uint32_t loader_start, loader_blocks;       /* geometry of the plaintext loader area, 0 blocks = none */
    uint32_t flight_start, flight_blocks;       /* geometry of the plaintext flight recorder area */
    uint32_t net_start, net_blocks;             /* geometry of the plaintext settings area */
    uint32_t journal_start, journal_blocks;     /* geometry of the journal area */
    uint32_t rec_start;                         /* state record start block */
    uint64_t gen;
    rwy_block_dev_t raw, dev;
    rwy_crypto_dev_ctx_t cctx;
} member_t;
typedef struct { uint8_t uuid[16]; int n, boot; int m[MAXM]; } group_t;

extern member_t g_mem[MAXG * MAXM];
extern int g_nmem;
extern group_t g_grp[MAXG];
extern int g_ngrp;
extern rwy_store_t g_store;   /* the opened store */
extern rwy_mirror_dev_t g_mirror;   /* its members */
int unlock_group(group_t *g);   /* find and check the key of an encrypted store; 0 or X_LOCKED */
extern int g_rw;   /* the command writes: members are opened read-write and locked */
extern const char *g_root;
extern char g_tmpmnt[8][PATH_MAX];   /* temporary mounts to undo at exit */
extern int g_ntmp;
extern int g_verbose;   /* -v: say why a partition was skipped */
extern char g_why[PATH_MAX + 128];   /* reason of the last failed add_member / temp_mount */
extern int g_explicit;

void warn(const char *fmt, const char *a);
void rpath(char *out, const char *p);
int lock_group(group_t *g);   /* reopen the members read-write and lock them; X_BUSY if one is held */
int system_allowed(void);     /* an offline SYSTEM operation needs --store or --device */

extern const char *g_passfile, *g_dekfile, *g_tpm;   /* --passphrase-file, --dek-file, --tpm */
extern const char *g_passstring;
extern rwy_block_dev_t g_raw_dev;
extern rwy_crypto_dev_ctx_t g_crypto_ctx;
extern rwy_block_dev_t g_crypto_dev;
extern uint8_t g_active_dek[32];
extern int g_is_enc;
int prompt_pass(const char *prompt_str, char *out, size_t max);
int host_random(void *buf, size_t len);
int slurp(const char *path, void *buf, size_t max);   /* bytes read, -1 on error */
const char *tpm_dev(void);
void note_write_failures(void);   /* count the write errors of the members in the record; call after a writing command */
int open_group(group_t *g);   /* unlock, lock if g_rw, mirror and mount; 0 or an X_ code */
int cmd_reencrypt(int argc, char **argv);

/* rwyctl_write.c: begin, change, then finish with one commit. */
int exit_of(int rc);   /* the X_ code of a core error */
int begin_write(void);   /* start a transaction; X_IO when EFI/RUNWAY is absent */
int begin_raise(void);   /* begin_write for a command that raises the epoch (anchor gate first) */
int finish(int rc);   /* commit, or abort when rc is an error; returns an X_ code */
int put_scalar(const char *path, const char *val);
int cmd_init(int argc);
int cmd_state(const char *cmd, const char *state, int argc, char **argv);   /* confirm, fail */
int cmd_set(int argc, char **argv);
int cmd_remove(int argc, char **argv);
int cmd_recover_next(int argc, char **argv);

/* rwyctl_check.c */
int cmd_check(int argc, char **argv);

/* rwyctl_install.c */
int cmd_install(int argc, char **argv);
int cmd_update(int argc, char **argv);
int cmd_recovery(int argc, char **argv);
int cmd_wdt(int argc, char **argv);
int remove_slot(const char *dir);   /* a slot directory and what it holds, in the open transaction */

/* rwyctl_blob.c: blobs named by the SHA-256 of their content, in the open transaction. */
#include "rwy_blob.h"
int blob_exists(const char *name);
int blob_available(const char *name);   /* stored */
int blob_changed(const char *name);   /* a store file that was to be linked is not as hashed; an RWY_ERR_ code */
int blob_missing(const char *name);   /* a blob is gone; an RWY_ERR_ code */
int blob_hash_file(const char *src, char name[RWY_BLOB_NAME_SIZE], uint64_t *size);   /* pass one: the content name and size of a file */
int blob_link(const char *name, const char *src, const char *dst);   /* link BLOBS/name at dst; a missing blob is stored from `src` (NULL: it must exist) */
int stamp_times(const char *dst, const struct stat *st);   /* give dst the mtime and ctime of a source file */
int blob_copy(const char *name, const char *dst);   /* the data of a blob as its own file at dst */

/* rwyctl_read.c */
#define MAXSLOTS 128
extern char g_names[MAXSLOTS][32];   /* filled by list_slots, sorted by number */
int read_scalar(const char *path, char *out, size_t max);   /* the file without trailing blanks; a RWY_ERR_ code */
int slot_name_ok(const char *n);   /* CONFIG<n> */
const char *slot_path(const char *name, char *out);   /* BOOTED, RECOVERY or CONFIG<n> under EFI/RUNWAY; NULL if bad */
int have_efi_runway(void);
int list_slots(void);
int in_list(const char *s, const char *const *list);
int cmd_stores(void);
int cmd_members(const group_t *g);
int cmd_status(void);
int cmd_get(int argc, char **argv);
int cmd_slots(int argc, char **argv);

/* rwyctl_file.c */
int cmd_put(int argc, char **argv);
int cmd_cat(int argc, char **argv);

/* rwyctl_anchor.c */
int anchor_refuse(void);   /* refuse a command that raises the epoch when the counter cannot be trusted; 0 or X_POLICY */
int cmd_anchor(int argc, char **argv);

/* rwyctl_mirror.c */
void print_last_evict(void);   /* the LAST-EVICT line of `status` */
void print_mirror_health(void);   /* the MIRROR lines of `status`, none when no counter is set */
int cmd_mirror(int argc, char **argv);

/* rwyctl_recarea.c: `recovery area set|show` on a group; it does not unlock or mount the tree. */
int cmd_recovery_area(group_t *g, int argc, char **argv);

/* rwyctl_loader.c: `loader show|set|commit` on a group; does not unlock or mount the tree. */
int cmd_loader(group_t *g, int argc, char **argv);
int cmd_log_settings(group_t *g, int argc, char **argv);

/* rwyctl_transp.c: flight recorder and update journal inspection. */
int cmd_flight(group_t *g, int argc, char **argv);
int cmd_log(int argc, char **argv);

/* rwyctl_disc.c */
int add_member(const char *path);
void add_to_group(int mi, int boot);
int probe_partition(const char *devnode);
void probe_disk(const char *disk, const char *boot_part);
void discover_all(const char *boot_part);
int boot_esp(char *part, char *disk);
void read_listed_members(void);
void probe_listed_members(void);
void check_listed_members(void);

#endif
