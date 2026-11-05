/*
 * rwyctl_loader.c — `rwyctl loader show|set|commit`: manage the plaintext loader area.
 * SPDX-License-Identifier: Apache-2.0
 * The area is outside the encrypted range, so commands need no keys and run before unlocking.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rwy_bundle.h"
#include "rwy_loader_fsm.h"
#include "rwyctl.h"

typedef struct {
    rwy_loader_record_t rec;
    int rec_ok;
    int status_a;
    int status_b;
    rwy_bundle_header_t hdr_a;
    rwy_bundle_header_t hdr_b;
} member_loader_info_t;

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

static int wdt_filter(const struct dirent *de) {
    return de->d_name[0] != '.';
}

static int load_wdt_dir(const char *dir, rwy_bundle_payload_t *parts, uint32_t max_wdt, uint32_t *loaded) {
    struct dirent **namelist = NULL;
    int n = scandir(dir, &namelist, wdt_filter, alphasort);
    uint32_t count = 0;

    if (n < 0) {
        warn("cannot read WDT directory: %s", strerror(errno));
        return -1;
    }
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, namelist[i]->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (count >= max_wdt) {
                warn("too many WDT drivers in --wdt (at most 8)", dir);
                for (int j = i; j < n; j++) free(namelist[j]);
                free(namelist);
                return -1;
            }
            if (load_file(path, &parts[count].data, &parts[count].len)) {
                warn("cannot read WDT driver %s", path);
                for (int j = i; j < n; j++) free(namelist[j]);
                free(namelist);
                return -1;
            }
            parts[count].kind = RWY_ENTRY_WDT;
            count++;
        }
        free(namelist[i]);
    }
    free(namelist);
    *loaded = count;
    return 0;
}

static void free_parts(rwy_bundle_payload_t *parts, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) free((void *)parts[i].data);
}

static int check_fit(group_t *g, uint32_t need) {
    for (int i = 0; i < g->n; i++) {
        member_t *m = &g_mem[g->m[i]];
        if (!m->ok) continue;
        if (!m->loader_blocks) {
            warn("member %s has no loader area (mkstore --loader)", m->path);
            return X_POLICY;
        }
        if (need > m->loader_blocks) {
            char msg[PATH_MAX + 64];
            snprintf(msg, sizeof(msg), "the bundle needs %u blocks, a copy of member %s has %u",
                     need, m->path, m->loader_blocks);
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

static void print_sha(const uint8_t sum[32]) {
    for (int i = 0; i < 32; i++) printf("%02x", sum[i]);
}

static const char *bad_str(uint8_t bad) {
    switch (bad & 3) {
    case 1: return "A";
    case 2: return "B";
    case 3: return "A,B";
    default: return "none";
    }
}

static const char *trial_str(uint8_t trial) {
    switch (trial) {
    case RWY_LOADER_TRIAL_A: return "A";
    case RWY_LOADER_TRIAL_B: return "B";
    default: return "none";
    }
}

static char good_char(uint8_t good) {
    return good == RWY_LOADER_COPY_B ? 'B' : 'A';
}

static void print_bundle_copy(char name, int status, const rwy_bundle_header_t *hdr) {
    if (status == RWY_BUNDLE_OK) {
        printf("  copy %c: valid, sequence %llu\n", name, (unsigned long long)hdr->sequence);
        for (uint32_t i = 0; i < hdr->count; i++) {
            const char *kind = (hdr->entry[i].kind == RWY_ENTRY_LOADER) ? "LOADER" : "WDT";
            printf("    %s %llu bytes sha256 ", kind, (unsigned long long)hdr->entry[i].length);
            print_sha(hdr->entry[i].sha256);
            putchar('\n');
        }
    } else {
        printf("  copy %c: %s\n", name, rwy_bundle_reason(status));
    }
}

static int read_member_info(const member_t *m, member_loader_info_t *info) {
    int active = -1;
    info->rec_ok = (rwy_loader_record_pick(&m->raw, m->loader_start, &info->rec, &active) == RWY_OK);
    info->status_a = rwy_bundle_check_copy(&m->raw, m->loader_start + 2, m->loader_blocks,
                                          &rwy_loader_bundle_spec, &info->hdr_a);
    info->status_b = rwy_bundle_check_copy(&m->raw, m->loader_start + 2 + m->loader_blocks, m->loader_blocks,
                                          &rwy_loader_bundle_spec, &info->hdr_b);
    return (!info->rec_ok || (info->status_a != RWY_BUNDLE_OK && info->status_b != RWY_BUNDLE_OK)) ? 1 : 0;
}

static void show_member_text(const member_t *m, const member_loader_info_t *info) {
    printf("member %s: loader area 2 x %u blocks at %u\n", m->path, m->loader_blocks, m->loader_start);
    if (info->rec_ok) {
        printf("  state record: seq %llu, good %c, trial %s, tries %u, timeout %us, bad %s, reason %s, wdt_armed %s\n",
               (unsigned long long)info->rec.sequence, good_char(info->rec.good), trial_str(info->rec.trial),
               (unsigned)info->rec.tries, (unsigned)info->rec.timeout, bad_str(info->rec.bad),
               rwy_loader_reason_str(info->rec.reason), info->rec.wdt_armed[0] ? info->rec.wdt_armed : "none");
    } else {
        printf("  state record: invalid or corrupt\n");
    }
    print_bundle_copy('A', info->status_a, &info->hdr_a);
    print_bundle_copy('B', info->status_b, &info->hdr_b);
}

static void print_json_copy(char name, int status, const rwy_bundle_header_t *hdr) {
    printf("        \"%c\": {\n", name);
    printf("          \"status\": \"%s\"", (status == RWY_BUNDLE_OK) ? "valid" : rwy_bundle_reason(status));
    if (status == RWY_BUNDLE_OK) {
        printf(",\n          \"sequence\": %llu,\n", (unsigned long long)hdr->sequence);
        printf("          \"entries\": [\n");
        for (uint32_t i = 0; i < hdr->count; i++) {
            const char *kind = (hdr->entry[i].kind == RWY_ENTRY_LOADER) ? "LOADER" : "WDT";
            printf("            {\n");
            printf("              \"kind\": \"%s\",\n", kind);
            printf("              \"size\": %llu,\n", (unsigned long long)hdr->entry[i].length);
            printf("              \"sha256\": \"");
            print_sha(hdr->entry[i].sha256);
            printf("\"\n");
            printf("            }%s\n", (i + 1 < hdr->count) ? "," : "");
        }
        printf("          ]\n");
    } else {
        printf("\n");
    }
    printf("        }");
}

static void show_member_json(const member_t *m, const member_loader_info_t *info, int last) {
    printf("    {\n");
    printf("      \"path\": \"%s\",\n", m->path);
    printf("      \"loader_start\": %u,\n", m->loader_start);
    printf("      \"loader_blocks\": %u,\n", m->loader_blocks);
    if (info->rec_ok) {
        printf("      \"record\": {\n");
        printf("        \"sequence\": %llu,\n", (unsigned long long)info->rec.sequence);
        printf("        \"good\": \"%c\",\n", good_char(info->rec.good));
        printf("        \"trial\": \"%s\",\n", trial_str(info->rec.trial));
        printf("        \"tries\": %u,\n", (unsigned)info->rec.tries);
        printf("        \"timeout\": %u,\n", (unsigned)info->rec.timeout);
        printf("        \"bad\": \"%s\",\n", bad_str(info->rec.bad));
        printf("        \"reason\": \"%s\",\n", rwy_loader_reason_str(info->rec.reason));
        printf("        \"wdt_armed\": \"%s\"\n", info->rec.wdt_armed[0] ? info->rec.wdt_armed : "");
        printf("      },\n");
    } else {
        printf("      \"record\": null,\n");
    }
    printf("      \"copies\": {\n");
    print_json_copy('A', info->status_a, &info->hdr_a);
    printf(",\n");
    print_json_copy('B', info->status_b, &info->hdr_b);
    printf("\n      }\n");
    printf("    }%s\n", last ? "" : ",");
}

static int bundle_header_same(const rwy_bundle_header_t *a, const rwy_bundle_header_t *b) {
    if (a->sequence != b->sequence || a->count != b->count) return 0;
    for (uint32_t i = 0; i < a->count; i++) {
        if (a->entry[i].kind != b->entry[i].kind ||
            a->entry[i].length != b->entry[i].length ||
            memcmp(a->entry[i].sha256, b->entry[i].sha256, 32) != 0)
            return 0;
    }
    return 1;
}

static int record_same(const rwy_loader_record_t *a, const rwy_loader_record_t *b) {
    if (a->sequence != b->sequence || a->good != b->good ||
        a->trial != b->trial || a->tries != b->tries ||
        a->bad != b->bad || a->timeout != b->timeout ||
        a->reason != b->reason)
        return 0;
    return strcmp(a->wdt_armed, b->wdt_armed) == 0;
}

static int info_same(const member_loader_info_t *a, const member_loader_info_t *b) {
    if (a->rec_ok != b->rec_ok) return 0;
    if (a->rec_ok && !record_same(&a->rec, &b->rec)) return 0;
    if (a->status_a != b->status_a || a->status_b != b->status_b) return 0;
    if (a->status_a == RWY_BUNDLE_OK && !bundle_header_same(&a->hdr_a, &b->hdr_a)) return 0;
    if (a->status_b == RWY_BUNDLE_OK && !bundle_header_same(&a->hdr_b, &b->hdr_b)) return 0;
    return 1;
}

static void show_member_dispatch(const member_t *m, const member_loader_info_t *info, int json, int last) {
    if (json) {
        show_member_json(m, info, last);
    } else {
        show_member_text(m, info);
    }
}

static void show_report(int json, int bad, int differ, int with_area) {
    if (json) {
        printf("  ],\n  \"consistent\": %s\n}\n", (differ || bad) ? "false" : "true");
    } else if (with_area > 1 && !bad) {
        printf(differ ? "warning: members hold different loader records or bundles\n" :
                        "members hold the same loader state and bundles\n");
    }
}

static int area_show(group_t *g, int json) {
    member_loader_info_t first_info, cur_info;
    int have_first = 0, bad = 0, differ = 0, with_area = 0;

    if (json) printf("{\n  \"members\": [\n");
    for (int i = 0; i < g->n; i++) {
        const member_t *m = &g_mem[g->m[i]];
        if (!m->ok) continue;
        if (!m->loader_blocks) {
            if (!json) printf("member %s: no loader area\n", m->path);
            continue;
        }
        with_area++;
        if (read_member_info(m, &cur_info)) bad = 1;
        show_member_dispatch(m, &cur_info, json, i == g->n - 1);
        if (!have_first) {
            first_info = cur_info;
            have_first = 1;
        } else if (!info_same(&first_info, &cur_info)) {
            differ = 1;
        }
    }
    show_report(json, bad, differ, with_area);
    return (bad || differ) ? X_CHECK : 0;
}

static uint64_t pick_next_seq(const member_t *m) {
    rwy_bundle_header_t hdr;
    uint64_t max_seq = 0;

    if (rwy_bundle_check_copy(&m->raw, m->loader_start + 2, m->loader_blocks,
                              &rwy_loader_bundle_spec, &hdr) == RWY_BUNDLE_OK) {
        max_seq = hdr.sequence;
    }
    if (rwy_bundle_check_copy(&m->raw, m->loader_start + 2 + m->loader_blocks, m->loader_blocks,
                              &rwy_loader_bundle_spec, &hdr) == RWY_BUNDLE_OK) {
        if (hdr.sequence > max_seq) max_seq = hdr.sequence;
    }
    return max_seq + 1;
}

static int write_member_loader(const member_t *m, const rwy_bundle_payload_t *parts,
                               uint32_t count, uint32_t tries, uint32_t timeout) {
    rwy_loader_record_t rec;
    int active_idx = -1, new_active_idx, target_copy, rc;
    uint32_t base;
    uint64_t seq;

    if (rwy_loader_record_pick(&m->raw, m->loader_start, &rec, &active_idx) != RWY_OK) {
        memset(&rec, 0, sizeof(rec));
        rec.good = RWY_LOADER_COPY_A;
        active_idx = 1;
        rec.sequence = 0;
    }
    target_copy = (rec.good == RWY_LOADER_COPY_A) ? RWY_LOADER_COPY_B : RWY_LOADER_COPY_A;
    base = m->loader_start + 2 + (uint32_t)target_copy * m->loader_blocks;
    seq = pick_next_seq(m);

    rc = rwy_bundle_write_copy(&m->raw, base, m->loader_blocks, &rwy_loader_bundle_spec, parts, count, seq);
    if (rc) {
        warn("member %s: cannot write loader bundle", m->path);
        return -1;
    }
    rec.trial = (target_copy == RWY_LOADER_COPY_A) ? RWY_LOADER_TRIAL_A : RWY_LOADER_TRIAL_B;
    rec.tries = (uint8_t)tries;
    rec.timeout = timeout;
    rec.bad &= (uint8_t)~(1U << target_copy);
    rec.reason = RWY_LOADER_REASON_NONE;
    rec.wdt_armed[0] = '\0';

    rc = rwy_loader_record_write(&m->raw, m->loader_start, active_idx, &rec, &new_active_idx);
    if (rc) {
        warn("member %s: cannot write loader state record", m->path);
        return -1;
    }
    printf("member %s: loader set on trial (copy %c, %u tries)\n", m->path, 'A' + target_copy, tries);
    return 0;
}

static int parse_set_options(int argc, char **argv, const char **file, const char **wdt,
                             uint32_t *tries, uint32_t *timeout) {
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--wdt")) {
            if (i + 1 >= argc) { warn("%s", "option --wdt needs an argument"); return X_USAGE; }
            *wdt = argv[++i];
        } else if (!strcmp(argv[i], "--tries")) {
            if (i + 1 >= argc) { warn("%s", "option --tries needs an argument"); return X_USAGE; }
            *tries = (uint32_t)strtoul(argv[++i], NULL, 10);
            if (*tries == 0 || *tries > 255) { warn("%s", "--tries must be between 1 and 255"); return X_USAGE; }
        } else if (!strcmp(argv[i], "--timeout")) {
            if (i + 1 >= argc) { warn("%s", "option --timeout needs an argument"); return X_USAGE; }
            *timeout = (uint32_t)strtoul(argv[++i], NULL, 10);
            if (*timeout == 0) { warn("%s", "--timeout must be positive"); return X_USAGE; }
        } else if (argv[i][0] == '-') {
            warn("bad option %s", argv[i]);
            return X_USAGE;
        } else if (!*file) {
            *file = argv[i];
        } else {
            warn("unexpected argument %s", argv[i]);
            return X_USAGE;
        }
    }
    if (!*file) { warn("%s", "loader set requires a loader binary"); return X_USAGE; }
    return 0;
}

static int area_set(group_t *g, int argc, char **argv) {
    const char *file = NULL, *wdt_dir = NULL;
    uint32_t tries = 3, timeout = RWY_LOADER_TIMEOUT_DEFAULT, wdt_count = 0, nparts = 0, need;
    rwy_bundle_payload_t parts[9];
    int rc, wanted, written = 0;

    memset(parts, 0, sizeof(parts));
    if ((rc = parse_set_options(argc, argv, &file, &wdt_dir, &tries, &timeout))) return rc;
    if (load_file(file, &parts[0].data, &parts[0].len)) {
        warn("cannot read loader binary %s", file);
        return X_USAGE;
    }
    parts[0].kind = RWY_ENTRY_LOADER;
    nparts = 1;
    if (wdt_dir) {
        if (load_wdt_dir(wdt_dir, parts + 1, 8, &wdt_count)) {
            free_parts(parts, nparts);
            return X_USAGE;
        }
        nparts += wdt_count;
    }
    need = rwy_bundle_blocks(&rwy_loader_bundle_spec, parts, nparts);
    if ((rc = check_fit(g, need)) || (rc = lock_group(g))) {
        free_parts(parts, nparts);
        return rc;
    }
    wanted = live_members(g);
    for (int i = 0; i < g->n; i++) {
        member_t *m = &g_mem[g->m[i]];
        if (!m->ok) continue;
        if (write_member_loader(m, parts, nparts, tries, timeout) == 0)
            written++;
    }
    free_parts(parts, nparts);
    rc = (written == wanted) ? 0 : X_IO;
    if (written != wanted) warn("%s", "degraded: not every member was written");
    return rc;
}

static int area_commit(group_t *g) {
    int wanted, written = 0, rc;

    if ((rc = lock_group(g))) return rc;
    wanted = live_members(g);
    for (int i = 0; i < g->n; i++) {
        member_t *m = &g_mem[g->m[i]];
        rwy_loader_record_t rec;
        int active_idx = -1, new_active_idx;

        if (!m->ok) continue;
        if (!m->loader_blocks) {
            warn("member %s has no loader area", m->path);
            continue;
        }
        if (rwy_loader_record_pick(&m->raw, m->loader_start, &rec, &active_idx) != RWY_OK) {
            warn("member %s: cannot read state record", m->path);
            continue;
        }
        if (rec.trial == RWY_LOADER_TRIAL_NONE) {
            warn("member %s: no trial loader copy is pending", m->path);
            return X_POLICY;
        }
        rec.good = (rec.trial == RWY_LOADER_TRIAL_A) ? RWY_LOADER_COPY_A : RWY_LOADER_COPY_B;
        rec.trial = RWY_LOADER_TRIAL_NONE;
        rec.tries = 0;
        if (rwy_loader_record_write(&m->raw, m->loader_start, active_idx, &rec, &new_active_idx)) {
            warn("member %s: cannot write state record", m->path);
            continue;
        }
        printf("member %s: loader committed (copy %c is good)\n", m->path, 'A' + rec.good);
        written++;
    }
    rc = (written == wanted) ? 0 : X_IO;
    if (written != wanted) warn("%s", "degraded: not every member was written");
    return rc;
}

int cmd_loader(group_t *g, int argc, char **argv) {
    if (argc >= 1 && !strcmp(argv[0], "set")) return area_set(g, argc - 1, argv + 1);
    if (argc >= 1 && !strcmp(argv[0], "show")) {
        int json = (argc >= 2 && !strcmp(argv[1], "--json"));
        return area_show(g, json);
    }
    if (argc == 1 && !strcmp(argv[0], "commit")) return area_commit(g);
    warn("%s", "usage: loader show [--json] | loader set FILE [--wdt DIR] [--timeout N] [--tries N] | loader commit");
    return X_USAGE;
}
