/* rwyctl: commands that put kernels and other payloads into slots: install, update, recovery set, wdt set. */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rwyctl.h"
#include "rwy_entry.h"
#include "rwy_meta.h"

#define COPY_CHUNK (1u << 20)

/* Copy a file into the store in chunks, so a large payload is never held in memory. */
static int copy_file(const char *dst, const char *src) {
    static char chunk[COPY_CHUNK];
    uint64_t pos = 0;
    struct stat st;
    ssize_t n;
    int fd = open(src, O_RDONLY), rc = fd < 0 ? RWY_ERR_NOENT : rwy_create(&g_store, dst, RWY_S_IFREG | 0644, 0, 0);
    if (fd < 0) { warn("cannot read %s", src); return rc; }
    while (!rc && (n = read(fd, chunk, sizeof(chunk))) > 0) {
        rc = rwy_write_path(&g_store, dst, pos, chunk, (size_t)n);
        pos += (uint64_t)n;
    }
    if (!rc && n < 0) { warn("cannot read %s", src); rc = RWY_ERR_IO; }
    if (!rc) rc = fstat(fd, &st) ? RWY_ERR_IO : stamp_times(dst, &st);
    close(fd);
    return rc;
}

typedef struct { const char *file, *src; char name[RWY_BLOB_NAME_SIZE]; uint64_t size; int counted_stored; } payload_t;

static int is_blob(const char *src) {
    return !strncmp(src, "blob:", 5);
}

/* The content name of a payload: what blob: names, or the hash of the file (pass one). Done once. */
static int payload_name(payload_t *p) {
    if (p->name[0]) return 0;
    if (!is_blob(p->src)) return blob_hash_file(p->src, p->name, &p->size);
    snprintf(p->name, sizeof(p->name), "%s", p->src + 5);
    return 0;
}

/* A payload goes into the slot as an own copy or as a link to its blob (`own_copy`). */
static int put_payload(const char *slot, payload_t *p, int own_copy) {
    char dst[PATH_MAX];
    int rc;
    snprintf(dst, sizeof(dst), "%s/%s", slot, p->file);
    if (own_copy) {
        if (!is_blob(p->src)) return copy_file(dst, p->src);
        return payload_name(p) ? RWY_ERR_NOENT : blob_copy(p->name, dst);
    }
    if ((rc = payload_name(p))) return rc;
    return blob_link(p->name, is_blob(p->src) ? NULL : p->src, dst);
}

/* blob: takes a content name only. */
static int blob_name_ok(const char *src) {
    return !is_blob(src) || rwy_bootcfg_content_name(src + 5, strlen(src + 5));
}

/* The next free CONFIG<n>: one more than the highest. */
static int next_slot_name(char *out) {
    unsigned long long max = 0;
    int i, n = list_slots();
    for (i = 0; i < n; i++) {
        unsigned long long v = strtoull(g_names[i] + 6, 0, 10);
        if (v > max) max = v;
    }
    if (max == ULLONG_MAX) return -1;
    snprintf(out, 32, "CONFIG%llu", n ? max + 1 : 1);
    return 0;
}

typedef struct {
    const char *name, *kernel, *initrd, *dtb, *args, *label, *entry, *meta, *tries, *watchdog;
    int dedup;
    payload_t pay[3];   /* KERNEL, INITRD, DTB; src is NULL when absent */
} install_t;

/* The ARGS file: the text, or the content of the file named after '@'; a trailing newline is dropped. */
static int put_args(const char *slot, const char *arg) {
    char p[PATH_MAX], args[4200];
    int n = arg[0] == '@' ? slurp(arg + 1, args, sizeof(args) - 1) : (int)snprintf(args, sizeof(args), "%s", arg), rc;
    if (n < 0 || n >= (int)sizeof(args) - 1) { warn("%s", "cannot read ARGS (or it is too long)"); return RWY_ERR_INVAL; }
    while (n && (args[n - 1] == '\n' || args[n - 1] == '\r')) n--;
    snprintf(p, sizeof(p), "%s/ARGS", slot);
    rc = rwy_create(&g_store, p, RWY_S_IFREG | 0644, 0, 0);
    if (!rc && n) rc = rwy_write_path(&g_store, p, 0, args, (size_t)n);
    return rc;
}

/* The LABEL file: the text, or the content of the file named after '@'; a trailing newline is dropped. */
static int put_label(const char *slot, const char *arg) {
    char p[PATH_MAX], label[256];
    int n = arg[0] == '@' ? slurp(arg + 1, label, sizeof(label) - 1) : (int)snprintf(label, sizeof(label), "%s", arg), rc;
    if (n < 0 || n >= (int)sizeof(label) - 1) { warn("%s", "cannot read LABEL (or it is too long)"); return RWY_ERR_INVAL; }
    while (n && (label[n - 1] == '\n' || label[n - 1] == '\r')) n--;
    if (n > (int)RWY_LABEL_MAX) { warn("%s", "cannot read LABEL (or it is too long)"); return RWY_ERR_INVAL; }
    snprintf(p, sizeof(p), "%s/LABEL", slot);
    rc = rwy_create(&g_store, p, RWY_S_IFREG | 0644, 0, 0);
    if (!rc && n) rc = rwy_write_path(&g_store, p, 0, label, (size_t)n);
    return rc;
}

/* Derives default entry name for a slot. */
static int default_entry(char *entry, size_t cap, const char *slot, int only_kernel) {
    const char *base = strrchr(slot, '/');
    unsigned long long num;
    base = base ? base + 1 : slot;
    num = strtoull(base + 6, NULL, 10);
    return rwy_entry_default(entry, cap, num, only_kernel);
}

/* The ENTRY file: the text, or the content of the file named after '@'. */
static int put_entry(const char *slot, const char *arg, int only_kernel) {
    char p[PATH_MAX], entry[RWY_ENTRY_MAX + 1];
    int n, rc;
    if (arg) {
        n = arg[0] == '@' ? slurp(arg + 1, entry, sizeof(entry) - 1) : (int)snprintf(entry, sizeof(entry), "%s", arg);
        if (n < 0 || n >= (int)sizeof(entry)) { warn("%s", "cannot read ENTRY (or it is too long)"); return RWY_ERR_INVAL; }
        while (n && (entry[n - 1] == '\n' || entry[n - 1] == '\r')) n--;
        entry[n] = 0;
        if (!rwy_entry_name_ok(entry, (size_t)n)) { warn("%s", "invalid ENTRY name"); return RWY_ERR_INVAL; }
        if (n >= 4 && !strcmp(entry + n - 4, ".efi") && !only_kernel) {
            warn("%s", "Type 2 entry (.efi) does not allow INITRD, DTB, ARGS, LABEL or META");
            return RWY_ERR_INVAL;
        }
    } else {
        n = default_entry(entry, sizeof(entry), slot, only_kernel);
        if (n < 0) return RWY_ERR_INVAL;
    }
    snprintf(p, sizeof(p), "%s/ENTRY", slot);
    rc = rwy_create(&g_store, p, RWY_S_IFREG | 0644, 0, 0);
    if (!rc) rc = rwy_write_path(&g_store, p, 0, entry, (size_t)n);
    return rc;
}

/* The META file: the text, or the content of the file named after '@'. */
static int put_meta(const char *slot, const char *arg) {
    char p[PATH_MAX], meta[RWY_META_MAX + 1];
    int n = arg[0] == '@' ? slurp(arg + 1, meta, sizeof(meta) - 1) : (int)snprintf(meta, sizeof(meta), "%s", arg), rc;
    if (n < 0 || n >= (int)sizeof(meta)) { warn("%s", "cannot read META (or it is too long)"); return RWY_ERR_INVAL; }
    while (n && (meta[n - 1] == '\n' || meta[n - 1] == '\r')) n--;
    if (n < (int)sizeof(meta) - 1) meta[n++] = '\n';
    meta[n] = 0;
    if (!rwy_meta_ok(meta, (size_t)n)) { warn("%s", "invalid META content"); return RWY_ERR_INVAL; }
    snprintf(p, sizeof(p), "%s/META", slot);
    rc = rwy_create(&g_store, p, RWY_S_IFREG | 0644, 0, 0);
    if (!rc && n) rc = rwy_write_path(&g_store, p, 0, meta, (size_t)n);
    return rc;
}

/* Create the slot directory with its payloads and ARGS. */
static int make_slot(install_t *in, const char *slot, int own_copy) {
    int i, rc = rwy_mkdir(&g_store, slot, RWY_S_IFDIR | 0755, 0, 0);
    for (i = 0; !rc && i < 3; i++)
        if (in->pay[i].src) rc = put_payload(slot, &in->pay[i], own_copy);
    if (!rc && in->args) rc = put_args(slot, in->args);
    if (!rc && in->label) rc = put_label(slot, in->label);
    return rc;
}

static int install_slot(install_t *in, const char *slot) {
    char p[PATH_MAX];
    int only_kernel = !in->initrd && !in->dtb && !in->args && !in->label && !in->meta;
    int rc = make_slot(in, slot, !in->dedup);
    if (!rc) rc = put_entry(slot, in->entry, only_kernel);
    if (!rc && in->meta) rc = put_meta(slot, in->meta);
    if (!rc && in->tries) { snprintf(p, sizeof(p), "%s/TRIES", slot); rc = put_scalar(p, in->tries); }
    if (!rc && in->watchdog) { snprintf(p, sizeof(p), "%s/WATCHDOG", slot); rc = put_scalar(p, in->watchdog); }
    if (!rc) { snprintf(p, sizeof(p), "%s/STATE", slot); rc = put_scalar(p, "INSTALLED"); }
    return rc;
}

/* Where the value of an install option goes; `recovery` refuses the slot-only options. NULL if unknown. */
static const char **install_option(install_t *in, const char *opt, int recovery) {
    if (!strcmp(opt, "--kernel")) return &in->kernel;
    if (!strcmp(opt, "--initrd")) return &in->initrd;
    if (!strcmp(opt, "--dtb")) return &in->dtb;
    if (!strcmp(opt, "--args")) return &in->args;
    if (!strcmp(opt, "--label")) return &in->label;
    if (recovery) return NULL;
    if (!strcmp(opt, "--name")) return &in->name;
    if (!strcmp(opt, "--tries")) return &in->tries;
    if (!strcmp(opt, "--watchdog")) return &in->watchdog;
    if (!strcmp(opt, "--entry")) return &in->entry;
    if (!strcmp(opt, "--meta")) return &in->meta;
    return NULL;
}

static int check_payload_names(const install_t *in) {
    return blob_name_ok(in->kernel) && (!in->initrd || blob_name_ok(in->initrd)) &&
           (!in->dtb || blob_name_ok(in->dtb));
}

/* Options of install and recovery set. `recovery` allows --unlock and refuses the slot-only ones. */
static int parse_install(install_t *in, int argc, char **argv, int recovery, int *unlock) {
    int i;
    for (i = 0; i < argc; i++) {
        const char **dst;
        if (recovery && !strcmp(argv[i], "--unlock")) { *unlock = 1; continue; }
        if (!recovery && !strcmp(argv[i], "--dedup")) { in->dedup = 1; continue; }
        dst = install_option(in, argv[i], recovery);
        if (!dst || i + 1 == argc) { warn("bad or incomplete option %s", argv[i]); return X_USAGE; }
        *dst = argv[++i];
    }
    if (!in->kernel) { warn("%s", "--kernel is required"); return X_USAGE; }
    if (in->name && !slot_name_ok(in->name)) { warn("bad slot name %s", in->name); return X_USAGE; }
    in->pay[0] = (payload_t){ .file = "KERNEL", .src = in->kernel };
    in->pay[1] = (payload_t){ .file = "INITRD", .src = in->initrd };
    in->pay[2] = (payload_t){ .file = "DTB", .src = in->dtb };
    if (!check_payload_names(in)) {
        warn("%s", "bad blob name");
        return X_USAGE;
    }
    return 0;
}

/* The number of slots the owner asked for (SLOTS); 0 when the file is absent or unreadable. */
static int slots_wanted(void) {
    char v[64];
    return read_scalar(EFI_ROOT "/SLOTS", v, sizeof(v)) ? 0 : atoi(v);
}

typedef struct { uint32_t total, largest; } need_t;

/* Find the size of a payload when copying. */
static int source_size(payload_t *p, uint64_t *out) {
    if (is_blob(p->src)) {
        char path[PATH_MAX];
        rwy_attr_t a;
        snprintf(path, sizeof(path), EFI_ROOT "/BLOBS/%s", p->src + 5);
        if (rwy_stat(&g_store, path, &a) != RWY_OK) return blob_missing(p->src + 5);
        *out = a.size;
        return 0;
    }
    struct stat st;
    if (stat(p->src, &st) || !S_ISREG(st.st_mode)) { warn("cannot read %s", p->src); return RWY_ERR_NOENT; }
    *out = (uint64_t)st.st_size;
    return 0;
}

/* Add the blocks one payload costs: none for a blob that is stored, its size for a file that is not. */
static int add_source(need_t *need, payload_t *p, int dedup) {
    uint32_t blocks;
    int rc;
    if (!p->src) return 0;
    if (dedup) {
        if ((rc = payload_name(p))) return rc;
        p->counted_stored = blob_available(p->name);
        if (p->counted_stored) return 0;
        if (is_blob(p->src)) return blob_missing(p->name);
    } else {
        p->counted_stored = 0;
        if ((rc = source_size(p, &p->size))) return rc;
    }
    blocks = (uint32_t)((p->size + RWY_BLOCK_SIZE - 1) / RWY_BLOCK_SIZE);
    need->total += blocks;
    if (blocks > need->largest) need->largest = blocks;
    return 0;
}

/* What the new slot will allocate; checks that every source can be read. */
static int payload_need(install_t *in, need_t *need) {
    int i, rc;
    need->total = RWY_SLOT_META_BLOCKS;
    need->largest = 0;
    for (i = 0; i < 3; i++)
        if ((rc = add_source(need, &in->pay[i], in->dedup))) return rc;
    return 0;
}

/* Did the eviction remove a blob that the plan counted as stored? */
static int blob_lost(const install_t *in) {
    int i;
    for (i = 0; i < 3; i++)
        if (in->pay[i].src && in->pay[i].counted_stored && !blob_available(in->pay[i].name)) return 1;
    return 0;
}

/* With QUOTA set: evict what the plan names so the new slot fits. Enters and leaves with a transaction open.
 * The removals commit on their own and the new slot is staged in the next transaction. When they remove a blob
 * that the new slot would have linked, the plan is made again with that blob counted as new. */
static int make_room(install_t *in) {
    need_t need;
    int rc;
    do {
        if ((rc = payload_need(in, &need))) return finish(rc);
        if ((rc = rwy_bootcfg_make_room(&g_store, need.total, need.largest))) {
            warn("refused: %s (the protected slots are intact)",
                 rc == RWY_ERR_NOSPC ? "no more slot may be removed to meet QUOTA or SLOTS" : rwy_strerror(rc));
            return exit_of(rc);
        }
    } while (blob_lost(in));
    return 0;
}

int cmd_install(int argc, char **argv) {
    install_t in = { 0 };
    char name[32], slot[PATH_MAX], p[PATH_MAX], rev[64];
    int rc, unlock = 0;
    if ((rc = parse_install(&in, argc, argv, 0, &unlock))) return rc;
    if ((rc = begin_raise())) return rc;
    if ((rc = make_room(&in))) return rc;
    if (!in.name && next_slot_name(name)) { rwy_abort(&g_store); warn("%s", "no slot number left"); return X_POLICY; }
    snprintf(slot, sizeof(slot), EFI_ROOT "/%s", in.name ? in.name : name);
    rc = install_slot(&in, slot);
    snprintf(p, sizeof(p), "%s/REVISION", slot);
    if (!rc && read_scalar(p, rev, sizeof(rev))) strcpy(rev, "-");
    if ((rc = finish(rc))) return rc;
    printf("%s %s\n", in.name ? in.name : name, rev);
    return 0;
}

/* Delete a slot and what it holds in the same transaction. */
int remove_slot(const char *dir) {
    return rwy_bootcfg_remove_slot(&g_store, dir);
}

/* update: which slot receives the new payload, as the name; `recycle` when an existing slot is removed in the same
 * commit. With QUOTA the plan decides what goes and the new slot always gets a fresh name. Returns an X_ code. */
static int update_target(install_t *in, char *name, int *recycle) {
    unsigned quota;
    int want = slots_wanted(), rc;
    *recycle = 0;
    if (rwy_bootcfg_quota(&g_store, &quota) == RWY_OK) want = -1;
    if (!want) {
        rwy_abort(&g_store);
        warn("%s", "neither SLOTS nor QUOTA is set, so there is nothing to rotate over: set one with `set`, or use `install`");
        return X_POLICY;
    }
    if ((want < 0 || list_slots() > want) && (rc = make_room(in))) return rc;
    if (want < 0 || list_slots() < want) return next_slot_name(name) ? finish(RWY_ERR_RANGE) : 0;
    *recycle = 1;
    if (rwy_bootcfg_recycle_pick(&g_store, name) != RWY_ERR_NOENT) return 0;
    rwy_abort(&g_store);
    warn("%s", "no slot can be recycled (each is booted, the newest OK one, or still being tested)");
    return X_POLICY;
}

/* update: install into a new slot while fewer than SLOTS exist, else in place of the slot the core picks, in one commit.
 * With QUOTA, the slots the plan names go in their own commit first. */
int cmd_update(int argc, char **argv) {
    install_t in = { 0 };
    char name[32], slot[PATH_MAX], p[PATH_MAX], rev[64];
    int rc, unlock = 0, recycle;
    if ((rc = parse_install(&in, argc, argv, 0, &unlock))) return rc;
    if (in.name) { warn("%s", "update picks the slot itself: --name is not accepted"); return X_USAGE; }
    if ((rc = begin_raise()) || (rc = update_target(&in, name, &recycle))) return rc;
    snprintf(slot, sizeof(slot), EFI_ROOT "/%s", name);
    if (recycle) rc = remove_slot(slot);
    if (!rc) rc = install_slot(&in, slot);
    snprintf(p, sizeof(p), "%s/REVISION", slot);
    if (!rc && read_scalar(p, rev, sizeof(rev))) strcpy(rev, "-");
    if ((rc = finish(rc))) return rc;
    printf("%s %s\n", name, rev);
    return 0;
}

/* recovery set: replace RECOVERY/ in one transaction, as SYSTEM with RECOVERY/ unlocked. */
int cmd_recovery(int argc, char **argv) {
    install_t in = { 0 };
    rwy_attr_t a;
    int rc, unlock = 0;
    if (argc < 1 || strcmp(argv[0], "set")) { warn("%s", "usage: recovery set --unlock --kernel SRC [--initrd SRC] [--dtb SRC] [--args TEXT|@FILE] [--label TEXT|@FILE]"); return X_USAGE; }
    if ((rc = parse_install(&in, argc - 1, argv + 1, 1, &unlock))) return rc;
    if (!unlock) { warn("%s", "recovery set needs --unlock"); return X_USAGE; }
    if ((rc = system_allowed()) || (rc = begin_write())) return rc;
    rwy_set_actor(&g_store, RWY_ACTOR_SYSTEM);
    rc = rwy_bootcfg_unlock_recovery(&g_store, 1);
    if (!rc && rwy_stat(&g_store, EFI_ROOT "/RECOVERY", &a) == RWY_OK) rc = remove_slot(EFI_ROOT "/RECOVERY");
    if (!rc) rc = make_slot(&in, EFI_ROOT "/RECOVERY", 1);
    rc = finish(rc);
    rwy_bootcfg_unlock_recovery(&g_store, 0);
    return rc;
}

/* One WDT/ application: a file is stored as a blob and linked under its base name. */
static int put_wdt_app(const char *src) {
    const char *base = strrchr(src, '/');
    payload_t p = { .src = src };
    if (is_blob(src)) { warn("%s", "wdt set takes files: a blob has no application name"); return RWY_ERR_INVAL; }
    p.file = base ? base + 1 : src;
    return put_payload(EFI_ROOT "/WDT", &p, 0);
}

/* wdt set: replace WDT/ in one transaction, as SYSTEM with the sealed directories unlocked. */
int cmd_wdt(int argc, char **argv) {
    rwy_attr_t a;
    int i, rc, unlock = 0, napps = 0;
    if (argc < 1 || strcmp(argv[0], "set")) { warn("%s", "usage: wdt set --unlock SRC..."); return X_USAGE; }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--unlock")) unlock = 1;
        else napps++;
    }
    if (!unlock || !napps) { warn("%s", "usage: wdt set --unlock FILE..."); return X_USAGE; }
    if ((rc = system_allowed()) || (rc = begin_write())) return rc;
    rwy_set_actor(&g_store, RWY_ACTOR_SYSTEM);
    rc = rwy_bootcfg_unlock_recovery(&g_store, 1);
    if (!rc && rwy_stat(&g_store, EFI_ROOT "/WDT", &a) == RWY_OK) rc = remove_slot(EFI_ROOT "/WDT");
    if (!rc) rc = rwy_mkdir(&g_store, EFI_ROOT "/WDT", RWY_S_IFDIR | 0755, 0, 0);
    for (i = 1; !rc && i < argc; i++)
        if (strcmp(argv[i], "--unlock")) rc = put_wdt_app(argv[i]);
    rc = finish(rc);
    rwy_bootcfg_unlock_recovery(&g_store, 0);
    return rc;
}
