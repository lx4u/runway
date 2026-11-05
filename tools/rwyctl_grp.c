/* rwyctl: opening and locking the store group (unlock, lock, mirror). */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>
#include "rwyctl.h"
#include "rwyctl_open.h"

rwy_block_dev_t g_raw_dev;
rwy_crypto_dev_ctx_t g_crypto_ctx;
rwy_block_dev_t g_crypto_dev;
uint8_t g_active_dek[32];
int g_is_enc;

/* Unlock every member with the key; returns how many accepted it. */
static int unlock_members(group_t *g, int have, const char *src, uint8_t dek[32]) {
    int i, any = 0;
    for (i = 0; i < g->n; i++) {
        member_t *m = &g_mem[g->m[i]];
        if (!have && pass_dek_dev(&m->raw, src, dek)) { m->ok = 0; continue; }
        m->ok = rwy_crypto_dev_wrap(&m->dev, &m->cctx, &m->raw, dek, RWY_SB_BLOCKS) == 0;
        any |= m->ok;
    }
    return any;
}

/* The key of the first member whose TPM unseals it; 0 when none does. */
static int group_tpm_dek(group_t *g, uint8_t dek[32]) {
    int i;
    for (i = 0; i < g->n; i++)
        if (tpm_dek_dev(&g_mem[g->m[i]].raw, dek) == 0) return 1;
    return 0;
}


/* Keys for the whole group; returns 0 or X_LOCKED. Members whose key differs are dropped (ok = 0). */
int unlock_group(group_t *g) {
    uint8_t dek[32];
    char pass[256];
    int tries, any = 0, rc;
    const char *file = g_dekfile ? g_dekfile : getenv("RWY_DEK_FILE");
    const char *key = getenv("RWY_KEY");
    const char *pf = g_passfile;
    int tpm = 0;
    if (!g_mem[g->m[0]].enc) return 0;
    if (!file && !pf && !key) tpm = group_tpm_dek(g, dek);
    for (tries = 0; tries < 3; tries++) {
        const char *src;
        int have;
        rc = key_source(tries, tpm, file, pf, dek, pass, &src, &have);
        if (rc) return rc;
        any = unlock_members(g, have, src, dek);
        if (any || tpm || file || pf || key) break;   /* only the prompt retries */
    }
    if (!any) { warn("%s", "authentication failed (incorrect key/passphrase)"); return X_LOCKED; }
    return 0;
}

/* Lock one member; return X_BUSY if held. Returns 0, an X_ code to give, or -1 to skip the member. */
static int lock_member(const hostdev_t *nh) {
    if (!flock(nh->fd, LOCK_EX | LOCK_NB)) return 0;
    if (errno == EWOULDBLOCK) {
        warn("%s", "store busy (another rwyctl holds it)");
        return X_BUSY;
    }
    return -1;
}

/* Reopen the members read-write and take their lock; one busy member is X_BUSY for the whole command. */
int lock_group(group_t *g) {
    int i, rc, any = 0;
    for (i = 0; i < g->n; i++) {
        member_t *m = &g_mem[g->m[i]];
        hostdev_t nh;
        if (!m->ok) continue;
        if (hostdev_open(&nh, m->path, O_RDWR) && errno == EROFS && !m->hd.block) {   /* a temporary read-only mount */
            char dir[PATH_MAX];
            snprintf(dir, sizeof(dir), "%s", m->path);
            *strrchr(dir, '/') = 0;
            if (!mount(NULL, dir, NULL, MS_REMOUNT, "sync")) hostdev_open(&nh, m->path, O_RDWR);
        }
        if (nh.fd < 0) { warn("member %s cannot be opened for writing", m->path); m->ok = 0; continue; }
        if ((rc = lock_member(&nh))) {
            hostdev_close(&nh);
            if (rc > 0) return rc;
            m->ok = 0;
            continue;
        }
        hostdev_close(&m->hd);
        rwy_dev_stats_add(&nh.stats, &m->hd.stats);   /* keep what the discovery read */
        m->hd = nh;
        any = 1;
    }
    return any ? 0 : X_IO;
}

static int newest_record(group_t *g);
static void note_lagging(void);

static rwy_block_dev_t g_recdevs[RWY_MIRROR_MAX_DEVS];   /* the members that opened, as the store sees them */
static size_t g_nrecdevs;
static unsigned g_lagging;   /* bit i: mirror member i is left out because it lags */

/* Mount each member under its key; returns how many mounted. *prim is the newest, with generation *best. */
static int probe_members(group_t *g, uint64_t *best, int *prim) {
    int i, nok = 0;
    *best = 0;
    *prim = -1;
    for (i = 0; i < g->n; i++) {   /* the generation under the key, so a wrong key is not a member */
        member_t *m = &g_mem[g->m[i]];
        rwy_store_t *probe = &g_store;
        if (!m->ok) continue;
        if (rwy_mount(probe, &m->dev) != RWY_OK) { m->ok = 0; continue; }
        m->gen = probe->sb.generation;
        if (!nok || m->gen > *best) { *best = m->gen; *prim = i; }
        nok++;
    }
    return nok;
}

/* Put the members that remain into the mirror: the newest is primary, lagging ones are faulted. */
static void fill_mirror(group_t *g, uint64_t best, int prim) {
    int i;
    for (i = 0; i < g->n; i++) {   /* the newest member's data key is the group's; others are foreign (as RUNWAY.EFI) */
        member_t *m = &g_mem[g->m[i]];
        if (m->ok && m->enc && memcmp(m->cctx.dek, g_mem[g->m[prim]].cctx.dek, 32)) m->ok = 0;
    }
    for (i = 0; i < g->n; i++) {
        member_t *m = &g_mem[g->m[i]];
        if (!m->ok) { warn("member %s excluded (cannot be mounted with this key)", m->path); continue; }
        rwy_mirror_add(&g_mirror, &m->raw);
        if (m->stale) rwy_mirror_set_faulted(&g_mirror, g_mirror.num_devs - 1, 1);
        else if (m->gen < best) {
            warn("warning: member %s lagging, RUNWAY.EFI will catch it up at the next boot", m->path);
            g_lagging |= 1u << (g_mirror.num_devs - 1);
            rwy_mirror_set_faulted(&g_mirror, g_mirror.num_devs - 1, 1);
        }
        if (i == prim) rwy_mirror_set_primary(&g_mirror, g_mirror.num_devs - 1);
    }
}

int host_random(void *buf, size_t len) {
    return getrandom(buf, len, 0) == (ssize_t)len ? 0 : -1;
}

static rwy_block_dev_t *setup_mount_dev(int prim_enc, const uint8_t *dek) {
    if (prim_enc) {
        memcpy(g_active_dek, dek, 32);
        g_is_enc = 1;
        if (rwy_crypto_dev_wrap(&g_crypto_dev, &g_crypto_ctx, &g_raw_dev, g_active_dek, RWY_SB_BLOCKS))
            return NULL;
        return &g_crypto_dev;
    }
    g_is_enc = 0;
    return &g_raw_dev;
}

static void resume_pending_reencrypt(void) {
    int pending = 0;
    if (g_rw && g_is_enc && rwy_reencrypt_pending(&g_store, &pending) == RWY_OK && pending)
        (void)rwy_reencrypt_finish(&g_store, NULL);
}

/* Mount the group through a mirror: newest member is primary, lagging members are faulted. */
int open_group(group_t *g) {
    uint64_t best;
    int prim, nok, rc = unlock_group(g);
    rwy_block_dev_t *pdev;
    if (rc) return rc;
    if (g_rw && (rc = lock_group(g))) return rc;
    rwy_mirror_init(&g_mirror);
    nok = probe_members(g, &best, &prim);
    if (!nok && g_mem[g->m[0]].enc) { warn("%s", "authentication failed (incorrect key)"); return X_LOCKED; }
    if (!nok) { warn("%s", "no member of the store can be mounted"); return X_IO; }
    fill_mirror(g, best, prim);
    if (g_rw && g_mirror.num_devs > 1 && rwy_mirror_name_members(&g_mirror, host_random))
        warn("%s", "a mirror member could not be given its UUID");
    rwy_mirror_wrap(&g_raw_dev, &g_mirror);
    if (!(pdev = setup_mount_dev(g_mem[g->m[prim]].enc, g_mem[g->m[prim]].cctx.dek))) return X_IO;
    if (rwy_mount(&g_store, pdev) != RWY_OK) return X_IO;
    hostdev_use_build_clock(&g_store);
    if ((rc = newest_record(g))) return rc;
    if (g_rw) note_lagging();
    resume_pending_reencrypt();
    return 0;
}

/* A record write keeps the tree generation, so the newest record can sit on any member. Read it;
 * the next record write reaches the live members and RUNWAY.EFI heals the others. */
static int newest_record(group_t *g) {
    size_t w = 0;
    int i;
    for (i = 0; i < g->n; i++)
        if (g_mem[g->m[i]].ok && g_nrecdevs < RWY_MIRROR_MAX_DEVS) g_recdevs[g_nrecdevs++] = g_mem[g->m[i]].dev;
    if (rwy_record_sync(g_recdevs, g_nrecdevs, g_store.sb.rec_start, 0, &w)) return X_IO;
    return rwy_record_adopt(&g_store, &g_recdevs[w]) == RWY_OK ? 0 : X_IO;
}

/* A command that writes records what it saw: the members left out because they lag, and the ones that are gone. */
static void note_lagging(void) {
    for (size_t i = 0; i < g_mirror.num_devs; i++) {
        uint8_t uuid[16];
        if ((g_lagging & (1u << i)) && rwy_block_member_uuid(&g_mirror.devs[i], uuid))
            (void)rwy_record_note_health(&g_store, g_recdevs, g_nrecdevs, uuid, RWY_SS_EV_LAGGING);
    }
    (void)rwy_mirror_note_members(&g_store, &g_mirror, g_recdevs, g_nrecdevs);
}

void note_write_failures(void) {
    (void)rwy_mirror_note_failures(&g_store, &g_mirror, g_recdevs, g_nrecdevs);
}
