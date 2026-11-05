/*
 * rwy_virt.c — Virtual files of EFI/RUNWAY: names that a directory lists and
 * resolves without an inode behind them. The table says where each one lives;
 * what it holds and who may change it is handled here.
 * SPDX-License-Identifier: Apache-2.0
 *
 * oid = RWY_OID_VIRT | kind << RWY_VKIND_SHIFT | oid of the directory. Mutators that take an
 * oid refuse a virtual one with RWY_ERR_PERM (rwy_mut_inode); the ones below handle it.
 */
#include "rwy_internal.h"
#include "rwy_bootcfg.h"
#include "rwy_flight.h"
#include "rwy_netset.h"

/* REASON text of record code n + 1. */
static const char *const REASON_NAMES[] = { "normal", "requested", "all_failed", "invalid_store", "boot_loop" };

typedef struct {
    const char *name;
    uint8_t role, type, finalized;   /* finalized: only in a slot that has its REVISION */
    uint32_t mode;
} vfile_t;

static const vfile_t VFILES[RWY_VK_COUNT] = {
    [RWY_VK_INDEX]        = { "INDEX",        RWY_ROLE_SLOT, RWY_TYPE_FILE,    0, RWY_S_IFREG | 0444 },
    [RWY_VK_STATE]        = { "STATE",        RWY_ROLE_SLOT, RWY_TYPE_FILE,    1, RWY_S_IFREG | RWY_EFI_DEFAULT_MODE_FILE },
    [RWY_VK_TRIES]        = { "TRIES",        RWY_ROLE_SLOT, RWY_TYPE_FILE,    1, RWY_S_IFREG | RWY_EFI_DEFAULT_MODE_FILE },
    [RWY_VK_CAUSE]        = { "CAUSE",        RWY_ROLE_SLOT, RWY_TYPE_FILE,    1, RWY_S_IFREG | 0444 },
    [RWY_VK_PIN]          = { "PIN",          RWY_ROLE_SLOT, RWY_TYPE_FILE,    1, RWY_S_IFREG | RWY_EFI_DEFAULT_MODE_FILE },
    [RWY_VK_BOOTED]       = { "BOOTED",       RWY_ROLE_ROOT, RWY_TYPE_SYMLINK, 0, RWY_S_IFLNK | 0777 },
    [RWY_VK_REASON]       = { "REASON",       RWY_ROLE_ROOT, RWY_TYPE_FILE,    0, RWY_S_IFREG | 0444 },
    [RWY_VK_WDT_HINT]     = { "WDT_HINT",     RWY_ROLE_ROOT, RWY_TYPE_FILE,    0, RWY_S_IFREG | 0444 },
    [RWY_VK_RECOVER_NEXT] = { "RECOVER_NEXT", RWY_ROLE_ROOT, RWY_TYPE_FILE,    0, RWY_S_IFREG | RWY_EFI_DEFAULT_MODE_FILE },
    [RWY_VK_FLIGHT]       = { "FLIGHT",       RWY_ROLE_ROOT, RWY_TYPE_FILE,    0, RWY_S_IFREG | 0444 },
    [RWY_VK_LOG]          = { "LOG",          RWY_ROLE_ROOT, RWY_TYPE_FILE,    0, RWY_S_IFREG | 0444 },
};

static uint64_t vmake(unsigned kind, uint64_t dir) {
    return RWY_OID_VIRT | ((uint64_t)kind << RWY_VKIND_SHIFT) | dir;
}

static unsigned vkind(uint64_t oid) { return (unsigned)((oid & ~RWY_OID_VIRT) >> RWY_VKIND_SHIFT); }

static uint64_t vdir(uint64_t oid) { return oid & ((1ull << RWY_VKIND_SHIFT) - 1); }

static int same(const char *a, const char *b, uint32_t blen) {
    return rwy_strlen(a) == blen && rwy_strncmp(a, b, blen) == 0;
}

/* Whether virtual file `kind` exists in directory `dir` right now. */
static int vexists(rwy_store_t *s, unsigned kind, uint64_t dir) {
    char slot[RWY_NAME_MAX + 1];
    uint64_t rev;

    if (rwy_bootcfg_role(s, dir, slot) != VFILES[kind].role) return 0;
    switch (kind) {
    case RWY_VK_STATE: case RWY_VK_TRIES: case RWY_VK_PIN: return bc_read_u64(s, dir, "REVISION", &rev);
    case RWY_VK_CAUSE: return bc_read_u64(s, dir, "REVISION", &rev) && bc_ss_entry(s->rec, rev).cause != RWY_SS_CAUSE_NONE;
    case RWY_VK_BOOTED: return rwy_ss_booted(s->rec, slot) != 0;
    case RWY_VK_REASON: return rwy_ss_reason(s->rec) != 0;
    case RWY_VK_WDT_HINT: return rwy_ss_wdt(s->rec, slot) != 0;
    case RWY_VK_RECOVER_NEXT: return rwy_ss_recover_next(s->rec);
    case RWY_VK_FLIGHT: return s->sb.flight_blocks > 0;
    case RWY_VK_LOG: return s->sb.net_blocks > 0;
    default: return 1;
    }
}

int rwy_virt_reserved(int role, int final, const char *name, uint32_t nlen) {
    for (unsigned k = 0; k < RWY_VK_COUNT; k++)
        if (VFILES[k].role == role && (final || !VFILES[k].finalized) && same(VFILES[k].name, name, nlen)) return 1;
    return 0;
}

int rwy_virt_lookup(rwy_store_t *s, uint64_t dir, const char *name, uint32_t nlen, uint64_t *oid) {
    for (unsigned k = 0; k < RWY_VK_COUNT; k++) {
        if (!same(VFILES[k].name, name, nlen) || !vexists(s, k, dir)) continue;
        *oid = vmake(k, dir);
        return RWY_OK;
    }
    return RWY_ERR_NOENT;
}

int rwy_virt_nth(rwy_store_t *s, uint64_t dir, size_t index, char *name, size_t name_sz, uint8_t *type) {
    for (unsigned k = 0; k < RWY_VK_COUNT; k++) {
        size_t n = rwy_strlen(VFILES[k].name);
        if (!vexists(s, k, dir) || index--) continue;
        if (n + 1 > name_sz) return RWY_ERR_NAMETOOLONG;
        for (size_t i = 0; i <= n; i++) name[i] = VFILES[k].name[i];
        if (type) *type = VFILES[k].type;
        return RWY_OK;
    }
    return RWY_ERR_NOENT;
}

static size_t put_text(char *out, const char *t, int newline) {
    size_t n = 0;
    while (*t) out[n++] = *t++;
    if (newline) out[n++] = '\n';
    return n;
}

/* The virtual files that show a field of the record entry of a slot. */
static int vkind_is_entry(unsigned kind) {
    return kind == RWY_VK_STATE || kind == RWY_VK_TRIES || kind == RWY_VK_CAUSE || kind == RWY_VK_PIN;
}

static int vcontent(rwy_store_t *s, unsigned kind, uint64_t dir, char *out, size_t *len) {
    char slot[RWY_NAME_MAX + 1], d[24];
    rwy_ss_entry_t e = { 1, RWY_SS_INSTALLED, RWY_SS_CAUSE_NONE, 0 };
    uint64_t rev;

    if (vkind_is_entry(kind)) {
        if (!bc_read_u64(s, dir, "REVISION", &rev)) return RWY_ERR_NOENT;
        e = bc_ss_entry(s->rec, rev);
    }
    switch (kind) {
    case RWY_VK_INDEX:
        if (rwy_bootcfg_role(s, dir, slot) != RWY_ROLE_SLOT) return RWY_ERR_NOENT;
        *len = put_text(out, slot + 6, 1);
        break;
    case RWY_VK_STATE: *len = put_text(out, BC_STATE_NAMES[e.state], 1); break;
    case RWY_VK_TRIES: *len = put_text(out, bc_dec(d, e.tries), 1); break;
    case RWY_VK_PIN: *len = put_text(out, e.pinned ? "1" : "0", 1); break;
    case RWY_VK_CAUSE: *len = put_text(out, BC_CAUSE_NAMES[e.cause - 1], 1); break;
    case RWY_VK_BOOTED: *len = rwy_ss_booted(s->rec, out); break;
    case RWY_VK_REASON: *len = put_text(out, REASON_NAMES[rwy_ss_reason(s->rec) - 1], 1); break;
    case RWY_VK_WDT_HINT: (void)rwy_ss_wdt(s->rec, slot); *len = put_text(out, slot, 1); break;
    default: *len = 0; break;   /* RECOVER_NEXT is a marker */
    }
    return RWY_OK;
}

/* Text of the virtual file behind `oid` in `out` (RWY_VTEXT_MAX bytes). */
static int vtext(rwy_store_t *s, uint64_t oid, char *out, size_t *len) {
    if (!(oid & RWY_OID_VIRT) || vkind(oid) >= RWY_VK_COUNT || !vexists(s, vkind(oid), vdir(oid)))
        return RWY_ERR_NOENT;
    return vcontent(s, vkind(oid), vdir(oid), out, len);
}

int rwy_virt_attr(rwy_store_t *s, uint64_t oid, rwy_attr_t *out) {
    char txt[RWY_VTEXT_MAX];
    size_t n;
    const rwy_inode_t *d;
    int rc;

    if (vkind(oid) == RWY_VK_FLIGHT) {
        if (!vexists(s, vkind(oid), vdir(oid))) return RWY_ERR_NOENT;
        rc = rwy_flight_vread(s, 0, NULL, NULL, &n);
        if (rc) return rc;
    } else if (vkind(oid) == RWY_VK_LOG) {
        if (!vexists(s, vkind(oid), vdir(oid))) return RWY_ERR_NOENT;
        rc = rwy_log_vread(&s->dev, s->sb.net_start, s->sb.net_blocks, 0, NULL, NULL, &n);
        if (rc) return rc;
    } else {
        rc = vtext(s, oid, txt, &n);
        if (rc) return rc;
    }
    d = rwy_inode_of(s, vdir(oid), s->in_txn);
    if (!d) return RWY_ERR_NOENT;
    rwy_fill_attr(d, out);                           /* times of the directory */
    out->oid = oid;
    out->mode = VFILES[vkind(oid)].mode;
    out->uid = RWY_EFI_DEFAULT_UID;
    out->gid = RWY_EFI_DEFAULT_GID;
    out->size = n;
    out->type = VFILES[vkind(oid)].type;
    out->nlink = 1;
    return RWY_OK;
}

int rwy_virt_read(rwy_store_t *s, uint64_t oid, uint64_t pos, void *buf, size_t *len) {
    char txt[RWY_VTEXT_MAX];
    size_t n, i, got = 0;
    int rc;

    if (vkind(oid) == RWY_VK_FLIGHT) {
        if (!vexists(s, vkind(oid), vdir(oid))) return RWY_ERR_NOENT;
        return rwy_flight_vread(s, pos, buf, len, NULL);
    }
    if (vkind(oid) == RWY_VK_LOG) {
        if (!vexists(s, vkind(oid), vdir(oid))) return RWY_ERR_NOENT;
        return rwy_log_vread(&s->dev, s->sb.net_start, s->sb.net_blocks, pos, buf, len, NULL);
    }
    rc = vtext(s, oid, txt, &n);
    if (rc) return rc;
    for (i = pos; i < n && got < *len; i++) ((char *)buf)[got++] = txt[i];
    *len = got;
    return RWY_OK;
}

int rwy_virt_readlink(rwy_store_t *s, uint64_t oid, char *out, size_t outsz) {
    size_t n = outsz - 1;
    int rc;
    if (!(oid & RWY_OID_VIRT) || vkind(oid) >= RWY_VK_COUNT || VFILES[vkind(oid)].type != RWY_TYPE_SYMLINK)
        return RWY_ERR_INVAL;
    rc = rwy_virt_read(s, oid, 0, out, &n);
    if (rc) return rc;
    out[n] = 0;
    return RWY_OK;
}

/* A new TRIES value for the record entry. */
static int vwrite_tries(rwy_store_t *s, const uint8_t *b, size_t n, rwy_ss_entry_t *e) {
    uint64_t v = 0;
    size_t i;

    if (!s->bc_priv && !bc_is_system(s)) return RWY_ERR_PERM;
    if (!bc_is_dec_u64(b, n)) return RWY_ERR_INVAL;
    for (i = 0; i < n; i++) v = v * 10 + (uint64_t)(b[i] - '0');
    e->tries = v;
    return RWY_OK;
}

/* A new PIN for the record entry: 0 or 1. */
static int vwrite_pin(const uint8_t *b, size_t n, rwy_ss_entry_t *e) {
    if (n != 1 || (b[0] != '0' && b[0] != '1')) return RWY_ERR_INVAL;
    e->pinned = b[0] == '1';
    return RWY_OK;
}

/* A new STATE for the record entry; *raise says the epoch has to be raised (a slot just failed). */
static int vwrite_state(rwy_store_t *s, const uint8_t *b, size_t n, uint8_t *rec, rwy_ss_entry_t *e, int *raise) {
    int id = bc_state_id(b, n);

    if (id < 0) return RWY_ERR_INVAL;
    if (!s->bc_priv) {
        if (id == ST_INSTALLED || (id == ST_TESTING && !bc_is_system(s))) return RWY_ERR_PERM;
        if (!bc_is_system(s) && id != ST_FAILED && e->state == ST_FAILED) return RWY_ERR_PERM;   /* FAILED is final for USER */
    }
    *raise = id == ST_FAILED && e->state != ST_FAILED && rwy_anchor_raise_rec(s, rec);
    e->cause = (uint8_t)(id != ST_FAILED ? RWY_SS_CAUSE_NONE : e->state == ST_FAILED ? e->cause : RWY_SS_CAUSE_OPERATOR);
    e->state = (uint8_t)id;
    return RWY_OK;
}

static void vwrite_flight_record(rwy_store_t *s, uint64_t dir, const uint8_t *rec) {
    char slot[RWY_NAME_MAX + 1];
    (void)rwy_bootcfg_role(s, dir, slot);
    (void)rwy_flight_record(s, rec, slot, RWY_FLIGHT_EVENT_TRIES, (uint8_t)rwy_ss_reason(rec), s->rec_seq + 1);
}

static void vwrite_log_record(rwy_store_t *s, unsigned kind, uint64_t dir, const uint8_t *r,
                              const rwy_ss_entry_t *e, uint64_t rev) {
    if (kind == RWY_VK_TRIES) {
        vwrite_flight_record(s, dir, r);
    } else if (kind == RWY_VK_STATE && (e->state == ST_OK || e->state == ST_FAILED)) {
        char slot[RWY_NAME_MAX + 1] = { 0 };
        char label[RWY_LABEL_MAX + 1] = { 0 };
        uint8_t payload_hash[32] = { 0 };
        uint8_t event = e->state == ST_OK ? RWY_JOURNAL_EVENT_CONFIRM : RWY_JOURNAL_EVENT_FAIL;
        (void)rwy_bootcfg_role(s, dir, slot);
        (void)bc_read_label(s, dir, label);
        (void)rwy_transplog_calc_payload_hash(s, slot, payload_hash);
        (void)rwy_transplog_record(s, slot, event, rev, label, payload_hash, s->rec_seq + 1, NULL);
    }
}

/* OK written to a slot that is OK: the health signal of the OS, with nothing to record. */
static int vwrite_is_confirmed(unsigned kind, const rwy_ss_entry_t *e, const uint8_t *b, size_t n) {
    return kind == RWY_VK_STATE && e->state == ST_OK && bc_state_id(b, n) == ST_OK;
}

/* Apply a write to STATE, TRIES or PIN to the record entry `e`. */
static int vwrite_apply(rwy_store_t *s, unsigned kind, const uint8_t *b, size_t n, uint8_t *rec,
                        rwy_ss_entry_t *e, int *raise) {
    if (kind == RWY_VK_TRIES) return vwrite_tries(s, b, n, e);
    if (kind == RWY_VK_PIN) return vwrite_pin(b, n, e);
    return vwrite_state(s, b, n, rec, e, raise);
}

/* A write to STATE, TRIES or PIN of a finalized slot (whole-file replace): the record changes, the tree does not. */
static int vwrite(rwy_store_t *s, unsigned kind, uint64_t dir, uint64_t pos, const void *buf, size_t len) {
    uint8_t r[RWY_REC_PAYLOAD];
    rwy_ss_entry_t e;
    uint64_t rev;
    size_t n;
    int rc, raise = 0;
    if (kind == RWY_VK_RECOVER_NEXT) return len ? RWY_ERR_INVAL : RWY_OK;   /* a marker: empty or nothing */
    if (kind != RWY_VK_STATE && kind != RWY_VK_TRIES && kind != RWY_VK_PIN) return RWY_ERR_PERM;   /* INDEX, BOOTED, REASON and WDT are written by RUNWAY.EFI only */
    if (!bc_read_u64(s, dir, "REVISION", &rev)) return RWY_ERR_NOENT;
    if (len == 0) return RWY_OK;
    if (pos != 0 || len > SCALAR_MAX) return RWY_ERR_INVAL;
    n = bc_strip_nl(buf, len);
    bc_record_load(s, r);
    e = bc_ss_entry(r, rev);
    if (vwrite_is_confirmed(kind, &e, buf, n)) return RWY_OK;
    rc = vwrite_apply(s, kind, buf, n, r, &e, &raise);
    if (rc) return rc;
    rc = rwy_ss_put(r, rev, &e);
    if (!rc) {
        vwrite_log_record(s, kind, dir, r, &e, rev);
        rc = rwy_record_write(s, r);
    }
    if (!rc && raise) rwy_anchor_raised(s);
    return rc;
}

int rwy_virt_write(rwy_store_t *s, uint64_t oid, uint64_t pos, const void *buf, size_t len) {
    if (vkind(oid) >= RWY_VK_COUNT || !vexists(s, vkind(oid), vdir(oid))) return RWY_ERR_NOENT;
    return vwrite(s, vkind(oid), vdir(oid), pos, buf, len);
}

int rwy_virt_truncate(rwy_store_t *s, uint64_t oid, uint64_t size) {
    unsigned kind = vkind(oid);
    if (kind >= RWY_VK_COUNT || !vexists(s, kind, vdir(oid))) return RWY_ERR_NOENT;
    /* `echo OK > STATE` truncates first under FUSE; keep the old value until the write replaces it. */
    if (kind != RWY_VK_STATE && kind != RWY_VK_TRIES && kind != RWY_VK_PIN && kind != RWY_VK_RECOVER_NEXT)
        return RWY_ERR_PERM;
    return size ? RWY_ERR_INVAL : RWY_OK;
}

int rwy_virt_unlink(rwy_store_t *s, uint64_t oid) {
    uint8_t r[RWY_REC_PAYLOAD];
    unsigned kind = vkind(oid);
    if (kind >= RWY_VK_COUNT || !vexists(s, kind, vdir(oid))) return RWY_ERR_NOENT;
    if (VFILES[kind].role == RWY_ROLE_SLOT) return RWY_OK;   /* not stored: the slot's rmdir removes them */
    if (kind != RWY_VK_RECOVER_NEXT) return RWY_ERR_PERM;
    (void)rwy_record_read(s, r);
    rwy_ss_set_recover_next(r, 0);
    return rwy_record_write(s, r);
}

/* chmod, chown and utimens: a no-op on the RECOVER_NEXT flag (so `touch` works), refused on the others. */
int rwy_virt_setattr(rwy_store_t *s, uint64_t oid) {
    if (vkind(oid) >= RWY_VK_COUNT || !vexists(s, vkind(oid), vdir(oid))) return RWY_ERR_NOENT;
    return vkind(oid) == RWY_VK_RECOVER_NEXT ? RWY_OK : RWY_ERR_PERM;
}

/* RECOVER_NEXT is a flag of the record: the request comes and goes with one record write. */
static int vcreate(rwy_store_t *s) {
    uint8_t r[RWY_REC_PAYLOAD];
    (void)rwy_record_read(s, r);
    if (rwy_ss_recover_next(r)) return RWY_ERR_EXIST;
    rwy_ss_set_recover_next(r, 1);
    (void)rwy_flight_record(s, r, "RECOVERY", RWY_FLIGHT_EVENT_RECOVER_NEXT,
                             (uint8_t)rwy_ss_reason(r), s->rec_seq + 1);
    return rwy_record_write(s, r);
}

int rwy_virt_create(rwy_store_t *s, uint64_t parent, const char *name, uint32_t nlen, uint8_t type, int *done) {
    char slot[RWY_NAME_MAX + 1];
    *done = 0;
    for (unsigned k = 0; k < RWY_VK_COUNT; k++) {
        if (!same(VFILES[k].name, name, nlen) || rwy_bootcfg_role(s, parent, slot) != VFILES[k].role) continue;
        if (k != RWY_VK_RECOVER_NEXT) return RWY_OK;   /* the policy hook refuses the name */
        *done = 1;
        if (type != RWY_TYPE_FILE) return RWY_ERR_INVAL;
        return vcreate(s);
    }
    return RWY_OK;
}
