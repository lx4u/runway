/*
 * rwy_bootaudit.c — audit of EFI/RUNWAY against the rules of the policy
 * layer, and repair of the REVISION counter.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_bootcfg.h"

typedef struct {
    sel_t x;                       /* message buffer and sink */
    rwy_bootcfg_audit_t *r;
    uint64_t high;                 /* highest slot REVISION, finalized or not valid */
    uint64_t live[RWY_SS_ENTRIES];/* revisions of the slots, for the stale entries of the record */
    size_t nlive;
} aud_t;

static void bad(aud_t *a, const char *dir, const char *name, const char *why) {
    bc_cat(&a->x, ROOT_PATH "/");
    if (dir) { bc_cat(&a->x, dir); bc_cat(&a->x, "/"); }
    bc_cat(&a->x, name);
    bc_cat(&a->x, ": ");
    bc_cat(&a->x, why);
    bc_emit(&a->x);
    a->r->violations++;
}

/* Why the scalar `kind` holding `text` is not acceptable; NULL if it is. */
static const char *scalar_fault(int kind, const char *text, size_t n) {
    const char *why;
    int ok;
    if (kind == K_STATE) return bc_state_id((const uint8_t *)text, n) < 0 ? "unknown STATE" : 0;
    ok = bc_setting_check(kind, (const uint8_t *)text, n, &why);
    if (ok >= 0) return ok ? 0 : why;
    return bc_is_dec_u64((const uint8_t *)text, n) ? 0 : "not a decimal number";
}

/* A scalar whose value is checked: not the root counter (audited apart), nor what RECOVERY ignores. */
static int scalar_checked(int where, int kind) {
    if (!bc_is_scalar(kind) || kind == K_ENTRY) return 0;
    if (where == W_ROOT && kind == K_REVISION) return 0;
    return !(where == W_RECOVERY && (kind == K_REVISION || kind == K_STATE));
}

/* Check the size and value of the scalar `name`, held by inode `in`. */
static void aud_scalar(aud_t *a, const char *dname, const char *name, int kind, uint64_t o,
                       const rwy_inode_t *in) {
    char text[SCALAR_MAX + 2];
    size_t n = sizeof(text) - 1;
    const char *why;

    if (in->size > SCALAR_MAX) why = "larger than 64 bytes";
    else if (bc_note(a->x.s, rwy_read(a->x.s, o, 0, text, &n)) != RWY_OK) why = "unreadable";
    else why = scalar_fault(kind, text, bc_strip_nl((const uint8_t *)text, n));
    if (why) bad(a, dname, name, why);
}

static void aud_label(aud_t *a, const char *dname, const char *name, int values, uint64_t o,
                      const rwy_inode_t *in) {
    uint8_t text[RWY_LABEL_MAX + 1];
    size_t n = sizeof(text);

    if (in->size > RWY_LABEL_MAX) {
        bad(a, dname, name, "larger than 128 bytes");
        return;
    }
    if (!values || in->size == 0) return;
    if (bc_note(a->x.s, rwy_read(a->x.s, o, 0, text, &n)) != RWY_OK) {
        bad(a, dname, name, "unreadable");
        return;
    }
    n = bc_strip_nl(text, n);
    if (!bc_label_ok(text, n))
        bad(a, dname, name, "invalid characters");
}

static void aud_entry_name(aud_t *a, const char *dname, const char *name, int values, uint64_t o,
                           const rwy_inode_t *in) {
    uint8_t text[RWY_ENTRY_MAX + 2];
    size_t n = sizeof(text) - 1;

    if (in->size > RWY_ENTRY_MAX + 1) {
        bad(a, dname, name, "larger than 255 bytes");
        return;
    }
    if (!values || in->size == 0) return;
    if (bc_note(a->x.s, rwy_read(a->x.s, o, 0, text, &n)) != RWY_OK) {
        bad(a, dname, name, "unreadable");
        return;
    }
    n = bc_strip_nl(text, n);
    if (!rwy_entry_name_ok((const char *)text, n))
        bad(a, dname, name, "invalid entry name");
}

static void aud_meta(aud_t *a, const char *dname, const char *name, int values, uint64_t o,
                     const rwy_inode_t *in) {
    uint8_t text[RWY_META_MAX + 1];
    size_t n = in->size;

    if (in->size > RWY_META_MAX) {
        bad(a, dname, name, "larger than 1 KiB");
        return;
    }
    if (!values || in->size == 0) return;
    if (n > sizeof(text) - 1) n = sizeof(text) - 1;
    if (bc_note(a->x.s, rwy_read(a->x.s, o, 0, text, &n)) != RWY_OK) {
        bad(a, dname, name, "unreadable");
        return;
    }
    text[n] = 0;
    if (!rwy_meta_ok((const char *)text, n))
        bad(a, dname, name, "invalid metadata");
}

static void aud_content(aud_t *a, const char *dname, const char *name, int where, int kind, int values,
                        uint64_t o, const rwy_inode_t *in) {
    if (kind == K_ARGS) {
        if (in->size > ARGS_MAX) bad(a, dname, name, "larger than 4 KiB");
    } else if (kind == K_LABEL) {
        aud_label(a, dname, name, values, o, in);
    } else if (kind == K_ENTRY) {
        aud_entry_name(a, dname, name, values, o, in);
    } else if (kind == K_META) {
        aud_meta(a, dname, name, values, o, in);
    } else if (kind == K_WDT_APP) {
        if (in->size > WDT_APP_MAX) bad(a, dname, name, "larger than 1 MiB");
    } else if (values && scalar_checked(where, kind)) {
        aud_scalar(a, dname, name, kind, o, in);
    }
}

/* One entry of a directory: its name, type, size and value. */
static void aud_entry(aud_t *a, const char *dname, uint64_t dir, int where, int values, const char *name,
                      uint8_t type) {
    rwy_store_t *s = a->x.s;
    uint32_t l = (uint32_t)rwy_strlen(name);
    int kind = bc_entry_kind(where, name, l);
    uint64_t o;
    const rwy_inode_t *in;

    if (kind == K_NONE) { bad(a, dname, name, "unknown entry"); return; }
    if (!bc_type_ok(kind, type)) { bad(a, dname, name, "wrong type"); return; }
    if (values && where == W_SLOT && (kind == K_STATE || kind == K_TRIES)) {
        bad(a, dname, name, "is in the record, not in the tree");
        return;
    }
    if (bc_note(s, rwy_dir_lookup(s, dir, name, l, &o, 0)) != RWY_OK || !(in = rwy_inode_of(s, o, 1))) return;
    aud_content(a, dname, name, where, kind, values, o, in);
}

/* Names, types, sizes and scalar values of directory `dir` (role `where`, called `dname`, 0 for the root).
 * An incomplete slot is still being written by an installer: its values are not checked until it is finalized. */
static void aud_dir(aud_t *a, const char *dname, uint64_t dir, int where) {
    rwy_store_t *s = a->x.s;
    char path[RWY_MAX_PATH + 1], name[RWY_NAME_MAX + 1];
    uint8_t type;
    size_t i;
    int values = where != W_SLOT || bc_slot_final(s, dir);
    int novirt = s->bc_novirt;
    bc_sel_path(path, dname, "");
    path[rwy_strlen(path) - 1] = 0;                       /* drop the trailing '/' */
    s->bc_novirt = 1;                                      /* virtual files are not stored: nothing to audit */
    for (i = 0; rwy_readdir_nth(s, path, i, name, sizeof(name), &type) == RWY_OK; i++)
        aud_entry(a, dname, dir, where, values, name, type);
    s->bc_novirt = novirt;
}

/* BOOTED must name an existing slot or RECOVERY. */
static void aud_booted(aud_t *a, rwy_store_t *s, char *bname) {
    uint32_t l;

    if (!rwy_ss_booted(s->rec, bname)) return;
    l = (uint32_t)rwy_strlen(bname);
    if (bc_dir_kind(bname, l) != K_DIR_SLOT && bc_dir_kind(bname, l) != K_DIR_RECOVERY)
        bad(a, 0, "BOOTED", "does not name CONFIG<n> or RECOVERY");
    else a->r->booted_gone = 1;
}

static void aud_slot_type_rules(aud_t *a, const char *name, uint64_t o) {
    char entry[RWY_ENTRY_MAX + 1];
    int has_entry = bc_read_entry(a->x.s, o, entry);
    size_t slen;
    int is_efi = 0;
    uint64_t f;

    if (has_entry > 0 && rwy_entry_split(entry, rwy_strlen(entry), &slen, &is_efi) && is_efi) {
        if (rwy_dir_lookup(a->x.s, o, "INITRD", 6, &f, 0) == RWY_OK)
            bad(a, name, "INITRD", "Type 2 slot (.efi) cannot have INITRD");
        if (rwy_dir_lookup(a->x.s, o, "DTB", 3, &f, 0) == RWY_OK)
            bad(a, name, "DTB", "Type 2 slot (.efi) cannot have DTB");
        if (rwy_dir_lookup(a->x.s, o, "ARGS", 4, &f, 0) == RWY_OK)
            bad(a, name, "ARGS", "Type 2 slot (.efi) cannot have ARGS");
        if (rwy_dir_lookup(a->x.s, o, "META", 4, &f, 0) == RWY_OK)
            bad(a, name, "META", "Type 2 slot (.efi) cannot have META");
    }
}

static void aud_slot(aud_t *a, const char *name, uint64_t o) {
    rwy_store_t *s = a->x.s;
    const char *f;
    uint64_t rev;

    a->r->slots++;
    aud_dir(a, name, o, W_SLOT);
    aud_slot_type_rules(a, name, o);
    if (bc_read_u64(s, o, "REVISION", &rev)) {
        if (rev > a->high) a->high = rev;
        if (a->nlive < RWY_SS_ENTRIES) a->live[a->nlive++] = rev;
        if (!rwy_ss_cause_ok(s->rec, rev)) bad(a, 0, name, "record entry has an invalid cause");
    }
    f = bc_slot_fault(s, o, name, &rev);
    if (f && !*f) a->r->incomplete++;
    else if (f) bad(a, 0, name, f);
}

static void aud_children(aud_t *a, uint64_t t, const char *bname) {
    rwy_store_t *s = a->x.s;
    char name[RWY_NAME_MAX + 1];
    uint32_t l;
    uint64_t o;
    size_t i = 0;
    const char *f;

    while (bc_next_dir(s, t, &i, name, &l, &o)) {
        if (a->r->booted_gone && !rwy_strcmp(name, bname)) a->r->booted_gone = 0;   /* it exists */
        switch (bc_dir_kind(name, l)) {
        case K_DIR_BLOBS:
            aud_dir(a, name, o, W_BLOBS);
            break;
        case K_DIR_RECOVERY:
            aud_dir(a, name, o, W_RECOVERY);
            if ((f = bc_payload_fault(s, o, name)) != 0) bad(a, 0, name, f);
            break;
        case K_DIR_WDT:
            aud_dir(a, name, o, W_WDT);
            break;
        case K_DIR_SLOT:
            aud_slot(a, name, o);
            break;
        }
    }
}

/* Too many slots, and entries of the record that no slot has anymore. */
static void aud_limits(aud_t *a, uint64_t t) {
    rwy_store_t *s = a->x.s;
    rwy_bootcfg_audit_t *out = a->r;
    uint64_t lim;
    unsigned k;

    if (out->slots > (int)SLOTS_MAX) {
        char d[24];
        bc_cat(&a->x, ROOT_PATH ": "); bc_cat(&a->x, bc_dec(d, (uint64_t)out->slots)); bc_cat(&a->x, " slots, more than "); bc_cat(&a->x, bc_dec(d, SLOTS_MAX));
        bc_emit(&a->x);
        out->violations++;
    }
    if (bc_read_u64(s, t, "SLOTS", &lim) && lim >= 1 && lim <= SLOTS_MAX && (uint64_t)out->slots > lim)
        out->over_slots = (int)lim;
    if (a->nlive < RWY_SS_ENTRIES) {      /* a full list may have been cut short: say nothing then */
        uint8_t r[RWY_REC_PAYLOAD], p[RWY_REC_PAYLOAD];
        (void)rwy_record_read(s, r);
        (void)rwy_record_read(s, p);
        rwy_ss_prune(p, a->live, a->nlive);
        for (k = 0; k < RWY_REC_PAYLOAD; k++)
            if (r[k] != p[k]) out->stale = 1;
    }
}

/* The counter must not trail the slots; missing is fine while nothing is finalized. */
static void aud_counter(aud_t *a, uint64_t t, int present) {
    rwy_store_t *s = a->x.s;
    const char *why = 0;
    uint64_t c = 0;

    a->r->counter = a->high;
    if (present && !bc_read_u64(s, t, "REVISION", &c)) why = "unparsable";
    else if (present ? c < a->high : a->high > 0) why = present ? "behind the highest slot revision" : "missing";
    if (!why) return;
    bc_cat(&a->x, ROOT_PATH "/REVISION: ");
    bc_cat(&a->x, why);
    bc_emit(&a->x);
    a->r->counter_bad = 1;
}

/*
 * A counter below rev_hw is a tree that was put back; selection refuses it too. A counter that
 * repair raises to the highest slot revision is not one, unless that revision is below rev_hw as well.
 */
static void aud_replayed(aud_t *a, uint64_t t, int present) {
    rwy_store_t *s = a->x.s;
    uint64_t c = 0;
    char d[24];

    rwy_anchor_get(s, &a->r->epoch, &a->r->enrolled);
    if (a->r->counter_bad) c = a->high;
    else if (!present || !bc_read_u64(s, t, "REVISION", &c)) c = 0;
    if (c >= rwy_ss_rev_hw(s->rec)) return;
    bc_cat(&a->x, ROOT_PATH ": the tree is older than the record (REVISION ");
    bc_cat(&a->x, bc_dec(d, c));
    bc_cat(&a->x, ", rev_hw ");
    bc_cat(&a->x, bc_dec(d, rwy_ss_rev_hw(s->rec)));
    bc_cat(&a->x, ")");
    bc_emit(&a->x);
    a->r->replayed = 1;
    a->r->violations++;
}

int rwy_bootcfg_audit(rwy_store_t *s, rwy_bootcfg_audit_t *out, rwy_log_fn log, void *ctx) {
    aud_t a;
    char bname[RWY_NAME_MAX + 1];
    uint64_t t;
    int present, rc;
    unsigned k;
    for (k = 0; k < sizeof(*out); k++) ((uint8_t *)out)[k] = 0;
    if (!s) return RWY_ERR_INVAL;
    rc = rwy_begin(s);
    if (rc) return rc;
    a.x.s = s; a.x.fn = log; a.x.ctx = ctx; a.x.n = 0;
    a.r = out;
    a.high = 0;
    a.nlive = 0;
    s->bc_err = 0;
    t = bc_active(s);
    if (!t) {
        rc = s->bc_err;
        (void)rwy_abort(s);
        return rc;
    }
    out->engaged = 1;
    aud_dir(&a, 0, t, W_ROOT);
    out->record_damaged = rwy_record_damaged(s);
    aud_booted(&a, s, bname);
    aud_children(&a, t, bname);
    aud_limits(&a, t);
    present = rwy_dir_lookup(s, t, "REVISION", 8, 0, 0) == RWY_OK;
    aud_counter(&a, t, present);
    aud_replayed(&a, t, present);
    rc = s->bc_err;
    (void)rwy_abort(s);
    if (rc) for (k = 0; k < sizeof(*out); k++) ((uint8_t *)out)[k] = 0;
    return rc;
}

int rwy_bootcfg_failed(rwy_store_t *s, rwy_log_fn log, void *ctx) {
    sel_t x = { 0 };
    char name[RWY_NAME_MAX + 1];
    uint64_t t, o, rev;
    uint32_t l;
    size_t i = 0;
    int n = 0, rc;
    if (!s) return RWY_ERR_INVAL;
    rc = rwy_begin(s);
    if (rc) return rc;
    x.s = s; x.fn = log; x.ctx = ctx;
    s->bc_err = 0;
    t = bc_active(s);
    while (t && bc_next_dir(s, t, &i, name, &l, &o)) {
        rwy_ss_entry_t e;
        if (bc_dir_kind(name, l) != K_DIR_SLOT || !bc_read_u64(s, o, "REVISION", &rev)) continue;
        e = bc_ss_entry(s->rec, rev);
        if (e.state != ST_FAILED) continue;
        bc_cat(&x, name); bc_cat(&x, " FAILED (");
        bc_cat(&x, e.cause ? BC_CAUSE_NAMES[e.cause - 1] : "cause unknown"); bc_cat(&x, ")");
        bc_cat_label_dir(&x, o);
        bc_emit(&x);
        n++;
    }
    rc = s->bc_err;
    (void)rwy_abort(s);
    return rc ? rc : n;
}

int rwy_bootcfg_repair_counter(rwy_store_t *s) {
    rwy_bootcfg_audit_t r;
    int rc, priv;
    if (!s) return RWY_ERR_INVAL;
    if (!bc_is_system(s)) return RWY_ERR_PERM;
    rc = rwy_bootcfg_audit(s, &r, 0, 0);
    if (rc || !r.engaged || !r.counter_bad) return rc;
    rc = rwy_begin(s);
    if (rc) return rc;
    priv = s->bc_priv;
    s->bc_priv = 1;
    rc = bc_put_u64(s, ROOT_PATH "/REVISION", r.counter);
    s->bc_priv = priv;
    if (rc == RWY_OK) rc = rwy_commit(s);
    if (rc && s->in_txn) (void)rwy_abort(s);
    return rc ? rc : 1;
}

int rwy_bootcfg_repair_record(rwy_store_t *s) {
    rwy_bootcfg_audit_t r;
    uint8_t rec[RWY_REC_PAYLOAD];
    int rc;
    if (!s) return RWY_ERR_INVAL;
    if (!bc_is_system(s)) return RWY_ERR_PERM;
    rc = rwy_bootcfg_audit(s, &r, 0, 0);
    if (rc || !r.engaged || !(r.record_damaged || r.stale)) return rc;
    rc = rwy_begin(s);
    if (rc) return rc;
    bc_record_load(s, rec);                    /* drops the stale entries; empty when damaged */
    rc = rwy_record_write(s, rec);
    (void)rwy_abort(s);
    return rc ? rc : 1;
}
