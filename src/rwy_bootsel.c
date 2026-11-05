/*
 * rwy_bootsel.c — boot selection under EFI/RUNWAY: choice of the slot to
 * boot, preview, fallback after a failed boot, and the slot to recycle.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_bootcfg.h"

void bc_cat(sel_t *x, const char *t) {
    for (; *t && x->n < sizeof(x->b) - 1; t++) x->b[x->n++] = *t;
}
void bc_cat_label_str(sel_t *x, const char *label) {
    size_t rem, len, i, keep;
    if (!label || !*label) return;
    rem = (x->n < sizeof(x->b) - 1) ? (sizeof(x->b) - 1 - x->n) : 0;
    if (rem < 7) return;
    len = rwy_strlen(label);
    if (!len) return;
    if (rem >= len + 3) {
        bc_cat(x, " \"");
        bc_cat(x, label);
        bc_cat(x, "\"");
    } else {
        keep = rem - 6;
        bc_cat(x, " \"");
        for (i = 0; i < keep && label[i]; i++) x->b[x->n++] = label[i];
        bc_cat(x, "...\"");
    }
}

void bc_cat_label_dir(sel_t *x, uint64_t dir) {
    char label[RWY_LABEL_MAX + 1];
    if (dir && bc_read_label(x->s, dir, label) > 0)
        bc_cat_label_str(x, label);
}

void bc_emit(sel_t *x) {
    x->b[x->n] = 0;
    if (x->fn) x->fn(x->ctx, x->b);
    x->n = 0;
}

static void hold(sel_t *x) {
    char *h = x->held[x->nheld++];
    for (size_t i = 0; i < x->n; i++) h[i] = x->b[i];
    h[x->n] = 0;
    x->n = 0;
}

const char *bc_dec(char *d, uint64_t v) {   /* d holds >= 21 bytes */
    char *p = d + 20;
    *p = 0;
    do { *--p = (char)('0' + v % 10); v /= 10; } while (v);
    return p;
}

static uint64_t slot_index(const char *name) {
    uint64_t v = 0;
    for (name += 6; *name; name++) v = v * 10 + (uint64_t)(*name - '0');
    return v;
}

/* NULL: payload and values of `name` are usable. Otherwise why not. */
const char *bc_payload_fault(rwy_store_t *s, uint64_t dir, const char *name) {
    static const char *const names[] = { "KERNEL", "INITRD", "DTB" };
    static const char *const faults[] = { "KERNEL invalid", "INITRD invalid", "DTB invalid" };
    char path[RWY_MAX_PATH + 1];
    uint64_t f;
    unsigned i;
    for (i = 0; i < 3; i++) {
        if (bc_note(s, rwy_dir_lookup(s, dir, names[i], (uint32_t)rwy_strlen(names[i]), &f, 0)) != RWY_OK) {
            if (i) continue;
            return "KERNEL missing";
        }
        bc_sel_path(path, name, names[i]);
        if (!bc_payload_ok(s, path, i == 0)) return faults[i];
    }
    if (bc_note(s, rwy_dir_lookup(s, dir, "ARGS", 4, &f, 0)) == RWY_OK) {
        const rwy_inode_t *in = rwy_inode_of(s, f, 1);
        if (!in || in->size > ARGS_MAX) return "ARGS invalid";
    }
    if (bc_read_label(s, dir, 0) < 0) return "LABEL invalid";
    return 0;
}

/* NULL: a valid, ordered slot. "": incomplete (no REVISION), ignored silently.
 * Anything else: why the slot is invalid. */
const char *bc_slot_fault(rwy_store_t *s, uint64_t dir, const char *name, uint64_t *rev) {
    if (bc_note(s, rwy_dir_lookup(s, dir, "REVISION", 8, 0, 0)) != RWY_OK) return "";
    if (!bc_read_u64(s, dir, "REVISION", rev)) return "REVISION invalid";
    return bc_payload_fault(s, dir, name);
}

void bc_sel_path(char *path, const char *slot, const char *file) {
    size_t n = 0, k;
    for (k = 0; ROOT_PATH[k]; k++) path[n++] = ROOT_PATH[k];
    path[n++] = '/';
    if (slot) {
        for (k = 0; slot[k]; k++) path[n++] = slot[k];
        path[n++] = '/';
    }
    for (k = 0; file[k]; k++) path[n++] = file[k];
    path[n] = 0;
}

/* State and tries left of the finalized slot `rev`: a slot with no entry is INSTALLED with 1 try. */
rwy_ss_entry_t bc_ss_entry(const uint8_t *rec, uint64_t rev) {
    rwy_ss_entry_t e = { 1, RWY_SS_INSTALLED, RWY_SS_CAUSE_NONE, 0 };
    (void)rwy_ss_get(rec, rev, &e);
    return e;
}

/* Record a new state and tries left for the slot `rev`. */
static int sel_set(sel_t *x, uint64_t rev, int state, uint64_t tries, int cause) {
    rwy_ss_entry_t e = bc_ss_entry(x->rec, rev);
    e.tries = tries;
    e.state = (uint8_t)state;
    e.cause = (uint8_t)cause;
    x->dirty = 1;
    if (state == ST_FAILED) x->failed = 1;
    return rwy_ss_put(x->rec, rev, &e);
}

static int slot_has_extra(rwy_store_t *s, uint64_t dir) {
    static const char *const extras[] = { "INITRD", "DTB", "ARGS", "LABEL", "META" };
    uint64_t o;
    for (unsigned i = 0; i < 5; i++) {
        if (bc_note(s, rwy_dir_lookup(s, dir, extras[i], (uint32_t)rwy_strlen(extras[i]), &o, 0)) == RWY_OK)
            return 1;
    }
    return 0;
}

/* The entry name of slot `dir`: its ENTRY file, else the default for the files it has. */
static void slot_entry(rwy_store_t *s, const char *name, uint64_t dir, char *out) {
    out[0] = 0;
    if (dir && bc_read_entry(s, dir, out) <= 0) {
        uint64_t num = (name[0] == 'C') ? slot_index(name) : 0;
        rwy_entry_default(out, RWY_ENTRY_MAX + 1, num, !slot_has_extra(s, dir));
    }
}

/* Describe the entry to boot: its name, revision and the WATCHDOG and BOOT-DELAY to carry. Writes nothing. */
static void sel_fill(sel_t *x, rwy_boot_choice_t *out, const char *name, uint64_t dir, uint64_t rev, uint64_t left, int trial) {
    rwy_store_t *s = x->s;
    uint64_t w;
    unsigned i;
    for (i = 0; name[i]; i++) out->slot[i] = name[i];
    out->slot[i] = 0;
    out->booted = 1;
    out->trial = trial;
    out->revision = rev;
    out->tries_left = left;
    if (!bc_read_u64(s, dir, "WATCHDOG", &w) && !bc_read_u64(s, x->t, "WATCHDOG", &w)) w = 0;
    out->watchdog = w;
    out->boot_delay = (bc_read_u64(s, x->t, "BOOT-DELAY", &w) && w <= DELAY_MAX) ? w : 0;
    out->has_label = bc_read_label(s, dir, out->label) > 0;
    slot_entry(s, name, dir, out->entry);
}

/* Make `name` the booted entry: BOOTED, REASON and the WATCHDOG to carry. */
static int sel_choose(sel_t *x, rwy_boot_choice_t *out, const char *name, uint64_t dir, uint64_t rev,
                      uint64_t left, int trial, int reason) {
    char cur[RWY_NAME_MAX + 1];
    int rc;
    sel_fill(x, out, name, dir, rev, left, trial);
    (void)rwy_ss_booted(x->rec, cur);
    if (rwy_strcmp(cur, name) != 0) {
        rc = rwy_ss_set_booted(x->rec, name);
        if (rc) return rc;
        x->dirty = 1;
    }
    if (rwy_ss_reason(x->rec) != reason) {
        rwy_ss_set_reason(x->rec, reason);
        x->dirty = 1;
    }
    return RWY_OK;
}

static int sel_boot(sel_t *x, rwy_boot_choice_t *out, const char *name, uint64_t dir, uint64_t rev, int st, uint64_t left) {
    char d[24];
    bc_cat(x, "RUNWAY: booting "); bc_cat(x, name); bc_cat(x, " (rev "); bc_cat(x, bc_dec(d, rev));
    if (st == ST_OK) {
        bc_cat(x, ", OK)");
    } else {
        bc_cat(x, ", TESTING, "); bc_cat(x, bc_dec(d, left)); bc_cat(x, left == 1 ? " try left)" : " tries left)");
    }
    bc_cat_label_dir(x, dir);
    hold(x);
    return sel_choose(x, out, name, dir, rev, left, st != ST_OK, x->loop ? RWY_SS_REASON_BOOT_LOOP : RWY_SS_REASON_NORMAL);
}

/* RECOVERY is sealed and has no lifecycle: no state, tries or revision are touched. */
static int sel_recovery(sel_t *x, rwy_boot_choice_t *out, int reason, const char *msg) {
    bc_cat(x, msg);
    if (x->rdir) bc_cat_label_dir(x, x->rdir);
    hold(x);
    return sel_choose(x, out, "RECOVERY", x->rdir, 0, 0, 0, reason);
}

/* No slot is left: RECOVERY, for a boot loop if a slot was demoted for it, else because all failed. */
static int sel_recovery_last(sel_t *x, rwy_boot_choice_t *out) {
    if (x->loop) return sel_recovery(x, out, RWY_SS_REASON_BOOT_LOOP, "RUNWAY: no OK slot left after a boot loop, booting RECOVERY");
    return sel_recovery(x, out, RWY_SS_REASON_ALL_FAILED, "RUNWAY: no bootable config (all_failed), booting RECOVERY");
}

typedef struct {
    int found;
    uint64_t rev, idx, dir;
    char name[RWY_NAME_MAX + 1];
} pick_t;

/* Slots at or above this (revision, index) were already tried. */
typedef struct {
    int bounded;
    uint64_t rev, idx;
} bound_t;

#define SEL_GO_ON 1

/*
 * The best valid slot below the bound: highest REVISION, then highest index.
 * With a record, only slots that it marks OK qualify.
 */
static void pick_best(rwy_store_t *s, uint64_t t, const bound_t *b, const uint8_t *rec, pick_t *p) {
    char name[RWY_NAME_MAX + 1];
    uint32_t l;
    uint64_t o, rev;
    size_t i = 0;

    p->found = 0;
    p->rev = p->idx = p->dir = 0;
    while (bc_next_dir(s, t, &i, name, &l, &o)) {
        uint64_t idx;
        if (bc_dir_kind(name, l) != K_DIR_SLOT || bc_slot_fault(s, o, name, &rev)) continue;
        idx = slot_index(name);
        if (b->bounded && (rev > b->rev || (rev == b->rev && idx >= b->idx))) continue;
        if (rec && bc_ss_entry(rec, rev).state != ST_OK) continue;
        if (!p->found || rev > p->rev || (rev == p->rev && idx > p->idx)) {
            p->found = 1; p->rev = rev; p->idx = idx; p->dir = o;
            for (l = 0; name[l]; l++) p->name[l] = name[l];
            p->name[l] = 0;
        }
    }
}

/* Say what is wrong with the layout and the entries; remember the usable RECOVERY. */
static void sel_scan(sel_t *x) {
    rwy_store_t *s = x->s;
    char name[RWY_NAME_MAX + 1], d[24];
    uint32_t l;
    uint64_t o, v, rev;
    size_t i = 0;
    unsigned nslots = 0;

    if (bc_read_u64(s, x->t, "VERSION", &v) && v > 1)
    {
        bc_cat(x, "RUNWAY: layout v"); bc_cat(x, bc_dec(d, v)); bc_cat(x, " newer than supported, using known entries only");
        bc_emit(x);
    }
    while (bc_next_dir(s, x->t, &i, name, &l, &o)) {
        const char *why;
        if (bc_dir_kind(name, l) == K_DIR_RECOVERY) {
            why = bc_payload_fault(s, o, name);
            if (why) { bc_cat(x, "RUNWAY: RECOVERY invalid ("); bc_cat(x, why); bc_cat(x, "), ignored"); bc_emit(x); }
            else x->rdir = o;
            continue;
        }
        if (bc_dir_kind(name, l) != K_DIR_SLOT) continue;
        nslots++;
        why = bc_slot_fault(s, o, name, &rev);
        if (why && *why) {
            bc_cat(x, "RUNWAY: "); bc_cat(x, name); bc_cat(x, " invalid ("); bc_cat(x, why); bc_cat(x, "), ignored");
            bc_emit(x);
        }
    }
    if (nslots > SLOTS_MAX) {   /* an image from before the limit, or a bypass: reading stays tolerant */
        bc_cat(x, "RUNWAY: "); bc_cat(x, bc_dec(d, nslots)); bc_cat(x, " slots, only the "); bc_cat(x, bc_dec(d, SLOTS_MAX));
        bc_cat(x, " highest REVISION considered");
        bc_emit(x);
    }
}

/* A requested recovery goes first; consuming the request is part of this commit. SEL_GO_ON when there is none. */
static int sel_request(sel_t *x, rwy_boot_choice_t *out) {
    if (!rwy_ss_recover_next(x->rec))
        return SEL_GO_ON;
    if (!x->rdir) {
        bc_cat(x, "RUNWAY: RECOVER_NEXT ignored, no RECOVERY");
        bc_emit(x);
    } else if (x->req) {
        rwy_ss_set_recover_next(x->rec, 0);
        x->dirty = x->requested = 1;
        return sel_recovery(x, out, RWY_SS_REASON_REQUESTED, "RUNWAY: recovery requested, booting RECOVERY");
    }
    return SEL_GO_ON;
}

/* True when the guard says the OK slot `p` reached the loop limit; *g holds the settings. */
static int sel_loop_hit(sel_t *x, const pick_t *p, rwy_loop_guard_t *g) {
    return x->guard && rwy_bootcfg_loop_guard(x->s, g) == RWY_OK && x->guard(x->gctx, p->rev, g);
}

/* True when a lower OK slot or RECOVERY can take over from `p`. */
static int sel_has_other(sel_t *x, const pick_t *p) {
    bound_t b = { 1, p->rev, p->idx };
    pick_t q;
    if (x->rdir) return 1;
    pick_best(x->s, x->t, &b, x->rec, &q);
    return q.found;
}

static void sel_loop_log(sel_t *x, const pick_t *p, const char *what, const rwy_loop_guard_t *g) {
    char d[24];
    bc_cat(x, "RUNWAY: "); bc_cat(x, p->name); bc_cat(x, what);
    bc_cat(x, "more than "); bc_cat(x, bc_dec(d, g->limit));
    bc_cat(x, " entries in "); bc_cat(x, bc_dec(d, g->window)); bc_cat(x, " s");
}

/* Boot the OK slot, unless it is in a boot loop and something else can run: then fail it and go on. */
static int sel_try_ok(sel_t *x, rwy_boot_choice_t *out, const pick_t *p) {
    rwy_loop_guard_t g;
    unsigned i;
    int rc;

    if (!sel_loop_hit(x, p, &g)) return sel_boot(x, out, p->name, p->dir, p->rev, ST_OK, 0);
    if (!sel_has_other(x, p)) {
        sel_loop_log(x, p, " reached the boot loop limit (", &g);
        bc_cat(x, "), no other slot, booting it");
        bc_emit(x);
        (void)rwy_flight_record(x->s, x->rec, p->name, RWY_FLIGHT_EVENT_LOOP_LIMIT, RWY_SS_REASON_NORMAL, x->s->rec_seq + 1);
        return sel_boot(x, out, p->name, p->dir, p->rev, ST_OK, 0);
    }
    rc = sel_set(x, p->rev, ST_FAILED, 0, RWY_SS_CAUSE_LOOP);
    if (rc) return rc;
    x->loop = 1;
    for (i = 0; p->name[i]; i++) out->loop_slot[i] = p->name[i];
    out->loop_slot[i] = 0;
    sel_loop_log(x, p, " FAILED, boot loop (", &g);
    bc_cat(x, "), skipped");
    bc_cat_label_dir(x, p->dir);
    bc_emit(x);
    return SEL_GO_ON;
}

/* Try the picked slot: boot it, or fail it and let the caller go on. */
static int sel_try(sel_t *x, rwy_boot_choice_t *out, const pick_t *p) {
    rwy_ss_entry_t e = bc_ss_entry(x->rec, p->rev);
    int rc;

    if (e.state == ST_OK) return sel_try_ok(x, out, p);
    if (e.state != ST_FAILED) {
        if (e.tries == 0) {
            rc = sel_set(x, p->rev, ST_FAILED, 0, RWY_SS_CAUSE_TRIES);
            if (rc) return rc;
        } else {
            rc = sel_set(x, p->rev, ST_TESTING, e.tries - 1, RWY_SS_CAUSE_NONE);
            return rc ? rc : sel_boot(x, out, p->name, p->dir, p->rev, ST_TESTING, e.tries - 1);
        }
    }
    e = bc_ss_entry(x->rec, p->rev);
    bc_cat(x, "RUNWAY: "); bc_cat(x, p->name); bc_cat(x, " FAILED, skipped");
    if (e.cause != RWY_SS_CAUSE_NONE) { bc_cat(x, " ("); bc_cat(x, BC_CAUSE_NAMES[e.cause - 1]); bc_cat(x, ")"); }
    bc_cat_label_dir(x, p->dir);
    bc_emit(x);
    return SEL_GO_ON;
}

static int sel_run(sel_t *x, rwy_boot_choice_t *out) {
    bound_t b = { 0 };
    pick_t p;
    unsigned seen = 0;
    int rc;

    sel_scan(x);
    rc = sel_request(x, out);
    if (rc != SEL_GO_ON) return rc;
    for (;;) {
        pick_best(x->s, x->t, &b, 0, &p);
        if (!p.found || seen == SLOTS_MAX)
            return x->rdir ? sel_recovery_last(x, out) : RWY_OK;
        b.bounded = 1; b.rev = p.rev; b.idx = p.idx;
        seen++;
        rc = sel_try(x, out, &p);
        if (rc != SEL_GO_ON) return rc;
    }
}

/* True when EFI/RUNWAY `t` holds a slot or RECOVERY. */
static int is_engaged(rwy_store_t *s, uint64_t t) {
    char name[RWY_NAME_MAX + 1];
    uint32_t l;
    uint64_t o;
    size_t i = 0;
    if (t)
        while (bc_next_dir(s, t, &i, name, &l, &o))
            if (bc_dir_kind(name, l) == K_DIR_SLOT || bc_dir_kind(name, l) == K_DIR_RECOVERY) return 1;
    return 0;
}

/* The highest REVISION in the tree: the counter or any slot. */
static uint64_t tree_high(rwy_store_t *s, uint64_t t) {
    char name[RWY_NAME_MAX + 1];
    uint32_t l;
    uint64_t o, v, high = 0;
    size_t i = 0;
    if (bc_read_u64(s, t, "REVISION", &v)) high = v;
    while (bc_next_dir(s, t, &i, name, &l, &o))
        if (bc_dir_kind(name, l) == K_DIR_SLOT && bc_read_u64(s, o, "REVISION", &v) && v > high) high = v;
    return high;
}

/* A counter below rev_hw means the tree is older than the record: it was put back. The anchor
 * refuses a record that does not match its counter. */
static int rolled_back(rwy_store_t *s, uint64_t t) {
    uint64_t counter = 0;
    (void)bc_read_u64(s, t, "REVISION", &counter);
    return counter < rwy_ss_rev_hw(s->rec) || rwy_anchor_refused(s);
}

/* Open the transaction and look at the tree. *done is set when the result is already known. */
static int select_begin(rwy_store_t *s, rwy_boot_choice_t *out, sel_t *x, int *done) {
    int rc;
    unsigned k;
    *done = 1;
    for (k = 0; k < sizeof(*out); k++) ((uint8_t *)out)[k] = 0;
    if (!s || !out) return RWY_ERR_INVAL;
    if (!bc_is_system(s)) return RWY_ERR_PERM;
    rc = rwy_begin(s);
    if (rc) return rc;
    s->bc_err = 0;
    x->t = bc_active(s);
    out->engaged = is_engaged(s, x->t);
    if (out->engaged && rolled_back(s, x->t)) s->bc_err = RWY_ERR_ROLLBACK;
    if (s->bc_err || !out->engaged) {
        rc = s->bc_err;
        (void)rwy_abort(s);
        out->engaged = 0;
        return rc;
    }
    *done = 0;
    return RWY_OK;
}

static void select_record_flight(sel_t *x) {
    char booted[RWY_NAME_MAX + 1];
    (void)rwy_ss_booted(x->rec, booted);
    (void)rwy_flight_record(x->s, x->rec, booted, RWY_FLIGHT_EVENT_BOOT,
                             (uint8_t)rwy_ss_reason(x->rec), x->s->rec_seq + 1);
}

/* Write the record changes (not in a dry run), end the transaction and pass on the held log lines. */
static int select_commit(sel_t *x, int rc, int dry) {
    rwy_store_t *s = x->s;
    int raise = x->failed && !dry && rwy_anchor_raise_rec(s, x->rec);
    unsigned k;

    if (rc == RWY_OK && x->dirty && !dry) {
        select_record_flight(x);
        rc = rwy_record_write(s, x->rec);
    }
    if (rc == RWY_OK && raise) rwy_anchor_raised(s);
    (void)rwy_abort(s);
    if (rc == RWY_OK && x->fn && !dry)
        for (k = 0; k < (unsigned)x->nheld; k++) x->fn(x->ctx, x->held[k]);
    return rc;
}

/*
 * One selection: load the record, select, and commit the record changes.
 * SEL_GO_ON when the request for RECOVERY could not be committed and the
 * selection has to run again without it.
 */
static int select_pass(sel_t *x, rwy_boot_choice_t *out, int dry) {
    rwy_store_t *s = x->s;
    int rc;

    x->dirty = x->requested = x->failed = x->loop = 0; x->rdir = 0; x->nheld = 0;
    out->loop_slot[0] = 0;
    bc_record_load(s, x->rec);
    if (tree_high(s, x->t) > rwy_ss_rev_hw(x->rec)) {
        rwy_ss_set_rev_hw(x->rec, tree_high(s, x->t));
        x->dirty = 1;
    }
    rc = sel_run(x, out);
    if (rc == RWY_OK && s->bc_err) {   /* a read error made a slot look invalid */
        rc = s->bc_err;
        out->engaged = out->booted = 0;
        (void)rwy_abort(s);
        return rc;
    }
    rc = select_commit(x, rc, dry);
    if (rc == RWY_OK || !x->requested) return rc;
    return SEL_GO_ON;
}

static int select_run(rwy_store_t *s, rwy_boot_choice_t *out, rwy_log_fn log, void *ctx, int dry,
                      rwy_guard_fn guard, void *gctx) {
    sel_t x;
    int rc, done;

    x.s = s; x.fn = log; x.ctx = ctx; x.n = 0; x.guard = dry ? 0 : guard; x.gctx = gctx;
    rc = select_begin(s, out, &x, &done);
    if (done) return rc;
    if (rwy_record_damaged(s)) {
        bc_cat(&x, "RUNWAY: state record damaged, every slot starts again with 1 try");
        bc_emit(&x);
    }
    for (x.req = 1;; x.req = 0) {
        rc = select_pass(&x, out, dry);
        if (rc != SEL_GO_ON) return rc;
        /* The request could not be committed: ignore it and take the normal path. */
        out->booted = 0;
        bc_cat(&x, "RUNWAY: RECOVER_NEXT could not be consumed, ignored");
        bc_emit(&x);
        rc = rwy_begin(s);
        if (rc) return rc;
        x.t = bc_active(s);
    }
}

int rwy_bootcfg_select(rwy_store_t *s, rwy_boot_choice_t *out, rwy_log_fn log, void *ctx) {
    return select_run(s, out, log, ctx, 0, 0, 0);
}

int rwy_bootcfg_select_guarded(rwy_store_t *s, rwy_boot_choice_t *out, rwy_log_fn log, void *ctx,
                               rwy_guard_fn guard, void *gctx) {
    return select_run(s, out, log, ctx, 0, guard, gctx);
}

int rwy_bootcfg_preview(rwy_store_t *s, rwy_boot_choice_t *out) {
    return select_run(s, out, 0, 0, 1, 0, 0);
}

int rwy_bootcfg_entries(rwy_store_t *s, rwy_entry_fn fn, void *ctx) {
    char name[RWY_NAME_MAX + 1], entry[RWY_ENTRY_MAX + 1];
    uint32_t l;
    uint64_t t, o, rev;
    size_t i = 0;
    if (!s || !fn || !rwy_in_txn(s)) return RWY_ERR_INVAL;
    t = bc_active(s);
    if (!t) return RWY_ERR_NOENT;
    while (bc_next_dir(s, t, &i, name, &l, &o)) {
        if (bc_dir_kind(name, l) != K_DIR_SLOT || bc_slot_fault(s, o, name, &rev)) continue;
        slot_entry(s, name, o, entry);
        fn(ctx, entry);
    }
    return RWY_OK;
}

/*
 * Find RECOVERY and the slot named by `after`. A rolled back store may
 * still offer RECOVERY: it has no lifecycle that the rollback could undo.
 */
static void fallback_scan(sel_t *x, const char *after, bound_t *b) {
    rwy_store_t *s = x->s;
    char name[RWY_NAME_MAX + 1];
    uint32_t l;
    uint64_t o, rev;
    size_t i = 0;

    while (bc_next_dir(s, x->t, &i, name, &l, &o)) {
        if (bc_dir_kind(name, l) == K_DIR_RECOVERY && !bc_payload_fault(s, o, name)) x->rdir = o;
        if (bc_dir_kind(name, l) != K_DIR_SLOT || bc_slot_fault(s, o, name, &rev)) continue;
        if (after && !rwy_strcmp(name, after)) {
            b->bounded = 1; b->rev = rev; b->idx = slot_index(name);
        }
    }
}

/* Open the transaction and look at the tree. Returns a fatal error; a rolled back tree is left in *rc. */
static int fallback_begin(rwy_store_t *s, rwy_boot_choice_t *out, sel_t *x, int *rc) {
    unsigned k;
    int err;
    for (k = 0; k < sizeof(*out); k++) ((uint8_t *)out)[k] = 0;
    if (!s || !out) return RWY_ERR_INVAL;
    if (!bc_is_system(s)) return RWY_ERR_PERM;
    err = rwy_begin(s);
    if (err) return err;
    x->s = s; x->fn = 0; x->ctx = 0; x->n = 0; x->nheld = 0;
    s->bc_err = 0;
    x->t = bc_active(s);
    x->rdir = 0;
    out->engaged = is_engaged(s, x->t);
    if (out->engaged && rolled_back(s, x->t)) s->bc_err = RWY_ERR_ROLLBACK;
    *rc = s->bc_err;
    if (*rc) out->engaged = 0;
    return RWY_OK;
}

/* The commit of select() failed: pick what to boot without writing anything. */
/* Scan the tree and pick the newest OK slot below `after`. */
static int fallback_find(sel_t *x, rwy_boot_choice_t *out, int rc, const char *after, int after_recovery,
                         pick_t *p) {
    rwy_store_t *s = x->s;
    bound_t b = { 0 };

    if (out->engaged || rc == RWY_ERR_ROLLBACK)
        fallback_scan(x, after_recovery ? 0 : after, &b);
    if (rc == RWY_OK && after && !after_recovery && !b.bounded) rc = RWY_ERR_INVAL;
    if (rc == RWY_OK && out->engaged && !after_recovery)
        pick_best(s, x->t, &b, s->rec, p);
    if (rc == RWY_OK && s->bc_err) {
        rc = s->bc_err;
        out->engaged = out->booted = 0;
    }
    return rc;
}

int rwy_bootcfg_fallback(rwy_store_t *s, rwy_boot_choice_t *out, const char *after) {
    sel_t x;
    pick_t p = { 0 };
    int rc, err, after_recovery;
    err = fallback_begin(s, out, &x, &rc);
    if (err) return err;
    after_recovery = after && !rwy_strcmp(after, "RECOVERY");
    rc = fallback_find(&x, out, rc, after, after_recovery, &p);
    if (rc == RWY_OK && p.found) sel_fill(&x, out, p.name, p.dir, p.rev, 0, 0);
    else if ((rc == RWY_OK || rc == RWY_ERR_ROLLBACK) && x.rdir && !after_recovery) sel_fill(&x, out, "RECOVERY", x.rdir, 0, 0, 0);
    (void)rwy_abort(s);
    return rc;
}

/* A chosen slot turned out not to start: mark it FAILED in one commit so selection can run again. */
int rwy_bootcfg_condemn(rwy_store_t *s, const char *slot) {
    uint8_t r[RWY_REC_PAYLOAD];
    rwy_ss_entry_t e;
    uint64_t t, o, rev;
    uint32_t l = 0;
    int rc, raise;
    if (!s || !slot) return RWY_ERR_INVAL;
    if (!bc_is_system(s)) return RWY_ERR_PERM;
    while (slot[l]) l++;
    if (bc_dir_kind(slot, l) != K_DIR_SLOT) return RWY_ERR_INVAL;
    rc = rwy_begin(s);
    if (rc) return rc;
    t = bc_active(s);
    if (!t || rwy_dir_lookup(s, t, slot, l, &o, 0) != RWY_OK || !bc_read_u64(s, o, "REVISION", &rev)) {
        (void)rwy_abort(s);
        return RWY_ERR_NOENT;
    }
    bc_record_load(s, r);
    e = bc_ss_entry(r, rev);
    e.state = ST_FAILED;
    e.cause = RWY_SS_CAUSE_LOAD;
    rc = rwy_ss_put(r, rev, &e);
    raise = rwy_anchor_raise_rec(s, r);
    if (rc == RWY_OK) {
        (void)rwy_flight_record(s, r, slot, RWY_FLIGHT_EVENT_CONDEMN, (uint8_t)rwy_ss_reason(r), s->rec_seq + 1);
        rc = rwy_record_write(s, r);
    }
    if (rc == RWY_OK && raise) rwy_anchor_raised(s);
    (void)rwy_abort(s);
    return rc;
}

/* Revision and state of slot `o`: rank 0 unusable, 1 FAILED, 2 OK, -1 pending. */
static int recycle_rank(rwy_store_t *s, uint64_t o, uint64_t *rev) {
    int st;
    if (!bc_read_u64(s, o, "REVISION", rev)) return 0;
    st = bc_ss_entry(s->rec, *rev).state;
    return st == ST_OK ? 2 : st == ST_FAILED ? 1 : -1;
}

/* Recycle the lowest rank first (FAILED, OK, then INSTALLED), then the oldest revision, then the lowest index. */
static int recycle_better(int rank, uint64_t rev, uint64_t idx, int best, uint64_t best_rev, uint64_t best_idx) {
    if (best < 0 || rank < best) return 1;
    return rank == best && (rev < best_rev || (rev == best_rev && idx < best_idx));
}

typedef struct {
    int rank;
    uint64_t rev, idx;
    char name[32];
} recycle_cand_t;

/* Put `x` in the sorted list `c` of `*n` entries (at most `max`); the worst one falls off when it is full. */
static void recycle_insert(recycle_cand_t *c, size_t *n, size_t max, const recycle_cand_t *x) {
    size_t i = *n;
    if (i == max) {
        if (!recycle_better(x->rank, x->rev, x->idx, c[max - 1].rank, c[max - 1].rev, c[max - 1].idx)) return;
        i--;
    } else {
        (*n)++;
    }
    for (; i > 0 && recycle_better(x->rank, x->rev, x->idx, c[i - 1].rank, c[i - 1].rev, c[i - 1].idx); i--) c[i] = c[i - 1];
    c[i] = *x;
}

typedef struct {
    char booted[RWY_NAME_MAX + 1];
    uint64_t newest, second, next;
    int have_newest, have_second, keep_second, installed;
} recycle_ctx_t;

/* Find the revisions of the two newest OK slots and of the newest slot that is not FAILED (the next boot). */
static void recycle_scan_ok(rwy_store_t *s, uint64_t t, recycle_ctx_t *c) {
    char n[RWY_NAME_MAX + 1];
    uint32_t l;
    uint64_t o, rev = 0;
    size_t i = 0;

    c->have_newest = c->have_second = 0;
    c->next = 0;
    while (bc_next_dir(s, t, &i, n, &l, &o)) {
        int rank = bc_dir_kind(n, l) == K_DIR_SLOT ? recycle_rank(s, o, &rev) : 0;
        if ((rank == 2 || rank == -1) && rev > c->next) c->next = rev;
        if (rank != 2) continue;
        if (!c->have_newest || rev > c->newest) {
            c->second = c->newest; c->have_second = c->have_newest;
            c->newest = rev; c->have_newest = 1;
        } else if (!c->have_second || rev > c->second) {
            c->second = rev; c->have_second = 1;
        }
    }
}

/* Rank 3 for a pending slot that may go (INSTALLED, not the next boot), else -1. */
static int recycle_pending_rank(rwy_store_t *s, const recycle_ctx_t *c, uint64_t rev) {
    return c->installed && rev != c->next && bc_ss_entry(s->rec, rev).state == ST_INSTALLED ? 3 : -1;
}

/* Fill `x` and return 1 when the directory entry is a slot that may go. */
static int recycle_eligible(rwy_store_t *s, const recycle_ctx_t *c, const char *n, uint32_t l, uint64_t o, recycle_cand_t *x) {
    size_t k;
    if (bc_dir_kind(n, l) != K_DIR_SLOT || l >= sizeof(x->name)) return 0;
    x->rank = recycle_rank(s, o, &x->rev);
    if (x->rank < 0) x->rank = recycle_pending_rank(s, c, x->rev);
    if (x->rank < 0 || !rwy_strcmp(n, c->booted) || bc_ss_entry(s->rec, x->rev).pinned) return 0;
    if (x->rank == 2 && c->have_newest && x->rev == c->newest) return 0;
    if (x->rank == 2 && c->keep_second && c->have_second && x->rev == c->second) return 0;
    x->idx = slot_index(n);
    for (k = 0; k <= l; k++) x->name[k] = n[k];
    return 1;
}

/* The walk of rwy_bootcfg_recycle_order. `evict` is the eviction plan: it also lets INSTALLED slots go (last) when
 * an OK slot or RECOVERY remains to boot, and without `recovery` it protects the second newest OK slot. */
static int recycle_walk(rwy_store_t *s, char names[][32], size_t max, size_t *count, int evict, int recovery) {
    char n[RWY_NAME_MAX + 1];
    recycle_ctx_t c;
    recycle_cand_t list[RWY_RECYCLE_MAX], x;
    uint32_t l;
    uint64_t t = bc_active(s), o;
    size_t i, k, found = 0;
    if (!t || !names || !count || !max) return RWY_ERR_INVAL;
    if (max > RWY_RECYCLE_MAX) max = RWY_RECYCLE_MAX;
    (void)rwy_ss_booted(s->rec, c.booted);
    c.keep_second = evict && !recovery;
    recycle_scan_ok(s, t, &c);
    c.installed = evict && (c.have_newest || recovery);
    for (i = 0; bc_next_dir(s, t, &i, n, &l, &o);)
        if (recycle_eligible(s, &c, n, l, o, &x)) recycle_insert(list, &found, max, &x);
    for (k = 0; k < found; k++)
        for (i = 0; i < sizeof(names[k]); i++) names[k][i] = list[k].name[i];
    *count = found;
    return RWY_OK;
}

int rwy_bootcfg_recycle_order(rwy_store_t *s, char names[][32], size_t max, size_t *count) {
    return recycle_walk(s, names, max, count, 0, 0);
}

static uint16_t evict_refs[RWY_MAX_INODES];

/* One more link to `oid` goes away; virtual files hold no blocks. */
static void evict_ref(uint64_t oid) {
    if (!(oid & RWY_OID_VIRT) && oid >= 1 && oid <= RWY_MAX_INODES) evict_refs[oid - 1]++;
}

/* Count the links of the slot directory `dir` (named `name`) and of the files in it. */
static int evict_count_slot(rwy_store_t *s, uint64_t dir, const char *name) {
    char path[RWY_MAX_PATH + 1], file[RWY_NAME_MAX + 1];
    size_t pl = rwy_strlen(ROOT_PATH), nl = rwy_strlen(name), i, k;
    uint64_t o;
    int rc = RWY_OK;
    for (i = 0; i < pl; i++) path[i] = ROOT_PATH[i];
    path[pl] = '/';
    for (i = 0; i <= nl; i++) path[pl + 1 + i] = name[i];
    evict_ref(dir);
    s->bc_novirt = 1;
    for (k = 0; rc == RWY_OK && rwy_readdir_nth(s, path, k, file, sizeof(file), 0) == RWY_OK; k++) {
        rc = rwy_dir_lookup(s, dir, file, (uint32_t)rwy_strlen(file), &o, 0);
        if (rc == RWY_OK) evict_ref(o);
    }
    s->bc_novirt = 0;
    return rc;
}

/* A content blob whose other links are all in the planned set goes with its last slot: count its BLOBS name. */
static int evict_count_blobs(rwy_store_t *s, uint64_t t) {
    char name[RWY_NAME_MAX + 1];
    uint64_t blobs, o;
    size_t k, l;
    int rc = rwy_dir_lookup(s, t, "BLOBS", 5, &blobs, 0);
    if (rc == RWY_ERR_NOENT) return RWY_OK;
    for (k = 0; rc == RWY_OK && rwy_readdir_nth(s, ROOT_PATH "/BLOBS", k, name, sizeof(name), 0) == RWY_OK; k++) {
        l = rwy_strlen(name);
        if (!rwy_bootcfg_content_name(name, l)) continue;
        rc = rwy_dir_lookup(s, blobs, name, (uint32_t)l, &o, 0);
        if (rc == RWY_OK && o >= 1 && o <= RWY_MAX_INODES && evict_refs[o - 1] &&
            (uint32_t)evict_refs[o - 1] + 1 == rwy_inode_of(s, o, 1)->nlink)
            evict_ref(o);
    }
    return rc;
}

int rwy_bootcfg_evict_blocks(rwy_store_t *s, char names[][32], size_t n, uint32_t *freed) {
    uint64_t t = bc_active(s), d;
    uint8_t type;
    size_t i;
    int rc = RWY_OK;
    if (!t || !names || !freed) return RWY_ERR_INVAL;
    for (i = 0; i < RWY_MAX_INODES; i++) evict_refs[i] = 0;
    for (i = 0; rc == RWY_OK && i < n; i++) {
        rc = rwy_dir_lookup(s, t, names[i], (uint32_t)rwy_strlen(names[i]), &d, &type);
        if (rc == RWY_OK && (type != RWY_TYPE_DIR || bc_dir_kind(names[i], (uint32_t)rwy_strlen(names[i])) != K_DIR_SLOT))
            rc = RWY_ERR_NOENT;
        if (rc == RWY_OK) rc = evict_count_slot(s, d, names[i]);
    }
    if (rc == RWY_OK) rc = evict_count_blobs(s, t);
    return rc ? rc : rwy_blocks_freed(s, evict_refs, freed);
}

int rwy_bootcfg_quota(rwy_store_t *s, unsigned *percent) {
    uint64_t t = bc_active(s), v;
    if (!t) return RWY_ERR_INVAL;
    if (!bc_read_u64(s, t, "QUOTA", &v) || v < QUOTA_MIN || v > QUOTA_MAX) return RWY_ERR_NOENT;
    *percent = (unsigned)v;
    return RWY_OK;
}

int rwy_bootcfg_loop_guard(rwy_store_t *s, rwy_loop_guard_t *g) {
    uint64_t t = bc_active(s), v;
    uint8_t b[SCALAR_MAX + 1];
    size_t n;
    if (!t) return RWY_ERR_INVAL;
    if (!bc_read_text(s, t, "LOOP-GUARD", b, &n) || bc_loop_guard_state(b, n) != 1) return RWY_ERR_NOENT;
    g->limit = (bc_read_u64(s, t, "LOOP-LIMIT", &v) && v >= LOOP_LIMIT_MIN && v <= LOOP_LIMIT_MAX) ? (unsigned)v : LOOP_LIMIT_DEF;
    g->window = (bc_read_u64(s, t, "LOOP-WINDOW", &v) && v >= LOOP_WINDOW_MIN && v <= LOOP_WINDOW_MAX) ? (unsigned)v : LOOP_WINDOW_DEF;
    return RWY_OK;
}

/* The number of slots, and whether EFI/RUNWAY `t` has a RECOVERY directory. */
static unsigned count_slots(rwy_store_t *s, uint64_t t, int *recovery) {
    char n[RWY_NAME_MAX + 1];
    uint32_t l;
    uint64_t o;
    size_t i = 0;
    unsigned slots = 0;
    *recovery = 0;
    while (bc_next_dir(s, t, &i, n, &l, &o)) {
        if (bc_dir_kind(n, l) == K_DIR_SLOT) slots++;
        else if (bc_dir_kind(n, l) == K_DIR_RECOVERY) *recovery = 1;
    }
    return slots;
}

typedef struct {
    uint32_t new_blocks, used, limit, free;
    unsigned slots, cap, quota;
} evict_goal_t;

/* True when removing `k` slots, which frees `freed` blocks, meets the quota and SLOTS. */
static int evict_goal_met(const evict_goal_t *g, size_t k, uint32_t freed) {
    uint64_t left = g->used > freed ? g->used - freed : 0;
    if (g->cap && g->slots - k + 1 > g->cap) return 0;
    if (!g->quota) return 1;
    return left + g->new_blocks <= g->limit && (uint64_t)g->free + freed >= g->new_blocks;
}

static void evict_goal_load(rwy_store_t *s, uint64_t t, uint32_t new_blocks, evict_goal_t *g) {
    uint64_t v;
    uint32_t usable;
    rwy_pool_usage(s, &g->used, &usable);
    g->new_blocks = new_blocks;
    g->free = rwy_free_blocks(s);
    if (rwy_bootcfg_quota(s, &g->quota) != RWY_OK) g->quota = 0;
    g->limit = (uint32_t)((uint64_t)usable * g->quota / 100);
    g->cap = bc_read_u64(s, t, "SLOTS", &v) && v >= 1 && v <= SLOTS_MAX ? (unsigned)v : 0;
}

/* Say, for each planned removal, whether the quota needed it or only SLOTS. */
static void evict_reasons(const evict_goal_t *g, rwy_evict_plan_t *plan) {
    evict_goal_t q = *g;
    uint32_t freed = 0;
    size_t i;
    q.cap = 0;
    for (i = 0; i < plan->count; i++) {
        plan->reasons[i] = evict_goal_met(&q, i, freed) ? RWY_EVICT_REASON_SLOTS : RWY_EVICT_REASON_QUOTA;
        freed += plan->slot_freed[i];
    }
}

int rwy_bootcfg_evict_plan(rwy_store_t *s, uint32_t new_blocks, rwy_evict_plan_t *plan) {
    uint64_t t = bc_active(s);
    evict_goal_t g;
    size_t n, k;
    int rc, recovery;
    if (!t || !plan) return RWY_ERR_INVAL;
    plan->count = 0;
    plan->freed = 0;
    evict_goal_load(s, t, new_blocks, &g);
    if (!g.quota && !g.cap) return RWY_OK;
    g.slots = count_slots(s, t, &recovery);
    rc = recycle_walk(s, plan->names, RWY_RECYCLE_MAX, &n, 1, recovery);
    if (rc) return rc;
    for (k = 0; k <= n; k++) {
        if (k) {
            uint32_t before = plan->freed;
            rc = rwy_bootcfg_evict_blocks(s, plan->names, k, &plan->freed);
            if (rc) return rc;
            plan->slot_freed[k - 1] = plan->freed - before;
        }
        if (evict_goal_met(&g, k, plan->freed)) {
            plan->count = k;
            evict_reasons(&g, plan);
            return RWY_OK;
        }
    }
    plan->freed = 0;
    return RWY_ERR_NOSPC;
}

int rwy_bootcfg_evict_execute(rwy_store_t *s, const rwy_evict_plan_t *plan) {
    char path[RWY_MAX_PATH + 1];
    size_t pl = rwy_strlen(ROOT_PATH), i, k;
    int rc = RWY_OK;
    if (!bc_active(s) || !plan) return RWY_ERR_INVAL;
    for (k = 0; rc == RWY_OK && k < plan->count; k++) {
        for (i = 0; i < pl; i++) path[i] = ROOT_PATH[i];
        path[pl] = '/';
        for (i = 0; i < sizeof(plan->names[k]); i++) path[pl + 1 + i] = plan->names[k][i];
        s->bc_evict_reason = plan->reasons[k];
        s->bc_evict_blocks = plan->slot_freed[k];
        rc = rwy_rmdir(s, path);
        s->bc_evict_reason = 0;
    }
    return rc;
}

/* Compact when the free blocks are enough but no run holds the largest payload. The install may still split,
 * so a store that cannot be compacted is not a reason to refuse. */
static int make_room_compact(rwy_store_t *s, uint32_t total, uint32_t largest) {
    rwy_frag_info_t fi;
    int rc = rwy_frag_info(s, &fi);
    if (rc || fi.free_blocks < total || fi.largest_free_run >= largest) return rc;
    rc = rwy_defrag(s);
    return rc == RWY_ERR_NOSPC ? RWY_OK : rc;
}

/* 1 when the store is enrolled but no counter is attached: the removals could not raise the epoch. */
static int evict_unanchored(const rwy_store_t *s) {
    uint64_t epoch;
    int enrolled;
    rwy_anchor_get(s, &epoch, &enrolled);
    return enrolled && !s->anchor_ops;
}

int rwy_bootcfg_make_room(rwy_store_t *s, uint32_t total, uint32_t largest) {
    rwy_evict_plan_t plan;
    int rc;
    if ((rc = rwy_bootcfg_evict_plan(s, total, &plan))) goto fail;
    if (!plan.count) return RWY_OK;
    if (evict_unanchored(s)) { rc = RWY_ERR_NOSPC; goto fail; }   /* the removals would raise the epoch */
    if ((rc = rwy_bootcfg_evict_execute(s, &plan))) goto fail;
    if ((rc = rwy_commit(s))) return rc;   /* a failed commit has closed its transaction */
    if ((rc = make_room_compact(s, total, largest))) return rc;
    return rwy_begin(s);
fail:
    rwy_abort(s);
    return rc;
}

int rwy_bootcfg_recycle_pick(rwy_store_t *s, char *name) {
    char first[1][32];
    size_t n, j;
    int rc = rwy_bootcfg_recycle_order(s, first, 1, &n);
    if (rc) return rc;
    if (!n) return RWY_ERR_NOENT;
    for (j = 0; j < sizeof(first[0]); j++) name[j] = first[0][j];
    return RWY_OK;
}
