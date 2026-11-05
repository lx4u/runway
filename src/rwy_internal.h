/*
 * rwy_internal.h — private interfaces shared between the Runway core's source files.
 * SPDX-License-Identifier: Apache-2.0
 * Not part of the public API (see rwy_core.h).
 */
#ifndef RWY_INTERNAL_H
#define RWY_INTERNAL_H

#include <string.h>

#include "rwy_core.h"

#define REACH_BYTES (RWY_MAX_BLOCKS / 8u)

extern uint8_t rwy_gc_prot[REACH_BYTES];
extern uint8_t rwy_gc_work[REACH_BYTES];

/* rwy_io.c */
void rwy_io_count(rwy_io_stats_t *st, rwy_io_kind_t kind, int write, size_t len);
/* Byte reads and writes through the device; they count a call of `kind` when the device has stats. */
int rwy_dev_pread(const rwy_block_dev_t *d, rwy_io_kind_t kind, uint64_t off, void *buf, size_t len);
int rwy_dev_pwrite(const rwy_block_dev_t *d, rwy_io_kind_t kind, uint64_t off, const void *buf, size_t len);
void rwy_dircache_drop(const rwy_block_dev_t *d);
int rwy_dircache_read(const rwy_block_dev_t *d, uint32_t blk, uint32_t off, void *buf, size_t len);
int rwy_blk_read(const rwy_block_dev_t *d, rwy_io_kind_t kind, uint32_t blk, void *buf);
int rwy_blk_write(const rwy_block_dev_t *d, rwy_io_kind_t kind, uint32_t blk, const void *buf);
/* Write n whole blocks starting at blk with one device call. */
int rwy_blk_write_run(const rwy_block_dev_t *d, rwy_io_kind_t kind, uint32_t blk, const void *buf, uint32_t n);
int rwy_dev_flush(const rwy_block_dev_t *d);
void rwy_blk_hdr_make(rwy_blk_header_t *h, uint64_t gen, uint32_t idx, uint32_t kind);
uint32_t rwy_blk_compute_crc(const uint8_t blk[RWY_BLOCK_SIZE]);
int rwy_blk_verify(const uint8_t blk[RWY_BLOCK_SIZE], uint64_t gen, uint32_t idx, uint32_t kind);

/* rwy_alloc.c */
int rwy_bmp_get(const uint8_t *bm, uint32_t b);
void rwy_bmp_set(uint8_t *bm, uint32_t b);
void rwy_bmp_clear(uint8_t *bm, uint32_t b);
int rwy_find_run_in(const uint8_t *occ, uint32_t total_blocks, uint32_t hint, uint32_t need, uint32_t *out);
int rwy_bmp_alloc_run(uint8_t *bm, uint32_t total_blocks, uint32_t hint, uint32_t need, uint32_t *out);
int rwy_w_alloc_runs(rwy_store_t *s, const uint32_t *needs, uint32_t *outs, uint32_t n);
int rwy_w_alloc_split(rwy_store_t *s, uint32_t total, rwy_extent_t *runs, uint32_t *nruns);
int rwy_w_alloc_run(rwy_store_t *s, uint32_t need, uint32_t *out);
int rwy_w_reserve_ok(const rwy_store_t *s, uint32_t need);

/* rwy_gc.c */
void rwy_mark_range(uint8_t *bits, uint32_t start, uint32_t n);
void rwy_mark_meta_zone(const rwy_store_t *s, uint8_t *bits);
int rwy_compact_working(rwy_store_t *s);

/* rwy_defrag.c */
typedef struct {
    uint32_t new_start[RWY_MAX_INODES];
    uint32_t new_len[RWY_MAX_INODES];
} reloc_ctx_t;
typedef reloc_ctx_t rwy_reloc_ctx_t;

int rwy_inode_is_frag(const rwy_inode_t *in);
int rwy_defrag_plan(rwy_store_t *s, reloc_ctx_t *ctx);
int rwy_defrag_copy(rwy_store_t *s, const rwy_block_dev_t *to, const reloc_ctx_t *ctx);
int rwy_defrag_build_bitmap(rwy_store_t *s, const reloc_ctx_t *ctx);
void rwy_defrag_patch_inodes(rwy_store_t *s, const reloc_ctx_t *ctx);
int rwy_defrag_publish(rwy_store_t *s);

#define inode_is_frag rwy_inode_is_frag
#define defrag_plan rwy_defrag_plan
#define defrag_copy rwy_defrag_copy
#define defrag_build_bitmap rwy_defrag_build_bitmap
#define defrag_patch_inodes rwy_defrag_patch_inodes
#define defrag_publish rwy_defrag_publish

/* rwy_mount.c */
void rwy_now_of(const rwy_store_t *s, int64_t *sec, uint32_t *nsec);

/* rwy_util.c — pick the valid copy with the highest sequence (state record, recovery area). `check` returns
 * RWY_OK and the sequence of copy `i`, RWY_ERR_IO if it could not be read, or another error if it is not valid.
 * An equal sequence keeps the lower index. Returns IO if no copy is valid and a read failed, else CORRUPT. */
typedef int (*rwy_copy_check_fn)(void *ctx, unsigned i, uint64_t *seq);
int rwy_pick_newest(unsigned n, rwy_copy_check_fn check, void *ctx, unsigned *best);

/* rwy_record.c */
int rwy_record_load(rwy_store_t *s);

/* rwy_txn.c */
int rwy_commit_write_publish(rwy_store_t *s);
int rwy_write_superblock(rwy_store_t *s, const rwy_superblock_t *nsb, const void *tail);
int rwy_write_superblock_to(rwy_store_t *s, const rwy_block_dev_t *dst, const rwy_superblock_t *nsb, const void *tail);
int rwy_write_inode_image_to(rwy_store_t *s, const rwy_block_dev_t *dst, uint32_t tbl_start);
int rwy_write_bitmap_image_to(rwy_store_t *s, const rwy_block_dev_t *dst, uint32_t bm_start);

/* rwy_inode.c */
const rwy_inode_t *rwy_inode_of(const rwy_store_t *s, uint64_t oid, int working);
rwy_inode_t *rwy_winode_of(rwy_store_t *s, uint64_t oid);
/* Working inode for a mutator taking a caller oid: EPERM for the virtual INDEX, ENOENT if gone. */
int rwy_mut_inode(rwy_store_t *s, uint64_t oid, rwy_inode_t **out);
void rwy_w_free(rwy_store_t *s, uint32_t blk);
void rwy_w_free_reset(rwy_store_t *s);
int rwy_phys_of(const rwy_store_t *s, const rwy_inode_t *in, uint32_t l, uint32_t *out);
int rwy_phys_find(const rwy_store_t *s, const rwy_inode_t *in, uint32_t l, uint32_t *out, uint32_t *run);
uint32_t rwy_nblocks_of(uint64_t size);
#define RWY_INDIRECT_PAIRS ((RWY_BLOCK_SIZE - sizeof(rwy_blk_header_t)) / 8)
#define RWY_MAX_EXTENTS    (RWY_DIRECT_EXTENTS + RWY_INDIRECT_PAIRS)
int rwy_extents_load(const rwy_store_t *s, const rwy_inode_t *in, rwy_extent_t *out, uint32_t *n);
void rwy_extents_push(rwy_extent_t *list, uint32_t *n, uint32_t start, uint32_t len);
void rwy_extents_range(rwy_extent_t *list, uint32_t *n, const rwy_extent_t *src, uint32_t sn,
                        uint32_t from, uint32_t to);
int rwy_extents_reserve(rwy_store_t *s, uint32_t n, uint32_t *ind);
int rwy_extents_set(rwy_store_t *s, rwy_inode_t *in, const rwy_extent_t *list, uint32_t n, uint32_t ind);
int rwy_inode_pread(const rwy_store_t *s, const rwy_inode_t *in, uint64_t pos, void *buf, size_t want, size_t *got);
void rwy_winode_free_data(rwy_store_t *s, rwy_inode_t *in);
void rwy_fill_attr(const rwy_inode_t *in, rwy_attr_t *a);
rwy_inode_t *rwy_alloc_inode(rwy_store_t *s, uint64_t *out_oid);
void rwy_inode_init(rwy_store_t *s, rwy_inode_t *in, uint64_t oid, uint8_t type, uint32_t mode, uint32_t uid, uint32_t gid);

/* rwy_dir.c */
int rwy_resolve_parent(rwy_store_t *s, const char *path, int working, uint64_t *out_parent, const char **out_base, uint32_t *out_baselen);
int rwy_resolve_ext(rwy_store_t *s, const char *path, int working, int follow_terminal, uint64_t *out);
int rwy_resolve(rwy_store_t *s, const char *path, int working, uint64_t *out);
int rwy_parent_insert(rwy_store_t *s, uint64_t parent, uint64_t child, uint8_t type, const char *base, uint32_t baselen);
int rwy_parent_remove(rwy_store_t *s, uint64_t parent, const char *base, uint32_t baselen, uint64_t *out_child);

/* rwy_slotstate.c — layout of the state record payload. A payload is RWY_REC_PAYLOAD bytes;
 * an all-zero one is empty. A slot without an entry reads as INSTALLED with 1 try. */
enum { RWY_SS_INSTALLED, RWY_SS_OK, RWY_SS_TESTING, RWY_SS_FAILED };
/* Room for twice the 64 slots the policy allows, so a store with a few more (an old image) still works. */
#define RWY_SS_ENTRIES 128u
/* Why a slot is FAILED; NONE for a slot that is not, or a FAILED one from before the cause was kept. */
enum { RWY_SS_CAUSE_NONE, RWY_SS_CAUSE_TRIES, RWY_SS_CAUSE_LOAD, RWY_SS_CAUSE_OPERATOR, RWY_SS_CAUSE_LOOP, RWY_SS_CAUSE_BLOB, RWY_SS_CAUSE_LAST = RWY_SS_CAUSE_BLOB };
typedef struct { uint64_t tries; uint8_t state, cause, pinned; } rwy_ss_entry_t;
int rwy_ss_recover_next(const uint8_t *r);
void rwy_ss_set_recover_next(uint8_t *r, int on);
int rwy_ss_reason(const uint8_t *r);
void rwy_ss_set_reason(uint8_t *r, int reason);
size_t rwy_ss_booted(const uint8_t *r, char *out);   /* `out` holds 30 bytes; returns the length, 0 = none */
int rwy_ss_set_booted(uint8_t *r, const char *name); /* NULL clears */
#define RWY_SS_WDT_MAX 32u
size_t rwy_ss_wdt(const uint8_t *r, char *out);      /* `out` holds 33 bytes; returns the length, 0 = none */
int rwy_ss_set_wdt(uint8_t *r, const char *name);    /* NULL clears */
int rwy_ss_get(const uint8_t *r, uint64_t rev, rwy_ss_entry_t *out);   /* 1 if the revision has an entry */
int rwy_ss_put(uint8_t *r, uint64_t rev, const rwy_ss_entry_t *in);    /* RWY_ERR_NOSPC if all are taken */
int rwy_ss_cause_ok(const uint8_t *r, uint64_t rev);   /* 0 if the cause byte is set on a slot that is not FAILED, or unknown */
void rwy_ss_prune(uint8_t *r, const uint64_t *live, size_t n);          /* drop entries of other revisions */
/* Anti-rollback fields: the TPM counter value at the last raise, the highest REVISION ever
 * committed, and whether the store is bound to a TPM counter. Zero in an old record. */
uint64_t rwy_ss_epoch(const uint8_t *r);
void rwy_ss_set_epoch(uint8_t *r, uint64_t v);
uint64_t rwy_ss_rev_hw(const uint8_t *r);
void rwy_ss_set_rev_hw(uint8_t *r, uint64_t v);
int rwy_ss_enrolled(const uint8_t *r);
void rwy_ss_set_enrolled(uint8_t *r, int on);

/* Mirror health: counters per member, keyed by the member UUID of the superblock. A zero payload has
 * none. Counters never go down except by rwy_ss_health_clear(); merging takes the larger value. */
#define RWY_SS_HEALTH_MEMBERS RWY_MIRROR_MAX_DEVS
/* Count `event` for the member; RWY_ERR_INVAL for a zero UUID or an unknown event, RWY_ERR_NOSPC if the table is full. */
int rwy_ss_health_note(uint8_t *r, const uint8_t uuid[16], int event, uint64_t seq);
void rwy_ss_health_merge(uint8_t *dst, const uint8_t *src);
void rwy_ss_health_clear(uint8_t *r);   /* drops the members; the reset count stays */
void rwy_ss_note_record_reset(uint8_t *r);

/* Roster: the UUIDs of the members the store should have (ADR 0005 §2.4). A zero payload has none, which
 * means no member is expected. Merging takes the union; only rwy_ss_roster_forget() removes an entry. */
#define RWY_SS_ROSTER_MEMBERS RWY_MIRROR_MAX_DEVS
int rwy_ss_roster_has(const uint8_t *r, const uint8_t uuid[16]);
/* 1 if added, 0 if already listed; RWY_ERR_INVAL for a zero UUID, RWY_ERR_NOSPC if the roster is full. */
int rwy_ss_roster_add(uint8_t *r, const uint8_t uuid[16]);
int rwy_ss_roster_forget(uint8_t *r, const uint8_t uuid[16]);   /* 1 if it was listed */
void rwy_ss_roster_merge(uint8_t *dst, const uint8_t *src);

/* rwy_bootcfg.c — EFI/RUNWAY boot policy. Every hook returns RWY_OK when
 * the store has no EFI/RUNWAY directory, so stores without it behave as before. */
void rwy_bootcfg_invalidate(rwy_store_t *s);
/* Core-internal bypass for finalize and for tests; adapters never call it. */
void rwy_bootcfg_set_priv(rwy_store_t *s, int on);
int rwy_bootcfg_check_create(rwy_store_t *s, uint64_t parent, const char *base, uint32_t blen,
                              uint8_t type, int is_link, uint32_t *mode, uint32_t *uid, uint32_t *gid);
int rwy_bootcfg_check_link(rwy_store_t *s, uint64_t old_oid, uint64_t parent, const char *base, uint32_t blen);
/* Returns 1 when the removal must succeed without doing anything (the REVISION of a slot goes with its rmdir). */
int rwy_bootcfg_check_remove(rwy_store_t *s, uint64_t parent, const char *base, uint32_t blen);
int rwy_bootcfg_empty_slot(rwy_store_t *s, uint64_t dir, const char *path);
int rwy_bootcfg_check_rename(rwy_store_t *s, uint64_t sp, const char *sb, uint32_t sl,
                              uint64_t dp, const char *db, uint32_t dl, uint8_t type, int replaces);
int rwy_bootcfg_check_attr(rwy_store_t *s, uint64_t oid);
/* *replace is set for scalar files: the caller truncates to 0 before writing.
 * RWY_BC_FINALIZE is or-ed in for INSTALLED on an incomplete slot: the caller then
 * calls rwy_bootcfg_finalize() in the same transaction once the write succeeded. */
#define RWY_BC_REPLACE  1
#define RWY_BC_FINALIZE 2
int rwy_bootcfg_check_write(rwy_store_t *s, uint64_t oid, uint64_t pos, const void *buf, size_t len, int *replace);
/* *noop is set when the truncate must be reported as a success without doing anything. */
int rwy_bootcfg_check_truncate(rwy_store_t *s, uint64_t oid, uint64_t size, int *noop);
/* What a directory is to the EFI/RUNWAY policy: the subtree root, a CONFIG<n> slot, or neither. Works with or
 * without a transaction; `slot` receives the CONFIG<n> name of a slot. */
enum { RWY_ROLE_NONE, RWY_ROLE_ROOT, RWY_ROLE_SLOT };
int rwy_bootcfg_role(rwy_store_t *s, uint64_t dir, char *slot);

/* rwy_virt.c — virtual files: oid = RWY_OID_VIRT | kind << RWY_VKIND_SHIFT | directory oid,
 * never in the inode table. Mutators that take an oid refuse it with RWY_ERR_PERM (rwy_mut_inode); write,
 * truncate, unlink and create are routed to the hooks below. */
#define RWY_OID_VIRT (1ull << 63)
#define RWY_VKIND_SHIFT 48
#define RWY_VTEXT_MAX 48
enum { RWY_VK_INDEX, RWY_VK_STATE, RWY_VK_TRIES, RWY_VK_CAUSE, RWY_VK_PIN, RWY_VK_BOOTED, RWY_VK_REASON, RWY_VK_RECOVER_NEXT, RWY_VK_WDT_HINT, RWY_VK_FLIGHT, RWY_VK_LOG, RWY_VK_COUNT };
int rwy_virt_lookup(rwy_store_t *s, uint64_t dir, const char *name, uint32_t nlen, uint64_t *oid);
/* The `index`-th virtual entry of `dir`, listed after the real ones; RWY_ERR_NOENT past the last. */
int rwy_virt_nth(rwy_store_t *s, uint64_t dir, size_t index, char *name, size_t name_sz, uint8_t *type);
int rwy_virt_attr(rwy_store_t *s, uint64_t oid, rwy_attr_t *out);
int rwy_virt_read(rwy_store_t *s, uint64_t oid, uint64_t pos, void *buf, size_t *len);
/* Target of a virtual symlink; RWY_ERR_INVAL for any other virtual file. */
int rwy_virt_readlink(rwy_store_t *s, uint64_t oid, char *out, size_t outsz);
int rwy_virt_write(rwy_store_t *s, uint64_t oid, uint64_t pos, const void *buf, size_t len);
int rwy_virt_truncate(rwy_store_t *s, uint64_t oid, uint64_t size);
int rwy_virt_unlink(rwy_store_t *s, uint64_t oid);
int rwy_virt_setattr(rwy_store_t *s, uint64_t oid);
/* Creating a name that is virtual: *done is set when the call made it (RECOVER_NEXT), else the caller goes on. */
int rwy_virt_create(rwy_store_t *s, uint64_t parent, const char *name, uint32_t nlen, uint8_t type, int *done);
/* 1 if `name` is a virtual file in a directory of `role` (`final`: a slot that has its REVISION), so no real
 * entry may carry it. */
int rwy_virt_reserved(int role, int final, const char *name, uint32_t nlen);
int rwy_bootcfg_mkdir_done(rwy_store_t *s, uint64_t oid);
int rwy_bootcfg_finalize(rwy_store_t *s, uint64_t state_oid);
/* Called once a commit has been published. */
void rwy_bootcfg_committed(rwy_store_t *s);
int rwy_bootcfg_sweep(rwy_store_t *s);

/* rwy_gc.c: the data blocks that removing links frees. refs[i] is the number of links to inode i that go away;
 * an inode frees its blocks only when that is all of its links. */
int rwy_blocks_freed(rwy_store_t *s, const uint16_t *refs, uint32_t *freed);

/* rwy_dir.c (lookup of one name inside a directory object) */
/* Next entry of a directory at byte offset *off (start with 0), name of up to RWY_NAME_MAX bytes and a NUL.
 * Real entries only. RWY_ERR_NOENT at the end. One pass over a directory reads each of its blocks once. */
int rwy_dir_next(const rwy_store_t *s, uint64_t dir, uint64_t *off, char *name, uint32_t *nlen,
                 uint64_t *oid, uint8_t *type);
int rwy_dir_lookup(const rwy_store_t *s, uint64_t dir, const char *name, uint32_t nlen,
                    uint64_t *out_oid, uint8_t *out_type);

/* rwy_ns.c */
int rwy_need_txn(const rwy_store_t *s);

/* rwy_file.c */
int rwy_cow_write(rwy_store_t *s, rwy_inode_t *in, uint64_t pos, const uint8_t *buf, size_t len);
int rwy_inode_truncate(rwy_store_t *s, rwy_inode_t *in, uint64_t size);

/* rwy_anchor.c: selection and condemn use the counter attached to the store */
int rwy_anchor_refused(const rwy_store_t *s);
int rwy_anchor_raise_rec(const rwy_store_t *s, uint8_t *rec);
void rwy_anchor_raised(const rwy_store_t *s);

#endif /* RWY_INTERNAL_H */
