/* Orphan content blobs: a blob lives while a slot, RECOVERY/ or WDT/ links it. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "bootcfgtest.h"
#include "rwy_crypto.h"

#define HASH_K "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
#define HASH_I "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100"
#define BLOB_K T "/BLOBS/" HASH_K
#define BLOB_I T "/BLOBS/" HASH_I

static uint32_t nlinks(const char *p) {
    rwy_attr_t a;
    return rwy_stat(&S, p, &a) == OK ? a.nlink : 0;
}

static uint32_t used_blocks(void) {
    uint32_t used, usable;
    rwy_pool_usage(&S, &used, &usable);
    return used;
}

/* A content blob of `blocks` blocks, made by SYSTEM the way an install does: staged, then renamed. */
static void blob(const char *path, char fill, unsigned blocks) {
    static char data[4 * 4096 + 1];
    memset(data, fill, blocks * 4096u);
    data[blocks * 4096u] = 0;
    as_sys();
    CHECK(cr(T "/BLOBS/stage") == OK && wr(T "/BLOBS/stage", 0, data) == OK && mv(T "/BLOBS/stage", path) == OK,
          "content blob made");
    as_user();
}

/* An open slot whose KERNEL is a link to the blob. */
static void blob_slot(int n, const char *path) {
    char d[64], k[64], st[64];
    snprintf(d, sizeof(d), T "/CONFIG%d", n);
    snprintf(k, sizeof(k), T "/CONFIG%d/KERNEL", n);
    snprintf(st, sizeof(st), T "/CONFIG%d/STATE", n);
    CHECK(mk(d) == OK && ln(path, k) == OK && cr(st) == OK, "slot with a linked kernel");
}

static void rm_slot(int n) {
    char d[64];
    snprintf(d, sizeof(d), T "/CONFIG%d", n);
    EXPECT(rd(d), OK, "USER removes the slot");
}

/* The sweep cases that need no other store: sharing, last slot, payload unlink and rename. */
static void sweep_cases(void) {
    blob(BLOB_K, 'k', 3);
    blob_slot(1, BLOB_K);
    blob_slot(2, BLOB_K);
    CHECK(nlinks(BLOB_K) == 3, "blob has its name and two slots");

    rm_slot(1);
    CHECK(nlinks(BLOB_K) == 2, "another slot links it: the blob stays");
    rm_slot(2);
    CHECK(nlinks(BLOB_K) == 0, "last slot gone: the blob is swept");

    blob(BLOB_K, 'k', 3);
    blob_slot(3, BLOB_K);
    EXPECT(rm(T "/CONFIG3/KERNEL"), OK, "USER unlinks the payload");
    CHECK(nlinks(BLOB_K) == 0, "last payload link gone: the blob is swept");
    rm_slot(3);

    blob(BLOB_K, 'k', 3);
    blob_slot(4, BLOB_K);
    CHECK(cr(T "/CONFIG4/INITRD") == OK && wr(T "/CONFIG4/INITRD", 0, "p") == OK, "plain initrd");
    EXPECT(mv(T "/CONFIG4/INITRD", T "/CONFIG4/KERNEL"), OK, "USER renames over the payload name");
    CHECK(nlinks(BLOB_K) == 0, "payload replaced: the blob is swept");
    rm_slot(4);

    blob(BLOB_K, 'k', 3);
    blob(BLOB_I, 'i', 1);
    blob_slot(5, BLOB_K);
    CHECK(nlinks(BLOB_I) == 1, "a blob with no slot yet");
    rm_slot(5);
    CHECK(nlinks(BLOB_K) == 0 && nlinks(BLOB_I) == 0, "the sweep takes every orphan");
}

/* One transaction removes the old slot and installs the new one: the shared blob must stay. */
static void update_cases(void) {
    blob(BLOB_K, 'k', 3);
    blob_slot(1, BLOB_K);
    rwy_begin(&S);
    EXPECT(rwy_rmdir(&S, T "/CONFIG1") || rwy_mkdir(&S, T "/CONFIG2", RWY_S_IFDIR | 0700, 1000, 1000) ||
           rwy_link(&S, BLOB_K, T "/CONFIG2/KERNEL") || rwy_create(&S, T "/CONFIG2/STATE", RWY_S_IFREG | 0600, 1000, 1000),
           OK, "old slot goes, new one links the blob");
    EXPECT(txn(0), OK, "commit");
    CHECK(nlinks(BLOB_K) == 2, "same kernel: the blob stays");
    rm_slot(2);
    CHECK(nlinks(BLOB_K) == 0, "the new slot goes: swept");
}

/* An install makes the blob and links it in one transaction; a removal in the same transaction must not take it. */
static void new_blob_cases(void) {
    blob(BLOB_I, 'i', 1);
    blob_slot(1, BLOB_I);
    as_sys();
    rwy_begin(&S);
    EXPECT(rwy_create(&S, T "/BLOBS/stage", RWY_S_IFREG | 0600, 0, 0) ||
           rwy_write_path(&S, T "/BLOBS/stage", 0, "new", 3) || rwy_rename(&S, T "/BLOBS/stage", BLOB_K) ||
           rwy_rmdir(&S, T "/CONFIG1") || rwy_mkdir(&S, T "/CONFIG2", RWY_S_IFDIR | 0700, 1000, 1000) ||
           rwy_link(&S, BLOB_K, T "/CONFIG2/KERNEL"), OK, "make, link and remove in one transaction");
    EXPECT(txn(0), OK, "commit");
    as_user();
    CHECK(nlinks(BLOB_K) == 2, "the new blob is linked in its own transaction: it stays");
    CHECK(nlinks(BLOB_I) == 0, "the replaced blob is swept");
    rm_slot(2);

    /* The known limit: a blob made without a link is taken by any removal that marks the transaction. */
    blob(BLOB_K, 'k', 1);
    blob(BLOB_I, 'i', 1);
    blob_slot(1, BLOB_I);
    CHECK(nlinks(BLOB_K) == 1, "a blob without a link");
    rm_slot(1);
    CHECK(nlinks(BLOB_K) == 0, "swept by an unrelated removal");
}

static void other_links(void) {
    blob(BLOB_K, 'k', 2);
    blob_slot(1, BLOB_K);
    mkrecovery();
    unlock(1); as_sys();
    EXPECT(rm(T "/RECOVERY/KERNEL"), OK, "free the RECOVERY name");
    EXPECT(ln(BLOB_K, T "/RECOVERY/KERNEL"), OK, "RECOVERY links the blob");
    unlock(0); as_user();
    rm_slot(1);
    CHECK(nlinks(BLOB_K) == 2, "RECOVERY still links it: the blob stays");

    unlock(1); as_sys();
    EXPECT(mk(T "/WDT"), OK, "WDT");
    EXPECT(ln(BLOB_K, T "/WDT/A.EFI"), OK, "WDT links the blob");
    EXPECT(rm(T "/RECOVERY/KERNEL"), OK, "RECOVERY lets go");
    unlock(0); as_user();
    CHECK(nlinks(BLOB_K) == 2, "WDT still links it: the blob stays");
    unlock(1); as_sys();
    EXPECT(rm(T "/WDT/A.EFI"), OK, "WDT lets go");
    CHECK(nlinks(BLOB_K) == 0, "last link gone: swept");
    EXPECT(rd(T "/WDT"), OK, "WDT removed");
    EXPECT(cr(T "/RECOVERY/KERNEL") || wr(T "/RECOVERY/KERNEL", 0, "rk"), OK, "RECOVERY kernel restored");
    unlock(0); as_user();
}

/* QE1 with content names: the last slot of a blob frees it, in the count and in fact. */
static void evict_numbers(void) {
    char one[1][32] = { "CONFIG1" }, two[2][32] = { "CONFIG1", "CONFIG2" };
    uint32_t used0, f1, f2, f12;
    blob(BLOB_K, 'k', 3);
    blob_slot(1, BLOB_K);
    blob_slot(2, BLOB_K);
    rwy_begin(&S);
    EXPECT(rwy_bootcfg_evict_blocks(&S, one, 1, &f1), OK, "count one slot");
    EXPECT(rwy_bootcfg_evict_blocks(&S, two, 2, &f12), OK, "count both slots");
    rwy_abort(&S);
    rwy_begin(&S);
    {
        char o2[1][32] = { "CONFIG2" };
        EXPECT(rwy_bootcfg_evict_blocks(&S, o2, 1, &f2), OK, "count the other slot");
    }
    rwy_abort(&S);
    CHECK(f12 == f1 + f2 + 3, "the blob is freed with the last slot");
    used0 = used_blocks();
    rm_slot(1);
    CHECK(used0 - used_blocks() == f1, "one slot frees what was counted");
    rm_slot(2);
    CHECK(used0 - used_blocks() == f12, "both slots free what was counted");
}

/* Both stores: the content name rule and the sweep give the same result. */
static void rules_and_sweep(void) {
    fresh_subtree();
    CHECK(cr(T "/BLOBS/keep") == OK && wr(T "/BLOBS/keep", 0, "k") == OK, "a plain blob keeps BLOBS non-empty");
    CHECK(cr(T "/BLOBS/" HASH_K) == PERM, "USER cannot make a content name");
    sweep_cases();
    update_cases();
    new_blob_cases();
    other_links();
}

typedef int (*txn_op_t)(void);
typedef void (*after_t)(void);

static void fi_open(rwy_block_dev_t *inner, rwy_block_dev_t *dev, file_t *fl, fi_t *fi) {
    inner->read = file_rd; inner->write = file_wr; inner->flush = file_fl; inner->ctx = fl;
    inner->size = RWY_STORE_SIZE; inner->block_size = RWY_BLOCK_SIZE;
    dev->read = fi_rd; dev->write = fi_wr; dev->flush = fi_fl; dev->ctx = fi;
    dev->size = RWY_STORE_SIZE; dev->block_size = RWY_BLOCK_SIZE;
}

/* Run `op` in one transaction, failing the Nth write or flush; `after` checks every crash state. */
static void fi_sweep(const char *img, const char *what, txn_op_t op, after_t after) {
    const char *cr_img = tpath("rwy_fi_sweep_crash.img");
    file_t fl;
    rwy_block_dev_t inner, dev;
    fi_t fi;
    long pts = 0;
    printf("== fault injection: %s ==\n", what);
    fi_open(&inner, &dev, &fl, &fi);
    for (long pt = 0; pt <= 400; pt++) {
        copy_file(img, cr_img);
        fl.f = fopen(cr_img, "r+b");
        fi.inner = inner; fi.point = 0; fi.fail_at = pt ? pt : -1;
        if (rwy_mount(&S, &dev) == OK && rwy_begin(&S) == OK) {
            as_user();
            if (op() == OK) (void)rwy_commit(&S);
            else (void)rwy_abort(&S);
        }
        if (!pt) pts = fi.point;
        fclose(fl.f);
        if (pt > pts && pts) break;
        fl.f = fopen(cr_img, "r+b");
        fi.fail_at = -1;
        CHECK(rwy_mount(&S, &inner) == OK, "remount after fault");
        after();
        fclose(fl.f);
        CHECK(run_checker(cr_img) == 0, "checker passes on crash state");
    }
    CHECK(pts > 0, "counted persistence points");
}

static int op_last_slot(void) { return rwy_rmdir(&S, T "/CONFIG1"); }

static void after_last_slot(void) {
    uint32_t slot = nlinks(T "/CONFIG1/KERNEL"), b = nlinks(BLOB_K);
    CHECK((slot == 2 && b == 2) || (slot == 0 && b == 0), "slot and blob go together");
}

static int op_update(void) {
    return rwy_rmdir(&S, T "/CONFIG1") || rwy_mkdir(&S, T "/CONFIG2", RWY_S_IFDIR | 0700, 1000, 1000) ||
           rwy_link(&S, BLOB_K, T "/CONFIG2/KERNEL") || rwy_create(&S, T "/CONFIG2/STATE", RWY_S_IFREG | 0600, 1000, 1000);
}

static void after_update(void) {
    uint32_t old = nlinks(T "/CONFIG1/KERNEL"), new = nlinks(T "/CONFIG2/KERNEL");
    CHECK((old == 2 && new == 0 && nlinks(BLOB_K) == 2) || (old == 0 && new == 2 && nlinks(BLOB_K) == 2),
          "the blob is linked by the old slot or by the new one");
}

static void fi_cases(const char *base) {
    const char *img = tpath("rwy_fi_sweep_base.img");
    file_t fl;
    rwy_block_dev_t d;
    copy_file(base, img);
    open_store(&S, &d, &fl, img);
    fresh_subtree();
    blob(BLOB_K, 'k', 3);
    blob_slot(1, BLOB_K);
    fclose(fl.f);
    fi_sweep(img, "sweep of the last slot", op_last_slot, after_last_slot);
    fi_sweep(img, "update with the same kernel", op_update, after_update);
}

static void on_plain(const char *base) {
    const char *img = tpath("rwy_test_sweep.img");
    file_t fl;
    rwy_block_dev_t d;
    printf("== orphan content blobs ==\n");
    copy_file(base, img);
    open_store(&S, &d, &fl, img);
    rules_and_sweep();
    evict_numbers();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes");
    fi_cases(base);
}

static void on_encrypted(void) {
    const char *img = tpath("rwy_test_sweep_enc.img"), *pass = "SweepTestPassphrase!";
    char cmd[512];
    file_t fl;
    rwy_block_dev_t raw, plain;
    rwy_crypto_dev_ctx_t cctx;
    rwy_keyslot_header_t kh;
    uint8_t dek[32];
    printf("== orphan content blobs: encrypted store ==\n");
    snprintf(cmd, sizeof(cmd), "%s --encrypt --passphrase '%s' %s >/dev/null", mkstore_path(), pass, img);
    CHECK(system(cmd) == 0, "mkstore --encrypt");
    fl.f = fopen(img, "r+b");
    raw.read = file_rd; raw.write = file_wr; raw.flush = file_fl; raw.ctx = &fl;
    raw.size = RWY_STORE_SIZE; raw.block_size = RWY_BLOCK_SIZE;
    CHECK(rwy_keyslot_read_from_dev(&raw, &kh) == 0 && rwy_keyslot_unwrap(&kh, pass, dek) == 0, "unwrap the key");
    CHECK(rwy_crypto_dev_wrap(&plain, &cctx, &raw, dek, RWY_SB_BLOCKS) == 0, "wrap");
    CHECK(rwy_mount(&S, &plain) == OK, "mount");
    rules_and_sweep();
    evict_numbers();
    fclose(fl.f);
    snprintf(cmd, sizeof(cmd), "%s --key '%s' %s >/dev/null 2>&1", g_checkbin, pass, img);
    CHECK(system(cmd) == 0, "checker passes with the key");
}

static void on_mirror(const char *base) {
    const char *img0 = tpath("rwy_test_sweep_m0.img"), *img1 = tpath("rwy_test_sweep_m1.img");
    file_t f0, f1;
    rwy_block_dev_t b0, b1, top;
    rwy_mirror_dev_t m;
    printf("== orphan content blobs: mirror ==\n");
    copy_file(base, img0);
    copy_file(base, img1);
    f0.f = fopen(img0, "r+b");
    f1.f = fopen(img1, "r+b");
    b0.read = file_rd; b0.write = file_wr; b0.flush = file_fl; b0.ctx = &f0;
    b0.size = RWY_STORE_SIZE; b0.block_size = RWY_BLOCK_SIZE;
    b1 = b0;
    b1.ctx = &f1;
    rwy_mirror_init(&m);
    rwy_mirror_add(&m, &b0);
    rwy_mirror_add(&m, &b1);
    rwy_mirror_wrap(&top, &m);
    CHECK(rwy_mount(&S, &top) == OK, "mount over the mirror");
    rules_and_sweep();
    evict_numbers();
    fclose(f0.f);
    fclose(f1.f);
    CHECK(run_checker(img0) == 0 && run_checker(img1) == 0, "both replicas pass the checker");
}

void test_sweep(const char *base) {
    on_plain(base);
    on_encrypted();
    on_mirror(base);
}
