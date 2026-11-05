/*
 * rwyctl.c — target-side tool for EFI/RUNWAY, acting as the USER actor through the core
 * library, without FUSE: the options, the choice of the store and the dispatch to the commands.
 * SPDX-License-Identifier: Apache-2.0
 * Discovery, keys, the read commands, the write commands, install, check and anchor each have a
 * rwyctl_*.c file.
 * A writing command locks every member for its whole run and makes one commit.
 *
 * Usage: rwyctl [--store FILE]... [--device DEV]... [--uuid UUID] [--all-devices]
 *                [--passphrase-file F] [--dek-file F] [--tpm DEV] COMMAND ...
 * Keys, in order: --dek-file / --passphrase-file / RWY_KEY, TPM unseal (--tpm or RWY_TPM, a device or a unix
 * socket; default /dev/tpmrm0) when the store has a TPM keyslot, then a prompt on the terminal.
 *   stores | members | status | get [NAME] FIELD
 *     members prints the paths of the store chosen (boot store, else the only one), one per line.
 *   confirm [NAME] | fail [NAME] | set [NAME] FIELD VALUE      (NAME defaults to BOOTED for confirm/fail)
 *     confirm of the BOOTED slot is refused (exit 6) when the volatile variable RunwayBoot says RUNWAY.EFI started
 *     another slot without recording it.
 *     confirm or set STATE OK of the booted slot also deletes the RunwayBootGuard variable (exit 1 if it cannot).
 *   mirror clear | mirror forget UUID      (drop the mirror health counters, or a roster member; needs --store or --device)
 *   init                                   (create an empty EFI/RUNWAY; install refuses without it)
 *   remove NAME | recover-next [--cancel]
 *   recovery area set --kernel SRC [--initrd SRC] [--dtb SRC] [--args TEXT|@FILE] | recovery area show
 *     the plaintext recovery area: no key, so it works on any store including a locked store.
 *     set writes every member and needs --store or --device; a member it cannot write makes it fail (degraded).
 *   install [--name CONFIG<n>] --kernel SRC [--initrd SRC] [--dtb SRC] [--dedup] [--args TEXT|@FILE] [--label TEXT|@FILE] [--tries N] [--watchdog S]
 *     SRC is a file to copy or blob:NAME. Without --dedup each slot has its own copy of the data. With --dedup
 *     identical payloads are stored once in EFI/RUNWAY/BLOBS/ and linked. The slot appears finalized in one
 *     commit; on any error nothing is published. Prints the slot name and its revision.
 *   anchor status | enroll | reset         (anti-rollback counter in the TPM; encrypted stores only)
 *     enroll defines an NV counter for the store (index from the store UUID, policy PCR 7), raises it once
 *     and records its value as the epoch. reset does the same after removing a counter that is there, for
 *     a restored copy, a changed PCR 7 or a replaced TPM. Both need the store key and the TPM. --pcr7 HEX (64 digits)
 *     binds the policy to that PCR 7 value instead of the live one, for a device that is prepared offline.
 * When rwy-fuse holds the store, a writing command freezes it for its run (rwyctl_lease.c); init, check and reencrypt are refused.
 * RWYCTL_ROOT prefixes /proc, /sys and /dev so tests can use fake trees.
 * Exit codes: 0 done, 1 I/O or other error, 2 usage, 3 no store, 4 locked, 5 busy, 6 refused by the
 * layout policy, 7 no space, 8 check found violations.
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include "crypto_openssl.h"
#include "freeze_linux.h"
#include "rwyctl.h"
#include "rwyctl_cmd.h"
#include "rwyctl_rpc.h"

member_t g_mem[MAXG * MAXM];
int g_nmem;
group_t g_grp[MAXG];
int g_ngrp;
rwy_store_t g_store;
rwy_mirror_dev_t g_mirror;
const char *g_root = "";
char g_tmpmnt[8][PATH_MAX];   /* temporary mounts to undo at exit */
int g_ntmp;
int g_rw;   /* the command writes: members are opened read-write and locked */
int g_verbose;   /* -v: say why a partition was skipped */
char g_why[PATH_MAX + 128];   /* reason of the last failed add_member / temp_mount */

void rpath(char *out, const char *p) { snprintf(out, PATH_MAX, "%s%s", g_root, p); }

static void usage(FILE *f, const char *prog) {
    if (!prog) prog = "rwyctl";
    fprintf(f, "usage: %s [-v] [--io-stats] [--store FILE]... [--device DEV]... [--uuid U] [--all-devices] "
               "[--passphrase-file F] [--dek-file F] [--tpm DEV] COMMAND\n"
               "  stores | members | status | get [NAME] FIELD | slots [--label L] | confirm [NAME] | fail [NAME] | set [NAME] FIELD VALUE\n"
               "  | remove NAME | recover-next [--cancel] | put PATH SRC | cat PATH\n"
               "  | install --kernel SRC [--name N] [--initrd SRC] [--dtb SRC] [--dedup]\n"
               "    [--args TEXT|@FILE] [--label TEXT|@FILE] [--tries N] [--watchdog S]\n"
               "  | update --kernel SRC [--initrd SRC] [--dtb SRC] [--dedup] [--args TEXT|@FILE] [--label TEXT|@FILE] [--tries N] [--watchdog S]\n"
               "  | anchor status|enroll|reset | init | check [--blobs] [--repair --unlock] | recovery set --unlock --kernel SRC [--initrd SRC] [--dtb SRC] [--args TEXT|@FILE] [--label TEXT|@FILE]\n"
               "  | recovery area set --kernel SRC [--initrd SRC] [--dtb SRC] [--args TEXT|@FILE] | recovery area show\n"
               "  | loader show [--json] | loader set FILE [--wdt DIR] [--timeout N] [--tries N] | loader commit\n"
               "  | flight show [--raw] | log show [--raw] | log verify | log settings | log set NAME VALUE | log unset NAME | log copy-address IFNAME | log test\n"
               "  | wdt set --unlock SRC... | mirror clear | mirror forget UUID\n"
               "  | reencrypt [--old-key K] [--new-key K] [--old-passphrase P] [--new-passphrase P]\n"
               "    [--old-passphrase-file F] [--new-passphrase-file F] [--old-dek-file F] [--new-dek-file F] [--new-dek-out F]\n"
               "  -v says why a partition was skipped; --io-stats prints the I/O counters at the end; --help shows this text\n", prog);
}

typedef struct {
    const char *stores[MAXM], *devices[MAXM], *uuid;
    int ns, nd, all, help, cmd, io_stats;
} options_t;

static const char *const commands[] = {
    "stores", "members", "status", "get", "slots", "confirm", "fail", "set", "remove", "recover-next", "install",
    "update", "check", "recovery", "wdt", "init", "anchor", "reencrypt", "flight", "log", "put", "cat", "mirror",
    "loader",
};

static int is_command(const char *name) {
    size_t i;
    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        if (!strcmp(name, commands[i])) return 1;
    return 0;
}

/* Takes one global option. Returns 0, or an exit code. */
static int global_option(int c, options_t *o) {
    switch (c) {
    case 'v': g_verbose = 1; break;
    case 'h': o->help = 1; break;
    case 'I': o->io_stats = 1; break;
    case 'a': o->all = 1; break;
    case 's':
        if (o->ns >= MAXM) { warn("%s", "too many --store"); return X_USAGE; }
        o->stores[o->ns++] = optarg;
        break;
    case 'd':
        if (o->nd >= MAXM) { warn("%s", "too many --device"); return X_USAGE; }
        o->devices[o->nd++] = optarg;
        break;
    case 'u': o->uuid = optarg; break;
    case 'p': g_passfile = optarg; break;
    case 'k': g_dekfile = optarg; break;
    case 't': g_tpm = optarg; break;
    default: warn("%s", "bad or incomplete option"); return X_USAGE;
    }
    return 0;
}

/* Reads the global options and finds the command word. Returns 0 or an exit code. */
static int parse_options(int argc, char **argv, options_t *o) {
    static const struct option opts[] = {
        {"help", 0, 0, 'h'}, {"io-stats", 0, 0, 'I'}, {"store", 1, 0, 's'}, {"device", 1, 0, 'd'},
        {"uuid", 1, 0, 'u'}, {"all-devices", 0, 0, 'a'}, {"passphrase-file", 1, 0, 'p'},
        {"dek-file", 1, 0, 'k'}, {"tpm", 1, 0, 't'}, {0, 0, 0, 0}
    };
    int c, rc, i;

    opterr = 0;
    while ((c = getopt_long(argc, argv, "+vh", opts, NULL)) != -1) {
        if ((rc = global_option(c, o))) return rc;
        if (o->help) return 0;
    }
    i = optind;
    if (i == argc || !is_command(argv[i])) {
        const char *prog = argv[0] ? (strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0]) : "rwyctl";
        usage(stderr, prog);
        return X_USAGE;
    }
    o->cmd = i;
    return 0;
}

static int discover_from_stores(const options_t *o) {
    int i;
    for (i = 0; i < o->ns; i++) {
        int mi = add_member(o->stores[i]);
        if (mi < 0) { warn("warning: member %s missing or not a Runway store", o->stores[i]); continue; }
        add_to_group(mi, 0);
    }
    if (g_ngrp > 1) { warn("%s", "the --store files belong to different stores"); return X_USAGE; }
    return 0;
}

/* Mirror members, one partition each. */
static int discover_from_devices(const options_t *o) {
    int i;
    for (i = 0; i < o->nd; i++) {
        int mi = probe_partition(o->devices[i]);
        if (mi < 0) warn("warning: member %s missing or not a Runway store", o->devices[i]);
        else add_to_group(mi, 0);
    }
    if (g_ngrp > 1) { warn("%s", "the --device partitions belong to different stores"); return X_USAGE; }
    return 0;
}

/* Fills the group table from --store, --device or by looking at the disks. Returns 0 or an exit code. */
static int discover_stores(const options_t *o) {
    char part[64], disk[64];
    int have;
    if (o->ns) return discover_from_stores(o);
    if (o->nd) return discover_from_devices(o);
    have = boot_esp(part, disk);
    if (!have && !o->all && !o->uuid) warn("%s", "warning: boot ESP unknown, looking at every disk");
    if (have) read_listed_members();
    if (have && !o->all && !o->uuid) probe_disk(disk, part);
    else discover_all(have ? part : 0);
    probe_listed_members();
    check_listed_members();
    return 0;
}

static void set_old_key_arg(const char *k) {
    struct stat st;
    if (stat(k, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 32) g_dekfile = k;
    else if (stat(k, &st) == 0 && S_ISREG(st.st_mode)) g_passfile = k;
    else g_passstring = k;
}

static void check_reencrypt_key(int argc, char **argv) {
    int i;
    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--old-passphrase-file") && i + 1 < argc) g_passfile = argv[++i];
        else if (!strcmp(argv[i], "--old-dek-file") && i + 1 < argc) g_dekfile = argv[++i];
        else if (!strcmp(argv[i], "--old-passphrase") && i + 1 < argc) g_passstring = argv[++i];
        else if (!strcmp(argv[i], "--old-key") && i + 1 < argc) set_old_key_arg(argv[++i]);
    }
}

/* Index of the group the user asked for (--uuid) or the boot or only one; -1 when none. */
static int pick_group(const char *uuid, const uint8_t *want) {
    int i;
    for (i = 0; i < g_ngrp; i++)
        if (uuid ? !memcmp(g_grp[i].uuid, want, 16) : (g_grp[i].boot || g_ngrp == 1)) return i;
    return -1;
}

/* The recovery area is plaintext: its commands run before, and without, unlocking the store. */
static int is_recovery_area(int argc, char **argv) {
    return argc >= 2 && !strcmp(argv[0], "recovery") && !strcmp(argv[1], "area");
}

/* The flight recorder area is plaintext: reads without unlocking the store. */
static int is_flight(int argc, char **argv) {
    return argc >= 1 && !strcmp(argv[0], "flight");
}

/* The network flight report settings are plaintext: show, set, unset, copy-address and test need no key. */
static int is_log_settings(int argc, char **argv) {
    return argc >= 2 && !strcmp(argv[0], "log") &&
           (!strcmp(argv[1], "settings") || !strcmp(argv[1], "set") || !strcmp(argv[1], "unset") ||
            !strcmp(argv[1], "copy-address") || !strcmp(argv[1], "test"));
}

/* Commands on plaintext areas: they need no key. Returns 1 and sets *rc when one ran. */
static int run_plain_area(group_t *g, int argc, char **argv, int *rc) {
    if (is_recovery_area(argc, argv)) *rc = cmd_recovery_area(g, argc - 2, argv + 2);
    else if (!strcmp(argv[0], "loader")) *rc = cmd_loader(g, argc - 1, argv + 1);
    else if (is_flight(argc, argv)) *rc = cmd_flight(g, argc - 1, argv + 1);
    else if (is_log_settings(argc, argv)) *rc = cmd_log_settings(g, argc - 1, argv + 1);
    else return 0;
    return 1;
}

static int run_on_store(const options_t *o, int argc, char **argv, const uint8_t *want) {
    int g, rc;
    if (!strcmp(argv[o->cmd], "stores")) return cmd_stores();
    g = pick_group(o->uuid, want);
    if (g < 0) {
        warn("%s", g_ngrp ? "several stores found and none chosen (see `stores`, use --uuid)" : "no Runway store found (use -v to see why a partition was skipped)");
        return X_NOSTORE;
    }
    if (o->uuid && !g_grp[g].boot && !o->ns && !o->nd)
        warn("%s", "note: this is not the boot store, RUNWAY.EFI will not boot from it");
    if (!strcmp(argv[o->cmd], "members")) return cmd_members(&g_grp[g]);
    if (run_plain_area(&g_grp[g], argc - o->cmd, argv + o->cmd, &rc)) return rc;

    /* The daemon has the key, but a client still has to show one. */
    if (rwyctl_rpc_available(g_grp[g].uuid) && (rc = unlock_group(&g_grp[g]))) return rc;
    rc = rwyctl_rpc_client(g_grp[g].uuid, argc - o->cmd, argv + o->cmd);
    if (rc >= 0) return rc;

    if (!strcmp(argv[o->cmd], "reencrypt")) check_reencrypt_key(argc - o->cmd - 1, argv + o->cmd + 1);
    if ((rc = open_group(&g_grp[g]))) return rc;
    rc = rwyctl_exec_command(argc - o->cmd, argv + o->cmd);
    return rc;
}

/* Only the two word commands `flight show` and `log show|verify` take a store after them. For any other command that
 * word is an argument of the command. */
static void add_positional_target(options_t *o, int argc, char **argv) {
    if (strcmp(argv[o->cmd], "flight")) {
        if (strcmp(argv[o->cmd], "log") || o->cmd + 1 >= argc ||
            (strcmp(argv[o->cmd + 1], "show") && strcmp(argv[o->cmd + 1], "verify")))
            return;
    }
    if (!o->ns && !o->nd && !o->uuid && !o->all && o->cmd + 2 < argc && argv[o->cmd + 2][0] != '-') {
        struct stat st;
        if (stat(argv[o->cmd + 2], &st) == 0 && (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)))
            o->devices[o->nd++] = argv[o->cmd + 2];
        else
            o->stores[o->ns++] = argv[o->cmd + 2];
    }
}

/* Calls of the core to the store, and of the opened devices to the kernel, on standard error. */
static void print_io_stats(void) {
    rwy_dev_stats_t dev = { 0 };
    char text[1024];
    int i;

    for (i = 0; i < g_nmem; i++) rwy_dev_stats_add(&dev, &g_mem[i].hd.stats);
    rwy_io_format(text, sizeof(text), &g_store.io, &dev);
    fputs(text, stderr);
}

int main(int argc, char **argv) {
    options_t o = { 0 };
    uint8_t want[16];
    int i, rc;
#ifndef RWYCTL_BUILTIN_CRYPTO
    rwy_crypto_host_init();
#endif
    rwy_crypto_set_random(host_random);
    if (getenv("RWYCTL_ROOT")) g_root = getenv("RWYCTL_ROOT");
    hostdev_root = g_root;
    if ((rc = parse_options(argc, argv, &o))) return rc;
    if (o.help) {
        const char *prog = argv[0] ? (strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0]) : "rwyctl";
        usage(stdout, prog);
        return X_OK_;
    }
    add_positional_target(&o, argc, argv);
    g_rw = command_writes(argc, argv, o.cmd);
    g_explicit = o.ns || o.nd;
    if (o.uuid && rwy_parse_uuid(o.uuid, want)) { warn("bad UUID %s", o.uuid); return X_USAGE; }
    if ((rc = discover_stores(&o))) return rc;
    rc = run_on_store(&o, argc, argv, want);
    if (o.io_stats) print_io_stats();
    for (i = 0; i < g_ntmp; i++) { umount(g_tmpmnt[i]); rmdir(g_tmpmnt[i]); }
    return rc;
}
