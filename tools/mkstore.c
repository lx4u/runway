/*
 * mkstore — build a deterministic store (a RUNWAY.DAT file or a partition window) in v11 format.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dual superblocks (blocks 0,1), bitmap image, inode table image with
 * per-block headers + CRC32C, one contiguous data extent per object, POSIX
 * metadata. Optional block-layer encryption (--encrypt: AES-256-XTS plus a
 * keyslot header with a transport passphrase in slot 0).
 *
 * The store starts with an empty root. Files come from --add, --efi-boot and
 * the recovery options; their parent directories are made on use.
 *
 * Output is byte-for-byte what the former tools/mkstore.py produced.
 */
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "hostdev.h"
#include "rwy_core.h"
#include "rwy_crypto.h"
#include "rwy_recarea.h"
#include "rwy_bundle.h"
#include "crypto_openssl.h"

#define MAX_FILES 64
#define MAX_ENTS  (MAX_FILES + 16)

typedef struct {
    uint8_t  type;      /* RWY_TYPE_FILE / RWY_TYPE_DIR */
    uint32_t mode;
    uint8_t *data;      /* file or serialized directory bytes */
    uint64_t size;
    uint32_t start, nb; /* data extent */
} obj_t;

typedef struct { uint64_t parent, oid; uint8_t type; char name[RWY_NAME_MAX + 1]; } ent_t;

static obj_t g_obj[RWY_MAX_INODES + 1];  /* indexed by oid */
static ent_t g_ent[MAX_ENTS];
static unsigned g_nent, g_nobj;

static void die(const char *msg, const char *arg) {
    fprintf(stderr, "mkstore: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(2);
}

static uint64_t new_obj(uint8_t type, uint32_t mode) {
    if (g_nobj >= RWY_MAX_INODES) die("too many objects", NULL);
    uint64_t oid = ++g_nobj;
    g_obj[oid].type = type;
    g_obj[oid].mode = mode;
    return oid;
}

static void link_ent(uint64_t parent, uint64_t oid, const char *name) {
    if (g_nent >= MAX_ENTS) die("too many entries", NULL);
    g_ent[g_nent].parent = parent;
    g_ent[g_nent].oid = oid;
    g_ent[g_nent].type = g_obj[oid].type;
    snprintf(g_ent[g_nent].name, sizeof(g_ent[g_nent].name), "%s", name);
    g_nent++;
}

static uint64_t add_dir(uint64_t parent, const char *name) {
    uint64_t oid = new_obj(RWY_TYPE_DIR, RWY_S_IFDIR | 0755);
    link_ent(parent, oid, name);
    return oid;
}

static uint8_t *read_file(const char *path, uint64_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) die(strerror(errno), path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n ? (size_t)n : 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) die("read failed", path);
    fclose(f);
    *len = (uint64_t)n;
    return b;
}

static uint64_t add_file(uint64_t parent, const char *name, uint32_t mode,
                         const void *data, uint64_t size) {
    uint64_t oid = new_obj(RWY_TYPE_FILE, RWY_S_IFREG | mode);
    g_obj[oid].data = (uint8_t *)data;
    g_obj[oid].size = size;
    link_ent(parent, oid, name);
    return oid;
}

/* Serialized directory: (oid, reclen, type, namelen, reserved, name), 8-aligned. */
static void build_dir(uint64_t dir) {
    size_t cap = 0, n = 0;
    uint8_t *buf;
    for (unsigned i = 0; i < g_nent; i++)
        if (g_ent[i].parent == dir) cap += (16 + strlen(g_ent[i].name) + 7) & ~7u;
    buf = calloc(cap ? cap : 1, 1);
    for (unsigned i = 0; i < g_nent; i++) {
        if (g_ent[i].parent != dir) continue;
        uint32_t nl = (uint32_t)strlen(g_ent[i].name), rl = (16 + nl + 7) & ~7u;
        memcpy(buf + n, &g_ent[i].oid, 8);
        memcpy(buf + n + 8, &rl, 4);
        buf[n + 12] = g_ent[i].type;
        buf[n + 13] = (uint8_t)nl;
        memcpy(buf + n + 16, g_ent[i].name, nl);
        n += rl;
    }
    g_obj[dir].data = buf;
    g_obj[dir].size = n;
}

static void parse_uuid(const char *in, uint8_t out[16]) {
    char hex[33];
    size_t n = 0;
    if (!strncmp(in, "urn:uuid:", 9)) in += 9;
    for (; *in; in++) {
        if (*in == '-' || *in == '{' || *in == '}') continue;
        if (n >= 32 || !strchr("0123456789abcdefABCDEF", *in)) die("invalid UUID format", in);
        hex[n++] = *in;
    }
    if (n != 32) die("invalid UUID format", NULL);
    for (int i = 0; i < 16; i++) {
        unsigned v;
        sscanf(&hex[2 * i], "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void random_uuid(uint8_t out[16]) {
    if (rwy_random(out, 16)) die("no random source (RWY_CRYPTO=builtin has none; give --uuid)", NULL);
    out[6] = (out[6] & 0x0f) | 0x40;
    out[8] = (out[8] & 0x3f) | 0x80;
}

/* Keyslot header: slot 0 holds the DEK wrapped by a PBKDF2-derived KEK. */
static void build_keyslot_header(rwy_keyslot_header_t *h, const uint8_t dek[32], const char *pass) {
    uint8_t kek[64], mac[32];
    const uint32_t iterations = 1000;
    rwy_keyslot_t *s0 = &h->slots[0];
    memset(h, 0, sizeof(*h));
    h->magic = RWY_KEYSLOT_MAGIC;
    h->version = 1;
    h->crypt_algo = RWY_CRYPT_AES_XTS_256;
    h->slot_count = RWY_KEYSLOT_COUNT;
    for (unsigned i = 0; i < 3; i++) h->slots[i].slot_type = i;
    s0->state = RWY_KEYSLOT_STATE_ACTIVE;
    s0->kdf_type = RWY_KDF_PBKDF2_SHA256;
    s0->kdf_iterations = iterations;
    if (rwy_random(s0->salt, sizeof(s0->salt))) die("no random source for the keyslot salt (give --random-seed)", NULL);
    rwy_pbkdf2_sha256(pass, strlen(pass), s0->salt, 32, iterations, kek, 64);
    for (unsigned i = 0; i < 32; i++) s0->wrapped_key[i] = dek[i] ^ kek[i];
    rwy_hmac_sha256(kek + 32, 32, dek, 32, mac);
    memcpy(s0->wrapped_key + 32, mac, 32);
    h->crc32c = rwy_crc32c(h, sizeof(*h));
}

static void sha_cat(const uint8_t *a, size_t an, const char *b, uint8_t out[32]) {
    uint8_t buf[256];
    size_t bn = strlen(b);
    if (an + bn > sizeof(buf)) die("seed too long", NULL);
    memcpy(buf, a, an);
    memcpy(buf + an, b, bn);
    rwy_sha256(buf, an + bn, out);
}

static hostdev_t g_hd;
static rwy_block_dev_t g_dev;   /* the output: a file, or a window of an image */
static int g_encrypt;
static rwy_xts_ctx_t g_xts;
static int g_seeded;

static void put_block(uint32_t idx, const uint8_t *blk) {
    uint8_t enc[RWY_BLOCK_SIZE];
    if (g_encrypt && idx >= 2) {
        rwy_xts_encrypt_sector(&g_xts, idx, blk, enc, RWY_BLOCK_SIZE);
        blk = enc;
    }
    if (g_dev.write(g_dev.ctx, (uint64_t)idx * RWY_BLOCK_SIZE, blk, RWY_BLOCK_SIZE))
        die("write failed", strerror(errno));
}

/* Metadata block: 32 B header (CRC at offset 24 over the block) + payload. */
static void put_meta(uint32_t idx, uint32_t i, uint32_t kind, const uint8_t *payload, size_t plen) {
    uint8_t blk[RWY_BLOCK_SIZE];
    rwy_blk_header_t h = { .magic = RWY_BLK_MAGIC, .generation = 1, .index = i, .kind = kind };
    memset(blk, 0, sizeof(blk));
    memcpy(blk, &h, sizeof(h));
    memcpy(blk + 32, payload, plen);
    h.crc32c = rwy_crc32c(blk, sizeof(blk));
    memcpy(blk, &h, sizeof(h));
    put_block(idx, blk);
}

#define USAGE "usage: mkstore [--size MiB] [--encrypt] [--passphrase P]\n" \
              "               [--dek-in FILE] [--dek-out FILE] [--uuid U] [--efi-boot EFI] [--add DEST=SRC] [--arch A] [--slots N] [--quota[=N]]\n" \
              "               [--flight-records N] [--journal-records M]\n" \
              "               [--recovery-size MiB] [--recovery KERNEL [--initrd F] [--dtb F] [--args TEXT|@FILE]]\n" \
              "               [--loader-size MiB] [--loader FILE [--wdt DIR]]\n" \
              "               [--random-seed S] [path|image@offset[+length]]\n"

typedef struct {
    const char *path, *pass, *uuid_arg, *efi_boot, *arch, *random_seed, *dek_in, *dek_out;
    const char *adds[MAX_FILES];
    unsigned nadds;
    const char *rec_file[RWY_RA_KINDS];   /* --recovery, --initrd, --dtb, --args */
    const char *loader_file, *wdt_dir;
    unsigned long size_mib, slots, quota, recovery_mib, flight_records, journal_records, loader_mib;
    int have_slots, have_quota;
} mkopts_t;

static uint32_t flight_blocks(unsigned long records) {
    return records ? (uint32_t)((records + 63) / 64) : 0;
}

static uint32_t journal_blocks(unsigned long records) {
    return records ? (uint32_t)((records + 9) / 10) : 0;
}

static void check_flight_and_journal_size(const mkopts_t *o) {
    uint32_t total = (uint32_t)(o->size_mib * 1024 * 1024 / RWY_BLOCK_SIZE);
    if (flight_blocks(o->flight_records) > total / 4)
        die("--flight-records: area exceeds a quarter of the store", NULL);
    if (journal_blocks(o->journal_records) > total / 4)
        die("--journal-records: area exceeds a quarter of the store", NULL);
}

static void check_recovery_size(const mkopts_t *o) {
    if (o->recovery_mib * 2 > o->size_mib / 2) die("--recovery-size: both copies may not exceed half the store", NULL);
}

static void check_loader_size(const mkopts_t *o) {
    if (o->wdt_dir && !o->loader_file) die("--wdt needs --loader", NULL);
    if (o->loader_mib && !o->loader_file) die("--loader-size needs --loader", NULL);
    if (o->loader_mib * 2 > o->size_mib / 2) die("--loader-size: both copies may not exceed half the store", NULL);
}

/* Sets a number option. Returns 0 if c is not one. */
static int number_option(int c, mkopts_t *o) {
    switch (c) {
    case 's': o->size_mib = strtoul(optarg, NULL, 10); break;
    case 'n': o->slots = strtoul(optarg, NULL, 10); o->have_slots = 1; break;
    case 'q': o->quota = optarg ? strtoul(optarg, NULL, 10) : 80; o->have_quota = 1; break;
    case 'F': o->flight_records = strtoul(optarg, NULL, 10); break;
    case 'J': o->journal_records = strtoul(optarg, NULL, 10); break;
    case 'r': o->recovery_mib = strtoul(optarg, NULL, 10); break;
    default: return 0;
    }
    return 1;
}

/* Sets a recovery file option. Returns 0 if c is not one. */
static int recovery_option(int c, mkopts_t *o) {
    switch (c) {
    case 'K': o->rec_file[RWY_RA_KERNEL] = optarg; break;
    case 'I': o->rec_file[RWY_RA_INITRD] = optarg; break;
    case 'D': o->rec_file[RWY_RA_DTB] = optarg; break;
    case 'A': o->rec_file[RWY_RA_ARGS] = optarg; break;
    default: return 0;
    }
    return 1;
}

/* Sets a loader file option. Returns 0 if c is not one. */
static int loader_option(int c, mkopts_t *o) {
    switch (c) {
    case 'L': o->loader_file = optarg; break;
    case 'W': o->wdt_dir = optarg; break;
    case 'S': o->loader_mib = strtoul(optarg, NULL, 10); break;
    default: return 0;
    }
    return 1;
}

/* Returns -1 to go on, else the exit code. */
static int apply_option(int c, mkopts_t *o) {
    if (number_option(c, o) || recovery_option(c, o) || loader_option(c, o)) return -1;
    switch (c) {
    case 'e': g_encrypt = 1; break;
    case 'p': o->pass = optarg; break;
    case 'u': o->uuid_arg = optarg; break;
    case 'b': o->efi_boot = optarg; break;
    case 'a': if (o->nadds >= MAX_FILES) die("too many --add", NULL); o->adds[o->nadds++] = optarg; break;
    case 'c': o->arch = optarg; break;
    case 'R': o->random_seed = optarg; break;
    case 'i': o->dek_in = optarg; break;
    case 'k': o->dek_out = optarg; break;
    case 'h':
        fputs(USAGE, stdout);
        return 0;
    default:
        fputs(USAGE, stderr);
        return 2;
    }
    return -1;
}

static void check_slots_and_quota(const mkopts_t *o) {
    if (o->have_slots && (o->slots < 2 || o->slots > 64)) die("--slots must be a number from 2 to 64 (rotation needs a fallback)", NULL);
    if (o->have_quota && (o->quota < 10 || o->quota > 95)) die("--quota must be a number from 10 to 95", NULL);
}

static void check_options(const mkopts_t *o) {
    if ((o->dek_in || o->dek_out) && !g_encrypt) die("--dek-in and --dek-out need --encrypt", NULL);
    check_slots_and_quota(o);
    if (o->size_mib < 8 || o->size_mib > 4095) die("store size must be between 8 and 4095 MiB", NULL);
    check_flight_and_journal_size(o);
    check_recovery_size(o);
    check_loader_size(o);
    if (!o->rec_file[RWY_RA_KERNEL] && (o->rec_file[RWY_RA_INITRD] || o->rec_file[RWY_RA_DTB] || o->rec_file[RWY_RA_ARGS]))
        die("--initrd, --dtb and --args need --recovery", NULL);
}

/* Returns -1 to go on, else the exit code. */
static int parse_options(int argc, char **argv, mkopts_t *o) {
    static const struct option opts[] = {
        {"size", 1, 0, 's'}, {"encrypt", 0, 0, 'e'}, {"passphrase", 1, 0, 'p'},
        {"uuid", 1, 0, 'u'}, {"efi-boot", 1, 0, 'b'},
        {"add", 1, 0, 'a'}, {"arch", 1, 0, 'c'}, {"slots", 1, 0, 'n'}, {"quota", 2, 0, 'q'},
        {"flight-records", 1, 0, 'F'}, {"journal-records", 1, 0, 'J'},
        {"recovery-size", 1, 0, 'r'}, {"recovery", 1, 0, 'K'}, {"initrd", 1, 0, 'I'}, {"dtb", 1, 0, 'D'}, {"args", 1, 0, 'A'},
        {"loader-size", 1, 0, 'S'}, {"loader", 1, 0, 'L'}, {"wdt", 1, 0, 'W'},
        {"random-seed", 1, 0, 'R'}, {"dek-in", 1, 0, 'i'}, {"dek-out", 1, 0, 'k'}, {"help", 0, 0, 'h'}, {0, 0, 0, 0}
    };
    int c, rc;

    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1)
        if ((rc = apply_option(c, o)) != -1) return rc;
    if (optind < argc) o->path = argv[optind];
    check_options(o);
    return -1;
}

static const char *boot_name_for(const char *arch) {
    if (!strcmp(arch, "arm64") || !strcmp(arch, "aarch64")) return "BOOTAA64.EFI";
    if (!strcmp(arch, "riscv64")) return "BOOTRISCV64.EFI";
    if (!strcmp(arch, "ia32")) return "BOOTIA32.EFI";
    if (!strcmp(arch, "arm")) return "BOOTARM.EFI";
    if (strcmp(arch, "x86_64")) die("invalid --arch", arch);
    return "BOOTX64.EFI";
}

typedef struct {
    uint64_t store_size;
    uint32_t total_blocks, bitmap_bytes, bitmap_blocks, tbl_blocks, area_blocks;
    uint32_t tbl_start, bitmap_start, rec_start, flight_start, flight_blocks;
    uint32_t journal_start, journal_blocks, recovery_start, recovery_blocks;
    uint32_t loader_start, loader_blocks, net_start, net_blocks, data_start;
} layout_t;

/* One block holds every setting of the network flight report. */
#define NET_COPY_BLOCKS 1U

static void compute_layout(layout_t *l, unsigned long size_mib, uint32_t flight_blocks,
                           uint32_t journal_blocks, uint32_t recovery_blocks,
                           uint32_t loader_blocks) {
    l->store_size = (uint64_t)size_mib * 1024 * 1024;
    l->total_blocks = (uint32_t)(l->store_size / RWY_BLOCK_SIZE);
    l->bitmap_bytes = (l->total_blocks + 7) / 8;
    l->bitmap_blocks = (l->bitmap_bytes + (RWY_BLOCK_SIZE - 32) - 1) / (RWY_BLOCK_SIZE - 32);
    l->tbl_blocks = RWY_INODE_TBL_BLOCKS;
    l->area_blocks = l->tbl_blocks + l->bitmap_blocks;
    l->tbl_start = RWY_SB_BLOCKS;   /* the first image area is the active one */
    l->bitmap_start = l->tbl_start + l->tbl_blocks;
    l->rec_start = RWY_SB_BLOCKS + 2 * l->area_blocks;
    l->flight_start = l->rec_start + RWY_REC_BLOCKS;
    l->flight_blocks = flight_blocks;
    l->journal_start = l->flight_start + 2 * l->flight_blocks;
    l->journal_blocks = journal_blocks;
    l->recovery_start = l->journal_start + 2 * l->journal_blocks;
    l->recovery_blocks = recovery_blocks;
    l->loader_start = l->recovery_start + 2 * l->recovery_blocks;
    l->loader_blocks = loader_blocks;
    l->net_start = loader_blocks ? (l->loader_start + 2 + 2 * l->loader_blocks) : l->loader_start;
    l->net_blocks = NET_COPY_BLOCKS;
    l->data_start = l->net_start + 2 * l->net_blocks;
}

/* Find or make the directory at PATH ("EFI/Linux"), creating missing parents. */
static uint64_t ensure_dir(const char *path) {
    char *copy = strdup(path), *save = NULL;
    uint64_t parent = 1;

    for (char *name = strtok_r(copy, "/", &save); name; name = strtok_r(NULL, "/", &save)) {
        uint64_t oid = 0;
        for (unsigned i = 0; i < g_nent && !oid; i++)
            if (g_ent[i].parent == parent && !strcmp(g_ent[i].name, name)) oid = g_ent[i].oid;
        parent = oid ? oid : add_dir(parent, name);
    }
    free(copy);
    return parent;
}

/* Add one --add DEST=SRC file; the parent directories of DEST are made on use. */
static void add_user_file(const char *spec) {
    char *dest = strdup(spec), *src = strchr(dest, '='), *slash;
    uint64_t parent = 1, n;
    uint8_t *b;

    if (!src) die("--add expects DEST=SRC", spec);
    *src++ = 0;
    slash = strrchr(dest, '/');
    if (slash) {
        *slash++ = 0;
        parent = ensure_dir(dest);
    } else slash = dest;
    b = read_file(src, &n);
    add_file(parent, slash, 0755, b, n);
}

/* The mode and owner are those the core gives EFI/RUNWAY entries. */
static void add_runway_config(const mkopts_t *o) {
    static char slots_txt[8], quota_txt[8];
    uint64_t d_runway = ensure_dir("EFI/RUNWAY");

    add_file(d_runway, "VERSION", 0644, "1\n", 2);
    if (o->have_slots) {
        snprintf(slots_txt, sizeof(slots_txt), "%lu\n", o->slots);
        add_file(d_runway, "SLOTS", 0644, slots_txt, strlen(slots_txt));
    }
    if (o->have_quota) {
        snprintf(quota_txt, sizeof(quota_txt), "%lu\n", o->quota);
        add_file(d_runway, "QUOTA", 0644, quota_txt, strlen(quota_txt));
    }
}

/* OIDs: 1 = root, then directories and files in the order they are made. */
static void add_contents(mkopts_t *o) {
    unsigned i;

    g_nobj = 1;
    g_obj[1].type = RWY_TYPE_DIR;
    g_obj[1].mode = RWY_S_IFDIR | 0755;
    if (o->have_slots || o->have_quota) add_runway_config(o);
    if (o->efi_boot) {
        uint64_t n;
        uint8_t *b = read_file(o->efi_boot, &n);
        add_file(ensure_dir("EFI/BOOT"), boot_name_for(o->arch), 0755, b, n);
    }
    for (i = 0; i < o->nadds; i++)
        add_user_file(o->adds[i]);

    for (uint64_t ob = 1; ob <= g_nobj; ob++)
        if (g_obj[ob].type == RWY_TYPE_DIR) build_dir(ob);
}

/* Sequential single runs in oid order; returns the bitmap. */
static uint8_t *allocate_blocks(const layout_t *l) {
    uint8_t *bm = calloc(l->bitmap_bytes, 1);
    uint32_t next = l->data_start;

    for (uint32_t b = 0; b < l->data_start; b++) bm[b >> 3] |= 1u << (b & 7);
    for (uint64_t o = 1; o <= g_nobj; o++) {
        obj_t *ob = &g_obj[o];
        ob->nb = (uint32_t)((ob->size + RWY_BLOCK_SIZE - 1) / RWY_BLOCK_SIZE);
        if (!ob->nb) continue;
        if ((uint64_t)next + ob->nb > l->total_blocks) die("store too small for contents", NULL);
        ob->start = next;
        for (uint32_t b = next; b < next + ob->nb; b++) bm[b >> 3] |= 1u << (b & 7);
        next += ob->nb;
    }
    return bm;
}

/* Write the raw data key for tests and tools that open a store by key (mode 0600). */
static void write_dek_file(const char *path, const uint8_t dek[32]) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, dek, 32) != 32 || close(fd)) die(strerror(errno), path);
}

static void setup_crypto(const mkopts_t *o, rwy_keyslot_header_t *ks) {
    uint8_t dek[32], k1[32], k2[32];

    if (o->dek_in) {
        uint64_t n;
        uint8_t *p = read_file(o->dek_in, &n);
        if (n != sizeof(dek)) die("--dek-in must hold 32 bytes", o->dek_in);
        memcpy(dek, p, sizeof(dek));
        free(p);
    } else if (rwy_random(dek, sizeof(dek))) die("no random source for the data key (give --random-seed)", NULL);
    sha_cat(dek, 32, "DATA_KEY", k1);
    sha_cat(dek, 32, "TWEAK_KEY", k2);
    rwy_xts_init(&g_xts, k1, k2);
    if (o->dek_out) write_dek_file(o->dek_out, dek);
    build_keyslot_header(ks, dek, o->pass);
}

static void write_data_blocks(void) {
    for (uint64_t o = 1; o <= g_nobj; o++) {
        obj_t *ob = &g_obj[o];
        for (uint32_t b = 0; b < ob->nb; b++) {
            uint8_t blk[RWY_BLOCK_SIZE];
            uint64_t off = (uint64_t)b * RWY_BLOCK_SIZE;
            memset(blk, 0, sizeof(blk));
            for (uint64_t i = 0; i < RWY_BLOCK_SIZE && off + i < ob->size; i++)
                blk[i] = ob->data[off + i];
            put_block(ob->start + b, blk);
        }
    }
}

static void write_bitmap(const layout_t *l, const uint8_t *bm) {
    for (uint32_t i = 0; i < l->bitmap_blocks; i++) {
        size_t off = (size_t)i * (RWY_BLOCK_SIZE - 32), n = l->bitmap_bytes - off;
        if (n > RWY_BLOCK_SIZE - 32) n = RWY_BLOCK_SIZE - 32;
        put_meta(l->bitmap_start + i, i, RWY_BLK_KIND_BITMAP, bm + off, n);
    }
}

/* A seeded image has a fixed time. SOURCE_DATE_EPOCH overrides it; else the clock. */
static int64_t image_time(void) {
    int64_t sec;
    int rc = hostdev_source_epoch(&sec);
    if (rc < 0) die("SOURCE_DATE_EPOCH is not a number of seconds", NULL);
    if (rc) return sec;
    return g_seeded ? 0 : (int64_t)time(NULL);
}

/* Inode table image: 15 inodes per block. */
static void write_inode_table(const layout_t *l) {
    static rwy_inode_t tbl[RWY_MAX_INODES + RWY_INODES_PER_BLOCK];

    for (uint64_t o = 1; o <= g_nobj; o++) {
        obj_t *ob = &g_obj[o];
        rwy_inode_t *in = &tbl[o - 1];
        in->oid = o;
        in->type = ob->type;
        in->mode = ob->mode;
        in->size = ob->size;
        in->atime_sec = in->mtime_sec = in->ctime_sec = in->btime_sec = image_time();
        in->generation = 1;
        in->extent_count = ob->nb ? 1 : 0;
        in->extents[0].start = ob->nb ? ob->start : 0;
        in->extents[0].len = ob->nb;
        in->nlink = 1;
    }
    for (uint32_t i = 0; i < l->tbl_blocks; i++)
        put_meta(l->tbl_start + i, i, RWY_BLK_KIND_INODE,
                 (const uint8_t *)tbl + (size_t)i * RWY_INODES_PER_BLOCK * RWY_INODE_SIZE,
                 (size_t)RWY_INODES_PER_BLOCK * RWY_INODE_SIZE);
}

/* Both superblock slots, generation 1. */
static void write_superblocks(const layout_t *l, const uint8_t uuid[16], uint32_t crypt_algo,
                              const rwy_keyslot_header_t *ks) {
    rwy_superblock_t sb;

    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, "RUNWAYSB", 8);
    sb.version = RWY_VERSION;
    sb.block_size = RWY_BLOCK_SIZE;
    sb.total_blocks = l->total_blocks;
    sb.max_inodes = RWY_MAX_INODES;
    sb.total_size = l->store_size;
    sb.generation = 1;
    sb.root_oid = 1;
    sb.inode_tbl_start = l->tbl_start;
    sb.inode_tbl_blocks = l->tbl_blocks;
    sb.bitmap_start = l->bitmap_start;
    sb.bitmap_blocks = l->bitmap_blocks;
    sb.data_blocks = l->total_blocks - l->data_start;
    sb.area_blocks = l->area_blocks;
    sb.pool_start = l->data_start;
    sb.rec_start = l->rec_start;
    sb.flight_start = l->flight_start;
    sb.flight_blocks = l->flight_blocks;
    sb.journal_start = l->journal_start;
    sb.journal_blocks = l->journal_blocks;
    sb.recovery_start = l->recovery_start;
    sb.recovery_blocks = l->recovery_blocks;
    sb.loader_start = l->loader_start;
    sb.loader_blocks = l->loader_blocks;
    sb.net_start = l->net_start;
    sb.net_blocks = l->net_blocks;
    sb.crypt_algo = crypt_algo;
    memcpy(sb.uuid, uuid, 16);
    if (rwy_random(sb.member_uuid, 16)) memset(sb.member_uuid, 0, 16);   /* no random source: named at the first mirror mount */
    sb.crc32c = rwy_crc32c(&sb, offsetof(rwy_superblock_t, crc32c));
    if (g_encrypt) memcpy((uint8_t *)&sb + RWY_KEYSLOT_OFFSET, ks, sizeof(*ks));
    put_block(0, (uint8_t *)&sb);
    put_block(1, (uint8_t *)&sb);
}

/* The bytes of one --recovery part; --args is text, or @FILE to read a file. */
static const uint8_t *load_part(unsigned kind, const char *arg, uint64_t *len) {
    if (kind == RWY_RA_ARGS && arg[0] != '@') {
        *len = strlen(arg);
        return (const uint8_t *)arg;
    }
    return read_file(kind == RWY_RA_ARGS ? arg + 1 : arg, len);
}

static void load_bundle(const mkopts_t *o, rwy_ra_bundle_t *b) {
    memset(b, 0, sizeof(*b));
    for (unsigned k = 0; k < RWY_RA_KINDS; k++) {
        uint64_t n;
        if (!o->rec_file[k]) continue;
        b->part[k].data = load_part(k, o->rec_file[k], &n);
        if (n == 0 || n > 0xffffffffu) die("empty or too large recovery part", o->rec_file[k]);
        b->part[k].len = (uint32_t)n;
    }
    if (rwy_ra_blocks(b) == 0) die("recovery bundle is not valid (--args is at most 4096 bytes)", NULL);
}

/* Blocks of one copy: --recovery-size, else the bundle plus half again for a later, larger one. */
static uint32_t recovery_size(const mkopts_t *o, const rwy_ra_bundle_t *b, unsigned long size_mib) {
    uint32_t need = rwy_ra_blocks(b), blocks = (uint32_t)(o->recovery_mib * 1024 * 1024 / RWY_BLOCK_SIZE);
    if (!need) return blocks;
    if (!blocks) {
        blocks = need + need / 2;
        printf("recovery area: 2 x %u blocks (bundle %u blocks)\n", blocks, need);
    }
    if (need > blocks) die("--recovery-size is smaller than the bundle", NULL);
    if (2ull * blocks * RWY_BLOCK_SIZE > (uint64_t)size_mib * 1024 * 1024 / 2) die("recovery bundle: both copies may not exceed half the store", NULL);
    return blocks;
}

/* Fill copy A through the raw file: the area is plaintext. */
static void write_recovery(const layout_t *l, const rwy_ra_bundle_t *b) {
    int rc = rwy_ra_write(&g_dev, l->recovery_start, l->recovery_blocks, b);
    if (rc) die("cannot write the recovery area", rwy_strerror(rc));
}

static int wdt_filter(const struct dirent *de) {
    return de->d_name[0] != '.';
}

static uint32_t load_wdt_dir(const char *dir, rwy_bundle_payload_t *parts, uint32_t max_wdt) {
    struct dirent **namelist = NULL;
    int n = scandir(dir, &namelist, wdt_filter, alphasort);
    uint32_t count = 0;

    if (n < 0) die(strerror(errno), dir);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, namelist[i]->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            uint64_t len = 0;
            if (count >= max_wdt) die("too many WDT drivers in --wdt (at most 8)", dir);
            parts[count].kind = RWY_ENTRY_WDT;
            parts[count].data = read_file(path, &len);
            parts[count].len = (uint32_t)len;
            count++;
        }
        free(namelist[i]);
    }
    free(namelist);
    return count;
}

static uint32_t load_loader_parts(const mkopts_t *o, rwy_bundle_payload_t parts[9]) {
    uint64_t len = 0;

    if (!o->loader_file) return 0;
    parts[0].kind = RWY_ENTRY_LOADER;
    parts[0].data = read_file(o->loader_file, &len);
    if (len == 0 || len > 0xffffffffu) die("empty or too large loader binary", o->loader_file);
    parts[0].len = (uint32_t)len;

    if (o->wdt_dir)
        return 1 + load_wdt_dir(o->wdt_dir, parts + 1, 8);
    return 1;
}

static uint32_t loader_size(const mkopts_t *o, const rwy_bundle_payload_t *parts, uint32_t count,
                            unsigned long size_mib) {
    uint32_t need, blocks;
    uint64_t combined = 0, twice, rounded;

    if (!count) return 0;
    need = rwy_bundle_blocks(&rwy_loader_bundle_spec, parts, count);
    if (!need) die("loader bundle is invalid", NULL);
    blocks = (uint32_t)(o->loader_mib * 1024 * 1024 / RWY_BLOCK_SIZE);
    if (blocks) {
        if (need > blocks) die("--loader-size is smaller than the bundle", NULL);
    } else {
        for (uint32_t i = 0; i < count; i++) combined += parts[i].len;
        twice = combined * 2;
        rounded = (twice + 1024 * 1024 - 1) / (1024 * 1024) * (1024 * 1024);
        blocks = (uint32_t)(rounded / RWY_BLOCK_SIZE);
        if (blocks < need) blocks = need;
        printf("loader area: 2 x %u blocks (bundle %u blocks)\n", blocks, need);
    }
    if ((2ull * blocks + 2) * RWY_BLOCK_SIZE > (uint64_t)size_mib * 1024 * 1024 / 2)
        die("loader area: both copies may not exceed half the store", NULL);
    return blocks;
}

static void write_loader_area(const layout_t *l, const rwy_bundle_payload_t *parts, uint32_t count) {
    rwy_loader_record_t rec;
    int rc;

    memset(&rec, 0, sizeof(rec));
    rec.magic = RWY_BLK_MAGIC;
    rec.version = RWY_LOADER_RECORD_VERSION;
    rec.sequence = 1;
    rec.good = 0;
    rec.timeout = RWY_LOADER_TIMEOUT_DEFAULT;
    rec.crc32c = rwy_crc32c(&rec, offsetof(rwy_loader_record_t, crc32c));

    if (g_dev.write(g_dev.ctx, (uint64_t)l->loader_start * RWY_BLOCK_SIZE, &rec, sizeof(rec)))
        die("write failed", "loader state record");

    rc = rwy_bundle_write(&g_dev, l->loader_start + 2, l->loader_blocks,
                          &rwy_loader_bundle_spec, parts, count);
    if (rc) die("cannot write the loader area", rwy_strerror(rc));
}

/* Without --efi-boot, a CHAIN.EFI next to the program seeds EFI/BOOT. */
static const char *default_boot_efi(const char *prog, char *buf, size_t n) {
    const char *slash = strrchr(prog, '/');

    if (!slash) return NULL;
    snprintf(buf, n, "%.*s/CHAIN.EFI", (int)(slash - prog), prog);
    return access(buf, F_OK) == 0 ? buf : NULL;
}

/* Zero the first size bytes of the output; areas that read as zero already are not written. */
static void clear_window(uint64_t size) {
    static uint8_t buf[1 << 20], zero[1 << 20];

    for (uint64_t off = 0; off < size; off += sizeof(buf)) {
        size_t n = size - off < sizeof(buf) ? (size_t)(size - off) : sizeof(buf);
        if (g_dev.read(g_dev.ctx, off, buf, n)) die("read failed", strerror(errno));
        if (memcmp(buf, zero, n) && g_dev.write(g_dev.ctx, off, zero, n)) die("write failed", strerror(errno));
    }
}

/* Make the output: a new file, or an existing window of an image (PATH@OFFSET[+LENGTH]) cleared to zero. */
static void open_output(const char *spec, uint64_t store_size) {
    char path[4096];
    uint64_t off, len;
    int windowed;

    if (hostdev_parse(spec, path, sizeof(path), &off, &len, &windowed)) die("bad window", spec);
    if (!windowed) {
        g_hd.fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (g_hd.fd < 0) die(strerror(errno), path);
        if (ftruncate(g_hd.fd, (off_t)store_size)) die("ftruncate failed", strerror(errno));
        hostdev_raw_dev(&g_hd, &g_dev);
        return;
    }
    if (hostdev_open(&g_hd, spec, O_RDWR)) die(strerror(errno), spec);
    hostdev_raw_dev(&g_hd, &g_dev);
    if (g_dev.size < store_size) die("the window is smaller than the store", spec);
    clear_window(store_size);
}

static void prepare_environment(mkopts_t *o, char *chain, size_t chain_sz, uint8_t uuid[16], const char *prog) {
    if (!o->efi_boot) o->efi_boot = default_boot_efi(prog, chain, chain_sz);
    if (o->random_seed) {
        if (rwy_drbg_seed(o->random_seed, strlen(o->random_seed))) die("--random-seed: bad seed (1 to 256 bytes)", NULL);
        g_seeded = 1;
        fprintf(stderr, "mkstore: random source seeded, the image is reproducible (not for production)\n");
    }
    boot_name_for(o->arch);   /* reject a bad --arch before anything is written */
    if (o->uuid_arg) parse_uuid(o->uuid_arg, uuid); else random_uuid(uuid);
}

int main(int argc, char **argv) {
    char chain[1024];
    mkopts_t o = {
        .path = "RUNWAY.DAT",
        .pass = "runway-test-passphrase",
        .arch = "x86_64",
        .size_mib = 256,
        .flight_records = 64,
        .journal_records = 320,
    };
    layout_t l;
    rwy_keyslot_header_t ks;
    rwy_ra_bundle_t bundle = {0};
    rwy_bundle_payload_t loader_parts[9];
    uint32_t nloader_parts = 0;
    uint32_t crypt_algo = RWY_CRYPT_NONE;
    uint8_t uuid[16], *bm;
    char u[37];
    int rc;

    rwy_crypto_host_init();
    rc = parse_options(argc, argv, &o);
    if (rc >= 0) return rc;
    prepare_environment(&o, chain, sizeof(chain), uuid, argv[0]);
    if (o.rec_file[RWY_RA_KERNEL]) load_bundle(&o, &bundle);
    if (o.loader_file) nloader_parts = load_loader_parts(&o, loader_parts);
    compute_layout(&l, o.size_mib, flight_blocks(o.flight_records),
                   journal_blocks(o.journal_records),
                   recovery_size(&o, &bundle, o.size_mib),
                   loader_size(&o, loader_parts, nloader_parts, o.size_mib));
    add_contents(&o);
    bm = allocate_blocks(&l);
    if (g_encrypt) {
        setup_crypto(&o, &ks);
        crypt_algo = RWY_CRYPT_AES_XTS_256;
    }

    open_output(o.path, l.store_size);
    write_data_blocks();
    write_bitmap(&l, bm);
    write_inode_table(&l);
    /* State record: copy 0 is current (sequence 1, empty); copy 1 stays zero, so it is invalid. */
    put_meta(l.rec_start, 0, RWY_BLK_KIND_RECORD, (const uint8_t *)"", 0);
    if (g_encrypt && l.journal_blocks) {
        uint8_t zero[RWY_BLOCK_SIZE] = {0};
        for (uint32_t b = 0; b < 2 * l.journal_blocks; b++)
            put_block(l.journal_start + b, zero);
    }
    write_superblocks(&l, uuid, crypt_algo, &ks);
    if (rwy_ra_blocks(&bundle)) write_recovery(&l, &bundle);
    if (l.loader_blocks) write_loader_area(&l, loader_parts, nloader_parts);
    if (g_dev.flush(g_dev.ctx)) die("flush failed", strerror(errno));
    hostdev_close(&g_hd);

    rwy_format_uuid(uuid, u);
    printf("wrote %s: %llu bytes, %u objects, gen=1, uuid=%s%s\n", o.path,
           (unsigned long long)l.store_size, g_nobj, u, g_encrypt ? " [ENCRYPTED AES-256-XTS]" : "");
    return 0;
}
