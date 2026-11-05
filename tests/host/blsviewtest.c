/*
 * blsviewtest.c — The snapshot of the BLS projection gives the answers of the scans it replaces.
 * SPDX-License-Identifier: Apache-2.0
 *
 * golden[] holds what the scans answered for the store of build_shapes(). After every change the view that
 * was kept must answer as a view that is built from the store at that moment.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "testdir.h"
#include "rwy_bls.h"
#include "rwy_bls_view.h"
#include "rwy_core.h"
#include "blsviewtest_golden.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        failures++; \
    } \
} while (0)

typedef struct { FILE *f; } test_file_t;
static int test_frd(void *ctx, uint64_t off, void *buf, size_t len) {
    test_file_t *fl = ctx;
    if (fseek(fl->f, (long)off, SEEK_SET)) return -1;
    return fread(buf, 1, len, fl->f) == len ? 0 : -1;
}
static int test_fwr(void *ctx, uint64_t off, const void *buf, size_t len) {
    test_file_t *fl = ctx;
    if (fseek(fl->f, (long)off, SEEK_SET)) return -1;
    return fwrite(buf, 1, len, fl->f) == len ? 0 : -1;
}
static int test_ffl(void *ctx) {
    test_file_t *fl = ctx;
    return fflush(fl->f) ? -1 : 0;
}

/* A list of names, joined with '|', as the readdir functions would hand them to the filler. */
typedef struct { char t[8192]; } list_t;

static void list_add(list_t *l, const char *name) {
    size_t n = strlen(l->t);
    snprintf(l->t + n, sizeof(l->t) - n, "%s|", name);
}

static void list_cb(void *ctx, const char *name) { list_add(ctx, name); }

/* ---- the store ---- */

typedef struct {
    test_file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
} env_t;

static int open_img(env_t *e, const char *img) {
    memset(e, 0, sizeof(*e));
    e->fl.f = fopen(img, "r+b");
    if (!e->fl.f) return -1;
    e->dev.read = test_frd; e->dev.write = test_fwr; e->dev.flush = test_ffl; e->dev.ctx = &e->fl;
    e->dev.size = 256ULL * 1024 * 1024; e->dev.block_size = 4096;
    return rwy_mount(&e->s, &e->dev);
}

static int copy_img(const char *from, const char *to) {
    char buf[4096];
    size_t nr;
    FILE *src = fopen(from, "rb"), *dst;
    if (!src) return -1;
    dst = fopen(to, "wb");
    if (!dst) { fclose(src); return -1; }
    while ((nr = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, nr, dst);
    fclose(src);
    fclose(dst);
    return 0;
}

static int open_env(env_t *e, const char *img, const char *seed) {
    if (copy_img(seed, img)) return -1;
    return open_img(e, img);
}

static uint64_t reads_of(const rwy_store_t *s) {
    uint64_t n = 0;
    int k, b;
    for (k = 0; k < RWY_IO_KINDS; k++)
        for (b = 0; b < RWY_IO_BUCKETS; b++) n += s->io.calls[0][k][b];
    return n;
}

static void put(rwy_store_t *s, const char *path, const void *data, size_t len) {
    int rc = rwy_create(s, path, RWY_S_IFREG | 0644, 0, 0);
    if (rc) printf("create %s: %d\n", path, rc);
    CHECK(rc == RWY_OK, path);
    if (len) { rc = rwy_write_path(s, path, 0, data, len); if (rc) printf("write %s: %d\n", path, rc); CHECK(rc == RWY_OK, path); }
}

static void stage(rwy_store_t *s, const char *logical, char fill) {
    char enc[256], path[300], data[64];
    memset(data, fill, sizeof(data));
    CHECK(rwy_bls_encode_blob_name(logical, enc, sizeof(enc)) > 0, "encode");
    snprintf(path, sizeof(path), "EFI/RUNWAY/BLOBS/%s", enc);
    rwy_begin(s);
    put(s, path, data, sizeof(data));
    CHECK(rwy_commit(s) == RWY_OK, "stage commit");
}

static void raw_slot(rwy_store_t *s, const char *slot, const char *kernel, const char *initrd, const char *dtb,
                     const char *label, const char *args, const char *meta, const char *entry, const char *state) {
    char p[300];
    rwy_set_actor(s, RWY_ACTOR_SYSTEM);
    s->bc_priv = 1;
    snprintf(p, sizeof(p), "EFI/RUNWAY/%s", slot);
    CHECK(rwy_begin(s) == RWY_OK, "begin");
    CHECK(rwy_mkdir(s, p, RWY_S_IFDIR | 0755, 0, 0) == RWY_OK, p);
#define F(name, v) do { if (v) { snprintf(p, sizeof(p), "EFI/RUNWAY/%s/%s", slot, name); put(s, p, v, strlen(v)); } } while (0)
    F("KERNEL", kernel); F("INITRD", initrd); F("DTB", dtb); F("LABEL", label);
    F("ARGS", args); F("META", meta); F("ENTRY", entry); F("STATE", state);
#undef F
    CHECK(rwy_commit(s) == RWY_OK, "raw slot commit");
    s->bc_priv = 0;
    rwy_set_actor(s, RWY_ACTOR_USER);
}

/* ---- the comparison ---- */

#define MAXQ 4096
static char *qpath[MAXQ];
static size_t nq;

static void q_add(const char *p) {
    size_t i;
    for (i = 0; i < nq; i++) if (!strcmp(qpath[i], p)) return;
    if (nq < MAXQ) qpath[nq++] = strdup(p);
}

static void q_clear(void) {
    size_t i;
    for (i = 0; i < nq; i++) free(qpath[i]);
    nq = 0;
}

/* Paths to ask about: what the store holds, and near misses of it. */
static void q_generate(rwy_store_t *s) {
    char name[256], slot[300], pfx[256], p[600], enc[256], dec[256];
    size_t i = 0;
    static const char *const fixed[] = { "/", "", "/loader", "/loader/entries", "/EFI", "/EFI/Linux", "/nothing",
        "/linux", "/q", "/q/", "/w", "/w/x", "/w/x/y", "/w/x/z", "/w/x/y/", "/tok", "/tok/6.12", "/tok/6.12/linux" };
    for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) q_add(fixed[i]);
    i = 0;
    while (rwy_readdir_nth(s, "EFI/RUNWAY", i++, name, sizeof(name), NULL) == RWY_OK) {
        rwy_bls_slot_info_t info;
        char *last;
        static const char *const leaf[] = { "linux", "initrd", "dtb", "Linux", "linux2", "" };
        size_t k;
        if (strncmp(name, "CONFIG", 6) != 0) continue;
        snprintf(slot, sizeof(slot), "EFI/RUNWAY/%.32s", name);
        if (rwy_bls_read_slot_info(s, slot, &info) != RWY_OK) continue;
        rwy_bls_derive_prefix(&info, pfx, sizeof(pfx));
        q_add(pfx);
        snprintf(p, sizeof(p), "%s/", pfx); q_add(p);
        for (k = 0; k < sizeof(leaf) / sizeof(leaf[0]); k++) { snprintf(p, sizeof(p), "%s/%s", pfx, leaf[k]); q_add(p); }
        last = strrchr(pfx, '/');
        if (last && last != pfx) { *last = 0; q_add(pfx); snprintf(p, sizeof(p), "%s/linux", pfx); q_add(p); }
    }
    i = 0;
    while (rwy_readdir_nth(s, "EFI/RUNWAY/BLOBS", i++, name, sizeof(name), NULL) == RWY_OK) {
        char *sl;
        snprintf(enc, sizeof(enc), "%s", name);
        if (rwy_bls_decode_blob_name(enc, dec, sizeof(dec)) < 0) continue;
        snprintf(p, sizeof(p), "/%s", dec);
        q_add(p);
        while ((sl = strrchr(p, '/')) && sl != p) { *sl = 0; q_add(p); }
    }
}

/* Entry file names to ask for: the rendered ones and variants of them. */
static char *qent[MAXQ];
static size_t nqe;

static void qe_clear(void) { size_t i; for (i = 0; i < nqe; i++) free(qent[i]); nqe = 0; }
static void qe_add(const char *n) {
    size_t i;
    for (i = 0; i < nqe; i++) if (!strcmp(qent[i], n)) return;
    if (nqe < MAXQ) qent[nqe++] = strdup(n);
}

static void qe_generate(rwy_store_t *s) {
    rwy_bls_view_t f;
    list_t l1, l2;
    char *p, *e;
    memset(&l1, 0, sizeof(l1)); memset(&l2, 0, sizeof(l2));
    rwy_bls_view_init(&f);
    CHECK(rwy_bls_view_sync(&f, s) == RWY_OK, "sync for the questions");
    rwy_bls_view_entries(&f, 0, list_cb, &l1);
    rwy_bls_view_entries(&f, 1, list_cb, &l2);
    rwy_bls_view_free(&f);
    strcat(l1.t, l2.t);
    for (p = l1.t; *p; p = e + 1) {
        char n[300];
        e = strchr(p, '|');
        *e = 0;
        qe_add(p);
        snprintf(n, sizeof(n), "%sx", p); qe_add(n);
        snprintf(n, sizeof(n), "x%s", p); qe_add(n);
        if (strlen(p) > 5) { snprintf(n, sizeof(n), "%.*s.efi", (int)(strlen(p) - 5), p); qe_add(n); }
        if (strlen(p) > 5) { snprintf(n, sizeof(n), "%.*s+3.conf", (int)(strlen(p) - 5), p); qe_add(n); }
    }
    qe_add(""); qe_add("nothing.conf");
}

/* Every answer of a view to the generated questions, one line each, in the format of golden[]. */
static char *answers(const rwy_bls_view_t *v) {
    size_t cap = 1u << 20, pos = 0, i;
    char *out = malloc(cap);
    char a[512], b[512];
    list_t bl, sl;
    int clen, r1, r2;

#define EMIT(...) do { pos += (size_t)snprintf(out + pos, cap - pos, __VA_ARGS__); } while (0)
    for (i = 0; i < nq; i++) {
        const char *p = qpath[i];
        memset(&bl, 0, sizeof(bl)); memset(&sl, 0, sizeof(sl));
        rwy_bls_view_blob_children(v, p, list_cb, &bl);
        rwy_bls_view_slot_children(v, p, list_cb, &sl);
        a[0] = 0;
        EMIT("Q %s | resolve=%s | dir=%d | blobs=%s | slots=%s\n", p,
             rwy_bls_view_resolve(v, p, a, sizeof(a)) == RWY_OK ? a : "none", rwy_bls_view_is_dir(v, p), bl.t, sl.t);
    }
    for (i = 0; i < nqe; i++) {
        const char *f = qent[i];
        a[0] = b[0] = 0;
        clen = -1;
        r1 = rwy_bls_view_entry(v, 0, f, a, sizeof(a), &clen);
        r2 = rwy_bls_view_entry(v, 1, f, b, sizeof(b), NULL);
        EMIT("E %s | t1=%s%s | t2=%s\n", f, r1 == RWY_OK ? a : "none",
             r1 != RWY_OK ? "" : clen >= 0 ? " conf=ok" : " conf=none", r2 == RWY_OK ? b : "none");
    }
    memset(&bl, 0, sizeof(bl));
    rwy_bls_view_entries(v, 0, list_cb, &bl);
    EMIT("L entries0 %s\n", bl.t);
    memset(&bl, 0, sizeof(bl));
    rwy_bls_view_entries(v, 1, list_cb, &bl);
    EMIT("L entries1 %s\n", bl.t);
    memset(&bl, 0, sizeof(bl));
    rwy_bls_view_roots(v, list_cb, &bl);
    EMIT("L roots %s\n", bl.t);
#undef EMIT
    return out;
}

/* The .conf length that the view keeps is the length of the text that the core renders. */
static void check_conf_lengths(rwy_store_t *s, const rwy_bls_view_t *v) {
    size_t i;
    for (i = 0; i < v->nslots; i++) {
        const rwy_bls_view_slot_t *sl = &v->slots[i];
        rwy_bls_slot_info_t info;
        char conf[4096];
        int n;
        if (sl->is_type2 || rwy_bls_read_slot_info(s, sl->path, &info) != RWY_OK) continue;
        n = rwy_bls_render_type1(&info, conf, sizeof(conf));
        CHECK(n == sl->conf_len || (n < 0 && sl->conf_len < 0), sl->path);
    }
}

/* Compare the view that was kept with one that is built from the store now. Returns the number of questions. */
static size_t compare_all(rwy_store_t *s, rwy_bls_view_t *v, const char *tag) {
    rwy_bls_view_t fresh;
    char *kept, *built, msg[400];
    size_t n;

    CHECK(rwy_bls_view_sync(v, s) == RWY_OK, "sync");
    q_clear(); qe_clear();
    q_generate(s); qe_generate(s);
    rwy_bls_view_init(&fresh);
    CHECK(rwy_bls_view_sync(&fresh, s) == RWY_OK, "sync a new view");
    kept = answers(v);
    built = answers(&fresh);
    snprintf(msg, sizeof(msg), "%s: the kept view answers as a new one", tag);
    CHECK(!strcmp(kept, built), msg);
    check_conf_lengths(s, v);
    free(kept); free(built);
    rwy_bls_view_free(&fresh);
    n = nq + nqe;
    return n;
}

/* The answers of the scans, line by line. */
static void check_golden(const rwy_bls_view_t *v) {
    char *got = answers(v), *line, *next;
    size_t i = 0;
    for (line = got; *line; line = next, i++) {
        char msg[1200];
        next = strchr(line, '\n');
        *next++ = 0;
        snprintf(msg, sizeof(msg), "golden %zu: '%s'", i, line);
        CHECK(i < sizeof(golden) / sizeof(golden[0]) && !strcmp(line, golden[i]), msg);
    }
    CHECK(i == sizeof(golden) / sizeof(golden[0]), "as many answers as golden lines");
    free(got);
}

/* Ask everything again through the view alone and return the device reads that took. */
static uint64_t view_only_reads(rwy_store_t *s, rwy_bls_view_t *v) {
    uint64_t before = reads_of(s);
    size_t i;
    char o[512];
    list_t b;
    int clen;
    rwy_bls_view_sync(v, s);
    for (i = 0; i < nq; i++) {
        memset(&b, 0, sizeof(b));
        rwy_bls_view_payload(v, qpath[i], o, sizeof(o));
        rwy_bls_view_resolve(v, qpath[i], o, sizeof(o));
        rwy_bls_view_is_dir(v, qpath[i]);
        rwy_bls_view_blob_children(v, qpath[i], list_cb, &b);
        rwy_bls_view_slot_children(v, qpath[i], list_cb, &b);
    }
    for (i = 0; i < nqe; i++) {
        rwy_bls_view_entry(v, 0, qent[i], o, sizeof(o), &clen);
        rwy_bls_view_entry(v, 1, qent[i], o, sizeof(o), NULL);
    }
    memset(&b, 0, sizeof(b));
    rwy_bls_view_entries(v, 0, list_cb, &b);
    rwy_bls_view_entries(v, 1, list_cb, &b);
    rwy_bls_view_roots(v, list_cb, &b);
    return reads_of(s) - before;
}

/* One change, then the view must move and agree with the scans. */
static void changed(rwy_store_t *s, rwy_bls_view_t *v, const char *what) {
    unsigned long b = v->builds;
    char tag[200];
    snprintf(tag, sizeof(tag), "after %s", what);
    compare_all(s, v, tag);
    snprintf(tag, sizeof(tag), "%s: the view was rebuilt", what);
    CHECK(v->builds == b + 1, tag);
}

static void unchanged(rwy_store_t *s, rwy_bls_view_t *v, const char *what) {
    unsigned long b = v->builds;
    char tag[200];
    snprintf(tag, sizeof(tag), "after %s", what);
    compare_all(s, v, tag);
    snprintf(tag, sizeof(tag), "%s: the view was kept", what);
    CHECK(v->builds == b, tag);
}

static void build_shapes(rwy_store_t *s) {
    static const char c1[] = "title One\nversion 1.0\nlinux /a/1.0/linux\ninitrd /a/1.0/initrd\noptions quiet\n";
    static const char c2[] = "title Two\nversion 2.0\nmachine-id 0123456789abcdef0123456789abcdef\nlinux /b/2.0/linux\ndevicetree /b/2.0/dtb\n";
    static const char c3[] = "title Long\nlinux /c/linux\n";
    static const char c4[] = "title Version\nversion vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv\nlinux /d/linux\n";
    static const char c5[] = "title Dup A\nversion 3.0\nmachine-id fedcba9876543210fedcba9876543210\nlinux /e/3.0/linux\n";
    static const char c6[] = "title Dup B\nversion 3.0\nmachine-id fedcba9876543210fedcba9876543210\nlinux /e/3.0/linux\n";
    static const char c7[] = "linux /f/linux\n";
    char long_name[300], conf[300];
    char uki[128];

    stage(s, "/a/1.0/linux", 'a'); stage(s, "/a/1.0/initrd", 'i');
    stage(s, "/b/2.0/linux", 'b'); stage(s, "/b/2.0/dtb", 'd');
    stage(s, "/c/linux", 'c'); stage(s, "/d/linux", 'e'); stage(s, "/e/3.0/linux", 'f'); stage(s, "/f/linux", 'g');
    CHECK(rwy_bls_commit_type1(s, "alpha.conf", c1, strlen(c1), 3) == RWY_OK, "commit alpha");
    CHECK(rwy_bls_commit_type1(s, "beta.conf", c2, strlen(c2), 2) == RWY_OK, "commit beta");
    snprintf(long_name, sizeof(long_name), "%0200d-end.conf", 7);
    memset(long_name, 'n', 100);
    snprintf(conf, sizeof(conf), "%s", c3);
    CHECK(rwy_bls_commit_type1(s, long_name, conf, strlen(conf), 3) == RWY_OK, "commit long name");
    CHECK(rwy_bls_commit_type1(s, "vers.conf", c4, strlen(c4), 3) == RWY_OK, "commit long version");
    CHECK(rwy_bls_commit_type1(s, "dup1.conf", c5, strlen(c5), 3) == RWY_OK, "commit dup1");
    CHECK(rwy_bls_commit_type1(s, "dup2.conf", c6, strlen(c6), 3) == RWY_OK, "commit dup2 (same prefix)");
    CHECK(rwy_bls_commit_type1(s, "plain.conf", c7, strlen(c7), 3) == RWY_OK, "commit no meta");
    memset(uki, 'U', sizeof(uki)); uki[0] = 'M'; uki[1] = 'Z';
    rwy_begin(s);
    put(s, "EFI/RUNWAY/BLOBS/one.efi", uki, sizeof(uki));
    put(s, "EFI/RUNWAY/BLOBS/two.efi", uki, sizeof(uki));
    CHECK(rwy_commit(s) == RWY_OK, "stage ukis");
    CHECK(rwy_bls_commit_type2(s, "uki-1.efi", "EFI/RUNWAY/BLOBS/one.efi", 3) == RWY_OK, "commit uki 1");
    CHECK(rwy_bls_commit_type2(s, "uki-2.efi", "EFI/RUNWAY/BLOBS/two.efi", 1) == RWY_OK, "commit uki 2");

    /* Slots that the commit would not make */
    raw_slot(s, "CONFIG90", "K", NULL, NULL, NULL, NULL, NULL, NULL, NULL);                     /* default name, type 2 */
    raw_slot(s, "CONFIG91", NULL, "I", "D", "no kernel", NULL, NULL, NULL, NULL);               /* no KERNEL */
    raw_slot(s, "CONFIG92", NULL, "I", NULL, NULL, NULL, "version 1.0\nmachine-id fedcba9876543210fedcba9876543210\n", "same.conf", "OK\n");
    raw_slot(s, "CONFIG93", "K", "I", NULL, "x", "ro", "version 1.0\nmachine-id fedcba9876543210fedcba9876543210\n", "same2.conf", "FAILED\n");
    raw_slot(s, "CONFIG94", "K", NULL, NULL, "t", NULL, NULL, "dotted.name.v2.conf", "TESTING\n");
    raw_slot(s, "CONFIG95", "K", NULL, NULL, "t", NULL, "machine-id aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n", "bigmid.conf", NULL);
    raw_slot(s, "CONFIG96", "K", NULL, NULL, "t", NULL, "version 9\nsort-key z\narchitecture x64\n", "meta.efi", NULL);

    /* BLOBS names that look like prefixes */
    rwy_begin(s);
    put(s, "EFI/RUNWAY/BLOBS/q%2F", "x", 1);
    put(s, "EFI/RUNWAY/BLOBS/w%2Fx%2Fy", "x", 1);
    put(s, "EFI/RUNWAY/BLOBS/w%2Fx%2Fz", "x", 1);
    put(s, "EFI/RUNWAY/BLOBS/w%2fx%2fyy", "x", 1);
    put(s, "EFI/RUNWAY/BLOBS/w%2Fx%2Fy%2Fdeep", "x", 1);
    put(s, "EFI/RUNWAY/BLOBS/w%2Fm%2fn%2Fo", "x", 1);
    put(s, "EFI/RUNWAY/BLOBS/tok%2F6.12%2Flinux", "x", 1);
    CHECK(rwy_commit(s) == RWY_OK, "odd blobs");
}

static void test_shapes(const char *seed) {
    env_t e;
    rwy_bls_view_t v;
    uint64_t before, built, again;
    size_t n;

    printf("== bls view: same answers as the scans ==\n");
    if (open_env(&e, tpath("rwy_test_blsview.img"), seed) != RWY_OK) { CHECK(0, "mount"); return; }
    rwy_bls_view_init(&v);
    rwy_begin(&e.s);
    CHECK(rwy_mkdir(&e.s, "EFI/RUNWAY", RWY_S_IFDIR | 0755, 0, 0) == RWY_OK || 1, "root");
    rwy_mkdir(&e.s, "EFI/RUNWAY/BLOBS", RWY_S_IFDIR | 0755, 0, 0);
    rwy_commit(&e.s);

    compare_all(&e.s, &v, "empty");
    build_shapes(&e.s);
    before = reads_of(&e.s);
    CHECK(rwy_bls_view_sync(&v, &e.s) == RWY_OK, "sync");
    built = reads_of(&e.s) - before;
    printf("   view build: %llu device reads, %zu slots\n", (unsigned long long)built, v.nslots);
    CHECK(built <= 16 * v.nslots, "a build reads about 7 times per slot");
    n = compare_all(&e.s, &v, "shapes");
    check_golden(&v);
    printf("   %zu questions\n", n);
    CHECK(n > 100, "enough questions");
    again = view_only_reads(&e.s, &v);
    CHECK(again == 0, "a second pass reads nothing");

    rwy_bls_view_free(&v);
    q_clear(); qe_clear();
    fclose(e.fl.f);
}

static void state_write(rwy_store_t *s, const char *slot, const char *file, const char *val) {
    char p[300];
    int actor = s->actor;
    snprintf(p, sizeof(p), "EFI/RUNWAY/%s/%s", slot, file);
    if (strcmp(file, "STATE")) rwy_set_actor(s, RWY_ACTOR_SYSTEM);
    CHECK(rwy_begin(s) == RWY_OK, "begin");
    CHECK(rwy_write_path(s, p, 0, val, strlen(val)) == RWY_OK, p);
    CHECK(rwy_commit(s) == RWY_OK, "commit");
    rwy_set_actor(s, actor);
}

static void test_staleness(const char *seed) {
    env_t e, o;
    rwy_bls_view_t v;
    rwy_store_t *s;
    uint64_t gen, seq;
    char p[300];

    printf("== bls view: every kind of change is seen ==\n");
    if (open_env(&e, tpath("rwy_test_blsview2.img"), seed) != RWY_OK) { CHECK(0, "mount"); return; }
    s = &e.s;
    rwy_bls_view_init(&v);
    rwy_begin(s);
    rwy_mkdir(s, "EFI/RUNWAY", RWY_S_IFDIR | 0755, 0, 0);
    rwy_mkdir(s, "EFI/RUNWAY/BLOBS", RWY_S_IFDIR | 0755, 0, 0);
    rwy_commit(s);
    build_shapes(s);
    compare_all(s, &v, "start");
    unchanged(s, &v, "nothing");

    /* Tree writes */
    rwy_begin(s); put(s, "EFI/RUNWAY/BLOBS/extra", "x", 1); rwy_commit(s);
    changed(s, &v, "create a blob");
    rwy_set_actor(s, RWY_ACTOR_SYSTEM);
    state_write(s, "CONFIG93", "LABEL", "A longer title\n");
    rwy_set_actor(s, RWY_ACTOR_USER);
    changed(s, &v, "rewrite LABEL");
    state_write(s, "CONFIG93", "ARGS", "a=b c=d\n");
    changed(s, &v, "rewrite ARGS");
    rwy_begin(s); rwy_rename(s, "EFI/RUNWAY/BLOBS/q%2F", "EFI/RUNWAY/BLOBS/r%2F"); rwy_commit(s);
    changed(s, &v, "rename a blob");
    rwy_begin(s); rwy_unlink(s, "EFI/RUNWAY/BLOBS/w%2Fx%2Fz"); rwy_commit(s);
    changed(s, &v, "unlink a blob");
    rwy_begin(s); put(s, "EFI/RUNWAY/CONFIG91/KERNEL", "kernel\n", 7); rwy_commit(s);
    changed(s, &v, "add a KERNEL");
    CHECK(rwy_bls_remove_slot(s, "CONFIG94") == RWY_OK, "remove slot");
    changed(s, &v, "remove a slot");
    {
        static const char c[] = "title New\nversion 8.0\nlinux /g/8.0/linux\n";
        stage(s, "/g/8.0/linux", 'h');
        compare_all(s, &v, "stage");
        CHECK(rwy_bls_commit_type1(s, "new.conf", c, strlen(c), 3) == RWY_OK, "add slot");
        changed(s, &v, "add a slot");
    }
    rwy_begin(s); rwy_mkdir(s, "EFI/RUNWAY/BLOBS/dir", RWY_S_IFDIR | 0755, 0, 0); rwy_commit(s);
    rwy_begin(s); rwy_rmdir(s, "EFI/RUNWAY/BLOBS/dir"); rwy_commit(s);
    compare_all(s, &v, "mkdir, rmdir");

    /* Record writes: the tree does not change, the record does */
    gen = s->sb.generation;
    seq = s->rec_seq;
    rwy_begin(s);
    CHECK(rwy_write_path(s, "EFI/RUNWAY/CONFIG1/STATE", 0, "OK\n", 3) == RWY_OK, "STATE in a transaction");
    rwy_abort(s);
    CHECK(s->sb.generation == gen && s->rec_seq == seq + 1, "the record moved, the tree did not");
    /* A key made of the tree alone would call the old view current. The entry names show it is not. */
    {
        rwy_bls_view_t f;
        list_t a, b;
        memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
        rwy_bls_view_init(&f);
        CHECK(rwy_bls_view_sync(&f, s) == RWY_OK, "sync a new view");
        rwy_bls_view_entries(&f, 0, list_cb, &a);
        rwy_bls_view_entries(&v, 0, list_cb, &b);
        rwy_bls_view_free(&f);
        CHECK(strcmp(a.t, b.t), "the record moved: without rec_seq in the key the view would be stale");
    }
    changed(s, &v, "STATE written and the transaction given up");
    state_write(s, "CONFIG1", "STATE", "OK\n");
    changed(s, &v, "STATE of a finalized slot to OK");
    state_write(s, "CONFIG2", "STATE", "FAILED\n");
    changed(s, &v, "STATE to FAILED");
    rwy_set_actor(s, RWY_ACTOR_SYSTEM);
    state_write(s, "CONFIG3", "TRIES", "7\n");
    changed(s, &v, "TRIES");
    state_write(s, "CONFIG3", "STATE", "TESTING\n");
    changed(s, &v, "STATE to TESTING");
    rwy_set_actor(s, RWY_ACTOR_USER);
    /* A write that changes nothing still commits: a new generation, so a rebuild, and the same answers. */
    state_write(s, "CONFIG1", "STATE", "OK\n");
    changed(s, &v, "OK written to OK");

    /* Transactions that are given up */
    rwy_begin(s);
    put(s, "EFI/RUNWAY/BLOBS/junk", "x", 1);
    CHECK(rwy_bls_view_sync(&v, s) == RWY_ERR_BUSY, "no view inside a transaction");
    rwy_abort(s);
    unchanged(s, &v, "an aborted transaction");
    rwy_begin(s); rwy_unlink(s, "EFI/RUNWAY/CONFIG1/KERNEL"); rwy_abort(s);
    unchanged(s, &v, "an aborted unlink");

    /* Members of the state that are not in the key: the view must not depend on them */
    rwy_set_actor(s, RWY_ACTOR_SYSTEM);
    unchanged(s, &v, "another actor");
    rwy_set_actor(s, RWY_ACTOR_USER);
    s->bc_priv = 1;
    unchanged(s, &v, "bc_priv");
    s->bc_priv = 0;
    s->bc_novirt = 1;
    CHECK(rwy_bls_view_sync(&v, s) == RWY_ERR_BUSY, "no view while virtual files are hidden");
    s->bc_novirt = 0;
    rwy_set_fixed_time(s, 1234, 5);
    unchanged(s, &v, "another clock");

    /* Remount of the same state keeps the view; a remount after another writer sees the new state */
    CHECK(rwy_remount(s) == RWY_OK, "remount");
    unchanged(s, &v, "a remount of the same state");
    memset(&o, 0, sizeof(o));
    o.fl.f = fopen(tpath("rwy_test_blsview2.img"), "r+b");
    o.dev = e.dev; o.dev.ctx = &o.fl;
    CHECK(rwy_mount(&o.s, &o.dev) == RWY_OK, "second mount");
    snprintf(p, sizeof(p), "EFI/RUNWAY/CONFIG92/LABEL");
    rwy_begin(&o.s); rwy_write_path(&o.s, p, 0, "by another mount\n", 17); rwy_commit(&o.s);
    state_write(&o.s, "CONFIG1", "STATE", "FAILED\n");
    fflush(o.fl.f);
    CHECK(rwy_remount(s) == RWY_OK, "remount after the other mount");
    changed(s, &v, "a remount after another mount");
    /* Without the remount the view is of the state this mount has: both are stale together */
    rwy_begin(&o.s); rwy_write_path(&o.s, p, 0, "again\n", 6); rwy_commit(&o.s);
    unchanged(s, &v, "a commit that this mount has not adopted");
    fclose(o.fl.f);

    rwy_bls_view_free(&v);
    q_clear(); qe_clear();
    fclose(e.fl.f);
}

/* A store file that was copied and then written on both sides: same UUID, generation and record sequence. */
static void test_clone(const char *seed) {
    env_t a, b;
    rwy_bls_view_t v;
    const char *ia = tpath("rwy_test_blsview3.img"), *ib = tpath("rwy_test_blsview3b.img");

    printf("== bls view: a store that is opened again starts with no view ==\n");
    if (open_env(&a, ia, seed) != RWY_OK) { CHECK(0, "mount"); return; }
    rwy_begin(&a.s);
    rwy_mkdir(&a.s, "EFI/RUNWAY", RWY_S_IFDIR | 0755, 0, 0);
    rwy_mkdir(&a.s, "EFI/RUNWAY/BLOBS", RWY_S_IFDIR | 0755, 0, 0);
    rwy_commit(&a.s);
    fflush(a.fl.f);
    CHECK(copy_img(ia, ib) == 0 && open_img(&b, ib) == RWY_OK, "clone");
    stage(&a.s, "/x/linux", 'a');
    stage(&b.s, "/y/linux", 'b');
    CHECK(a.s.sb.generation == b.s.sb.generation && a.s.rec_seq == b.s.rec_seq &&
          !memcmp(a.s.sb.uuid, b.s.sb.uuid, 16), "the two stores carry the same counters");

    rwy_bls_view_init(&v);
    CHECK(rwy_bls_view_sync(&v, &a.s) == RWY_OK, "sync a");
    CHECK(rwy_bls_view_is_dir(&v, "/x") && !rwy_bls_view_is_dir(&v, "/y"), "the view shows a");
    CHECK(rwy_bls_view_sync(&v, &b.s) == RWY_OK, "sync b");
    CHECK(v.builds == 2, "the view was rebuilt for the other open");
    CHECK(rwy_bls_view_is_dir(&v, "/y") && !rwy_bls_view_is_dir(&v, "/x"), "the view shows b");
    CHECK(rwy_remount(&b.s) == RWY_OK && rwy_bls_view_sync(&v, &b.s) == RWY_OK && v.builds == 2,
          "a remount of the same handle keeps the view");

    rwy_bls_view_free(&v);
    fclose(a.fl.f);
    fclose(b.fl.f);
}

int main(int argc, char **argv) {
    const char *seed = argc > 1 ? argv[1] : "build/tests/host/RUNWAY.DAT";
    test_shapes(seed);
    test_staleness(seed);
    test_clone(seed);
    if (failures) {
        printf("BLSVIEWTEST: %d FAILURES\n", failures);
        return 1;
    }
    printf("BLSVIEWTEST: PASS\n");
    return 0;
}
