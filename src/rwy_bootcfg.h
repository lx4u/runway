/*
 * rwy_bootcfg.h — names and helpers shared by the boot configuration files
 * (rwy_bootcfg.c, rwy_bootsel.c, rwy_bootaudit.c). Internal.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_BOOTCFG_H
#define RWY_BOOTCFG_H

#include "rwy_internal.h"
#include "rwy_flight.h"
#include "rwy_transplog.h"
#include "rwy_entry.h"
#include "rwy_meta.h"

#define ROOT_PATH "EFI/RUNWAY"
#define ARGS_MAX 4096u
#define LABEL_MAX RWY_LABEL_MAX
#define META_MAX RWY_META_MAX
#define SCALAR_MAX 64u
#define DELAY_MAX 30u   /* BOOT-DELAY, seconds */
#define SLOTS_MAX 64u   /* CONFIG<n> directories; RECOVERY and BLOBS are not slots */
#define QUOTA_MIN 10u   /* QUOTA, percent of the usable pool */
#define QUOTA_MAX 95u
#define LOOP_LIMIT_MIN 2u    /* LOOP-LIMIT, entries inside the window */
#define LOOP_LIMIT_MAX 255u
#define LOOP_LIMIT_DEF 5u
#define LOOP_WINDOW_MIN 30u  /* LOOP-WINDOW, seconds */
#define LOOP_WINDOW_MAX 86400u
#define LOOP_WINDOW_DEF 600u

#define WDT_APP_MAX (1024u * 1024u)   /* bytes per watchdog application */

enum { W_OUT, W_ROOT, W_BLOBS, W_SLOT, W_RECOVERY, W_WDT };
enum { K_NONE, K_VERSION, K_REVISION, K_WATCHDOG, K_SLOTS, K_BOOT_DELAY, K_QUOTA,
       K_LOOP_GUARD, K_LOOP_LIMIT, K_LOOP_WINDOW, K_STATE, K_TRIES, K_ARGS, K_LABEL, K_ENTRY, K_META, K_PAYLOAD, K_BLOB, K_WDT_APP,
       K_DIR_BLOBS, K_DIR_SLOT, K_DIR_RECOVERY, K_DIR_WDT };
enum { ST_INSTALLED = RWY_SS_INSTALLED, ST_OK = RWY_SS_OK, ST_TESTING = RWY_SS_TESTING, ST_FAILED = RWY_SS_FAILED };

/* Selection state; the audit reuses its message buffer. */
typedef struct {
    rwy_store_t *s;
    rwy_log_fn fn;
    void *ctx;
    uint64_t t, rdir;       /* EFI/RUNWAY, and a usable RECOVERY (0 if none) */
    rwy_guard_fn guard;     /* boot loop guard, NULL when off or in a preview */
    void *gctx;
    int dirty, req, requested, failed;   /* failed: a slot became FAILED in this pass */
    int loop;               /* a slot was demoted for a boot loop in this pass */
    char b[160];
    size_t n;
    char held[2][160];      /* decision lines, logged once the record is written */
    int nheld;
    uint8_t rec[RWY_REC_PAYLOAD];   /* the record as selection would leave it */
} sel_t;

/* rwy_bootcfg.c */
int bc_is_system(const rwy_store_t *s);
uint64_t bc_active(rwy_store_t *s);
int bc_dir_kind(const char *n, uint32_t l);
int bc_entry_kind(int where, const char *n, uint32_t l);
int bc_type_ok(int kind, uint8_t type);
int bc_slot_final(rwy_store_t *s, uint64_t slot);
int bc_next_dir(rwy_store_t *s, uint64_t t, size_t *pos, char *name, uint32_t *len, uint64_t *oid);
size_t bc_strip_nl(const uint8_t *b, size_t len);
int bc_is_dec_u64(const uint8_t *b, size_t len);
int bc_setting_check(int kind, const uint8_t *b, size_t len, const char **why);
int bc_loop_guard_state(const uint8_t *b, size_t len);   /* 1 on, 0 off, -1 not a setting */
int bc_state_id(const uint8_t *b, size_t len);
int bc_is_scalar(int kind);
int bc_label_ok(const uint8_t *b, size_t len);
int bc_read_label(rwy_store_t *s, uint64_t dir, char *out);
int bc_read_entry(rwy_store_t *s, uint64_t dir, char *out);
int bc_read_meta(rwy_store_t *s, uint64_t dir, char *out, size_t *out_len);
int bc_payload_ok(rwy_store_t *s, const char *path, int data);
int bc_read_text(rwy_store_t *s, uint64_t dir, const char *name, uint8_t *b, size_t *n);
int bc_read_u64(rwy_store_t *s, uint64_t dir, const char *name, uint64_t *v);
int bc_put_u64(rwy_store_t *s, const char *path, uint64_t v);
void bc_record_load(rwy_store_t *s, uint8_t *r);
int bc_blob_orphan(rwy_store_t *s, uint64_t blobs, const char *name);   /* BLOBS/name is a content blob that only its BLOBS name links */

/* rwy_bootsel.c */
void bc_cat(sel_t *x, const char *t);
void bc_emit(sel_t *x);
const char *bc_dec(char *d, uint64_t v);   /* d holds >= 21 bytes */
int bc_note(rwy_store_t *s, int rc);
const char *bc_payload_fault(rwy_store_t *s, uint64_t dir, const char *name);
const char *bc_slot_fault(rwy_store_t *s, uint64_t dir, const char *name, uint64_t *rev);
void bc_sel_path(char *path, const char *slot, const char *file);
rwy_ss_entry_t bc_ss_entry(const uint8_t *rec, uint64_t rev);
void bc_cat_label_str(sel_t *x, const char *label);
void bc_cat_label_dir(sel_t *x, uint64_t dir);
extern const char *const BC_STATE_NAMES[];
extern const char *const BC_CAUSE_NAMES[];   /* CAUSE text of code n + 1 */

#endif
