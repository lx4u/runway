/*
 * rwy_fuse_store — the store behind the mount of rwy-fuse: the members of the mirror and the locks on them,
 * the key, the mount of the newest copy and the anchor.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "rwy_block.h"
#include "rwy_core.h"
#include "rwy_crypto.h"
#include "rwy_format.h"
#include "hostdev.h"
#include "anchor_linux.h"
#include "rwyctl_open.h"
#include "rwy_fuse_store.h"

/* One mirror member: its file, the lock on it and the (possibly decrypting) device. */
typedef struct {
    hostdev_t lf;
    const char *path;
    rwy_block_dev_t raw, dev;
    rwy_crypto_dev_ctx_t cctx;
    uint64_t gen;
    int ok;
} member_t;
static member_t g_mem[RWY_MIRROR_MAX_DEVS];
rwy_mirror_dev_t g_mirror;
static rwy_block_dev_t g_recdevs[RWY_MIRROR_MAX_DEVS];   /* the members in the mirror, as the store sees them */
static size_t g_nrecdevs;
static unsigned g_lagging;   /* bit i: mirror member i is left out because it lags */
static char g_specs[RWY_MIRROR_MAX_DEVS][PATH_MAX];   /* what the first open was given, made absolute */
static int g_nspecs;
static int g_open;   /* the members are open and the store is mounted */
static int g_first_open; /* set until the first open has mounted: only it notes member health */

static void warn_member(const char *path, const char *why) {
    fprintf(stderr, "rwy-fuse: warning: %s %s, treated as missing\n", path, why);
}

static uint8_t g_saved_dek[32];   /* kept across a close, so a reopen needs no passphrase or TPM */
static int g_have_saved_dek;

/*
 * Open and lock one member and unlock it with the key. 0 is done, -1 is a member to treat as
 * missing, -2 is a member that exists but cannot be opened or locked: the caller must abort,
 * since writing the other copies could leave them ahead of it.
 */
static int open_member(member_t *m, const char *path, const char *key_pass) {
    rwy_keyslot_header_t khdr;
    int krc;
    m->path = path;
    if (hostdev_open(&m->lf, path, O_RDWR)) {
        if (errno == ENOENT) { warn_member(path, "does not exist"); return -1; }
        fprintf(stderr, "rwy-fuse: %s cannot be opened: %s\n", path, strerror(errno));
        return -2;
    }
    if (flock(m->lf.fd, LOCK_EX | LOCK_NB)) {
        fprintf(stderr, "rwy-fuse: %s is busy or cannot be locked (another rwy-fuse or rwyctl holds it)\n", path);
        hostdev_close(&m->lf);
        return -2;
    }
    hostdev_dev(&m->lf, &m->raw);
    if (!m->raw.size) m->raw.size = RWY_STORE_SIZE;

    krc = rwy_keyslot_read_from_dev(&m->raw, &khdr);
    if (krc == RWY_CRYPTO_ERR_EMPTY) { m->dev = m->raw; return 0; }
    if (krc != RWY_CRYPTO_OK) { warn_member(path, "has a corrupt or missing keyslot header"); goto fail; }
    {
        uint8_t dek[32];
        if (g_have_saved_dek) {
            memcpy(dek, g_saved_dek, 32);
        } else if (rwy_resolve_dev_key(&m->raw, key_pass, dek) == 0) {
            memcpy(g_saved_dek, dek, 32);
            g_have_saved_dek = 1;
        } else {
            warn_member(path, "cannot be unlocked (rejects key or unseal failed)");
            goto fail;
        }
        if (rwy_crypto_dev_wrap(&m->dev, &m->cctx, &m->raw, dek, RWY_SB_BLOCKS)) {
            warn_member(path, "cannot set up encryption");
            goto fail;
        }
    }
    return 0;
fail:
    hostdev_close(&m->lf);
    return -1;
}

/* Members that are copies of one another get their own UUIDs. */
static void name_members(void) {
    if (g_mirror.num_devs > 1 && rwy_mirror_name_members(&g_mirror, rwy_random))
        fprintf(stderr, "rwy-fuse: warning: a mirror member could not be given its UUID\n");
}

/* Find the newest member to serve as primary; returns primary index or -1. */
static int find_primary(rwy_store_t *s, int n, uint64_t *best_gen) {
    uint64_t best = 0;
    int i, prim = -1;
    for (i = 0; i < n; i++) {   /* the generation under the key, so a wrong key is not a member */
        member_t *m = &g_mem[i];
        if (!m->ok) continue;
        if (rwy_mount(s, &m->dev) != RWY_OK) {
            warn_member(m->path, "cannot be mounted");
            m->ok = 0;
            continue;
        }
        m->gen = s->sb.generation;
        if (prim < 0 || m->gen > best) { best = m->gen; prim = i; }
    }
    *best_gen = best;
    return prim;
}

/* The newest state record may sit on any member: take it. At the first open only, count the members left out
 * because they lag and the ones that are gone; a reopen would count them again. */
static void note_lagging(rwy_store_t *s) {
    size_t w = 0;
    if (rwy_record_sync(g_recdevs, g_nrecdevs, s->sb.rec_start, 0, &w) || rwy_record_adopt(s, &g_recdevs[w]))
        return;
    if (!g_first_open) return;
    for (size_t i = 0; i < g_mirror.num_devs; i++) {
        uint8_t uuid[16];
        if ((g_lagging & (1u << i)) && rwy_block_member_uuid(&g_mirror.devs[i], uuid))
            (void)rwy_record_note_health(s, g_recdevs, g_nrecdevs, uuid, RWY_SS_EV_LAGGING);
    }
    (void)rwy_mirror_note_members(s, &g_mirror, g_recdevs, g_nrecdevs);
}

/* Assemble the mirror with primary, lagging, and foreign key members. */
static int assemble_mirror(rwy_store_t *s, int n, uint64_t best, int prim, int named) {
    int i, used = 0;
    rwy_block_dev_t dev;
    rwy_mirror_init(&g_mirror);
    for (i = 0; i < n; i++) {
        member_t *m = &g_mem[i];
        if (m->ok && memcmp(m->cctx.dek, g_mem[prim].cctx.dek, 32)) {
            warn_member(m->path, "has another data key");
            m->ok = 0;
        }
        if (!m->ok) continue;
        rwy_mirror_add(&g_mirror, &m->dev);
        g_recdevs[g_nrecdevs++] = m->dev;
        if (m->gen < best) {
            fprintf(stderr, "rwy-fuse: warning: %s is lagging (generation %llu, newest %llu), not written\n",
                    m->path, (unsigned long long)m->gen, (unsigned long long)best);
            g_lagging |= 1u << (g_mirror.num_devs - 1);
            rwy_mirror_set_faulted(&g_mirror, g_mirror.num_devs - 1, 1);
        } else {
            used++;
        }
        if (i == prim) rwy_mirror_set_primary(&g_mirror, g_mirror.num_devs - 1);
    }
    if (used < named)
        fprintf(stderr, "rwy-fuse: warning: running degraded, %d of %d members are written\n", used, named);
    name_members();
    rwy_mirror_wrap(&dev, &g_mirror);
    if (rwy_mount(s, &dev) != RWY_OK) return -1;
    note_lagging(s);
    return 0;
}

/* Mount the members that opened: the newest is primary, lagging ones are faulted, foreign data keys are dropped. */
static int mount_members(rwy_store_t *s, int n, int named) {
    uint64_t best = 0;
    int prim = find_primary(s, n, &best);
    if (prim < 0) { fprintf(stderr, "rwy-fuse: no member of the store can be mounted\n"); return -1; }
    return assemble_mirror(s, n, best, prim, named);
}

/* Open and lock every named store; returns how many opened, or -1 after printing why not. */
static int open_members(const char **pos, int npos, const char *key_pass) {
    int i, rc, nmem = 0;
    int failed[RWY_MIRROR_MAX_DEVS], nfailed = 0;

    for (i = 0; i < npos; i++) {
        rc = open_member(&g_mem[nmem], pos[i], key_pass);
        if (rc == -2) {
            fprintf(stderr, "rwy-fuse: refusing to mount without all existing members locked\n");
            return -1;
        }
        if (rc == 0) g_mem[nmem++].ok = 1;
        else failed[nfailed++] = i;
    }
    if (g_have_saved_dek && nfailed > 0) {
        for (i = 0; i < nfailed; i++) {
            rc = open_member(&g_mem[nmem], pos[failed[i]], key_pass);
            if (rc == 0) g_mem[nmem++].ok = 1;
        }
    }
    if (!nmem) {
        fprintf(stderr, "rwy-fuse: no store could be opened and locked\n");
        return -1;
    }
    return nmem;
}

/* Attach the counter so commits raise the epoch. Without it nothing is evicted; the mount still serves. */
static void attach_anchor(rwy_store_t *s) {
    int r = anchor_gate(s);
    if (r < 0) fprintf(stderr, "rwy-fuse: warning: the anchor needs the TPM %s, eviction is off\n", tpm_dev());
    else if (r) fprintf(stderr, "rwy-fuse: warning: the anchor says %s, eviction is off\n", anchor_word(r));
}

/* Close every member (this drops its lock) and forget the mirror. */
static void release_members(void) {
    for (size_t i = 0; i < RWY_MIRROR_MAX_DEVS; i++)
        if (g_mem[i].path) hostdev_close(&g_mem[i].lf);
    memset(g_mem, 0, sizeof(g_mem));
    memset(&g_mirror, 0, sizeof(g_mirror));
    memset(g_recdevs, 0, sizeof(g_recdevs));
    g_nrecdevs = 0;
    g_lagging = 0;
}

/* Open the members named in g_specs and mount the newest copy. */
static int session_start(rwy_store_t *s, const char *key_pass) {
    const char *pos[RWY_MIRROR_MAX_DEVS];
    int nmem;
    for (int i = 0; i < g_nspecs; i++) pos[i] = g_specs[i];
    nmem = open_members(pos, g_nspecs, key_pass);
    if (nmem < 0 || mount_members(s, nmem, g_nspecs)) {
        release_members();
        return -1;
    }
    g_first_open = 0;
    attach_anchor(s);
    g_open = 1;
    return 0;
}

/* Put the file part of a spec (the part before the last '@' unless the whole spec is a file) in absolute form. */
static int absolute_spec(const char *spec, char *out) {
    const char *at = strrchr(spec, '@');
    size_t flen = !at || !access(spec, F_OK) ? strlen(spec) : (size_t)(at - spec);
    char file[PATH_MAX], real[PATH_MAX];
    if (flen >= sizeof(file)) return -1;
    memcpy(file, spec, flen);
    file[flen] = 0;
    if (file[0] == '/' || !strncmp(file, "PARTUUID=", 9) || !realpath(file, real)) strcpy(real, file);
    return snprintf(out, PATH_MAX, "%s%s", real, spec + flen) < PATH_MAX ? 0 : -1;
}

int rwy_fuse_store_open(rwy_store_t *s, const char **specs, int n, const char *key_pass) {
    if (n < 1 || n > RWY_MIRROR_MAX_DEVS) return fprintf(stderr, "rwy-fuse: bad number of stores\n"), -1;
    for (int i = 0; i < n; i++)
        if (absolute_spec(specs[i], g_specs[i])) return fprintf(stderr, "rwy-fuse: %s is too long\n", specs[i]), -1;
    g_nspecs = n;
    g_have_saved_dek = 0;
    g_first_open = 1;
    return session_start(s, key_pass);
}

void rwy_fuse_store_close(rwy_store_t *s) {
    if (!g_open) return;
    rwy_fuse_store_note_failures(s);
    rwy_anchor_attach(s, NULL);
    anchor_release();
    release_members();
    g_open = 0;
}

int rwy_fuse_store_reopen(rwy_store_t *s) {
    if (g_open || !g_nspecs) return -1;
    return session_start(s, NULL);
}

void rwy_fuse_store_note_failures(rwy_store_t *s) {
    if (g_open) (void)rwy_mirror_note_failures(s, &g_mirror, g_recdevs, g_nrecdevs);
}
