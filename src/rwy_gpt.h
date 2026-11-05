/*
 * rwy_gpt.h — find the type of a partition by reading the GPT of its disk.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Firmware older than UEFI 2.7 does not provide EFI_PARTITION_INFO_PROTOCOL, so the loader cannot ask which
 * partition is a store partition. It can still read the table itself: the device path names the partition by
 * its number and unique GUID, and the table gives the type of that entry.
 */
#ifndef RWY_GPT_H
#define RWY_GPT_H

#include <stddef.h>
#include <stdint.h>

/* Read `len` bytes at byte offset `off` of the disk. Returns 0 on success. */
typedef int (*rwy_gpt_read_fn)(void *ctx, uint64_t off, void *buf, size_t len);

/*
 * The type GUID of partition `number` (counted from 1), which must have the unique GUID `unique`. The GUIDs are
 * the 16 bytes of the table, with the first three fields little endian. The table of the disk is checked (signature,
 * position, both CRCs) and the backup table is used when the first one is not valid.
 * Returns 0 and fills `type`, or -1 when no valid table names that partition.
 */
int rwy_gpt_partition_type(rwy_gpt_read_fn rd, void *ctx, uint32_t block_size, uint64_t last_lba, uint32_t number,
                           const uint8_t unique[16], uint8_t type[16]);

#endif
