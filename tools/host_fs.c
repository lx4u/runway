/*
 * host_fs.c — scripted edits of a Runway store from the host, one commit per command,
 * as the USER actor (what an updater over FUSE would do). With HOST_FS_SYSTEM=1 in the
 * environment it acts as SYSTEM with RECOVERY/ unlocked (what mkstore does offline).
 * SPDX-License-Identifier: Apache-2.0
 * HOST_FS_PRIV=1 also sets the core-internal privileged flag (fixtures, old images).
 * HOST_FS_PASSPHRASE=P opens an encrypted store with the passphrase of keyslot 0
 * (`mkstore --encrypt` uses "runway-test-passphrase" unless given --passphrase).
 * HOST_FS_DEK_FILE=F opens it with the raw data key (`mkstore --dek-out`), for a store whose
 * keyslot 0 was cleared by TPM enrolment.
 * STORE is a file or IMAGE@OFFSET[+LENGTH], a store inside an image.
 * Usage: host_fs STORE mkdir PATH | put PATH TEXT|@FILE | symlink PATH TARGET | link OLD NEW
 *                     | rm PATH | cat PATH | booted NAME|-
 * `booted` sets (or clears with "-") the slot BOOTED names, which only selection writes (fixture).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_crypto.h"
#include "crypto_openssl.h"
#include "rwy_format.h"
#include "hostdev.h"
void rwy_bootcfg_set_priv(rwy_store_t *s, int on);   /* src/rwy_internal.h; not public API */
int rwy_ss_set_booted(uint8_t *r, const char *name);

static char *slurp(const char *arg, size_t *n) {
    char *b;
    FILE *f;
    if (arg[0] != '@') { *n = strlen(arg); return (char *)arg; }
    f = fopen(arg + 1, "rb");
    if (!f) { perror(arg + 1); exit(1); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); rewind(f);
    b = malloc(*n ? *n : 1);
    if (!b || fread(b, 1, *n, f) != *n) { fprintf(stderr, "read %s\n", arg + 1); exit(1); }
    fclose(f);
    return b;
}

/* Get the data key from HOST_FS_DEK_FILE or HOST_FS_PASSPHRASE. Returns 1 when the store is plain, else 0 or an exit code. */
static int get_dek(rwy_block_dev_t *dev, uint8_t dek[32]) {
    const char *file = getenv("HOST_FS_DEK_FILE"), *pass = getenv("HOST_FS_PASSPHRASE");
    rwy_keyslot_header_t khdr;
    FILE *k;

    if (file) {
        k = fopen(file, "rb");
        if (!k || fread(dek, 1, 32, k) != 32) { fprintf(stderr, "host_fs: cannot read %s\n", file); return 2; }
        fclose(k);
        return 0;
    }
    if (!pass) return -1;
    return rwy_keyslot_read_from_dev(dev, &khdr) || rwy_keyslot_unwrap(&khdr, pass, dek) ? 2 : 0;
}

/* Wrap the store as a block device, decrypted when a key is given. Returns 0 or an exit code. */
static int open_dev(hostdev_t *hd, rwy_block_dev_t *dev) {
    static rwy_crypto_dev_ctx_t cctx;
    rwy_block_dev_t cdev;
    uint8_t dek[32];
    int rc;

    hostdev_dev(hd, dev);
    rc = get_dek(dev, dek);
    if (rc < 0) return 0;
    if (rc) return rc;
    if (rwy_crypto_dev_wrap(&cdev, &cctx, dev, dek, RWY_SB_BLOCKS)) return 1;
    *dev = cdev;
    return 0;
}

static int cmd_cat(rwy_store_t *s, const char *path) {
    static char buf[1 << 20];
    uint64_t off = 0;
    size_t n;
    int rc;
    do {
        n = sizeof(buf);
        rc = rwy_read_path(s, path, off, buf, &n);
        if (!rc) fwrite(buf, 1, n, stdout);
        off += n;
    } while (!rc && n == sizeof(buf));
    return rc ? 1 : 0;
}

/* Print "mtime ctime" in seconds. */
static int cmd_times(rwy_store_t *s, const char *path) {
    rwy_attr_t a;
    if (rwy_stat(s, path, &a)) return 1;
    printf("%lld %lld\n", (long long)a.mtime_sec, (long long)a.ctime_sec);
    return 0;
}

static int cmd_put(rwy_store_t *s, const char *path, const char *arg) {
    size_t n;
    uint64_t o;
    char *d = slurp(arg, &n);
    int rc = rwy_lookup(s, path, &o) == RWY_OK ? RWY_OK : rwy_create(s, path, RWY_S_IFREG | 0644, 0, 0);
    return rc ? rc : rwy_write_path(s, path, 0, d, n);
}

static int cmd_booted(rwy_store_t *s, const char *slot) {
    uint8_t r[RWY_REC_PAYLOAD];
    int rc = rwy_record_read(s, r);
    if (!rc) rc = rwy_ss_set_booted(r, strcmp(slot, "-") ? slot : NULL);
    return rc ? rc : rwy_record_write(s, r);
}

/* Print the mirror health counters the record holds for a member UUID; "none" if it has no entry. */
static int cmd_health(rwy_store_t *s, const char *uuid_text) {
    uint8_t r[RWY_REC_PAYLOAD], uuid[16];
    rwy_ss_health_t h;
    if (rwy_parse_uuid(uuid_text, uuid) || rwy_record_read(s, r)) return 1;
    if (!rwy_ss_health_get(r, uuid, &h)) { puts("none"); return 0; }
    printf("lagging=%u sync_failed=%u write_failed=%u missing=%u\n", h.count[RWY_SS_EV_LAGGING],
           h.count[RWY_SS_EV_SYNC_FAILED], h.count[RWY_SS_EV_WRITE_FAILED], h.count[RWY_SS_EV_MISSING]);
    return 0;
}

/* Run a command that changes the store; -1 for an unknown command. */
static int run_mutation(rwy_store_t *s, const char *cmd, int argc, char **argv) {
    if (!strcmp(cmd, "mkdir")) return rwy_mkdir(s, argv[3], RWY_S_IFDIR | 0755, 0, 0);
    if (!strcmp(cmd, "rm")) return rwy_unlink(s, argv[3]);
    if (!strcmp(cmd, "symlink") && argc > 4) return rwy_symlink(s, argv[3], argv[4], 0, 0);
    if (!strcmp(cmd, "link") && argc > 4) return rwy_link(s, argv[3], argv[4]);
    if (!strcmp(cmd, "put") && argc > 4) return cmd_put(s, argv[3], argv[4]);
    if (!strcmp(cmd, "booted") && argc > 3) return cmd_booted(s, argv[3]);
    return -1;
}

int main(int argc, char **argv) {
    static rwy_store_t s;
    rwy_block_dev_t dev;
    const char *cmd;
    hostdev_t hd;
    int rc;
    rwy_crypto_host_init();
    if (argc < 4) { fprintf(stderr, "usage: host_fs STORE CMD ARGS...\n"); return 2; }
    if (hostdev_open(&hd, argv[1], O_RDWR)) { perror(argv[1]); return 1; }
    rc = open_dev(&hd, &dev);
    if (rc) return rc;
    if (rwy_mount(&s, &dev)) { fprintf(stderr, "mount failed\n"); return 1; }
    if (getenv("HOST_FS_SYSTEM")) {
        rwy_set_actor(&s, RWY_ACTOR_SYSTEM);
        rwy_bootcfg_unlock_recovery(&s, 1);
    }
    if (getenv("HOST_FS_PRIV")) rwy_bootcfg_set_priv(&s, 1);   /* fixtures the layout rules refuse (more than 64 slots) */
    cmd = argv[2];
    if (!strcmp(cmd, "cat")) return cmd_cat(&s, argv[3]);
    if (!strcmp(cmd, "health")) return cmd_health(&s, argv[3]);
    if (!strcmp(cmd, "times")) return cmd_times(&s, argv[3]);
    if ((rc = rwy_begin(&s))) return 1;
    rc = run_mutation(&s, cmd, argc, argv);
    if (rc == -1) { fprintf(stderr, "bad command\n"); return 2; }
    if (rc) { fprintf(stderr, "host_fs %s %s: %s\n", cmd, argv[3], rwy_strerror(rc)); return 1; }
    if ((rc = rwy_commit(&s))) { fprintf(stderr, "commit: %s\n", rwy_strerror(rc)); return 1; }
    hostdev_close(&hd);
    return 0;
}
