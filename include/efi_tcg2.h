/*
 * efi_tcg2.h — EFI TCG2 Protocol definitions for UEFI TPM 2.0 support.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Defines EFI_TCG2_PROTOCOL_GUID and minimal protocol structure for
 * submitting TPM 2.0 commands, querying capabilities, and event logging.
 */
#ifndef EFI_TCG2_H
#define EFI_TCG2_H

#include <efi.h>
#include <efilib.h>

#define EFI_TCG2_PROTOCOL_GUID \
    { 0x607f766c, 0x7455, 0x42be, { 0x93, 0x0b, 0xe4, 0xd7, 0x6d, 0xb2, 0x72, 0x0f } }

typedef struct _EFI_TCG2_PROTOCOL EFI_TCG2_PROTOCOL;

typedef struct {
    UINT8 Major;
    UINT8 Minor;
} EFI_TCG2_VERSION;

typedef UINT32 EFI_TCG2_EVENT_LOG_BITMAP;
typedef UINT32 EFI_TCG2_EVENT_LOG_FORMAT;
#define EFI_TCG2_EVENT_LOG_FORMAT_TCG_1_2 0x00000001
#define EFI_TCG2_EVENT_LOG_FORMAT_TCG_2   0x00000002

typedef struct {
    UINT8  Size;
    EFI_TCG2_VERSION StructureVersion;
    EFI_TCG2_VERSION ProtocolVersion;
    UINT32 HashAlgorithmBitmap;
    EFI_TCG2_EVENT_LOG_BITMAP SupportedEventLogs;
    BOOLEAN TPMPresentFlag;
    UINT16 MaxCommandSize;
    UINT16 MaxResponseSize;
    UINT32 ManufacturerID;
    UINT32 NumberOfPcrBanks;
    UINT32 ActivePcrBanks;
} EFI_TCG2_BOOT_SERVICE_CAPABILITY;

#define EFI_TCG2_BOOT_HASH_ALG_SHA1    0x00000001
#define EFI_TCG2_BOOT_HASH_ALG_SHA256  0x00000002
#define EFI_TCG2_BOOT_HASH_ALG_SHA384  0x00000004
#define EFI_TCG2_BOOT_HASH_ALG_SHA512  0x00000008
#define EFI_TCG2_BOOT_HASH_ALG_SM3_256 0x00000010

typedef struct {
    UINT32 Size;
    /* Trailing event fields */
} EFI_TCG2_EVENT;

typedef EFI_STATUS (EFIAPI *EFI_TCG2_GET_CAPABILITY)(
    IN EFI_TCG2_PROTOCOL *This,
    IN OUT EFI_TCG2_BOOT_SERVICE_CAPABILITY *ProtocolCapability
);

typedef EFI_STATUS (EFIAPI *EFI_TCG2_GET_EVENT_LOG)(
    IN EFI_TCG2_PROTOCOL *This,
    IN EFI_TCG2_EVENT_LOG_FORMAT EventLogFormat,
    OUT EFI_PHYSICAL_ADDRESS *EventLogLocation,
    OUT EFI_PHYSICAL_ADDRESS *EventLogLastEntry,
    OUT BOOLEAN *EventLogTruncated
);

typedef EFI_STATUS (EFIAPI *EFI_TCG2_HASH_LOG_EXTEND_EVENT)(
    IN EFI_TCG2_PROTOCOL *This,
    IN UINT64 Flags,
    IN EFI_PHYSICAL_ADDRESS DataToHash,
    IN UINT64 DataToHashLen,
    IN EFI_TCG2_EVENT *EfiTcg2Event
);

typedef EFI_STATUS (EFIAPI *EFI_TCG2_SUBMIT_COMMAND)(
    IN EFI_TCG2_PROTOCOL *This,
    IN UINT32 InputParameterBlockSize,
    IN UINT8 *InputParameterBlock,
    IN UINT32 OutputParameterBlockSize,
    OUT UINT8 *OutputParameterBlock
);

typedef EFI_STATUS (EFIAPI *EFI_TCG2_GET_ACTIVE_PCR_BANKS)(
    IN EFI_TCG2_PROTOCOL *This,
    OUT UINT32 *ActivePcrBanks
);

typedef EFI_STATUS (EFIAPI *EFI_TCG2_SET_ACTIVE_PCR_BANKS)(
    IN EFI_TCG2_PROTOCOL *This,
    IN UINT32 ActivePcrBanks
);

typedef EFI_STATUS (EFIAPI *EFI_TCG2_GET_RESULT_OF_SET_ACTIVE_PCR_BANKS)(
    IN EFI_TCG2_PROTOCOL *This,
    OUT UINT32 *OperationPresent,
    OUT UINT32 *Response
);

struct _EFI_TCG2_PROTOCOL {
    EFI_TCG2_GET_CAPABILITY                 GetCapability;
    EFI_TCG2_GET_EVENT_LOG                  GetEventLog;
    EFI_TCG2_HASH_LOG_EXTEND_EVENT          HashLogExtendEvent;
    EFI_TCG2_SUBMIT_COMMAND                 SubmitCommand;
    EFI_TCG2_GET_ACTIVE_PCR_BANKS           GetActivePcrBanks;
    EFI_TCG2_SET_ACTIVE_PCR_BANKS           SetActivePcrBanks;
    EFI_TCG2_GET_RESULT_OF_SET_ACTIVE_PCR_BANKS GetResultOfSetActivePcrBanks;
};

#endif /* EFI_TCG2_H */
