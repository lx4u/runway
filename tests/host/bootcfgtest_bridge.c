/* Fault injection over the bridge commit: the slot, the content names and the links come in one transaction. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "bootcfgtest.h"
#include "rwy_bls.h"
#include "rwy_blob.h"

#define CONF "linux /tok/%s/linux\ninitrd /tok/%s/initrd1\ninitrd /tok/%s/initrd2\n"

static char kernel[6001], initrd1[4001], initrd2[4101];

/* The files that kernel-install leaves in BLOBS/ for the version `v`. */
static void bridge_file(const char *v, const char *file, const char *data) {
    char p[96];
    snprintf(p, sizeof(p), T "/BLOBS/tok%%2F%s%%2F%s", v, file);
    CHECK(cr(p) == OK && wr(p, 0, data) == OK, "bridge file");
}

static void bridge_files(const char *v) {
    bridge_file(v, "linux", kernel);
    bridge_file(v, "initrd1", initrd1);
    bridge_file(v, "initrd2", initrd2);
}

static int commit_entry(rwy_store_t *s, const char *v) {
    char conf[160];
    int n = snprintf(conf, sizeof(conf), CONF, v, v, v);
    char entry[32];
    snprintf(entry, sizeof(entry), "%s.conf", v);
    return rwy_bls_commit_type1(s, entry, conf, (size_t)n, 3);
}

/* Names of 64 hex digits in BLOBS/. */
static int content_names(void) {
    char n[256];
    int count = 0;
    for (size_t i = 0; rwy_readdir_nth(&S, T "/BLOBS", i, n, sizeof(n), NULL) == OK; i++)
        if (rwy_bootcfg_content_name(n, strlen(n))) count++;
    return count;
}

static int staged(void) {
    rwy_attr_t a;
    return rwy_stat(&S, T "/BLOBS/" RWY_BLOB_STAGE_NAME, &a) == OK;
}

/* The slot is not there, and nothing of it is: no content name made, no staging file, the bridge files plain. */
static void check_untouched(const char *slot_dir, int names_before, const char *what) {
    rwy_attr_t a, k;
    CHECK(rwy_stat(&S, slot_dir, &a) != OK, what);
    CHECK(content_names() == names_before && !staged(), "no content name and no staging file without the slot");
    CHECK(rwy_stat(&S, T "/BLOBS/tok%2Fv2%2Flinux", &k) == OK && k.nlink == 1, "bridge kernel is plain");
}

/* The slot is complete: every payload is a content blob, the joined initrd has its own hash. */
static void check_complete(const char *slot_dir, int names_after, const char *what) {
    char p[96], name[RWY_BLOB_NAME_SIZE], cp[128];
    uint8_t d[32];
    rwy_attr_t a, c;
    const char *files[] = { "KERNEL", "INITRD" };

    for (int i = 0; i < 2; i++) {
        snprintf(p, sizeof(p), "%s/%s", slot_dir, files[i]);
        CHECK(rwy_stat(&S, p, &a) == OK && a.nlink >= 2, what);
        CHECK(rwy_blob_hash_path(&S, p, d) == OK, "hash the payload");
        rwy_blob_name(d, name);
        snprintf(cp, sizeof(cp), T "/BLOBS/%s", name);
        CHECK(rwy_stat(&S, cp, &c) == OK && c.oid == a.oid, "payload is the blob of its hash");
    }
    snprintf(p, sizeof(p), "%s/KERNEL", slot_dir);
    CHECK(rwy_stat(&S, p, &a) == OK && rwy_stat(&S, T "/BLOBS/tok%2Fv2%2Flinux", &c) == OK && a.oid == c.oid,
          "the bridge kernel is the blob");
    snprintf(p, sizeof(p), "%s/STATE", slot_dir);
    CHECK(text_is(p, "INSTALLED\n"), "slot is INSTALLED");
    CHECK(content_names() == names_after && !staged(), "kernel and joined initrd have a name, no staging file");
}

/* `again`: an entry with the same data is committed first, so the second commit finds the blobs and relinks. */
void fi_bridge(const char *base, int again) {
    const char *img = tpath("rwy_fi_bridge_base.img"), *cr_img = tpath("rwy_fi_bridge_crash.img");
    file_t fl;
    rwy_block_dev_t inner, dev, d2;
    fi_t fi;
    long pts = 0, complete = 0, untouched = 0;
    const char *slot_dir = again ? T "/CONFIG2" : T "/CONFIG1";

    printf("== fault injection: bridge commit (%s) ==\n", again ? "blobs already stored" : "new blobs");
    memset(kernel, 'K', sizeof(kernel) - 1);
    memset(initrd1, 'a', sizeof(initrd1) - 1);
    memset(initrd2, 'b', sizeof(initrd2) - 1);
    copy_file(base, img);
    open_store(&S, &d2, &fl, img);
    fresh_subtree();
    if (again) {
        bridge_files("v1");
        EXPECT(commit_entry(&S, "v1"), OK, "first entry");
    }
    bridge_files("v2");
    int names_before = content_names();
    fclose(fl.f);
    inner.read = file_rd; inner.write = file_wr; inner.flush = file_fl; inner.ctx = &fl;
    inner.size = RWY_STORE_SIZE; inner.block_size = RWY_BLOCK_SIZE;
    dev.read = fi_rd; dev.write = fi_wr; dev.flush = fi_fl; dev.ctx = &fi;
    dev.size = RWY_STORE_SIZE; dev.block_size = RWY_BLOCK_SIZE;
    for (long pt = 0; pt <= 600; pt++) {
        rwy_store_t s;
        rwy_attr_t a;
        copy_file(img, cr_img);
        fl.f = fopen(cr_img, "r+b");
        fi.inner = inner; fi.point = 0; fi.fail_at = pt ? pt : -1;
        if (rwy_mount(&s, &dev) == OK) (void)commit_entry(&s, "v2");
        if (!pt) pts = fi.point;
        fclose(fl.f);
        if (pt > pts && pts) break;
        fl.f = fopen(cr_img, "r+b");
        fi.fail_at = -1;
        CHECK(rwy_mount(&S, &inner) == OK, "remount after fault");
        if (rwy_stat(&S, slot_dir, &a) == OK) { complete++; check_complete(slot_dir, 2, "slot complete"); }
        else { untouched++; check_untouched(slot_dir, names_before, "slot absent"); }
        fclose(fl.f);
        CHECK(run_checker(cr_img) == 0, "checker passes on crash state");
    }
    CHECK(pts > 10 && complete > 0 && untouched > 0, "points reached both the old and the new state");
    printf("  %ld points: %ld old, %ld new\n", pts, untouched, complete);
}
