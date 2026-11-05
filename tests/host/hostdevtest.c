/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
/* hostdev with O_DIRECT: unaligned reads and block writes, in a file and in a window, against a model. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "hostdev.h"
#include "testdir.h"

#define FILE_SIZE (3u << 20)
#define WIN_OFF 8192u
#define WIN_LEN (2u << 20)

static unsigned long long rng = 88172645463325252ULL;
static unsigned next(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (unsigned)(rng >> 11);
}

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } } while (0)

/* Random reads and block writes through dev, mirrored on the model of the same size. */
static void exercise(rwy_block_dev_t *d, uint8_t *model, size_t size, const char *what) {
    static uint8_t buf[(3u << 19) + 8];
    for (int i = 0; i < 400; i++) {
        size_t n = (next() % 8 == 0) ? next() % (3u << 19) + 1 : next() % 6000 + 1;
        size_t off = next() % (size - n + 1);
        if (next() % 2) {
            n = (n + 4095) / 4096 * 4096;
            if (n > size) n = size;
            off = off / 4096 * 4096;
            if (off + n > size) off = size - n;
            for (size_t k = 0; k < n; k++) buf[k] = (uint8_t)next();
            if (d->write(d->ctx, off, buf, n)) { printf("FAIL: %s write at %zu len %zu\n", what, off, n); fails++; return; }
            memcpy(model + off, buf, n);
        } else {
            if (d->read(d->ctx, off, buf, n) || memcmp(buf, model + off, n)) {
                printf("FAIL: %s read at %zu len %zu\n", what, off, n); fails++; return;
            }
        }
    }
}

int main(void) {
    const char *path = tpath("direct.img");
    static uint8_t model[FILE_SIZE], disk[FILE_SIZE];
    char spec[4200];
    hostdev_t h;
    rwy_block_dev_t d;
    FILE *f;
    uint8_t one = 0;

    for (size_t i = 0; i < FILE_SIZE; i++) model[i] = (uint8_t)next();
    f = fopen(path, "wb");
    if (!f || fwrite(model, 1, FILE_SIZE, f) != FILE_SIZE) { perror(path); return 1; }
    fclose(f);

    if (hostdev_open(&h, path, O_RDWR | O_DIRECT)) {
        printf("hostdevtest: SKIP (O_DIRECT not available here: %s)\n", strerror(errno));
        return 0;
    }
    CHECK(h.direct && !h.block, "direct flag on a file");
    hostdev_raw_dev(&h, &d);
    CHECK(d.size == FILE_SIZE, "file size");
    exercise(&d, model, FILE_SIZE, "file");
    CHECK(!d.flush(d.ctx), "flush");
    hostdev_close(&h);

    f = fopen(path, "rb");
    CHECK(f && fread(disk, 1, FILE_SIZE, f) == FILE_SIZE && !memcmp(disk, model, FILE_SIZE), "file content after direct writes");
    if (f) fclose(f);

    /* A write that is not whole blocks at block offsets fails, and changes nothing */
    CHECK(!hostdev_open(&h, path, O_RDWR | O_DIRECT), "reopen for the refused writes");
    hostdev_raw_dev(&h, &d);
    CHECK(d.write(d.ctx, 4096, model, 100) != 0, "short write is refused");
    CHECK(d.write(d.ctx, 512, model, 4096) != 0, "write at a 512 byte offset is refused");
    CHECK(d.write(d.ctx, 4096, model, 4096 + 512) != 0, "write of a block and a half is refused");
    CHECK(d.write(d.ctx, 4096, model + 4096, 8192) == 0, "two blocks at a block offset");
    hostdev_close(&h);
    CHECK(!hostdev_open(&h, path, O_RDWR), "reopen without O_DIRECT");
    hostdev_raw_dev(&h, &d);
    CHECK(d.write(d.ctx, 100, model, 4096) != 0, "plain file: write at an odd offset is refused");
    CHECK(d.write(d.ctx, 0, model, 5) != 0, "plain file: short write is refused");
    CHECK(h.stats.writes == 0 && h.stats.write_bytes == 0, "refused writes are not counted");
    CHECK(!d.write(d.ctx, 0, model, 8192) && !d.read(d.ctx, 0, disk, 4096) && !d.flush(d.ctx), "count: write, read, flush");
    CHECK(h.stats.writes == 1 && h.stats.write_bytes == 8192, "counted one write of two blocks");
    CHECK(h.stats.reads == 1 && h.stats.read_bytes == 4096 && h.stats.flushes == 1, "counted a read and a flush");
    hostdev_close(&h);
    f = fopen(path, "rb");
    CHECK(f && fread(disk, 1, FILE_SIZE, f) == FILE_SIZE && !memcmp(disk, model, FILE_SIZE),
          "refused writes left the file alone");
    if (f) fclose(f);

    /* A window that does not start or end on a block is refused */
    snprintf(spec, sizeof(spec), "%s@%u+%u", path, WIN_OFF + 512, WIN_LEN);
    CHECK(hostdev_open(&h, spec, O_RDWR) != 0, "window start off the block size is refused");
    snprintf(spec, sizeof(spec), "%s@%u+%u", path, WIN_OFF, WIN_LEN + 512);
    CHECK(hostdev_open(&h, spec, O_RDWR) != 0, "window length off the block size is refused");

    /* A window: writes stay in it */
    snprintf(spec, sizeof(spec), "%s@%u+%u", path, WIN_OFF, WIN_LEN);
    if (hostdev_open(&h, spec, O_RDWR | O_DIRECT)) { perror(spec); return 1; }
    hostdev_raw_dev(&h, &d);
    CHECK(d.size == WIN_LEN, "window size");
    exercise(&d, model + WIN_OFF, WIN_LEN, "window");
    CHECK(d.write(d.ctx, WIN_LEN - 1, &one, 2) != 0, "write past the window");
    CHECK(d.read(d.ctx, WIN_LEN, &one, 1) != 0, "read past the window");
    hostdev_close(&h);

    f = fopen(path, "rb");
    CHECK(f && fread(disk, 1, FILE_SIZE, f) == FILE_SIZE && !memcmp(disk, model, FILE_SIZE), "image after window writes");
    if (f) fclose(f);

    /* A file opened without O_DIRECT keeps its plain path */
    CHECK(!hostdev_open(&h, path, O_RDONLY) && !h.direct, "plain open is not direct");
    hostdev_close(&h);

    if (fails) return 1;
    printf("hostdevtest: OK\n");
    return 0;
}
