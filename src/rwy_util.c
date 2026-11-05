/*
 * rwy_util.c — CRC32C, error strings, string helpers and UUID parsing/formatting.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_internal.h"

static uint32_t crc_tab[256];

static int crc_tab_ready;

static void crc_init(void) {
    if (crc_tab_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
        crc_tab[i] = c;
    }
    crc_tab_ready = 1;
}

uint32_t rwy_crc32c(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    crc_init();
    while (len--) crc = crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

int rwy_pick_newest(unsigned n, rwy_copy_check_fn check, void *ctx, unsigned *best) {
    uint64_t best_seq = 0;
    int io = 0, found = 0;
    if (!check || !best) return RWY_ERR_INVAL;
    for (unsigned i = 0; i < n; i++) {
        uint64_t seq = 0;
        int rc = check(ctx, i, &seq);
        if (rc == RWY_ERR_IO) io = 1;
        if (rc != RWY_OK) continue;
        if (!found || seq > best_seq) {
            best_seq = seq;
            *best = i;
            found = 1;
        }
    }
    if (found) return RWY_OK;
    return io ? RWY_ERR_IO : RWY_ERR_CORRUPT;
}

/* The error codes are 0 and the negative numbers down to RWY_ERR_ROLLBACK, so -rc indexes the table. */
static const char *const err_text[] = {
    [-RWY_OK] = "OK",
    [-RWY_ERR_MAGIC] = "bad magic",
    [-RWY_ERR_VERSION] = "unsupported version",
    [-RWY_ERR_SIZE] = "bad store size",
    [-RWY_ERR_NOENT] = "no such file",
    [-RWY_ERR_EXIST] = "already exists",
    [-RWY_ERR_NOSPC] = "no space",
    [-RWY_ERR_INVAL] = "invalid argument",
    [-RWY_ERR_IO] = "I/O error",
    [-RWY_ERR_NOTDIR] = "not a directory",
    [-RWY_ERR_ISDIR] = "is a directory",
    [-RWY_ERR_NAMETOOLONG] = "name too long",
    [-RWY_ERR_NOTEMPTY] = "directory not empty",
    [-RWY_ERR_FEAT] = "unknown incompatible feature",
    [-RWY_ERR_CORRUPT] = "corrupt metadata",
    [-RWY_ERR_NOTXN] = "no active transaction",
    [-RWY_ERR_XATTR] = "no such extended attribute",
    [-RWY_ERR_RANGE] = "result out of range",
    [-RWY_ERR_LOOP] = "symlink loop",
    [-RWY_ERR_PERM] = "not permitted by EFI/RUNWAY policy",
    [-RWY_ERR_ROLLBACK] = "store rolled back",
    [-RWY_ERR_BUSY] = "device or resource busy",
};

const char *rwy_strerror(int rc) {
    if (rc > 0 || (size_t)-rc >= sizeof(err_text) / sizeof(err_text[0]) || !err_text[-rc])
        return "unknown error";
    return err_text[-rc];
}

size_t rwy_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int rwy_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int rwy_strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

void rwy_format_uuid(const uint8_t uuid[16], char out[37]) {
    static const char hex[] = "0123456789abcdef";
    int j = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[j++] = '-';
        }
        out[j++] = hex[(uuid[i] >> 4) & 0x0F];
        out[j++] = hex[uuid[i] & 0x0F];
    }
    out[j] = '\0';
}

static int hex_char_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int rwy_parse_uuid(const char *in, uint8_t out[16]) {
    if (!in || !out) return RWY_ERR_INVAL;
    int idx = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            if (*in == '-') in++;
        }
        int h = hex_char_val(*in++);
        int l = hex_char_val(*in++);
        if (h < 0 || l < 0) return RWY_ERR_INVAL;
        out[idx++] = (uint8_t)((h << 4) | l);
    }
    if (*in != '\0') return RWY_ERR_INVAL;
    return RWY_OK;
}
