/* rwyctl: commands that change the state of slots: init, confirm, fail, set, remove, recover-next. */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "guard_linux.h"
#include "rwyctl.h"

int exit_of(int rc) {
    switch (rc) {
    case RWY_ERR_PERM: case RWY_ERR_INVAL: return X_POLICY;
    case RWY_ERR_NOSPC: return X_NOSPC;
    default: return X_IO;
    }
}

/* Whole-file replace of a scalar, creating the file when it is absent. */
int put_scalar(const char *path, const char *val) {
    rwy_attr_t a;
    char line[4200];
    int n, rc;
    if (val[0] == '@') {
        n = slurp(val + 1, line, sizeof(line) - 1);
        if (n < 0) { warn("cannot read %s", val + 1); return RWY_ERR_IO; }
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
        line[n++] = '\n';
        line[n] = 0;
    } else {
        n = snprintf(line, sizeof(line), "%s\n", val);
        if (n >= (int)sizeof(line)) return RWY_ERR_INVAL;
    }
    if (rwy_stat(&g_store, path, &a) == RWY_ERR_NOENT && (rc = rwy_create(&g_store, path, RWY_S_IFREG | 0644, 0, 0))) return rc;
    return rwy_write_path(&g_store, path, 0, line, (size_t)n);
}

/* End of a writing command: commit one generation, or drop the work on error. */
int finish(int rc) {
    size_t i;
    if (rc) {
        rwy_abort(&g_store);
        warn("refused: %s", rwy_strerror(rc));
        return exit_of(rc);
    }
    if ((rc = rwy_commit(&g_store))) {   /* a failed commit has closed its transaction */
        warn("cannot commit: %s", rwy_strerror(rc));
        return exit_of(rc);
    }
    for (i = 0; i < g_mirror.num_devs; i++)
        if (!g_mirror.dev_faulted[i] && (g_mirror.write_errors[i] || g_mirror.flush_errors[i]))
            warn("warning: a member failed during the commit (%s)", "the other copies were written");
    return 0;
}

int begin_write(void) {
    if (!have_efi_runway()) { warn("%s", "EFI/RUNWAY is absent (native selection is off); create it with `rwyctl init`"); return X_IO; }
    return rwy_begin(&g_store) ? X_IO : 0;
}

/* begin_write for a command that raises the epoch. */
int begin_raise(void) {
    int rc = anchor_refuse();
    return rc ? rc : begin_write();
}

/* init: create the empty EFI/RUNWAY (the core stamps VERSION). Native selection stays off until a slot exists. */
int cmd_init(int argc) {
    rwy_attr_t a;
    int rc;
    if (argc) { warn("%s", "usage: init"); return X_USAGE; }
    if (have_efi_runway()) { warn("%s", "EFI/RUNWAY already exists"); return 0; }
    if (rwy_begin(&g_store)) return X_IO;
    rc = rwy_stat(&g_store, "EFI", &a) ? rwy_mkdir(&g_store, "EFI", RWY_S_IFDIR | 0755, 0, 0) : 0;
    if (!rc) rc = rwy_mkdir(&g_store, EFI_ROOT, RWY_S_IFDIR | 0755, 0, 0);
    return finish(rc);
}

/* Confirming the slot BOOTED names while the firmware started another one would confirm what did not run.
 * RUNWAY.EFI sets the volatile variable RunwayBoot to "<slot> recorded|unrecorded" before it starts a slot. */
static int confirm_ok(const char *name) {
    char booted[64], started[64];
    if (!guard_is_booted(&g_store, name, booted, sizeof(booted))) return 1;
    if (!guard_started(g_root, started, sizeof(started)) || !strcmp(started, booted)) return 1;
    fprintf(stderr, "rwyctl: %s did not start, %s did without being recorded\n", booted, started);
    return 0;
}

/* The health point: the slot that started is OK, so the boot loop counter goes. The confirm stays done. */
static int clear_loop_count(const char *name) {
    int rc = guard_healthy(&g_store, g_root, name);
    if (rc) warn("confirmed, but cannot clear the boot loop counter: %s", strerror(-rc));
    return rc ? X_IO : 0;
}

/* confirm / fail: STATE of NAME (default BOOTED). */
int cmd_state(const char *cmd, const char *state, int argc, char **argv) {
    char sp[PATH_MAX], p[PATH_MAX];
    const char *name = argc ? argv[0] : "BOOTED";
    int rc;
    if (argc > 1 || !slot_path(name, sp)) { warn("usage: %s [NAME]", cmd); return X_USAGE; }
    if (!strcmp(cmd, "confirm") && !confirm_ok(name)) return X_POLICY;
    snprintf(p, sizeof(p), "%s/STATE", sp);
    if ((rc = !strcmp(state, "FAILED") ? begin_raise() : begin_write())) return rc;
    rc = finish(put_scalar(p, state));
    return rc || strcmp(state, "OK") ? rc : clear_loop_count(name);
}

static const char *const global_set_fields[] = { "WATCHDOG", "SLOTS", "BOOT-DELAY", "QUOTA", "LOOP-GUARD", "LOOP-LIMIT", "LOOP-WINDOW", NULL };
static const char *const slot_set_fields[] = { "STATE", "TRIES", "PIN", "WATCHDOG", "ARGS", "LABEL", "ENTRY", "META", "REVISION", "INDEX", NULL };

/* The path of the field to set, or an exit code. */
static int set_path(const char *name, const char *field, char *p, size_t size) {
    char sp[PATH_MAX];
    if (!name) {
        if (!in_list(field, global_set_fields)) { warn("unknown global field %s", field); return X_USAGE; }
        snprintf(p, size, EFI_ROOT "/%s", field);
        return 0;
    }
    if (!slot_path(name, sp) || !strcmp(name, "BOOTED")) { warn("bad slot name %s", name); return X_USAGE; }
    if (!in_list(field, slot_set_fields)) { warn("unknown field %s", field); return X_USAGE; }
    snprintf(p, size, "%s/%s", sp, field);
    return 0;
}

/* SLOTS and QUOTA: "off" removes the file, since no file means no limit. */
static int clear_limit(const char *p) {
    rwy_attr_t a;
    int rc;
    if ((rc = begin_write())) return rc;
    rc = rwy_stat(&g_store, p, &a) == RWY_OK ? rwy_unlink(&g_store, p) : 0;
    return finish(rc);
}

static int is_limit_off(const char *name, const char *field, const char *val) {
    return !name && !strcmp(val, "off") && (!strcmp(field, "SLOTS") || !strcmp(field, "QUOTA"));
}

int cmd_set(int argc, char **argv) {
    char p[PATH_MAX];
    const char *name = 0, *field, *val;
    int rc;
    if (argc == 2) { field = argv[0]; val = argv[1]; }
    else if (argc == 3) { name = argv[0]; field = argv[1]; val = argv[2]; }
    else { warn("%s", "usage: set [NAME] FIELD VALUE"); return X_USAGE; }
    if ((rc = set_path(name, field, p, sizeof(p)))) return rc;
    if (is_limit_off(name, field, val)) return clear_limit(p);
    if ((rc = name && !strcmp(field, "STATE") && !strcmp(val, "FAILED") ? begin_raise() : begin_write())) return rc;
    rc = finish(put_scalar(p, val));
    return rc || !name || strcmp(field, "STATE") || strcmp(val, "OK") ? rc : clear_loop_count(name);
}

int cmd_remove(int argc, char **argv) {
    char p[PATH_MAX];
    rwy_attr_t a;
    int rc;
    if (argc != 1 || !(slot_name_ok(argv[0]) || !strcmp(argv[0], "RECOVERY"))) { warn("%s", "usage: remove NAME"); return X_USAGE; }
    snprintf(p, sizeof(p), EFI_ROOT "/%s", argv[0]);
    if ((rc = begin_raise())) return rc;
    if (rwy_stat(&g_store, p, &a)) { rwy_abort(&g_store); warn("%s does not exist", argv[0]); return X_IO; }
    return finish(remove_slot(p));
}

int cmd_recover_next(int argc, char **argv) {
    rwy_attr_t a;
    int rc, cancel = argc == 1 && !strcmp(argv[0], "--cancel"), have;
    if (argc > 1 || (argc == 1 && !cancel)) { warn("%s", "usage: recover-next [--cancel]"); return X_USAGE; }
    if ((rc = begin_write())) return rc;
    have = rwy_stat(&g_store, EFI_ROOT "/RECOVER_NEXT", &a) == RWY_OK;
    if (cancel) rc = have ? rwy_unlink(&g_store, EFI_ROOT "/RECOVER_NEXT") : 0;
    else {
        if (rwy_stat(&g_store, EFI_ROOT "/RECOVERY", &a)) warn("%s", "warning: RECOVERY is absent, the request will be ignored");
        rc = have ? 0 : rwy_create(&g_store, EFI_ROOT "/RECOVER_NEXT", RWY_S_IFREG | 0644, 0, 0);
    }
    return finish(rc);
}

