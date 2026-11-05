/*
 * rwy_core — portable transactional filesystem core (v2).
 * SPDX-License-Identifier: Apache-2.0
 *
 * Correctness model: "transactional CoW with atomic root publication"
 * (no journal). A transaction stages new data/bitmap/inode-table blocks
 * in free space; commit flushes them, then writes exactly one superblock
 * slot and flushes again. Mount picks the highest-valid generation.
 *
 * Threading: single-threaded (EFI boot services, FUSE serialized by the
 * adapter, host tests). No locking in the core.
 *
 * Memory: the core needs ~ (bitmap 8 KiB + inode table 256 KiB) x2 plus
 * one block scratch. Callers provide no allocator: the rwy_store_t is
 * a caller-allocated struct with fixed-size arrays (portable, no malloc
 * in the core). Adapters own block I/O.
 */
#ifndef RWY_CORE_H
#define RWY_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "rwy_block.h"
#include "rwy_format.h"
#include "rwy_entry.h"

#define RWY_OK            0
#define RWY_ERR_MAGIC    -1
#define RWY_ERR_VERSION  -2
#define RWY_ERR_SIZE     -3
#define RWY_ERR_NOENT    -4
#define RWY_ERR_EXIST    -5
#define RWY_ERR_NOSPC    -6
#define RWY_ERR_INVAL    -7
#define RWY_ERR_IO       -8
#define RWY_ERR_NOTDIR   -9
#define RWY_ERR_ISDIR    -10
#define RWY_ERR_NAMETOOLONG -11
#define RWY_ERR_NOTEMPTY -12
#define RWY_ERR_FEAT     -13
#define RWY_ERR_CORRUPT  -14
#define RWY_ERR_NOTXN    -15
#define RWY_ERR_XATTR    -16
#define RWY_ERR_RANGE    -17
#define RWY_ERR_LOOP     -18
#define RWY_ERR_PERM     -19   /* refused by the EFI/RUNWAY policy (see rwy_bootcfg.c) */
#define RWY_ERR_ROLLBACK -20   /* the tree is older than the state record says it can be */
#define RWY_ERR_BUSY     -21   /* slot is busy (booted or last OK slot) */

/* File-type test helpers on rwy_attr_t.mode. */
#define RWY_S_ISREG(m) (((m) & RWY_S_IFMT) == RWY_S_IFREG)
#define RWY_S_ISDIR(m) (((m) & RWY_S_IFMT) == RWY_S_IFDIR)
#define RWY_S_ISLNK(m) (((m) & RWY_S_IFMT) == RWY_S_IFLNK)

/* Portable stat-like attributes. */
typedef struct {
    uint64_t oid;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    int64_t  atime_sec; uint32_t atime_nsec;
    int64_t  mtime_sec; uint32_t mtime_nsec;
    int64_t  ctime_sec; uint32_t ctime_nsec;
    int64_t  btime_sec; uint32_t btime_nsec;
    uint8_t  type;
    uint32_t nlink;
} rwy_attr_t;

#define RWY_MAX_PATH 1024u

struct rwy_anchor_ops;
/* Blocks of the buffer a commit builds its metadata images in; the inode table fits one run. */
#define RWY_WRUN_BLOCKS 38u

/* Not-normal flight events of one boot kept in memory for the report, oldest first. */
#define RWY_FLIGHT_BOOT_MAX 16u

typedef struct {
    /* Committed state (from mount / last commit). */
    rwy_superblock_t sb;                       /* committed SB image */
    int               sb_slot;                  /* slot it came from */
    rwy_inode_t      inodes[RWY_MAX_INODES];  /* committed table */
    uint8_t           bitmap[RWY_MAX_BLOCKS / 8];

    /* State record (I11): the current copy, kept in memory. Not part of a transaction. */
    uint8_t           rec[RWY_REC_PAYLOAD];
    uint64_t          rec_seq;
    int               rec_cur;                  /* copy it came from (0 or 1) */
    int               rec_damaged;              /* mount found no valid copy and used an empty record */
    uint64_t          mount_id;                 /* which open of a store this is (rwy_mount); rwy_remount keeps it */
    rwy_flight_entry_t flight_boot[RWY_FLIGHT_BOOT_MAX];   /* this boot's events that warrant a report */
    uint32_t          flight_boot_count;        /* entries used in flight_boot */

    /* Working transaction state (valid iff in_txn). */
    int          in_txn;
    rwy_inode_t w_inodes[RWY_MAX_INODES];
    uint8_t      w_bitmap[RWY_MAX_BLOCKS / 8];
    uint8_t      w_free[RWY_MAX_BLOCKS / 8];   /* blocks of the committed generation freed in this txn */
    uint64_t     w_generation;   /* sb.generation + 1 */
    uint8_t      wrun[RWY_WRUN_BLOCKS * RWY_BLOCK_SIZE];   /* images being written by a commit */
    uint32_t     next_hint;      /* allocation search hint */
    int          alloc_reserve;  /* > 0 while the operation may take the state-change reserve */

    rwy_block_dev_t dev;
    /* Deterministic clock for tests/image builds: if set, used as "now". */
    int64_t  fixed_now_sec;
    uint32_t fixed_now_nsec;
    int      have_fixed_now;

    /* EFI/RUNWAY boot policy. `actor` is who is writing; adapters leave it
     * at RWY_ACTOR_USER. The rest is private to rwy_bootcfg.c: the cached
     * "EFI/RUNWAY" root and the classification of the last OID written, both
     * dropped on every namespace change. */
    int      actor;
    int      bc_priv;
    int      bc_unlock;      /* RECOVERY/ writable by SYSTEM; cleared by rwy_mount() */
    int      bc_novirt;      /* hide the virtual INDEX (fsck.runway, audit); kept by rwy_remount() */
    const struct rwy_anchor_ops *anchor_ops;   /* the counter of the anti-rollback anchor, or NULL; kept by rwy_remount() */
    int      bc_root_valid;
    int      bc_err;         /* last read error met while looking for EFI/RUNWAY, 0 if none */
    uint64_t bc_rev_pending; /* revision finalize allocated in this transaction; recorded after the commit */
    uint8_t  bc_evict_reason; /* RWY_EVICT_REASON_* while rwy_bootcfg_evict_execute removes a slot, else 0 */
    uint32_t bc_evict_blocks; /* blocks that removal frees */
    int      bc_sweep;        /* a link count fell to 1 in this transaction: look for orphan content blobs before the commit */
    int      bc_slot_removed; /* a slot directory was removed in this transaction; the epoch is raised after the commit */
    uint64_t bc_root;
    int      bc_cls_valid;
    uint64_t bc_cls_oid;
    uint8_t  bc_cls[8];
    rwy_io_stats_t io;       /* calls the core made to dev since the mount */
    rwy_dircache_t dircache; /* directory blocks of dev, dropped by every write */
} rwy_store_t;

void rwy_dev_stats_add(rwy_dev_stats_t *sum, const rwy_dev_stats_t *add);
/* Lines of text for the counters of a store and, when dev is not NULL, for the devices below it.
 * Returns the length; the text is cut to fit and always ends in a NUL. */
size_t rwy_io_format(char *buf, size_t cap, const rwy_io_stats_t *core, const rwy_dev_stats_t *dev);

#define RWY_ACTOR_USER   0   /* adapters: FUSE, EFI file protocol */
#define RWY_ACTOR_SYSTEM 1   /* RUNWAY.EFI, mkstore, fsck.runway --repair */
/* Select who is writing. Survives rwy_remount(); rwy_mount() resets it to USER. */
void rwy_set_actor(rwy_store_t *s, int actor);

/* Blocks an ordinary write can still take: the free blocks of the working state (committed
 * state outside a transaction) minus RWY_RESERVE_BLOCKS, or 0. */
uint32_t rwy_free_blocks(const rwy_store_t *s);

/* Data blocks in use by live objects (leftovers of earlier commits are not counted) and the data pool without
 * RWY_RESERVE_BLOCKS, for the visible state. Read-only. */
void rwy_pool_usage(rwy_store_t *s, uint32_t *used, uint32_t *usable);

#define RWY_LABEL_MAX 128u

/* --- EFI/RUNWAY boot selection (rwy_bootcfg.c) ---
 * Needs the SYSTEM actor and no open transaction. Applies the slot state transitions
 * (INSTALLED/TESTING -> TESTING/FAILED, TRIES) and updates BOOTED and REASON in ONE
 * commit, none when nothing changes. `log` (optional) receives each "Runway: ..." line.
 * engaged = 0 when the store has no CONFIG<n> (legacy boot, nothing was touched);
 * booted = 0 when engaged but no slot is bootable. */
typedef struct {
    int      engaged, booted, trial;
    char     slot[32];                 /* "CONFIG<n>", up to 26 characters and the NUL */
    uint64_t revision, tries_left, watchdog;
    uint64_t boot_delay;               /* seconds to pause before starting the slot, 0 = none */
    int      has_label;
    char     label[RWY_LABEL_MAX + 1];
    char     entry[RWY_ENTRY_MAX + 1];
    char     loop_slot[32];            /* the OK slot demoted for a boot loop in this selection, else empty */
} rwy_boot_choice_t;
typedef void (*rwy_log_fn)(void *ctx, const char *line);
int rwy_bootcfg_select(rwy_store_t *s, rwy_boot_choice_t *out, rwy_log_fn log, void *ctx);

/* What select() would choose now, without committing or logging. Same rules and result; the store is
 * not touched, a RECOVER_NEXT request is not consumed. SYSTEM actor, no open transaction. */
int rwy_bootcfg_preview(rwy_store_t *s, rwy_boot_choice_t *out);

/* Call `fn` with the entry name of every slot that selection can choose (the ids a loader reports to the OS).
 * Needs an open transaction; changes nothing. RWY_ERR_INVAL without one, RWY_ERR_NOENT when the store has no EFI/RUNWAY. */
typedef void (*rwy_entry_fn)(void *ctx, const char *entry);
int rwy_bootcfg_entries(rwy_store_t *s, rwy_entry_fn fn, void *ctx);

/* select() could not commit: choose what to boot without writing. SYSTEM actor, no open transaction.
 * The highest-REVISION slot whose STATE is OK, else RECOVERY, else booted = 0. `after` (NULL on the first call) is
 * the slot that was chosen and could not be started: only lower slots are considered; "RECOVERY" leaves nothing.
 * tries_left is 0 and nothing is touched: BOOTED, REASON, TRIES and a RECOVER_NEXT request stay as they are.
 * RWY_ERR_INVAL when `after` is not a valid slot or RECOVERY. */
int rwy_bootcfg_fallback(rwy_store_t *s, rwy_boot_choice_t *out, const char *after);

/* The slot select() chose could not be started (LoadImage refused it, ...): set its STATE to FAILED in one commit
 * (SYSTEM actor, no open transaction), so the next select() skips it. RWY_ERR_INVAL unless `slot` is "CONFIG<n>"
 * (RECOVERY has no lifecycle), RWY_ERR_NOENT if it has no STATE. */
int rwy_bootcfg_condemn(rwy_store_t *s, const char *slot);

/* The slot `rwyctl update` may recycle. Needs an open transaction; changes nothing.
 * Order: a slot that is not usable (incomplete, or an unparsable REVISION), then a FAILED one, then an
 * OK one, the lowest REVISION first. Never a pending slot (INSTALLED, TESTING), never the slot BOOTED
 * names, never the OK slot with the highest REVISION (the one that boots next). `name` holds >= 32 bytes.
 * RWY_ERR_NOENT when no slot may go, RWY_ERR_INVAL when the store has no EFI/RUNWAY. */
int rwy_bootcfg_recycle_pick(rwy_store_t *s, char *name);

/* The share of the usable pool that the slots may use (QUOTA, in percent). Needs an open transaction. RWY_ERR_NOENT when QUOTA is absent or
 * out of range: quota eviction is off. RWY_ERR_INVAL when the store has no EFI/RUNWAY. */
int rwy_bootcfg_quota(rwy_store_t *s, unsigned *percent);

/* The boot loop guard settings: LOOP-GUARD, LOOP-LIMIT and LOOP-WINDOW. */
typedef struct {
    unsigned limit;    /* entries allowed inside the window */
    unsigned window;   /* seconds */
} rwy_loop_guard_t;

/* The guard settings. Needs an open transaction. RWY_OK when LOOP-GUARD is "on"; a missing or bad LOOP-LIMIT or LOOP-WINDOW
 * takes its default (5, 600). RWY_ERR_NOENT when LOOP-GUARD is off, absent or bad. RWY_ERR_INVAL when the store has no EFI/RUNWAY. */
int rwy_bootcfg_loop_guard(rwy_store_t *s, rwy_loop_guard_t *g);

/* Asked at each entry into the OK slot `revision` while the guard is on. Non-zero: the limit is reached, demote it. */
typedef int (*rwy_guard_fn)(void *ctx, uint64_t revision, const rwy_loop_guard_t *g);

/* select() with the boot loop guard. A demoted OK slot becomes FAILED with CAUSE boot_loop and the next lower OK slot (else
 * RECOVERY) is chosen with REASON boot_loop; choice->loop_slot names it, even when the commit then fails. When no OK slot
 * and no RECOVERY is left, the slot is not demoted: it boots and the flight recorder gets a LOOP_LIMIT event. `guard` NULL
 * is select(). */
int rwy_bootcfg_select_guarded(rwy_store_t *s, rwy_boot_choice_t *out, rwy_log_fn log, void *ctx,
                               rwy_guard_fn guard, void *gctx);

/* The slots that may go, in the order rwy_bootcfg_recycle_pick would take them: names[0] is its answer. Same rules
 * and same transaction need. `count` gets the number of names stored, at most `max` and RWY_RECYCLE_MAX; 0 when no
 * slot may go. RWY_ERR_INVAL when the store has no EFI/RUNWAY. */
#define RWY_RECYCLE_MAX 128
int rwy_bootcfg_recycle_order(rwy_store_t *s, char names[][32], size_t max, size_t *count);

/* The data blocks that removing the `n` slots `names` would free. A file shared through BLOBS/ frees its blocks
 * only when every link to it is in the list. Slots hold files only. Read-only; RWY_ERR_NOENT if a name is not a slot. */
int rwy_bootcfg_evict_blocks(rwy_store_t *s, char names[][32], size_t n, uint32_t *freed);

/* What to remove so that a new slot of `new_blocks` blocks fits the quota and SLOTS. */
typedef struct {
    char names[RWY_RECYCLE_MAX][32];
    size_t count;
    uint32_t freed;      /* blocks the removals free together */
    uint8_t reasons[RWY_RECYCLE_MAX];       /* RWY_EVICT_REASON_QUOTA or _SLOTS: what asked for names[i] */
    uint32_t slot_freed[RWY_RECYCLE_MAX];   /* blocks that removing names[i] frees after the ones before it */
} rwy_evict_plan_t;

/* Plan, in memory, the slots to remove before a new slot of `new_blocks` blocks (only the blocks not already in
 * BLOBS/) is installed. Needs an open transaction; changes nothing. Walks rwy_bootcfg_recycle_order, but also keeps the
 * second newest OK slot when there is no RECOVERY/. Stops at the first set of removals that meets QUOTA (blocks
 * freed counted together, shared blobs once) and SLOTS (room for one more slot). `count` is 0 when nothing has to go,
 * and also when neither QUOTA nor SLOTS is set. RWY_ERR_NOSPC when the protected slots and the new slot do not
 * fit; `count` is then 0 and nothing may be deleted. */
int rwy_bootcfg_evict_plan(rwy_store_t *s, uint32_t new_blocks, rwy_evict_plan_t *plan);

/* Remove the slots of `plan` in the open transaction, each with an EVICT journal record (state, reason, blocks freed;
 * a slot without REVISION is recorded as incomplete). The caller commits. The plan must come from this transaction. */
int rwy_bootcfg_evict_execute(rwy_store_t *s, const rwy_evict_plan_t *plan);
/* Remove a slot (or RECOVERY/, WDT/) and what it holds, in the open transaction. A slot's REMOVE journal line
 * carries its label and payload hash. */
int rwy_bootcfg_remove_slot(rwy_store_t *s, const char *path);

/* Blocks a new slot takes for its small files (ENTRY, ARGS, STATE, ...), which the payload sizes do not show. */
#define RWY_SLOT_META_BLOCKS 8u

/* With QUOTA set, make room for a new slot of `total` blocks whose largest file needs a run of `largest` blocks. Needs
 * an open transaction. Without QUOTA, or with nothing to evict, changes nothing and keeps it open. Otherwise: plan,
 * remove the planned slots in their own commit, defragment when the free blocks suffice but no run is long enough, and
 * open a new transaction for the caller. On RWY_OK a transaction is open. On error none is, nothing is half done, and the
 * protected slots are intact (RWY_ERR_NOSPC when the new slot cannot fit). */
int rwy_bootcfg_make_room(rwy_store_t *s, uint32_t total, uint32_t largest);

/* 1 if `n` (length `l`) is a content name of EFI/RUNWAY/BLOBS: 64 lower case hex digits. Only SYSTEM makes, links,
 * renames to or unlinks one, and nobody writes an object that has one. */
int rwy_bootcfg_content_name(const char *n, size_t l);

/* Watchdog applications in EFI/RUNWAY/WDT: the sorted names of the valid regular files, at most `max` of them
 * copied to `names`. Returns how many valid names there are (more than `max` means the rest was left out), 0 when
 * there is no WDT/, < 0 on a read error. No open transaction needed. */
#define RWY_WDT_MAX 8
#define RWY_WDT_NAME_MAX 32
int rwy_bootcfg_wdt_list(rwy_store_t *s, char names[][RWY_WDT_NAME_MAX + 1], size_t max);

/* The WDT application that armed the watchdog on the last boot, kept in the state record. wdt_hint copies the
 * name to `name` (>= RWY_WDT_NAME_MAX + 1 bytes) and returns its length, 0 = none. wdt_set_hint stores `name`
 * (NULL or "" clears) and writes the record only when the value changes. Only RUNWAY.EFI calls the setter. */
size_t rwy_bootcfg_wdt_hint(rwy_store_t *s, char *name);
int rwy_bootcfg_wdt_set_hint(rwy_store_t *s, const char *name);

/* Open (on = 1) or close RECOVERY/ and WDT/ for writing by the SYSTEM actor: mkstore and
 * offline repair only. RWY_ERR_PERM unless the actor is SYSTEM. The adapters never call it,
 * and mount/remount close it again. */
int rwy_bootcfg_unlock_recovery(rwy_store_t *s, int on);

/* Offline audit of EFI/RUNWAY: strict, read-only, any actor. `log` receives one line
 * per violation. engaged = 0 when the store has no EFI/RUNWAY (nothing was looked at).
 * violations: entries the schema forbids, bad values, finalized slots or RECOVERY that selection would reject.
 * incomplete: slots without REVISION (reported, never an error). counter_bad: the global REVISION is
 * missing (with slots), unparsable or behind the highest slot revision (repair fixes it);
 * counter is what repair would write. record_damaged: both record copies failed their CRC at mount (repair
 * writes an empty record). Warnings, never errors: stale = the record has entries of revisions no slot has
 * (repair drops them), booted_gone = BOOTED names a slot that does not exist, over_slots = the value of SLOTS when
 * there are more slots than that. replayed: the tree counter is below the rev_hw of the record, so the tree was put back
 * (a violation that repair never touches). enrolled and epoch are the anchor fields of the record; the counter they
 * are checked against is outside the store (rwy_anchor_check). Needs no open transaction. */
typedef struct {
    int      engaged, slots, incomplete, violations, counter_bad, record_damaged, stale, booted_gone, over_slots, replayed, enrolled;
    uint64_t counter, epoch;
} rwy_bootcfg_audit_t;
int rwy_bootcfg_audit(rwy_store_t *s, rwy_bootcfg_audit_t *out, rwy_log_fn log, void *ctx);

/* Call `log` with "CONFIG<n> FAILED (<cause>)" for each FAILED slot, in directory order. Returns how many,
 * or < 0 on a read error. */
int rwy_bootcfg_failed(rwy_store_t *s, rwy_log_fn log, void *ctx);

/* Set the global REVISION to the highest slot revision when the audit finds it bad: SYSTEM only,
 * one commit, none when nothing is wrong. Returns 1 if repaired, 0 if untouched, < 0 on error. */
int rwy_bootcfg_repair_counter(rwy_store_t *s);

/* Write the record without its stale entries, or an empty one when it is damaged: SYSTEM only, one record
 * write, none when nothing is wrong. Returns 1 if written, 0 if untouched, < 0 on error. */
int rwy_bootcfg_repair_record(rwy_store_t *s);

const char *rwy_strerror(int rc);

/* Mount: read both SB slots, select highest valid generation. */
int rwy_mount(rwy_store_t *s, const rwy_block_dev_t *dev);
/* Re-read committed state from backing store (after external power-loss
 * simulation or to discard an aborted txn). */
int rwy_remount(rwy_store_t *s);

/* Transaction lifecycle. begin snapshots committed state; commit publishes
 * a new generation; abort discards the working copy. Mutating ops auto-begin
 * a transaction if none is active? No — they REQUIRE an active txn and
 * return RWY_ERR_NOTXN otherwise, except the convenience rwy_txn_* wrappers
 * below which do begin/op/commit. Adapters choose explicit batching.
 * A commit that fails (NOSPC, I/O) discards the transaction: nothing was published
 * and the next rwy_begin() works. */
int rwy_begin(rwy_store_t *s);
int rwy_commit(rwy_store_t *s);
int rwy_abort(rwy_store_t *s);
int rwy_in_txn(const rwy_store_t *s);

/* --- Namespace / file ops (require active txn for mutating ops) --- */
#define RWY_SYMLOOP_MAX 32u
int rwy_lookup(rwy_store_t *s, const char *path, uint64_t *out_oid);
int rwy_lookup_follow(rwy_store_t *s, const char *path, int follow_terminal, uint64_t *out_oid);
int rwy_getattr(rwy_store_t *s, uint64_t oid, rwy_attr_t *out);
int rwy_getattr_path(rwy_store_t *s, const char *path, rwy_attr_t *out);
int rwy_stat(rwy_store_t *s, const char *path, rwy_attr_t *out);
int rwy_lstat(rwy_store_t *s, const char *path, rwy_attr_t *out);

/* create/mkdir/symlink with explicit metadata (deterministic builds).
 * mode includes file-type bits; uid/gid/times stored verbatim. */
int rwy_create(rwy_store_t *s, const char *path, uint32_t mode,
                uint32_t uid, uint32_t gid);
int rwy_mkdir(rwy_store_t *s, const char *path, uint32_t mode,
               uint32_t uid, uint32_t gid);
int rwy_symlink(rwy_store_t *s, const char *path, const char *target,
                 uint32_t uid, uint32_t gid);
int rwy_readlink(rwy_store_t *s, const char *path, char *out, size_t outsz);

/* Hardlinks: adds directory entry pointing to existing file's OID and increments nlink. */
int rwy_link(rwy_store_t *s, const char *oldpath, const char *newpath);

int rwy_unlink(rwy_store_t *s, const char *path);
int rwy_rmdir(rwy_store_t *s, const char *path);
/* rename with POSIX replace semantics: dst is atomically replaced if it
 * exists (file->file only; dir replacement only if empty and types match).
 * The REPLACEMENT object keeps its own metadata (new object wins); the
 * replaced object's metadata is discarded with it. Documented in FORMAT. */
int rwy_rename(rwy_store_t *s, const char *src, const char *dst);

int rwy_truncate(rwy_store_t *s, const char *path, uint64_t size);
int rwy_truncate_oid(rwy_store_t *s, uint64_t oid, uint64_t size);

int rwy_read(rwy_store_t *s, uint64_t oid, uint64_t pos, void *buf, size_t *len);
int rwy_read_path(rwy_store_t *s, const char *path, uint64_t pos, void *buf, size_t *len);
int rwy_write(rwy_store_t *s, uint64_t oid, uint64_t pos, const void *buf, size_t len);
int rwy_write_path(rwy_store_t *s, const char *path, uint64_t pos, const void *buf, size_t len);

int rwy_chmod(rwy_store_t *s, const char *path, uint32_t mode);
int rwy_chown(rwy_store_t *s, const char *path, uint32_t uid, uint32_t gid);
int rwy_utimens(rwy_store_t *s, const char *path,
                 int64_t a_sec, uint32_t a_nsec,
                 int64_t m_sec, uint32_t m_nsec);
/* Set mtime and ctime as given (64-bit seconds), for a file copied in from elsewhere. Call it after the
 * last write or link: both set ctime to now. */
int rwy_set_times(rwy_store_t *s, const char *path,
                  int64_t m_sec, uint32_t m_nsec,
                  int64_t c_sec, uint32_t c_nsec);

/* Extended attributes API. */
int rwy_getxattr(rwy_store_t *s, const char *path, const char *name,
                  void *buf, size_t size);
int rwy_getxattr_oid(rwy_store_t *s, uint64_t oid, const char *name,
                      void *buf, size_t size);
int rwy_setxattr(rwy_store_t *s, const char *path, const char *name,
                  const void *value, size_t size, int flags);
int rwy_setxattr_oid(rwy_store_t *s, uint64_t oid, const char *name,
                      const void *value, size_t size, int flags);
int rwy_listxattr(rwy_store_t *s, const char *path, char *list, size_t size);
int rwy_listxattr_oid(rwy_store_t *s, uint64_t oid, char *list, size_t size);
int rwy_removexattr(rwy_store_t *s, const char *path, const char *name);
int rwy_removexattr_oid(rwy_store_t *s, uint64_t oid, const char *name);

/* readdir: list immediate child names of dir path. out_names is a
 * caller buffer; names packed NUL-separated. Returns count via
 * inout_count (capacity in, used out). */
int rwy_readdir(rwy_store_t *s, const char *path,
                 char *out_names, size_t out_sz, size_t *inout_count,
                 uint8_t *out_types /* optional, may be NULL */);
int rwy_readdir_nth(rwy_store_t *s, const char *path, size_t index,
                     char *out_name, size_t out_name_sz, uint8_t *out_type);

/* Convenience single-transaction wrappers (begin/op/commit). */
int rwy_txn_create(rwy_store_t *s, const char *p, uint32_t m, uint32_t u, uint32_t g);
int rwy_txn_mkdir(rwy_store_t *s, const char *p, uint32_t m, uint32_t u, uint32_t g);
int rwy_txn_link(rwy_store_t *s, const char *oldpath, const char *newpath);
int rwy_txn_write_path(rwy_store_t *s, const char *p, uint64_t pos, const void *b, size_t l);
int rwy_txn_rename(rwy_store_t *s, const char *a, const char *b_);
int rwy_txn_unlink(rwy_store_t *s, const char *p);
int rwy_txn_rmdir(rwy_store_t *s, const char *p);
int rwy_txn_truncate(rwy_store_t *s, const char *p, uint64_t sz);
int rwy_txn_chmod(rwy_store_t *s, const char *p, uint32_t m);
int rwy_txn_chown(rwy_store_t *s, const char *p, uint32_t u, uint32_t g);
int rwy_txn_setxattr(rwy_store_t *s, const char *path, const char *name,
                      const void *value, size_t size, int flags);
int rwy_txn_removexattr(rwy_store_t *s, const char *path, const char *name);

/* Time control for reproducible images. */
void rwy_set_fixed_time(rwy_store_t *s, int64_t sec, uint32_t nsec);
void rwy_clear_fixed_time(rwy_store_t *s);

/* Portable string helpers (freestanding-safe; ASCII). */
size_t rwy_strlen(const char *s);
int    rwy_strcmp(const char *a, const char *b);
int    rwy_strncmp(const char *a, const char *b, size_t n);
uint32_t rwy_crc32c(const void *data, size_t len);
void   rwy_format_uuid(const uint8_t uuid[16], char out[37]);
int    rwy_parse_uuid(const char *in, uint8_t out[16]);

/* Validation entry points shared with `fsck.runway`. */
int rwy_validate_sb(const rwy_superblock_t *sb);
int rwy_validate_sb_block(const void *blk, rwy_superblock_t *out);
int rwy_sb_slot_valid(rwy_store_t *s, int slot, rwy_superblock_t *out);
/* Put a member UUID into a superblock block and fix its CRC. */
void rwy_sb_block_set_member(uint8_t *blk, const uint8_t uuid[16]);

/* --- State record (format I11) ---
 * RWY_REC_PAYLOAD opaque bytes, written in place outside the CoW generations. A write
 * needs no free block and no free inode, and is not part of a transaction. On error the
 * record read back is the one before the call; the next write goes to the same copy. */
int rwy_record_read(const rwy_store_t *s, void *buf);
int rwy_record_write(rwy_store_t *s, const void *buf);
/* 1 from a mount that found both copies damaged until the next successful write. The record is then empty. */
int rwy_record_damaged(const rwy_store_t *s);
/* Mirror members of one store: a record write does not change the tree generation, so members of
 * the same generation can hold different records. Finds the member with the highest sequence (the
 * first on a tie, 0 if none is valid) and, with `heal`, copies its record to each member that is
 * behind. A member that cannot be written is skipped. */
int rwy_record_sync(const rwy_block_dev_t *m, size_t n, uint32_t rec_start, int heal, size_t *newest);
/* Max-merge the mirror health counters, and merge the roster, of the newest record of each member into the
 * record held by `s`. Memory only: the next record write puts them on disk. A member that cannot be read is skipped. */
int rwy_record_merge_health(rwy_store_t *s, const rwy_block_dev_t *m, size_t n);
/* Mirror health events (ADR 0005). */
enum { RWY_SS_EV_NONE, RWY_SS_EV_LAGGING, RWY_SS_EV_SYNC_FAILED, RWY_SS_EV_WRITE_FAILED, RWY_SS_EV_MISSING,
       RWY_SS_EV_LAST = RWY_SS_EV_MISSING };
/* Count `event` for the member with that UUID and write the record. `devs` (n members, as for the merge) is
 * optional: the counters of the other members are merged first. */
int rwy_record_note_health(rwy_store_t *s, const rwy_block_dev_t *devs, size_t n, const uint8_t uuid[16], int event);
/* Count a write failure for each live member of `m` that failed a write since the last call. A faulted
 * member is not counted: it is lagging and has its own event. Returns the first error of the record writes. */
int rwy_mirror_note_failures(rwy_store_t *s, rwy_mirror_dev_t *m, const rwy_block_dev_t *devs, size_t n);
/* The mount-time roster check for the members of `m`: list the ones the roster lacks (two members or more),
 * and count a MISSING event for each roster member that `m` does not hold. Nothing is written for a stable
 * mirror. A member that is gone for good keeps raising MISSING until rwy_record_forget_member(). */
int rwy_mirror_note_members(rwy_store_t *s, const rwy_mirror_dev_t *m, const rwy_block_dev_t *devs, size_t n);
typedef struct {
    uint32_t count[RWY_SS_EV_LAST + 1];   /* indexed by event; [0] is unused */
    uint8_t  last_event;                   /* RWY_SS_EV_* */
    uint64_t seq;                          /* record sequence at that event */
} rwy_ss_health_t;
/* The counters of the member with that UUID in a record payload; 1 if the member has an entry. */
int rwy_ss_health_get(const uint8_t *r, const uint8_t uuid[16], rwy_ss_health_t *out);
/* Entry `i` (0 to RWY_MIRROR_MAX_DEVS - 1) of the table in a record payload; 1 if it is in use. */
int rwy_ss_health_at(const uint8_t *r, unsigned i, uint8_t uuid[16], rwy_ss_health_t *out);
/* How many times the record was found damaged and started empty. */
uint32_t rwy_ss_record_reset(const uint8_t *r);
/* Name of an event as `rwyctl` and `fsck.runway` print it ("lagging"), "none" for an unknown one. */
const char *rwy_ss_event_name(int event);
/* Entry `i` (0 to RWY_MIRROR_MAX_DEVS - 1) of the roster in a record payload; 1 if it is in use. */
int rwy_ss_roster_at(const uint8_t *r, unsigned i, uint8_t uuid[16]);
/* Add the `n` UUIDs in `uuids` (16 bytes each) that the roster lacks and write the record, after the merge
 * of `devs` as for rwy_record_note_health(). Nothing is written when none is new, or when `n` is below 2: a
 * single member has nothing to mirror. */
int rwy_record_note_roster(rwy_store_t *s, const rwy_block_dev_t *devs, size_t n, const uint8_t (*uuids)[16], size_t count);
/* Drop a member from the roster and write the record; RWY_ERR_NOENT if it is not listed. Its counters stay. */
int rwy_record_forget_member(rwy_store_t *s, const uint8_t uuid[16]);
/* Drop the counters of every member and write the record. The record-reset count stays. */
int rwy_record_clear_health(rwy_store_t *s);
/* Read the record from one member instead of from the store's device (a mirror reads its primary only). */
int rwy_record_adopt(rwy_store_t *s, const rwy_block_dev_t *m);
/* --- Anti-rollback anchor ---
 * A monotonic counter outside the disk (a TPM NV counter). The core only calls these two callbacks. */
typedef struct rwy_anchor_ops {
    int (*read)(void *ctx, uint64_t *value);   /* RWY_OK, a RWY_ANCHOR_READ_* value, or an error */
    int (*increment)(void *ctx);
    void *ctx;
} rwy_anchor_ops_t;
/* Other results of read() than a value: the counter exists, so the store has been enrolled. */
enum {
    RWY_ANCHOR_READ_UNSET = 1,   /* defined but never incremented */
    RWY_ANCHOR_READ_ABSENT,      /* no such counter */
    RWY_ANCHOR_READ_DENIED       /* the counter exists, but the TPM refuses to read it (PCR 7 differs) */
};
enum {
    RWY_ANCHOR_OFF,        /* the store is not enrolled; the counter is not read */
    RWY_ANCHOR_OK,         /* epoch equals the counter */
    RWY_ANCHOR_FINISHED,   /* epoch was one ahead (a cut before the increment); the increment is done now */
    RWY_ANCHOR_BEHIND,     /* epoch below the counter: the record was replayed */
    RWY_ANCHOR_AHEAD,      /* epoch two or more above the counter: the record is from elsewhere */
    RWY_ANCHOR_MISSING,    /* enrolled, but the counter cannot be read (or `ops` is NULL) */
    RWY_ANCHOR_NEW,        /* enrolled offline: the counter was never incremented, the first increment and the epoch are to do */
    RWY_ANCHOR_STRAY       /* not enrolled, but the counter of this (encrypted) store exists: the record is a copy from before the enrollment */
};
/* Compare the record's epoch with the counter. An encrypted store that is not enrolled is looked up too, when `ops` is given. Returns a RWY_ANCHOR_* value, or a negative error when the
 * increment of the FINISHED case fails. Writes nothing to the store. */
int rwy_anchor_check(const rwy_store_t *s, const rwy_anchor_ops_t *ops);
/* Check, and finish the enrollment of a store enrolled offline (RWY_ANCHOR_NEW, epoch 0): increment the counter
 * once and record its value as the epoch. Returns the check result afterwards (RWY_ANCHOR_OK when it adopted), or
 * a negative error. A NEW counter with an epoch other than 0 is left as NEW. */
int rwy_anchor_adopt(rwy_store_t *s, const rwy_anchor_ops_t *ops);
/* The epoch and the enrolled flag of the record. `set` changes both in one record write. */
/* The NV index of the store's counter: the owner-defined range, picked by the store UUID so stores do not share one. */
uint32_t rwy_anchor_index(const rwy_store_t *s);
void rwy_anchor_get(const rwy_store_t *s, uint64_t *epoch, int *enrolled);
int rwy_anchor_set(rwy_store_t *s, uint64_t epoch, int enrolled);
/* The counter that select, preview, fallback and condemn check and raise; NULL for none. An enrolled store with
 * none cannot be verified, so those calls return RWY_ERR_ROLLBACK. `ops` must outlive the store handle. */
void rwy_anchor_attach(rwy_store_t *s, const rwy_anchor_ops_t *ops);

/* Allocation query: 1 if block is marked used in the visible (working if a
 * transaction is open, else committed) bitmap. */
int rwy_blk_used(const rwy_store_t *s, uint32_t blk);

/* --- Reachability / space reclamation (GC) ---
 *
 * GC is an optimization, never part of correctness: it recovers space,
 * never filesystem contents. Model: a stop-the-world mark-and-rebuild
 * collector over physical block ownership.
 *
 * Reachability: the authoritative set "blocks required by a committed
 * generation" is computed by rwy_mark_data_reachable() — every data /
 * indirect block referenced by any live inode in the given table — plus
 * the generation's own inode-table and bitmap images (added by callers).
 * `fsck.runway` uses this same function, so the checker and the collector
 * cannot disagree about what is reachable.
 *
 * rwy_gc() performs one synchronous collection transaction:
 *   1. scans reachability from the CURRENTLY COMMITTED generation,
 *   2. allocates fresh table+bitmap images from blocks that are neither
 *      reachable nor part of the current images (never overwrites the
 *      old generation's metadata in place),
 *   3. builds the replacement bitmap (reachable + new images only),
 *   4. writes both images CoW, flushes,
 *   5. publishes a new superblock generation, flushes.
 * A crash before publication leaves the old generation (and old bitmap)
 * fully valid; GC output is unreachable garbage. A crash after
 * publication leaves the new generation valid with all reachable data
 * intact. Either way the mount selects a valid generation; a failed GC
 * costs space, never correctness.
 *
 * Guarantees: synchronous; publishes exactly one new generation with
 * identical visible contents; safe to call repeatedly (idempotent from
 * the contents' perspective); needs no keys/plaintext (physical blocks
 * only), so it stays compatible with the future block-layer encryption
 * and with future multi-root (snapshot) collectors — only the root set
 * would grow.
 * Failure modes: RWY_ERR_INVAL (no store / a transaction is open —
 * commit or abort first), RWY_ERR_NOSPC (no suitable contiguous run
 * for the replacement metadata images even after reclaiming), RWY_ERR_IO
 * / RWY_ERR_CORRUPT (backing layer or metadata unreadable; committed
 * state untouched).
 *
 * Automatic reclamation: normal data allocation retries once after an
 * in-memory compaction of the working bitmap (drops only blocks that are
 * reachable from NEITHER the committed NOR the working image, so the
 * old generation is never overwritten pre-commit), and rwy_commit()
 * transparently rebuilds the published bitmap from the working reachable
 * set when the incremental bitmap has no room for the new metadata
 * images. Callers therefore see NOSPC only when space is genuinely
 * exhausted or no contiguous run exists; explicit rwy_gc() is for
 * proactive cleanup (e.g. before large installs). */
int rwy_gc(rwy_store_t *s);
/* Mark every data block referenced by live inodes in `tbl`
 * (direct extents, indirect block, indirect extents) into `bits`, a
 * caller-zeroed bitset of RWY_TOTAL_BLOCKS/8 bytes. Reads indirect
 * blocks through s->dev; read-only (no keys/plaintext needed). Does NOT
 * mark superblocks or table/bitmap images. Returns RWY_OK or
 * RWY_ERR_CORRUPT (unreadable/indirect metadata) / RWY_ERR_INVAL. */
int rwy_mark_data_reachable(const rwy_store_t *s, const rwy_inode_t *tbl,
                             uint8_t *bits);
/* Count of marked-but-unreachable blocks in the visible bitmap
 * (transaction leftovers / reclaimable space), excluding the current
 * metadata images. Zero after a successful rwy_gc(). */
uint32_t rwy_reclaimable(rwy_store_t *s);

/* --- Fragmentation metric / offline compaction (defrag) ---
 *
 * Fragmentation here means one thing: live objects split over several
 * physical runs (extent_count > 1, or an indirect block in use). The
 * checker reports it but never acts on it by itself (report always,
 * never auto-trigger; compaction is an explicit offline operation).
 *
 * rwy_frag_info() fills `out` from the COMMITTED generation (it does
 * not require or open a transaction):
 *   frag_objects     live inodes stored in >1 extent or via indirect
 *   live_blocks      data/indirect blocks reachable from live inodes
 *   free_blocks      clear bits in the committed bitmap (pool only)
 *   largest_free_run longest contiguous clear run in the pool
 * A store is "compact" when frag_objects == 0. Defrag converges to
 * that: every live object ends up in a single contiguous run
 * (extent_count <= 1, no indirect), so plain `check` afterwards shows
 * zero multi-extent objects with identical visible contents.
 *
 * rwy_defrag() performs one synchronous stop-the-world compaction
 * transaction that implies the GC rebuild (single combined pass):
 *   1. scans reachability from the CURRENTLY COMMITTED generation,
 *   2. allocates fresh single runs for every fragmented object plus
 *      fresh table+bitmap images, all from blocks that are neither
 *      reachable nor part of the current images (never overwrites the
 *      old generation in place),
 *   3. copies each fragmented object's data to its new run (physical
 *      block copies only; no keys/plaintext),
 *   4. builds the replacement bitmap (retained in-place runs + new
 *      runs + new images only),
 *   5. writes data + both images CoW, flushes, publishes a new
 *      superblock generation, flushes.
 * Objects that are already single-run are left in place (shared, not
 * copied): this keeps large-object defrag feasible under the 2x-space
 * CoW constraint and makes "already compact" a clean no-op.
 * OIDs and the directory tree are untouched; only data extents and
 * the table/bitmap images move.
 *
 * Crash safety matches rwy_gc(): any failure before publication
 * leaves the old generation valid (output is unreachable garbage); a
 * crash after publication leaves the new generation valid with all
 * reachable data intact. A failed defrag costs space, never data.
 *
 * No-op: when the store is already compact AND has zero reclaimable
 * leftovers, rwy_defrag() publishes nothing and returns RWY_OK with
 * the generation unchanged (callers detect this by comparing
 * sb.generation before/after, as `check --defrag` does).
 *
 * Failure modes: RWY_ERR_INVAL (no store / a transaction is open),
 * RWY_ERR_NOSPC (no suitable contiguous scratch runs even though the
 * store is consistent — genuine exhaustion or a fragmented object too
 * large to duplicate under CoW), RWY_ERR_IO / RWY_ERR_CORRUPT. */
typedef struct {
    uint32_t frag_objects;
    uint32_t live_blocks;
    uint32_t free_blocks;
    uint32_t largest_free_run;
} rwy_frag_info_t;
int rwy_frag_info(const rwy_store_t *s, rwy_frag_info_t *out);
int rwy_defrag(rwy_store_t *s);

/* Relocate all live blocks to another device in a single crash-safe pass,
 * write the state record copy, publish one generation in the alternate slot
 * with the given tail (or carry the current tail if NULL), and swap s->dev. */
int rwy_relocate(rwy_store_t *s, const rwy_block_dev_t *to, const void *sb_tail);

#endif /* RWY_CORE_H */
