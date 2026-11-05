/* Shared harness of the bootcfg tests: store access, transactions and fixtures. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "bootcfgtest.h"

int failures;
const char *g_checkbin = "build/tools/fsck.runway";

int file_rd(void *ctx, uint64_t off, void *buf, size_t len) {
    file_t *fl = ctx;
    if (fseek(fl->f, (long)off, SEEK_SET)) return -1;
    return fread(buf, 1, len, fl->f) == len ? 0 : -1;
}
int file_wr(void *ctx, uint64_t off, const void *buf, size_t len) {
    file_t *fl = ctx;
    if (fseek(fl->f, (long)off, SEEK_SET)) return -1;
    return fwrite(buf, 1, len, fl->f) == len ? 0 : -1;
}
int file_fl(void *ctx) { file_t *fl = ctx; return fflush(fl->f) ? -1 : 0; }

void open_store(rwy_store_t *s, rwy_block_dev_t *dev, file_t *fl, const char *path) {
    fl->f = fopen(path, "r+b");
    if (!fl->f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    dev->read = file_rd; dev->write = file_wr; dev->flush = file_fl;
    dev->ctx = fl; dev->size = RWY_STORE_SIZE; dev->block_size = RWY_BLOCK_SIZE;
    if (rwy_mount(s, dev)) { fprintf(stderr, "mount failed %s\n", path); exit(1); }
}

void copy_file(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "wb");
    static char buf[1 << 20];
    size_t n;
    if (!fa || !fb) { fprintf(stderr, "copy failed %s -> %s\n", a, b); exit(1); }
    while ((n = fread(buf, 1, sizeof(buf), fa)) > 0)
        if (fwrite(buf, 1, n, fb) != n) { fprintf(stderr, "copy failed\n"); exit(1); }
    fclose(fa); fclose(fb);
}

int run_checker(const char *p) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s %s >/dev/null 2>&1", g_checkbin, p);
    return system(cmd) >> 8;
}

/* Run the checker with extra arguments and keep its output; returns the exit status. */
int run_checker_args(const char *args, const char *p, const char *env) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s %s %s %s >%s 2>&1", env, g_checkbin, args, p, tpath("rwy_bootcfg_chk.log"));
    return system(cmd) >> 8;
}
int chk_has(const char *sub) {
    static char b[1 << 16];
    FILE *f = fopen(tpath("rwy_bootcfg_chk.log"), "r");
    size_t n = f ? fread(b, 1, sizeof(b) - 1, f) : 0;
    if (f) fclose(f);
    b[n] = 0;
    return strstr(b, sub) != NULL;
}

rwy_store_t S;

int txn(int rc_op) { /* op already ran inside an open txn */
    int rc;
    if (rc_op) { rwy_abort(&S); return rc_op; }
    rc = rwy_commit(&S);
    if (rc && S.in_txn) rwy_abort(&S);       /* a failed commit leaves the transaction open */
    return rc;
}
int mk(const char *p)   { rwy_begin(&S); return txn(rwy_mkdir(&S, p, RWY_S_IFDIR | 0700, 1000, 1000)); }
int cr(const char *p)   { rwy_begin(&S); return txn(rwy_create(&S, p, RWY_S_IFREG | 0600, 1000, 1000)); }
int sy(const char *p, const char *t) { rwy_begin(&S); return txn(rwy_symlink(&S, p, t, 1000, 1000)); }
int ln(const char *a, const char *b) { rwy_begin(&S); return txn(rwy_link(&S, a, b)); }
int rm(const char *p)   { rwy_begin(&S); return txn(rwy_unlink(&S, p)); }
int rd(const char *p)   { rwy_begin(&S); return txn(rwy_rmdir(&S, p)); }
int mv(const char *a, const char *b) { rwy_begin(&S); return txn(rwy_rename(&S, a, b)); }
int wr(const char *p, uint64_t pos, const char *d) {
    rwy_begin(&S); return txn(rwy_write_path(&S, p, pos, d, strlen(d)));
}
int tr(const char *p, uint64_t n) { rwy_begin(&S); return txn(rwy_truncate(&S, p, n)); }
int ch(const char *p)   { rwy_begin(&S); return txn(rwy_chmod(&S, p, 0600)); }
int co(const char *p)   { rwy_begin(&S); return txn(rwy_chown(&S, p, 5, 5)); }
int ut(const char *p)   { rwy_begin(&S); return txn(rwy_utimens(&S, p, 1, 0, 2, 0)); }
int sx(const char *p)   { rwy_begin(&S); return txn(rwy_setxattr(&S, p, "user.x", "v", 1, 0)); }
int rx(const char *p)   { rwy_begin(&S); return txn(rwy_removexattr(&S, p, "user.x")); }

void priv(int on) { rwy_bootcfg_set_priv(&S, on); }
void as_user(void) { rwy_set_actor(&S, RWY_ACTOR_USER); }
void as_sys(void)  { rwy_set_actor(&S, RWY_ACTOR_SYSTEM); }

/* The record is where BOOTED, REASON, RECOVER_NEXT and the slot states live. */
uint64_t gen(void) { return S.sb.generation; }
uint64_t rseq(void) { return S.rec_seq; }
static void rec_edit(void (*fn)(uint8_t *, const void *), const void *arg) {
    uint8_t r[RWY_REC_PAYLOAD];
    rwy_record_read(&S, r);
    fn(r, arg);
    if (rwy_record_write(&S, r)) { printf("FAIL: record write (line %d)\n", __LINE__); failures++; }
}
static void rec_booted(uint8_t *r, const void *a) { rwy_ss_set_booted(r, a); }
static void rec_reason(uint8_t *r, const void *a) { rwy_ss_set_reason(r, *(const int *)a); }
void set_booted(const char *name) { rec_edit(rec_booted, name); }
void set_reason(int code) { rec_edit(rec_reason, &code); }

/* Create a path through the core while bypassing the schema (test fixture). */
void seed(const char *p, const char *content) {
    priv(1);
    rwy_begin(&S);
    if (rwy_create(&S, p, RWY_S_IFREG | 0644, 0, 0) || (content && rwy_write_path(&S, p, 0, content, strlen(content))))
        { printf("FAIL: seed %s\n", p); failures++; rwy_abort(&S); }
    else rwy_commit(&S);
    priv(0);
}
void seed_dir(const char *p) {
    priv(1); rwy_begin(&S);
    if (rwy_mkdir(&S, p, RWY_S_IFDIR | 0755, 0, 0)) { printf("FAIL: seed dir %s\n", p); failures++; rwy_abort(&S); }
    else rwy_commit(&S);
    priv(0);
}

int read_all(const char *p, char *buf, size_t cap) {
    uint64_t o;
    size_t n = cap - 1;
    int rc = rwy_resolve_ext(&S, p, 0, 1, &o);
    if (rc) return rc;
    rc = rwy_read(&S, o, 0, buf, &n);
    if (rc) return rc;
    buf[n] = 0;
    return (int)n;
}

void same_text(const char *p, const char *want, const char *msg) {
    char b[8200];
    int n = read_all(p, b, sizeof(b));
    CHECK(n >= 0 && (size_t)n == strlen(want) && !memcmp(b, want, (size_t)n), msg);
}

long num(const char *path) {          /* -1 if absent, -2 if not a decimal line */
    char b[64];
    int n = read_all(path, b, sizeof(b));
    long v = 0;
    if (n < 0) return -1;
    if (n < 2 || b[n - 1] != '\n') return -2;
    for (int i = 0; i < n - 1; i++) { if (b[i] < '0' || b[i] > '9') return -2; v = v * 10 + (b[i] - '0'); }
    return v;
}
long rev(int n) { char p[64]; snprintf(p, sizeof(p), T "/CONFIG%d/REVISION", n); return num(p); }
long counter(void) { return num(T "/REVISION"); }

/* Create an incomplete slot with a one-byte KERNEL and an empty STATE. */
void slot(int n) {
    char d[64], k[64], st[64];
    snprintf(d, sizeof(d), T "/CONFIG%d", n);
    snprintf(k, sizeof(k), T "/CONFIG%d/KERNEL", n);
    snprintf(st, sizeof(st), T "/CONFIG%d/STATE", n);
    CHECK(mk(d) == OK && cr(k) == OK && wr(k, 0, "k") == OK && cr(st) == OK, "slot setup");
}
int install(int n) {
    char st[64];
    snprintf(st, sizeof(st), T "/CONFIG%d/STATE", n);
    return wr(st, 0, "INSTALLED\n");
}
void drop(int n) {                      /* delete a whole slot (all entries removable) */
    static const char *const f[] = { "KERNEL", "STATE", "REVISION" };
    char p[96];
    for (unsigned i = 0; i < 3; i++) { snprintf(p, sizeof(p), T "/CONFIG%d/%s", n, f[i]); rm(p); }
    snprintf(p, sizeof(p), T "/CONFIG%d", n);
    CHECK(rd(p) == OK, "slot removed");
}
void set_counter(const char *v) {       /* privileged fixture: "" removes it */
    priv(1);
    if (!*v) rm(T "/REVISION");
    else {
        if (num(T "/REVISION") == -1) cr(T "/REVISION");
        tr(T "/REVISION", 0);
        wr(T "/REVISION", 0, v);
    }
    priv(0);
}

char logbuf[2048];
void logfn(void *ctx, const char *line) {
    (void)ctx;
    if (strlen(logbuf) + strlen(line) + 2 < sizeof(logbuf)) { strcat(logbuf, line); strcat(logbuf, "\n"); }
}
int has(const char *sub) { return strstr(logbuf, sub) != NULL; }

int sel_with(rwy_store_t *s, rwy_boot_choice_t *c) {
    logbuf[0] = 0;
    rwy_set_actor(s, RWY_ACTOR_SYSTEM);
    int rc = rwy_bootcfg_select(s, c, logfn, NULL);
    rwy_set_actor(s, RWY_ACTOR_USER);
    return rc;
}
int sel(rwy_boot_choice_t *c) { return sel_with(&S, c); }
int text_is(const char *p, const char *want) {
    char b[128];
    int n = read_all(p, b, sizeof(b));
    return n >= 0 && (size_t)n == strlen(want) && !memcmp(b, want, (size_t)n);
}
int booted_is(const char *want) {
    char b[64];
    return rwy_readlink(&S, T "/BOOTED", b, sizeof(b)) == OK && !strcmp(b, want);
}
void set_state(int n, const char *v) {   /* finalized slot: USER may write OK / FAILED */
    char p[64];
    snprintf(p, sizeof(p), T "/CONFIG%d/STATE", n);
    priv(1); tr(p, 0); wr(p, 0, v); priv(0);
}
void set_file(int n, const char *f, const char *v) {   /* fixture, bypasses the schema */
    char p[64];
    snprintf(p, sizeof(p), T "/CONFIG%d/%s", n, f);
    priv(1);
    if (num(p) == -1 && read_all(p, (char[4]){0}, 4) < 0) cr(p); else tr(p, 0);
    wr(p, 0, v);
    priv(0);
}
/* slot n installed (finalized, revision assigned in call order) and set to `state` */
void fin(int n, const char *state) {
    slot(n);
    EXPECT(install(n), OK, "fixture install");
    if (strcmp(state, "INSTALLED")) { char v[16]; snprintf(v, sizeof(v), "%s\n", state); set_state(n, v); }
}
void fresh_subtree(void) { CHECK(mk(T) == OK && mk(T "/BLOBS") == OK, "subtree"); }

void mkrecovery(void) {      /* a sealed RECOVERY made the way mkstore does */
    unlock(1); as_sys();
    CHECK(mk(T "/RECOVERY") == OK && cr(T "/RECOVERY/KERNEL") == OK && wr(T "/RECOVERY/KERNEL", 0, "rk") == OK, "RECOVERY fixture");
    CHECK(cr(T "/RECOVERY/STATE") == OK && wr(T "/RECOVERY/STATE", 0, "FAILED\n") == OK, "RECOVERY/STATE fixture");
    as_user(); unlock(0);
}

void unlock(int on) { as_sys(); rwy_bootcfg_unlock_recovery(&S, on); as_user(); }

int fi_rd(void *c, uint64_t o, void *b, size_t l) { fi_t *f = c; return f->inner.read(f->inner.ctx, o, b, l); }
int fi_wr(void *c, uint64_t o, const void *b, size_t l) {
    fi_t *f = c;
    if (f->fail_at == ++f->point) return -1;
    return f->inner.write(f->inner.ctx, o, b, l);
}
int fi_fl(void *c) {
    fi_t *f = c;
    if (f->fail_at == ++f->point) return -1;
    return f->inner.flush(f->inner.ctx);
}
