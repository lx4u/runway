/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOTCFGTEST_H
#define BOOTCFGTEST_H

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
#include "rwy_flight.h"
#include "rwy_entry.h"
#include "rwy_meta.h"
#include "rwy_transplog.h"
#include "rwy_internal.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)
#define EXPECT(rc, want, msg) do { \
    int _r = (rc); \
    if (_r != (want)) { printf("FAIL: %s: got %d want %d (line %d)\n", msg, _r, (int)(want), __LINE__); failures++; } \
} while (0)

#define T   "EFI/RUNWAY"
#define OK  RWY_OK
#define PERM RWY_ERR_PERM
#define INV RWY_ERR_INVAL

extern int failures;
extern const char *g_checkbin;
extern rwy_store_t S;
extern char logbuf[2048];

typedef struct { FILE *f; } file_t;
typedef struct { rwy_block_dev_t inner; long point, fail_at; } fi_t;

int file_rd(void *ctx, uint64_t off, void *buf, size_t len);
int file_wr(void *ctx, uint64_t off, const void *buf, size_t len);
int file_fl(void *ctx);
void open_store(rwy_store_t *s, rwy_block_dev_t *dev, file_t *fl, const char *path);
void copy_file(const char *a, const char *b);
int run_checker(const char *p);
int run_checker_args(const char *args, const char *p, const char *env);
int chk_has(const char *sub);
int txn(int rc_op);
int mk(const char *p);
int cr(const char *p);
int sy(const char *p, const char *t);
int ln(const char *a, const char *b);
int rm(const char *p);
int rd(const char *p);
int mv(const char *a, const char *b);
int wr(const char *p, uint64_t pos, const char *d);
int tr(const char *p, uint64_t n);
int ch(const char *p);
int co(const char *p);
int ut(const char *p);
int sx(const char *p);
int rx(const char *p);
void priv(int on);
void as_user(void);
void as_sys(void);
uint64_t gen(void);
uint64_t rseq(void);
void set_booted(const char *name);
void set_reason(int code);
void seed(const char *p, const char *content);
void seed_dir(const char *p);
int read_all(const char *p, char *buf, size_t cap);
void same_text(const char *p, const char *want, const char *msg);
long num(const char *path);
long rev(int n);
long counter(void);
void slot(int n);
int install(int n);
void drop(int n);
void set_counter(const char *v);
void logfn(void *ctx, const char *line);
int has(const char *sub);
int sel_with(rwy_store_t *s, rwy_boot_choice_t *c);
int sel(rwy_boot_choice_t *c);
int text_is(const char *p, const char *want);
int booted_is(const char *want);
void set_state(int n, const char *v);
void set_file(int n, const char *f, const char *v);
void fin(int n, const char *state);
void fresh_subtree(void);
void mkrecovery(void);
void unlock(int on);
int fi_rd(void *c, uint64_t o, void *b, size_t l);
int fi_wr(void *c, uint64_t o, const void *b, size_t l);
int fi_fl(void *c);

/* schema tests */
void test_inactive(void);
void test_subtree_create(void);
void test_names_types(void);
void test_mode_owner(void);
void test_scalars(void);
void test_finalized(void);
void test_links(void);
void test_class_cache(void);
void test_content_names(void);
void test_booted_and_root(void);
void test_recovery(void);
void test_wdt(void);
void test_wdt_hint(void);
void test_rename_rmdir(void);
void test_finalize(void);
void test_entry_helpers(void);
void test_meta_helpers(void);
void test_transplog_payload_digest(void);
void test_slot_names(void);
void test_link_corners(void);
void test_index(void);
void test_dir_reads(void);
void test_slot_limit(void);

/* select tests */
void test_select(void);
void test_recovery_unlock(void);
void test_select_recovery(void);
void test_select_recovery_only(void);
void test_fallback(void);
void test_preview(void);
void test_select_watchdog_version(void);
void test_full_store(void);
void test_state_on_full_store(void);
void test_failed_commit(const char *base);
void test_recycle_pick(void);
void test_evict_blocks(void);
void test_evict_plan(void);
void test_evict_execute(void);
void test_slots_scalar(void);
void test_boot_delay(void);
void test_quota_scalar(void);
void test_loop_guard(void);
void test_loop_demote_to_older(void);
void test_loop_demote_to_recovery(void);
void test_loop_no_other_way(void);
void test_loop_not_for_trial_or_preview(void);
void test_condemn(void);
void test_select_label(void);
void test_select_entry(void);
void test_entries_list(void);

/* fi tests */
void test_fault_injection(const char *base);
void fi_finalize(const char *base, const char *what, int with_counter);
void fi_bridge(const char *base, int again);
void fi_select(const char *base, int fallback);
void fi_make_room(const char *base);
void test_make_room_compacts(const char *base);
void fi_recovery(const char *base, int requested);
void fi_condemn(const char *base);
void test_read_error(const char *base);
void test_read_error_sweep(const char *base);

/* check tests */
void test_check_repair(const char *base);
void test_check_record(const char *base);
void test_record_damaged(const char *base);
void test_slotstate(void);
void test_slotstate_health(void);
void test_slotstate_roster(void);

/* sweep tests */
void test_sweep(const char *base);
void test_blob_check(const char *base);

/* anchor tests */
void test_anchor(const char *base);

#endif
