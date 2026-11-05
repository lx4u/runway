/*
 * rwy_bls_view.c — A snapshot of the BLS projection of a store.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-only code.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "rwy_bls_view.h"
#include "rwy_internal.h"

#define CONF_MAX 4096     /* the size of the buffer that the projection renders a .conf into */
#define SEEN_MAX 32       /* items per children or roots list; the next ones are dropped */

void rwy_bls_view_init(rwy_bls_view_t *v) { memset(v, 0, sizeof(*v)); }

static void drop(rwy_bls_view_t *v) {
    size_t i;
    for (i = 0; i < v->nslots; i++) { free(v->slots[i].rendered); free(v->slots[i].prefix); }
    free(v->slots);
    free(v->blob_names);
    free(v->blob_off);
    memset(v, 0, offsetof(rwy_bls_view_t, builds));
}

void rwy_bls_view_free(rwy_bls_view_t *v) { drop(v); }

static int key_matches(const rwy_bls_view_t *v, const rwy_store_t *s) {
    return v->valid && v->generation == s->sb.generation && v->rec_seq == s->rec_seq &&
           v->mount_id == s->mount_id && !memcmp(v->uuid, s->sb.uuid, sizeof(v->uuid));
}

typedef int (*walk_fn)(void *ctx, const char *name);

/* Call fn for every name of a directory, in directory order. A missing directory is an empty one. */
static int walk_dir(rwy_store_t *s, const char *path, walk_fn fn, void *ctx) {
    char name[RWY_NAME_MAX + 1];
    uint64_t dir, oid, off = 0;
    uint32_t nlen;
    uint8_t type;
    int rc;

    if (rwy_lookup(s, path, &dir) != RWY_OK) return RWY_OK;
    while ((rc = rwy_dir_next(s, dir, &off, name, &nlen, &oid, &type)) == RWY_OK) {
        rc = fn(ctx, name);
        if (rc) return rc;
    }
    return rc == RWY_ERR_NOENT ? RWY_OK : rc;
}

typedef struct {
    rwy_bls_view_t *v;
    rwy_store_t *s;
    rwy_bls_slot_info_t info;
    size_t cap;
} build_t;

/* Fill the slot from its files. A slot that cannot be read is left out, as the scans left it out. */
static int fill_slot(build_t *b, const char *name) {
    rwy_bls_view_slot_t *sl = &b->v->slots[b->v->nslots];
    rwy_bls_slot_info_t *info = &b->info;
    char rendered[RWY_ENTRY_MAX + 32], pfx[256], conf[CONF_MAX];

    memset(sl, 0, sizeof(*sl));
    snprintf(sl->path, sizeof(sl->path), "EFI/RUNWAY/%.32s", name);
    if (rwy_bls_read_slot_info(b->s, sl->path, info) != RWY_OK) return RWY_OK;
    sl->is_type2 = (uint8_t)info->is_type2;
    sl->has_initrd = (uint8_t)info->has_initrd;
    sl->has_dtb = (uint8_t)info->has_dtb;
    sl->conf_len = -1;
    if (rwy_bls_name_render(info->entry_name, strlen(info->entry_name), info->state, info->tries,
                            rendered, sizeof(rendered)) >= 0) {
        sl->rendered = strdup(rendered);
        if (!sl->rendered) return RWY_ERR_NOSPC;
    }
    if (!sl->is_type2) {
        sl->conf_len = rwy_bls_render_type1(info, conf, sizeof(conf));
        rwy_bls_derive_prefix(info, pfx, sizeof(pfx));
        sl->prefix = strdup(pfx);
        if (!sl->prefix) { free(sl->rendered); return RWY_ERR_NOSPC; }
    }
    b->v->nslots++;
    return RWY_OK;
}

static int add_slot(void *ctx, const char *name) {
    build_t *b = ctx;
    if (strncmp(name, "CONFIG", 6) != 0) return RWY_OK;
    if (b->v->nslots == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 16;
        rwy_bls_view_slot_t *p = realloc(b->v->slots, cap * sizeof(*p));
        if (!p) return RWY_ERR_NOSPC;
        b->v->slots = p;
        b->cap = cap;
    }
    return fill_slot(b, name);
}

typedef struct {
    rwy_bls_view_t *v;
    size_t used, cap, off_cap;
} blobs_t;

static int add_blob(void *ctx, const char *name) {
    blobs_t *b = ctx;
    rwy_bls_view_t *v = b->v;
    size_t len = strlen(name) + 1;

    if (v->nblobs == b->off_cap) {
        size_t cap = b->off_cap ? b->off_cap * 2 : 64;
        uint32_t *p = realloc(v->blob_off, cap * sizeof(*p));
        if (!p) return RWY_ERR_NOSPC;
        v->blob_off = p;
        b->off_cap = cap;
    }
    if (b->used + len > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        char *p;
        while (cap < b->used + len) cap *= 2;
        p = realloc(v->blob_names, cap);
        if (!p) return RWY_ERR_NOSPC;
        v->blob_names = p;
        b->cap = cap;
    }
    v->blob_off[v->nblobs++] = (uint32_t)b->used;
    memcpy(v->blob_names + b->used, name, len);
    b->used += len;
    return RWY_OK;
}

int rwy_bls_view_sync(rwy_bls_view_t *v, rwy_store_t *s) {
    build_t b = { .v = v, .s = s };
    blobs_t bl = { .v = v };
    int rc;

    if (s->in_txn || s->bc_novirt) return RWY_ERR_BUSY;
    if (key_matches(v, s)) return RWY_OK;
    drop(v);
    rc = walk_dir(s, "EFI/RUNWAY", add_slot, &b);
    if (!rc) rc = walk_dir(s, "EFI/RUNWAY/BLOBS", add_blob, &bl);
    if (rc) { drop(v); return rc; }
    memcpy(v->uuid, s->sb.uuid, sizeof(v->uuid));
    v->generation = s->sb.generation;
    v->rec_seq = s->rec_seq;
    v->mount_id = s->mount_id;
    v->valid = 1;
    v->builds++;
    return RWY_OK;
}

static const char *blob_name(const rwy_bls_view_t *v, size_t i) { return v->blob_names + v->blob_off[i]; }

int rwy_bls_view_payload(const rwy_bls_view_t *v, const char *path, char *out, size_t out_sz) {
    size_t i;
    for (i = 0; i < v->nslots; i++) {
        const rwy_bls_view_slot_t *sl = &v->slots[i];
        size_t plen;
        const char *rest;
        if (sl->is_type2) continue;
        plen = strlen(sl->prefix);
        if (strncmp(path, sl->prefix, plen) || path[plen] != '/') continue;
        rest = path + plen + 1;
        if (!strcmp(rest, "linux")) { snprintf(out, out_sz, "%.40s/KERNEL", sl->path); return RWY_OK; }
        if (sl->has_initrd && !strcmp(rest, "initrd")) { snprintf(out, out_sz, "%.40s/INITRD", sl->path); return RWY_OK; }
        if (sl->has_dtb && !strcmp(rest, "dtb")) { snprintf(out, out_sz, "%.40s/DTB", sl->path); return RWY_OK; }
    }
    return RWY_ERR_NOENT;
}

int rwy_bls_view_resolve(const rwy_bls_view_t *v, const char *path, char *out, size_t out_sz) {
    char enc[256];
    size_t i;
    if (rwy_bls_encode_blob_name(path, enc, sizeof(enc)) > 0) {
        for (i = 0; i < v->nblobs; i++)
            if (!strcmp(blob_name(v, i), enc)) {
                snprintf(out, out_sz, "EFI/RUNWAY/BLOBS/%s", enc);
                return RWY_OK;
            }
    }
    return rwy_bls_view_payload(v, path, out, out_sz);
}

/* Whether blob name `b` is the encoded `enc` followed by an encoded slash. */
static int blob_under(const char *b, const char *enc, size_t enclen) {
    return !strncmp(b, enc, enclen) && b[enclen] == '%' && b[enclen + 1] == '2' &&
           (b[enclen + 2] == 'F' || b[enclen + 2] == 'f');
}

static int is_blob_prefix(const rwy_bls_view_t *v, const char *path) {
    char enc[256];
    size_t i, enclen;
    if (rwy_bls_encode_blob_name(path, enc, sizeof(enc)) <= 0) return 0;
    enclen = strlen(enc);
    for (i = 0; i < v->nblobs; i++)
        if (blob_under(blob_name(v, i), enc, enclen)) return 1;
    return 0;
}

/* The parent directory of a prefix when `path` is it: the name after its last slash, else NULL. */
static const char *prefix_child(const rwy_bls_view_slot_t *sl, const char *path) {
    const char *last = strrchr(sl->prefix, '/');
    size_t plen;
    if (!last || last == sl->prefix) return NULL;
    plen = (size_t)(last - sl->prefix);
    return strlen(path) == plen && !strncmp(path, sl->prefix, plen) ? last + 1 : NULL;
}

static int is_slot_dir(const rwy_bls_view_t *v, const char *path) {
    size_t i;
    for (i = 0; i < v->nslots; i++) {
        const rwy_bls_view_slot_t *sl = &v->slots[i];
        if (sl->is_type2) continue;
        if (!strcmp(path, sl->prefix) || prefix_child(sl, path)) return 1;
    }
    return 0;
}

int rwy_bls_view_is_dir(const rwy_bls_view_t *v, const char *path) {
    return is_blob_prefix(v, path) || is_slot_dir(v, path);
}

int rwy_bls_view_entry(const rwy_bls_view_t *v, int type2, const char *file,
                       char *slot_out, size_t slot_sz, int *conf_len) {
    size_t i;
    for (i = 0; i < v->nslots; i++) {
        const rwy_bls_view_slot_t *sl = &v->slots[i];
        if (!!sl->is_type2 != !!type2 || !sl->rendered || strcmp(sl->rendered, file)) continue;
        if (slot_out) snprintf(slot_out, slot_sz, "%s", sl->path);
        if (conf_len) *conf_len = sl->conf_len;
        return RWY_OK;
    }
    return RWY_ERR_NOENT;
}

void rwy_bls_view_entries(const rwy_bls_view_t *v, int type2, rwy_bls_view_name_fn fn, void *ctx) {
    size_t i;
    for (i = 0; i < v->nslots; i++)
        if (!!v->slots[i].is_type2 == !!type2 && v->slots[i].rendered) fn(ctx, v->slots[i].rendered);
}

/* Call fn once per distinct item, at most SEEN_MAX of them. */
typedef struct { char seen[SEEN_MAX][64]; size_t n; rwy_bls_view_name_fn fn; void *ctx; } uniq_t;

static void uniq_add(uniq_t *u, const char *item) {
    size_t j;
    for (j = 0; j < u->n; j++)
        if (!strcmp(u->seen[j], item)) return;
    if (u->n >= SEEN_MAX) return;
    snprintf(u->seen[u->n++], sizeof(u->seen[0]), "%s", item);
    u->fn(u->ctx, item);
}

/* The text up to `end` (or all of it when NULL), cut to the 63 characters of an item. */
static void item_of(const char *text, const char *end, char *item, size_t item_sz) {
    size_t n = end ? (size_t)(end - text) : strlen(text);
    if (n >= item_sz) n = item_sz - 1;
    memcpy(item, text, n);
    item[n] = 0;
}

void rwy_bls_view_blob_children(const rwy_bls_view_t *v, const char *path, rwy_bls_view_name_fn fn, void *ctx) {
    char enc[256];
    uniq_t u = { .n = 0, .fn = fn, .ctx = ctx };
    size_t i, enclen;
    if (rwy_bls_encode_blob_name(path, enc, sizeof(enc)) <= 0) return;
    enclen = strlen(enc);
    for (i = 0; i < v->nblobs; i++) {
        const char *b = blob_name(v, i), *sub, *slash;
        char item[64];
        if (!blob_under(b, enc, enclen)) continue;
        sub = b + enclen + 3;
        slash = strstr(sub, "%2F");
        if (!slash) slash = strstr(sub, "%2f");
        item_of(sub, slash, item, sizeof(item));
        uniq_add(&u, item);
    }
}

void rwy_bls_view_slot_children(const rwy_bls_view_t *v, const char *path, rwy_bls_view_name_fn fn, void *ctx) {
    size_t i;
    for (i = 0; i < v->nslots; i++) {
        const rwy_bls_view_slot_t *sl = &v->slots[i];
        const char *child;
        if (sl->is_type2) continue;
        if (!strcmp(path, sl->prefix)) {
            fn(ctx, "linux");
            if (sl->has_initrd) fn(ctx, "initrd");
            if (sl->has_dtb) fn(ctx, "dtb");
        } else if ((child = prefix_child(sl, path))) {
            fn(ctx, child);
        }
    }
}

void rwy_bls_view_roots(const rwy_bls_view_t *v, rwy_bls_view_name_fn fn, void *ctx) {
    uniq_t u = { .n = 0, .fn = fn, .ctx = ctx };
    size_t i;
    for (i = 0; i < v->nslots; i++) {
        const rwy_bls_view_slot_t *sl = &v->slots[i];
        const char *first = sl->prefix + 1, *slash = strchr(first, '/');
        char token[64];
        if (sl->is_type2) continue;
        if (slash) item_of(first, slash, token, sizeof(token));
        else snprintf(token, sizeof(token), "%.60s", first);
        uniq_add(&u, token);
    }
}
