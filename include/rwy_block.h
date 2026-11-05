/*
 * rwy_block — block-device abstraction.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Runway core never knows what backs the store (FAT file, Linux file,
 * raw device, encrypted layer, in-memory test buffer). It only talks
 * through rwy_block_dev_t:
 *
 *   read(offset,size) / write(offset,size) — byte granularity, but all
 *   core I/O is block-aligned 4096 B except the two superblock slots
 *   (also full blocks). Short or torn writes may occur at the backing
 *   layer (especially on FAT); correctness never depends on atomic
 *   multi-block updates — only on single-superblock atomic visibility
 *   plus per-block CRCs.
 *
 * Future encryption lives BELOW this interface (a crypto block device
 * wrapping another block device), never inside the filesystem core.
 * Future TPM key providers supply keys to that layer via
 * rwy_key_provider (declared here, defined later), which the core
 * never touches.
 */
#ifndef RWY_BLOCK_H
#define RWY_BLOCK_H

#include <stddef.h>
#include <stdint.h>

typedef struct rwy_block_dev rwy_block_dev_t;

/* What a core call reads or writes: superblock, bitmap, inode table, records, logs and attributes (meta);
 * directory contents; extent lists (indirect blocks); file data. */
typedef enum { RWY_IO_META, RWY_IO_DIR, RWY_IO_EXTENT, RWY_IO_DATA, RWY_IO_KINDS } rwy_io_kind_t;

/* Size classes of a call: up to 4 KiB, up to 64 KiB, up to 1 MiB, above. */
#define RWY_IO_BUCKETS 4

/* What a device did: calls to the firmware or the kernel, not calls of the core. A backend with a read
 * window counts a hit when a read came from the window and a fill when it had to load it. */
typedef struct {
    uint64_t reads, writes, read_bytes, write_bytes, flushes, window_hits, window_fills;
} rwy_dev_stats_t;

/* Calls the core made to a device. Index 0 is read, 1 is write. */
typedef struct {
    uint64_t calls[2][RWY_IO_KINDS][RWY_IO_BUCKETS];
    uint64_t bytes[2][RWY_IO_KINDS];
    uint64_t flushes;
} rwy_io_stats_t;

typedef int (*rwy_blk_read_fn)(void *ctx, uint64_t offset, void *buf, size_t len);
typedef int (*rwy_blk_write_fn)(void *ctx, uint64_t offset, const void *buf, size_t len);
typedef int (*rwy_blk_flush_fn)(void *ctx);

/* Directory blocks of the store, kept so that a scan of a directory does not read the same block again for
 * every entry. Direct mapped by block number. Any write through the store drops all of it. */
#ifndef RWY_DIRCACHE_BLOCKS
#define RWY_DIRCACHE_BLOCKS 64u
#endif
typedef struct {
    uint32_t tag[RWY_DIRCACHE_BLOCKS];                   /* block number + 1, 0 when empty */
    uint8_t  data[RWY_DIRCACHE_BLOCKS][4096];
} rwy_dircache_t;

struct rwy_block_dev {
    rwy_blk_read_fn  read;
    rwy_blk_write_fn write;
    rwy_blk_flush_fn flush;       /* may be NULL (no-op) */
    void             *ctx;
    uint64_t          size;        /* total bytes (RWY_STORE_SIZE) */
    uint32_t          block_size;  /* RWY_BLOCK_SIZE */
    rwy_io_stats_t   *stats;       /* counts the core calls through this device; rwy_mount() sets it, else NULL */
    rwy_dircache_t   *dircache;    /* directory blocks read through this device; rwy_mount() sets it, else NULL */
};

/* Future key-provider interface (reserved; no implementation yet). */
struct rwy_key_provider {
    int (*get_key)(void *ctx, uint64_t key_id, uint8_t *out, size_t *inout_len);
    void (*release_key)(void *ctx, uint8_t *key, size_t len);
};

/* --- Multi-Device Mirror Block Device --- */
#define RWY_MIRROR_MAX_DEVS 8

#define RWY_MIRROR_OK          0
#define RWY_MIRROR_ERR_INVAL  -1
#define RWY_MIRROR_ERR_IO     -2
#define RWY_MIRROR_ERR_FULL   -3
#define RWY_MIRROR_ERR_NODEV  -4

typedef enum {
    RWY_MIRROR_WRITE_BEST_EFFORT = 0,     /* At least one replica must succeed */
    RWY_MIRROR_WRITE_REQUIRE_PRIMARY = 1, /* Primary must succeed */
    RWY_MIRROR_WRITE_REQUIRE_ALL = 2,     /* All registered replicas must succeed */
} rwy_mirror_write_policy_t;

typedef struct rwy_mirror_dev {
    rwy_block_dev_t            devs[RWY_MIRROR_MAX_DEVS];
    size_t                      num_devs;
    size_t                      primary_idx;    /* Authoritative master index */
    rwy_mirror_write_policy_t  write_policy;

    /* Per-device operational and error counters for testing and diagnostics */
    uint64_t                    read_ops[RWY_MIRROR_MAX_DEVS];
    uint64_t                    read_errors[RWY_MIRROR_MAX_DEVS];
    uint64_t                    write_ops[RWY_MIRROR_MAX_DEVS];
    uint64_t                    write_errors[RWY_MIRROR_MAX_DEVS];
    uint64_t                    flush_ops[RWY_MIRROR_MAX_DEVS];
    uint64_t                    flush_errors[RWY_MIRROR_MAX_DEVS];

    /* Active/faulted flags: 0 = online/healthy, 1 = offline/failed */
    uint8_t                     dev_faulted[RWY_MIRROR_MAX_DEVS];

    /* write_errors already counted as a health event */
    uint64_t                    write_noted[RWY_MIRROR_MAX_DEVS];
} rwy_mirror_dev_t;

int rwy_mirror_init(rwy_mirror_dev_t *mdev);
int rwy_mirror_add(rwy_mirror_dev_t *mdev, const rwy_block_dev_t *dev);
int rwy_mirror_set_primary(rwy_mirror_dev_t *mdev, size_t primary_idx);
int rwy_mirror_set_faulted(rwy_mirror_dev_t *mdev, size_t idx, int faulted);
int rwy_mirror_read(void *ctx, uint64_t offset, void *buf, size_t len);
int rwy_mirror_write(void *ctx, uint64_t offset, const void *buf, size_t len);
int rwy_mirror_flush(void *ctx);
int rwy_mirror_wrap(rwy_block_dev_t *out_dev, rwy_mirror_dev_t *mdev);
/* Give each member its own UUID when it has none or shares one with another member. Writes nothing otherwise. */
/* The member UUID of the newest valid superblock slot of a raw member: 1 and `out` set, or 0 when it has none. */
int rwy_block_member_uuid(const rwy_block_dev_t *d, uint8_t out[16]);
int rwy_mirror_name_members(rwy_mirror_dev_t *mdev, int (*random)(void *buf, size_t len));

static inline int rwy_mirror_dev_wrap(rwy_block_dev_t *out_dev, rwy_mirror_dev_t *mdev) {
    return rwy_mirror_wrap(out_dev, mdev);
}

#endif /* RWY_BLOCK_H */
