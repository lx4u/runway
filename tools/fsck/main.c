/*
 * fsck.runway — host-side consistency checker for Runway v8 stores.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Modes:
 *   fsck.runway [-n] STORE       read-only check. Never writes.
 *   fsck.runway -a STORE         conservative repair (also -p, -y, --repair):
 *                               if and only if the
 *                               store is consistent except for accumulated
 *                               marked-but-unreachable leftovers, publish
 *                               one GC-equivalent generation (the same
 *                               rwy_gc() machinery). Any real corruption
 *                               refuses with exit 4 and the store untouched.
 *   fsck.runway --defrag STORE   offline compaction: if and only if the
 *                               store is otherwise consistent, repack live
 *                               data so every object sits in a single
 *                               contiguous run (the rwy_defrag() machinery:
 *                               one combined GC+compaction pass, exactly one
 *                               new generation, identical visible contents).
 *                               Refuses with exit 4 on any corruption, and
 *                               publishes nothing when the store is already
 *                               compact (zero multi-extent objects and zero
 *                               leftovers).
 *
 * The plaintext recovery area is checked with or without a key: each copy is reported, a bad
 * copy next to a good one warns, and no good copy is an error. --member FILE (repeatable) names another
 * store of the same mirror; its bundle is compared with the one of STORE and a difference warns.
 *
 * --tpm DEV (or RWY_TPM) lets the check of an enrolled EFI/RUNWAY store read
 * the anti-rollback counter; it never increments it.
 *
 * Repair is leftover-only and never compacts; defrag is the explicit
 * on-demand compaction entry point (one combined pass, so fewer
 * generations and one crash-safety argument). Plain `check` never
 * writes under either mode.
 *
 * Exit codes (as in fsck(8)):
 *    0 = no errors. Plain check: no errors (leftovers may remain and are
 *        reported as notes; orphans as warnings). --defrag: compaction
 *        done or not needed.
 *    1 = errors corrected: --repair changed the store.
 *    4 = errors left uncorrected: corruption detected, or --repair/--defrag
 *        refused (store untouched).
 *    8 = operational error: cannot open, lock or unlock the store, or
 *        I/O failed during a fix (the store keeps a valid generation).
 *   16 = usage error.
 */
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "crypto_openssl.h"
#include "hostdev.h"

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [-a | -p | -y | -n | --repair | --defrag] [-f] [-k PASSPHRASE] [--tpm DEV] [--member FILE]... STORE\n"
            "  -a, -p, --repair  fix leftover blocks and the boot counter, nothing else (-y is the same)\n"
            "  -n                check only, never write (the default)\n"
            "  -f                accepted for fsck(8); the check is always complete\n"
            "  --defrag          offline compaction\n"
            "STORE and FILE are files, partitions (/dev/sda2 or PARTUUID=UUID), or IMAGE@OFFSET[+LENGTH] for a store inside an image\n", prog);
}

/* One --member file, which must be readable. */
static int add_member(check_ctx_t *c, const char *path) {
    char file[4096];
    uint64_t off, len;
    int win;
    if (c->nmember == 8 || hostdev_parse(path, file, sizeof(file), &off, &len, &win) || access(file, R_OK)) { fprintf(stderr, "cannot use member %s\n", path); return -1; }
    c->member[c->nmember++] = path;
    return 0;
}

enum { OPT_DEFRAG = 256, OPT_TPM, OPT_MEMBER };

static const struct option long_opts[] = {
    { "repair", no_argument, NULL, 'a' },
    { "defrag", no_argument, NULL, OPT_DEFRAG },
    { "key", required_argument, NULL, 'k' },
    { "tpm", required_argument, NULL, OPT_TPM },
    { "member", required_argument, NULL, OPT_MEMBER },
    { "help", no_argument, NULL, 'h' },
    { NULL, 0, NULL, 0 },
};

/* Takes the store, the environment defaults, and rejects conflicting modes. */
static int finish_args(check_ctx_t *c, opts_t *o, int argc, char **argv, int readonly) {
    if (optind + 1 != argc) { usage(argv[0]); return FSCK_USAGE; }
    o->store_path = argv[optind];
    if (!o->key_pass) o->key_pass = getenv("RWY_KEY");
    if (!c->tpm) c->tpm = getenv("RWY_TPM");
    if (o->repair + o->defrag + readonly > 1) {
        fprintf(stderr, "only one of -a/-p/-y/--repair, --defrag and -n may be given\n");
        usage(argv[0]);
        return FSCK_USAGE;
    }
    return 0;
}

/* Returns 0, or the exit code to use (FSCK_USAGE for usage errors and --help). */
static int parse_args(check_ctx_t *c, int argc, char **argv, opts_t *o) {
    int opt, readonly = 0;

    while ((opt = getopt_long(argc, argv, "apynfk:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'a': case 'p': case 'y': o->repair = 1; break;
        case 'n': readonly = 1; break;
        case 'f': break;
        case 'k': o->key_pass = optarg; break;
        case OPT_DEFRAG: o->defrag = 1; break;
        case OPT_TPM: c->tpm = optarg; break;
        case OPT_MEMBER: if (add_member(c, optarg)) return FSCK_ERROR; break;
        default: usage(argv[0]); return FSCK_USAGE;
        }
    }
    return finish_args(c, o, argc, argv, readonly);
}

static int cmd_check(check_ctx_t *c, const opts_t *o) {
    session_t ss;
    int rc = session_open(&ss, o->store_path, o->key_pass, 0, "");

    if (rc) return rc;
    if (ss.is_enc && !o->key_pass) {
        rc = run_unauthenticated_check(c, &ss.fl, &ss.dev);
    } else if (session_check(c, &ss, 0)) {
        rc = FSCK_UNCORRECTED;
    } else {
        rc = c->errors || c->cfg_counter || c->cfg_record ? FSCK_UNCORRECTED : FSCK_OK;
    }
    file_close(&ss.fl);
    return rc;
}

int main(int argc, char **argv) {
    check_ctx_t c = { 0 };
    opts_t o = { 0 };
    const char *e;
    int rc;

    rwy_crypto_host_init();
    rc = parse_args(&c, argc, argv, &o);
    if (rc) return rc;
    e = getenv("RWY_CHECK_FI_AT");
    if (e) file_set_fail_at((long)atol(e));
    if (o.repair) return cmd_repair(&c, &o);
    if (o.defrag) return cmd_defrag(&c, &o);
    return cmd_check(&c, &o);
}
