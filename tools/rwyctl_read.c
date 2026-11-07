/* rwyctl: reading EFI/RUNWAY: slot names and scalars, and the commands `stores`, `status`, `get` and `slots`. */
/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rwyctl.h"
#include "rwy_meta.h"
#include "anchor_linux.h"

int read_scalar(const char *path, char *out, size_t max) {
    size_t n = max - 1;
    int rc = rwy_read_path(&g_store, path, 0, out, &n);
    if (rc) return rc;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) n--;
    out[n] = 0;
    return 0;
}

int slot_name_ok(const char *n) {
    int i;
    if (strncmp(n, "CONFIG", 6) || !n[6]) return 0;
    for (i = 6; n[i]; i++) if (!isdigit((unsigned char)n[i])) return 0;
    return 1;
}

static int cmp_slot(const void *a, const void *b) {
    const char *x = a, *y = b;
    size_t lx = strlen(x), ly = strlen(y);
    return lx != ly ? (lx < ly ? -1 : 1) : strcmp(x, y);
}

int have_efi_runway(void) {
    rwy_attr_t a;
    return rwy_stat(&g_store, EFI_ROOT, &a) == RWY_OK && RWY_S_ISDIR(a.mode);
}

char g_names[MAXSLOTS][32];
int list_slots(void) {
    static char buf[MAXSLOTS * 64];
    size_t count = MAXSLOTS, off = 0, i;
    int n = 0;
    uint8_t types[MAXSLOTS];
    if (rwy_readdir(&g_store, EFI_ROOT, buf, sizeof(buf), &count, types)) return 0;
    for (i = 0; i < count && n < MAXSLOTS; i++) {
        const char *nm = buf + off;
        off += strlen(nm) + 1;
        if (slot_name_ok(nm) && strlen(nm) < 32) snprintf(g_names[n++], 32, "%s", nm);
    }
    qsort(g_names, (size_t)n, 32, cmp_slot);
    return n;
}

int cmd_stores(void) {
    int g, i;
    char u[37];
    printf("%-36s %7s %10s %9s %s\n", "UUID", "MEMBERS", "GENERATION", "ENCRYPTED", "BOOT");
    for (g = 0; g < g_ngrp; g++) {
        uint64_t best = 0;
        for (i = 0; i < g_grp[g].n; i++) if (g_mem[g_grp[g].m[i]].gen > best) best = g_mem[g_grp[g].m[i]].gen;
        rwy_format_uuid(g_grp[g].uuid, u);
        printf("%-36s %7d %10llu %9s %s\n", u, g_grp[g].n, (unsigned long long)best,
               g_mem[g_grp[g].m[0]].enc ? "yes" : "no", g_grp[g].boot ? "boot" : "-");
        for (i = 0; i < g_grp[g].n; i++)
            printf("  %s %llu\n", g_mem[g_grp[g].m[i]].path, (unsigned long long)g_mem[g_grp[g].m[i]].gen);
    }
    return 0;
}

/* The member paths of a store, one per line, for a mount helper. A RUNWAY.DAT that was only mounted for the look is
 * gone at exit, so it is refused. */
int cmd_members(const group_t *g) {
    int i, k;
    for (i = 0; i < g->n; i++) {
        for (k = 0; k < g_ntmp; k++) {
            if (!strncmp(g_mem[g->m[i]].path, g_tmpmnt[k], strlen(g_tmpmnt[k]))) {
                warn("%s is not mounted, mount its file system first", g_mem[g->m[i]].path);
                return X_NOSTORE;
            }
        }
    }
    for (i = 0; i < g->n; i++) puts(g_mem[g->m[i]].path);
    return 0;
}

const char *slot_path(const char *name, char *out) {
    if (!strcmp(name, "BOOTED")) { snprintf(out, PATH_MAX, EFI_ROOT "/BOOTED"); return out; }
    if (!strcmp(name, "RECOVERY") || slot_name_ok(name)) { snprintf(out, PATH_MAX, EFI_ROOT "/%s", name); return out; }
    return 0;
}

static void print_slot_line(const char *slot, const char *rev, const char *st, const char *tries, const char *wd,
                            const char *cause, const char *label, const char *entry, const char *meta, const char *pin) {
    printf("%-26s %-20s %-10s %-6s %-10s %-15s %-15s %-25s %-10s %s\n", slot, rev, st, tries, wd, cause, label, entry, meta, pin);
}

/* Read EFI/RUNWAY/<slot>/<file>, "-" when it cannot be read. Returns 1 then. */
static int slot_scalar(const char *slot, const char *file, char *out, size_t size) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), EFI_ROOT "/%s/%s", slot, file);
    if (!read_scalar(p, out, size) && out[0]) return 0;
    strcpy(out, "-");
    return 1;
}

/* Extract version or presence from META for status display. */
static void slot_meta_summary(const char *slot, char *out, size_t size) {
    char p[PATH_MAX], buf[RWY_META_MAX + 1];
    const char *val;
    size_t n = sizeof(buf) - 1, vlen;
    snprintf(p, sizeof(p), EFI_ROOT "/%s/META", slot);
    if (rwy_read_path(&g_store, p, 0, buf, &n) || n == 0) {
        snprintf(out, size, "-");
        return;
    }
    buf[n] = 0;
    if (rwy_meta_find(buf, n, "version", &val, &vlen)) {
        if (vlen >= size) vlen = size - 1;
        memcpy(out, val, vlen);
        out[vlen] = 0;
    } else {
        snprintf(out, size, "yes");
    }
}

typedef struct {
    char rev[128], st[128], tries[128], wd[128], cause[128], label[256], entry[256], meta[128], pin[16];
} slot_info_t;

/* Read the display values of a slot. Returns 1 when it has no REVISION (incomplete). */
static int slot_load(const char *slot, slot_info_t *s) {
    int inc = slot_scalar(slot, "REVISION", s->rev, sizeof(s->rev));
    slot_scalar(slot, "STATE", s->st, sizeof(s->st));
    slot_scalar(slot, "TRIES", s->tries, sizeof(s->tries));
    slot_scalar(slot, "WATCHDOG", s->wd, sizeof(s->wd));
    slot_scalar(slot, "CAUSE", s->cause, sizeof(s->cause));
    slot_scalar(slot, "LABEL", s->label, sizeof(s->label));
    slot_scalar(slot, "ENTRY", s->entry, sizeof(s->entry));
    slot_meta_summary(slot, s->meta, sizeof(s->meta));
    slot_scalar(slot, "PIN", s->pin, sizeof(s->pin));
    return inc;
}

/* The name of the slot the BOOTED link points to, or NULL when it is not set. */
static const char *booted_name(char *v, size_t size) {
    char *b;
    if (rwy_readlink(&g_store, EFI_ROOT "/BOOTED", v, size)) return NULL;
    b = strrchr(v, '/');
    return b ? b + 1 : v;
}

/* One line per slot; returns how many have no REVISION (incomplete). */
static int print_slots(void) {
    slot_info_t s;
    int i, n = list_slots(), inc = 0;
    print_slot_line("SLOT", "REVISION", "STATE", "TRIES", "WATCHDOG", "CAUSE", "LABEL", "ENTRY", "META", "PIN");
    for (i = 0; i < n; i++) {
        inc += slot_load(g_names[i], &s);
        print_slot_line(g_names[i], s.rev, s.st, s.tries, s.wd, s.cause, s.label, s.entry, s.meta,
                        !strcmp(s.pin, "1") ? "yes" : "-");
    }
    return inc;
}

static void print_global(const char *label, const char *file) {
    char v[128];
    printf("%s=%s\n", label, read_scalar(file, v, sizeof(v)) ? "-" : v);
}

/* USED=<percent of the usable pool> (<used>/<usable> blocks) */
static void print_used(int json) {
    uint32_t used, usable;
    rwy_pool_usage(&g_store, &used, &usable);
    unsigned pct = usable ? (unsigned)((uint64_t)used * 100 / usable) : 0;
    if (json) printf("  \"used\": {\"percent\": %u, \"blocks\": %u, \"usable\": %u},\n", pct, used, usable);
    else printf("USED=%u%% (%u/%u blocks)\n", pct, used, usable);
}

/* A JSON string, or null for no value and for "-". Control characters are escaped. */
void json_str(const char *s) {
    if (!s || !strcmp(s, "-")) { fputs("null", stdout); return; }
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static void json_kv_str(const char *pad, const char *key, const char *val) {
    printf("%s\"%s\": ", pad, key);
    json_str(val);
    puts(",");
}

/* A JSON number when the value starts with a digit, else null. */
static void json_kv_num(const char *pad, const char *key, const char *val) {
    if (val && isdigit((unsigned char)val[0])) printf("%s\"%s\": %llu,\n", pad, key, strtoull(val, NULL, 10));
    else printf("%s\"%s\": null,\n", pad, key);
}

static void print_global_json(const char *key, const char *file, int num) {
    char v[128];
    const char *val = read_scalar(file, v, sizeof(v)) ? NULL : v;
    if (num) json_kv_num("  ", key, val);
    else json_kv_str("  ", key, val);
}

static void print_slot_json(const char *slot, const slot_info_t *s) {
    const char *pad = "      ";
    printf("    {\n");
    json_kv_str(pad, "slot", slot);
    json_kv_num(pad, "revision", s->rev);
    json_kv_str(pad, "state", s->st);
    json_kv_num(pad, "tries", s->tries);
    json_kv_num(pad, "watchdog", s->wd);
    json_kv_str(pad, "cause", s->cause);
    json_kv_str(pad, "label", s->label);
    json_kv_str(pad, "entry", s->entry);
    json_kv_str(pad, "meta", s->meta);
    printf("%s\"pinned\": %s\n    }", pad, !strcmp(s->pin, "1") ? "true" : "false");
}

static int print_slots_json(void) {
    slot_info_t s;
    int i, n = list_slots(), inc = 0;

    printf("  \"slots\": [");
    for (i = 0; i < n; i++) {
        inc += slot_load(g_names[i], &s);
        printf("%s\n", i ? "," : "");
        print_slot_json(g_names[i], &s);
    }
    printf("%s  ],\n", n ? "\n" : "");
    return inc;
}

static const char *recovery_state(void) {
    rwy_attr_t a;
    if (rwy_stat(&g_store, EFI_ROOT "/RECOVERY", &a)) return "absent";
    return rwy_stat(&g_store, EFI_ROOT "/RECOVERY/KERNEL", &a) ? "invalid" : "valid";
}

static void status_json(void) {
    char v[128];
    rwy_attr_t a;
    int inc;

    printf("{\n");
    inc = print_slots_json();
    json_kv_str("  ", "booted", booted_name(v, sizeof(v)));
    print_global_json("reason", EFI_ROOT "/REASON", 0);
    print_global_json("watchdog", EFI_ROOT "/WATCHDOG", 1);
    print_global_json("slots_limit", EFI_ROOT "/SLOTS", 1);
    print_global_json("boot_delay", EFI_ROOT "/BOOT-DELAY", 1);
    print_global_json("quota", EFI_ROOT "/QUOTA", 1);
    print_global_json("loop_guard", EFI_ROOT "/LOOP-GUARD", 1);
    print_global_json("loop_limit", EFI_ROOT "/LOOP-LIMIT", 1);
    print_global_json("loop_window", EFI_ROOT "/LOOP-WINDOW", 1);
    print_used(1);
    print_last_evict(1);
    printf("  \"recover_next\": %s,\n", rwy_stat(&g_store, EFI_ROOT "/RECOVER_NEXT", &a) == RWY_OK ? "true" : "false");
    print_global_json("counter", EFI_ROOT "/REVISION", 1);
    printf("  \"recovery\": \"%s\",\n", recovery_state());
    printf("  \"record\": \"%s\",\n", rwy_record_damaged(&g_store) ? "damaged" : "ok");
    printf("  \"anchor\": \"%s\",\n", anchor_state(&g_store));
    printf("  \"incomplete\": %d,\n", inc);
    print_mirror_health(1);
    printf("}\n");
}

static void status_text(void) {
    char v[128];
    rwy_attr_t a;
    const char *booted;
    int inc = print_slots();

    booted = booted_name(v, sizeof(v));
    printf("BOOTED=%s\n", booted ? booted : "-");
    print_global("REASON", EFI_ROOT "/REASON");
    print_global("WATCHDOG", EFI_ROOT "/WATCHDOG");
    print_global("SLOTS", EFI_ROOT "/SLOTS");
    print_global("BOOT-DELAY", EFI_ROOT "/BOOT-DELAY");
    print_global("QUOTA", EFI_ROOT "/QUOTA");
    print_global("LOOP-GUARD", EFI_ROOT "/LOOP-GUARD");
    print_global("LOOP-LIMIT", EFI_ROOT "/LOOP-LIMIT");
    print_global("LOOP-WINDOW", EFI_ROOT "/LOOP-WINDOW");
    print_used(0);
    print_last_evict(0);
    printf("RECOVER_NEXT=%s\n", rwy_stat(&g_store, EFI_ROOT "/RECOVER_NEXT", &a) == RWY_OK ? "yes" : "no");
    print_global("COUNTER", EFI_ROOT "/REVISION");
    printf("RECOVERY=%s\n", recovery_state());
    printf("RECORD=%s\n", rwy_record_damaged(&g_store) ? "damaged" : "ok");
    printf("ANCHOR=%s\n", anchor_state(&g_store));
    printf("INCOMPLETE=%d\n", inc);
    print_mirror_health(0);
}

int cmd_status(int argc, char **argv) {
    int json = 0;
    if (argc == 1 && !strcmp(argv[0], "--json")) json = 1;
    else if (argc) { warn("%s", "usage: status [--json]"); return X_USAGE; }
    if (!have_efi_runway()) {
        warn("%s", "EFI/RUNWAY is absent (native selection is off)");
        if (json) puts("{}");
        return 0;
    }
    if (json) status_json();
    else status_text();
    return 0;
}

int in_list(const char *s, const char *const *list) {
    for (; *list; list++)
        if (!strcmp(s, *list)) return 1;
    return 0;
}

static const char *const global_get_fields[] = { "REVISION", "WATCHDOG", "REASON", "SLOTS", "BOOT-DELAY", "QUOTA", "LOOP-GUARD", "LOOP-LIMIT", "LOOP-WINDOW", NULL };
static const char *const slot_get_fields[] = { "STATE", "TRIES", "REVISION", "INDEX", "WATCHDOG", "ARGS", "CAUSE", "PIN", "LABEL", "ENTRY", "META", NULL };

/* Print BOOTED, the name of the slot the link points to. */
static int get_booted(void) {
    char v[4200];
    const char *b = booted_name(v, sizeof(v));
    if (!b) { warn("%s", "BOOTED is not set"); return X_IO; }
    puts(b);
    return 0;
}

int cmd_get(int argc, char **argv) {
    char p[PATH_MAX], v[4200], sp[PATH_MAX];
    const char *name = 0, *field;
    int rc;
    if (argc == 1) field = argv[0];
    else if (argc == 2) { name = argv[0]; field = argv[1]; }
    else { warn("%s", "usage: get [NAME] FIELD"); return X_USAGE; }
    if (!have_efi_runway()) { warn("%s", "EFI/RUNWAY is absent (native selection is off)"); return X_IO; }
    if (!name && !strcmp(field, "BOOTED")) return get_booted();
    if (!name) {   /* global scalars */
        if (!in_list(field, global_get_fields)) { warn("unknown field %s", field); return X_USAGE; }
        snprintf(p, sizeof(p), EFI_ROOT "/%s", field);
    } else {
        if (!slot_path(name, sp)) { warn("bad slot name %s", name); return X_USAGE; }
        if (!in_list(field, slot_get_fields)) { warn("unknown field %s", field); return X_USAGE; }
        snprintf(p, sizeof(p), "%s/%s", sp, field);
    }
    rc = read_scalar(p, v, sizeof(v));
    if (rc == RWY_ERR_NOENT) { warn("%s is not set", p); return X_IO; }
    if (rc) { warn("cannot read %s", p); return X_IO; }
    puts(v);
    return 0;
}

/* True when no label is wanted, or the LABEL of the slot is exactly that text. */
static int slot_matches(const char *slot, const char *label) {
    char p[PATH_MAX], v[4200];
    if (!label) return 1;
    snprintf(p, sizeof(p), EFI_ROOT "/%s/LABEL", slot);
    return !read_scalar(p, v, sizeof(v)) && !strcmp(v, label);
}

static void print_slots_filtered_json(int n, const char *label) {
    int i, first = 1;
    printf("[");
    for (i = 0; i < n; i++) {
        if (!slot_matches(g_names[i], label)) continue;
        printf("%s\n  ", first ? "" : ",");
        json_str(g_names[i]);
        first = 0;
    }
    puts(first ? "]" : "\n]");
}

/* Print the slot names, or only the slots whose LABEL is exactly the given text. */
int cmd_slots(int argc, char **argv) {
    const char *label = NULL;
    int json = 0, i, n;

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) {
            json = 1;
        } else if (!strcmp(argv[i], "--label") && i + 1 < argc) {
            label = argv[++i];
        } else {
            warn("%s", "usage: slots [--json] [--label LABEL]");
            return X_USAGE;
        }
    }
    if (!have_efi_runway()) { warn("%s", "EFI/RUNWAY is absent (native selection is off)"); return X_IO; }
    n = list_slots();
    if (json) {
        print_slots_filtered_json(n, label);
        return 0;
    }
    for (i = 0; i < n; i++)
        if (slot_matches(g_names[i], label)) puts(g_names[i]);
    return 0;
}
