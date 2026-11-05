/*
 * Fragmentation metric (reported always, never auto-triggered):
 *   frag: N multi-extent objects, live=X blocks, free=Y blocks,
 *         largest free run=Z blocks.
 * SPDX-License-Identifier: Apache-2.0
 * "Needs defrag" is a fact from these numbers, not a guess; compaction
 * only runs under explicit --defrag (offline, stop-the-world, same as
 * GC — no online or allocation-time balancing).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "rwy_format.h"

#define DIR_STACK_MAX 256

typedef struct {
    char path[DIR_STACK_MAX][RWY_MAX_PATH];
    int depth;                 /* index of the top entry, -1 when empty */
} dir_stack_t;

static int valid_name(const char *n, size_t len) {
    size_t i;
    if (len == 0 || len > RWY_NAME_MAX) return 0;
    if (len == 1 && n[0] == '.') return 0;
    if (len == 2 && n[0] == '.' && n[1] == '.') return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)n[i];
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

static int cmp_str_ptrs(const void *a, const void *b) {
    const char * const *sa = a;
    const char * const *sb = b;
    return strcmp(*sa, *sb);
}

static void check_dir_names(check_ctx_t *c, const char *names, size_t cnt, const char *path) {
    const char **nptrs = malloc(cnt * sizeof(const char *));
    size_t j, off = 0;

    if (!nptrs) { fprintf(stderr, "oom\n"); exit(2); }
    for (j = 0; j < cnt; j++) {
        size_t len = strlen(names + off);
        if (!valid_name(names + off, len))
            ERR(c, "bad name '%s' in '%s'", names + off, path);
        nptrs[j] = names + off;
        off += len + 1;
    }
    qsort((void *)nptrs, cnt, sizeof(const char *), cmp_str_ptrs);
    for (j = 1; j < cnt; j++) {
        if (strcmp(nptrs[j], nptrs[j - 1]) == 0)
            ERR(c, "duplicate name '%s' in '%s'", nptrs[j], path);
    }
    free((void *)nptrs);
}

/* Look up one directory entry, count its reference and queue it when it is a directory. */
static void check_dir_entry(check_ctx_t *c, rwy_store_t *s, const char *path, const char *name, uint8_t type,
                                            scan_t *sc, dir_stack_t *st) {
    char child[RWY_MAX_PATH + 8];
    uint64_t co;
    rwy_attr_t ca;
    uint8_t want;

    if (path[0]) snprintf(child, sizeof(child), "%s/%s", path, name);
    else snprintf(child, sizeof(child), "%s", name);
    if (rwy_lookup(s, child, &co) != RWY_OK) {
        ERR(c, "dangling entry '%s'", child);
        return;
    }
    if (rwy_getattr(s, co, &ca) != RWY_OK) {
        ERR(c, "cannot stat '%s'", child);
        return;
    }
    if (co >= 1 && co <= RWY_MAX_INODES)
        sc->dirent_refs[co]++;
    want = ca.type == RWY_TYPE_DIR ? RWY_TYPE_DIR :
           ca.type == RWY_TYPE_SYMLINK ? RWY_TYPE_SYMLINK : RWY_TYPE_FILE;
    if (type != want)
        ERR(c, "type mismatch for '%s'", child);
    if (ca.type != RWY_TYPE_DIR)
        return;
    if (st->depth + 1 >= DIR_STACK_MAX) ERR(c, "dir too deep at '%s'", child);
    else strcpy(st->path[++st->depth], child);
}

/* Check one directory: its names, then each entry. */
static void check_dir(check_ctx_t *c, rwy_store_t *s, const char *path, const rwy_attr_t *da, scan_t *sc,
                                      dir_stack_t *st) {
    size_t names_sz = (size_t)da->size + 4096;
    size_t cnt = (size_t)(da->size / 16 + 16), j, off = 0;
    char *names = malloc(names_sz);
    uint8_t *types;

    if (cnt < 64) cnt = 64;
    types = malloc(cnt);
    if (!names || !types) { fprintf(stderr, "oom\n"); exit(2); }
    if (rwy_readdir(s, path, names, names_sz, &cnt, types) != RWY_OK) {
        ERR(c, "readdir '%s'", path);
        goto out;
    }
    check_dir_names(c, names, cnt, path);
    for (j = 0; j < cnt; j++) {
        check_dir_entry(c, s, path, names + off, types[j], sc, st);
        off += strlen(names + off) + 1;
    }
out:
    free(names);
    free(types);
}

static int seen_before(const uint64_t *seen, uint32_t nseen, uint64_t oid) {
    uint32_t k;
    for (k = 0; k < nseen; k++)
        if (seen[k] == oid) return 1;
    return 0;
}

/* Walk the tree from the root with an explicit stack, then cross-check the link counts. */
void check_dir_tree(check_ctx_t *c, rwy_store_t *s, scan_t *sc) {
    static dir_stack_t st;
    uint64_t seen[RWY_MAX_INODES];
    uint32_t nseen = 0, i;

    st.depth = 0;
    st.path[0][0] = 0;
    while (st.depth >= 0) {
        char cur[RWY_MAX_PATH];
        uint64_t doid;
        rwy_attr_t da;
        strcpy(cur, st.path[st.depth--]); /* copy: pushes below reuse stack slots */
        if (rwy_lookup(s, cur, &doid) != RWY_OK) {
            ERR(c, "cannot resolve dir '%s'", cur); continue;
        }
        if (rwy_getattr(s, doid, &da) != RWY_OK || da.type != RWY_TYPE_DIR) {
            ERR(c, "not a dir: '%s'", cur); continue;
        }
        if (seen_before(seen, nseen, doid)) { ERR(c, "directory loop at '%s'", cur); continue; }
        seen[nseen++] = doid;
        check_dir(c, s, cur, &da, sc, &st);
    }

    if (s->inodes[0].nlink != 1)
        ERR(c, "root inode nlink is %u, expected 1", s->inodes[0].nlink);
    for (i = 0; i < sc->nlive; i++) {
        uint64_t o = sc->live_oids[i];
        if (o == 1) continue;
        if (s->inodes[o - 1].nlink != sc->dirent_refs[o])
            ERR(c, "oid %llu: nlink mismatch (inode has %u, directory entries count %u)",
                (unsigned long long)o, s->inodes[o - 1].nlink, sc->dirent_refs[o]);
    }
}

/*
 * Compare the checker's own walk with the core's reachability scan: the
 * checker and the collector share one notion of "reachable", so any
 * divergence is a bug in either side.
 */
static void check_core_reachability(check_ctx_t *c, rwy_store_t *s) {
    uint8_t *reach = calloc(1, c->total_blocks / 8);
    uint32_t b;

    if (!reach) { fprintf(stderr, "oom\n"); exit(2); }
    if (rwy_mark_data_reachable(s, s->inodes, reach) != RWY_OK)
        ERR(c, "core reachability scan failed (corrupt extent metadata)");
    else {
        for (b = c->pool_start; b < c->total_blocks; b++) {
            int core_ref = ((reach[b >> 3] >> (b & 7)) & 1);
            if ((c->owner[b] != -1) != core_ref)
                ERR(c, "checker/core reachability mismatch at block %u", b);
        }
    }
    free(reach);
}

/* A marked block nobody references that still holds a current xattr block is an orphan. */
static void check_orphan_xattr_blocks(check_ctx_t *c, rwy_block_dev_t *dev, rwy_store_t *s) {
    uint32_t b;

    for (b = c->pool_start; b < c->total_blocks; b++) {
        uint8_t blk[RWY_BLOCK_SIZE];
        const rwy_blk_header_t *h = (const rwy_blk_header_t *)blk;
        if (c->owner[b] != -1 || !rwy_blk_used(s, b))
            continue;
        if (dev->read(dev->ctx, (uint64_t)b * RWY_BLOCK_SIZE, blk, sizeof(blk)))
            continue;
        if (h->magic == RWY_BLK_MAGIC && h->kind == RWY_BLK_KIND_XATTR &&
            h->generation == s->sb.generation)
            ERR(c, "block %u: orphan xattr block (generation %llu, unreferenced)",
                b, (unsigned long long)h->generation);
    }
}

void check_bitmap(check_ctx_t *c, rwy_block_dev_t *dev, rwy_store_t *s, uint32_t referenced_data) {
    uint32_t b, marked = 0, referenced = 0, ghost = 0;

    check_core_reachability(c, s);
    for (b = 0; b < c->total_blocks; b++) {
        int is_marked = rwy_blk_used(s, b);
        if (is_marked) marked++;
        if (c->owner[b] == -1)
            continue;
        referenced++;
        if (!is_marked) {
            ERR(c, "block %u referenced but not marked in bitmap", b);
            ghost++;
        }
    }
    check_orphan_xattr_blocks(c, dev, s);
    c->leftovers = (int)rwy_reclaimable(s);
    printf("blocks: marked=%u referenced=%u data=%u\n", marked, referenced, referenced_data);
    if (c->leftovers)
        NOTE(c, "%u marked-but-unreachable blocks (transaction leftovers, reclaimable)", c->leftovers);
    else
        NOTE(c, "no reclaimable leftovers (bitmap matches reachable set)");
    if (ghost == 0) NOTE(c, "no referenced-but-unmarked blocks");
}

/* Fragmentation is reported always and never triggers anything. */
void report_fragmentation(check_ctx_t *c, rwy_store_t *s) {
    rwy_frag_info_t fi;

    if (rwy_frag_info(s, &fi) != RWY_OK)
        return;
    c->frag_objects = (int)fi.frag_objects;
    printf("frag: %u multi-extent objects, live=%u blocks, free=%u blocks, largest free run=%u blocks\n",
           fi.frag_objects, fi.live_blocks, fi.free_blocks, fi.largest_free_run);
}

/* Unreachable inodes block repair, so they only warn. */
void check_orphans(check_ctx_t *c, const scan_t *sc) {
    uint32_t k;

    for (k = 0; k < sc->nlive; k++) {
        uint64_t o = sc->live_oids[k];
        if (o == 1) continue;
        if (sc->dirent_refs[o] == 0) {
            WARN(c, "oid %llu live but unreachable (orphan)", (unsigned long long)o);
            c->orphans++;
        }
    }
}
