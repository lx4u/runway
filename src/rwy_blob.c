/*
 * rwy_blob.c — Content names of blobs.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-only code.
 */
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include "rwy_blob.h"
#include "rwy_core.h"
#include "rwy_crypto.h"

#define BLOB_CHUNK (1U << 20)

int rwy_blob_hash_fd(int fd, uint8_t digest[32]) {
    rwy_sha256_stream_t s;
    uint8_t *buf;
    off_t off = 0;
    int rc = RWY_OK;

    if (fd < 0 || !digest) return RWY_ERR_INVAL;
    buf = malloc(BLOB_CHUNK);
    if (!buf) return RWY_ERR_IO;
    if (rwy_sha256_stream_init(&s) != RWY_CRYPTO_OK) rc = RWY_ERR_IO;
    while (rc == RWY_OK) {
        ssize_t n = pread(fd, buf, BLOB_CHUNK, off);

        if (n < 0 && errno == EINTR) continue;
        if (n < 0) rc = RWY_ERR_IO;
        if (n <= 0) break;
        rwy_sha256_stream_update(&s, buf, (size_t)n);
        off += n;
    }
    rwy_sha256_stream_final(&s, digest);
    free(buf);
    return rc;
}

int rwy_blob_hash_path(rwy_store_t *st, const char *path, uint8_t digest[32]) {
    rwy_sha256_stream_t s;
    uint8_t *buf;
    uint64_t off = 0;
    int rc = RWY_OK;

    if (!st || !path || !digest) return RWY_ERR_INVAL;
    buf = malloc(BLOB_CHUNK);
    if (!buf) return RWY_ERR_IO;
    if (rwy_sha256_stream_init(&s) != RWY_CRYPTO_OK) rc = RWY_ERR_IO;
    while (rc == RWY_OK) {
        size_t n = BLOB_CHUNK;

        rc = rwy_read_path(st, path, off, buf, &n);
        if (rc != RWY_OK || n == 0) break;
        rwy_sha256_stream_update(&s, buf, n);
        off += n;
    }
    rwy_sha256_stream_final(&s, digest);
    free(buf);
    return rc;
}

void rwy_blob_name(const uint8_t digest[32], char out[RWY_BLOB_NAME_SIZE]) {
    static const char hex[] = "0123456789abcdef";

    for (int i = 0; i < 32; i++) {
        out[2 * i]     = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15];
    }
    out[RWY_BLOB_NAME_LEN] = '\0';
}
