/* Schema enforcement and the virtual files of the boot configuration subtree. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "bootcfgtest.h"
#include "rwy_crypto.h"

void test_inactive(void) {
    rwy_attr_t a;
    printf("== inactive: no EFI/RUNWAY ==\n");
    EXPECT(mk("EFI/CONFIG01"), OK, "any dir name outside the subtree");
    EXPECT(cr("EFI/CONFIG01/STATE"), OK, "STATE outside subtree");
    EXPECT(wr("EFI/CONFIG01/STATE", 3, "junk"), OK, "unvalidated write outside");
    EXPECT(ch("EFI/CONFIG01/STATE"), OK, "chmod outside");
    EXPECT(co("EFI/CONFIG01/STATE"), OK, "chown outside");
    EXPECT(sx("EFI/CONFIG01/STATE"), OK, "setxattr outside");
    EXPECT(rwy_stat(&S, "EFI/CONFIG01", &a), OK, "stat");
    CHECK((a.mode & 07777) == 0700 && a.uid == 1000, "mode and owner untouched outside");
}

void test_subtree_create(void) {
    rwy_attr_t a;
    printf("== subtree creation writes VERSION ==\n");
    EXPECT(mk(T), OK, "mkdir EFI/Runway");
    same_text(T "/VERSION", "1\n", "VERSION content");
    EXPECT(rwy_stat(&S, T "/VERSION", &a), OK, "stat VERSION");
    CHECK((a.mode & 07777) == 0644 && a.uid == 0 && a.gid == 0, "VERSION mode and owner");
    CHECK((a.mode & RWY_S_IFMT) == RWY_S_IFREG, "VERSION regular");
    EXPECT(wr(T "/VERSION", 0, "2\n"), PERM, "USER cannot write VERSION");
    as_sys();
    EXPECT(wr(T "/VERSION", 0, "2\n"), PERM, "SYSTEM cannot write VERSION");
    EXPECT(rm(T "/VERSION"), PERM, "SYSTEM cannot unlink VERSION");
    as_user();
    EXPECT(rm(T "/VERSION"), PERM, "USER cannot unlink VERSION");
    EXPECT(mv(T "/VERSION", T "/OTHER"), PERM, "VERSION cannot be renamed");
}

void test_names_types(void) {
    printf("== names, types and sizes ==\n");
    EXPECT(cr(T "/JUNK"), INV, "unknown file at root");
    EXPECT(mk(T "/JUNKDIR"), INV, "unknown dir at root");
    EXPECT(mk(T "/CONFIG01"), INV, "leading zero");
    EXPECT(mk(T "/CONFIG"), INV, "CONFIG without number");
    EXPECT(mk(T "/CONFIGx"), INV, "CONFIGx");
    EXPECT(mk(T "/CONFIG-1"), INV, "negative");
    EXPECT(mk(T "/CONFIG18446744073709551616"), INV, "u64 overflow");
    EXPECT(cr(T "/CONFIG1"), INV, "file named like a slot");
    EXPECT(mk(T "/CONFIG1"), OK, "CONFIG1");
    EXPECT(mk(T "/CONFIG0"), OK, "CONFIG0 is canonical");
    EXPECT(mk(T "/CONFIG18446744073709551615"), OK, "max u64");
    EXPECT(mk(T "/BLOBS"), OK, "BLOBS");
    EXPECT(mk(T "/RECOVERY"), PERM, "RECOVERY by USER");
    as_sys();
    EXPECT(mk(T "/RECOVERY"), PERM, "RECOVERY by SYSTEM without unlock");
    as_user();
    EXPECT(sy(T "/CONFIG1/DTB", "../BLOBS/x"), OK, "symlink payload");
    EXPECT(cr(T "/CONFIG1/KERNEL"), OK, "KERNEL");
    EXPECT(cr(T "/CONFIG1/INITRD"), OK, "INITRD");
    EXPECT(cr(T "/CONFIG1/ARGS"), OK, "ARGS");
    EXPECT(cr(T "/CONFIG1/LABEL"), OK, "LABEL");
    EXPECT(cr(T "/CONFIG1/ENTRY"), OK, "ENTRY");
    EXPECT(cr(T "/CONFIG1/META"), OK, "META");
    EXPECT(cr(T "/CONFIG1/STATE"), OK, "STATE");
    EXPECT(cr(T "/CONFIG1/TRIES"), OK, "TRIES");
    EXPECT(cr(T "/CONFIG1/WATCHDOG"), OK, "slot WATCHDOG");
    EXPECT(cr(T "/CONFIG1/NOPE"), INV, "unknown name in slot");
    EXPECT(mk(T "/CONFIG1/SUB"), INV, "dir in slot");
    EXPECT(sy(T "/CONFIG1/STATE2", "x"), INV, "unknown symlink in slot");
    EXPECT(sy(T "/CONFIG1/ARGS2", "x"), INV, "symlink with unknown name");
    EXPECT(sy(T "/CONFIG1/LABEL2", "x"), INV, "symlink with unknown name");
    EXPECT(sy(T "/CONFIG1/ENTRY2", "x"), INV, "symlink with unknown name");
    EXPECT(sy(T "/CONFIG1/META2", "x"), INV, "symlink with unknown name");
    EXPECT(cr(T "/CONFIG1/INDEX"), PERM, "INDEX is virtual");
    EXPECT(cr(T "/CONFIG1/REVISION"), PERM, "REVISION by USER");
    as_sys();
    EXPECT(cr(T "/CONFIG1/REVISION"), PERM, "REVISION by SYSTEM");
    as_user();
    EXPECT(mk(T "/BLOBS/sub"), INV, "dir in BLOBS");
    EXPECT(sy(T "/BLOBS/lnk", "x"), INV, "symlink in BLOBS");
    EXPECT(cr(T "/BLOBS/uki-1.efi"), OK, "blob");
    EXPECT(cr(T "/VERSION2"), INV, "unknown root name");
    EXPECT(cr(T "/REVISION"), PERM, "global REVISION by USER");
    EXPECT(cr(T "/REASON"), PERM, "REASON by USER");
    EXPECT(sy(T "/BOOTED", "CONFIG1"), PERM, "BOOTED by USER");
    EXPECT(cr(T "/WATCHDOG"), OK, "global WATCHDOG");
    EXPECT(cr(T "/RECOVER_NEXT"), OK, "RECOVER_NEXT");
    EXPECT(cr(T "/BOOTED2"), INV, "unknown name");

    /* ARGS size limit */
    {
        static char big[4200];
        memset(big, 'a', sizeof(big) - 1);
        big[4096] = 0;
        EXPECT(wr(T "/CONFIG1/ARGS", 0, big), OK, "ARGS 4096 bytes");
        EXPECT(wr(T "/CONFIG1/ARGS", 4096, "b"), INV, "ARGS 4097 bytes");
    }

    /* LABEL size limit */
    {
        char big[132];
        memset(big, 'a', 128);
        big[128] = 0;
        EXPECT(wr(T "/CONFIG1/LABEL", 0, big), OK, "LABEL 128 bytes");
        EXPECT(wr(T "/CONFIG1/LABEL", 128, "b"), INV, "LABEL 129 bytes");
    }

    /* ENTRY size limit and name validation */
    {
        char big[270];
        memset(big, 'a', 250);
        memcpy(big + 250, ".conf", 5);
        big[255] = 0;
        EXPECT(wr(T "/CONFIG1/ENTRY", 0, big), OK, "ENTRY 255 bytes");
        memset(big, 'a', 251);
        memcpy(big + 251, ".conf", 5);
        big[256] = 0;
        EXPECT(wr(T "/CONFIG1/ENTRY", 0, big), INV, "ENTRY 256 bytes");
        EXPECT(wr(T "/CONFIG1/ENTRY", 0, "foo.conf"), OK, "valid .conf");
        EXPECT(wr(T "/CONFIG1/ENTRY", 0, "foo.efi"), OK, "valid .efi");
        EXPECT(wr(T "/CONFIG1/ENTRY", 0, ".conf"), INV, "empty stem .conf");
        EXPECT(wr(T "/CONFIG1/ENTRY", 0, ".efi"), INV, "empty stem .efi");
        EXPECT(wr(T "/CONFIG1/ENTRY", 0, "foo.bin"), INV, "wrong extension");
        EXPECT(wr(T "/CONFIG1/ENTRY", 0, "foo+1.conf"), INV, "stem ending in counter");
        EXPECT(wr(T "/CONFIG1/ENTRY", 0, "foo+1-2.efi"), INV, "stem ending in counter range");
        EXPECT(wr(T "/CONFIG1/ENTRY", 0, "tok-6.12.107+deb13-amd64.efi"), OK, "Debian version with +");
        EXPECT(wr(T "/CONFIG1/ENTRY", 0, "foo bar.conf"), INV, "space in entry");
        EXPECT(wr(T "/CONFIG1/ENTRY", 0, ""), OK, "empty write is a no-op");
        EXPECT(wr(T "/CONFIG1/ENTRY", 1, "foo.conf"), INV, "scalar write must start at 0");
    }

    /* META size limit */
    {
        static char big[1040];
        memset(big, 'a', sizeof(big) - 1);
        big[1024] = 0;
        EXPECT(wr(T "/CONFIG1/META", 0, big), OK, "META 1024 bytes");
        EXPECT(wr(T "/CONFIG1/META", 1024, "b"), INV, "META 1025 bytes");
        EXPECT(wr(T "/CONFIG1/META", 0, "version 1.0\n"), OK, "valid META line");
    }
}

void test_mode_owner(void) {
    rwy_attr_t a;
    printf("== fixed mode and owner ==\n");
    EXPECT(mk(T "/CONFIG2"), OK, "CONFIG2");
    EXPECT(rwy_stat(&S, T "/CONFIG2", &a), OK, "stat dir");
    CHECK((a.mode & 07777) == 0755 && a.uid == 0 && a.gid == 0, "dir coerced to 0755 root");
    EXPECT(cr(T "/CONFIG2/KERNEL"), OK, "KERNEL");
    EXPECT(rwy_stat(&S, T "/CONFIG2/KERNEL", &a), OK, "stat file");
    CHECK((a.mode & 07777) == 0644 && a.uid == 0 && a.gid == 0, "file coerced to 0644 root");
    EXPECT(rwy_lstat(&S, T "/CONFIG1/DTB", &a), OK, "stat symlink");
    CHECK(a.uid == 0 && a.gid == 0, "symlink owner coerced");

    const char *files[] = { T "/CONFIG2/KERNEL", T "/CONFIG2", T "/BLOBS", T "/VERSION", T "/BLOBS/uki-1.efi" };
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        EXPECT(ch(files[i]), PERM, "chmod");
        EXPECT(co(files[i]), PERM, "chown");
        EXPECT(ut(files[i]), PERM, "utimens");
        EXPECT(sx(files[i]), PERM, "setxattr");
        EXPECT(rx(files[i]), PERM, "removexattr");
    }
    EXPECT(ch(T), PERM, "chmod on the subtree root");
}

void test_scalars(void) {
    printf("== scalar files ==\n");
    const char *st = T "/CONFIG2/STATE";
    EXPECT(cr(st), OK, "STATE");
    EXPECT(wr(st, 0, "OK\n"), PERM, "OK on incomplete slot");
    EXPECT(wr(st, 0, "FAILED"), PERM, "FAILED on incomplete slot");
    EXPECT(wr(st, 0, "TESTING"), PERM, "TESTING by USER");
    EXPECT(wr(st, 0, "garbage"), INV, "garbage STATE");
    EXPECT(wr(st, 0, ""), OK, "empty write is a no-op");
    EXPECT(wr(st, 1, "INSTALLED"), INV, "scalar write must start at 0");
    const char *tries = T "/CONFIG2/TRIES";
    EXPECT(cr(tries), OK, "TRIES");
    EXPECT(wr(tries, 0, "3\n"), OK, "TRIES 3");
    EXPECT(wr(tries, 0, "x"), INV, "TRIES x");
    EXPECT(wr(tries, 0, "-1"), INV, "TRIES -1");
    EXPECT(wr(tries, 0, "1 2"), INV, "TRIES with space");
    EXPECT(wr(tries, 0, "18446744073709551615"), OK, "max u64");
    EXPECT(wr(tries, 0, "18446744073709551616"), INV, "u64 overflow");
    EXPECT(wr(tries, 0, "000000000000000000000"), INV, "21 digits");
    EXPECT(wr(tries, 0, "7"), OK, "TRIES 7");
    same_text(tries, "7", "TRIES content");

    EXPECT(wr(T "/CONFIG2/KERNEL", 0, "k"), OK, "KERNEL content");
    EXPECT(wr(st, 0, "INSTALLED\n"), OK, "INSTALLED finalizes");
    same_text(st, "INSTALLED\n", "STATE content");
    same_text(tries, "7\n", "TRIES moved into the record at finalize");
    EXPECT(wr(st, 0, "INSTALLED"), PERM, "INSTALLED again is refused");
    same_text(st, "INSTALLED\n", "STATE unchanged");
    EXPECT(wr(st, 0, "OK"), OK, "OK replaces");
    same_text(st, "OK\n", "the value is read back as a whole line");
    EXPECT(wr(st, 0, "FAILED\n"), OK, "FAILED replaces");
    same_text(st, "FAILED\n", "FAILED replaces OK");
    EXPECT(tr(st, 0), OK, "truncate(0) is a no-op success");
    same_text(st, "FAILED\n", "STATE survives truncate(0)");
    EXPECT(tr(st, 3), INV, "truncate to non-zero");

    const char *wd = T "/WATCHDOG";
    EXPECT(wr(wd, 0, "30\n"), OK, "global WATCHDOG");
    EXPECT(wr(wd, 0, "thirty"), INV, "WATCHDOG text");
    EXPECT(wr(T "/CONFIG1/WATCHDOG", 0, "0"), OK, "slot WATCHDOG 0");
    EXPECT(wr(T "/CONFIG1/WATCHDOG", 0, "-"), INV, "slot WATCHDOG text");
}

void test_finalized(void) {
    printf("== finalized slot ==\n");
    const char *c = T "/CONFIG3";
    uint64_t seq;
    EXPECT(mk(c), OK, "CONFIG3");
    EXPECT(cr(T "/CONFIG3/KERNEL"), OK, "KERNEL");
    EXPECT(wr(T "/CONFIG3/KERNEL", 0, "kernel"), OK, "write KERNEL before finalize");
    EXPECT(cr(T "/CONFIG3/ARGS"), OK, "ARGS");
    EXPECT(wr(T "/CONFIG3/ARGS", 0, "a=b"), OK, "write ARGS before finalize");
    EXPECT(cr(T "/CONFIG3/LABEL"), OK, "LABEL");
    EXPECT(wr(T "/CONFIG3/LABEL", 0, "test-label"), OK, "write LABEL before finalize");
    EXPECT(cr(T "/CONFIG3/ENTRY"), OK, "ENTRY");
    EXPECT(wr(T "/CONFIG3/ENTRY", 0, "entry-3.conf"), OK, "write ENTRY before finalize");
    EXPECT(cr(T "/CONFIG3/META"), OK, "META");
    EXPECT(wr(T "/CONFIG3/META", 0, "version 3.0\n"), OK, "write META before finalize");
    EXPECT(cr(T "/CONFIG3/STATE"), OK, "STATE");
    EXPECT(wr(T "/CONFIG3/STATE", 0, "INSTALLED\n"), OK, "INSTALLED");
    CHECK(read_all(T "/CONFIG3/REVISION", (char[16]){0}, 16) > 0, "finalize assigned a REVISION");
    {
        rwy_attr_t a;
        S.bc_novirt = 1;
        EXPECT(rwy_lstat(&S, T "/CONFIG3/STATE", &a), RWY_ERR_NOENT, "finalize removed STATE from the tree");
        EXPECT(rwy_lstat(&S, T "/CONFIG3/TRIES", &a), RWY_ERR_NOENT, "...and TRIES");
        S.bc_novirt = 0;
        EXPECT(rwy_lstat(&S, T "/CONFIG3/STATE", &a), OK, "STATE is still there, virtual");
        CHECK(a.size == 10 && (a.mode & 07777) == 0644, "size of the text and the usual mode");
    }
    same_text(T "/CONFIG3/TRIES", "1\n", "no TRIES written: the default of one try");
    EXPECT(cr(T "/CONFIG3/TRIES"), PERM, "TRIES after finalize");
    as_sys();
    EXPECT(cr(T "/CONFIG3/TRIES"), PERM, "TRIES is virtual, also for SYSTEM");
    EXPECT(wr(T "/CONFIG3/TRIES", 0, "1"), OK, "TRIES value");
    as_user();

    EXPECT(wr(T "/CONFIG3/STATE", 0, "INSTALLED"), PERM, "INSTALLED on finalized (USER)");
    EXPECT(wr(T "/CONFIG3/STATE", 0, "TESTING"), PERM, "TESTING by USER");
    EXPECT(wr(T "/CONFIG3/STATE", 0, "FAILED"), OK, "FAILED by USER");
    /* ADR 0002 D2: FAILED is final for USER */
    EXPECT(wr(T "/CONFIG3/STATE", 0, "OK\n"), PERM, "OK over FAILED by USER");
    EXPECT(wr(T "/CONFIG3/STATE", 0, "FAILED\n"), OK, "FAILED over FAILED by USER");
    EXPECT(rm(T "/CONFIG3/STATE"), OK, "USER unlink of STATE is a no-op: it cannot be lost");
    same_text(T "/CONFIG3/STATE", "FAILED\n", "STATE is still there");
    EXPECT(cr(T "/CONFIG3/STATE"), PERM, "USER cannot create STATE of a finalized slot");
    as_sys();
    EXPECT(rm(T "/CONFIG3/STATE"), OK, "SYSTEM unlink of STATE is a no-op too");
    EXPECT(cr(T "/CONFIG3/STATE"), PERM, "SYSTEM cannot create it");
    EXPECT(wr(T "/CONFIG3/STATE", 0, "FAILED\n"), OK, "SYSTEM writes STATE");
    EXPECT(wr(T "/CONFIG3/STATE", 0, "OK\n"), OK, "OK over FAILED by SYSTEM");
    as_user();
    seq = rseq();
    EXPECT(wr(T "/CONFIG3/STATE", 0, "OK\n"), OK, "OK over OK by USER");
    CHECK(rseq() == seq, "OK over OK writes no record");
    same_text(T "/CONFIG3/STATE", "OK\n", "STATE is OK");
    as_sys();
    EXPECT(wr(T "/CONFIG3/STATE", 0, "TESTING"), OK, "TESTING by SYSTEM");
    EXPECT(wr(T "/CONFIG3/STATE", 0, "INSTALLED"), PERM, "INSTALLED on finalized (SYSTEM)");
    EXPECT(wr(T "/CONFIG3/TRIES", 0, "2"), OK, "TRIES by SYSTEM");
    EXPECT(wr(T "/CONFIG3/KERNEL", 0, "x"), PERM, "KERNEL frozen (SYSTEM)");
    EXPECT(wr(T "/CONFIG3/REVISION", 0, "9\n"), PERM, "REVISION by SYSTEM");
    as_user();
    EXPECT(wr(T "/CONFIG3/TRIES", 0, "9"), PERM, "TRIES by USER after finalize");
    EXPECT(wr(T "/CONFIG3/KERNEL", 0, "x"), PERM, "KERNEL frozen");
    EXPECT(tr(T "/CONFIG3/KERNEL", 0), PERM, "KERNEL truncate frozen");
    EXPECT(wr(T "/CONFIG3/ARGS", 0, "x"), PERM, "ARGS frozen");
    EXPECT(wr(T "/CONFIG3/LABEL", 0, "x"), PERM, "LABEL frozen");
    EXPECT(tr(T "/CONFIG3/LABEL", 0), PERM, "LABEL truncate frozen");
    EXPECT(wr(T "/CONFIG3/ENTRY", 0, "x.conf"), PERM, "ENTRY frozen");
    EXPECT(tr(T "/CONFIG3/ENTRY", 0), PERM, "ENTRY truncate frozen");
    EXPECT(wr(T "/CONFIG3/META", 0, "x"), PERM, "META frozen");
    EXPECT(tr(T "/CONFIG3/META", 0), PERM, "META truncate frozen");
    EXPECT(wr(T "/CONFIG3/REVISION", 0, "9\n"), PERM, "REVISION by USER");
    EXPECT(tr(T "/CONFIG3/REVISION", 0), PERM, "REVISION truncate");
    EXPECT(cr(T "/CONFIG3/INITRD"), PERM, "new payload in finalized slot");
    EXPECT(cr(T "/CONFIG3/WATCHDOG"), PERM, "new WATCHDOG in finalized slot");
    same_text(T "/CONFIG3/KERNEL", "kernel", "KERNEL intact");
    same_text(T "/CONFIG3/LABEL", "test-label", "LABEL intact");
    same_text(T "/CONFIG3/ENTRY", "entry-3.conf", "ENTRY intact");
    same_text(T "/CONFIG3/META", "version 3.0\n", "META intact");
    EXPECT(rm(T "/CONFIG3/ARGS"), OK, "entries may be removed");
    EXPECT(rm(T "/CONFIG3/LABEL"), OK, "LABEL removed");
    EXPECT(rm(T "/CONFIG3/ENTRY"), OK, "ENTRY removed");
    EXPECT(rm(T "/CONFIG3/META"), OK, "META removed");
    EXPECT(rm(T "/CONFIG3/TRIES"), OK, "unlink of TRIES is a no-op");
    EXPECT(cr(T "/CONFIG3/TRIES"), PERM, "TRIES cannot be created by USER");
    as_sys();
    EXPECT(cr(T "/CONFIG3/TRIES"), PERM, "TRIES cannot be created by SYSTEM");
    EXPECT(wr(T "/CONFIG3/TRIES", 0, "1"), OK, "TRIES value");
    as_user();
}

void test_links(void) {
    printf("== hardlinked and symlinked payloads ==\n");
    EXPECT(mk(T "/CONFIG4"), OK, "CONFIG4");
    EXPECT(cr(T "/BLOBS/b1"), OK, "blob b1");
    EXPECT(wr(T "/BLOBS/b1", 0, "blob1"), OK, "blob writable while nlink==1");
    EXPECT(ln(T "/BLOBS/b1", T "/CONFIG4/KERNEL"), OK, "hardlink KERNEL");
    EXPECT(wr(T "/BLOBS/b1", 0, "x"), PERM, "blob frozen by a slot link");
    EXPECT(wr(T "/CONFIG4/KERNEL", 0, "x"), PERM, "linked KERNEL frozen");
    EXPECT(tr(T "/CONFIG4/KERNEL", 0), PERM, "linked KERNEL truncate");
    same_text(T "/CONFIG4/KERNEL", "blob1", "payload intact");

    EXPECT(cr("OUT.BIN"), OK, "outside file");
    EXPECT(wr("OUT.BIN", 0, "out"), OK, "outside writable");
    EXPECT(ln("OUT.BIN", T "/CONFIG4/INITRD"), OK, "hardlink from outside the subtree");
    EXPECT(wr("OUT.BIN", 0, "x"), PERM, "outside name frozen once linked in");
    EXPECT(cr("O2.BIN"), OK, "O2");
    EXPECT(ln("O2.BIN", "O3.BIN"), OK, "outside-only hardlink");
    EXPECT(wr("O2.BIN", 0, "ok"), OK, "outside-only hardlink stays writable");

    EXPECT(cr(T "/CONFIG4/STATE"), OK, "STATE");
    EXPECT(ln(T "/CONFIG4/STATE", "S.BIN"), PERM, "scalar cannot be linked out");
    EXPECT(ln(T "/CONFIG4/STATE", T "/BLOBS/s"), PERM, "scalar cannot be linked into BLOBS");
    EXPECT(cr(T "/CONFIG4/LABEL"), OK, "LABEL");
    EXPECT(ln(T "/CONFIG4/LABEL", "L.BIN"), PERM, "label cannot be linked out");
    EXPECT(ln("O2.BIN", T "/CONFIG4/ARGS"), INV, "only payload names accept links");
    EXPECT(ln("O2.BIN", T "/CONFIG4/LABEL"), INV, "only payload names accept links");
    EXPECT(ln("O2.BIN", T "/CONFIG4/STATE2"), INV, "unknown link name");
    EXPECT(ln("O2.BIN", T "/NEWFILE"), INV, "unknown link name at root");
    EXPECT(ln(T "/CONFIG4", T "/BLOBS/d"), INV, "directories cannot be linked");

    EXPECT(sy(T "/CONFIG4/DTB", "../BLOBS/b2"), OK, "dangling symlink may be created");
    EXPECT(cr(T "/BLOBS/b2"), OK, "b2");
    EXPECT(wr(T "/CONFIG4/DTB", 0, "dtb"), OK, "write through DTB symlink reaches the target");
    same_text(T "/BLOBS/b2", "dtb", "target content");

    EXPECT(rm(T "/BLOBS/b1"), OK, "blob name may be removed");
    EXPECT(wr(T "/CONFIG4/KERNEL", 0, "kern"), OK, "sole link in an incomplete slot is writable again");
}

#define HASH_A "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define HASH_B "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"

/* Content names: SYSTEM makes and removes them, nobody writes their object. */
void test_content_names(void) {
    printf("== content names ==\n");
    EXPECT(mk(T "/CONFIG21"), OK, "CONFIG21");
    EXPECT(cr(T "/BLOBS/plain"), OK, "plain blob");
    EXPECT(wr(T "/BLOBS/plain", 0, "data"), OK, "plain blob data");
    EXPECT(cr("CN.BIN"), OK, "outside file");

    EXPECT(cr(T "/BLOBS/" HASH_A), PERM, "USER cannot create a content name");
    EXPECT(ln(T "/BLOBS/plain", T "/BLOBS/" HASH_A), PERM, "USER cannot link to a content name");
    EXPECT(ln("CN.BIN", T "/BLOBS/" HASH_A), PERM, "USER cannot link an outside file to a content name");
    EXPECT(mv(T "/BLOBS/plain", T "/BLOBS/" HASH_A), PERM, "USER cannot rename to a content name");
    EXPECT(cr(T "/BLOBS/" HASH_A "0"), OK, "65 digits are an ordinary name");
    EXPECT(cr(T "/BLOBS/0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"), OK, "upper case is an ordinary name");
    EXPECT(cr(T "/BLOBS/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde"), OK, "63 digits are an ordinary name");
    EXPECT(wr(T "/BLOBS/plain", 0, "more"), OK, "plain blob stays writable");

    as_sys();
    EXPECT(mv(T "/BLOBS/plain", T "/BLOBS/" HASH_A), OK, "SYSTEM renames to a content name");
    EXPECT(wr(T "/BLOBS/" HASH_A, 0, "x"), PERM, "SYSTEM cannot write a content object");
    EXPECT(tr(T "/BLOBS/" HASH_A, 0), PERM, "SYSTEM cannot truncate a content object");
    EXPECT(ln("CN.BIN", T "/BLOBS/" HASH_B), OK, "SYSTEM links to a content name");
    as_user();

    EXPECT(wr(T "/BLOBS/" HASH_A, 0, "x"), PERM, "USER cannot write a content object");
    EXPECT(tr(T "/BLOBS/" HASH_A, 0), PERM, "USER cannot truncate a content object");
    EXPECT(ln(T "/BLOBS/" HASH_A, T "/BLOBS/alias"), OK, "USER may add an ordinary name");
    EXPECT(wr(T "/BLOBS/alias", 0, "x"), PERM, "no write through another name of the object");
    EXPECT(ln(T "/BLOBS/" HASH_A, T "/CONFIG21/KERNEL"), OK, "USER links it into an open slot");
    EXPECT(wr(T "/CONFIG21/KERNEL", 0, "x"), PERM, "no write through the slot name");
    EXPECT(rm(T "/BLOBS/alias"), OK, "USER removes its own name");
    EXPECT(rm(T "/BLOBS/" HASH_A), PERM, "USER cannot unlink a content name");
    EXPECT(mv(T "/BLOBS/" HASH_A, T "/BLOBS/moved"), PERM, "USER cannot rename away a content name");
    EXPECT(mv(T "/BLOBS/" HASH_A "0", T "/BLOBS/" HASH_B), PERM, "USER cannot rename over a content name");
    EXPECT(rd(T "/CONFIG21"), OK, "USER removes a slot that links the content object");
    EXPECT(rm(T "/BLOBS/" HASH_A), RWY_ERR_NOENT, "the last slot took the content object with it");

    as_sys();
    EXPECT(rm(T "/BLOBS/" HASH_B), OK, "SYSTEM unlinks a content name");
    as_user();
    EXPECT(rm(T "/BLOBS/" HASH_A "0"), OK, "ordinary names go");
}

/* The other tests open a transaction per call, which drops the classification
 * cache; keep several calls on one OID inside a single transaction here. */
void test_class_cache(void) {
    uint64_t o;
    printf("== classification cache ==\n");
    EXPECT(mk(T "/CONFIG5"), OK, "CONFIG5");
    EXPECT(cr(T "/BLOBS/c1"), OK, "blob c1");
    rwy_begin(&S);
    CHECK(rwy_lookup(&S, T "/BLOBS/c1", &o) == OK, "lookup c1");
    CHECK(rwy_write(&S, o, 0, "a", 1) == OK, "unshared blob writable");
    CHECK(rwy_write(&S, o, 0, "b", 1) == OK, "...and again (cached)");
    CHECK(rwy_link(&S, T "/BLOBS/c1", T "/CONFIG5/KERNEL") == OK, "link into the slot");
    CHECK(rwy_write(&S, o, 0, "c", 1) == PERM, "linked blob frozen, cache not stale");
    rwy_abort(&S);

    EXPECT(cr(T "/CONFIG5/KERNEL"), OK, "KERNEL");
    EXPECT(wr(T "/CONFIG5/KERNEL", 0, "k"), OK, "KERNEL content");
    EXPECT(cr(T "/CONFIG5/STATE"), OK, "STATE");
    rwy_begin(&S);
    CHECK(rwy_lookup(&S, T "/CONFIG5/STATE", &o) == OK, "lookup STATE");
    CHECK(rwy_write(&S, o, 0, "INSTALLED", 9) == OK, "INSTALLED finalizes");
    CHECK(rwy_write(&S, o, 0, "INSTALLED", 9) == RWY_ERR_NOENT, "...the tree STATE is gone afterwards (cache not stale)");
    rwy_abort(&S);
}

/* How often `name` is listed in `dir`, by readdir_nth and by readdir; the type seen by readdir_nth is in *type. */
static int listed_as(const char *dir, const char *name, uint8_t *type) {
    char one[RWY_NAME_MAX + 1], names[2048];
    uint8_t types[64], t;
    size_t count = 64, off = 0, i;
    int found = 0, batch = 0;
    for (i = 0; rwy_readdir_nth(&S, dir, i, one, sizeof(one), &t) == RWY_OK; i++)
        if (!strcmp(one, name)) { found++; *type = t; }
    if (rwy_readdir(&S, dir, names, sizeof(names), &count, types) != RWY_OK) return -1;
    for (i = 0; i < count; i++) {
        if (!strcmp(names + off, name)) batch++;
        off += strlen(names + off) + 1;
    }
    return found == batch ? found : -1;
}

void test_booted_and_root(void) {
    rwy_attr_t a;
    char b[64];
    uint64_t q;
    printf("== BOOTED, REASON, RECOVER_NEXT ==\n");
    EXPECT(rwy_lstat(&S, T "/BOOTED", &a), RWY_ERR_NOENT, "BOOTED is absent until selection names a slot");
    EXPECT(rwy_lstat(&S, T "/REASON", &a), RWY_ERR_NOENT, "REASON is absent too");
    set_booted("CONFIG3");
    {
        uint8_t ty = 0;
        CHECK(listed_as(T, "BOOTED", &ty) == 1 && ty == RWY_TYPE_SYMLINK, "BOOTED is listed in the root, as a symlink");
        CHECK(listed_as(T "/CONFIG3", "STATE", &ty) == 1 && listed_as(T "/CONFIG3", "TRIES", &ty) == 1 &&
              listed_as(T "/CONFIG3", "INDEX", &ty) == 1 && ty == RWY_TYPE_FILE, "a finalized slot lists STATE, TRIES and INDEX once");
        CHECK(listed_as(T "/CONFIG3", "BOOTED", &ty) == 0 && listed_as(T "/BLOBS", "STATE", &ty) == 0,
              "virtual names only appear where they belong");
    }
    EXPECT(rwy_lstat(&S, T "/BOOTED", &a), OK, "BOOTED appears with the record");
    CHECK(RWY_S_ISLNK(a.mode) && a.type == RWY_TYPE_SYMLINK && a.size == 7, "a symlink of the length of the name");
    EXPECT(rwy_readlink(&S, T "/BOOTED", b, sizeof(b)), OK, "readlink");
    CHECK(!strcmp(b, "CONFIG3"), "BOOTED names CONFIG3");
    EXPECT(wr(T "/BOOTED/STATE", 0, "FAILED"), OK, "USER confirms through BOOTED");
    same_text(T "/CONFIG3/STATE", "FAILED\n", "write landed in CONFIG3");
    same_text(T "/BOOTED/STATE", "FAILED\n", "BOOTED/STATE reads it back");
    EXPECT(wr(T "/BOOTED/STATE", 0, "TESTING"), PERM, "TESTING through BOOTED");
    EXPECT(wr(T "/BOOTED/KERNEL", 0, "x"), PERM, "KERNEL through BOOTED");
    EXPECT(rwy_stat(&S, T "/BOOTED", &a), OK, "stat follows the link");
    CHECK(RWY_S_ISDIR(a.mode), "...to the slot directory");
    for (int sys = 0; sys < 2; sys++) {
        if (sys) as_sys(); else as_user();
        EXPECT(rm(T "/BOOTED"), PERM, "BOOTED cannot be unlinked");
        EXPECT(mv(T "/BOOTED", T "/BOOTED2"), PERM, "BOOTED cannot be renamed");
        EXPECT(sy(T "/BOOTED", "CONFIG1"), PERM, "BOOTED cannot be created over");
        EXPECT(ln(T "/CONFIG3/KERNEL", T "/BOOTED"), PERM, "nor linked over");
        EXPECT(ch(T "/BOOTED"), PERM, "BOOTED has no attributes to change");
    }
    as_user();

    EXPECT(rwy_lstat(&S, T "/REASON", &a), RWY_ERR_NOENT, "REASON still absent");
    set_reason(RWY_SS_REASON_ALL_FAILED);
    {
        uint8_t ty = 0;
        CHECK(listed_as(T, "REASON", &ty) == 1 && ty == RWY_TYPE_FILE, "REASON is listed once the record has one");
    }
    same_text(T "/REASON", "all_failed\n", "REASON all_failed");
    set_reason(RWY_SS_REASON_NORMAL);
    same_text(T "/REASON", "normal\n", "REASON normal");
    set_reason(RWY_SS_REASON_REQUESTED);
    same_text(T "/REASON", "requested\n", "REASON requested");
    set_reason(RWY_SS_REASON_INVALID_STORE);
    same_text(T "/REASON", "invalid_store\n", "REASON invalid_store");
    set_reason(RWY_SS_REASON_BOOT_LOOP);
    same_text(T "/REASON", "boot_loop\n", "REASON boot_loop");
    for (int sys = 0; sys < 2; sys++) {
        if (sys) as_sys(); else as_user();
        EXPECT(wr(T "/REASON", 0, "normal"), PERM, "REASON is read-only");
        EXPECT(rm(T "/REASON"), PERM, "REASON cannot be unlinked");
        EXPECT(cr(T "/REASON"), PERM, "REASON cannot be created");
        EXPECT(tr(T "/REASON", 0), PERM, "REASON cannot be truncated");
    }
    as_user();

    /* RECOVER_NEXT is a flag in the record: a request costs no inode and no tree commit. */
    (void)rm(T "/RECOVER_NEXT");
    EXPECT(rwy_lstat(&S, T "/RECOVER_NEXT", &a), RWY_ERR_NOENT, "no request yet");
    q = rseq();
    EXPECT(cr(T "/RECOVER_NEXT"), OK, "USER creates RECOVER_NEXT");
    CHECK(rseq() == q + 1, "one record write");
    EXPECT(cr(T "/RECOVER_NEXT"), RWY_ERR_EXIST, "a second request is refused");
    EXPECT(rwy_lstat(&S, T "/RECOVER_NEXT", &a), OK, "RECOVER_NEXT is visible");
    CHECK(a.size == 0 && RWY_S_ISREG(a.mode), "empty regular file");
    EXPECT(wr(T "/RECOVER_NEXT", 0, "x"), INV, "RECOVER_NEXT is empty");
    EXPECT(wr(T "/RECOVER_NEXT", 0, ""), OK, "an empty write is accepted");
    EXPECT(tr(T "/RECOVER_NEXT", 0), OK, "RECOVER_NEXT truncate(0)");
    EXPECT(tr(T "/RECOVER_NEXT", 1), INV, "truncate to non-zero");
    EXPECT(mv(T "/RECOVER_NEXT", T "/RECOVER_NEXT2"), PERM, "RECOVER_NEXT cannot be renamed");
    EXPECT(ut(T "/RECOVER_NEXT"), OK, "touch RECOVER_NEXT is a no-op");
    EXPECT(ch(T "/RECOVER_NEXT"), OK, "chmod RECOVER_NEXT is a no-op");
    EXPECT(co(T "/RECOVER_NEXT"), OK, "chown RECOVER_NEXT is a no-op");
    EXPECT(sx(T "/RECOVER_NEXT"), PERM, "setxattr RECOVER_NEXT stays refused");
    EXPECT(rm(T "/RECOVER_NEXT"), OK, "USER deletes RECOVER_NEXT");
    EXPECT(rwy_lstat(&S, T "/RECOVER_NEXT", &a), RWY_ERR_NOENT, "gone");
    EXPECT(cr(T "/RECOVER_NEXT"), OK, "USER creates RECOVER_NEXT");
    as_sys();
    EXPECT(rm(T "/RECOVER_NEXT"), OK, "SYSTEM consumes RECOVER_NEXT");
    as_user();
    set_booted(0);
    set_reason(0);
}

void test_recovery(void) {
    printf("== sealed RECOVERY ==\n");
    seed_dir(T "/RECOVERY");
    seed(T "/RECOVERY/KERNEL", "rk");
    seed(T "/RECOVERY/STATE", "OK");
    for (int a = 0; a < 2; a++) {
        if (a) as_sys(); else as_user();
        EXPECT(cr(T "/RECOVERY/ARGS"), PERM, "create in RECOVERY");
        EXPECT(cr(T "/RECOVERY/LABEL"), PERM, "create in RECOVERY");
        EXPECT(wr(T "/RECOVERY/KERNEL", 0, "x"), PERM, "write RECOVERY/KERNEL");
        EXPECT(wr(T "/RECOVERY/STATE", 0, "OK"), PERM, "write RECOVERY/STATE");
        EXPECT(tr(T "/RECOVERY/KERNEL", 0), PERM, "truncate RECOVERY/KERNEL");
        EXPECT(rm(T "/RECOVERY/KERNEL"), PERM, "unlink RECOVERY/KERNEL");
        EXPECT(mv(T "/RECOVERY/KERNEL", T "/RECOVERY/K2"), PERM, "rename in RECOVERY");
        EXPECT(ch(T "/RECOVERY/KERNEL"), PERM, "chmod RECOVERY/KERNEL");
        EXPECT(rd(T "/RECOVERY"), PERM, "rmdir RECOVERY");
    }
    as_user();
    EXPECT(ln(T "/RECOVERY/KERNEL", T "/CONFIG4/DTB2"), INV, "unknown link name");
    EXPECT(rm(T "/CONFIG4/INITRD"), OK, "make room");
    EXPECT(ln(T "/RECOVERY/KERNEL", T "/CONFIG4/INITRD"), OK, "RECOVERY payload may be shared into a slot");
    EXPECT(wr(T "/CONFIG4/INITRD", 0, "x"), PERM, "...and stays sealed through the slot name");
    set_booted("RECOVERY");
    EXPECT(wr(T "/BOOTED/STATE", 0, "OK"), PERM, "STATE write through BOOTED -> RECOVERY");
    as_sys();
    EXPECT(wr(T "/BOOTED/STATE", 0, "OK"), PERM, "...also for SYSTEM");
    as_user();
    set_booted(0);
}

/* WDT holds EFI applications: sealed like RECOVERY, regular files and blob links only. */
void test_wdt(void) {
    static char names[RWY_WDT_MAX][RWY_WDT_NAME_MAX + 1];
    char many[32];
    int n;
    printf("== sealed WDT ==\n");
    EXPECT(mk(T "/WDT"), PERM, "WDT by USER");
    as_sys();
    EXPECT(mk(T "/WDT"), PERM, "WDT by SYSTEM without unlock");
    as_user();
    CHECK(rwy_bootcfg_wdt_list(&S, names, RWY_WDT_MAX) == 0, "no WDT: empty list");
    unlock(1); as_sys();
    EXPECT(mk(T "/WDT"), OK, "unlocked: create WDT");
    EXPECT(cr(T "/WDT/I6300ESB.EFI"), OK, "application");
    EXPECT(cr(T "/WDT/itco-2_a.efi"), OK, "name is case-insensitive and may hold - and _");
    EXPECT(cr(T "/WDT/A.TXT"), INV, "not .EFI");
    EXPECT(cr(T "/WDT/.EFI"), INV, "empty stem");
    EXPECT(cr(T "/WDT/A B.EFI"), INV, "space in the name");
    EXPECT(cr(T "/WDT/A.B.EFI"), INV, "dot in the stem");
    EXPECT(cr(T "/WDT/ABCDEFGHIJKLMNOPQRSTUVWXYZ012.EFI"), INV, "name over 32 characters");
    EXPECT(cr(T "/WDT/ABCDEFGHIJKLMNOPQRSTUVWXYZ01.EFI"), OK, "exactly 32 characters");
    EXPECT(cr(T "/WDT/ABCDEFGHIJKLMNOPQRSTUVWXYZ.EFI"), OK, "30 characters");
    EXPECT(mk(T "/WDT/SUB.EFI"), INV, "no subdirectory");
    EXPECT(sy(T "/WDT/LINK.EFI", "I6300ESB.EFI"), INV, "no symlink");
    EXPECT(wr(T "/WDT/I6300ESB.EFI", 0, "app"), OK, "write an application");
    EXPECT(wr(T "/WDT/I6300ESB.EFI", 1024 * 1024, "x"), INV, "write past 1 MiB");
    EXPECT(tr(T "/WDT/I6300ESB.EFI", 1024 * 1024 + 1), INV, "truncate past 1 MiB");
    EXPECT(ch(T "/WDT/I6300ESB.EFI"), PERM, "chmod stays refused");
    as_user(); unlock(0);

    for (int a = 0; a < 2; a++) {
        if (a) as_sys(); else as_user();
        EXPECT(cr(T "/WDT/NEW.EFI"), PERM, "create in WDT");
        EXPECT(wr(T "/WDT/I6300ESB.EFI", 0, "x"), PERM, "write an application");
        EXPECT(tr(T "/WDT/I6300ESB.EFI", 0), PERM, "truncate an application");
        EXPECT(rm(T "/WDT/I6300ESB.EFI"), PERM, "unlink an application");
        EXPECT(mv(T "/WDT/I6300ESB.EFI", T "/WDT/OTHER.EFI"), PERM, "rename in WDT");
        EXPECT(ch(T "/WDT/I6300ESB.EFI"), PERM, "chmod an application");
        EXPECT(rd(T "/WDT"), PERM, "rmdir WDT");
        EXPECT(mv(T "/WDT", T "/WDT2"), PERM, "rename WDT");
    }
    as_user();

    /* Only a blob may gain a name in WDT, and then it is as immutable as the blob. */
    EXPECT(cr(T "/BLOBS/wdtb"), OK, "blob");
    EXPECT(wr(T "/BLOBS/wdtb", 0, "blob"), OK, "blob content");
    EXPECT(ln(T "/BLOBS/wdtb", T "/WDT/BLOB.EFI"), PERM, "USER cannot link into WDT");
    unlock(1); as_sys();
    EXPECT(ln(T "/BLOBS/wdtb", T "/WDT/BLOB.EFI"), OK, "unlocked: link a blob");
    EXPECT(ln(T "/RECOVERY/KERNEL", T "/WDT/RK.EFI"), PERM, "a payload is not a blob");
    EXPECT(ln(T "/WDT/I6300ESB.EFI", T "/BLOBS/wdtc"), PERM, "an application cannot gain another name");
    EXPECT(ln(T "/WDT/I6300ESB.EFI", T "/WDT/COPY.EFI"), PERM, "...not even inside WDT");
    as_user(); unlock(0);
    EXPECT(wr(T "/BLOBS/wdtb", 0, "x"), PERM, "the blob is immutable once linked");
    EXPECT(wr(T "/WDT/BLOB.EFI", 0, "x"), PERM, "...and so is its WDT name");

    /* Listing: sorted, bounded, the total is returned. */
    n = rwy_bootcfg_wdt_list(&S, names, RWY_WDT_MAX);
    CHECK(n == 5 && !strcmp(names[0], "ABCDEFGHIJKLMNOPQRSTUVWXYZ.EFI") && !strcmp(names[1], "ABCDEFGHIJKLMNOPQRSTUVWXYZ01.EFI") &&
          !strcmp(names[2], "BLOB.EFI") && !strcmp(names[3], "I6300ESB.EFI") && !strcmp(names[4], "itco-2_a.efi"),
          "list is sorted");
    unlock(1); as_sys();
    for (int i = 0; i < 6; i++) {
        snprintf(many, sizeof(many), T "/WDT/M%d.EFI", i);
        EXPECT(cr(many), OK, "more applications");
    }
    as_user(); unlock(0);
    n = rwy_bootcfg_wdt_list(&S, names, 3);
    CHECK(n == 11 && !strcmp(names[0], "ABCDEFGHIJKLMNOPQRSTUVWXYZ.EFI") && !strcmp(names[1], "ABCDEFGHIJKLMNOPQRSTUVWXYZ01.EFI") &&
          !strcmp(names[2], "BLOB.EFI"), "bounded list keeps the first names, returns the total");
    CHECK(rwy_bootcfg_wdt_list(&S, names, 0) == 11, "no room: still counts");
}

void test_wdt_hint(void) {
    char name[RWY_WDT_NAME_MAX + 1], longname[RWY_WDT_NAME_MAX + 2];
    printf("== remembered WDT application ==\n");
    CHECK(rwy_bootcfg_wdt_hint(&S, name) == 0 && !name[0], "no hint at first");
    CHECK(read_all(T "/WDT_HINT", longname, sizeof(longname)) == RWY_ERR_NOENT, "no WDT_HINT file without a hint");
    CHECK(rwy_bootcfg_wdt_set_hint(&S, "I6300ESB.EFI") == RWY_OK, "record a hint");
    CHECK(rwy_bootcfg_wdt_hint(&S, name) == 12 && !strcmp(name, "I6300ESB.EFI"), "hint reads back");
    same_text(T "/WDT_HINT", "I6300ESB.EFI\n", "WDT_HINT");
    for (int a = 0; a < 2; a++) {
        if (a) as_sys(); else as_user();
        EXPECT(wr(T "/WDT_HINT", 0, "X.EFI"), PERM, "write WDT_HINT");
        EXPECT(tr(T "/WDT_HINT", 0), PERM, "truncate WDT_HINT");
        EXPECT(rm(T "/WDT_HINT"), PERM, "unlink WDT_HINT");
    }
    as_user();
    CHECK(rwy_bootcfg_wdt_hint(&S, name) == 12, "refused writes leave the hint");
    memset(longname, 'A', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = 0;
    CHECK(rwy_bootcfg_wdt_set_hint(&S, longname) == RWY_ERR_INVAL, "name over 32 characters");
    CHECK(rwy_bootcfg_wdt_hint(&S, name) == 12, "...changes nothing");
    CHECK(rwy_bootcfg_wdt_set_hint(&S, NULL) == RWY_OK && !rwy_bootcfg_wdt_hint(&S, name), "clear the hint");
    CHECK(read_all(T "/WDT_HINT", longname, sizeof(longname)) == RWY_ERR_NOENT, "WDT_HINT is gone");
}

void test_rename_rmdir(void) {
    printf("== rename and rmdir ==\n");
    EXPECT(mv(T "/CONFIG1", T "/CONFIG9"), PERM, "slots cannot be renamed");
    EXPECT(mv(T "/BLOBS", T "/BLOBS2"), PERM, "BLOBS cannot be renamed");
    EXPECT(mv("EFI/CONFIG01", T "/CONFIG9"), PERM, "no rename into the subtree");
    EXPECT(mv(T "/BLOBS/b2", "OUT2.BIN"), PERM, "no rename out of the subtree");
    EXPECT(mv(T "/BLOBS/b2", T "/CONFIG4/KERNEL"), PERM, "no rename between directories of the subtree");
    EXPECT(mv(T "/BLOBS/b2", T "/BLOBS/b3"), OK, "rename inside BLOBS");
    EXPECT(mv(T "/WATCHDOG", T "/REASON"), PERM, "rename onto a SYSTEM-only name");
    EXPECT(mv(T "/WATCHDOG", T "/NOPE"), INV, "rename to an unknown name");
    EXPECT(rd(T "/CONFIG0"), OK, "rmdir empty slot");
    EXPECT(rd(T "/CONFIG1"), OK, "rmdir of a slot removes what it holds");
    EXPECT(rd(T "/BLOBS"), RWY_ERR_NOTEMPTY, "rmdir non-empty BLOBS");
    EXPECT(rd(T), RWY_ERR_NOTEMPTY, "rmdir non-empty subtree");
}

void test_finalize(void) {
    printf("== finalize and revision counter ==\n");
    EXPECT(mk(T), OK, "subtree");
    EXPECT(mk(T "/BLOBS"), OK, "BLOBS");
    CHECK(counter() == -1, "no counter yet");
    slot(10);
    CHECK(rev(10) == -1, "incomplete slot has no REVISION");
    EXPECT(install(10), OK, "install slot 10");
    CHECK(rev(10) == 1 && counter() == 1, "first revision is 1");
    same_text(T "/CONFIG10/STATE", "INSTALLED\n", "STATE");
    slot(3);
    EXPECT(install(3), OK, "install slot 3");
    CHECK(rev(3) == 2 && counter() == 2, "revision does not follow the directory name");
    EXPECT(install(3), PERM, "INSTALLED on a finalized slot");
    CHECK(rev(3) == 2 && counter() == 2, "refused install changes nothing");
    drop(3);
    slot(4);
    EXPECT(install(4), OK, "install after delete");
    CHECK(rev(4) == 3 && counter() == 3, "revision of a deleted slot is not reused");

    /* Validation failures abort the whole commit; the slot can be fixed and retried. */
    EXPECT(mk(T "/CONFIG20"), OK, "slot 20");
    EXPECT(cr(T "/CONFIG20/STATE"), OK, "STATE");
    EXPECT(install(20), INV, "no KERNEL");
    CHECK(rev(20) == -1 && counter() == 3, "nothing allocated");
    same_text(T "/CONFIG20/STATE", "", "STATE still empty");
    EXPECT(cr(T "/CONFIG20/KERNEL"), OK, "empty KERNEL");
    EXPECT(install(20), INV, "empty KERNEL");
    EXPECT(wr(T "/CONFIG20/KERNEL", 0, "k"), OK, "KERNEL content");
    EXPECT(cr(T "/CONFIG20/TRIES"), OK, "empty TRIES");
    EXPECT(install(20), INV, "unparsable TRIES");
    EXPECT(wr(T "/CONFIG20/TRIES", 0, "2"), OK, "TRIES");
    EXPECT(sy(T "/CONFIG20/INITRD", "../BLOBS/nope"), OK, "dangling INITRD");
    EXPECT(install(20), INV, "dangling INITRD");
    EXPECT(rm(T "/CONFIG20/INITRD"), OK, "drop INITRD");
    EXPECT(cr(T "/CONFIG20/LABEL"), OK, "create LABEL");
    EXPECT(wr(T "/CONFIG20/LABEL", 0, "bad\"quote"), OK, "LABEL with quote");
    EXPECT(install(20), INV, "finalize rejects quote in LABEL");
    EXPECT(wr(T "/CONFIG20/LABEL", 0, "bad\x1f" "ctrl"), OK, "LABEL with control char");
    EXPECT(install(20), INV, "finalize rejects control char in LABEL");
    EXPECT(wr(T "/CONFIG20/LABEL", 0, "valid-label-utf8-\xc3\xa9\n"), OK, "valid UTF-8 LABEL with trailing newline");
    EXPECT(cr(T "/CONFIG20/META"), OK, "create META");
    EXPECT(wr(T "/CONFIG20/META", 0, "title bad\n"), OK, "unknown key in META");
    EXPECT(install(20), INV, "finalize rejects unknown key in META");
    EXPECT(wr(T "/CONFIG20/META", 0, "version 1.0\nversion 2.0\n"), OK, "duplicate key in META");
    EXPECT(install(20), INV, "finalize rejects duplicate key in META");
    EXPECT(wr(T "/CONFIG20/META", 0, "version\n"), OK, "missing value in META");
    EXPECT(install(20), INV, "finalize rejects missing value in META");
    EXPECT(wr(T "/CONFIG20/META", 0, "version 1.0\nsort-key debian\n"), OK, "valid META");
    /* Type 2 rule: .efi cannot have META or ARGS */
    EXPECT(cr(T "/CONFIG20/ENTRY"), OK, "create ENTRY");
    EXPECT(wr(T "/CONFIG20/ENTRY", 0, "slot20.efi"), OK, "Type 2 ENTRY");
    EXPECT(install(20), INV, "finalize rejects Type 2 with META");
    EXPECT(rm(T "/CONFIG20/META"), OK, "remove META");
    EXPECT(cr(T "/CONFIG20/ARGS"), OK, "create ARGS");
    EXPECT(wr(T "/CONFIG20/ARGS", 0, "quiet"), OK, "write ARGS");
    EXPECT(install(20), INV, "finalize rejects Type 2 with ARGS");
    EXPECT(rm(T "/CONFIG20/ARGS"), OK, "remove ARGS");
    EXPECT(wr(T "/CONFIG20/ENTRY", 0, "slot20.conf"), OK, "switch back to Type 1");
    EXPECT(cr(T "/CONFIG20/META"), OK, "re-create META");
    EXPECT(wr(T "/CONFIG20/META", 0, "version 1.0\nsort-key debian\n"), OK, "valid META");
    EXPECT(install(20), OK, "fixed slot installs");
    CHECK(rev(20) == 4 && counter() == 4, "retry allocates the next revision");

    /* Payload links: inside the subtree only, at most 8 hops, ending at a non-empty file. */
    EXPECT(cr(T "/BLOBS/uki"), OK, "blob");
    EXPECT(wr(T "/BLOBS/uki", 0, "uki"), OK, "blob content");
    EXPECT(mk(T "/CONFIG21"), OK, "slot 21");
    EXPECT(ln(T "/BLOBS/uki", T "/CONFIG21/KERNEL"), OK, "hardlinked KERNEL");
    EXPECT(cr(T "/CONFIG21/STATE"), OK, "STATE");
    EXPECT(install(21), OK, "hardlinked KERNEL installs");
    EXPECT(mk(T "/CONFIG22"), OK, "slot 22");
    EXPECT(sy(T "/CONFIG22/KERNEL", "/" T "/BLOBS/uki"), OK, "absolute symlink");
    EXPECT(cr(T "/CONFIG22/STATE"), OK, "STATE");
    EXPECT(install(22), OK, "absolute symlink inside the subtree");
    EXPECT(mk(T "/CONFIG23"), OK, "slot 23");
    EXPECT(sy(T "/CONFIG23/KERNEL", "../../../README.TXT"), OK, "symlink out of the subtree");
    EXPECT(cr(T "/CONFIG23/STATE"), OK, "STATE");
    EXPECT(install(23), INV, "symlink target outside EFI/RUNWAY");
    EXPECT(mk(T "/CONFIG24"), OK, "slot 24");
    EXPECT(sy(T "/CONFIG24/KERNEL", "KERNEL"), OK, "self loop");
    EXPECT(cr(T "/CONFIG24/STATE"), OK, "STATE");
    EXPECT(install(24), INV, "symlink loop");
    EXPECT(mk(T "/CONFIG25"), OK, "slot 25");
    EXPECT(sy(T "/CONFIG25/KERNEL", "../BLOBS/../../../../README.TXT"), OK, "climbing symlink");
    EXPECT(cr(T "/CONFIG25/STATE"), OK, "STATE");
    EXPECT(install(25), INV, "climbing out through ..");
    /* chain: CONFIG3<i>/KERNEL -> CONFIG3<i+1>/KERNEL, last one regular */
    for (int i = 30; i <= 39; i++) {
        char d[64], k[64];
        snprintf(d, sizeof(d), T "/CONFIG%d", i);
        snprintf(k, sizeof(k), T "/CONFIG%d/KERNEL", i);
        EXPECT(mk(d), OK, "chain slot");
        if (i < 39) { char t[64]; snprintf(t, sizeof(t), "../CONFIG%d/KERNEL", i + 1); EXPECT(sy(k, t), OK, "chain link"); }
        else { EXPECT(cr(k), OK, "chain end"); EXPECT(wr(k, 0, "k"), OK, "chain end content"); }
        snprintf(k, sizeof(k), T "/CONFIG%d/STATE", i);
        EXPECT(cr(k), OK, "STATE");
    }
    EXPECT(install(30), INV, "9 hops");
    EXPECT(install(31), OK, "8 hops");

    /* Counter table (§2.5). Highest slot revision is 7 here; slots 30..39 stay incomplete. */
    CHECK(counter() == 7 && rev(31) == 7, "revisions are dense");
    {
        long hi = counter(), r;
        char b[32];
        set_counter("");
        slot(40);
        EXPECT(install(40), OK, "counter missing");
        CHECK(counter() == hi + 1 && rev(40) == hi + 1, "missing counter starts from the highest slot");
        hi = counter();
        set_counter("1\n");
        slot(41);
        EXPECT(install(41), OK, "counter behind");
        CHECK(counter() == hi + 1 && rev(41) == hi + 1, "counter behind: max + 1, counter repaired");
        hi = counter();
        set_counter("junk");
        slot(42);
        EXPECT(install(42), OK, "counter unparsable");
        CHECK(counter() == hi + 1 && rev(42) == hi + 1, "unparsable counter repaired");
        hi = counter();
        snprintf(b, sizeof(b), "%ld\n", hi + 100);
        set_counter(b);
        slot(43);
        EXPECT(install(43), OK, "counter ahead");
        CHECK(counter() == hi + 101 && rev(43) == hi + 101, "counter ahead of every slot is honoured");
        r = counter();
        set_counter("18446744073709551615");
        slot(44);
        EXPECT(install(44), RWY_ERR_RANGE, "counter at UINT64_MAX");
        CHECK(rev(44) == -1, "no revision at UINT64_MAX");
        same_text(T "/REVISION", "18446744073709551615", "counter untouched");
        set_counter("junk\n");
        as_sys();
        EXPECT(install(44), OK, "SYSTEM finalizes too");
        as_user();
        CHECK(rev(44) == r + 1 && counter() == r + 1, "highest slot wins over a broken counter");
    }
    EXPECT(wr(T "/REVISION", 0, "99\n"), PERM, "counter is not writable");

    /* A slot name used again and again: the entries of removed revisions are dropped, the table never fills. */
    {
        int bad = 0;
        for (int i = 0; i < 150 && !bad; i++) {
            slot(60);
            bad = install(60) != OK;
            drop(60);
        }
        CHECK(!bad, "150 install and remove cycles of one slot name");
    }
}

static int idx_listed(const char *dir) {
    char name[RWY_NAME_MAX + 1];
    int found = 0;
    for (size_t i = 0; rwy_readdir_nth(&S, dir, i, name, sizeof(name), 0) == RWY_OK; i++)
        if (!strcmp(name, "INDEX")) found++;
    return found;
}
static int idx_listed_batch(const char *dir) {
    static char names[8192];
    size_t cnt = 64, off = 0;
    int found = 0;
    if (rwy_readdir(&S, dir, names, sizeof(names), &cnt, 0) != RWY_OK) return -1;
    for (size_t i = 0; i < cnt; i++) { if (!strcmp(names + off, "INDEX")) found++; off += strlen(names + off) + 1; }
    return found;
}

static int mkslot(const char *n) {             /* incomplete slot CONFIG<n>, one-byte KERNEL */
    char d[96], k[96];
    snprintf(d, sizeof(d), T "/CONFIG%s", n);
    snprintf(k, sizeof(k), T "/CONFIG%s/KERNEL", n);
    return mk(d) == OK && cr(k) == OK && wr(k, 0, "k") == OK;
}
static int install_s(const char *n) {
    char st[96];
    snprintf(st, sizeof(st), T "/CONFIG%s/STATE", n);
    return cr(st) == OK ? wr(st, 0, "INSTALLED\n") : -1;
}

void test_slot_names(void) {
    static const char *const bad[] = { "01", "00", "007", "18446744073709551616", "99999999999999999999",
                                       "100000000000000000000", "123456789012345678901", "-1", "+1", "1 ", " 1", "0x1", "" };
    static const char *const good[] = { "0", "18446744073709551615", "9" };   /* installed in this order */
    rwy_boot_choice_t c;
    char p[160], line[160];
    printf("== slot-name corners ==\n");
    fresh_subtree();
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        snprintf(p, sizeof(p), T "/CONFIG%s", bad[i]);
        EXPECT(mk(p), INV, bad[i]);
        priv(1); EXPECT(mk(p), INV, "same with the privileged flag"); priv(0);
        as_sys(); EXPECT(mk(p), INV, "same for SYSTEM"); as_user();
        snprintf(p, sizeof(p), "EFI/CONFIG%s", bad[i]);
        EXPECT(mk(p), OK, "outside the subtree it is an ordinary name");
        snprintf(line, sizeof(line), T "/CONFIG%s", bad[i]);
        EXPECT(mv(p, line), PERM, "no rename into the subtree");
    }
    for (unsigned i = 0; i < 3; i++) {
        CHECK(mkslot(good[i]), "slot setup");
        snprintf(p, sizeof(p), T "/CONFIG%s/INDEX", good[i]);
        snprintf(line, sizeof(line), "%s\n", good[i]);
        same_text(p, line, "INDEX is n");
    }
    for (unsigned i = 0; i < 3; i++) {
        CHECK(install_s(good[i]) == OK, "finalize");
        snprintf(p, sizeof(p), T "/CONFIG%s/REVISION", good[i]);
        snprintf(line, sizeof(line), "%u\n", i + 1);
        same_text(p, line, "revision follows install order, not the name");
    }
    same_text(T "/REVISION", "3\n", "counter");
    /* selection orders by REVISION: CONFIG9 (rev 3), then the largest n (rev 2), then CONFIG0 (rev 1) */
    EXPECT(sel(&c), OK, "select");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG9") && c.revision == 3, "highest REVISION wins whatever the name");
    set_state(9, "FAILED\n");
    EXPECT(sel(&c), OK, "select after CONFIG9 FAILED");
    CHECK(!strcmp(c.slot, "CONFIG18446744073709551615") && c.revision == 2 && c.tries_left == 0, "largest n");
    CHECK(has("RUNWAY: booting CONFIG18446744073709551615 (rev 2, TESTING, 0 tries left)"), "message with a 20-digit name");
    CHECK(booted_is("CONFIG18446744073709551615"), "BOOTED names it");
    same_text(T "/BOOTED/INDEX", "18446744073709551615\n", "BOOTED/INDEX");
    EXPECT(wr(T "/BOOTED/STATE", 0, "OK\n"), OK, "confirm through BOOTED");
    CHECK(text_is(T "/CONFIG18446744073709551615/STATE", "OK\n"), "reaches the slot");
    set_state(0, "OK\n");
    {
        char s0[96];
        snprintf(s0, sizeof(s0), T "/CONFIG18446744073709551615/STATE");
        priv(1); tr(s0, 0); wr(s0, 0, "FAILED\n"); priv(0);
    }
    EXPECT(sel(&c), OK, "select after the largest n FAILED");
    CHECK(!strcmp(c.slot, "CONFIG0") && c.revision == 1, "CONFIG0 boots");
    CHECK(booted_is("CONFIG0"), "BOOTED -> CONFIG0");
    same_text(T "/BOOTED/INDEX", "0\n", "BOOTED/INDEX of CONFIG0");
    }

void test_link_corners(void) {
    rwy_boot_choice_t c;
    printf("== link corners after finalize ==\n");
    fresh_subtree();
    EXPECT(cr(T "/BLOBS/uki"), OK, "blob uki");
    EXPECT(wr(T "/BLOBS/uki", 0, "uki"), OK, "uki content");
    EXPECT(cr(T "/BLOBS/dtb"), OK, "blob dtb");
    EXPECT(wr(T "/BLOBS/dtb", 0, "dtb"), OK, "dtb content");
    EXPECT(mk(T "/CONFIG1"), OK, "CONFIG1");
    EXPECT(ln(T "/BLOBS/uki", T "/CONFIG1/KERNEL"), OK, "KERNEL hardlink");
    EXPECT(sy(T "/CONFIG1/DTB", "../BLOBS/dtb"), OK, "DTB symlink");
    EXPECT(cr(T "/CONFIG1/STATE"), OK, "STATE");
    EXPECT(install(1), OK, "finalize");
    EXPECT(wr(T "/CONFIG1/STATE", 0, "OK\n"), OK, "confirmed, so selection does not consume tries");

    /* hardlinked KERNEL: the blob name may go, the slot keeps the pinned content */
    EXPECT(rm(T "/BLOBS/uki"), OK, "unlink the blob name of a linked KERNEL");
    same_text(T "/CONFIG1/KERNEL", "uki", "KERNEL content pinned");
    EXPECT(wr(T "/CONFIG1/KERNEL", 0, "x"), PERM, "KERNEL stays frozen with its last link");
    EXPECT(tr(T "/CONFIG1/KERNEL", 0), PERM, "...and cannot be truncated");
    EXPECT(sel(&c), OK, "select");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG1"), "slot still boots");

    /* a finalized payload name is never replaced or re-pointed by USER */
    EXPECT(sy(T "/CONFIG1/DTB", "../BLOBS/other"), PERM, "no new symlink over DTB");
    EXPECT(mv(T "/BLOBS/dtb", T "/CONFIG1/DTB"), PERM, "no rename over DTB");
    EXPECT(ln(T "/BLOBS/dtb", T "/CONFIG1/INITRD"), PERM, "no new payload name in a finalized slot");
    EXPECT(cr(T "/CONFIG1/INITRD"), PERM, "no new INITRD file");

    /* a symlink payload is a "latest" pointer: its target content may change */
    EXPECT(wr(T "/CONFIG1/DTB", 0, "new"), OK, "write through DTB reaches the target");
    same_text(T "/BLOBS/dtb", "new", "target changed");
    /* ...and a removed target makes the slot invalid (ADR 2.3) */
    EXPECT(rm(T "/BLOBS/dtb"), OK, "unlink the symlink target");
    EXPECT(sel(&c), OK, "select with a dangling DTB");
    CHECK(!c.booted && has("RUNWAY: CONFIG1 invalid (DTB invalid), ignored"), "dangling symlink: slot ignored");

    /* USER may delete payload names of a finalized slot (needed for rm -r); the slot degrades visibly */
    EXPECT(cr(T "/BLOBS/dtb"), OK, "target back");
    EXPECT(sel(&c), OK, "select again");
    CHECK(c.booted, "slot valid again");
    EXPECT(rm(T "/CONFIG1/DTB"), OK, "USER removes DTB of a finalized slot");
    EXPECT(sel(&c), OK, "select without DTB");
    CHECK(c.booted, "boots without it");
    EXPECT(rm(T "/CONFIG1/KERNEL"), OK, "USER removes KERNEL");
    EXPECT(sel(&c), OK, "select without KERNEL");
    CHECK(!c.booted && has("CONFIG1 invalid"), "slot skipped");
}

void test_index(void) {
    rwy_attr_t a;
    uint64_t o, o2;
    char b[64];
    size_t n;
    printf("== virtual INDEX ==\n");
    EXPECT(mk(T), OK, "subtree");
    EXPECT(mk(T "/BLOBS"), OK, "BLOBS");
    slot(7);                                   /* incomplete */
    slot(42);
    EXPECT(install(42), OK, "finalize 42");
    EXPECT(mk(T "/CONFIG18446744073709551615"), OK, "largest n");
    set_booted("CONFIG42");

    /* content, attributes, both ways to reach it */
    same_text(T "/CONFIG42/INDEX", "42\n", "INDEX of a finalized slot is n");
    same_text(T "/CONFIG7/INDEX", "7\n", "INDEX of an incomplete slot is n");
    same_text(T "/CONFIG18446744073709551615/INDEX", "18446744073709551615\n", "INDEX of the largest n");
    same_text(T "/BOOTED/INDEX", "42\n", "BOOTED/INDEX");
    EXPECT(rwy_lstat(&S, T "/CONFIG42/INDEX", &a), OK, "stat INDEX");
    CHECK(RWY_S_ISREG(a.mode) && (a.mode & 07777) == 0444 && a.size == 3 && a.nlink == 1 && a.uid == 0 && a.gid == 0,
          "INDEX is a 0444 regular file of len(n)+1 bytes owned by root");
    EXPECT(rwy_lookup(&S, T "/CONFIG42/INDEX", &o), OK, "lookup INDEX");
    EXPECT(rwy_getattr(&S, o, &a), OK, "getattr by oid");
    CHECK(a.oid == o && a.size == 3, "oid getattr agrees with path stat");
    n = 1;
    EXPECT(rwy_read(&S, o, 0, b, &n), OK, "read by oid at 0");
    CHECK(n == 1 && b[0] == '4', "partial read");
    n = 1;
    EXPECT(rwy_read(&S, o, 1, b, &n), OK, "read at offset 1");
    CHECK(n == 1 && b[0] == '2', "read from offset");
    n = sizeof(b);
    EXPECT(rwy_read(&S, o, 2, b, &n), OK, "read the newline");
    CHECK(n == 1 && b[0] == '\n', "INDEX ends with a newline");
    n = sizeof(b);
    EXPECT(rwy_read(&S, o, 3, b, &n), OK, "read at end");
    CHECK(n == 0, "nothing past the end");
    EXPECT(rwy_lookup(&S, T "/BOOTED/INDEX", &o2), OK, "lookup through BOOTED");
    CHECK(o2 == o, "same object through BOOTED");
    rwy_begin(&S);
    same_text(T "/CONFIG42/INDEX", "42\n", "readable inside a transaction");
    rwy_abort(&S);

    /* reading xattrs is like a file that has none (T8: `ls -l` and `cp -a` do it) */
    CHECK(rwy_listxattr(&S, T "/CONFIG42/INDEX", b, sizeof(b)) == 0, "listxattr of INDEX is empty");
    CHECK(rwy_listxattr_oid(&S, o, b, sizeof(b)) == 0, "listxattr by oid is empty");
    CHECK(rwy_getxattr(&S, T "/CONFIG42/INDEX", "user.x", b, sizeof(b)) == RWY_ERR_XATTR, "getxattr of INDEX has no data");
    CHECK(rwy_getxattr_oid(&S, o, "user.x", b, sizeof(b)) == RWY_ERR_XATTR, "getxattr by oid has no data");

    /* listing */
    CHECK(idx_listed(T "/CONFIG42") == 1 && idx_listed(T "/CONFIG7") == 1, "readdir_nth lists INDEX once");
    CHECK(idx_listed_batch(T "/CONFIG42") == 1 && idx_listed_batch(T "/CONFIG7") == 1, "readdir lists INDEX once");
    CHECK(idx_listed(T) == 0 && idx_listed(T "/BLOBS") == 0, "not in the root or BLOBS");
    CHECK(idx_listed_batch(T) == 0, "not in the root (batch)");
    CHECK(rwy_lstat(&S, T "/INDEX", &a) == RWY_ERR_NOENT && rwy_lstat(&S, T "/BLOBS/INDEX", &a) == RWY_ERR_NOENT,
          "no INDEX outside slots");
    CHECK(rwy_lstat(&S, T "/CONFIG42/INDEX/x", &a) != OK, "INDEX is not a directory");
    CHECK(rwy_lstat(&S, T "/CONFIG99/INDEX", &a) == RWY_ERR_NOENT, "no slot, no INDEX");
    {   /* a stale virtual oid must not resolve once its slot is gone */
        EXPECT(rwy_lookup(&S, T "/CONFIG7/INDEX", &o2), OK, "lookup 7");
        drop(7);
        CHECK(rwy_getattr(&S, o2, &a) == RWY_ERR_NOENT, "INDEX of a removed slot is gone");
        n = sizeof(b);
        CHECK(rwy_read(&S, o2, 0, b, &n) == RWY_ERR_NOENT, "read of a removed slot's INDEX");
    }

    /* every mutation is EPERM, for both actors */
    for (int sys = 0; sys < 2; sys++) {
        if (sys) as_sys(); else as_user();
        EXPECT(cr(T "/CONFIG42/INDEX"), PERM, "create INDEX");
        EXPECT(mk(T "/CONFIG5"), OK, "new slot");
        EXPECT(cr(T "/CONFIG5/INDEX"), PERM, "create INDEX in a new slot");
        EXPECT(rd(T "/CONFIG5"), OK, "slot with only a virtual INDEX is removable");
        EXPECT(mk(T "/CONFIG42/INDEX"), PERM, "mkdir INDEX");
        EXPECT(sy(T "/CONFIG42/INDEX", "x"), PERM, "symlink INDEX");
        EXPECT(rm(T "/CONFIG42/INDEX"), OK, "unlink INDEX is a no-op");
        same_text(T "/CONFIG42/INDEX", "42\n", "...and INDEX is still there");
        EXPECT(rd(T "/CONFIG42/INDEX"), PERM, "rmdir INDEX");
        EXPECT(rm(T "/BOOTED/INDEX"), OK, "unlink via BOOTED is a no-op too");
        EXPECT(wr(T "/CONFIG42/INDEX", 0, "9"), PERM, "write INDEX");
        EXPECT(wr(T "/BOOTED/INDEX", 0, "9"), PERM, "write via BOOTED");
        EXPECT(tr(T "/CONFIG42/INDEX", 0), PERM, "truncate INDEX");
        EXPECT(ch(T "/CONFIG42/INDEX"), PERM, "chmod INDEX");
        EXPECT(co(T "/CONFIG42/INDEX"), PERM, "chown INDEX");
        EXPECT(ut(T "/CONFIG42/INDEX"), PERM, "utimens INDEX");
        EXPECT(sx(T "/CONFIG42/INDEX"), PERM, "setxattr INDEX");
        EXPECT(rx(T "/CONFIG42/INDEX"), PERM, "removexattr INDEX");
        EXPECT(ln(T "/CONFIG42/INDEX", T "/BLOBS/i"), PERM, "link INDEX into BLOBS");
        EXPECT(ln(T "/CONFIG42/INDEX", "I"), PERM, "link INDEX outside");
        EXPECT(ln(T "/CONFIG42/KERNEL", T "/CONFIG42/INDEX"), PERM, "link onto INDEX");
        EXPECT(mv(T "/CONFIG42/INDEX", T "/CONFIG42/STATE"), PERM, "rename INDEX away");
        EXPECT(mv(T "/CONFIG42/KERNEL", T "/CONFIG42/INDEX"), PERM, "rename onto INDEX");
        rwy_begin(&S);
        EXPECT(rwy_lookup(&S, T "/CONFIG42/INDEX", &o), OK, "lookup in txn");
        EXPECT(rwy_write(&S, o, 0, "9", 1), PERM, "write by oid");
        EXPECT(rwy_truncate_oid(&S, o, 0), PERM, "truncate by oid");
        EXPECT(rwy_setxattr_oid(&S, o, "user.x", "v", 1, 0), PERM, "setxattr by oid");
        EXPECT(rwy_removexattr_oid(&S, o, "user.x"), PERM, "removexattr by oid");
        rwy_abort(&S);
        same_text(T "/CONFIG42/INDEX", "42\n", "INDEX unchanged");
    }
    as_user();

    /* the slot itself keeps working */
    EXPECT(wr(T "/CONFIG42/STATE", 0, "FAILED\n"), OK, "STATE still writable");
    EXPECT(rm(T "/CONFIG42/KERNEL"), OK, "remove real entries");
    EXPECT(rm(T "/CONFIG42/STATE"), OK, "unlink of the virtual STATE is a no-op");
    EXPECT(rm(T "/CONFIG42/TRIES"), OK, "unlink of the virtual TRIES is a no-op");
    EXPECT(rm(T "/CONFIG42/REVISION"), OK, "unlink of REVISION is a no-op");
    same_text(T "/CONFIG42/STATE", "FAILED\n", "STATE is unchanged");
    CHECK(rev(42) > 0, "REVISION is still there");
    EXPECT(wr(T "/CONFIG42/STATE", 0, "INSTALLED\n"), PERM, "FAILED stays final after rm of REVISION");
    EXPECT(rd(T "/CONFIG42"), OK, "rmdir removes the slot with its real entries");
    EXPECT(rwy_lstat(&S, T "/CONFIG42", &a), RWY_ERR_NOENT, "slot gone");
    slot(43);
    EXPECT(install(43), OK, "finalize 43");
    EXPECT(rd(T "/CONFIG43"), OK, "rmdir of a slot with KERNEL and REVISION");
    EXPECT(rwy_lstat(&S, T "/CONFIG43", &a), RWY_ERR_NOENT, "slot with a payload gone");
    set_booted(0);

    /* the same name outside the subtree is an ordinary one */
    EXPECT(mk("CONFIG1"), OK, "outside dir");
    EXPECT(cr("CONFIG1/INDEX"), OK, "INDEX is an ordinary name outside EFI/Runway");
    EXPECT(wr("CONFIG1/INDEX", 0, "x"), OK, "...and writable");
}

void test_slot_limit(void) {
    rwy_boot_choice_t c;
    printf("== slot limit ==\n");
    fresh_subtree();
    EXPECT(cr(T "/CONFIG1x"), INV, "not a slot name");
    for (int n = 1; n <= 64; n++) fin(n, "INSTALLED");
    set_state(1, "OK\n");
    for (int n = 2; n <= 64; n++) set_state(n, "FAILED\n");
    EXPECT(mk(T "/CONFIG65"), RWY_ERR_NOSPC, "65th slot refused");
    as_sys();
    EXPECT(mk(T "/CONFIG65"), RWY_ERR_NOSPC, "65th slot refused for SYSTEM too");
    as_user();
    EXPECT(mk(T "/RECOVERY"), PERM, "RECOVERY is not a slot (sealed, but not counted)");
    drop(64);
    EXPECT(mk(T "/CONFIG65"), OK, "a slot freed makes room");
    EXPECT(mk(T "/CONFIG66"), RWY_ERR_NOSPC, "...once");
    EXPECT(rd(T "/CONFIG65"), OK, "incomplete slots count and can be removed");
    fin(64, "INSTALLED");
    set_state(64, "FAILED\n");
    EXPECT(sel(&c), OK, "select at the limit");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG1") && !has("slots, only"), "64 slots: all considered, no message");

    /* A store that already has more (an old image, a bypass): the 64 highest revisions are considered. */
    for (int n = 65; n <= 66; n++) {
        char d[48];
        snprintf(d, sizeof(d), T "/CONFIG%d", n);
        priv(1); EXPECT(mk(d), OK, "fixture slot over the limit"); priv(0);
        set_file(n, "KERNEL", "k");
        snprintf(d, sizeof(d), T "/CONFIG%d/STATE", n);
        EXPECT(cr(d), OK, "STATE");
        EXPECT(install(n), OK, "slots over the limit still finalize");
        set_state(n, "FAILED\n");
    }
    EXPECT(sel(&c), OK, "select over the limit");
    CHECK(c.engaged && !c.booted && has("RUNWAY: 66 slots, only the 64 highest REVISION considered"),
          "CONFIG1 (revision 1) is outside the window: nothing bootable, warned");
    set_state(63, "OK\n");
    EXPECT(sel(&c), OK, "select over the limit, a slot inside the window");
    CHECK(c.booted && !strcmp(c.slot, "CONFIG63") && has("66 slots, only the 64 highest"), "slot inside the window boots");
}

void test_entry_helpers(void) {
    char buf[64];
    size_t stem_len;
    int is_efi;

    printf("== entry helpers ==\n");
    CHECK(rwy_entry_name_ok("foo.conf", strlen("foo.conf")), "valid .conf");
    CHECK(rwy_entry_name_ok("foo.efi", strlen("foo.efi")), "valid .efi");
    CHECK(!rwy_entry_name_ok(".conf", strlen(".conf")), "empty stem");
    CHECK(!rwy_entry_name_ok(".efi", strlen(".efi")), "empty stem");
    CHECK(!rwy_entry_name_ok("foo.bin", strlen("foo.bin")), "invalid ext");
    CHECK(!rwy_entry_name_ok("foo+1.conf", strlen("foo+1.conf")), "ends in counter");
    CHECK(!rwy_entry_name_ok("foo+1-2.efi", strlen("foo+1-2.efi")), "ends in counter range");
    CHECK(rwy_entry_name_ok("tok-6.12.107+deb13-amd64.efi", strlen("tok-6.12.107+deb13-amd64.efi")), "valid Debian kernel version with +");
    CHECK(rwy_entry_name_ok("tok-6.12.107+deb13.conf", strlen("tok-6.12.107+deb13.conf")), "valid Debian version without trailing counter");

    CHECK(rwy_entry_split("test-entry.conf", strlen("test-entry.conf"), &stem_len, &is_efi) && stem_len == 10 && !is_efi, "split .conf");
    CHECK(rwy_entry_split("test-entry.efi", strlen("test-entry.efi"), &stem_len, &is_efi) && stem_len == 10 && is_efi, "split .efi");

    CHECK(rwy_entry_default(buf, sizeof(buf), 1, 0) == 13 && !strcmp(buf, "runway-1.conf"), "default Type 1");
    CHECK(rwy_entry_default(buf, sizeof(buf), 2, 1) == 12 && !strcmp(buf, "runway-2.efi"), "default Type 2");
    CHECK(rwy_entry_default(buf, 7, 1, 0) == -1, "default buffer overflow");
}

void test_meta_helpers(void) {
    const char *val;
    size_t vlen;
    const char *text;
    size_t tlen;

    printf("== meta helpers ==\n");
    CHECK(rwy_meta_ok("version 6.12.107\n", strlen("version 6.12.107\n")), "valid version");
    CHECK(rwy_meta_ok("machine-id 0123456789abcdef\n", strlen("machine-id 0123456789abcdef\n")), "valid machine-id");
    CHECK(rwy_meta_ok("sort-key debian\n", strlen("sort-key debian\n")), "valid sort-key");
    CHECK(rwy_meta_ok("architecture x86-64\n", strlen("architecture x86-64\n")), "valid architecture");
    CHECK(rwy_meta_ok("# comment\n\nversion 1.0\n  sort-key test  \n", strlen("# comment\n\nversion 1.0\n  sort-key test  \n")), "comments and blank lines");
    CHECK(!rwy_meta_ok("", 0), "empty text");
    CHECK(!rwy_meta_ok("# only comment\n", strlen("# only comment\n")), "only comment");
    CHECK(!rwy_meta_ok("title test\n", strlen("title test\n")), "unknown key title");
    CHECK(!rwy_meta_ok("options quiet\n", strlen("options quiet\n")), "unknown key options");
    CHECK(!rwy_meta_ok("version 1\nversion 2\n", strlen("version 1\nversion 2\n")), "duplicate version");
    CHECK(!rwy_meta_ok("version\n", strlen("version\n")), "missing value");
    CHECK(!rwy_meta_ok("version \t \n", strlen("version \t \n")), "whitespace-only value");
    CHECK(!rwy_meta_ok("version 1\x01test\n", strlen("version 1\x01test\n")), "control char in value");

    text = "# BLS Type 1 metadata\nversion 6.12.107+deb13\nsort-key 10-debian\narchitecture x86-64\n";
    tlen = strlen(text);
    CHECK(rwy_meta_find(text, tlen, "version", &val, &vlen) && vlen == strlen("6.12.107+deb13") && !strncmp(val, "6.12.107+deb13", vlen), "find version");
    CHECK(rwy_meta_find(text, tlen, "sort-key", &val, &vlen) && vlen == strlen("10-debian") && !strncmp(val, "10-debian", vlen), "find sort-key");
    CHECK(rwy_meta_find(text, tlen, "architecture", &val, &vlen) && vlen == strlen("x86-64") && !strncmp(val, "x86-64", vlen), "find architecture");
    CHECK(!rwy_meta_find(text, tlen, "machine-id", &val, &vlen), "absent machine-id");
    CHECK(!rwy_meta_find(text, tlen, "title", &val, &vlen), "unknown key title");
}

/* One record of the journal hash: name, size, then the content hash of a blob or the data of any other file. */
static void payload_record(rwy_sha256_ctx_t *c, const char *name, uint64_t size, const void *body, size_t len) {
    rwy_sha256_update(c, name, strlen(name) + 1);
    rwy_sha256_update(c, &size, sizeof(size));
    rwy_sha256_update(c, body, len);
}

static int hexv(char c) {
    return c > '9' ? c - 'a' + 10 : c - '0';
}

static void test_payload_digest_blob(void) {
    static const char name[] = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    static char data[4096];
    uint8_t digest[32], want[32], got[32];
    rwy_sha256_ctx_t c;

    printf("== transplog payload digest of blobs ==\n");
    for (int i = 0; i < 32; i++) digest[i] = (uint8_t)(hexv(name[2 * i]) << 4 | hexv(name[2 * i + 1]));
    memset(data, 'k', sizeof(data) - 1);
    as_sys();
    EXPECT(cr(T "/BLOBS/stage"), OK, "stage");
    EXPECT(wr(T "/BLOBS/stage", 0, data), OK, "stage data");
    EXPECT(mv(T "/BLOBS/stage", T "/BLOBS/00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"), OK, "content name");
    as_user();
    EXPECT(mk(T "/CONFIG51"), OK, "slot 51");
    EXPECT(ln(T "/BLOBS/00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", T "/CONFIG51/KERNEL"), OK, "link blob");
    EXPECT(cr(T "/CONFIG51/LABEL"), OK, "LABEL");
    EXPECT(wr(T "/CONFIG51/LABEL", 0, "Blob"), OK, "write LABEL");

    /* The blob name is not the hash of its data: the digest must come from the name. */
    rwy_sha256_init(&c);
    payload_record(&c, "KERNEL", sizeof(data) - 1, digest, 32);
    payload_record(&c, "LABEL", 4, "Blob", 4);
    rwy_sha256_final(&c, want);
    CHECK(rwy_transplog_calc_payload_hash(&S, "CONFIG51", got) == RWY_OK && !memcmp(got, want, 32), "blob payload hashes its name");

    /* A file with another link but no content name still reads its data. */
    EXPECT(rm(T "/CONFIG51/KERNEL"), OK, "unlink blob");
    EXPECT(cr(T "/BLOBS/plain"), OK, "plain");
    EXPECT(wr(T "/BLOBS/plain", 0, "plain-bytes"), OK, "write plain");
    EXPECT(ln(T "/BLOBS/plain", T "/CONFIG51/KERNEL"), OK, "link plain");
    rwy_sha256_init(&c);
    payload_record(&c, "KERNEL", 11, "plain-bytes", 11);
    payload_record(&c, "LABEL", 4, "Blob", 4);
    rwy_sha256_final(&c, want);
    CHECK(rwy_transplog_calc_payload_hash(&S, "CONFIG51", got) == RWY_OK && !memcmp(got, want, 32), "other file hashes its data");

    EXPECT(rd(T "/CONFIG51"), OK, "remove slot 51");
    EXPECT(rm(T "/BLOBS/plain"), OK, "remove plain");
}

void test_transplog_payload_digest(void) {
    uint8_t golden[32] = { 0 };
    uint8_t h_entry[32] = { 0 };
    uint8_t h_revert[32] = { 0 };
    uint8_t h_meta[32] = { 0 };
    uint8_t h_both[32] = { 0 };
    uint8_t zero[32] = { 0 };

    printf("== transplog payload digest ==\n");
    EXPECT(mk(T "/CONFIG50"), OK, "slot 50");
    EXPECT(cr(T "/CONFIG50/KERNEL"), OK, "KERNEL");
    EXPECT(wr(T "/CONFIG50/KERNEL", 0, "kernel-bytes"), OK, "write KERNEL");
    EXPECT(cr(T "/CONFIG50/ARGS"), OK, "ARGS");
    EXPECT(wr(T "/CONFIG50/ARGS", 0, "console=ttyS0"), OK, "write ARGS");
    EXPECT(cr(T "/CONFIG50/LABEL"), OK, "LABEL");
    EXPECT(wr(T "/CONFIG50/LABEL", 0, "Linux 6.1"), OK, "write LABEL");

    CHECK(rwy_transplog_calc_payload_hash(&S, "CONFIG50", golden) == RWY_OK, "calc baseline hash");
    CHECK(memcmp(golden, zero, 32) != 0, "baseline hash is non-zero");

    /* Add ENTRY */
    EXPECT(cr(T "/CONFIG50/ENTRY"), OK, "create ENTRY");
    EXPECT(wr(T "/CONFIG50/ENTRY", 0, "test-entry.conf"), OK, "write ENTRY");
    CHECK(rwy_transplog_calc_payload_hash(&S, "CONFIG50", h_entry) == RWY_OK, "calc hash with ENTRY");
    CHECK(memcmp(h_entry, golden, 32) != 0, "digest changes with ENTRY");

    /* Remove ENTRY: reverts to golden */
    EXPECT(rm(T "/CONFIG50/ENTRY"), OK, "remove ENTRY");
    CHECK(rwy_transplog_calc_payload_hash(&S, "CONFIG50", h_revert) == RWY_OK, "calc hash after removing ENTRY");
    CHECK(memcmp(h_revert, golden, 32) == 0, "digest without ENTRY matches golden baseline");

    /* Add META */
    EXPECT(cr(T "/CONFIG50/META"), OK, "create META");
    EXPECT(wr(T "/CONFIG50/META", 0, "version 6.1.0\n"), OK, "write META");
    CHECK(rwy_transplog_calc_payload_hash(&S, "CONFIG50", h_meta) == RWY_OK, "calc hash with META");
    CHECK(memcmp(h_meta, golden, 32) != 0, "digest changes with META");
    CHECK(memcmp(h_meta, h_entry, 32) != 0, "digest with META differs from ENTRY");

    /* Add both ENTRY and META */
    EXPECT(cr(T "/CONFIG50/ENTRY"), OK, "re-create ENTRY");
    EXPECT(wr(T "/CONFIG50/ENTRY", 0, "test-entry.conf"), OK, "write ENTRY again");
    CHECK(rwy_transplog_calc_payload_hash(&S, "CONFIG50", h_both) == RWY_OK, "calc hash with both");
    CHECK(memcmp(h_both, golden, 32) != 0, "digest with both differs from golden");
    CHECK(memcmp(h_both, h_entry, 32) != 0, "digest with both differs from ENTRY only");
    CHECK(memcmp(h_both, h_meta, 32) != 0, "digest with both differs from META only");

    /* Clean up */
    EXPECT(rm(T "/CONFIG50/ENTRY"), OK, "cleanup ENTRY");
    EXPECT(rm(T "/CONFIG50/META"), OK, "cleanup META");
    EXPECT(rm(T "/CONFIG50/LABEL"), OK, "cleanup LABEL");
    EXPECT(rm(T "/CONFIG50/ARGS"), OK, "cleanup ARGS");
    EXPECT(rm(T "/CONFIG50/KERNEL"), OK, "cleanup KERNEL");
    EXPECT(rd(T "/CONFIG50"), OK, "cleanup slot 50");
    test_payload_digest_blob();
}
