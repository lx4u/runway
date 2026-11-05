/*
 * rwy_bootcfg.c — EFI/RUNWAY policy layer (policy layer).
 * SPDX-License-Identifier: Apache-2.0
 *
 * The directory EFI/RUNWAY holds boot configuration as plain files. This file
 * is the single source of truth for what may exist there and who may change
 * it; FUSE, the EFI adapter, mkstore and RUNWAY.EFI all reach it through the
 * core mutators, so they share one implementation. A store without
 * EFI/RUNWAY is never affected: every hook returns RWY_OK at once.
 *
 *   EFI/RUNWAY/{VERSION,REVISION,WATCHDOG,SLOTS,BOOT-DELAY,QUOTA,LOOP-GUARD,LOOP-LIMIT,LOOP-WINDOW,RECOVER_NEXT,REASON,BOOTED}
 *   EFI/RUNWAY/BLOBS/<file>
 *   EFI/RUNWAY/CONFIG<n>/{KERNEL,INITRD,DTB,ARGS,STATE,TRIES,WATCHDOG,REVISION}
 *   EFI/RUNWAY/RECOVERY/...        sealed
 *   EFI/RUNWAY/WDT/<name>.EFI      sealed
 *
 * Enforcement is by object: adapters write by OID and payloads can be
 * hardlinked, so a mutator classifies the OID by finding its names in the
 * subtree. Path rules apply to namespace operations only.
 */
#include "rwy_bootcfg.h"

#define LAYOUT_VERSION "1\n"
#define LINK_HOPS 8u

const char *const BC_STATE_NAMES[] = { "INSTALLED", "OK", "TESTING", "FAILED" };
/* CAUSE text of record code n + 1. */
const char *const BC_CAUSE_NAMES[] = { "tries_used_up", "load_error", "operator", "boot_loop", "bad_blob" };

static const char *const SLOT_FILES[] = { "KERNEL", "INITRD", "DTB", "ARGS", "LABEL", "ENTRY", "META", "STATE", "TRIES", "WATCHDOG", "REVISION" };
static const int SLOT_KINDS[] = { K_PAYLOAD, K_PAYLOAD, K_PAYLOAD, K_ARGS, K_LABEL, K_ENTRY, K_META, K_STATE, K_TRIES, K_WATCHDOG, K_REVISION };
static const char *const ROOT_FILES[] = { "VERSION", "REVISION", "WATCHDOG", "SLOTS", "BOOT-DELAY", "QUOTA",
                                          "LOOP-GUARD", "LOOP-LIMIT", "LOOP-WINDOW" };
static const int ROOT_KINDS[] = { K_VERSION, K_REVISION, K_WATCHDOG, K_SLOTS, K_BOOT_DELAY, K_QUOTA,
                                  K_LOOP_GUARD, K_LOOP_LIMIT, K_LOOP_WINDOW };
#define N_SLOT_FILES (sizeof(SLOT_FILES) / sizeof(SLOT_FILES[0]))
#define N_ROOT_FILES (sizeof(ROOT_FILES) / sizeof(ROOT_FILES[0]))

/* What a name in the subtree says about the object behind it. */
typedef struct { uint8_t in_tree, where, kind, fin, sealed, frozen, blob, content; } cls_t;

void rwy_bootcfg_invalidate(rwy_store_t *s) {
    if (s) {
        s->bc_root_valid = s->bc_cls_valid = 0;
    }
}

void rwy_bootcfg_set_priv(rwy_store_t *s, int on) { if (s) s->bc_priv = on; }

int bc_is_system(const rwy_store_t *s) { return s->actor == RWY_ACTOR_SYSTEM; }

static int unlocked(const rwy_store_t *s) { return s->bc_unlock && bc_is_system(s); }

int rwy_bootcfg_unlock_recovery(rwy_store_t *s, int on) {
    if (!s) return RWY_ERR_INVAL;
    if (!bc_is_system(s)) return RWY_ERR_PERM;
    s->bc_unlock = on != 0;
    return RWY_OK;
}

static int eq(const char *n, uint32_t l, const char *lit) {
    return rwy_strlen(lit) == l && rwy_strncmp(n, lit, l) == 0;
}

/* OID of EFI/RUNWAY, or 0 when the feature is off (or no transaction is open). */
uint64_t bc_active(rwy_store_t *s) {
    uint64_t o;
    const rwy_inode_t *in;
    if (!s->in_txn) return 0;
    if (!s->bc_root_valid) {
        int rc = rwy_resolve_ext(s, ROOT_PATH, 1, 0, &o);
        s->bc_root = 0;
        if (rc == RWY_OK && (in = rwy_inode_of(s, o, 1)) != 0 && in->type == RWY_TYPE_DIR)
            s->bc_root = o;
        if (rc != RWY_OK && rc != RWY_ERR_NOENT && rc != RWY_ERR_NOTDIR) {
            s->bc_err = rc;   /* not cached: the next call looks again */
            return 0;
        }
        s->bc_root_valid = 1;
    }
    return s->bc_root;
}

/* Remember a read error met inside the subtree; a missing or malformed entry is not one. */
int bc_note(rwy_store_t *s, int rc) {
    if (rc == RWY_ERR_IO || rc == RWY_ERR_CORRUPT) s->bc_err = rc;
    return rc;
}

/* CONFIG<n>, n a canonical decimal u64 (no leading zeros, no overflow). */
static int slot_name(const char *n, uint32_t l) {
    uint32_t i, d;
    if (l < 7 || l > 6 + 20 || rwy_strncmp(n, "CONFIG", 6) != 0) return 0;
    d = l - 6;
    for (i = 6; i < l; i++) if (n[i] < '0' || n[i] > '9') return 0;
    if (d > 1 && n[6] == '0') return 0;
    if (d == 20 && rwy_strncmp(n + 6, "18446744073709551615", 20) > 0) return 0;
    return 1;
}

/* A content name: 64 lower case hex digits. */
int rwy_bootcfg_content_name(const char *n, size_t l) {
    size_t i;
    if (l != 64) return 0;
    for (i = 0; i < l; i++)
        if (!((n[i] >= '0' && n[i] <= '9') || (n[i] >= 'a' && n[i] <= 'f'))) return 0;
    return 1;
}

/* A watchdog application name: 1 or more of [A-Za-z0-9_-], then ".EFI" in any case, 32 characters at most. */
static int wdt_name_ok(const char *n, uint32_t l) {
    uint32_t i, stem;
    if (l <= 4 || l > RWY_WDT_NAME_MAX) return 0;
    stem = l - 4;
    for (i = 0; i < stem; i++) {
        char c = n[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return n[stem] == '.' && (n[stem + 1] | 0x20) == 'e' && (n[stem + 2] | 0x20) == 'f' && (n[stem + 3] | 0x20) == 'i';
}

int bc_dir_kind(const char *n, uint32_t l) {
    if (eq(n, l, "BLOBS")) return K_DIR_BLOBS;
    if (eq(n, l, "RECOVERY")) return K_DIR_RECOVERY;
    if (eq(n, l, "WDT")) return K_DIR_WDT;
    if (slot_name(n, l)) return K_DIR_SLOT;
    return K_NONE;
}

static int where_of_dir_kind(int k) {
    switch (k) {
    case K_DIR_BLOBS: return W_BLOBS;
    case K_DIR_RECOVERY: return W_RECOVERY;
    case K_DIR_WDT: return W_WDT;
    case K_DIR_SLOT: return W_SLOT;
    default: return W_OUT;
    }
}

/* Kind of entry `n` in a directory with role `where`; K_NONE if the schema has no such name. */
int bc_entry_kind(int where, const char *n, uint32_t l) {
    unsigned i;
    if (where == W_BLOBS) return K_BLOB;
    if (where == W_WDT) return wdt_name_ok(n, l) ? K_WDT_APP : K_NONE;
    if (where == W_ROOT) {
        for (i = 0; i < N_ROOT_FILES; i++) if (eq(n, l, ROOT_FILES[i])) return ROOT_KINDS[i];
        return bc_dir_kind(n, l);
    }
    for (i = 0; i < N_SLOT_FILES; i++) if (eq(n, l, SLOT_FILES[i])) return SLOT_KINDS[i];
    return K_NONE;
}

int bc_type_ok(int kind, uint8_t type) {
    switch (kind) {
    case K_DIR_BLOBS: case K_DIR_SLOT: case K_DIR_RECOVERY: case K_DIR_WDT: return type == RWY_TYPE_DIR;
    case K_PAYLOAD: return type == RWY_TYPE_FILE || type == RWY_TYPE_SYMLINK;
    default: return type == RWY_TYPE_FILE;
    }
}

/* Put `name` in the sorted list of `*n` names if it ranks among the first `max`. */
static void wdt_insert(char names[][RWY_WDT_NAME_MAX + 1], size_t *n, size_t max, const char *name) {
    size_t p, k;
    for (p = 0; p < *n && rwy_strcmp(names[p], name) < 0; p++) {}
    if (p >= max) return;
    if (*n < max) (*n)++;
    for (k = *n - 1; k > p; k--) memcpy(names[k], names[k - 1], RWY_WDT_NAME_MAX + 1);
    memcpy(names[p], name, rwy_strlen(name) + 1);
}

int rwy_bootcfg_wdt_list(rwy_store_t *s, char names[][RWY_WDT_NAME_MAX + 1], size_t max) {
    char name[RWY_NAME_MAX + 1];
    uint8_t type;
    size_t i, kept = 0;
    int rc, total = 0;
    if (!s || (max && !names)) return RWY_ERR_INVAL;
    for (i = 0; (rc = rwy_readdir_nth(s, ROOT_PATH "/WDT", i, name, sizeof(name), &type)) == RWY_OK; i++) {
        if (type != RWY_TYPE_FILE || !wdt_name_ok(name, (uint32_t)rwy_strlen(name))) continue;
        total++;
        wdt_insert(names, &kept, max, name);
    }
    return rc == RWY_ERR_NOENT || rc == RWY_ERR_NOTDIR ? total : rc;
}

size_t rwy_bootcfg_wdt_hint(rwy_store_t *s, char *name) {
    uint8_t r[RWY_REC_PAYLOAD];
    name[0] = 0;
    if (!s || rwy_record_read(s, r) != RWY_OK) return 0;
    return rwy_ss_wdt(r, name);
}

int rwy_bootcfg_wdt_set_hint(rwy_store_t *s, const char *name) {
    uint8_t r[RWY_REC_PAYLOAD];
    char cur[RWY_WDT_NAME_MAX + 1];
    int rc;
    if (!s) return RWY_ERR_INVAL;
    rc = rwy_record_read(s, r);
    if (rc) return rc;
    (void)rwy_ss_wdt(r, cur);
    if (rwy_strcmp(cur, name ? name : "") == 0) return RWY_OK;
    rc = rwy_ss_set_wdt(r, name);
    return rc ? rc : rwy_record_write(s, r);
}

int bc_slot_final(rwy_store_t *s, uint64_t slot) {
    return rwy_dir_lookup(s, slot, "REVISION", 8, 0, 0) == RWY_OK;
}

/* Next subdirectory of EFI/RUNWAY. *pos is the byte offset in the directory; start with 0. */
int bc_next_dir(rwy_store_t *s, uint64_t t, size_t *pos, char *name, uint32_t *len, uint64_t *oid) {
    uint64_t off = *pos;
    uint8_t type;
    int rc;
    while ((rc = rwy_dir_next(s, t, &off, name, len, oid, &type)) == RWY_OK) {
        *pos = (size_t)off;
        if (type == RWY_TYPE_DIR) return 1;
    }
    *pos = (size_t)off;
    if (rc != RWY_ERR_NOENT) s->bc_err = rc;
    return 0;
}

/* Role of directory `oid` inside the subtree; *fin is set for a finalized slot. */
static int dir_where(rwy_store_t *s, uint64_t t, uint64_t oid, int *fin) {
    char name[RWY_NAME_MAX + 1];
    uint32_t l;
    uint64_t o;
    size_t i = 0;
    if (fin) *fin = 0;
    if (oid == t) return W_ROOT;
    while (bc_next_dir(s, t, &i, name, &l, &o)) {
        int w;
        if (o != oid) continue;
        w = where_of_dir_kind(bc_dir_kind(name, l));
        if (w == W_SLOT && fin) *fin = bc_slot_final(s, oid);
        return w;
    }
    return W_OUT;
}

/* Kinds that freeze when finalized in a slot or shared across links. */
static int frozen_kind(int kind) {
    return kind == K_PAYLOAD || kind == K_BLOB || kind == K_ARGS || kind == K_LABEL ||
           kind == K_ENTRY || kind == K_META;
}

static int mark_wins(int kind) {
    return kind == K_STATE || kind == K_TRIES || kind == K_WATCHDOG ||
           kind == K_REVISION || kind == K_ARGS || kind == K_LABEL ||
           kind == K_ENTRY || kind == K_META;
}

static void mark(cls_t *c, int where, int kind, int fin) {
    /* A scalar identity wins over the payload one when an object has several names. */
    if (!c->in_tree || c->kind == K_WDT_APP || mark_wins(kind)) {
        c->kind = (uint8_t)kind;
        c->where = (uint8_t)where;
        c->fin = (uint8_t)fin;
    }
    c->in_tree = 1;
    if (where == W_BLOBS) c->blob = 1;
    if (where == W_RECOVERY || where == W_WDT) c->sealed = 1;
    if (where == W_SLOT && fin && frozen_kind(kind)) c->frozen = 1;
}

typedef char cls_fits_store[sizeof(cls_t) <= sizeof(((rwy_store_t *)0)->bc_cls) ? 1 : -1];

/* Names of `oid` in a slot or RECOVERY directory. */
static void classify_slot_dir(rwy_store_t *s, cls_t *c, uint64_t dir, int where, uint64_t oid) {
    int fin = bc_slot_final(s, dir);
    uint64_t f;
    unsigned k;

    for (k = 0; k < N_SLOT_FILES; k++)
        if (rwy_dir_lookup(s, dir, SLOT_FILES[k], (uint32_t)rwy_strlen(SLOT_FILES[k]), &f, 0) == RWY_OK && f == oid)
            mark(c, where, SLOT_KINDS[k], fin);
}

/* Names of `oid` in BLOBS. */
static void classify_blobs(rwy_store_t *s, cls_t *c, uint64_t dir, uint64_t oid) {
    char bn[RWY_NAME_MAX + 1];
    uint64_t off = 0, f;
    uint32_t l;
    uint8_t type;

    while (rwy_dir_next(s, dir, &off, bn, &l, &f, &type) == RWY_OK)
        if (f == oid) {
            mark(c, W_BLOBS, K_BLOB, 0);
            if (rwy_bootcfg_content_name(bn, l)) c->content = 1;
        }
}

/* Names of `oid` in WDT. */
static void classify_wdt(rwy_store_t *s, cls_t *c, uint64_t dir, uint64_t oid) {
    char wn[RWY_NAME_MAX + 1];
    uint64_t off = 0, f;
    uint32_t l;
    uint8_t type;

    while (rwy_dir_next(s, dir, &off, wn, &l, &f, &type) == RWY_OK)
        if (f == oid) mark(c, W_WDT, K_WDT_APP, 0);
}

/* Names of `oid` in the root of the subtree. */
static void classify_root(rwy_store_t *s, cls_t *c, uint64_t t, uint64_t oid) {
    uint64_t f;
    unsigned k;

    for (k = 0; k < N_ROOT_FILES; k++)
        if (rwy_dir_lookup(s, t, ROOT_FILES[k], (uint32_t)rwy_strlen(ROOT_FILES[k]), &f, 0) == RWY_OK && f == oid)
            mark(c, W_ROOT, ROOT_KINDS[k], 0);
}

static void classify_walk(rwy_store_t *s, uint64_t t, uint64_t oid, cls_t *c) {
    char name[RWY_NAME_MAX + 1];
    const rwy_inode_t *in = rwy_inode_of(s, oid, 1);
    uint32_t l;
    uint64_t o;
    size_t i = 0;
    c->in_tree = c->where = c->kind = c->fin = c->sealed = c->frozen = c->blob = c->content = 0;
    classify_root(s, c, t, oid);
    while (bc_next_dir(s, t, &i, name, &l, &o)) {
        int w = where_of_dir_kind(bc_dir_kind(name, l));
        if (w == W_SLOT || w == W_RECOVERY)
            classify_slot_dir(s, c, o, w, oid);
        else if (w == W_WDT)
            classify_wdt(s, c, o, oid);   /* sealed even when unshared: always scanned */
        else if (w == W_BLOBS && in)
            classify_blobs(s, c, o, oid);
    }
    /* Any object with a name in the subtree and another link anywhere is shared, hence frozen. */
    if (c->in_tree && in && in->nlink > 1 && frozen_kind(c->kind))
        c->frozen = 1;
}

/* Find every name of `oid` inside the subtree.
 * Writes (deep == 0) keep the result for the last OID: it depends only on the names
 * pointing at the OID and on REVISION, which change only through the namespace
 * events that call rwy_bootcfg_invalidate(). */
static void classify(rwy_store_t *s, uint64_t t, uint64_t oid, int deep, cls_t *c) {
    unsigned k;
    if (!deep && s->bc_cls_valid && s->bc_cls_oid == oid) {
        for (k = 0; k < sizeof(*c); k++) ((uint8_t *)c)[k] = s->bc_cls[k];
        return;
    }
    classify_walk(s, t, oid, c);
    if (!deep) {
        s->bc_cls_valid = 1;
        s->bc_cls_oid = oid;
        for (k = 0; k < sizeof(*c); k++) s->bc_cls[k] = ((const uint8_t *)c)[k];
    }
}

/* ---- directory roles for the virtual files ------------- */

/* Role of directory `dir`, for readers: works with or without a transaction. */
int rwy_bootcfg_role(rwy_store_t *s, uint64_t dir, char *slot) {
    uint64_t t, o;
    const rwy_inode_t *in;
    uint32_t l;
    size_t i = 0;
    if (s->bc_novirt) return RWY_ROLE_NONE;
    if (s->in_txn) t = bc_active(s);
    else t = (rwy_resolve_ext(s, ROOT_PATH, 0, 0, &o) == RWY_OK && (in = rwy_inode_of(s, o, 0)) != 0 &&
              in->type == RWY_TYPE_DIR) ? o : 0;
    if (!t) return RWY_ROLE_NONE;
    if (dir == t) return RWY_ROLE_ROOT;
    while (bc_next_dir(s, t, &i, slot, &l, &o))
        if (o == dir && slot_name(slot, l)) return RWY_ROLE_SLOT;
    return RWY_ROLE_NONE;
}

/* ---- value rules ------------------------------------------------------- */

size_t bc_strip_nl(const uint8_t *b, size_t len) {
    return (len && b[len - 1] == '\n') ? len - 1 : len;
}

int bc_is_dec_u64(const uint8_t *b, size_t len) {
    size_t i;
    if (len == 0 || len > 20) return 0;
    for (i = 0; i < len; i++) if (b[i] < '0' || b[i] > '9') return 0;
    return len < 20 || rwy_strncmp((const char *)b, "18446744073709551615", 20) <= 0;
}

/* A decimal number from `lo` to `hi`. */
static int dec_in_range(const uint8_t *b, size_t len, unsigned lo, unsigned hi) {
    size_t i;
    unsigned v = 0;
    if (!bc_is_dec_u64(b, len)) return 0;
    for (i = 0; i < len && v <= hi; i++) v = v * 10 + (unsigned)(b[i] - '0');
    return v >= lo && v <= hi;
}

/* LOOP-GUARD: "on" or "off". */
int bc_loop_guard_state(const uint8_t *b, size_t len) {
    if (len == 2 && !rwy_strncmp((const char *)b, "on", 2)) return 1;
    if (len == 3 && !rwy_strncmp((const char *)b, "off", 3)) return 0;
    return -1;
}

static int loop_guard_ok(const uint8_t *b, size_t len, unsigned lo, unsigned hi) {
    (void)lo; (void)hi;
    return bc_loop_guard_state(b, len) >= 0;
}

/* The global settings whose value has a range, and what the audit says about a bad one. */
static const struct {
    int kind;
    int (*ok)(const uint8_t *b, size_t len, unsigned lo, unsigned hi);
    unsigned lo, hi;
    const char *why;
} SETTINGS[] = {
    { K_BOOT_DELAY, dec_in_range, 0, DELAY_MAX, "not a number from 0 to 30" },
    { K_SLOTS, dec_in_range, 1, SLOTS_MAX, "not a number from 1 to 64" },
    { K_QUOTA, dec_in_range, QUOTA_MIN, QUOTA_MAX, "not a number from 10 to 95" },
    { K_LOOP_GUARD, loop_guard_ok, 0, 0, "not on or off" },
    { K_LOOP_LIMIT, dec_in_range, LOOP_LIMIT_MIN, LOOP_LIMIT_MAX, "not a number from 2 to 255" },
    { K_LOOP_WINDOW, dec_in_range, LOOP_WINDOW_MIN, LOOP_WINDOW_MAX, "not a number from 30 to 86400" },
};

/* 1 if `b` is a good value for the setting `kind`, 0 if not (`*why` says), -1 if `kind` has no range. */
int bc_setting_check(int kind, const uint8_t *b, size_t len, const char **why) {
    size_t i;
    for (i = 0; i < sizeof(SETTINGS) / sizeof(SETTINGS[0]); i++) {
        if (SETTINGS[i].kind != kind) continue;
        *why = SETTINGS[i].why;
        return SETTINGS[i].ok(b, len, SETTINGS[i].lo, SETTINGS[i].hi);
    }
    return -1;
}

int bc_label_ok(const uint8_t *b, size_t len) {
    size_t i;
    if (len > RWY_LABEL_MAX) return 0;
    if (!b) return len == 0;
    for (i = 0; i < len; i++) {
        uint8_t c = b[i];
        if (c < 0x20 || c == 0x7f || c == '"') return 0;
    }
    return 1;
}

int bc_state_id(const uint8_t *b, size_t len) {
    unsigned i;
    for (i = 0; i < 4; i++)
        if (rwy_strlen(BC_STATE_NAMES[i]) == len && rwy_strncmp((const char *)b, BC_STATE_NAMES[i], len) == 0) return (int)i;
    return -1;
}

int bc_is_scalar(int kind) {
    return kind == K_STATE || kind == K_TRIES || kind == K_WATCHDOG || kind == K_SLOTS || kind == K_BOOT_DELAY ||
           kind == K_QUOTA || kind == K_LOOP_GUARD || kind == K_LOOP_LIMIT || kind == K_LOOP_WINDOW ||
           kind == K_REVISION || kind == K_ENTRY;
}

/* Who may change a scalar at all (the value is checked separately for STATE). */
static int scalar_perm(const rwy_store_t *s, const cls_t *c) {
    switch (c->kind) {
    case K_TRIES: return (!c->fin || bc_is_system(s)) ? RWY_OK : RWY_ERR_PERM;
    case K_WATCHDOG: return (c->where == W_SLOT && c->fin) ? RWY_ERR_PERM : RWY_OK;
    default: return RWY_OK;
    }
}

/* Shared by write and truncate: refuses what no value could make acceptable. */
static int object_perm(const rwy_store_t *s, const cls_t *c) {
    if ((c->sealed && !unlocked(s)) || c->frozen || c->content) return RWY_ERR_PERM;
    switch (c->kind) {
    case K_REVISION: return c->where == W_RECOVERY ? RWY_OK : RWY_ERR_PERM;   /* ignored by selection */
    case K_VERSION: return RWY_ERR_PERM;
    default: return bc_is_scalar(c->kind) ? scalar_perm(s, c) : RWY_OK;
    }
}

/* Number of CONFIG<n> directories, finalized or not. */
static unsigned slot_count(rwy_store_t *s, uint64_t t) {
    char name[RWY_NAME_MAX + 1];
    uint32_t l;
    uint64_t o;
    size_t i = 0;
    unsigned n = 0;
    while (bc_next_dir(s, t, &i, name, &l, &o)) if (bc_dir_kind(name, l) == K_DIR_SLOT) n++;
    return n;
}

/* ---- namespace hooks --------------------------------------------------- */

static int role_of_where(int where) {
    if (where == W_ROOT) return RWY_ROLE_ROOT;
    if (where == W_SLOT) return RWY_ROLE_SLOT;
    return RWY_ROLE_NONE;
}

static int linkable_kind(int kind) {
    return kind == K_PAYLOAD || kind == K_BLOB || kind == K_WDT_APP;
}

/* May the entry exist at all (name, type, links, slot limit)? Sets *kind. */
static int create_shape(rwy_store_t *s, uint64_t t, int where, int fin, const char *base, uint32_t blen,
                        uint8_t type, int is_link, int *kind) {
    if (rwy_virt_reserved(role_of_where(where), fin, base, blen))
        return RWY_ERR_PERM;   /* virtual */
    *kind = bc_entry_kind(where, base, blen);
    if (*kind == K_NONE || !bc_type_ok(*kind, type)) return RWY_ERR_INVAL;
    if (where == W_BLOBS && type != RWY_TYPE_FILE) return RWY_ERR_INVAL;
    if (is_link && !linkable_kind(*kind)) return RWY_ERR_INVAL;
    if (*kind == K_DIR_SLOT && !s->bc_priv && slot_count(s, t) >= SLOTS_MAX) return RWY_ERR_NOSPC;
    return RWY_OK;
}

/* RECOVERY and WDT are sealed: only SYSTEM with the unlock may change them. */
static int sealed_dir(int where, int kind) {
    return where == W_RECOVERY || where == W_WDT || kind == K_DIR_RECOVERY || kind == K_DIR_WDT;
}

/* Who may create it: only SYSTEM makes a content name, RECOVERY and WDT need the unlock, VERSION and REVISION are never created, a finalized slot is closed. */
static int create_perm(const rwy_store_t *s, int where, int kind, int fin, const char *base, uint32_t blen) {
    if (s->bc_priv) return RWY_OK;
    if (where == W_BLOBS && rwy_bootcfg_content_name(base, blen) && !bc_is_system(s)) return RWY_ERR_PERM;
    if (sealed_dir(where, kind)) return unlocked(s) ? RWY_OK : RWY_ERR_PERM;
    if (kind == K_VERSION || kind == K_REVISION) return RWY_ERR_PERM;
    return where == W_SLOT && fin ? RWY_ERR_PERM : RWY_OK;
}

int rwy_bootcfg_check_create(rwy_store_t *s, uint64_t parent, const char *base, uint32_t blen,
                              uint8_t type, int is_link, uint32_t *mode, uint32_t *uid, uint32_t *gid) {
    uint64_t t = bc_active(s);
    int fin, where, kind, rc;
    if (!t) return RWY_OK;
    where = dir_where(s, t, parent, &fin);
    if (where == W_OUT) return RWY_OK;

    rc = create_shape(s, t, where, fin, base, blen, type, is_link, &kind);
    if (rc) return rc;
    rc = create_perm(s, where, kind, fin, base, blen);
    if (rc) return rc;
    /* Fixed mode and owner, whatever the caller asked for. */
    if (uid) *uid = RWY_EFI_DEFAULT_UID;
    if (gid) *gid = RWY_EFI_DEFAULT_GID;
    if (mode) *mode = type == RWY_TYPE_DIR ? (RWY_S_IFDIR | RWY_EFI_DEFAULT_MODE_DIR)
                                            : (RWY_S_IFREG | RWY_EFI_DEFAULT_MODE_FILE);
    return RWY_OK;
}

int rwy_bootcfg_check_link(rwy_store_t *s, uint64_t old_oid, uint64_t parent, const char *base, uint32_t blen) {
    uint64_t t = bc_active(s);
    const rwy_inode_t *in;
    cls_t c;
    int to_wdt;
    if (!t) return RWY_OK;
    to_wdt = dir_where(s, t, parent, 0) == W_WDT;
    /* Only payloads and blobs may gain a second name; a scalar must keep exactly one. */
    classify(s, t, old_oid, to_wdt, &c);
    if (c.in_tree && c.kind != K_PAYLOAD && c.kind != K_BLOB) return RWY_ERR_PERM;
    in = rwy_inode_of(s, old_oid, 1);
    if (!in) return RWY_ERR_NOENT;
    /* WDT takes a second name of a blob only, so an application is as immutable as the blob. */
    if (to_wdt && !c.blob) return RWY_ERR_PERM;
    if (to_wdt && in->size > WDT_APP_MAX) return RWY_ERR_INVAL;
    return rwy_bootcfg_check_create(s, parent, base, blen, in->type, 1, 0, 0, 0);
}

/* True when the slot `base` of EFI/RUNWAY `t` is pinned. */
static int slot_pinned(rwy_store_t *s, uint64_t t, const char *base, uint32_t blen) {
    uint64_t o, rev;
    if (rwy_dir_lookup(s, t, base, blen, &o, 0) != RWY_OK || !bc_read_u64(s, o, "REVISION", &rev)) return 0;
    return bc_ss_entry(s->rec, rev).pinned;
}

int rwy_bootcfg_check_remove(rwy_store_t *s, uint64_t parent, const char *base, uint32_t blen) {
    uint64_t t = bc_active(s);
    int fin, where, kind;
    if (!t || s->bc_priv) return RWY_OK;
    where = dir_where(s, t, parent, &fin);
    if (where == W_OUT) return RWY_OK;
    kind = bc_entry_kind(where, base, blen);
    if (where == W_BLOBS && rwy_bootcfg_content_name(base, blen) && !bc_is_system(s)) return RWY_ERR_PERM;
    if (sealed_dir(where, kind)) return unlocked(s) ? RWY_OK : RWY_ERR_PERM;
    if (kind == K_DIR_SLOT) {
        if (slot_pinned(s, t, base, blen)) return RWY_ERR_BUSY;
        s->bc_slot_removed = 1;
    }
    switch (kind) {
    case K_VERSION: return RWY_ERR_PERM;
    case K_REVISION: return where == W_ROOT ? RWY_ERR_PERM : 1;   /* a slot's own goes with its rmdir */
    default: return RWY_OK;   /* unknown entries stay removable so a bad tree can be cleaned up */
    }
}

static void empty_slot_transplog_record(rwy_store_t *s, uint64_t dir, const char *path, const char *name) {
    char label[RWY_LABEL_MAX + 1] = { 0 };
    uint8_t payload_hash[32] = { 0 };
    uint64_t rev = 0;
    int known = bc_read_u64(s, dir, "REVISION", &rev);

    (void)bc_read_label(s, dir, label);
    (void)rwy_transplog_calc_payload_hash(s, path, payload_hash);
    if (s->bc_evict_reason)
        (void)rwy_transplog_record_evict(s, name, rev, label, payload_hash, known ? (uint8_t)bc_ss_entry(s->rec, rev).state : 0,
                                         known ? s->bc_evict_reason : RWY_EVICT_REASON_INCOMPLETE, s->bc_evict_blocks);
    else
        (void)rwy_transplog_record(s, name, RWY_JOURNAL_EVENT_REMOVE, rev, label, payload_hash, s->rec_seq, NULL);
}

/* child = dir "/" name; the caller has checked that it fits. */
static void child_path(char *child, const char *dir, size_t dl, const char *name) {
    size_t nl = rwy_strlen(name);
    for (size_t i = 0; i < dl; i++) child[i] = dir[i];
    child[dl] = '/';
    for (size_t i = 0; i <= nl; i++) child[dl + 1 + i] = name[i];
}

/* rmdir of a slot removes the real entries it holds, so REVISION never has to be unlinked on its own. */
int rwy_bootcfg_empty_slot(rwy_store_t *s, uint64_t dir, const char *path) {
    char name[RWY_NAME_MAX + 1], child[RWY_MAX_PATH + 1];
    size_t pl = rwy_strlen(path);
    int priv = s->bc_priv, rc = RWY_OK;
    if (!bc_active(s) || priv || rwy_bootcfg_role(s, dir, name) != RWY_ROLE_SLOT) return RWY_OK;
    if (pl + 1 + RWY_NAME_MAX > RWY_MAX_PATH) return RWY_ERR_NAMETOOLONG;
    empty_slot_transplog_record(s, dir, path, name);
    s->bc_priv = 1;
    s->bc_novirt = 1;
    while (rc == RWY_OK && rwy_readdir_nth(s, path, 0, name, sizeof(name), 0) == RWY_OK) {
        child_path(child, path, pl, name);
        rc = rwy_unlink(s, child);
    }
    s->bc_novirt = 0;
    s->bc_priv = priv;
    return rc;
}

/* RECOVERY/ and WDT/: unlink what the directory holds, with the rights of the caller, then the directory. */
static int remove_plain_dir(rwy_store_t *s, const char *path) {
    char buf[1024], child[RWY_MAX_PATH + 1];
    uint8_t types[32];
    size_t pl = rwy_strlen(path), count = sizeof(types), off = 0;
    int rc;
    if (pl + 1 + RWY_NAME_MAX > RWY_MAX_PATH) return RWY_ERR_NAMETOOLONG;
    s->bc_novirt = 1;
    rc = rwy_readdir(s, path, buf, sizeof(buf), &count, types);
    for (size_t i = 0; !rc && i < count; i++) {
        child_path(child, path, pl, buf + off);
        off += rwy_strlen(buf + off) + 1;
        rc = rwy_unlink(s, child);
    }
    s->bc_novirt = 0;
    return rc ? rc : rwy_rmdir(s, path);
}

int rwy_bootcfg_remove_slot(rwy_store_t *s, const char *path) {
    char name[RWY_NAME_MAX + 1];
    uint64_t dir;
    int rc;
    if (!s || !path) return RWY_ERR_INVAL;
    if ((rc = rwy_resolve(s, path, 1, &dir))) return rc;
    /* The rmdir of a slot journals it, then empties it. */
    if (rwy_bootcfg_role(s, dir, name) == RWY_ROLE_SLOT) return rwy_rmdir(s, path);
    return remove_plain_dir(s, path);
}

int rwy_bootcfg_check_rename(rwy_store_t *s, uint64_t sp, const char *sb, uint32_t sl,
                              uint64_t dp, const char *db, uint32_t dl, uint8_t type, int replaces) {
    uint64_t t = bc_active(s);
    int sw, dw, rc;
    if (!t || s->bc_priv) return RWY_OK;
    sw = dir_where(s, t, sp, 0);
    dw = dir_where(s, t, dp, 0);
    if (sw == W_OUT && dw == W_OUT) return RWY_OK;
    if (sp != dp) return RWY_ERR_PERM;                    /* nothing moves in, out or between directories */
    if (sw == W_ROOT && type == RWY_TYPE_DIR) return RWY_ERR_PERM;   /* slots keep their identity */
    rc = rwy_bootcfg_check_remove(s, sp, sb, sl);
    if (rc) return rc > 0 ? RWY_ERR_PERM : rc;
    if (replaces) {
        rc = rwy_bootcfg_check_remove(s, dp, db, dl);
        if (rc) return rc > 0 ? RWY_ERR_PERM : rc;
    }
    return rwy_bootcfg_check_create(s, dp, db, dl, type, 0, 0, 0, 0);
}

int rwy_bootcfg_mkdir_done(rwy_store_t *s, uint64_t oid) {
    int priv = s->bc_priv, rc;
    if (!s->in_txn || bc_active(s) != oid) return RWY_OK;
    /* The subtree was just created: stamp the layout version in the same transaction. */
    s->bc_priv = 1;
    rc = rwy_create(s, ROOT_PATH "/VERSION", RWY_S_IFREG | RWY_EFI_DEFAULT_MODE_FILE,
                     RWY_EFI_DEFAULT_UID, RWY_EFI_DEFAULT_GID);
    if (rc == RWY_OK)
        rc = rwy_write_path(s, ROOT_PATH "/VERSION", 0, LAYOUT_VERSION, sizeof(LAYOUT_VERSION) - 1);
    s->bc_priv = priv;
    return rc;
}

/* ---- finalize ------------------------------------ */

/* Drop the last component of the output; 0 if there is none. */
static int norm_pop(char *p, char **w) {
    if (*w == p) return 0;
    while (*w > p && *--*w != '/') {}
    return 1;
}

/* Collapse "." and ".." in a root-relative path; 0 if it climbs above the root. */
static int normalize(char *p) {
    char *w = p, *r = p, *e;
    while (*r) {
        while (*r == '/') r++;
        if (!*r) break;
        for (e = r; *e && *e != '/'; e++) {}
        if (e - r == 2 && r[0] == '.' && r[1] == '.') {
            if (!norm_pop(p, &w)) return 0;
        } else if (e - r != 1 || r[0] != '.') {
            if (w != p) *w++ = '/';
            while (r < e) *w++ = *r++;
        }
        r = e;
    }
    *w = 0;
    return 1;
}

/* The path that a symlink at `cur` leads to: an absolute target replaces it, a relative one follows its directory. */
static int link_target_path(char *cur, const char *tgt) {
    size_t tl = rwy_strlen(tgt), dl, n;

    if (tgt[0] == '/') {
        for (dl = 0; dl <= tl; dl++) cur[dl] = tgt[dl];
        return 1;
    }
    for (dl = rwy_strlen(cur); dl > 0 && cur[dl - 1] != '/'; dl--) {}
    if (dl + tl > RWY_MAX_PATH) return 0;
    for (n = 0; n <= tl; n++) cur[dl + n] = tgt[n];
    return 1;
}

/* A payload must end at a regular file (non-empty if `data`) after at most LINK_HOPS
 * symlinks, every hop staying inside EFI/RUNWAY/. */
int bc_payload_ok(rwy_store_t *s, const char *path, int data) {
    char cur[RWY_MAX_PATH + 1], tgt[RWY_MAX_PATH + 1];
    unsigned hops, n = (unsigned)rwy_strlen(path);
    if (n > RWY_MAX_PATH) return 0;
    for (hops = 0; hops <= n; hops++) cur[hops] = path[hops];   /* includes the NUL */
    for (hops = 0;; hops++) {
        const rwy_inode_t *in;
        uint64_t o;
        if (rwy_strncmp(cur, ROOT_PATH "/", sizeof(ROOT_PATH)) != 0 ||
            bc_note(s, rwy_resolve_ext(s, cur, 1, 0, &o)) != RWY_OK || !(in = rwy_inode_of(s, o, 1)))
            return 0;
        if (in->type == RWY_TYPE_FILE) return !data || in->size > 0;
        if (in->type != RWY_TYPE_SYMLINK || hops == LINK_HOPS || bc_note(s, rwy_readlink(s, cur, tgt, sizeof(tgt))) != RWY_OK)
            return 0;
        if (!link_target_path(cur, tgt) || !normalize(cur)) return 0;
    }
}

/* Parse a decimal u64 file (trailing newline tolerated); 0 if absent or malformed. */
/* Read a scalar into `b` (SCALAR_MAX + 1 bytes), without its newline. Returns 1 if it could be read. */
int bc_read_text(rwy_store_t *s, uint64_t dir, const char *name, uint8_t *b, size_t *n) {
    uint64_t o;
    *n = SCALAR_MAX + 1;
    if (bc_note(s, rwy_dir_lookup(s, dir, name, (uint32_t)rwy_strlen(name), &o, 0)) != RWY_OK ||
        bc_note(s, rwy_read(s, o, 0, b, n)) != RWY_OK)
        return 0;
    *n = bc_strip_nl(b, *n);
    return 1;
}

int bc_read_u64(rwy_store_t *s, uint64_t dir, const char *name, uint64_t *v) {
    uint8_t b[SCALAR_MAX + 1];
    size_t n, i;
    if (!bc_read_text(s, dir, name, b, &n)) return 0;
    if (!bc_is_dec_u64(b, n)) return 0;
    for (*v = 0, i = 0; i < n; i++) *v = *v * 10 + (uint64_t)(b[i] - '0');
    return 1;
}

/* Read the slot label; returns 1 if present and valid, 0 if absent or empty, -1 if invalid. */
int bc_read_label(rwy_store_t *s, uint64_t dir, char *out) {
    uint8_t b[RWY_LABEL_MAX + 1];
    size_t n = sizeof(b);
    uint64_t o;
    const rwy_inode_t *in;
    size_t i;

    if (out) out[0] = 0;
    if (bc_note(s, rwy_dir_lookup(s, dir, "LABEL", 5, &o, 0)) != RWY_OK) return 0;
    in = rwy_inode_of(s, o, 1);
    if (!in || in->type != RWY_TYPE_FILE || in->size > RWY_LABEL_MAX) return -1;
    if (in->size == 0) return 0;
    if (bc_note(s, rwy_read(s, o, 0, b, &n)) != RWY_OK) return -1;
    n = bc_strip_nl(b, n);
    if (n == 0) return 0;
    if (!bc_label_ok(b, n)) return -1;
    if (out) {
        for (i = 0; i < n; i++) out[i] = (char)b[i];
        out[n] = 0;
    }
    return 1;
}

/* Read the slot entry name; returns 1 if present and valid, 0 if absent, -1 if invalid or empty. */
int bc_read_entry(rwy_store_t *s, uint64_t dir, char *out) {
    uint8_t b[RWY_ENTRY_MAX + 2];
    size_t n = sizeof(b);
    uint64_t o;
    const rwy_inode_t *in;
    size_t i;

    if (out) out[0] = 0;
    if (bc_note(s, rwy_dir_lookup(s, dir, "ENTRY", 5, &o, 0)) != RWY_OK) return 0;
    in = rwy_inode_of(s, o, 1);
    if (!in || in->type != RWY_TYPE_FILE || in->size == 0 || in->size > RWY_ENTRY_MAX + 1) return -1;
    if (bc_note(s, rwy_read(s, o, 0, b, &n)) != RWY_OK) return -1;
    n = bc_strip_nl(b, n);
    if (!rwy_entry_name_ok((const char *)b, n)) return -1;
    if (out) {
        for (i = 0; i < n; i++) out[i] = (char)b[i];
        out[n] = 0;
    }
    return 1;
}

/* Read the slot META; returns 1 if present and valid, 0 if absent, -1 if invalid or empty. */
int bc_read_meta(rwy_store_t *s, uint64_t dir, char *out, size_t *out_len) {
    uint8_t b[RWY_META_MAX + 1];
    size_t n = sizeof(b) - 1;
    uint64_t o;
    const rwy_inode_t *in;
    size_t i;

    if (out) out[0] = 0;
    if (out_len) *out_len = 0;
    if (bc_note(s, rwy_dir_lookup(s, dir, "META", 4, &o, 0)) != RWY_OK) return 0;
    in = rwy_inode_of(s, o, 1);
    if (!in || in->type != RWY_TYPE_FILE || in->size == 0 || in->size > RWY_META_MAX) return -1;
    n = in->size;
    if (bc_note(s, rwy_read(s, o, 0, b, &n)) != RWY_OK) return -1;
    b[n] = 0;
    if (!rwy_meta_ok((const char *)b, n)) return -1;
    if (out) {
        for (i = 0; i < n; i++) out[i] = (char)b[i];
        out[n] = 0;
    }
    if (out_len) *out_len = n;
    return 1;
}

static int put_file(rwy_store_t *s, const char *path, const char *b, size_t n) {
    uint64_t o;
    int rc = rwy_resolve_ext(s, path, 1, 0, &o) == RWY_OK
         ? rwy_truncate(s, path, 0)
         : rwy_create(s, path, RWY_S_IFREG | RWY_EFI_DEFAULT_MODE_FILE, RWY_EFI_DEFAULT_UID, RWY_EFI_DEFAULT_GID);
    return rc ? rc : rwy_write_path(s, path, 0, b, n);
}

int bc_put_u64(rwy_store_t *s, const char *path, uint64_t v) {
    char d[24], b[24];
    size_t n = 0, i = 0;
    do { d[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) b[n++] = d[--i];
    b[n++] = '\n';
    return put_file(s, path, b, n);
}

/* Copy the record into `r` without the entries of revisions that no slot has, so a slot recycled many times
 * never finds the table full. Needs an open transaction. */
void bc_record_load(rwy_store_t *s, uint8_t *r) {
    char name[RWY_NAME_MAX + 1];
    uint64_t live[RWY_SS_ENTRIES], t = bc_active(s), o, v;
    uint32_t l;
    size_t i = 0, n = 0;
    (void)rwy_record_read(s, r);
    while (t && n < RWY_SS_ENTRIES && bc_next_dir(s, t, &i, name, &l, &o))
        if (bc_dir_kind(name, l) == K_DIR_SLOT && bc_read_u64(s, o, "REVISION", &v)) live[n++] = v;
    if (t) rwy_ss_prune(r, live, n);
}

/* The slot is final: its STATE and TRIES move from the tree into the record. The record is written first, so a
 * cut before the commit leaves an entry that no slot uses and the next slot to get this revision overwrites. */
static int move_state(rwy_store_t *s, uint64_t slot, char *path, size_t pl, uint64_t rev) {
    static const char *const files[] = { "STATE", "TRIES" };
    uint8_t r[RWY_REC_PAYLOAD];
    rwy_ss_entry_t e = { 1, RWY_SS_INSTALLED, RWY_SS_CAUSE_NONE, 0 };
    uint64_t tries, o;
    unsigned i, k;
    int rc;
    if (bc_read_u64(s, slot, "TRIES", &tries)) e.tries = tries;
    bc_record_load(s, r);
    rc = rwy_ss_put(r, rev, &e);
    if (rc == RWY_OK) rc = rwy_record_write(s, r);
    for (i = 0; rc == RWY_OK && i < 2; i++) {
        for (k = 0; k <= rwy_strlen(files[i]); k++) path[pl + k] = files[i][k];
        if (rwy_dir_lookup(s, slot, files[i], (uint32_t)rwy_strlen(files[i]), &o, 0) == RWY_OK) rc = rwy_unlink(s, path);
    }
    return rc;
}

/* The slot whose STATE is `state_oid`: its directory and path; also the highest REVISION of any slot. */
static uint64_t find_slot_of_state(rwy_store_t *s, uint64_t t, uint64_t state_oid, char *path, uint64_t *high) {
    char name[RWY_NAME_MAX + 1];
    uint32_t l;
    uint64_t o, v, slot = 0;
    size_t i = 0, pl, k;

    *high = 0;
    while (bc_next_dir(s, t, &i, name, &l, &o)) {
        if (bc_dir_kind(name, l) != K_DIR_SLOT) continue;
        if (!slot && rwy_dir_lookup(s, o, "STATE", 5, &v, 0) == RWY_OK && v == state_oid) {
            slot = o;
            pl = 0;
            for (k = 0; k < sizeof(ROOT_PATH) - 1; k++) path[pl++] = ROOT_PATH[k];
            path[pl++] = '/';
            for (k = 0; k < l; k++) path[pl++] = name[k];
            path[pl] = 0;
        }
        if (bc_read_u64(s, o, "REVISION", &v) && v > *high) *high = v;
    }
    return slot;
}

static int slot_scalar_ok(rwy_store_t *s, uint64_t slot, size_t k, const rwy_inode_t *in) {
    uint64_t v;
    if (SLOT_KINDS[k] == K_ARGS) return !in || in->size > ARGS_MAX ? RWY_ERR_INVAL : RWY_OK;
    if (SLOT_KINDS[k] == K_LABEL) return bc_read_label(s, slot, 0) < 0 ? RWY_ERR_INVAL : RWY_OK;
    if (SLOT_KINDS[k] == K_ENTRY) return bc_read_entry(s, slot, 0) < 0 ? RWY_ERR_INVAL : RWY_OK;
    if (SLOT_KINDS[k] == K_META) return bc_read_meta(s, slot, 0, 0) < 0 ? RWY_ERR_INVAL : RWY_OK;
    if (SLOT_KINDS[k] == K_TRIES || SLOT_KINDS[k] == K_WATCHDOG)
        return bc_read_u64(s, slot, SLOT_FILES[k], &v) ? RWY_OK : RWY_ERR_INVAL;
    return RWY_OK;
}

/* Does slot file number k hold what its kind needs? The file name is written after `pl` in `path`. */
static int slot_file_ok(rwy_store_t *s, uint64_t slot, char *path, size_t pl, size_t k) {
    uint64_t f;
    size_t n = rwy_strlen(SLOT_FILES[k]);
    int have = rwy_dir_lookup(s, slot, SLOT_FILES[k], (uint32_t)n, &f, 0) == RWY_OK;
    unsigned j;

    for (j = 0; j <= n; j++) path[pl + j] = SLOT_FILES[k][j];
    if (SLOT_KINDS[k] == K_PAYLOAD)
        return (have || k == 0) && !bc_payload_ok(s, path, k == 0) ? RWY_ERR_INVAL : RWY_OK;
    if (have) return slot_scalar_ok(s, slot, k, rwy_inode_of(s, f, 1));
    return RWY_OK;
}

static int slot_type_rules_ok(rwy_store_t *s, uint64_t slot) {
    char entry[RWY_ENTRY_MAX + 1];
    int has_entry = bc_read_entry(s, slot, entry);
    size_t slen;
    int is_efi = 0;
    uint64_t f;

    if (has_entry < 0) return RWY_ERR_INVAL;
    if (has_entry > 0) (void)rwy_entry_split(entry, rwy_strlen(entry), &slen, &is_efi);

    if (is_efi) {
        if (rwy_dir_lookup(s, slot, "INITRD", 6, &f, 0) == RWY_OK) return RWY_ERR_INVAL;
        if (rwy_dir_lookup(s, slot, "DTB", 3, &f, 0) == RWY_OK) return RWY_ERR_INVAL;
        if (rwy_dir_lookup(s, slot, "ARGS", 4, &f, 0) == RWY_OK) return RWY_ERR_INVAL;
        if (rwy_dir_lookup(s, slot, "META", 4, &f, 0) == RWY_OK) return RWY_ERR_INVAL;
    }
    return RWY_OK;
}

/* Validate the slot at `path` (the path ends with '/' at pl): KERNEL present and non-empty, links resolve,
 * values parse. */
static int validate_slot(rwy_store_t *s, uint64_t slot, char *path, size_t pl) {
    int rc = slot_type_rules_ok(s, slot);
    if (rc) return rc;
    for (size_t k = 0; k < N_SLOT_FILES; k++) {
        rc = slot_file_ok(s, slot, path, pl, k);
        if (rc) return rc;
    }
    return RWY_OK;
}

static void finalize_transplog_record(rwy_store_t *s, uint64_t slot, char *path, size_t pl, uint64_t rev) {
    char label[RWY_LABEL_MAX + 1] = { 0 };
    uint8_t payload_hash[32] = { 0 };
    const char *sname = path + sizeof(ROOT_PATH);
    uint8_t event = s->bc_slot_removed ? RWY_JOURNAL_EVENT_UPDATE : RWY_JOURNAL_EVENT_INSTALL;

    (void)bc_read_label(s, slot, label);
    path[pl - 1] = 0;
    (void)rwy_transplog_calc_payload_hash(s, path, payload_hash);
    path[pl - 1] = '/';
    (void)rwy_transplog_record(s, sname, event, rev, label, payload_hash, s->rec_seq, NULL);
}

/* Called inside the transaction that just wrote INSTALLED to STATE of an incomplete
 * slot: validate the slot, allocate its revision and advance the counter. Any error
 * makes the caller abort, so the slot stays as it was. */
int rwy_bootcfg_finalize(rwy_store_t *s, uint64_t state_oid) {
    char path[RWY_MAX_PATH + 1];
    uint64_t t = bc_active(s), high, counter = 0, slot, rev;
    size_t pl, k;
    int priv, rc;
    if (!t) return RWY_OK;
    slot = find_slot_of_state(s, t, state_oid, path, &high);
    if (!slot) return RWY_OK;
    pl = rwy_strlen(path);
    path[pl++] = '/';
    rc = validate_slot(s, slot, path, pl);
    if (rc) return rc;

    /* The next revision is above both the counter and every slot. */
    (void)bc_read_u64(s, t, "REVISION", &counter);
    rev = counter > high ? counter : high;
    if (rev == UINT64_MAX) return RWY_ERR_RANGE;
    rev++;
    priv = s->bc_priv;
    s->bc_priv = 1;
    rc = bc_put_u64(s, ROOT_PATH "/REVISION", rev);
    if (rc == RWY_OK) {
        for (k = 0; k < sizeof("REVISION"); k++) path[pl + k] = "REVISION"[k];
        rc = bc_put_u64(s, path, rev);
    }
    if (rc == RWY_OK) rc = move_state(s, slot, path, pl, rev);
    if (rc == RWY_OK) {
        finalize_transplog_record(s, slot, path, pl, rev);
        s->bc_rev_pending = rev;
    }
    s->bc_priv = priv;
    return rc;
}

/* The commit succeeded: record the revision a finalize issued as the highest, and raise the epoch when the
 * commit finalized or removed a slot. A failed write is not an error of the commit; the next selection
 * (rev_hw) or mount check (epoch) repairs it. */
void rwy_bootcfg_committed(rwy_store_t *s) {
    uint8_t r[RWY_REC_PAYLOAD];
    uint64_t rev = s->bc_rev_pending;
    int raise, dirty;
    raise = rev || s->bc_slot_removed;
    s->bc_rev_pending = 0;
    s->bc_slot_removed = 0;
    if (!raise || rwy_record_read(s, r)) return;
    dirty = rev && rwy_ss_rev_hw(r) < rev;
    if (dirty) rwy_ss_set_rev_hw(r, rev);
    raise = rwy_anchor_raise_rec(s, r);
    if (!dirty && !raise) return;
    if (rwy_record_write(s, r) == RWY_OK && raise) rwy_anchor_raised(s);
}

/* Is BLOBS/`name` a content blob that nothing but its BLOBS name links? */
int bc_blob_orphan(rwy_store_t *s, uint64_t blobs, const char *name) {
    const rwy_inode_t *in;
    uint64_t o;
    uint8_t type;
    size_t l = rwy_strlen(name);
    if (!rwy_bootcfg_content_name(name, l)) return 0;
    if (rwy_dir_lookup(s, blobs, name, (uint32_t)l, &o, &type) != RWY_OK || type != RWY_TYPE_FILE) return 0;
    in = rwy_inode_of(s, o, 1);
    return in && in->nlink == 1;
}

/* Called before the commit publishes: unlink the content blobs that no slot, RECOVERY/ or WDT/ links
 * any more. Running here, after every link of the transaction exists, keeps a blob that an update
 * moves from one slot to the next. */
int rwy_bootcfg_sweep(rwy_store_t *s) {
    char name[RWY_NAME_MAX + 1], path[RWY_MAX_PATH + 1];
    uint64_t t = bc_active(s), blobs;
    size_t j = 0, pl = rwy_strlen(ROOT_PATH "/BLOBS");
    int priv = s->bc_priv, rc = RWY_OK;
    if (!s->bc_sweep) return RWY_OK;
    s->bc_sweep = 0;
    if (!t || rwy_dir_lookup(s, t, "BLOBS", 5, &blobs, 0) != RWY_OK) return RWY_OK;
    s->bc_priv = 1;
    s->bc_novirt = 1;
    while (rc == RWY_OK && rwy_readdir_nth(s, ROOT_PATH "/BLOBS", j, name, sizeof(name), 0) == RWY_OK) {
        if (!bc_blob_orphan(s, blobs, name)) {
            j++;
            continue;
        }
        child_path(path, ROOT_PATH "/BLOBS", pl, name);
        rc = rwy_unlink(s, path);
        j = 0;
    }
    s->bc_novirt = 0;
    s->bc_priv = priv;
    return rc;
}

/* ---- object hooks ------------------------------------------------------ */

int rwy_bootcfg_check_attr(rwy_store_t *s, uint64_t oid) {
    uint64_t t = bc_active(s);
    const rwy_inode_t *in;
    cls_t c;
    if (!t || s->bc_priv) return RWY_OK;
    in = rwy_inode_of(s, oid, 1);
    if (!in) return RWY_ERR_NOENT;
    if (in->type == RWY_TYPE_DIR) return dir_where(s, t, oid, 0) == W_OUT ? RWY_OK : RWY_ERR_PERM;
    classify(s, t, oid, 1, &c);
    return c.in_tree ? RWY_ERR_PERM : RWY_OK;
}

/* Is `b` a value that the scalar accepts? */
static int scalar_value_ok(const cls_t *c, const uint8_t *b, size_t n) {
    int id;
    const char *why;

    if ((id = bc_setting_check(c->kind, b, n, &why)) >= 0) return id ? RWY_OK : RWY_ERR_INVAL;
    switch (c->kind) {
    case K_STATE:
        id = bc_state_id(b, n);
        if (id < 0) return RWY_ERR_INVAL;
        if (c->where != W_RECOVERY && id != ST_INSTALLED) return RWY_ERR_PERM;   /* RECOVERY has no lifecycle: any valid state */
        return RWY_OK;
    case K_ENTRY: return rwy_entry_name_ok((const char *)b, n) ? RWY_OK : RWY_ERR_INVAL;
    default: return bc_is_dec_u64(b, n) ? RWY_OK : RWY_ERR_INVAL;
    }
}

/* The value of a whole-file scalar write; sets *replace as the write needs. */
static int scalar_write_check(const cls_t *c, const uint8_t *b, uint64_t pos, size_t len, int *replace) {
    int rc;
    size_t max = c->kind == K_ENTRY ? RWY_ENTRY_MAX + 1 : SCALAR_MAX;

    /* Scalars have whole-file replace semantics. */
    if (len == 0) return RWY_OK;
    if (pos != 0 || len > max) return RWY_ERR_INVAL;
    rc = scalar_value_ok(c, b, bc_strip_nl(b, len));
    if (rc) return rc;
    *replace = RWY_BC_REPLACE;
    if (c->kind == K_STATE && !c->fin && c->where != W_RECOVERY) *replace |= RWY_BC_FINALIZE;
    return RWY_OK;
}

static uint64_t bounded_file_max(int kind) {
    if (kind == K_ARGS) return ARGS_MAX;
    if (kind == K_LABEL) return RWY_LABEL_MAX;
    if (kind == K_META) return META_MAX;
    if (kind == K_WDT_APP) return WDT_APP_MAX;
    return 0;
}

static int bounded_file_kind(int kind) {
    return kind == K_ARGS || kind == K_LABEL || kind == K_META || kind == K_WDT_APP;
}

static int bounded_write_check(int kind, uint64_t pos, size_t len) {
    uint64_t max = bounded_file_max(kind);
    return (pos > max || len > max - pos) ? RWY_ERR_INVAL : RWY_OK;
}

/* STATE as selection reads it: anything but a valid non-FAILED value is FAILED. */
int rwy_bootcfg_check_write(rwy_store_t *s, uint64_t oid, uint64_t pos, const void *buf, size_t len, int *replace) {
    uint64_t t = bc_active(s);
    cls_t c;
    int rc;
    *replace = 0;
    if (!t || s->bc_priv) return RWY_OK;
    classify(s, t, oid, 0, &c);
    if (!c.in_tree) return RWY_OK;
    rc = object_perm(s, &c);
    if (rc) return rc;
    if (bounded_file_kind(c.kind)) return bounded_write_check(c.kind, pos, len);
    if (!bc_is_scalar(c.kind)) return RWY_OK;
    return scalar_write_check(&c, buf, pos, len, replace);
}

int rwy_bootcfg_check_truncate(rwy_store_t *s, uint64_t oid, uint64_t size, int *noop) {
    uint64_t t = bc_active(s);
    cls_t c;
    int rc;
    *noop = 0;
    if (!t || s->bc_priv) return RWY_OK;
    classify(s, t, oid, 0, &c);
    if (!c.in_tree) return RWY_OK;
    rc = object_perm(s, &c);
    if (rc) return rc;
    if (bc_is_scalar(c.kind)) {
        /* `echo OK > STATE` truncates first under FUSE; keep the old value until the write replaces it. */
        if (size) return RWY_ERR_INVAL;
        *noop = 1;
    } else if (bounded_file_kind(c.kind)) {
        if (size > bounded_file_max(c.kind)) return RWY_ERR_INVAL;
    }
    return RWY_OK;
}


