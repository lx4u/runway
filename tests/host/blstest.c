/*
 * blstest.c — Unit tests for synthetic BLS interface.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "testdir.h"
#include "rwy_bls.h"
#include "rwy_blob.h"
#include "rwy_core.h"
#include "rwy_internal.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        failures++; \
    } \
} while (0)

static void test_bls_name_parse(void) {
    char canon[RWY_ENTRY_MAX + 1];
    int has_counter = 0;
    uint32_t left = 0;
    int32_t done = -1;

    printf("== bls name: parsing ==\n");

    /* Canonical names without counter */
    CHECK(rwy_bls_name_parse("foo.conf", 8, canon, sizeof(canon), &has_counter, &left, &done) == RWY_OK, "parse foo.conf");
    CHECK(has_counter == 0 && !strcmp(canon, "foo.conf"), "canon foo.conf");

    CHECK(rwy_bls_name_parse("foo.efi", 7, canon, sizeof(canon), &has_counter, &left, &done) == RWY_OK, "parse foo.efi");
    CHECK(has_counter == 0 && !strcmp(canon, "foo.efi"), "canon foo.efi");

    /* Single counter */
    CHECK(rwy_bls_name_parse("foo+3.conf", 10, canon, sizeof(canon), &has_counter, &left, &done) == RWY_OK, "parse foo+3.conf");
    CHECK(has_counter == 1 && left == 3 && done == -1 && !strcmp(canon, "foo.conf"), "canon foo+3.conf");

    CHECK(rwy_bls_name_parse("foo+0.efi", 9, canon, sizeof(canon), &has_counter, &left, &done) == RWY_OK, "parse foo+0.efi");
    CHECK(has_counter == 1 && left == 0 && done == -1 && !strcmp(canon, "foo.efi"), "canon foo+0.efi");

    /* Two counters (+left-done) */
    CHECK(rwy_bls_name_parse("foo+3-1.conf", 12, canon, sizeof(canon), &has_counter, &left, &done) == RWY_OK, "parse foo+3-1.conf");
    CHECK(has_counter == 1 && left == 3 && done == 1 && !strcmp(canon, "foo.conf"), "canon foo+3-1.conf");

    CHECK(rwy_bls_name_parse("foo+0-2.efi", 11, canon, sizeof(canon), &has_counter, &left, &done) == RWY_OK, "parse foo+0-2.efi");
    CHECK(has_counter == 1 && left == 0 && done == 2 && !strcmp(canon, "foo.efi"), "canon foo+0-2.efi");

    /* Version containing '+' character from Debian kernel */
    CHECK(rwy_bls_name_parse("tok-6.12.107+deb13-amd64+2.efi", 30, canon, sizeof(canon), &has_counter, &left, &done) == RWY_OK, "parse debian with counter");
    CHECK(has_counter == 1 && left == 2 && done == -1 && !strcmp(canon, "tok-6.12.107+deb13-amd64.efi"), "canon debian with counter");

    CHECK(rwy_bls_name_parse("tok-6.12.107+deb13-amd64.efi", 28, canon, sizeof(canon), &has_counter, &left, &done) == RWY_OK, "parse debian canonical");
    CHECK(has_counter == 0 && !strcmp(canon, "tok-6.12.107+deb13-amd64.efi"), "canon debian canonical");

    /* Ambiguous stem ending in counter */
    CHECK(rwy_bls_name_parse("foo+1+2.conf", 12, canon, sizeof(canon), &has_counter, &left, &done) == RWY_ERR_INVAL, "refuse foo+1+2.conf");
    CHECK(rwy_bls_name_parse("foo+1-2+3.conf", 14, canon, sizeof(canon), &has_counter, &left, &done) == RWY_ERR_INVAL, "refuse foo+1-2+3.conf");

    /* Invalid filenames */
    CHECK(rwy_bls_name_parse("+1.conf", 7, canon, sizeof(canon), &has_counter, &left, &done) == RWY_ERR_INVAL, "empty stem with counter");
    CHECK(rwy_bls_name_parse(".conf", 5, canon, sizeof(canon), &has_counter, &left, &done) == RWY_ERR_INVAL, "empty stem");
    CHECK(rwy_bls_name_parse("foo+.conf", 9, canon, sizeof(canon), &has_counter, &left, &done) == RWY_OK &&
          has_counter == 0 && !strcmp(canon, "foo+.conf"), "plus without digits is not counter");
    CHECK(rwy_bls_name_parse("foo.bin", 7, canon, sizeof(canon), &has_counter, &left, &done) == RWY_ERR_INVAL, "bad extension");
    CHECK(rwy_bls_name_parse("", 0, canon, sizeof(canon), &has_counter, &left, &done) == RWY_ERR_INVAL, "empty string");
}

static void test_bls_name_render(void) {
    char out[64];
    int n;

    printf("== bls name: rendering ==\n");

    /* State OK */
    n = rwy_bls_name_render("foo.conf", 8, "OK", 0, out, sizeof(out));
    CHECK(n == 8 && !strcmp(out, "foo.conf"), "render OK .conf");

    n = rwy_bls_name_render("tok.efi", 7, "OK", 0, out, sizeof(out));
    CHECK(n == 7 && !strcmp(out, "tok.efi"), "render OK .efi");

    /* State INSTALLED */
    n = rwy_bls_name_render("foo.conf", 8, "INSTALLED", 3, out, sizeof(out));
    CHECK(n == 10 && !strcmp(out, "foo+3.conf"), "render INSTALLED .conf");

    n = rwy_bls_name_render("tok.efi", 7, "INSTALLED", 1, out, sizeof(out));
    CHECK(n == 9 && !strcmp(out, "tok+1.efi"), "render INSTALLED .efi");

    /* State TESTING */
    n = rwy_bls_name_render("foo.conf", 8, "TESTING", 2, out, sizeof(out));
    CHECK(n == 10 && !strcmp(out, "foo+2.conf"), "render TESTING .conf");

    n = rwy_bls_name_render("foo.conf", 8, "TESTING", 0, out, sizeof(out));
    CHECK(n == 10 && !strcmp(out, "foo+0.conf"), "render TESTING last try");

    /* State FAILED */
    n = rwy_bls_name_render("foo.conf", 8, "FAILED", 0, out, sizeof(out));
    CHECK(n == 10 && !strcmp(out, "foo+0.conf"), "render FAILED .conf");

    n = rwy_bls_name_render("foo.conf", 8, "FAILED", 5, out, sizeof(out));
    CHECK(n == 10 && !strcmp(out, "foo+0.conf"), "render FAILED always +0");

    /* Buffer overflow */
    n = rwy_bls_name_render("foo.conf", 8, "INSTALLED", 3, out, 5);
    CHECK(n == RWY_ERR_NOSPC, "render buffer overflow");

    /* Invalid state */
    n = rwy_bls_name_render("foo.conf", 8, "UNKNOWN", 3, out, sizeof(out));
    CHECK(n == RWY_ERR_INVAL, "render unknown state");
}

static void test_bls_name_boot_count_path(void) {
    char out[128];
    int n;

    printf("== bls name: boot count path ==\n");

    n = rwy_bls_name_boot_count_path("foo.conf", 8, 2, out, sizeof(out));
    CHECK(n == 26 && !strcmp(out, "/loader/entries/foo+2.conf"), "path Type 1");

    n = rwy_bls_name_boot_count_path("tok.efi", 7, 1, out, sizeof(out));
    CHECK(n == 20 && !strcmp(out, "/EFI/Linux/tok+1.efi"), "path Type 2");
}

static void test_bls_name_rename(void) {
    int target_state = -1;

    printf("== bls name: rename matching ==\n");

    /* Bless good: drops counter suffix */
    CHECK(rwy_bls_name_match_rename("foo+3.conf", 10, "foo.conf", 8, &target_state) == RWY_OK &&
          target_state == RWY_BLS_STATE_OK, "bless good .conf");

    CHECK(rwy_bls_name_match_rename("foo+3-1.conf", 12, "foo.conf", 8, &target_state) == RWY_OK &&
          target_state == RWY_BLS_STATE_OK, "bless good with done counter");

    CHECK(rwy_bls_name_match_rename("tok+2.efi", 9, "tok.efi", 7, &target_state) == RWY_OK &&
          target_state == RWY_BLS_STATE_OK, "bless good .efi");

    /* Bless bad: counter set to +0 */
    CHECK(rwy_bls_name_match_rename("foo+2.conf", 10, "foo+0.conf", 10, &target_state) == RWY_OK &&
          target_state == RWY_BLS_STATE_FAILED, "bless bad .conf");

    CHECK(rwy_bls_name_match_rename("foo+2.conf", 10, "foo+0-1.conf", 12, &target_state) == RWY_OK &&
          target_state == RWY_BLS_STATE_FAILED, "bless bad with done counter");

    /* Prohibited renames */
    CHECK(rwy_bls_name_match_rename("foo.conf", 8, "foo+1.conf", 10, &target_state) == RWY_ERR_PERM,
          "prohibit rename from uncounted");

    CHECK(rwy_bls_name_match_rename("foo+2.conf", 10, "bar.conf", 8, &target_state) == RWY_ERR_PERM,
          "prohibit rename changing stem");

    CHECK(rwy_bls_name_match_rename("foo+2.conf", 10, "foo+1.conf", 10, &target_state) == RWY_ERR_PERM,
          "prohibit rename to non-zero counter");

    CHECK(rwy_bls_name_match_rename("foo+2.conf", 10, "foo+2.efi", 9, &target_state) == RWY_ERR_PERM,
          "prohibit rename changing type");
}

static void test_bls_name_match_entry(void) {
    printf("== bls name: entry matching ==\n");

    /* Exact matches */
    CHECK(rwy_bls_name_match_entry("foo.conf", "foo.conf") == 1, "exact match uncounted conf");
    CHECK(rwy_bls_name_match_entry("foo.efi", "foo.efi") == 1, "exact match uncounted efi");
    CHECK(rwy_bls_name_match_entry("foo+3.conf", "foo+3.conf") == 1, "exact match counted conf");
    CHECK(rwy_bls_name_match_entry("foo+3.efi", "foo+3.efi") == 1, "exact match counted efi");

    /* Tries-done 0 equivalence */
    CHECK(rwy_bls_name_match_entry("foo+3.conf", "foo+3-0.conf") == 1, "match +3 to +3-0 conf");
    CHECK(rwy_bls_name_match_entry("foo+3-0.conf", "foo+3.conf") == 1, "match +3-0 to +3 conf");
    CHECK(rwy_bls_name_match_entry("foo+3.efi", "foo+3-0.efi") == 1, "match +3 to +3-0 efi");
    CHECK(rwy_bls_name_match_entry("foo+3-0.efi", "foo+3.efi") == 1, "match +3-0 to +3 efi");
    CHECK(rwy_bls_name_match_entry("foo+0.efi", "foo+0-0.efi") == 1, "match +0 to +0-0 efi");

    /* Non-matches */
    CHECK(rwy_bls_name_match_entry("foo+3.efi", "foo+3-1.efi") == 0, "mismatch tries-done");
    CHECK(rwy_bls_name_match_entry("foo+3.efi", "foo+2.efi") == 0, "mismatch tries-left");
    CHECK(rwy_bls_name_match_entry("foo.efi", "foo+3-0.efi") == 0, "mismatch state");
    CHECK(rwy_bls_name_match_entry("foo+3.efi", "bar+3.efi") == 0, "mismatch stem");
    CHECK(rwy_bls_name_match_entry("foo+3.efi", "foo+3.conf") == 0, "mismatch ext");
    CHECK(rwy_bls_name_match_entry("foo+3.efi", NULL) == 0, "null query");
    CHECK(rwy_bls_name_match_entry(NULL, "foo+3.efi") == 0, "null rendered");
}

static void test_bls_parse_valid(void) {
    const char *text =
        "# BLS Type 1 Entry\n"
        "title Debian GNU/Linux 13\n"
        "version 6.12.107\n"
        "machine-id 0123456789abcdef\n"
        "sort-key 10-debian\n"
        "architecture x86-64\n"
        "linux /token/6.12.107/linux\n"
        "initrd /token/6.12.107/initrd\n"
        "devicetree /token/6.12.107/dtb\n"
        "options root=UUID=123 ro\n"
        "options quiet splash\n";
    rwy_bls_entry_t e;

    printf("== bls parse: valid entry ==\n");
    CHECK(rwy_bls_parse(text, strlen(text), &e) == RWY_OK, "parse valid entry");
    CHECK(e.has_linux && !strcmp(e.linux_path, "/token/6.12.107/linux"), "linux path");
    CHECK(e.num_initrd == 1 && !strcmp(e.initrd_paths[0], "/token/6.12.107/initrd"), "initrd path");
    CHECK(e.has_dtb && !strcmp(e.dtb_path, "/token/6.12.107/dtb"), "dtb path");
    CHECK(e.has_title && !strcmp(e.title, "Debian GNU/Linux 13"), "title");
    CHECK(!strcmp(e.options, "root=UUID=123 ro quiet splash"), "joined options");
    CHECK(e.meta_len > 0, "meta populated");
}

static void test_bls_parse_multiple_initrd(void) {
    char buf[1024];
    rwy_bls_entry_t e;
    size_t i;

    printf("== bls parse: multiple initrd ==\n");
    snprintf(buf, sizeof(buf), "linux /boot/vmlinuz\ninitrd /boot/ucode\ninitrd /boot/initramfs\n");
    CHECK(rwy_bls_parse(buf, strlen(buf), &e) == RWY_OK, "parse 2 initrds");
    CHECK(e.num_initrd == 2 && !strcmp(e.initrd_paths[0], "/boot/ucode") &&
          !strcmp(e.initrd_paths[1], "/boot/initramfs"), "2 initrd paths recorded");

    /* Exceed max initrd limit (8) */
    buf[0] = 0;
    snprintf(buf, sizeof(buf), "linux /boot/vmlinuz\n");
    for (i = 0; i < 9; i++) {
        char line[64];
        snprintf(line, sizeof(line), "initrd /boot/initrd%zu\n", i);
        strcat(buf, line);
    }
    CHECK(rwy_bls_parse(buf, strlen(buf), &e) == RWY_ERR_INVAL, "refuse 9 initrds");
}

static void test_bls_parse_faults(void) {
    const char *no_linux = "title Test\noptions quiet\n";
    const char *second_linux = "linux /boot/vmlinuz1\nlinux /boot/vmlinuz2\n";
    const char *overlay = "linux /boot/vmlinuz\ndevicetree-overlay /boot/ovl.dtbo\n";
    const char *efi_key = "linux /boot/vmlinuz\nefi /boot/uki.efi\n";
    const char *dotdot1 = "linux /../escape\n";
    const char *dotdot2 = "linux foo/../bar\n";
    const char *dotdot3 = "linux /foo/..\n";
    const char *dotdot4 = "linux ..\n";
    const char *bad_ctrl = "linux /boot/\x01vmlinuz\n";
    rwy_bls_entry_t e;

    printf("== bls parse: definite faults ==\n");

    /* Incomplete file is allowed by check_definite_faults, but rejected by parse */
    CHECK(rwy_bls_check_definite_faults(no_linux, strlen(no_linux)) == RWY_OK, "no linux allowed at flush");
    CHECK(rwy_bls_parse(no_linux, strlen(no_linux), &e) == RWY_ERR_INVAL, "no linux rejected at parse");

    /* Definite faults fail both */
    CHECK(rwy_bls_check_definite_faults(second_linux, strlen(second_linux)) == RWY_ERR_INVAL, "second linux fault");
    CHECK(rwy_bls_parse(second_linux, strlen(second_linux), &e) == RWY_ERR_INVAL, "second linux parse");

    CHECK(rwy_bls_check_definite_faults(overlay, strlen(overlay)) == RWY_ERR_INVAL, "devicetree-overlay fault");
    CHECK(rwy_bls_check_definite_faults(efi_key, strlen(efi_key)) == RWY_ERR_INVAL, "efi key fault");

    CHECK(rwy_bls_check_definite_faults(dotdot1, strlen(dotdot1)) == RWY_ERR_INVAL, "dotdot1 fault");
    CHECK(rwy_bls_check_definite_faults(dotdot2, strlen(dotdot2)) == RWY_ERR_INVAL, "dotdot2 fault");
    CHECK(rwy_bls_check_definite_faults(dotdot3, strlen(dotdot3)) == RWY_ERR_INVAL, "dotdot3 fault");
    CHECK(rwy_bls_check_definite_faults(dotdot4, strlen(dotdot4)) == RWY_ERR_INVAL, "dotdot4 fault");
    CHECK(rwy_bls_check_definite_faults(bad_ctrl, strlen(bad_ctrl)) == RWY_ERR_INVAL, "ctrl char in path");
}

static void test_bls_parse_fuzz(void) {
    char base[] =
        "title Linux Kernel Test\n"
        "version 6.12.1\n"
        "linux /token/version/linux\n"
        "initrd /token/version/initrd\n"
        "options root=/dev/sda1 quiet ro\n";
    char fuzzed[sizeof(base)];
    rwy_bls_entry_t e;
    size_t i, j;
    unsigned int seed = 12345;

    printf("== bls parse: fuzzing ==\n");
    for (i = 0; i < 2000; i++) {
        memcpy(fuzzed, base, sizeof(base));
        /* Mutate up to 5 random bytes */
        size_t nmut = 1 + (size_t)(rand_r(&seed) % 5);
        for (j = 0; j < nmut; j++) {
            size_t idx = (size_t)(rand_r(&seed) % (sizeof(base) - 1));
            fuzzed[idx] = (char)(rand_r(&seed) % 256);
        }
        /* Neither call must crash */
        (void)rwy_bls_check_definite_faults(fuzzed, sizeof(base) - 1);
        (void)rwy_bls_parse(fuzzed, sizeof(base) - 1, &e);
    }
}

static void test_bls_render_roundtrip(void) {
    rwy_bls_slot_info_t info;
    rwy_bls_entry_t e;
    char conf[2048];
    int n;

    printf("== bls render: roundtrip ==\n");
    memset(&info, 0, sizeof(info));
    snprintf(info.slot_name, sizeof(info.slot_name), "CONFIG1");
    snprintf(info.entry_name, sizeof(info.entry_name), "tok-6.12.107+deb13.conf");
    info.has_kernel = 1;
    info.has_initrd = 1;
    info.has_dtb = 1;
    info.has_label = 1;
    snprintf(info.label, sizeof(info.label), "Debian GNU/Linux 13");
    info.has_args = 1;
    snprintf(info.args, sizeof(info.args), "console=ttyS0 root=UUID=abcd quiet");
    const char *meta = "version 6.12.107+deb13\nmachine-id 0123456789abcdef\nsort-key 10-debian\narchitecture x86-64\n";
    snprintf(info.meta, sizeof(info.meta), "%s", meta);
    info.meta_len = strlen(meta);

    n = rwy_bls_render_type1(&info, conf, sizeof(conf));
    CHECK(n > 0, "render type 1 success");

    CHECK(rwy_bls_parse(conf, (size_t)n, &e) == RWY_OK, "parse rendered conf");
    CHECK(e.has_title && !strcmp(e.title, "Debian GNU/Linux 13"), "roundtrip title");
    CHECK(!strcmp(e.options, "console=ttyS0 root=UUID=abcd quiet"), "roundtrip options");
    CHECK(e.has_linux && !strcmp(e.linux_path, "/0123456789abcdef/6.12.107+deb13/linux"), "roundtrip linux path");
    CHECK(e.num_initrd == 1 && !strcmp(e.initrd_paths[0], "/0123456789abcdef/6.12.107+deb13/initrd"), "roundtrip initrd path");
    CHECK(e.has_dtb && !strcmp(e.dtb_path, "/0123456789abcdef/6.12.107+deb13/dtb"), "roundtrip dtb path");
}

static void test_bls_render_long_names(void) {
    rwy_bls_slot_info_t info;
    rwy_bls_entry_t e;
    char conf[2048], pfx[256], want[300], name[101], ver[60], meta[128];
    int n;

    printf("== bls render: long names, same path as FUSE ==\n");
    memset(&info, 0, sizeof(info));
    snprintf(info.slot_name, sizeof(info.slot_name), "CONFIG3");
    info.has_kernel = 1;

    memset(name, 'n', 100); name[100] = 0;
    memcpy(name + 96, ".conf", 5);
    snprintf(info.entry_name, sizeof(info.entry_name), "%s", name);
    n = rwy_bls_render_type1(&info, conf, sizeof(conf));
    CHECK(n > 0 && rwy_bls_parse(conf, (size_t)n, &e) == RWY_OK, "render 100-character entry name");
    rwy_bls_derive_prefix(&info, pfx, sizeof(pfx));
    snprintf(want, sizeof(want), "%s/linux", pfx);
    CHECK(strlen(pfx) == 41 && !strcmp(e.linux_path, want), "linux line is the served path (name cut at 40)");

    memset(ver, '7', 59); ver[59] = 0;
    snprintf(meta, sizeof(meta), "version %s\nmachine-id abc\n", ver);
    snprintf(info.meta, sizeof(info.meta), "%s", meta);
    info.meta_len = strlen(meta);
    n = rwy_bls_render_type1(&info, conf, sizeof(conf));
    CHECK(n > 0 && rwy_bls_parse(conf, (size_t)n, &e) == RWY_OK, "render 59-character version");
    rwy_bls_derive_prefix(&info, pfx, sizeof(pfx));
    snprintf(want, sizeof(want), "%s/linux", pfx);
    CHECK(strlen(pfx) == 1 + 3 + 1 + 50 && !strcmp(e.linux_path, want), "linux line is the served path (version cut at 50)");
}

static void test_bls_render_defaults(void) {
    rwy_bls_slot_info_t info;
    rwy_bls_entry_t e;
    char conf[2048];
    int n;

    printf("== bls render: defaults ==\n");
    memset(&info, 0, sizeof(info));
    snprintf(info.slot_name, sizeof(info.slot_name), "CONFIG2");
    snprintf(info.entry_name, sizeof(info.entry_name), "runway-2.conf");
    info.has_kernel = 1;

    n = rwy_bls_render_type1(&info, conf, sizeof(conf));
    CHECK(n > 0, "render default type 1");
    CHECK(rwy_bls_parse(conf, (size_t)n, &e) == RWY_OK, "parse default conf");
    CHECK(e.has_title && !strcmp(e.title, "CONFIG2"), "default title from slot name");
    CHECK(e.has_linux && !strcmp(e.linux_path, "/runway-2/linux"), "default linux path");
}

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

static void test_bls_slot_info(const char *seed) {
    const char *img = tpath("rwy_test_bls_slot.img");
    test_file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    rwy_bls_slot_info_t info;
    FILE *src, *dst;
    char buf[4096];
    size_t nr;

    printf("== bls render: slot info from store ==\n");
    src = fopen(seed, "rb");
    if (!src) return;
    dst = fopen(img, "wb");
    if (!dst) { fclose(src); return; }
    while ((nr = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, nr, dst);
    fclose(src);
    fclose(dst);

    memset(&s, 0, sizeof(s));
    memset(&dev, 0, sizeof(dev));
    fl.f = fopen(img, "r+b");
    if (!fl.f) return;
    dev.read = test_frd; dev.write = test_fwr; dev.flush = test_ffl; dev.ctx = &fl;
    dev.size = 256ULL * 1024 * 1024; dev.block_size = 4096;
    if (rwy_mount(&s, &dev) != RWY_OK) { CHECK(0, "mount seed store"); fclose(fl.f); return; }
    rwy_begin(&s);
    rwy_mkdir(&s, "EFI/RUNWAY", RWY_S_IFDIR | 0755, 0, 0);
    rwy_commit(&s);

    /* Create CONFIG1 with only KERNEL */
    rwy_begin(&s);
    rwy_mkdir(&s, "EFI/RUNWAY/CONFIG1", RWY_S_IFDIR | 0755, 0, 0);
    rwy_create(&s, "EFI/RUNWAY/CONFIG1/KERNEL", RWY_S_IFREG | 0644, 0, 0);
    rwy_commit(&s);

    CHECK(rwy_bls_read_slot_info(&s, "EFI/RUNWAY/CONFIG1", &info) == RWY_OK, "read slot CONFIG1");
    CHECK(info.is_type2 == 1, "CONFIG1 only kernel is Type 2");
    CHECK(!strcmp(info.entry_name, "runway-1.efi"), "CONFIG1 default entry name runway-1.efi");

    /* Create CONFIG2 with KERNEL and INITRD */
    rwy_begin(&s);
    rwy_mkdir(&s, "EFI/RUNWAY/CONFIG2", RWY_S_IFDIR | 0755, 0, 0);
    rwy_create(&s, "EFI/RUNWAY/CONFIG2/KERNEL", RWY_S_IFREG | 0644, 0, 0);
    rwy_create(&s, "EFI/RUNWAY/CONFIG2/INITRD", RWY_S_IFREG | 0644, 0, 0);
    rwy_commit(&s);

    CHECK(rwy_bls_read_slot_info(&s, "EFI/RUNWAY/CONFIG2", &info) == RWY_OK, "read slot CONFIG2");
    CHECK(info.is_type2 == 0, "CONFIG2 with initrd is Type 1");
    CHECK(!strcmp(info.entry_name, "runway-2.conf"), "CONFIG2 default entry name runway-2.conf");

    fclose(fl.f);
}

static void test_bls_commit_blob_encoding(void) {
    char enc[128], dec[128];

    printf("== bls commit: blob encoding ==\n");
    CHECK(rwy_bls_encode_blob_name("/token/version/linux", enc, sizeof(enc)) > 0, "encode path");
    CHECK(!strcmp(enc, "token%2Fversion%2Flinux"), "encoded path value");

    CHECK(rwy_bls_decode_blob_name(enc, dec, sizeof(dec)) > 0, "decode path");
    CHECK(!strcmp(dec, "token/version/linux"), "decoded path value");

    /* Path with '%' */
    CHECK(rwy_bls_encode_blob_name("foo%bar/baz", enc, sizeof(enc)) > 0, "encode percent");
    CHECK(!strcmp(enc, "foo%25bar%2Fbaz"), "encoded percent value");
    CHECK(rwy_bls_decode_blob_name(enc, dec, sizeof(dec)) > 0, "decode percent");
    CHECK(!strcmp(dec, "foo%bar/baz"), "decoded percent value");
}

static void test_bls_commit(const char *seed) {
    const char *img = tpath("rwy_test_bls_commit.img");
    test_file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    rwy_bls_slot_info_t info;
    FILE *src, *dst;
    char buf[4096];
    size_t nr;
    rwy_attr_t a;

    printf("== bls commit: type 1 and type 2 ==\n");
    src = fopen(seed, "rb");
    if (!src) return;
    dst = fopen(img, "wb");
    if (!dst) { fclose(src); return; }
    while ((nr = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, nr, dst);
    fclose(src);
    fclose(dst);

    memset(&s, 0, sizeof(s));
    memset(&dev, 0, sizeof(dev));
    fl.f = fopen(img, "r+b");
    if (!fl.f) return;
    dev.read = test_frd; dev.write = test_fwr; dev.flush = test_ffl; dev.ctx = &fl;
    dev.size = 256ULL * 1024 * 1024; dev.block_size = 4096;
    if (rwy_mount(&s, &dev) != RWY_OK) { CHECK(0, "mount seed store"); fclose(fl.f); return; }
    rwy_begin(&s);
    rwy_mkdir(&s, "EFI/RUNWAY", RWY_S_IFDIR | 0755, 0, 0);
    rwy_commit(&s);

    /* Stage payload blobs in BLOBS/ */
    rwy_begin(&s);
    rwy_mkdir(&s, "EFI/RUNWAY/BLOBS", RWY_S_IFDIR | 0755, 0, 0);

    /* Kernel: 100 bytes */
    char k_blob[100]; memset(k_blob, 'K', sizeof(k_blob));
    rwy_create(&s, "EFI/RUNWAY/BLOBS/tok%2F6.12%2Flinux", RWY_S_IFREG | 0644, 0, 0);
    rwy_write_path(&s, "EFI/RUNWAY/BLOBS/tok%2F6.12%2Flinux", 0, k_blob, sizeof(k_blob));

    /* Initrd 1: 86 bytes (86 % 4 = 2, so 2 bytes zero pad) */
    char initrd1[86]; memset(initrd1, '1', sizeof(initrd1));
    rwy_create(&s, "EFI/RUNWAY/BLOBS/tok%2F6.12%2Finitrd1", RWY_S_IFREG | 0644, 0, 0);
    rwy_write_path(&s, "EFI/RUNWAY/BLOBS/tok%2F6.12%2Finitrd1", 0, initrd1, sizeof(initrd1));

    /* Initrd 2: 50 bytes */
    char initrd2[50]; memset(initrd2, '2', sizeof(initrd2));
    rwy_create(&s, "EFI/RUNWAY/BLOBS/tok%2F6.12%2Finitrd2", RWY_S_IFREG | 0644, 0, 0);
    rwy_write_path(&s, "EFI/RUNWAY/BLOBS/tok%2F6.12%2Finitrd2", 0, initrd2, sizeof(initrd2));

    /* UKI: starts with MZ, 120 bytes */
    char uki_blob[120]; memset(uki_blob, 'U', sizeof(uki_blob));
    uki_blob[0] = 'M'; uki_blob[1] = 'Z';
    rwy_create(&s, "EFI/RUNWAY/BLOBS/test-uki.efi", RWY_S_IFREG | 0644, 0, 0);
    rwy_write_path(&s, "EFI/RUNWAY/BLOBS/test-uki.efi", 0, uki_blob, sizeof(uki_blob));

    rwy_commit(&s);

    /* Commit Type 1 with joined initrd */
    const char *conf =
        "title Test Type 1\n"
        "version 6.12\n"
        "linux /tok/6.12/linux\n"
        "initrd /tok/6.12/initrd1\n"
        "initrd /tok/6.12/initrd2\n"
        "options root=/dev/sda2\n";

    CHECK(rwy_bls_commit_type1(&s, "tok-6.12.conf", conf, strlen(conf), 3) == RWY_OK, "commit Type 1");

    /* Check created slot */
    CHECK(rwy_bls_read_slot_info(&s, "EFI/RUNWAY/CONFIG1", &info) == RWY_OK, "read committed CONFIG1");
    CHECK(!strcmp(info.entry_name, "tok-6.12.conf"), "CONFIG1 entry name");
    CHECK(!strcmp(info.state, "INSTALLED"), "CONFIG1 state INSTALLED");
    CHECK(info.tries == 3, "CONFIG1 tries 3");

    /* Check joined INITRD size: each part is padded to 4 bytes: 86 + 2 + 50 + 2 = 140 */
    CHECK(rwy_stat(&s, "EFI/RUNWAY/CONFIG1/INITRD", &a) == RWY_OK && a.size == 140, "joined initrd size with pad");

    /* Verify padding bytes are 0 */
    uint8_t pad_check[2];
    size_t rsz = sizeof(pad_check);
    CHECK(rwy_read_path(&s, "EFI/RUNWAY/CONFIG1/INITRD", 86, pad_check, &rsz) == RWY_OK, "read pad bytes");
    CHECK(pad_check[0] == 0 && pad_check[1] == 0, "initrd padding bytes are zero");

    /* Commit Type 2 UKI */
    CHECK(rwy_bls_commit_type2(&s, "test-uki.efi", "EFI/RUNWAY/BLOBS/test-uki.efi", 2) == RWY_OK, "commit Type 2");
    CHECK(rwy_bls_read_slot_info(&s, "EFI/RUNWAY/CONFIG2", &info) == RWY_OK, "read committed CONFIG2");
    CHECK(!strcmp(info.entry_name, "test-uki.efi"), "CONFIG2 entry name");
    CHECK(info.is_type2 == 1, "CONFIG2 is Type 2");
    CHECK(info.tries == 2, "CONFIG2 tries 2");

    /* The payloads are linked under the name of their hash: one inode, bridge file included */
    {
        uint8_t dig[32];
        char cname[RWY_BLOB_NAME_SIZE], cpath[128];
        rwy_attr_t blob, enc, slot_k;

        CHECK(rwy_blob_hash_path(&s, "EFI/RUNWAY/BLOBS/tok%2F6.12%2Flinux", dig) == RWY_OK, "hash the kernel");
        rwy_blob_name(dig, cname);
        snprintf(cpath, sizeof(cpath), "EFI/RUNWAY/BLOBS/%s", cname);
        CHECK(rwy_stat(&s, cpath, &blob) == RWY_OK, "kernel has a content name");
        CHECK(rwy_stat(&s, "EFI/RUNWAY/BLOBS/tok%2F6.12%2Flinux", &enc) == RWY_OK && enc.oid == blob.oid,
              "bridge file is a link of the content blob");
        CHECK(rwy_stat(&s, "EFI/RUNWAY/CONFIG1/KERNEL", &slot_k) == RWY_OK && slot_k.oid == blob.oid, "slot links the blob");
        CHECK(blob.nlink == 3, "blob has the content, bridge and slot names");

        CHECK(rwy_blob_hash_path(&s, "EFI/RUNWAY/BLOBS/test-uki.efi", dig) == RWY_OK, "hash the UKI");
        rwy_blob_name(dig, cname);
        snprintf(cpath, sizeof(cpath), "EFI/RUNWAY/BLOBS/%s", cname);
        CHECK(rwy_stat(&s, cpath, &blob) == RWY_OK, "UKI has a content name");
        CHECK(rwy_stat(&s, "EFI/RUNWAY/CONFIG2/KERNEL", &slot_k) == RWY_OK && slot_k.oid == blob.oid, "Type 2 slot links the blob");
    }

    /* The same kernel written again under another name: the slot shares the first blob, the new file becomes a link */
    {
        static const char again[] = "linux /tok/6.13/linux\ninitrd /tok/6.12/initrd1\n";
        char k2[100];
        rwy_attr_t first, second, slot_k;

        memset(k2, 'K', sizeof(k2));
        rwy_begin(&s);
        rwy_create(&s, "EFI/RUNWAY/BLOBS/tok%2F6.13%2Flinux", RWY_S_IFREG | 0644, 0, 0);
        rwy_write_path(&s, "EFI/RUNWAY/BLOBS/tok%2F6.13%2Flinux", 0, k2, sizeof(k2));
        rwy_commit(&s);
        CHECK(rwy_stat(&s, "EFI/RUNWAY/CONFIG1/KERNEL", &first) == RWY_OK, "first kernel slot");
        CHECK(rwy_bls_commit_type1(&s, "tok-6.13.conf", again, strlen(again), 3) == RWY_OK, "commit the same kernel again");
        CHECK(rwy_stat(&s, "EFI/RUNWAY/CONFIG3/KERNEL", &slot_k) == RWY_OK && slot_k.oid == first.oid,
              "the second slot shares the first blob");
        CHECK(rwy_stat(&s, "EFI/RUNWAY/BLOBS/tok%2F6.13%2Flinux", &second) == RWY_OK && second.oid == first.oid,
              "the second bridge file is a link too");
        CHECK(first.nlink == 3, "the first kernel got no extra copy");
        CHECK(rwy_stat(&s, "EFI/RUNWAY/CONFIG1/KERNEL", &first) == RWY_OK && first.nlink == 5, "five names for the shared kernel");
    }

    /* A joined initrd is named by its own hash and shared by the next entry that joins the same parts */
    {
        static const char joined[] = "linux /tok/6.12/linux\ninitrd /tok/6.12/initrd1\ninitrd /tok/6.12/initrd2\n";
        uint8_t dig[32];
        char cname[RWY_BLOB_NAME_SIZE], cpath[128];
        rwy_attr_t blob, first, second;

        CHECK(rwy_blob_hash_path(&s, "EFI/RUNWAY/CONFIG1/INITRD", dig) == RWY_OK, "hash the joined initrd");
        rwy_blob_name(dig, cname);
        snprintf(cpath, sizeof(cpath), "EFI/RUNWAY/BLOBS/%s", cname);
        CHECK(rwy_stat(&s, cpath, &blob) == RWY_OK, "joined initrd has a content name");
        CHECK(rwy_stat(&s, "EFI/RUNWAY/CONFIG1/INITRD", &first) == RWY_OK && first.oid == blob.oid && first.nlink == 2,
              "slot links the joined blob");
        CHECK(rwy_stat(&s, "EFI/RUNWAY/BLOBS/" RWY_BLOB_STAGE_NAME, &second) == RWY_ERR_NOENT, "no staging file is left");
        CHECK(rwy_bls_commit_type1(&s, "tok-6.14.conf", joined, strlen(joined), 3) == RWY_OK, "commit the same join again");
        CHECK(rwy_stat(&s, "EFI/RUNWAY/CONFIG4/INITRD", &second) == RWY_OK && second.oid == first.oid && second.nlink == 3,
              "the second slot shares the joined blob");
        CHECK(rwy_stat(&s, "EFI/RUNWAY/BLOBS/" RWY_BLOB_STAGE_NAME, &second) == RWY_ERR_NOENT, "no staging file after the second join");
    }

    /* After the last slot is removed, the blob keeps the bridge file as its name: not an orphan, until that file goes */
    {
        static const char only[] = "linux /tok/6.20/linux\n";
        char k3[100];
        char cpath[128], cname[RWY_BLOB_NAME_SIZE];
        uint8_t dig[32];
        rwy_attr_t blob;

        memset(k3, 'Z', sizeof(k3));
        rwy_begin(&s);
        rwy_create(&s, "EFI/RUNWAY/BLOBS/tok%2F6.20%2Flinux", RWY_S_IFREG | 0644, 0, 0);
        rwy_write_path(&s, "EFI/RUNWAY/BLOBS/tok%2F6.20%2Flinux", 0, k3, sizeof(k3));
        rwy_commit(&s);
        CHECK(rwy_bls_commit_type1(&s, "tok-6.20.conf", only, strlen(only), 3) == RWY_OK, "commit a kernel of its own");
        CHECK(rwy_blob_hash_path(&s, "EFI/RUNWAY/CONFIG5/KERNEL", dig) == RWY_OK, "hash the kernel of CONFIG5");
        rwy_blob_name(dig, cname);
        snprintf(cpath, sizeof(cpath), "EFI/RUNWAY/BLOBS/%s", cname);
        CHECK(rwy_bls_remove_slot(&s, "CONFIG5") == RWY_OK, "remove the only slot of the kernel");
        CHECK(rwy_stat(&s, cpath, &blob) == RWY_OK && blob.nlink == 2, "the blob stays: content name and bridge file");
        CHECK(rwy_blob_stale(&s, NULL, NULL) == 0, "a blob with a bridge file is not an orphan");
        rwy_begin(&s);
        rwy_unlink(&s, "EFI/RUNWAY/BLOBS/tok%2F6.20%2Flinux");
        rwy_commit(&s);
        CHECK(rwy_stat(&s, cpath, &blob) == RWY_ERR_NOENT, "the bridge file is gone: the sweep removes the blob");
    }

    /* The eviction plan does not promise the blocks of a kernel that its /boot file still holds */
    {
        static const char only[] = "linux /tok/6.21/linux\n";
        static char big[100 * 4096];
        char names[1][32], path[64];
        int n;
        uint32_t planned = 0, free_before, free_after;

        memset(big, 'Y', sizeof(big));
        rwy_begin(&s);
        rwy_create(&s, "EFI/RUNWAY/BLOBS/tok%2F6.21%2Flinux", RWY_S_IFREG | 0644, 0, 0);
        rwy_write_path(&s, "EFI/RUNWAY/BLOBS/tok%2F6.21%2Flinux", 0, big, sizeof(big));
        rwy_commit(&s);
        CHECK(rwy_bls_commit_type1(&s, "tok-6.21.conf", only, strlen(only), 3) == RWY_OK, "commit a slot with a big kernel");
        for (n = 1; n < 10; n++) {
            snprintf(names[0], sizeof(names[0]), "CONFIG%d", n);
            snprintf(path, sizeof(path), "EFI/RUNWAY/%s/KERNEL", names[0]);
            if (rwy_stat(&s, path, &a) == RWY_OK && a.size == sizeof(big)) break;
        }
        CHECK(n < 10, "find the slot with the big kernel");
        rwy_begin(&s);
        CHECK(rwy_bootcfg_evict_blocks(&s, names, 1, &planned) == RWY_OK, "plan the removal of the slot");
        rwy_abort(&s);
        free_before = rwy_free_blocks(&s);
        CHECK(rwy_bls_remove_slot(&s, names[0]) == RWY_OK, "remove the slot");
        free_after = rwy_free_blocks(&s);
        CHECK(planned <= free_after - free_before, "the plan does not count the blocks that the /boot file keeps");
        CHECK(free_after - free_before < 100, "the kernel blocks stay while its /boot file lives");
    }

    /* Refusal: Type 2 with bad magic */
    rwy_begin(&s);
    const char bad_uki[] = "NOT_AN_EFI";
    rwy_create(&s, "EFI/RUNWAY/BLOBS/bad.efi", RWY_S_IFREG | 0644, 0, 0);
    rwy_write_path(&s, "EFI/RUNWAY/BLOBS/bad.efi", 0, bad_uki, sizeof(bad_uki));
    rwy_commit(&s);

    CHECK(rwy_bls_commit_type2(&s, "bad.efi", "EFI/RUNWAY/BLOBS/bad.efi", 3) == RWY_ERR_INVAL, "reject non-MZ UKI");

    /* Refusal: Type 1 with missing payload */
    const char *missing_conf =
        "title Missing\n"
        "linux /tok/6.12/absent\n";
    CHECK(rwy_bls_commit_type1(&s, "missing.conf", missing_conf, strlen(missing_conf), 3) == RWY_ERR_NOENT,
          "reject missing payload");

    /* Hash of a store file: more than one chunk, same as the hash of a host file with that data */
    {
        enum { HASH_LEN = 2 * 1024 * 1024 + 4321 };
        uint8_t *data = malloc(HASH_LEN), d_store[32], d_host[32];
        const char *hp = tpath("rwy_test_bls_hash.bin");
        FILE *hf = fopen(hp, "w+b");
        int i;

        for (i = 0; i < HASH_LEN; i++) data[i] = (uint8_t)(i * 7 + i / 251);
        fwrite(data, 1, HASH_LEN, hf);
        fflush(hf);
        rwy_begin(&s);
        rwy_create(&s, "EFI/RUNWAY/BLOBS/hashme", RWY_S_IFREG | 0644, 0, 0);
        rwy_write_path(&s, "EFI/RUNWAY/BLOBS/hashme", 0, data, HASH_LEN);
        rwy_commit(&s);
        CHECK(rwy_blob_hash_path(&s, "EFI/RUNWAY/BLOBS/hashme", d_store) == RWY_OK, "hash a store file");
        CHECK(rwy_blob_hash_fd(fileno(hf), d_host) == RWY_OK && !memcmp(d_store, d_host, 32), "store hash equals file hash");
        CHECK(rwy_blob_hash_path(&s, "EFI/RUNWAY/BLOBS/absent", d_store) == RWY_ERR_NOENT, "hash of a missing file");
        fclose(hf);
        remove(hp);
        free(data);
    }

    /* Refusal: a payload path that encodes to a content name, or is too long for a name */
    char long_conf[600];
    const char *content_conf =
        "linux /tok/6.12/linux\n"
        "initrd /tok/6.12/initrd1\n"
        "initrd /0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
    CHECK(rwy_bls_commit_type1(&s, "content.conf", content_conf, strlen(content_conf), 3) == RWY_ERR_PERM,
          "reject content name payload");
    memset(long_conf, 'a', sizeof(long_conf));
    memcpy(long_conf, "linux /", 7);
    memcpy(long_conf + 400, "\n", 2);
    CHECK(rwy_bls_commit_type1(&s, "long.conf", long_conf, 402, 3) != RWY_OK, "reject overlong payload");

    fclose(fl.f);
}

/* A slot with a kernel of its own, made by a bridge commit: INSTALLED */
static void bls_add_slot(rwy_store_t *s, int n) {
    char path[64], conf[64], entry[32], kernel[100];
    int rc;

    snprintf(path, sizeof(path), "EFI/RUNWAY/BLOBS/rm%%2F%d%%2Flinux", n);
    snprintf(conf, sizeof(conf), "linux /rm/%d/linux\n", n);
    snprintf(entry, sizeof(entry), "rm-%d.conf", n);
    memset(kernel, 'A' + n, sizeof(kernel));
    rwy_begin(s);
    rwy_create(s, path, RWY_S_IFREG | 0644, 0, 0);
    rwy_write_path(s, path, 0, kernel, sizeof(kernel));
    rwy_commit(s);
    rc = rwy_bls_commit_type1(s, entry, conf, strlen(conf), 3);
    CHECK(rc == RWY_OK, "commit a slot to remove");
}

static void bls_confirm_slot(rwy_store_t *s, const char *slot) {
    char path[64];
    int rc;

    snprintf(path, sizeof(path), "EFI/RUNWAY/%s/STATE", slot);
    rwy_set_actor(s, RWY_ACTOR_SYSTEM);
    rwy_begin(s);
    rc = rwy_write_path(s, path, 0, "OK\n", 3);
    rwy_commit(s);
    CHECK(rc == RWY_OK, "confirm a slot");
    rwy_set_actor(s, RWY_ACTOR_USER);
}

static void bls_set_booted(rwy_store_t *s, const char *slot) {
    uint8_t rec[RWY_REC_PAYLOAD];

    rwy_record_read(s, rec);
    rwy_ss_set_booted(rec, slot);
    CHECK(rwy_record_write(s, rec) == RWY_OK, "write the booted slot to the record");
}

static void test_bls_remove(const char *seed) {
    const char *img = tpath("rwy_test_bls_remove.img");
    test_file_t fl;
    rwy_block_dev_t dev;
    rwy_store_t s;
    FILE *src, *dst;
    char buf[4096];
    size_t nr;
    rwy_attr_t gone;

    printf("== bls remove: refuse booted and last OK ==\n");
    src = fopen(seed, "rb");
    if (!src) return;
    dst = fopen(img, "wb");
    if (!dst) { fclose(src); return; }
    while ((nr = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, nr, dst);
    fclose(src);
    fclose(dst);

    memset(&s, 0, sizeof(s));
    memset(&dev, 0, sizeof(dev));
    fl.f = fopen(img, "r+b");
    if (!fl.f) return;
    dev.read = test_frd; dev.write = test_fwr; dev.flush = test_ffl; dev.ctx = &fl;
    dev.size = 256ULL * 1024 * 1024; dev.block_size = 4096;
    if (rwy_mount(&s, &dev) != RWY_OK) { CHECK(0, "mount seed store"); fclose(fl.f); return; }
    rwy_begin(&s);
    rwy_mkdir(&s, "EFI/RUNWAY", RWY_S_IFDIR | 0755, 0, 0);
    rwy_commit(&s);
    rwy_begin(&s);
    rwy_mkdir(&s, "EFI/RUNWAY/BLOBS", RWY_S_IFDIR | 0755, 0, 0);
    rwy_commit(&s);

    /* CONFIG1 is OK and booted, CONFIG2 is INSTALLED */
    bls_add_slot(&s, 1);
    bls_add_slot(&s, 2);
    bls_confirm_slot(&s, "CONFIG1");
    bls_set_booted(&s, "CONFIG1");

    CHECK(rwy_bls_can_remove_slot(&s, "CONFIG1") == RWY_ERR_BUSY, "refuse remove BOOTED slot");
    CHECK(rwy_bls_remove_slot(&s, "CONFIG1") == RWY_ERR_BUSY, "remove_slot returns EBUSY for BOOTED");

    /* CONFIG1 is not booted any more, but it is the only OK slot */
    bls_set_booted(&s, "CONFIG99");
    CHECK(rwy_bls_can_remove_slot(&s, "CONFIG1") == RWY_ERR_BUSY, "refuse remove last OK slot");
    CHECK(rwy_bls_remove_slot(&s, "CONFIG1") == RWY_ERR_BUSY, "remove_slot returns EBUSY for last OK");

    CHECK(rwy_bls_can_remove_slot(&s, "CONFIG2") == RWY_OK, "can remove INSTALLED slot");
    CHECK(rwy_bls_remove_slot(&s, "CONFIG2") == RWY_OK, "remove INSTALLED slot succeeds");
    CHECK(rwy_stat(&s, "EFI/RUNWAY/CONFIG2", &gone) == RWY_ERR_NOENT, "the removed slot is gone");

    /* Two OK slots: neither is the last one (the new slot takes the free number 2) */
    bls_add_slot(&s, 3);
    bls_confirm_slot(&s, "CONFIG2");
    CHECK(rwy_bls_can_remove_slot(&s, "CONFIG2") == RWY_OK, "can remove OK slot when another OK exists");
    CHECK(rwy_bls_remove_slot(&s, "CONFIG2") == RWY_OK, "remove the second OK slot succeeds");
    CHECK(rwy_bls_can_remove_slot(&s, "CONFIG1") == RWY_ERR_BUSY, "CONFIG1 protected again as only OK");
    CHECK(rwy_bls_can_remove_slot(&s, "CONFIG42") == RWY_ERR_NOENT, "a missing slot is not found");

    fclose(fl.f);
}

int main(int argc, char **argv) {
    const char *seed = argc > 1 ? argv[1] : "build/tests/host/RUNWAY.DAT";

    test_bls_name_parse();
    test_bls_name_render();
    test_bls_name_boot_count_path();
    test_bls_name_rename();
    test_bls_name_match_entry();
    test_bls_parse_valid();
    test_bls_parse_multiple_initrd();
    test_bls_parse_faults();
    test_bls_parse_fuzz();
    test_bls_render_roundtrip();
    test_bls_render_long_names();
    test_bls_render_defaults();
    test_bls_slot_info(seed);
    test_bls_commit_blob_encoding();
    test_bls_commit(seed);
    test_bls_remove(seed);

    if (failures) {
        printf("BLSTEST: %d FAILURES\n", failures);
        return 1;
    }
    printf("BLSTEST: PASS\n");
    return 0;
}
