/* rwyctl: finding the store members on the running system (partitions, mounts, EFI variables). */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include "partuuid.h"
#include "rwyctl.h"

/* ---------- members and groups ---------- */
int add_member(const char *path) {
    member_t *m = &g_mem[g_nmem];
    uint8_t blk[RWY_BLOCK_SIZE];
    struct stat st;
    rwy_superblock_t sb;
    int i;
    memset(m, 0, sizeof(*m));
    snprintf(m->path, sizeof(m->path), "%s", path);
    if (hostdev_open(&m->hd, path, O_RDONLY) || fstat(m->hd.fd, &st) || !(S_ISREG(st.st_mode) || S_ISBLK(st.st_mode))) {
        snprintf(g_why, sizeof(g_why), "%s: %s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path,
                 m->hd.fd < 0 ? strerror(errno) : "not a regular file or block device");
        hostdev_close(&m->hd);
        return -1;
    }
    hostdev_dev(&m->hd, &m->raw);
    for (i = 0; i < 2; i++) {   /* both superblock slots, highest valid generation */
        if (m->raw.read(m->raw.ctx, (uint64_t)i * RWY_BLOCK_SIZE, blk, sizeof(blk))) continue;
        if (rwy_validate_sb_block(blk, &sb) != RWY_OK) continue;
        if (!m->ok || sb.generation > m->gen) {
            m->ok = 1; m->gen = sb.generation; m->enc = sb.crypt_algo == RWY_CRYPT_AES_XTS_256;
            memcpy(m->uuid, sb.uuid, 16);
            m->recovery_start = sb.recovery_start; m->recovery_blocks = sb.recovery_blocks;
            m->loader_start = sb.loader_start; m->loader_blocks = sb.loader_blocks;
            m->flight_start = sb.flight_start; m->flight_blocks = sb.flight_blocks;
            m->net_start = sb.net_start; m->net_blocks = sb.net_blocks;
            m->journal_start = sb.journal_start; m->journal_blocks = sb.journal_blocks;
            m->rec_start = sb.rec_start;
        }
    }
    if (!m->ok) {
        snprintf(g_why, sizeof(g_why), "not a Runway store (no valid superblock)");
        hostdev_close(&m->hd);
        return -1;
    }
    m->dev = m->raw;
    g_nmem++;
    return g_nmem - 1;
}

void add_to_group(int mi, int boot) {
    int g;
    for (g = 0; g < g_ngrp; g++) if (!memcmp(g_grp[g].uuid, g_mem[mi].uuid, 16)) break;
    if (g == g_ngrp) {
        if (g_ngrp == MAXG) return;
        memset(&g_grp[g], 0, sizeof(g_grp[g]));
        memcpy(g_grp[g].uuid, g_mem[mi].uuid, 16);
        g_ngrp++;
    }
    if (g_grp[g].n < MAXM) g_grp[g].m[g_grp[g].n++] = mi;
    if (boot) g_grp[g].boot = 1;
}

/* ---------- discovery ---------- */
/* The partition type of a store partition (Extended Boot Loader), as RUNWAY.EFI wants it. */
#define RWY_STORE_PART_TYPE "bc13c2ff-59e6-4262-a352-b275fd6f7172"

static int mountpoint_of(const char *devnode, char *out) {
    char p[PATH_MAX], line[2 * PATH_MAX];
    FILE *f;
    int found = 0;
    rpath(p, "/proc/self/mountinfo");
    f = fopen(p, "r");
    if (!f) return 0;
    while (!found && fgets(line, sizeof(line), f)) {   /* id parent maj:min root mountpoint opts ... - fstype source */
        char *sep = strstr(line, " - "), mp[PATH_MAX], fst[64], src[PATH_MAX];
        if (!sep || sscanf(line, "%*s %*s %*s %*s %4095s", mp) != 1) continue;
        if (sscanf(sep + 3, "%63s %4095s", fst, src) != 2) continue;
        if (!strcmp(src, devnode)) { snprintf(out, PATH_MAX, "%s", mp); found = 1; }
    }
    fclose(f);
    return found;
}

static int temp_mount(const char *devnode, char *out) {
    if (g_ntmp == 8) { snprintf(g_why, sizeof(g_why), "too many temporary mounts"); return 0; }
    snprintf(g_tmpmnt[g_ntmp], PATH_MAX, "/var/tmp/rwyctl-XXXXXX");
    if (!mkdtemp(g_tmpmnt[g_ntmp])) { snprintf(g_why, sizeof(g_why), "mkdtemp: %s", strerror(errno)); return 0; }
    if (mount(devnode, g_tmpmnt[g_ntmp], "vfat", MS_RDONLY, "") && mount(devnode, g_tmpmnt[g_ntmp], "vfat", 0, "sync")) {
        snprintf(g_why, sizeof(g_why), "mount: %s", strerror(errno));
        rmdir(g_tmpmnt[g_ntmp]);
        return 0;
    }
    snprintf(out, PATH_MAX, "%s", g_tmpmnt[g_ntmp++]);
    return 1;
}

/* The RUNWAY.DAT of a partition with a FAT file system. Never reached for a partition that holds a store. */
static int probe_file_store(const char *devnode) {
    char mp[PATH_MAX], f[PATH_MAX + 16];
    if (!mountpoint_of(devnode, mp) && !temp_mount(devnode, mp)) return -1;
    snprintf(f, sizeof(f), "%s/RUNWAY.DAT", mp);
    return add_member(f);
}

/* Names the member after its partition. Returns mi. */
static int named(const char *devnode, int mi) {
    if (mi >= 0) snprintf(g_mem[mi].node, sizeof(g_mem[mi].node), "%s", strrchr(devnode, '/') ? strrchr(devnode, '/') + 1 : devnode);
    return mi;
}

static int probe_raw_store(const char *devnode) {
    char node[PATH_MAX];
    rpath(node, devnode);
    return named(devnode, add_member(node));
}

/* A partition that holds no store, with the reason on -v. */
static int probed(const char *devnode, int mi) {
    if (mi < 0 && g_verbose) fprintf(stderr, "rwyctl: skipped %s: %s\n", devnode, g_why);
    return named(devnode, mi);
}

/* One partition: the store on it becomes a member. The partition is read first (a raw store); a partition
 * without one is tried as a FAT file system with a RUNWAY.DAT. Nothing is written. Returns the member index or -1. */
int probe_partition(const char *devnode) {
    int mi = probe_raw_store(devnode);
    return mi >= 0 ? mi : probed(devnode, probe_file_store(devnode));
}

/* One pass over the partitions of a disk (`d` is its sysfs directory). Pass 0 reads the raw stores, and only on
 * partitions of the store type, as RUNWAY.EFI does (a disk with no readable GPT is not filtered). Pass 1 looks
 * for a RUNWAY.DAT. Returns the number of members found. */
static int probe_disk_pass(const char *d, const char *disk, const char *boot_part, int pass) {
    char p[PATH_MAX], node[PATH_MAX];
    struct dirent *e;
    DIR *dir = opendir(d);
    int n = 0;
    if (!dir) return 0;
    while ((e = readdir(dir))) {
        int mi;
        if (strncmp(e->d_name, disk, strlen(disk))) continue;
        snprintf(p, sizeof(p), "%s/%s/partition", d, e->d_name);
        if (access(p, F_OK)) continue;
        snprintf(node, sizeof(node), "/dev/%s", e->d_name);
        if (pass == 0 && partuuid_has_type(g_root, disk, e->d_name, RWY_STORE_PART_TYPE) == 0) continue;
        mi = pass == 0 ? probe_raw_store(node) : probed(node, probe_file_store(node));
        if (mi < 0) continue;
        add_to_group(mi, boot_part && !strcmp(boot_part, e->d_name));
        n++;
    }
    closedir(dir);
    return n;
}

/* Partitions of a disk (sysfs name "sda"). A RUNWAY.DAT is only looked for when the disk has no store partition,
 * as RUNWAY.EFI ignores it then. */
void probe_disk(const char *disk, const char *boot_part) {
    char d[PATH_MAX];
    rpath(d, "/sys/block/");
    strncat(d, disk, sizeof(d) - strlen(d) - 1);
    if (!probe_disk_pass(d, disk, boot_part, 0)) probe_disk_pass(d, disk, boot_part, 1);
}

#define EFIVARS "/sys/firmware/efi/efivars/"
#define RWY_VAR_GUID "b371a076-b7ef-4e6e-b98a-8d0d2c493c7b"

/* The text of an efivar holding UTF-16 (4 attribute bytes first), as lower-case ASCII. Returns its length or 0. */
static int var_text(const char *var, char *text, size_t cap) {
    char p[PATH_MAX];
    unsigned char raw[512];
    ssize_t n;
    size_t i, j = 0;
    int fd;
    rpath(p, var);
    fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    n = read(fd, raw, sizeof(raw));
    close(fd);
    for (i = 4; i + 1 < (size_t)n && j + 1 < cap && raw[i]; i += 2) text[j++] = (char)tolower(raw[i]);
    text[j] = 0;
    return (int)j;
}

/* The ESP of the variable `var` (a UTF-16 partition UUID). */
static int esp_from_var(const char *var, char *part, char *disk) {
    char uuid[40];
    if (!var_text(var, uuid, 37)) return 0;
    return partuuid_find(g_root, uuid, part, disk);
}

/* The ESP the loader was loaded from (RunwayBootESP), else the one the firmware booted from (LoaderDevicePartUUID). */
int boot_esp(char *part, char *disk) {
    return esp_from_var(EFIVARS "RunwayBootESP-" RWY_VAR_GUID, part, disk) ||
           esp_from_var(EFIVARS "LoaderDevicePartUUID-4a67b082-0a4c-41cf-b6c7-440b29bb8c4f", part, disk);
}

/* The members in use (RunwayBootMembers): the partition UUIDs it lists and the partition each one names. */
static struct { char uuid[37]; char node[64]; } g_listed[MAXM];
static int g_nlisted;

void read_listed_members(void) {
    char text[MAXM * 37 + 1], *tok;
    if (!var_text(EFIVARS "RunwayBootMembers-" RWY_VAR_GUID, text, sizeof(text))) return;
    for (tok = strtok(text, " "); tok && g_nlisted < MAXM; tok = strtok(NULL, " ")) {
        char disk[64];
        snprintf(g_listed[g_nlisted].uuid, sizeof(g_listed[g_nlisted].uuid), "%s", tok);
        if (!partuuid_find(g_root, tok, g_listed[g_nlisted].node, disk)) g_listed[g_nlisted].node[0] = 0;
        g_nlisted++;
    }
}

static int member_of_node(const char *node) {
    int i;
    for (i = 0; i < g_nmem; i++) if (g_mem[i].node[0] && !strcmp(g_mem[i].node, node)) return i;
    return -1;
}

/* Members on other disks than the boot ESP's: probe the partitions RUNWAY.EFI listed. */
void probe_listed_members(void) {
    char devnode[80];
    int i, mi;
    for (i = 0; i < g_nlisted; i++) {
        if (!g_listed[i].node[0] || member_of_node(g_listed[i].node) >= 0) continue;
        snprintf(devnode, sizeof(devnode), "/dev/%s", g_listed[i].node);
        mi = probe_partition(devnode);
        if (mi >= 0) add_to_group(mi, 0);
    }
}

/* Compare what was found with what RUNWAY.EFI listed: a missing or foreign member is a warning, a member
 * it did not use is only read (RUNWAY.EFI will catch it up). */
void check_listed_members(void) {
    int i, g, k, mi;
    if (!g_nlisted) return;
    for (g = 0; g < g_ngrp && !g_grp[g].boot; g++);
    if (g == g_ngrp) return;
    for (i = 0; i < g_nlisted; i++) {
        mi = g_listed[i].node[0] ? member_of_node(g_listed[i].node) : -1;
        if (mi < 0) warn("warning: member %s of the boot store is missing", g_listed[i].uuid);
        else if (memcmp(g_mem[mi].uuid, g_grp[g].uuid, 16)) warn("warning: member %s holds another store, left out", g_listed[i].uuid);
    }
    for (k = 0; k < g_grp[g].n; k++) {
        member_t *m = &g_mem[g_grp[g].m[k]];
        for (i = 0; i < g_nlisted && strcmp(m->node, g_listed[i].node); i++);
        if (i == g_nlisted) {
            warn("note: member %s was not used at boot, not written", m->path);
            m->stale = 1;
        }
    }
}

void discover_all(const char *boot_part) {
    char d[PATH_MAX];
    struct dirent *e;
    DIR *dir;
    rpath(d, "/sys/block");
    dir = opendir(d);
    if (!dir) return;
    while ((e = readdir(dir))) if (e->d_name[0] != '.') probe_disk(e->d_name, boot_part);
    closedir(dir);
}
