/*
 * rwyctl_recarea.c — `rwyctl recovery area set|show`: the plaintext recovery area of a store.
 * SPDX-License-Identifier: Apache-2.0
 * The area is outside any encrypted range, so these commands need no key and work on a locked
 * store. They read and write the raw members, never the tree.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rwy_recarea.h"
#include "rwyctl.h"

static const char *const PART_NAME[RWY_RA_KINDS] = { "KERNEL", "INITRD", "DTB", "ARGS" };

/* The whole file in memory; 0 on success. A part is at least one byte and fits 32 bits. */
static int load_file(const char *path, const void **data, uint32_t *len) {
    struct stat st;
    void *buf;
    int fd = open(path, O_RDONLY), ok;
    if (fd < 0) return -1;
    ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && (uint64_t)st.st_size <= 0xffffffffu;
    buf = ok ? malloc((size_t)st.st_size) : NULL;
    ok = buf && read(fd, buf, (size_t)st.st_size) == (ssize_t)st.st_size;
    close(fd);
    if (!ok) { free(buf); return -1; }
    *data = buf;
    *len = (uint32_t)st.st_size;
    return 0;
}

static int part_of_option(const char *opt) {
    if (!strcmp(opt, "--kernel")) return RWY_RA_KERNEL;
    if (!strcmp(opt, "--initrd")) return RWY_RA_INITRD;
    if (!strcmp(opt, "--dtb")) return RWY_RA_DTB;
    if (!strcmp(opt, "--args")) return RWY_RA_ARGS;
    return -1;
}

/* --args is text, or @FILE; the other parts are files. */
static int load_part(rwy_ra_part_t *p, int kind, const char *arg) {
    if (kind == RWY_RA_ARGS && arg[0] != '@') {
        p->len = (uint32_t)strlen(arg);
        p->data = p->len ? strdup(arg) : NULL;
        return p->data ? 0 : -1;
    }
    return load_file(kind == RWY_RA_ARGS ? arg + 1 : arg, &p->data, &p->len);
}

static void free_bundle(rwy_ra_bundle_t *b) {
    for (unsigned k = 0; k < RWY_RA_KINDS; k++) free((void *)b->part[k].data);
}

static int parse_bundle(int argc, char **argv, rwy_ra_bundle_t *b) {
    for (int i = 0; i < argc; i++) {
        int kind = part_of_option(argv[i]);
        if (kind < 0 || i + 1 >= argc || b->part[kind].data) { warn("bad option %s", argv[i]); return X_USAGE; }
        if (load_part(&b->part[kind], kind, argv[i + 1])) { warn("cannot use %s as a recovery part", argv[i + 1]); return X_USAGE; }
        i++;
    }
    if (!b->part[RWY_RA_KERNEL].data) { warn("%s", "a bundle needs --kernel"); return X_USAGE; }
    if (!rwy_ra_blocks(b)) { warn("%s", "the bundle is not valid (--args is at most 4096 bytes)"); return X_USAGE; }
    return 0;
}

/* Refuse before any write: every live member needs an area, and the bundle must fit it. */
static int check_fit(group_t *g, uint32_t need) {
    for (int i = 0; i < g->n; i++) {
        member_t *m = &g_mem[g->m[i]];
        if (!m->ok) continue;
        if (!m->recovery_blocks) { warn("member %s has no recovery area (mkstore --recovery-size)", m->path); return X_POLICY; }
        if (need > m->recovery_blocks) {
            char msg[PATH_MAX + 64];
            snprintf(msg, sizeof(msg), "the bundle needs %u blocks, a copy of member %s has %u", need, m->path, m->recovery_blocks);
            warn("%s", msg);
            return X_NOSPC;
        }
    }
    return 0;
}

static int live_members(group_t *g) {
    int n = 0;
    for (int i = 0; i < g->n; i++) n += g_mem[g->m[i]].ok;
    return n;
}

/* set: write every member; one that cannot be opened or written makes the command fail after the others. */
static int area_set(group_t *g, int argc, char **argv) {
    rwy_ra_bundle_t b = { 0 };
    int rc, wanted, written = 0;
    if ((rc = parse_bundle(argc, argv, &b)) || (rc = system_allowed()) || (rc = check_fit(g, rwy_ra_blocks(&b)))) goto out;
    wanted = live_members(g);
    if ((rc = lock_group(g))) goto out;
    for (int i = 0; i < g->n; i++) {
        member_t *m = &g_mem[g->m[i]];
        if (!m->ok) continue;
        if (rwy_ra_write(&m->raw, m->recovery_start, m->recovery_blocks, &b)) {
            warn("member %s: cannot write the recovery area", m->path);
            continue;
        }
        printf("member %s: recovery area written\n", m->path);
        written++;
    }
    rc = written == wanted ? 0 : X_IO;
    if (written != wanted) warn("%s", "degraded: not every member was written");
out:
    free_bundle(&b);
    return rc;
}

static void print_sha(const uint8_t sum[32]) {
    for (int i = 0; i < 32; i++) printf("%02x", sum[i]);
}

static void print_copy(char name, int status, uint64_t seq) {
    if (status == RWY_RA_OK) printf("  copy %c: valid, sequence %llu\n", name, (unsigned long long)seq);
    else printf("  copy %c: %s\n", name, rwy_ra_reason(status));
}

/* One member: both copies, which one wins and its parts. Returns RWY_OK when a copy is valid. */
static int show_member(const member_t *m, rwy_ra_info_t *info) {
    int rc = rwy_ra_pick(&m->raw, m->recovery_start, m->recovery_blocks, info);
    printf("member %s: recovery area 2 x %u blocks at %u\n", m->path, m->recovery_blocks, m->recovery_start);
    print_copy('A', info->status[0], info->seq[0]);
    print_copy('B', info->status[1], info->seq[1]);
    if (rc) { printf("  no valid bundle\n"); return rc; }
    printf("  using copy %c\n", 'A' + info->copy);
    for (uint32_t i = 0; i < info->count; i++) {
        printf("    %s %llu bytes sha256 ", PART_NAME[info->entry[i].kind], (unsigned long long)info->entry[i].length);
        print_sha(info->entry[i].sha256);
        putchar('\n');
    }
    return RWY_OK;
}

/* show: read-only; exit X_CHECK when a member has no valid bundle or the members differ. */
static int area_show(group_t *g) {
    rwy_ra_info_t first, info;
    int have_first = 0, bad = 0, differ = 0, with_area = 0;
    for (int i = 0; i < g->n; i++) {
        const member_t *m = &g_mem[g->m[i]];
        int rc;
        if (!m->ok) continue;
        if (!m->recovery_blocks) { printf("member %s: no recovery area\n", m->path); continue; }
        with_area++;
        rc = show_member(m, &info);
        if (rc == RWY_ERR_IO) return X_IO;
        if (rc) { bad = 1; continue; }
        if (!have_first) { first = info; have_first = 1; }
        else if (!rwy_ra_same(&first, &info)) differ = 1;
    }
    if (with_area > 1 && !bad) printf(differ ? "warning: members hold different bundles\n" : "members hold the same bundle\n");
    return bad || differ ? X_CHECK : 0;
}

int cmd_recovery_area(group_t *g, int argc, char **argv) {
    if (argc >= 1 && !strcmp(argv[0], "set")) return area_set(g, argc - 1, argv + 1);
    if (argc == 1 && !strcmp(argv[0], "show")) return area_show(g);
    warn("%s", "usage: recovery area set --kernel SRC [--initrd SRC] [--dtb SRC] [--args TEXT|@FILE] | recovery area show");
    return X_USAGE;
}
