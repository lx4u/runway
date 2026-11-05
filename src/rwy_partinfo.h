/*
 * rwy_partinfo.h — the GPT type of a partition handle.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_PARTINFO_H
#define RWY_PARTINFO_H

#include <efi.h>

#define PARTITION_INFO_GUID \
    { 0x8cf2f62c, 0xbc9b, 0x4821, { 0x80, 0x8d, 0xec, 0x9e, 0xc4, 0x21, 0xa1, 0xa0 } }
#define PARTITION_INFO_MBR 1
#define PARTITION_INFO_GPT 2

/* EFI_PARTITION_INFO_PROTOCOL: for a GPT partition the entry is the 128 bytes of the GPT table, which start with the type GUID. */
typedef struct {
    UINT32 revision;
    UINT32 type;
    UINT8 system;
    UINT8 reserved[7];
    UINT8 entry[128];
} partition_info_t;

/* TRUE and the type GUID when the handle is a GPT partition and its type is known. */
BOOLEAN rwy_partition_type(EFI_HANDLE part, EFI_GUID *type);

#endif
