/*
 * rwy_dir.c — Directory records, path resolution and readdir.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

#define DIRENT_HDR 16u

static uint32_t dirent_reclen(uint32_t namelen) {
    uint32_t r = DIRENT_HDR + namelen;
    return (r + 7u) & ~7u;
}

static void dirent_write(uint8_t *p, uint64_t oid, uint8_t type,
                         const char *name, uint32_t namelen) {
    uint32_t rl = dirent_reclen(namelen), i;
    p[0]=(uint8_t)oid; p[1]=(uint8_t)(oid>>8); p[2]=(uint8_t)(oid>>16); p[3]=(uint8_t)(oid>>24);
    p[4]=(uint8_t)(oid>>32); p[5]=(uint8_t)(oid>>40); p[6]=(uint8_t)(oid>>48); p[7]=(uint8_t)(oid>>56);
    p[8]=(uint8_t)rl; p[9]=(uint8_t)(rl>>8); p[10]=(uint8_t)(rl>>16); p[11]=(uint8_t)(rl>>24);
    p[12]=type; p[13]=(uint8_t)namelen; p[14]=0; p[15]=0;
    for (i = 0; i < namelen; i++) p[DIRENT_HDR + i] = (uint8_t)name[i];
    for (i = DIRENT_HDR + namelen; i < rl; i++) p[i] = 0;
}

/* Read and check the header of the dirent at `off`. */
static int dirent_read_hdr(const rwy_store_t *s, const rwy_inode_t *dir, uint64_t off,
                           uint64_t *oid, uint32_t *rl, uint8_t *type, uint8_t *nl) {
    uint8_t hdr[DIRENT_HDR];
    size_t got = 0;
    uint32_t i;
    int rc;

    if (off + DIRENT_HDR > dir->size) return RWY_ERR_CORRUPT;
    rc = rwy_inode_pread(s, dir, off, hdr, DIRENT_HDR, &got);
    if (rc) return rc;
    if (got != DIRENT_HDR) return RWY_ERR_CORRUPT;
    *oid = 0;
    for (i = 0; i < 8; i++) *oid |= ((uint64_t)hdr[i]) << (8 * i);
    *rl = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
          ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    *type = hdr[12];
    *nl = hdr[13];
    if (*rl < DIRENT_HDR || (*rl & 7) || off + *rl > dir->size || *rl == 0)
        return RWY_ERR_CORRUPT;
    return RWY_OK;
}

/* Resolve a directory path to its oid and inode. */
static int open_dir(rwy_store_t *s, const char *path, uint64_t *o, const rwy_inode_t **d) {
    int rc = rwy_resolve_ext(s, path, s->in_txn, 1, o);

    if (rc) return rc;
    *d = rwy_inode_of(s, *o, s->in_txn);
    if (!*d) return RWY_ERR_NOENT;
    if ((*d)->type != RWY_TYPE_DIR) return RWY_ERR_NOTDIR;
    return RWY_OK;
}

/* Search for a dirent by name in a directory inode. Works on committed or working inode.
 * Returns RWY_OK if found (*out_oid, *out_type, *out_off, *out_rl populated if non-NULL).
 * Returns RWY_ERR_NOENT if not found. */
static int dir_find_entry(const rwy_store_t *s, const rwy_inode_t *dir,
                          const char *name, uint32_t namelen,
                          uint64_t *out_oid, uint8_t *out_type,
                          uint64_t *out_off, uint32_t *out_rl) {
    uint64_t off = 0;
    if (dir->type != RWY_TYPE_DIR) return RWY_ERR_NOTDIR;
    while (off < dir->size) {
        size_t got = 0;
        uint64_t oid;
        uint32_t rl;
        uint8_t type, nl;
        int rc = dirent_read_hdr(s, dir, off, &oid, &rl, &type, &nl);

        if (rc) return rc;

        if (nl == namelen) {
            char nbuf[RWY_NAME_MAX + 1];
            rc = rwy_inode_pread(s, dir, off + DIRENT_HDR, nbuf, nl, &got);
            if (rc) return rc;
            if (got != nl) return RWY_ERR_CORRUPT;
            if (rwy_strncmp(nbuf, name, nl) == 0) {
                if (out_oid) *out_oid = oid;
                if (out_type) *out_type = type;
                if (out_off) *out_off = off;
                if (out_rl) *out_rl = rl;
                return RWY_OK;
            }
        }
        off += rl;
    }
    return RWY_ERR_NOENT;
}

int rwy_dir_lookup(const rwy_store_t *s, uint64_t dir, const char *name, uint32_t nlen,
                    uint64_t *out_oid, uint8_t *out_type) {
    const rwy_inode_t *d = rwy_inode_of(s, dir, s->in_txn);
    if (!d) return RWY_ERR_NOENT;
    return dir_find_entry(s, d, name, nlen, out_oid, out_type, 0, 0);
}

int rwy_dir_next(const rwy_store_t *s, uint64_t dir, uint64_t *off, char *name, uint32_t *nlen,
                 uint64_t *oid, uint8_t *type) {
    const rwy_inode_t *d = rwy_inode_of(s, dir, s->in_txn);
    uint32_t rl;
    uint8_t nl;
    size_t got = 0;
    int rc;

    if (!d || d->type != RWY_TYPE_DIR) return RWY_ERR_NOENT;
    if (*off >= d->size) return RWY_ERR_NOENT;
    rc = dirent_read_hdr(s, d, *off, oid, &rl, type, &nl);
    if (rc) return rc;
    rc = rwy_inode_pread(s, d, *off + DIRENT_HDR, name, nl, &got);
    if (rc) return rc;
    if (got != nl) return RWY_ERR_CORRUPT;
    name[nl] = 0;
    *nlen = nl;
    *off += rl;
    return RWY_OK;
}

/* True when a path component holds none of the characters a name cannot have. */
static int component_chars_ok(const char *p, size_t len) {
    for (size_t k = 0; k < len; k++) {
        unsigned char ch = (unsigned char)p[k];
        if (ch == '/' || ch == '\\' || ch < 0x20 || ch == 0x7f)
            return 0;
    }
    return 1;
}

/* The last component must be a plain name, not "." or "..". */
static int basename_ok(const char *base, uint32_t len) {
    if (len == 0 || len > RWY_NAME_MAX)
        return RWY_ERR_NAMETOOLONG;
    if (len == 1 && base[0] == '.')
        return RWY_ERR_INVAL;
    if (len == 2 && base[0] == '.' && base[1] == '.')
        return RWY_ERR_INVAL;
    if (!component_chars_ok(base, len))
        return RWY_ERR_INVAL;
    return RWY_OK;
}

/* Resolve the directory that holds the last component, which starts after last_slash. */
static int resolve_dir_of(rwy_store_t *s, const char *path, const char *last_slash, int working,
                          uint64_t *out_parent) {
    char parent_path[RWY_MAX_PATH + 1];
    size_t plen = (size_t)(last_slash - path);
    const rwy_inode_t *pin;
    uint64_t poid;
    int rc;

    if (plen == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        for (size_t i = 0; i < plen; i++) parent_path[i] = path[i];
        parent_path[plen] = '\0';
    }
    rc = rwy_resolve_ext(s, parent_path, working, 1, &poid);
    if (rc) return rc;
    pin = rwy_inode_of(s, poid, working);
    if (!pin || pin->type != RWY_TYPE_DIR) return RWY_ERR_NOTDIR;
    *out_parent = poid;
    return RWY_OK;
}

/* Resolve parent dir OID + basename for a path. */
int rwy_resolve_parent(rwy_store_t *s, const char *path, int working,
                          uint64_t *out_parent, const char **out_base,
                          uint32_t *out_baselen) {
    size_t len;
    const char *last_slash = 0;
    const char *p;

    if (!s || !path || !out_parent || !out_base || !out_baselen)
        return RWY_ERR_INVAL;

    len = rwy_strlen(path);
    if (len == 0 || len > RWY_MAX_PATH) return RWY_ERR_INVAL;

    /* Trim trailing slashes */
    while (len > 0 && path[len - 1] == '/') len--;
    if (len == 0) return RWY_ERR_INVAL; /* was "/" or empty */

    for (p = path + len - 1; p >= path; p--) {
        if (*p == '/') { last_slash = p; break; }
    }

    if (!last_slash) {
        *out_parent = 1;
        *out_base = path;
        *out_baselen = (uint32_t)len;
    } else {
        int rc = resolve_dir_of(s, path, last_slash, working, out_parent);
        if (rc) return rc;
        *out_base = last_slash + 1;
        *out_baselen = (uint32_t)((path + len) - (last_slash + 1));
    }
    return basename_ok(*out_base, *out_baselen);
}

/* Replace the symlink component of `cur_path` by `target`: the rest of the path (`tail`) follows it.
 * An absolute target restarts from the root. */
static int link_splice(char *cur_path, const char *tail, const char *target, uint64_t *cur, int *depth,
                       uint64_t *ancestors) {
    char new_path[RWY_MAX_PATH + 1];
    size_t tlen, rem_len, new_len;
    const char *tstart = target;
    if (*tstart == '/') {
        *cur = 1;
        *depth = 0;
        ancestors[0] = 1;
        while (*tstart == '/') tstart++;
    }
    rem_len = rwy_strlen(tail);
    tlen = rwy_strlen(tstart);
    new_len = tlen + (rem_len ? (1 + rem_len) : 0);
    if (new_len > RWY_MAX_PATH) return RWY_ERR_NAMETOOLONG;
    for (size_t i = 0; i < tlen; i++) new_path[i] = tstart[i];
    if (rem_len) {
        new_path[tlen] = '/';
        for (size_t i = 0; i <= rem_len; i++)
            new_path[tlen + 1 + i] = tail[i];
    } else {
        new_path[tlen] = '\0';
    }
    for (size_t i = 0; i <= new_len; i++) cur_path[i] = new_path[i];
    return RWY_OK;
}

typedef struct {
    char path[RWY_MAX_PATH + 1];   /* the path being walked; a symlink splices its target in here */
    const char *p;                  /* next component, inside path */
    uint64_t cur;
    uint64_t ancestors[256];
    int depth;
    uint32_t nloops;
} walk_t;

#define WALK_MORE 1

enum { ENTRY_NODE, ENTRY_LINK };

/* Find the component in the current directory; a symlink also returns its target. */
static int walk_lookup(rwy_store_t *s, const walk_t *w, size_t len, int working, uint64_t *child,
                       const rwy_inode_t **c, char *target, int *kind) {
    const rwy_inode_t *d = rwy_inode_of(s, w->cur, working);
    size_t got = 0;
    int rc;

    if (!d || d->type != RWY_TYPE_DIR) return RWY_ERR_NOTDIR;
    rc = dir_find_entry(s, d, w->p, (uint32_t)len, child, 0, 0, 0);
    target[0] = 0;
    *kind = ENTRY_NODE;
    if (rc == RWY_ERR_NOENT) {
        if (rwy_virt_lookup(s, w->cur, w->p, (uint32_t)len, child) != RWY_OK) return RWY_ERR_NOENT;
        /* a virtual file has no inode: *c stays 0 */
        if (rwy_virt_readlink(s, *child, target, RWY_MAX_PATH + 1) == RWY_OK) *kind = ENTRY_LINK;
        return RWY_OK;
    }
    if (rc != RWY_OK) return rc;
    *c = rwy_inode_of(s, *child, working);
    if (!*c) return RWY_ERR_CORRUPT;
    if ((*c)->type != RWY_TYPE_SYMLINK) return RWY_OK;
    *kind = ENTRY_LINK;
    if ((*c)->size > RWY_MAX_PATH) return RWY_ERR_NAMETOOLONG;
    rc = rwy_inode_pread(s, *c, 0, target, (size_t)(*c)->size, &got);
    if (rc) return rc;
    target[got] = '\0';
    return RWY_OK;
}

/* Follow a symlink (or stop on it when it is the last component and is not followed). */
static int walk_link(walk_t *w, const char *tail, const char *target, uint64_t child, int is_terminal,
                     int follow, uint64_t *out) {
    int rc;

    if (!follow) {
        *out = child;
        return RWY_OK;
    }
    if (++w->nloops > RWY_SYMLOOP_MAX) return RWY_ERR_LOOP;
    rc = link_splice(w->path, tail, target, &w->cur, &w->depth, w->ancestors);
    if (rc) return rc;
    w->p = w->path;
    while (*w->p == '/') w->p++;
    if (!*w->p && is_terminal) {
        *out = w->cur;
        return RWY_OK;
    }
    return WALK_MORE;
}

/* A found node that is not a symlink (c is 0 for a virtual file): enter a directory, or end on a file. */
static int walk_node(walk_t *w, const rwy_inode_t *c, uint64_t child, const char *tail, int is_terminal,
                     int has_trailing_slash, uint64_t *out) {
    if (c && c->type == RWY_TYPE_DIR) {
        if (is_terminal) {
            *out = child;
            return RWY_OK;
        }
        if (w->depth + 1 >= 256) return RWY_ERR_NAMETOOLONG;
        w->depth++;
        w->ancestors[w->depth] = child;
        w->cur = child;
        w->p = tail;
        return WALK_MORE;
    }
    if (!is_terminal || has_trailing_slash) return RWY_ERR_NOTDIR;
    *out = child;
    return RWY_OK;
}

/* Step over "." and "..": returns 1 when the component was one of them. */
static int walk_special(walk_t *w, const char *p, size_t len, const char *tail) {
    if (len == 1 && p[0] == '.') {
        w->p = tail;
        return 1;
    }
    if (len == 2 && p[0] == '.' && p[1] == '.') {
        if (w->depth > 0) w->cur = w->ancestors[--w->depth];
        else w->cur = 1;
        w->p = tail;
        return 1;
    }
    return 0;
}

/* Walk one path component: WALK_MORE to go on, RWY_OK when *out is set, else an error. */
static int walk_step(rwy_store_t *s, walk_t *w, int working, int follow_terminal, uint64_t *out) {
    const char *p = w->p, *e = p, *tail;
    size_t len;
    int is_terminal, has_trailing_slash, kind, rc;
    uint64_t child = 0;
    const rwy_inode_t *c = 0;
    char target[RWY_MAX_PATH + 1];

    while (*e && *e != '/') e++;
    len = (size_t)(e - p);
    if (len == 0) return RWY_ERR_INVAL;
    if (len > RWY_NAME_MAX) return RWY_ERR_NAMETOOLONG;
    tail = e;
    while (*tail == '/') tail++;
    is_terminal = (*tail == '\0');
    has_trailing_slash = (*e == '/' && is_terminal);

    if (walk_special(w, p, len, tail)) return WALK_MORE;
    if (!component_chars_ok(p, len)) return RWY_ERR_INVAL;

    rc = walk_lookup(s, w, len, working, &child, &c, target, &kind);
    if (rc) return rc;
    if (kind == ENTRY_LINK)
        return walk_link(w, tail, target, child, is_terminal,
                         !is_terminal || follow_terminal || has_trailing_slash, out);
    return walk_node(w, c, child, tail, is_terminal, has_trailing_slash, out);
}

int rwy_resolve_ext(rwy_store_t *s, const char *path, int working,
                       int follow_terminal, uint64_t *out) {
    walk_t w;
    size_t plen;
    int rc;

    if (!s || !path || !out) return RWY_ERR_INVAL;
    plen = rwy_strlen(path);
    if (plen > RWY_MAX_PATH) return RWY_ERR_NAMETOOLONG;
    for (size_t i = 0; i <= plen; i++) w.path[i] = path[i];
    w.cur = 1;
    w.ancestors[0] = 1;
    w.depth = 0;
    w.nloops = 0;
    w.p = w.path;
    while (*w.p == '/') w.p++;
    if (!*w.p) { *out = 1; return RWY_OK; }

    while (*w.p) {
        rc = walk_step(s, &w, working, follow_terminal, out);
        if (rc != WALK_MORE) return rc;
    }
    *out = w.cur;
    return RWY_OK;
}

int rwy_resolve(rwy_store_t *s, const char *path, int working, uint64_t *out) {
    return rwy_resolve_ext(s, path, working, 0, out);
}

int rwy_lookup_follow(rwy_store_t *s, const char *path, int follow_terminal, uint64_t *out_oid) {
    uint64_t o;
    int rc;
    if (!s || !path || !out_oid) return RWY_ERR_INVAL;
    rc = rwy_resolve_ext(s, path, s->in_txn, follow_terminal, &o);
    if (rc) return rc;
    *out_oid = o;
    return RWY_OK;
}

int rwy_lookup(rwy_store_t *s, const char *path, uint64_t *out_oid) {
    return rwy_lookup_follow(s, path, 0, out_oid);
}

/* Insert a dirent into parent (working). */
int rwy_parent_insert(rwy_store_t *s, uint64_t parent, uint64_t child,
                         uint8_t type, const char *base, uint32_t baselen) {
    rwy_inode_t *p = rwy_winode_of(s, parent);
    uint8_t rec[RWY_NAME_MAX + 32];
    uint32_t rl;
    int rc;
    rwy_bootcfg_invalidate(s);
    if (!p || p->type != RWY_TYPE_DIR) return RWY_ERR_NOTDIR;
    rc = dir_find_entry(s, p, base, baselen, 0, 0, 0, 0);
    if (rc == RWY_OK) return RWY_ERR_EXIST;
    if (rc != RWY_ERR_NOENT) return rc;
    rl = dirent_reclen(baselen);
    dirent_write(rec, child, type, base, baselen);
    return rwy_cow_write(s, p, p->size, rec, rl);
}

int rwy_parent_remove(rwy_store_t *s, uint64_t parent,
                         const char *base, uint32_t baselen,
                         uint64_t *out_child) {
    rwy_inode_t *p = rwy_winode_of(s, parent);
    uint64_t child = 0, off = 0;
    uint32_t rl = 0;
    int rc;
    rwy_bootcfg_invalidate(s);
    if (!p || p->type != RWY_TYPE_DIR) return RWY_ERR_NOTDIR;
    rc = dir_find_entry(s, p, base, baselen, &child, 0, &off, &rl);
    if (rc) return rc;

    /* Shift subsequent entries forward */
    {
        uint64_t cur = off;
        uint64_t src = off + rl;
        uint8_t move_buf[RWY_BLOCK_SIZE];
        while (src < p->size) {
            size_t chunk = (size_t)(p->size - src);
            size_t max_chunk = RWY_BLOCK_SIZE - (size_t)(cur % RWY_BLOCK_SIZE);
            size_t got = 0;
            if (chunk > max_chunk) chunk = max_chunk;
            rc = rwy_inode_pread(s, p, src, move_buf, chunk, &got);
            if (rc) return rc;
            if (got != chunk) return RWY_ERR_CORRUPT;
            rc = rwy_cow_write(s, p, cur, move_buf, chunk);
            if (rc) return rc;
            cur += chunk;
            src += chunk;
        }
    }
    rc = rwy_inode_truncate(s, p, p->size - rl);
    if (rc) return rc;
    if (out_child) *out_child = child;
    return RWY_OK;
}

/* Append the virtual files of a directory to the names read so far. */
static void readdir_virtual(rwy_store_t *s, uint64_t o, size_t cap, char *out_names, size_t out_sz,
                            uint8_t *out_types, size_t *used, size_t *n) {
    for (size_t v = 0; *n < cap; v++) {
        char name[RWY_NAME_MAX + 1];
        uint8_t type = 0;
        size_t nl;
        if (rwy_virt_nth(s, o, v, name, sizeof(name), &type) != RWY_OK) break;
        nl = rwy_strlen(name);
        if (*used + nl + 1 > out_sz) break;
        for (size_t i = 0; i <= nl; i++) out_names[*used + i] = name[i];
        *used += nl + 1;
        if (out_types) out_types[*n] = type;
        (*n)++;
    }
}

int rwy_readdir(rwy_store_t *s, const char *path,
                 char *out_names, size_t out_sz, size_t *inout_count,
                 uint8_t *out_types) {
    uint64_t o;
    const rwy_inode_t *d;
    uint64_t off = 0;
    size_t cap, used = 0, n = 0;
    int rc;

    if (!s || !path || !out_names || !inout_count) return RWY_ERR_INVAL;
    cap = *inout_count;
    rc = open_dir(s, path, &o, &d);
    if (rc) return rc;

    while (off < d->size && n < cap) {
        size_t got = 0;
        uint64_t oid;
        uint32_t rl;
        uint8_t type, nl;

        rc = dirent_read_hdr(s, d, off, &oid, &rl, &type, &nl);
        if (rc) return rc;
        if (used + nl + 1 > out_sz) break;

        rc = rwy_inode_pread(s, d, off + DIRENT_HDR, out_names + used, nl, &got);
        if (rc) return rc;
        if (got != nl) return RWY_ERR_CORRUPT;
        out_names[used + nl] = 0;

        used += nl + 1;
        if (out_types) out_types[n] = type;
        n++;
        off += rl;
    }
    /* Virtual files come after the real entries. */
    if (off >= d->size)
        readdir_virtual(s, o, cap, out_names, out_sz, out_types, &used, &n);
    *inout_count = n;
    return RWY_OK;
}

int rwy_readdir_nth(rwy_store_t *s, const char *path, size_t index,
                     char *out_name, size_t out_name_sz, uint8_t *out_type) {
    uint64_t o;
    const rwy_inode_t *d;
    uint64_t off = 0;
    size_t cur = 0;
    int rc;

    if (!s || !path || !out_name || out_name_sz == 0) return RWY_ERR_INVAL;
    rc = open_dir(s, path, &o, &d);
    if (rc) return rc;

    while (off < d->size) {
        size_t got = 0;
        uint64_t oid;
        uint32_t rl;
        uint8_t type, nl;

        rc = dirent_read_hdr(s, d, off, &oid, &rl, &type, &nl);
        if (rc) return rc;

        if (cur == index) {
            if ((size_t)nl + 1 > out_name_sz) return RWY_ERR_NAMETOOLONG;
            rc = rwy_inode_pread(s, d, off + DIRENT_HDR, out_name, nl, &got);
            if (rc) return rc;
            if (got != nl) return RWY_ERR_CORRUPT;
            out_name[nl] = 0;
            if (out_type) *out_type = type;
            return RWY_OK;
        }
        cur++;
        off += rl;
    }
    return rwy_virt_nth(s, o, index - cur, out_name, out_name_sz, out_type);
}
