/* tools/rwyctl_cmd.c: command execution dispatch shared between rwyctl and rwy-fuse. */
/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <string.h>
#include "rwyctl.h"
#include "rwyctl_cmd.h"

int g_explicit;

__attribute__((weak)) int store_commit_active(void) {
    return 0;
}

void warn(const char *fmt, const char *a) {
    fprintf(stderr, "rwyctl: ");
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
}

int system_allowed(void) {
    if (!g_explicit) {
        warn("%s", "--unlock needs --store or --device (not a store found on the running system)");
        return X_POLICY;
    }
    return 0;
}

int has_arg(int argc, char **argv, const char *a) {
    while (argc-- > 0) if (!strcmp(*argv++, a)) return 1;
    return 0;
}

int command_writes(int argc, char **argv, int cmd) {
    const char *name = argv[cmd];
    int has_repair = has_arg(argc - cmd - 1, argv + cmd + 1, "--repair");
    int has_status = has_arg(argc - cmd - 1, argv + cmd + 1, "status");
    int has_show = has_arg(argc - cmd - 1, argv + cmd + 1, "show");

    if (!strcmp(name, "stores") || !strcmp(name, "members") || !strcmp(name, "status")) return 0;
    if (!strcmp(name, "get") || !strcmp(name, "slots") || !strcmp(name, "cat")) return 0;
    if (!strcmp(name, "flight") || !strcmp(name, "log")) return 0;
    if (!strcmp(name, "check")) return has_repair;
    if (!strcmp(name, "anchor")) return !has_status;
    if (!strcmp(name, "loader")) return !has_show;
    return 1;
}

static int run_read_cmd(const char *name, int argc, char **argv) {
    if (!strcmp(name, "status")) return cmd_status(argc, argv);
    if (!strcmp(name, "get")) return cmd_get(argc, argv);
    if (!strcmp(name, "slots")) return cmd_slots(argc, argv);
    if (!strcmp(name, "cat")) return cmd_cat(argc, argv);
    if (!strcmp(name, "check")) return cmd_check(argc, argv);
    if (!strcmp(name, "log")) return cmd_log(argc, argv);
    if (!strcmp(name, "mirror")) return cmd_mirror(argc, argv);
    return -1;
}

static int run_write_slot(const char *name, int argc, char **argv) {
    if (!strcmp(name, "confirm")) return cmd_state("confirm", "OK", argc, argv);
    if (!strcmp(name, "fail")) return cmd_state("fail", "FAILED", argc, argv);
    if (!strcmp(name, "set")) return cmd_set(argc, argv);
    if (!strcmp(name, "remove")) return cmd_remove(argc, argv);
    if (!strcmp(name, "put")) return cmd_put(argc, argv);
    if (!strcmp(name, "install")) return cmd_install(argc, argv);
    if (!strcmp(name, "update")) return cmd_update(argc, argv);
    if (!strcmp(name, "recover-next")) return cmd_recover_next(argc, argv);
    return -1;
}

__attribute__((weak)) int cmd_reencrypt(int argc, char **argv) {
    (void)argc; (void)argv;
    warn("%s", "store busy: it is mounted by rwy-fuse, unmount it first");
    return X_BUSY;
}

static int run_write_cmd(const char *name, int argc, char **argv) {
    int rc = run_write_slot(name, argc, argv);
    if (rc >= 0) return rc;
    if (!strcmp(name, "recovery")) return cmd_recovery(argc, argv);
    if (!strcmp(name, "wdt")) return cmd_wdt(argc, argv);
    if (!strcmp(name, "anchor")) return cmd_anchor(argc, argv);
    if (!strcmp(name, "init")) return cmd_init(argc);
    if (!strcmp(name, "reencrypt")) return cmd_reencrypt(argc, argv);
    return -1;
}

int rwyctl_exec_command(int argc, char **argv) {
    int rc;

    if (argc < 1) return X_USAGE;
    g_rw = command_writes(argc, argv, 0);
    rc = run_read_cmd(argv[0], argc - 1, argv + 1);
    if (rc < 0) rc = run_write_cmd(argv[0], argc - 1, argv + 1);
    if (rc < 0) return X_USAGE;
    if (g_rw) note_write_failures();
    return rc;
}
