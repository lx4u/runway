/*
 * Runway on-disk format v5 — transactional CoW with atomic root publication.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Design invariants (must hold for every committed generation):
 *
 *  I1. A store holds exactly two superblock slots: block 0 (slot A) and
 *      block 1 (slot B). Every other block is either metadata, file data,
 *      directory data, or free.
 *  I2. Each superblock slot is self-validating: magic + version + geometry
 *      + generation + root pointers + feature flags + CRC32C over the
 *      superblock prefix. A slot with a bad magic, version, geometry or CRC
 *      is ignored (treated as invalid, never as committed state).
 *  I3. Mount selects the highest-generation VALID slot. All readers and the
 *      next transaction start from that generation. An interrupted commit
 *      leaves at most one slot torn; the other slot still describes the
 *      previous committed state, so the visible filesystem is always either
 *      the old or the new state — never a mixture.
 *  I4. Committed metadata is never mutated in place. A transaction writes
 *      new data blocks, then a new inode table image and a new allocation
 *      bitmap image into the inactive image area, flushes them, then
 *      publishes a new superblock generation in the alternate slot and
 *      flushes again. Publication is the single atomic visibility point.
 *  I5. Directory entries reference stable logical object IDs (OIDs), never
 *      physical block numbers. The inode table maps OID -> inode. OIDs are
 *      never reused within a generation; freed OIDs go to a free list.
 *  I6. Inode table blocks and bitmap blocks carry a block header
 *      (magic + generation + index + CRC) so torn writes are detectable
 *      even when the superblock that points at them is intact.
 *  I7. File and directory data blocks are always written fresh (CoW);
 *      committed data blocks are never overwritten. Hence data blocks need
 *      no per-block CRC: a crash before publication leaves them simply
 *      unreachable (reported by `fsck.runway` as transaction leftovers).
 *  I8. Unknown incompatible feature bits refuse to mount. Unknown
 *      compatible bits are ignored (forward compatibility for e.g. extra
 *      xattr kinds). Reserved fields are zero on write and ignored on read.
 *  I9. Two image areas follow the superblock slots, each sized for one inode
 *      table image and one bitmap image (both sizes are known when the store
 *      is made). The superblock points into the active area; a commit writes
 *      the other one. A commit therefore allocates no block for metadata and
 *      cannot fail for lack of free data blocks. The data pool starts after
 *      the areas and the state record (`pool_start`); the bitmap keeps every block
 *      below it set.
 *  I10. RWY_RESERVE_BLOCKS data blocks stay free for the paths that must always
 *      work: selection and condemn commits, writes of STATE, TRIES and
 *      RECOVER_NEXT in EFI/RUNWAY, and the operations that free space. Everything
 *      else gets NOSPC once the free blocks would drop under the reserve. The
 *      reserve is not stored: it is the same constant for every store.
 *  I11. Two blocks after the image areas hold the state record. They are
 *      written in place, never by a commit: a block has the common header
 *      (kind RWY_BLK_KIND_RECORD, index = copy number, generation = the
 *      record sequence) and an opaque payload of RWY_REC_PAYLOAD bytes. A
 *      write goes to the copy that is not current and is flushed; mount
 *      takes the valid copy with the higher sequence; with none it uses an
 *      empty record and reports it (rwy_record_damaged). A record write allocates nothing, so it needs no free
 *      block and no free inode. A torn write damages only the copy that was
 *      not current.
 *  I12. The recovery area follows the state record: two copies of recovery_blocks
 *      blocks, only on an encrypted store. Its geometry is written once by mkstore and
 *      never changes, so a change of its content never writes the superblock. The
 *      blocks stay set in the bitmap and are never allocated. The area is plaintext
 *      (outside the encrypted range) and has no filesystem. A copy is one header
 *      block (rwy_ra_header_t, CRC32C in its last 4 bytes) followed by block-aligned
 *      payloads (KERNEL, INITRD, DTB, ARGS) that the header lists with a SHA-256 each.
 *      The valid copy with the higher sequence wins; the header is written last.
 *
 * Space reclamation (GC) needs no format change and adds no invariant:
 * each commit already leaves its predecessor's table/bitmap images
 * unreachable; a collector rebuilds the allocation bitmap from the
 * reachable set (live inodes' extents + the new images) and publishes it
 * as a fresh generation through I4. A failed collection is therefore
 * indistinguishable from a crash: more marked-but-unreachable blocks,
 * never lost data. See rwy_core.h (rwy_gc) for the crash-safety rule.
 * Checker `--repair` needs no format change either: it publishes a
 * GC-equivalent generation and only on generations the checker already
 * deems consistent apart from marked-but-unreachable blocks, so no new
 * invariant is introduced (a no-op repair with zero leftovers publishes
 * nothing at all).
 * Offline compaction (`rwy_defrag()` in the core, `fsck.runway --defrag`
 * in the checker) also requires no format change: it relocates live data
 * blocks into single contiguous runs and rebuilds table+bitmap images
 * in a single combined pass, publishing one atomic generation.
 *
 * Intended scale: stores up to 4095 MiB (32-bit block indexes are ample
 * at 4 KiB blocks), individual files up to 256 MiB. A 256 MiB file is an
 * extremely large object here (roughly a very large UKI); the workload
 * is a handful of large UKIs plus EFI/update files, so a simple
 * stop-the-world mark-and-rebuild collector and offline compactor is
 * the right trade.
 *
 * Fragmentation and compaction: large files prefer single runs, so an
 * uncompacted store with scattered free blocks can report NOSPC while free
 * blocks remain. GC reclaims unreachable
 * blocks without relocating live data; offline compaction (`rwy_defrag` /
 * `fsck.runway --defrag`) repacks live multi-extent data into minimal contiguous
 * runs and consolidates free space in a single atomic generation.
 *
 * Layout:
 *
 *   block 0        superblock slot A (4096 B, only first 256 B defined)
 *   block 1        superblock slot B
 *   block 2..      image area 0: inode table image, then bitmap image
 *   block 2+N..    image area 1, same shape (N = area_blocks)
 *   rec_start      state record copy 0 (rec_start = 2 + 2N)
 *   rec_start+1    state record copy 1
 *   flight_start   flight area: copy A, then copy B (flight_blocks each; none when 0)
 *   journal_start  journal area: copy A, then copy B (journal_blocks each; none when 0)
 *   recovery_start recovery area: copy A, then copy B (recovery_blocks each; none when 0)
 *   loader_start   loader area: state record copy 0, copy 1, then bundle copies A and B
 *                  (loader_blocks each; none when 0)
 *   net_start      plaintext settings area: copy A, then copy B (net_blocks each; 0 = none)
 *   pool_start..   data pool (file data, directory data, indirect and xattr
 *                  blocks), managed by the allocation bitmap.
 *                  Bit i of the bitmap governs block i (every bit below
 *                  pool_start is always set). The superblock points at the
 *                  table and bitmap images of the active area.
 *
 * All multi-byte integers are little-endian. Structs are packed.
 * Block size is fixed at 4096 for this milestone (field exists for future
 * negotiation; mount rejects any other value as incompatible).
 */

#ifndef RWY_FORMAT_H
#define RWY_FORMAT_H

#include <stdint.h>

#define RWY_MIN_STORE_SIZE   (8ULL * 1024 * 1024)       /* 8 MiB */
#define RWY_MAX_STORE_SIZE   (4095ULL * 1024 * 1024)    /* 4,095 MiB FAT32 limit */
#define RWY_DEFAULT_STORE_SIZE (268435456ULL)           /* 256 MiB default */
#define RWY_STORE_SIZE       RWY_DEFAULT_STORE_SIZE
#define RWY_BLOCK_SIZE       (4096U)
#define RWY_MAX_BLOCKS       ((uint32_t)(RWY_MAX_STORE_SIZE / RWY_BLOCK_SIZE)) /* 1048320 */
#define RWY_TOTAL_BLOCKS     ((uint32_t)(RWY_STORE_SIZE / RWY_BLOCK_SIZE))     /* 65536 */
#define RWY_VERSION           (11U)  /* v11: 512 inodes / 35 inode table blocks */

/* Superblock slots (block indexes). */
#define RWY_SB_SLOT_A        (0U)
#define RWY_SB_SLOT_B        (1U)
#define RWY_SB_BLOCKS        (2U)   /* the two slots; the first image area follows */
#define RWY_REC_BLOCKS       (2U)   /* the two copies of the state record, after the image areas */

/* Data blocks only the state-change paths may take (I10): a store never fills up to
 * the point where a state change, or a deletion, cannot be committed. */
#define RWY_RESERVE_BLOCKS   (64U)

/* Object model limits. */
#define RWY_MAX_INODES       (512U)  /* fixed table size this milestone */
#define RWY_INODE_SIZE       (256U)
/* Each inode-table block reserves 32 B for rwy_blk_header_t; the payload
 * holds exactly 15 inodes (3840 B) + 224 B zero pad. */
#define RWY_INODES_PER_BLOCK (15U)
#define RWY_INODE_TBL_BLOCKS \
    ((RWY_MAX_INODES + RWY_INODES_PER_BLOCK - 1) / RWY_INODES_PER_BLOCK) /* 35 */
#define RWY_BITMAP_BLOCKS \
    ((RWY_TOTAL_BLOCKS / 8 + (RWY_BLOCK_SIZE - 32) - 1) / (RWY_BLOCK_SIZE - 32)) /* 3 */

/* Superblock magic: "RUNWAYSB". */
#define RWY_SB_MAGIC_0 'R'
#define RWY_SB_MAGIC_1 'U'
#define RWY_SB_MAGIC_2 'N'
#define RWY_SB_MAGIC_3 'W'
#define RWY_SB_MAGIC_4 'A'
#define RWY_SB_MAGIC_5 'Y'
#define RWY_SB_MAGIC_6 'S'
#define RWY_SB_MAGIC_7 'B'
#define RWY_SB_MAGIC   "RUNWAYSB"

/* Metadata block header magic "RUNWAYB1". */
#define RWY_BLK_MAGIC 0x31425941574e5552uLL  /* LE bytes of "RUNWAYB1" */

/* Inode types. */
#define RWY_TYPE_FREE    (0U)
#define RWY_TYPE_FILE    (1U)
#define RWY_TYPE_DIR     (2U)
#define RWY_TYPE_SYMLINK (3U)

/* POSIX file-type bits preserved in mode (standard octal). */
#define RWY_S_IFMT   0170000U
#define RWY_S_IFREG  0100000U
#define RWY_S_IFDIR  0040000U
#define RWY_S_IFLNK  0120000U
#define RWY_S_ISUID  0004000U
#define RWY_S_ISGID  0002000U
#define RWY_S_ISVTX  0001000U

/* Feature flags: incompatible bits refuse mount if unknown. */
#define RWY_FEAT_INCOMPAT_MASK_KNOWN (0U) /* no incompat features in v2 */
#define RWY_FEAT_COMPAT_XATTR        (1U << 0) /* xattr_root may be set */
#define RWY_FEAT_COMPAT_SNAPSHOT     (1U << 1) /* snapshot refs may exist */

/* Encryption algorithms */
#define RWY_CRYPT_NONE        (0U)
#define RWY_CRYPT_AES_XTS_256 (1U)

/* Keyslot metadata layout (located in superblock slots at offset RWY_KEYSLOT_OFFSET). */
#define RWY_KEYSLOT_OFFSET        512U
#define RWY_KEYSLOT_MAGIC         0x314b5941574e5552uLL /* "RUNWAYK1" in LE */
#define RWY_KEYSLOT_COUNT         3U

#define RWY_KEYSLOT_TYPE_PASSWORD 0U /* Slot 0: Password / Transport Key */
#define RWY_KEYSLOT_TYPE_TPM2     1U /* Slot 1: TPM 2.0 / TCG2 Sealed Blob */
#define RWY_KEYSLOT_TYPE_RECOVERY 2U /* Slot 2: Linux / Recovery Key */

#define RWY_KEYSLOT_STATE_EMPTY   0U
#define RWY_KEYSLOT_STATE_ACTIVE  1U
#define RWY_KEYSLOT_STATE_REVOKED 2U

#define RWY_KDF_NONE              0U
#define RWY_KDF_PBKDF2_SHA256     1U

#define RWY_KEY_SALT_SIZE         32U
#define RWY_KEY_BLOB_SIZE         64U   /* 32B encrypted DEK + 32B HMAC-SHA256 */

/* Default POSIX metadata for EFI-created objects (§17). */
#define RWY_EFI_DEFAULT_MODE_FILE 0644U
#define RWY_EFI_DEFAULT_MODE_DIR  0755U
#define RWY_EFI_DEFAULT_UID 0U
#define RWY_EFI_DEFAULT_GID 0U

#pragma pack(push, 1)

/* Superblock: exactly one block (4096 B). CRC covers bytes [0, crc_off). */
typedef struct {
    char     magic[8];            /* "RUNWAYSB" */
    uint32_t version;            /* RWY_VERSION */
    uint32_t block_size;         /* RWY_BLOCK_SIZE */
    uint32_t total_blocks;       /* RWY_TOTAL_BLOCKS */
    uint32_t max_inodes;         /* RWY_MAX_INODES */
    uint64_t total_size;         /* RWY_STORE_SIZE */
    uint64_t generation;         /* monotonic commit id, >= 1 */
    uint64_t root_oid;           /* always 1 */
    uint32_t inode_tbl_start;    /* block index of inode table image (in the active image area) */
    uint32_t inode_tbl_blocks;   /* RWY_INODE_TBL_BLOCKS */
    uint32_t bitmap_start;       /* block index of bitmap image (in the active image area) */
    uint32_t bitmap_blocks;      /* RWY_BITMAP_BLOCKS */
    uint32_t data_blocks;        /* data pool block count */
    uint32_t feat_compat;        /* compatible feature bits */
    uint32_t feat_incompat;      /* incompatible feature bits */
    uint32_t crypt_algo;         /* RWY_CRYPT_NONE (reserved) */
    uint32_t auth_algo;          /* reserved */
    uint64_t key_id;             /* reserved */
    uint64_t key_gen;            /* reserved */
    uint64_t snapshot_gen;       /* reserved: newest snapshot generation or 0 */
    uint64_t snapshot_root;      /* reserved: snapshot root OID or 0 */
    uint8_t  uuid[16];           /* 128-bit filesystem / array UUID */
    uint32_t area_blocks;        /* blocks of one image area: inode table + bitmap */
    uint32_t pool_start;         /* first data block: after the recovery area */
    uint32_t rec_start;          /* first block of the state record: after the two image areas */
    uint32_t recovery_start;     /* first block of recovery copy A: after the state record */
    uint32_t recovery_blocks;    /* blocks of one recovery copy; 0 = no area. Copy B follows copy A */
    uint8_t  member_uuid[16];    /* this copy of a mirror; zero = not named yet. Differs between members */
    uint32_t flight_start;       /* first block of flight copy A */
    uint32_t flight_blocks;      /* blocks per flight copy; 0 = absent */
    uint32_t journal_start;      /* first block of journal copy A */
    uint32_t journal_blocks;     /* blocks per journal copy; 0 = absent */
    uint32_t loader_start;       /* first block of loader area: state record copy 0 */
    uint32_t loader_blocks;      /* blocks per bundle copy; 0 = absent */
    uint32_t net_start;          /* first block of settings copy A */
    uint32_t net_blocks;         /* blocks per settings copy; 0 = absent */
    uint32_t crc32c;             /* CRC32C (Castagnoli) of bytes [0, off) */
    uint8_t  reserved[3892];     /* zero; 4096 - 204 */
} rwy_superblock_t;

/* Keyslot metadata entry (256 bytes). */
typedef struct {
    uint32_t slot_type;      /* RWY_KEYSLOT_TYPE_* */
    uint32_t state;          /* RWY_KEYSLOT_STATE_* */
    uint32_t kdf_type;       /* RWY_KDF_PBKDF2_SHA256 */
    uint32_t kdf_iterations; /* iteration count for KDF */
    uint8_t  salt[RWY_KEY_SALT_SIZE]; /* salt for PBKDF2 */
    uint8_t  wrapped_key[RWY_KEY_BLOB_SIZE]; /* 32B encrypted DEK + 32B HMAC-SHA256 */
    uint8_t  pcr_mask[32];   /* PCR mask / policy binding for TPM2 */
    uint32_t blob_len;       /* length of TPM2 sealed blob */
    uint8_t  blob[364];      /* TPM2 sealed blob or auxiliary key data; pads slot to 512 B */
} rwy_keyslot_t;

/* Keyslot header (located in superblock slot at offset RWY_KEYSLOT_OFFSET; 1600 bytes). */
typedef struct {
    uint64_t       magic;       /* RWY_KEYSLOT_MAGIC */
    uint32_t       version;     /* 1 */
    uint32_t       crypt_algo;  /* RWY_CRYPT_AES_XTS_256 */
    uint32_t       slot_count;  /* RWY_KEYSLOT_COUNT (3) */
    uint32_t       crc32c;      /* CRC32C of header with crc32c field = 0 */
    uint8_t        reserved[40];/* pads header prefix to 64 bytes */
    rwy_keyslot_t slots[RWY_KEYSLOT_COUNT]; /* 3 * 512 = 1536 bytes; total = 1600 bytes */
} rwy_keyslot_header_t;

/* Per-metadata-block header. Each inode-table block and bitmap block
 * begins with this header; payload follows. CRC covers the whole block
 * with crc32c field zeroed. */
typedef struct {
    uint64_t magic;              /* RWY_BLK_MAGIC */
    uint64_t generation;         /* SB generation this block belongs to */
    uint32_t index;              /* block position within its image */
    uint32_t kind;               /* 1 = inode table, 2 = bitmap, 3 = indirect */
    uint32_t crc32c;             /* over full 4096 B block, this field = 0 */
    uint32_t reserved;
} rwy_blk_header_t;

#define RWY_BLK_KIND_INODE   (1U)
#define RWY_BLK_KIND_BITMAP  (2U)
#define RWY_BLK_KIND_INDIR   (3U)
#define RWY_BLK_KIND_XATTR   (4U)
#define RWY_BLK_KIND_RECORD  (5U)
#define RWY_BLK_KIND_FLIGHT  (6U)
#define RWY_BLK_KIND_JOURNAL (7U)
#define RWY_BLK_KIND_LOG     (8U)
/* Payload of one state record copy: the block minus its header. */
#define RWY_REC_PAYLOAD     (RWY_BLOCK_SIZE - sizeof(rwy_blk_header_t))

/* Flat bundle definitions (shared by recovery area and loader area).
 * A copy starts with a header block; CRC32C in the last 4 bytes covers
 * bytes [0, 4092). Offsets are bytes from the start of the copy, block aligned. */
#define RWY_BUNDLE_VERSION      (1U)
#define RWY_BUNDLE_CRC_OFFSET   (RWY_BLOCK_SIZE - 4U)
#define RWY_BUNDLE_MAX_ENTRIES  (16U)

#define RWY_LOADER_MAGIC        0x314c5941574e5552uLL   /* "RUNWAYL1" in LE */
#define RWY_ENTRY_LOADER        (0U)
#define RWY_ENTRY_WDT           (1U)

typedef struct {
    uint32_t kind;               /* entry kind */
    uint32_t reserved;           /* zero */
    uint64_t offset;             /* bytes from the start of the copy */
    uint64_t length;             /* bytes, at least 1 */
    uint8_t  sha256[32];         /* of the length bytes */
} rwy_bundle_entry_t;

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t count;
    uint64_t sequence;
    rwy_bundle_entry_t entry[RWY_BUNDLE_MAX_ENTRIES];
} rwy_bundle_header_t;

/* Loader area state record (I13). Two copies at loader_start and loader_start + 1. */
#define RWY_LOADER_RECORD_VERSION   (1U)
#define RWY_LOADER_TIMEOUT_DEFAULT  (300U)

/* Failure reason codes for rwy_loader_record_t. */
#define RWY_LOADER_REASON_NONE             (0U)
#define RWY_LOADER_REASON_BAD_HEADER       (1U)
#define RWY_LOADER_REASON_BAD_RANGE        (2U)
#define RWY_LOADER_REASON_BAD_HASH         (3U)
#define RWY_LOADER_REASON_START_FAILED     (4U)
#define RWY_LOADER_REASON_TRIES_EXHAUSTED  (5U)
#define RWY_LOADER_REASON_RECORD_IO        (6U)

#define RWY_LOADER_COPY_A                  (0U)
#define RWY_LOADER_COPY_B                  (1U)

#define RWY_LOADER_TRIAL_NONE              (0U)
#define RWY_LOADER_TRIAL_A                 (1U)
#define RWY_LOADER_TRIAL_B                 (2U)

#define RWY_LOADER_BAD_A                   (1U << 0)
#define RWY_LOADER_BAD_B                   (1U << 1)

#define RWY_LOADER_HANDSHAKE_VAR           L"RunwayLoaderHandshake"
#define RWY_BOOT_VAR_GUID \
    { 0xb371a076, 0xb7ef, 0x4e6e, { 0xb9, 0x8a, 0x8d, 0x0d, 0x2c, 0x49, 0x3c, 0x7b } }

typedef struct {
    uint8_t  copy;          /* 0 = A, 1 = B */
    uint8_t  trial;         /* 0 = normal, 1 = trial */
    uint16_t reserved;      /* zero padding */
    uint32_t timeout;       /* seconds of the watchdog during trial */
    char     wdt_armed[32]; /* name of WDT driver that armed, or empty */
} rwy_loader_handshake_t;

typedef struct {
    uint64_t magic;          /* RWY_BLK_MAGIC ("RUNWAYB1") */
    uint64_t sequence;       /* monotonic sequence, highest valid wins */
    uint32_t version;        /* RWY_LOADER_RECORD_VERSION (1) */
    uint8_t  good;           /* 0 = A, 1 = B: the last confirmed loader */
    uint8_t  trial;          /* 0 = none, 1 = A, 2 = B: pending trial copy */
    uint8_t  tries;          /* boots left for the trial copy */
    uint8_t  bad;            /* bit 0 = A bad, bit 1 = B bad */
    uint32_t timeout;        /* seconds of the watchdog during trial (default 300) */
    uint32_t reason;         /* last failure reason code (for diagnosis) */
    char     wdt_armed[32];  /* name of WDT driver that armed, or empty */
    uint8_t  reserved[4028]; /* zero padding up to byte 4092 */
    uint32_t crc32c;         /* CRC32C of bytes [0, 4092) */
} rwy_loader_record_t;

/* Recovery area bundle (I12). */
#define RWY_RA_MAGIC        0x31525941574e5552uLL   /* "RUNWAYR1" in LE */
#define RWY_RA_VERSION      RWY_BUNDLE_VERSION
#define RWY_RA_KERNEL       (0U)
#define RWY_RA_INITRD       (1U)
#define RWY_RA_DTB          (2U)
#define RWY_RA_ARGS         (3U)
#define RWY_RA_KINDS        (4U)
#define RWY_RA_ARGS_MAX     (4096U)
#define RWY_RA_CRC_OFFSET   RWY_BUNDLE_CRC_OFFSET

typedef rwy_bundle_entry_t rwy_ra_entry_t;

typedef struct {
    uint64_t magic;              /* RWY_RA_MAGIC */
    uint32_t version;            /* RWY_RA_VERSION */
    uint32_t count;              /* entries used, 1 to RWY_RA_KINDS; KERNEL is required */
    uint64_t sequence;           /* the higher valid copy wins */
    rwy_ra_entry_t entry[RWY_RA_KINDS];
} rwy_ra_header_t;

/* Flight recorder area (diagnostic boot log). */
#define RWY_FLIGHT_MAGIC     RWY_BLK_MAGIC
#define RWY_FLIGHT_VERSION   (1U)

#define RWY_FLIGHT_EVENT_NONE         (0U)
#define RWY_FLIGHT_EVENT_BOOT         (1U)
#define RWY_FLIGHT_EVENT_CONDEMN      (2U)
#define RWY_FLIGHT_EVENT_CONFIRM      (3U)
#define RWY_FLIGHT_EVENT_FAIL         (4U)
#define RWY_FLIGHT_EVENT_TRIES        (5U)
#define RWY_FLIGHT_EVENT_RECOVER_NEXT (6U)
#define RWY_FLIGHT_EVENT_LOOP_LIMIT   (7U)
#define RWY_FLIGHT_EVENT_STORE        (8U)   /* the boot store cannot be mounted; kept in memory only */

#define RWY_SS_REASON_NONE          (0U)
#define RWY_SS_REASON_NORMAL        (1U)
#define RWY_SS_REASON_REQUESTED     (2U)
#define RWY_SS_REASON_ALL_FAILED    (3U)
#define RWY_SS_REASON_INVALID_STORE (4U)
#define RWY_SS_REASON_BOOT_LOOP     (5U)
#define RWY_SS_REASON_LAST          RWY_SS_REASON_BOOT_LOOP

typedef struct {
    rwy_blk_header_t hdr;          /* magic RWY_BLK_MAGIC, kind RWY_BLK_KIND_FLIGHT */
    uint32_t          count;        /* entries currently stored in ring */
    uint32_t          head;         /* index where next entry will be written */
    uint32_t          capacity;     /* maximum entries that fit in this copy */
    uint8_t           reserved[20]; /* zero; pads header to 64 bytes */
} rwy_flight_header_t;

typedef struct {
    uint64_t seq;        /* monotonic event sequence */
    uint64_t generation; /* tree generation */
    uint64_t rec_seq;    /* state record sequence binding */
    uint64_t timestamp;  /* timestamp or EFI timer ticks */
    char     booted[22]; /* slot name (CONFIG<n>, RECOVERY) */
    uint8_t  event;      /* RWY_FLIGHT_EVENT_* */
    uint8_t  reason;     /* RWY_SS_REASON_* */
    uint32_t epoch;      /* TPM NV anchor epoch */
    uint32_t rev_hw;     /* hardware revision */
} rwy_flight_entry_t;

/* Update journal area (encrypted rolling audit ring). */
#define RWY_JOURNAL_MAGIC     RWY_BLK_MAGIC
#define RWY_JOURNAL_VERSION   (1U)

#define RWY_JOURNAL_EVENT_NONE    (0U)
#define RWY_JOURNAL_EVENT_INSTALL (1U)
#define RWY_JOURNAL_EVENT_UPDATE  (2U)
#define RWY_JOURNAL_EVENT_CONFIRM (3U)
#define RWY_JOURNAL_EVENT_FAIL    (4U)
#define RWY_JOURNAL_EVENT_REMOVE  (5U)
#define RWY_JOURNAL_EVENT_EVICT   (6U)

/* EVICT keeps its details in the entry's reserved bytes: the state the slot had (RWY_SS_*), the reason
 * (RWY_EVICT_REASON_*) and the data blocks the removal freed (little endian). */
#define RWY_EVICT_STATE_OFF   (0U)
#define RWY_EVICT_REASON_OFF  (1U)
#define RWY_EVICT_BLOCKS_OFF  (2U)
#define RWY_EVICT_REASON_QUOTA      (1U)
#define RWY_EVICT_REASON_SLOTS      (2U)
#define RWY_EVICT_REASON_INCOMPLETE (3U)

typedef struct {
    rwy_blk_header_t hdr;          /* magic RWY_BLK_MAGIC, kind RWY_BLK_KIND_JOURNAL */
    uint32_t          count;        /* entries currently stored in ring */
    uint32_t          head;         /* index where next entry will be written */
    uint32_t          capacity;     /* maximum entries that fit in this copy */
    uint8_t           head_hash[32];/* SHA-256 hash of the newest entry (entry_hash) */
    uint8_t           reserved[180];/* pads header to 256 bytes */
} rwy_journal_header_t;

typedef struct {
    uint64_t seq;              /* monotonic log index */
    uint64_t generation;       /* filesystem tree generation */
    uint64_t rec_seq;          /* state record sequence */
    uint8_t  prev_hash[32];    /* SHA-256 of preceding entry */
    uint8_t  event;            /* RWY_JOURNAL_EVENT_* */
    char     slot[15];         /* slot name (CONFIG<n>, RECOVERY) */
    uint64_t revision;         /* slot revision */
    char     label[129];       /* null-terminated copy of slot label */
    uint8_t  payload_hash[32]; /* SHA-256 digest over slot files */
    uint8_t  store_uuid[16];   /* filesystem UUID */
    uint64_t epoch;            /* anchor epoch at time of log */
    uint64_t timestamp;        /* timestamp */
    char     caller[16];       /* "rwyctl", "efi", etc. */
    uint8_t  reserved[63];     /* zero padding to 352 */
    uint8_t  entry_hash[32];   /* SHA-256 over bytes [0, 352) */
} rwy_journal_entry_t;

/* Plaintext network report settings area. Each copy starts with a block header (kind RWY_BLK_KIND_LOG,
 * index = copy, generation = sequence). The higher valid sequence wins. */
#define RWY_LOG_VERSION         (1U)
#define RWY_LOG_SERVER_PORT     (514U)
#define RWY_LOG_TIMEOUT_DEFAULT (5U)
#define RWY_LOG_TIMEOUT_MIN     (1U)
#define RWY_LOG_TIMEOUT_MAX     (30U)
#define RWY_LOG_BACKLOG_DEFAULT (8U)
#define RWY_LOG_BACKLOG_MAX     (32U)

#define RWY_LOG_SET_SERVER   (1U << 0)
#define RWY_LOG_SET_MAC      (1U << 1)
#define RWY_LOG_SET_ADDRESS  (1U << 2)

typedef struct {
    rwy_blk_header_t hdr;          /* magic RWY_BLK_MAGIC, kind RWY_BLK_KIND_LOG */
    uint32_t         version;      /* RWY_LOG_VERSION */
    uint32_t         flags;        /* RWY_LOG_SET_* */
    uint32_t         server;       /* IPv4 syslog server */
    uint16_t         port;         /* UDP port */
    uint8_t          mac[6];       /* permanent address of the interface */
    uint32_t         address;      /* static IPv4 address */
    uint32_t         gateway;      /* 0 = none */
    uint8_t          prefix;       /* network prefix length, 0 to 32 */
    uint8_t          timeout;      /* total report budget in seconds */
    uint8_t          backlog;      /* committed events sent as context */
    uint8_t          reserved0;
    uint8_t          reserved[80]; /* zero */
} rwy_log_block_t;

/* Inode: 256 bytes. Extents describe file/dir data as block runs.
 * Direct extents cover small/medium files; `indirect` optionally points
 * at a block containing up to 1024 further (start,len) pairs. */
#define RWY_DIRECT_EXTENTS 8u

typedef struct {
    uint32_t start;  /* first block index */
    uint32_t len;    /* block count */
} rwy_extent_t;

typedef struct {
    uint64_t oid;            /* object id; 0 = free slot */
    uint8_t  type;           /* RWY_TYPE_* */
    uint8_t  reserved0[3];
    uint32_t mode;           /* POSIX mode incl. file-type + perms + suid/sgid/sticky */
    uint32_t uid;            /* 32-bit */
    uint32_t gid;            /* 32-bit */
    uint64_t size;           /* logical file size in bytes */
    int64_t  atime_sec;      /* signed 64-bit seconds */
    uint32_t atime_nsec;
    int64_t  mtime_sec;
    uint32_t mtime_nsec;
    int64_t  ctime_sec;
    uint32_t ctime_nsec;
    int64_t  btime_sec;      /* creation time (optional; 0 if unset) */
    uint32_t btime_nsec;
    uint64_t generation;     /* last-modifying generation */
    uint32_t extent_count;   /* number of valid direct extents */
    uint32_t indirect;       /* block index of indirect extent block, or 0 */
    uint64_t xattr_root;     /* block index of xattr root block, or 0 */
    uint32_t flags;          /* reserved */
    uint32_t crc_reserved;   /* reserved (per-inode CRC is future work) */
    rwy_extent_t extents[RWY_DIRECT_EXTENTS];
    uint32_t nlink;          /* number of directory hard links (>= 1 for live) */
    uint8_t  reserved1[76];  /* zero; pads to 256 */
} rwy_inode_t;

/* Directory record (in directory file data, 8-byte aligned):
 *   u64 oid; u32 reclen; u8 type; u8 namelen; u16 reserved; name[namelen];
 * reclen includes the header and is a multiple of 8. Deleted records are
 * compacted (directories are rewritten CoW on each change). */
#define RWY_NAME_MAX 255u

/* Extended attributes block header (kind RWY_BLK_KIND_XATTR).
 * Total header is 64 bytes; leaves 4032 bytes for packed records. */
typedef struct {
    rwy_blk_header_t hdr;          /* magic RWY_BLK_MAGIC, kind RWY_BLK_KIND_XATTR */
    uint32_t          count;        /* number of xattr entries in this block */
    uint32_t          used_bytes;   /* total payload bytes used after this header */
    uint32_t          next;         /* block index of next chained xattr block, or 0 */
    uint8_t           reserved[20]; /* zero; pads header to 64 bytes */
} rwy_xattr_header_t;

/* Packed record header for each extended attribute:
 * followed immediately by name[namelen] and value[valuelen]. */
typedef struct {
    uint16_t namelen;
    uint16_t valuelen;
    uint32_t flags;
} rwy_xattr_entry_t;

#ifndef XATTR_CREATE
#define XATTR_CREATE  1
#endif
#ifndef XATTR_REPLACE
#define XATTR_REPLACE 2
#endif

#pragma pack(pop)

#endif /* RWY_FORMAT_H */
