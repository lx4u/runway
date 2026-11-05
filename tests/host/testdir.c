/* Per-run scratch directory shared by the host tests. */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "testdir.h"

#define MAX_NAMES 512
#define SCRATCH_ROOT "/var/tmp"
#define SCRATCH_PREFIX "rwy-test."

static char g_dir[PATH_MAX];
static pid_t g_owner;

static int rm_entry(const char *path, const struct stat *sb, int type, struct FTW *ftw) {
    (void)sb; (void)type; (void)ftw;
    remove(path);
    return 0;
}

static void remove_tree(const char *dir) {
    nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* A forked child that calls exit() must not delete the directory of its parent */
static void cleanup(void) {
    if (getpid() == g_owner) remove_tree(g_dir);
}

/* A test stopped by a signal does not run atexit: remove the tree, then stop as before */
static void on_signal(int sig) {
    cleanup();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Remove scratch trees of earlier runs whose process is gone (killed with SIGKILL) */
static void sweep_stale(void) {
    DIR *d = opendir(SCRATCH_ROOT);
    struct dirent *de;
    size_t plen = strlen(SCRATCH_PREFIX);

    if (!d) return;
    while ((de = readdir(d)) != NULL) {
        char path[PATH_MAX];
        char *end;
        long pid;

        if (strncmp(de->d_name, SCRATCH_PREFIX, plen)) continue;
        pid = strtol(de->d_name + plen, &end, 10);
        if (end == de->d_name + plen || *end != '.' || pid <= 0) continue;
        if (kill((pid_t)pid, 0) == 0 || errno != ESRCH) continue;
        snprintf(path, sizeof(path), "%s/%s", SCRATCH_ROOT, de->d_name);
        remove_tree(path);
    }
    closedir(d);
}

static void make_dir(void) {
    sweep_stale();
    snprintf(g_dir, sizeof(g_dir), "%s/%s%ld.XXXXXX", SCRATCH_ROOT, SCRATCH_PREFIX, (long)getpid());
    if (!mkdtemp(g_dir)) { perror("mkdtemp " SCRATCH_ROOT); exit(2); }
    g_owner = getpid();
    atexit(cleanup);
    signal(SIGHUP, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGQUIT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGABRT, on_signal);
}

const char *tpath(const char *name) {
    static struct { char *name, *path; } seen[MAX_NAMES];
    static unsigned n;

    if (!g_dir[0]) make_dir();
    for (unsigned i = 0; i < n; i++)
        if (!strcmp(seen[i].name, name)) return seen[i].path;
    if (n == MAX_NAMES) { fprintf(stderr, "testdir: too many names\n"); exit(2); }
    seen[n].name = strdup(name);
    if (asprintf(&seen[n].path, "%s/%s", g_dir, name) < 0) exit(2);
    return seen[n++].path;
}

const char *mkstore_path(void) {
    const char *p = getenv("MKSTORE");
    if (p) return p;
    if (access("build/tools/mkstore", X_OK) == 0) return "build/tools/mkstore";
    if (access("tools/mkstore", X_OK) == 0) return "tools/mkstore";
    return "build/mkstore";
}
