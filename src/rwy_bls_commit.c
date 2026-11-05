/*
 * rwy_bls_commit.c — Turning staged BLS entries into slots.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-only code.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "rwy_bls.h"
#include "rwy_blob.h"
#include "rwy_core.h"
#include "rwy_crypto.h"
#include "rwy_meta.h"

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int rwy_bls_encode_blob_name(const char *path, char *out, size_t out_sz) {
    size_t i = 0, pos = 0;
    if (!path || !out || out_sz == 0) return RWY_ERR_INVAL;
    while (path[i] == '/') i++;
    while (path[i]) {
        char c = path[i++];
        if (c == '/' || c == '%') {
            if (pos + 3 >= out_sz) { out[0] = 0; return RWY_ERR_NOSPC; }
            out[pos++] = '%';
            out[pos++] = '2';
            out[pos++] = (c == '/') ? 'F' : '5';
        } else {
            if (pos + 1 >= out_sz) { out[0] = 0; return RWY_ERR_NOSPC; }
            out[pos++] = c;
        }
    }
    out[pos] = 0;
    if (rwy_bootcfg_content_name(out, pos)) {   /* content names are not the bridge's to make */
        out[0] = 0;
        return RWY_ERR_PERM;
    }
    return (int)pos;
}

int rwy_bls_decode_blob_name(const char *blob, char *out, size_t out_sz) {
    size_t i = 0, pos = 0;
    if (!blob || !out || out_sz == 0) return RWY_ERR_INVAL;
    while (blob[i]) {
        if (blob[i] == '%' && blob[i + 1] && blob[i + 2]) {
            int h1 = hex_val(blob[i + 1]);
            int h2 = hex_val(blob[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
                if (pos + 1 >= out_sz) return RWY_ERR_NOSPC;
                out[pos++] = (char)((h1 << 4) | h2);
                i += 3;
                continue;
            }
        }
        if (pos + 1 >= out_sz) return RWY_ERR_NOSPC;
        out[pos++] = blob[i++];
    }
    out[pos] = 0;
    return (int)pos;
}

/* The BLOBS/ path of the bridge file that holds a /boot payload. */
static int payload_blob_path(const char *logical_path, char *out, size_t out_sz) {
    char enc[256];
    int rc = rwy_bls_encode_blob_name(logical_path, enc, sizeof(enc));
    if (rc <= 0) return rc < 0 ? rc : RWY_ERR_INVAL;
    snprintf(out, out_sz, "EFI/RUNWAY/BLOBS/%s", enc);
    return RWY_OK;
}

static int count_and_next_slot(rwy_store_t *s, char *slot_out, size_t slot_sz) {
    char name[256];
    size_t i = 0;
    uint32_t count = 0;
    uint64_t max_num = 0;

    while (rwy_readdir_nth(s, "EFI/RUNWAY", i++, name, sizeof(name), NULL) == RWY_OK) {
        if (strncmp(name, "CONFIG", 6) == 0 && name[6] >= '0' && name[6] <= '9') {
            uint64_t num = strtoull(name + 6, NULL, 10);
            if (num > max_num) max_num = num;
            count++;
        }
    }
    if (count >= 64) return RWY_ERR_NOSPC;
    snprintf(slot_out, slot_sz, "EFI/RUNWAY/CONFIG%llu", (unsigned long long)(max_num + 1));
    return RWY_OK;
}

static int write_slot_scalar(rwy_store_t *s, const char *path, const char *val, size_t len) {
    int rc = rwy_create(s, path, RWY_S_IFREG | 0644, 0, 0);
    if (!rc && len > 0) rc = rwy_write_path(s, path, 0, val, len);
    return rc;
}

typedef struct {
    char path[300];      /* the bridge file in BLOBS/ */
    char content[300];   /* BLOBS/<hash of its data> */
} bridge_blob_t;

/* Write what the joined initrd gets at `off` and feed the same bytes to the hash. */
static int write_hashed(rwy_store_t *s, const char *path, uint64_t off, const void *data, size_t len,
                        rwy_sha256_stream_t *h) {
    int rc = rwy_write_path(s, path, off, data, len);
    if (rc == RWY_OK) rwy_sha256_stream_update(h, data, len);
    return rc;
}

/* Copy the initrds one after the other, each padded to 4 bytes, into a new file. */
static int join_initrd_files(rwy_store_t *s, const char *dst_path,
                             const rwy_bls_entry_t *entry, rwy_sha256_stream_t *h) {
    uint64_t dst_off = 0;
    size_t k;
    int rc = rwy_create(s, dst_path, RWY_S_IFREG | 0644, 0, 0);
    if (rc != RWY_OK) return rc;

    for (k = 0; k < entry->num_initrd; k++) {
        char blob_path[300], chunk[4096];
        rwy_attr_t a;
        uint64_t src_off = 0;
        size_t pad;
        static const uint8_t zeros[4] = { 0 };

        rc = payload_blob_path(entry->initrd_paths[k], blob_path, sizeof(blob_path));
        if (rc == RWY_OK) rc = rwy_stat(s, blob_path, &a);
        if (rc != RWY_OK) return rc;

        while (src_off < a.size) {
            size_t rlen = sizeof(chunk);
            rc = rwy_read_path(s, blob_path, src_off, chunk, &rlen);
            if (rc != RWY_OK || rlen == 0) return rc != RWY_OK ? rc : RWY_ERR_IO;
            rc = write_hashed(s, dst_path, dst_off, chunk, rlen, h);
            if (rc != RWY_OK) return rc;
            src_off += rlen;
            dst_off += rlen;
        }

        pad = (size_t)((4 - (a.size % 4)) % 4);
        if (pad > 0) {
            rc = write_hashed(s, dst_path, dst_off, zeros, pad, h);
            if (rc != RWY_OK) return rc;
            dst_off += pad;
        }
    }
    return RWY_OK;
}

/* Name the BLOBS/ file of a bridge payload by the hash of its data. Reads the data, so call it before rwy_begin. */
static int name_bridge_blob(rwy_store_t *s, const char *blob_path, bridge_blob_t *b) {
    uint8_t digest[32];
    char name[RWY_BLOB_NAME_SIZE];
    int rc = rwy_blob_hash_path(s, blob_path, digest);
    if (rc != RWY_OK) return rc;
    rwy_blob_name(digest, name);
    snprintf(b->path, sizeof(b->path), "%s", blob_path);
    snprintf(b->content, sizeof(b->content), "EFI/RUNWAY/BLOBS/%s", name);
    return RWY_OK;
}

static int name_payload_blob(rwy_store_t *s, const char *logical_path, bridge_blob_t *b) {
    char blob_path[300];
    int rc = payload_blob_path(logical_path, blob_path, sizeof(blob_path));
    return rc == RWY_OK ? name_bridge_blob(s, blob_path, b) : rc;
}

/* Give the file the content name, as SYSTEM. If the blob is already there, `relink` makes the file its link. */
static int adopt_bridge_blob(rwy_store_t *s, const bridge_blob_t *b, int relink) {
    int actor = s->actor, rc;
    rwy_attr_t file, blob;

    rwy_set_actor(s, RWY_ACTOR_SYSTEM);
    if (rwy_stat(s, b->content, &blob) != RWY_OK) {
        rc = rwy_link(s, b->path, b->content);
    } else if (!relink || (rc = rwy_stat(s, b->path, &file)) != RWY_OK || file.oid == blob.oid) {
        rc = RWY_OK;
    } else {
        rc = rwy_unlink(s, b->path);
        if (rc == RWY_OK) rc = rwy_link(s, b->content, b->path);
    }
    rwy_set_actor(s, actor);
    return rc;
}

static int link_content_payload(rwy_store_t *s, const bridge_blob_t *b, int relink, const char *dst_path) {
    int rc = adopt_bridge_blob(s, b, relink);
    return rc == RWY_OK ? rwy_link(s, b->content, dst_path) : rc;
}

/* Join the initrds under a plain name while hashing them, then give the file its content name, or reuse the blob. */
static int make_joined_initrd(rwy_store_t *s, const rwy_bls_entry_t *entry, const char *dst_path) {
    bridge_blob_t b;
    rwy_sha256_stream_t h;
    uint8_t digest[32];
    char name[RWY_BLOB_NAME_SIZE];
    int rc;

    snprintf(b.path, sizeof(b.path), "EFI/RUNWAY/BLOBS/%s", RWY_BLOB_STAGE_NAME);
    if (rwy_sha256_stream_init(&h) != RWY_CRYPTO_OK) return RWY_ERR_IO;
    rc = join_initrd_files(s, b.path, entry, &h);
    rwy_sha256_stream_final(&h, digest);
    if (rc != RWY_OK) return rc;

    rwy_blob_name(digest, name);
    snprintf(b.content, sizeof(b.content), "EFI/RUNWAY/BLOBS/%s", name);
    rc = adopt_bridge_blob(s, &b, 0);
    if (rc == RWY_OK) rc = rwy_unlink(s, b.path);
    return rc == RWY_OK ? rwy_link(s, b.content, dst_path) : rc;
}

typedef struct {
    bridge_blob_t kernel, initrd, dtb;
} type1_blobs_t;

/* The payloads that are linked as they are (a joined initrd is made by the commit). */
static int name_type1_blobs(rwy_store_t *s, const rwy_bls_entry_t *entry, type1_blobs_t *tb) {
    int rc = name_payload_blob(s, entry->linux_path, &tb->kernel);
    if (rc == RWY_OK && entry->num_initrd == 1) rc = name_payload_blob(s, entry->initrd_paths[0], &tb->initrd);
    if (rc == RWY_OK && entry->has_dtb) rc = name_payload_blob(s, entry->dtb_path, &tb->dtb);
    return rc;
}

static int populate_type1_payloads(rwy_store_t *s, const char *slot,
                                  const rwy_bls_entry_t *entry, const type1_blobs_t *tb) {
    char dst[128];
    int rc;

    snprintf(dst, sizeof(dst), "%s/KERNEL", slot);
    rc = link_content_payload(s, &tb->kernel, 1, dst);
    if (rc != RWY_OK) return rc;

    snprintf(dst, sizeof(dst), "%s/INITRD", slot);
    if (entry->num_initrd == 1) {
        rc = link_content_payload(s, &tb->initrd, 1, dst);
        if (rc != RWY_OK) return rc;
    } else if (entry->num_initrd > 1) {
        rc = make_joined_initrd(s, entry, dst);
        if (rc != RWY_OK) return rc;
    }

    if (entry->has_dtb) {
        snprintf(dst, sizeof(dst), "%s/DTB", slot);
        rc = link_content_payload(s, &tb->dtb, 1, dst);
        if (rc != RWY_OK) return rc;
    }
    return RWY_OK;
}

static int populate_type1_scalars(rwy_store_t *s, const char *slot,
                                  const char *entry_name,
                                  const rwy_bls_entry_t *entry,
                                  uint32_t initial_tries) {
    char p[128], tries_str[16];
    int rc;

    snprintf(p, sizeof(p), "%s/ENTRY", slot);
    rc = write_slot_scalar(s, p, entry_name, strlen(entry_name));
    if (rc != RWY_OK) return rc;

    if (entry->has_title) {
        snprintf(p, sizeof(p), "%s/LABEL", slot);
        rc = write_slot_scalar(s, p, entry->title, strlen(entry->title));
        if (rc != RWY_OK) return rc;
    }

    if (entry->options_len > 0) {
        snprintf(p, sizeof(p), "%s/ARGS", slot);
        rc = write_slot_scalar(s, p, entry->options, entry->options_len);
        if (rc != RWY_OK) return rc;
    }

    if (entry->meta_len > 0) {
        snprintf(p, sizeof(p), "%s/META", slot);
        rc = write_slot_scalar(s, p, entry->meta, entry->meta_len);
        if (rc != RWY_OK) return rc;
    }

    snprintf(p, sizeof(p), "%s/TRIES", slot);
    snprintf(tries_str, sizeof(tries_str), "%u\n", initial_tries ? initial_tries : 3);
    rc = write_slot_scalar(s, p, tries_str, strlen(tries_str));
    if (rc != RWY_OK) return rc;

    snprintf(p, sizeof(p), "%s/STATE", slot);
    return write_slot_scalar(s, p, "INSTALLED\n", 10);
}

/* Blocks the joined INITRD of an entry takes: its blobs are linked when there is one, copied when there are several. */
static int joined_initrd_blocks(rwy_store_t *s, const rwy_bls_entry_t *entry, uint32_t *blocks) {
    size_t k;
    uint64_t bytes = 0;
    *blocks = 0;
    if (entry->num_initrd < 2) return RWY_OK;
    for (k = 0; k < entry->num_initrd; k++) {
        char blob_path[300];
        rwy_attr_t a;
        int rc = payload_blob_path(entry->initrd_paths[k], blob_path, sizeof(blob_path));
        if (rc == RWY_OK) rc = rwy_stat(s, blob_path, &a);
        if (rc != RWY_OK) return rc;
        bytes += a.size + 3;
    }
    *blocks = (uint32_t)((bytes + RWY_BLOCK_SIZE - 1) / RWY_BLOCK_SIZE);
    return RWY_OK;
}

int rwy_bls_commit_type1(rwy_store_t *s, const char *entry_name,
                          const char *conf_text, size_t conf_len,
                          uint32_t initial_tries) {
    rwy_bls_entry_t entry;
    type1_blobs_t tb;
    char slot[64];
    uint32_t initrd_blocks;
    int rc;

    if (!s || !entry_name || !conf_text || conf_len == 0) return RWY_ERR_INVAL;
    rc = rwy_bls_parse(conf_text, conf_len, &entry);
    if (rc != RWY_OK) return rc;
    rc = name_type1_blobs(s, &entry, &tb);
    if (rc != RWY_OK) return rc;

    rc = rwy_begin(s);
    if (rc != RWY_OK) return rc;
    rc = joined_initrd_blocks(s, &entry, &initrd_blocks);
    if (rc == RWY_OK) {
        rc = rwy_bootcfg_make_room(s, RWY_SLOT_META_BLOCKS + initrd_blocks, initrd_blocks);
        if (rc != RWY_OK) return rc;   /* the transaction is closed */
    } else {
        rwy_abort(s);
        return rc;
    }

    rc = count_and_next_slot(s, slot, sizeof(slot));
    if (rc == RWY_OK) rc = rwy_mkdir(s, slot, RWY_S_IFDIR | 0755, 0, 0);
    if (rc == RWY_OK) rc = populate_type1_payloads(s, slot, &entry, &tb);
    if (rc == RWY_OK) rc = populate_type1_scalars(s, slot, entry_name, &entry, initial_tries);

    if (rc == RWY_OK) {
        rc = rwy_commit(s);
    } else {
        rwy_abort(s);
    }
    return rc;
}

static int populate_type2_slot(rwy_store_t *s, const char *slot,
                               const char *entry_name,
                               const bridge_blob_t *blob,
                               uint32_t initial_tries) {
    char dst[128], p[128], tries_str[16];
    int rc;

    snprintf(dst, sizeof(dst), "%s/KERNEL", slot);
    rc = link_content_payload(s, blob, 0, dst);
    if (rc != RWY_OK) return rc;

    snprintf(p, sizeof(p), "%s/ENTRY", slot);
    rc = write_slot_scalar(s, p, entry_name, strlen(entry_name));
    if (rc != RWY_OK) return rc;

    snprintf(p, sizeof(p), "%s/TRIES", slot);
    snprintf(tries_str, sizeof(tries_str), "%u\n", initial_tries ? initial_tries : 3);
    rc = write_slot_scalar(s, p, tries_str, strlen(tries_str));
    if (rc != RWY_OK) return rc;

    snprintf(p, sizeof(p), "%s/STATE", slot);
    return write_slot_scalar(s, p, "INSTALLED\n", 10);
}

int rwy_bls_commit_type2(rwy_store_t *s, const char *entry_name,
                          const char *blob_path, uint32_t initial_tries) {
    char slot[64], magic[2];
    bridge_blob_t blob;
    size_t rlen = sizeof(magic);
    int rc;

    if (!s || !entry_name || !blob_path) return RWY_ERR_INVAL;

    rc = rwy_read_path(s, blob_path, 0, magic, &rlen);
    if (rc != RWY_OK || rlen < 2 || magic[0] != 'M' || magic[1] != 'Z') {
        return RWY_ERR_INVAL;
    }
    rc = name_bridge_blob(s, blob_path, &blob);
    if (rc != RWY_OK) return rc;

    rc = rwy_begin(s);
    if (rc != RWY_OK) return rc;
    rc = rwy_bootcfg_make_room(s, RWY_SLOT_META_BLOCKS, 0);
    if (rc != RWY_OK) return rc;   /* the transaction is closed */

    rc = count_and_next_slot(s, slot, sizeof(slot));
    if (rc == RWY_OK) rc = rwy_mkdir(s, slot, RWY_S_IFDIR | 0755, 0, 0);
    if (rc == RWY_OK) rc = populate_type2_slot(s, slot, entry_name, &blob, initial_tries);

    if (rc == RWY_OK) {
        rc = rwy_commit(s);
    } else {
        rwy_abort(s);
    }
    return rc;
}

static int is_booted_slot(rwy_store_t *s, const char *sname) {
    char booted_link[256];
    const char *bname;
    if (rwy_readlink(s, "EFI/RUNWAY/BOOTED", booted_link, sizeof(booted_link)) != RWY_OK) return 0;
    bname = strrchr(booted_link, '/');
    bname = bname ? bname + 1 : booted_link;
    return strcmp(sname, bname) == 0;
}

static int is_last_ok_slot(rwy_store_t *s, const char *sname) {
    char name[256], state_path[300], st[16];
    size_t i = 0, ok_count = 0;
    int target_is_ok = 0;

    while (rwy_readdir_nth(s, "EFI/RUNWAY", i++, name, sizeof(name), NULL) == RWY_OK) {
        if (strncmp(name, "CONFIG", 6) == 0) {
            size_t len = sizeof(st) - 1;
            snprintf(state_path, sizeof(state_path), "EFI/RUNWAY/%s/STATE", name);
            if (rwy_read_path(s, state_path, 0, st, &len) == RWY_OK && len > 0) {
                while (len && (st[len - 1] == '\n' || st[len - 1] == '\r')) len--;
                st[len] = 0;
                if (strcmp(st, "OK") == 0) {
                    ok_count++;
                    if (strcmp(name, sname) == 0) target_is_ok = 1;
                }
            }
        }
    }
    return target_is_ok && ok_count <= 1;
}

int rwy_bls_can_remove_slot(rwy_store_t *s, const char *slot_name) {
    const char *base;
    const char *sname;
    char slot_dir[128];
    rwy_attr_t a;

    if (!s || !slot_name) return RWY_ERR_INVAL;
    base = strrchr(slot_name, '/');
    sname = base ? base + 1 : slot_name;
    snprintf(slot_dir, sizeof(slot_dir), "EFI/RUNWAY/%s", sname);
    if (rwy_stat(s, slot_dir, &a) != RWY_OK) return RWY_ERR_NOENT;

    if (is_booted_slot(s, sname)) return RWY_ERR_BUSY;
    if (is_last_ok_slot(s, sname)) return RWY_ERR_BUSY;
    return RWY_OK;
}

int rwy_bls_remove_slot(rwy_store_t *s, const char *slot_name) {
    const char *base;
    char slot_dir[128];
    int rc;

    rc = rwy_bls_can_remove_slot(s, slot_name);
    if (rc != RWY_OK) return rc;

    rc = rwy_begin(s);
    if (rc != RWY_OK) return rc;

    base = strrchr(slot_name, '/');
    snprintf(slot_dir, sizeof(slot_dir), "EFI/RUNWAY/%s", base ? base + 1 : slot_name);
    rc = rwy_bootcfg_remove_slot(s, slot_dir);

    if (rc == RWY_OK) {
        rc = rwy_commit(s);
    } else {
        rwy_abort(s);
    }
    return rc;
}
