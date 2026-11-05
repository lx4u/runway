/*
 * blobtest.c — Unit tests for the content name of a blob (rwy_blob_*).
 * SPDX-License-Identifier: Apache-2.0
 *
 * The same vectors run through the built-in code and through OpenSSL.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "testdir.h"
#include "crypto_openssl.h"
#include "rwy_blob.h"
#include "rwy_core.h"
#include "rwy_crypto.h"

#define MIB (1U << 20)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

/* A byte pattern that changes in the low, middle and high bytes of the offset */
static uint8_t pattern(size_t i) {
    return (uint8_t)(i ^ (i >> 8) ^ (i >> 16));
}

/* Make a file of len bytes: data if given, else the pattern */
static const char *make_file(const char *name, size_t len, const char *data) {
    const char *path = tpath(name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    uint8_t *buf = malloc(len ? len : 1);

    CHECK(fd >= 0 && buf, "make test file");
    for (size_t i = 0; i < len; i++) buf[i] = data ? (uint8_t)data[i] : pattern(i);
    CHECK((size_t)write(fd, buf, len) == len, "write test file");
    close(fd);
    free(buf);
    return path;
}

static void check_file(const char *what, const char *path, const char *want) {
    uint8_t d[32];
    char name[RWY_BLOB_NAME_SIZE];
    int fd = open(path, O_RDONLY);

    CHECK(fd >= 0 && rwy_blob_hash_fd(fd, d) == RWY_OK, what);
    rwy_blob_name(d, name);
    if (strcmp(name, want)) {
        printf("FAIL: %s: got %s, want %s\n", what, name, want);
        failures++;
    }
    close(fd);
}

static void run_vectors(const char *backend) {
    printf("--- Blob names: %s ---\n", backend);
    check_file("empty file", make_file("empty", 0, NULL),
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_file("abc", make_file("abc", 3, "abc"),
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_file("one chunk", make_file("chunk", MIB, NULL),
               "9a058339229372b03c3b56553873e3681bb2ec068f7b9f08d7d6c9dd93157cbd");
    check_file("one chunk and a byte", make_file("chunk1", MIB + 1, NULL),
               "c422bdfb339517edba3eb3110e631b9a93991dbdb2ee3ca1ddb91948fe210baf");
    check_file("16 MiB", make_file("large", 16 * (size_t)MIB, NULL),
               "0afe2536a8655704beed830075f66297e104e974b469956893f08a8e29436f1b");
}

static void test_errors(void) {
    uint8_t d[32];
    int dirfd = open(tpath("."), O_RDONLY);

    printf("--- Blob names: errors ---\n");
    CHECK(rwy_blob_hash_fd(-1, d) == RWY_ERR_INVAL, "bad descriptor");
    CHECK(rwy_blob_hash_fd(dirfd, d) == RWY_ERR_IO, "a directory is not hashed");
    close(dirfd);
}

int main(void) {
    rwy_crypto_set_ops(NULL);
    run_vectors("built-in");
    rwy_crypto_set_ops(&rwy_crypto_openssl_ops);
    run_vectors("OpenSSL");
    test_errors();
    printf(failures ? "FAILED: %d\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
