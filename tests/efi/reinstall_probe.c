/*
 * tests/reinstall_probe.c — Standalone probe for ReinstallProtocolInterface (Phase 0).
 * SPDX-License-Identifier: Apache-2.0
 *
 * Verifies:
 * 1. gBS->ReinstallProtocolInterface(part_handle, &gEfiSimpleFileSystemProtocolGuid,
 *                                   old_fat_sfs, test_sfs) succeeds on the active boot handle.
 * 2. The already-opened raw FAT file handle (store_fh) continues to read, write,
 *    and flush without error after protocol reinstallation.
 * 3. DevicePathFromHandle(part_handle) remains unchanged, preserving the authentic
 *    parent bus path and GPT HARDDRIVE_DEVICE_PATH node.
 * 4. Firmware's gBS->LoadImage() resolves file paths against the reinstalled protocol interface.
 */
#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include <string.h>
#include "efi_util.h"

int memcmp(const void *s1, const void *s2, UINTN n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    while (n--) {
        if (*p1 != *p2) return (int)*p1 - (int)*p2;
        p1++;
        p2++;
    }
    return 0;
}

/* ---------- Protocol & DevicePath Helpers ---------- */

static UINTN devpath_size(const EFI_DEVICE_PATH *dp) {
    const EFI_DEVICE_PATH *n = dp;
    while (!IsDevicePathEnd(n)) {
        n = NextDevicePathNode(n);
    }
    return ((const UINT8 *)n - (const UINT8 *)dp) + DevicePathNodeLength(n);
}

static EFI_DEVICE_PATH *make_file_device_path(const EFI_DEVICE_PATH *parent, const CHAR16 *path) {
    UINTN parent_sz = devpath_size(parent);
    UINTN parent_bytes = parent_sz - sizeof(EFI_DEVICE_PATH_PROTOCOL); /* strip end node */
    UINTN path_chars = 0;
    while (path[path_chars]) path_chars++;
    UINTN path_bytes = (path_chars + 1) * sizeof(CHAR16);
    UINTN file_node_sz = sizeof(EFI_DEVICE_PATH_PROTOCOL) + path_bytes;
    UINTN total_sz = parent_bytes + file_node_sz + sizeof(EFI_DEVICE_PATH_PROTOCOL);

    UINT8 *buf = NULL;
    EFI_STATUS s = gBS->AllocatePool(EfiLoaderData, total_sz, (void **)&buf);
    if (EFI_ERROR(s)) return NULL;

    /* 1. Parent path (excluding end node) */
    memcpy(buf, parent, parent_bytes);

    /* 2. FilePath node */
    EFI_DEVICE_PATH_PROTOCOL *fp = (EFI_DEVICE_PATH_PROTOCOL *)(buf + parent_bytes);
    fp->Type = MEDIA_DEVICE_PATH;
    fp->SubType = MEDIA_FILEPATH_DP;
    fp->Length[0] = (UINT8)(file_node_sz & 0xFF);
    fp->Length[1] = (UINT8)((file_node_sz >> 8) & 0xFF);
    memcpy(buf + parent_bytes + sizeof(EFI_DEVICE_PATH_PROTOCOL), path, path_bytes);

    /* 3. End node */
    EFI_DEVICE_PATH_PROTOCOL *end = (EFI_DEVICE_PATH_PROTOCOL *)(buf + parent_bytes + file_node_sz);
    end->Type = END_DEVICE_PATH_TYPE;
    end->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    end->Length[0] = 4;
    end->Length[1] = 0;

    return (EFI_DEVICE_PATH *)buf;
}

static void format_guid(const UINT8 *guid, char *out) {
    static const char hex[] = "0123456789abcdef";
    int pos = 0;
    const UINT8 *d = guid;
    /* Data1 (LE) */
    out[pos++] = hex[(d[3] >> 4) & 0xF]; out[pos++] = hex[d[3] & 0xF];
    out[pos++] = hex[(d[2] >> 4) & 0xF]; out[pos++] = hex[d[2] & 0xF];
    out[pos++] = hex[(d[1] >> 4) & 0xF]; out[pos++] = hex[d[1] & 0xF];
    out[pos++] = hex[(d[0] >> 4) & 0xF]; out[pos++] = hex[d[0] & 0xF];
    out[pos++] = '-';
    /* Data2 (LE) */
    out[pos++] = hex[(d[5] >> 4) & 0xF]; out[pos++] = hex[d[5] & 0xF];
    out[pos++] = hex[(d[4] >> 4) & 0xF]; out[pos++] = hex[d[4] & 0xF];
    out[pos++] = '-';
    /* Data3 (LE) */
    out[pos++] = hex[(d[7] >> 4) & 0xF]; out[pos++] = hex[d[7] & 0xF];
    out[pos++] = hex[(d[6] >> 4) & 0xF]; out[pos++] = hex[d[6] & 0xF];
    out[pos++] = '-';
    /* Data4 (BE) */
    out[pos++] = hex[(d[8] >> 4) & 0xF]; out[pos++] = hex[d[8] & 0xF];
    out[pos++] = hex[(d[9] >> 4) & 0xF]; out[pos++] = hex[d[9] & 0xF];
    out[pos++] = '-';
    for (int i = 10; i < 16; i++) {
        out[pos++] = hex[(d[i] >> 4) & 0xF];
        out[pos++] = hex[d[i] & 0xF];
    }
    out[pos] = '\0';
}

static BOOLEAN path_matches(const CHAR16 *requested, const CHAR16 *target) {
    if (!requested || !target) return FALSE;
    while (*requested == L'\\') requested++;
    while (*target == L'\\') target++;
    while (*requested && *target) {
        CHAR16 a = *requested++;
        CHAR16 b = *target++;
        if (a >= L'a' && a <= L'z') a -= (L'a' - L'A');
        if (b >= L'a' && b <= L'A') b -= (L'a' - L'A');
        if (a != b) return FALSE;
    }
    return (*requested == 0 && *target == 0);
}

/* ---------- Reinstalled test_sfs Implementation ---------- */

static UINTN gTestSfsOpenVolumeCalls = 0;
static UINTN gTestSfsFileOpenCalls = 0;
static UINTN gTestSfsFileReadCalls = 0;
static UINTN gTestSfsFileGetInfoCalls = 0;

static UINT8 *gChildBinaryBuf = NULL;
static UINTN gChildBinarySize = 0;
static EFI_FILE_HANDLE gFatRoot = NULL;

typedef struct {
    EFI_FILE_PROTOCOL proto;
    UINT64 pos;
    BOOLEAN is_open;
} intercepted_file_t;

static intercepted_file_t gInterceptedFile;
static EFI_FILE_PROTOCOL gTestRootDir;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL gTestSfs;

static EFI_STATUS EFIAPI intercepted_read(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer) {
    intercepted_file_t *f = (intercepted_file_t *)This;
    gTestSfsFileReadCalls++;
    if (!f->is_open || !BufferSize || (!Buffer && *BufferSize > 0)) return EFI_INVALID_PARAMETER;
    if (f->pos >= gChildBinarySize) {
        *BufferSize = 0;
        return EFI_SUCCESS;
    }
    UINTN remain = (UINTN)(gChildBinarySize - f->pos);
    UINTN to_copy = *BufferSize;
    if (to_copy > remain) to_copy = remain;
    memcpy(Buffer, gChildBinaryBuf + f->pos, to_copy);
    f->pos += to_copy;
    *BufferSize = to_copy;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI intercepted_setpos(EFI_FILE_PROTOCOL *This, UINT64 Position) {
    intercepted_file_t *f = (intercepted_file_t *)This;
    if (!f->is_open) return EFI_INVALID_PARAMETER;
    if (Position == 0xFFFFFFFFFFFFFFFFULL) f->pos = gChildBinarySize;
    else f->pos = Position;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI intercepted_getpos(EFI_FILE_PROTOCOL *This, UINT64 *Position) {
    intercepted_file_t *f = (intercepted_file_t *)This;
    if (!f->is_open || !Position) return EFI_INVALID_PARAMETER;
    *Position = f->pos;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI intercepted_getinfo(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
                                             UINTN *BufferSize, VOID *Buffer) {
    intercepted_file_t *f = (intercepted_file_t *)This;
    EFI_GUID fig = EFI_FILE_INFO_ID;
    gTestSfsFileGetInfoCalls++;
    if (!f->is_open || !InformationType || !BufferSize) return EFI_INVALID_PARAMETER;
    if (!guid_eq(InformationType, &fig)) return EFI_UNSUPPORTED;

    static const CHAR16 name[] = L"INTERCEPTED.EFI";
    UINTN name_bytes = sizeof(name);
    UINTN needed = SIZE_OF_EFI_FILE_INFO + name_bytes;

    if (*BufferSize < needed) {
        *BufferSize = needed;
        return EFI_BUFFER_TOO_SMALL;
    }

    memset(Buffer, 0, needed);
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)Buffer;
    fi->Size = needed;
    fi->FileSize = gChildBinarySize;
    fi->PhysicalSize = gChildBinarySize;
    fi->Attribute = EFI_FILE_READ_ONLY;
    memcpy(fi->FileName, name, name_bytes);
    *BufferSize = needed;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI intercepted_close(EFI_FILE_PROTOCOL *This) {
    intercepted_file_t *f = (intercepted_file_t *)This;
    f->is_open = FALSE;
    f->pos = 0;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI stub_unsupported(EFI_FILE_PROTOCOL *This, ...) {
    (void)This;
    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI test_root_open(EFI_FILE_PROTOCOL *This,
                                        EFI_FILE_PROTOCOL **NewHandle,
                                        CHAR16 *FileName,
                                        UINT64 OpenMode,
                                        UINT64 Attributes) {
    (void)This;
    (void)OpenMode;
    (void)Attributes;
    gTestSfsFileOpenCalls++;
    if (!NewHandle || !FileName) return EFI_INVALID_PARAMETER;

    if (path_matches(FileName, L"INTERCEPTED.EFI")) {
        gInterceptedFile.proto.Revision = EFI_FILE_PROTOCOL_REVISION;
        gInterceptedFile.proto.Open = (EFI_FILE_OPEN)stub_unsupported;
        gInterceptedFile.proto.Close = intercepted_close;
        gInterceptedFile.proto.Delete = (EFI_FILE_DELETE)stub_unsupported;
        gInterceptedFile.proto.Read = intercepted_read;
        gInterceptedFile.proto.Write = (EFI_FILE_WRITE)stub_unsupported;
        gInterceptedFile.proto.GetPosition = intercepted_getpos;
        gInterceptedFile.proto.SetPosition = intercepted_setpos;
        gInterceptedFile.proto.GetInfo = intercepted_getinfo;
        gInterceptedFile.proto.SetInfo = (EFI_FILE_SET_INFO)stub_unsupported;
        gInterceptedFile.proto.Flush = (EFI_FILE_FLUSH)intercepted_close;
        gInterceptedFile.pos = 0;
        gInterceptedFile.is_open = TRUE;
        *NewHandle = &gInterceptedFile.proto;
        return EFI_SUCCESS;
    }

    /* Fallback: delegate any other path requests to raw FAT root */
    if (gFatRoot) {
        return gFatRoot->Open(gFatRoot, NewHandle, FileName, OpenMode, Attributes);
    }
    return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI test_root_close(EFI_FILE_PROTOCOL *This) {
    (void)This;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI test_sfs_openvolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                             EFI_FILE_PROTOCOL **Root) {
    (void)This;
    gTestSfsOpenVolumeCalls++;
    if (!Root) return EFI_INVALID_PARAMETER;
    *Root = &gTestRootDir;
    return EFI_SUCCESS;
}

static EFI_STATUS raw_write_at(EFI_FILE_HANDLE fh, UINT64 pos, const VOID *buf, UINTN len) {
    UINTN sz = len;
    EFI_STATUS s = fh->SetPosition(fh, pos);
    if (EFI_ERROR(s)) return s;
    s = fh->Write(fh, &sz, (VOID *)buf);
    if (EFI_ERROR(s)) return s;
    if (sz != len) return EFI_DEVICE_ERROR;
    return fh->Flush(fh);
}

static EFI_STATUS raw_read_at(EFI_FILE_HANDLE fh, UINT64 pos, VOID *buf, UINTN len) {
    UINTN sz = len;
    EFI_STATUS s = fh->SetPosition(fh, pos);
    if (EFI_ERROR(s)) return s;
    s = fh->Read(fh, &sz, buf);
    if (EFI_ERROR(s)) return s;
    if (sz != len) return EFI_DEVICE_ERROR;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI __attribute__((noinline)) reinstall_probe_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS s;
    EFI_LOADED_IMAGE *li = NULL;
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;

    util_init(ImageHandle, SystemTable);

    uprint("\n=================================================================\n");
    uprint(" Runway Phase 0: Standalone probe for ReinstallProtocolInterface    \n");
    uprint("=================================================================\n\n");

    /* Step 0: Identify active boot partition handle */
    s = gBS->HandleProtocol(ImageHandle, &li_guid, (void **)&li);
    if (EFI_ERROR(s)) return ufail("Cannot obtain LoadedImage on self", s);
    EFI_HANDLE part_handle = li->DeviceHandle;
    uprintf("[INIT] Self ImageHandle:         %p\n", ImageHandle);
    uprintf("[INIT] Boot partition handle:    %p\n", part_handle);

    /* Step 1: Open firmware FAT root and raw backing file (RUNWAY.DAT) */
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *old_fat_sfs = NULL;
    s = gBS->HandleProtocol(part_handle, &sfs_guid, (void **)&old_fat_sfs);
    if (EFI_ERROR(s)) return ufail("Cannot obtain SFS on boot partition handle", s);
    uprintf("[INIT] Firmware FAT SFS:         %p\n", old_fat_sfs);

    s = old_fat_sfs->OpenVolume(old_fat_sfs, &gFatRoot);
    if (EFI_ERROR(s)) return ufail("Cannot open root volume on firmware FAT", s);

    EFI_FILE_HANDLE store_fh = NULL;
    s = gFatRoot->Open(gFatRoot, &store_fh, L"\\RUNWAY.DAT",
                       EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(s)) {
        s = gFatRoot->Open(gFatRoot, &store_fh, L"\\RUNWAY.DAT",
                           EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    }
    if (EFI_ERROR(s)) return ufail("Cannot open \\RUNWAY.DAT on firmware FAT", s);
    uprintf("[INIT] Raw store_fh handle:      %p\n", store_fh);

    /* Perform initial write to store_fh at sector 0 */
    static const UINT8 pre_magic[16] = "RWY_PRE_REINST";
    s = raw_write_at(store_fh, 0, pre_magic, sizeof(pre_magic));
    if (EFI_ERROR(s)) return ufail("store_fh initial raw_write_at(0) failed", s);
    uprint("[INIT] Written initial baseline pattern to store_fh (offset 0): OK\n");

    /* Step 2: Read child binary into memory so test_sfs can serve it */
    s = fat_read_file(gFatRoot, L"\\EFI\\REINSTALL\\CHILD.EFI", &gChildBinaryBuf, &gChildBinarySize);
    if (EFI_ERROR(s)) return ufail("Cannot read \\EFI\\REINSTALL\\CHILD.EFI from FAT", s);
    uprintf("[INIT] Loaded child payload:     %llu bytes into memory\n",
            (unsigned long long)gChildBinarySize);

    /* Verify that \\INTERCEPTED.EFI does NOT exist on the physical FAT filesystem */
    EFI_FILE_HANDLE non_existent_fh = NULL;
    s = gFatRoot->Open(gFatRoot, &non_existent_fh, L"\\INTERCEPTED.EFI", EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR(s)) {
        non_existent_fh->Close(non_existent_fh);
        return ufail("\\INTERCEPTED.EFI unexpectedly exists on FAT!", EFI_ALREADY_STARTED);
    }
    uprint("[INIT] Verified \\INTERCEPTED.EFI does NOT exist on FAT: OK\n");

    /* Step 3: Inspect and record authentic DevicePath before reinstallation */
    EFI_DEVICE_PATH *dp_before = NULL;
    s = gBS->HandleProtocol(part_handle, &dp_guid, (void **)&dp_before);
    if (EFI_ERROR(s)) return ufail("Cannot obtain DevicePath on boot handle", s);

    UINTN dp_size_before = devpath_size(dp_before);
    uprintf("[INIT] DevicePath before:        %p (size %u bytes)\n",
            dp_before, (unsigned int)dp_size_before);

    UINT8 *dp_copy = NULL;
    s = gBS->AllocatePool(EfiLoaderData, dp_size_before, (void **)&dp_copy);
    if (EFI_ERROR(s)) return ufail("AllocatePool for dp_copy failed", s);
    memcpy(dp_copy, dp_before, dp_size_before);

    BOOLEAN found_gpt_hd_before = FALSE;
    HARDDRIVE_DEVICE_PATH gpt_hd_before;
    memset(&gpt_hd_before, 0, sizeof(gpt_hd_before));
    UINTN node_idx = 0;
    EFI_DEVICE_PATH *cur_node = dp_before;
    while (!IsDevicePathEnd(cur_node)) {
        UINT8 t = DevicePathType(cur_node);
        UINT8 st = DevicePathSubType(cur_node);
        UINTN l = DevicePathNodeLength(cur_node);
        node_idx++;
        uprintf("       Node %u: Type=0x%02x SubType=0x%02x Length=%u\n",
                (unsigned int)node_idx, t, st, (unsigned int)l);
        if (t == MEDIA_DEVICE_PATH && st == MEDIA_HARDDRIVE_DP) {
            HARDDRIVE_DEVICE_PATH *hd = (HARDDRIVE_DEVICE_PATH *)cur_node;
            gpt_hd_before = *hd;
            found_gpt_hd_before = TRUE;
            char guid_str[37];
            format_guid(hd->Signature, guid_str);
            uprintf("       -> GPT Partition Node Found:\n");
            uprintf("          PartitionNumber: %u\n", (unsigned int)hd->PartitionNumber);
            uprintf("          PartitionStart:  LBA %llu (0x%llx)\n",
                    (unsigned long long)hd->PartitionStart, (unsigned long long)hd->PartitionStart);
            uprintf("          PartitionSize:   %llu sectors\n",
                    (unsigned long long)hd->PartitionSize);
            uprintf("          MBRType:         0x%02x (%s)\n",
                    hd->MBRType, hd->MBRType == 2 ? "GPT Header" : "Other");
            uprintf("          SignatureType:   0x%02x (%s)\n",
                    hd->SignatureType, hd->SignatureType == 2 ? "GUID" : "Other");
            uprintf("          Partition GUID:  %s\n", guid_str);
        }
        cur_node = NextDevicePathNode(cur_node);
    }
    if (!found_gpt_hd_before) {
        return ufail("Pre-check failed: No GPT HARDDRIVE_DEVICE_PATH node found", EFI_NOT_FOUND);
    }
    uprint("[INIT] GPT HARDDRIVE_DEVICE_PATH verified: OK\n\n");

    /* =========================================================================
     * PROOF 1: ReinstallProtocolInterface succeeds on active boot handle
     * ========================================================================= */
    uprint("-----------------------------------------------------------------\n");
    uprint("[TEST 1] ReinstallProtocolInterface on active boot handle...\n");

    gTestRootDir.Revision = EFI_FILE_PROTOCOL_REVISION;
    gTestRootDir.Open = test_root_open;
    gTestRootDir.Close = test_root_close;
    gTestRootDir.Delete = (EFI_FILE_DELETE)stub_unsupported;
    gTestRootDir.Read = (EFI_FILE_READ)stub_unsupported;
    gTestRootDir.Write = (EFI_FILE_WRITE)stub_unsupported;
    gTestRootDir.GetPosition = (EFI_FILE_GET_POSITION)stub_unsupported;
    gTestRootDir.SetPosition = (EFI_FILE_SET_POSITION)stub_unsupported;
    gTestRootDir.GetInfo = (EFI_FILE_GET_INFO)stub_unsupported;
    gTestRootDir.SetInfo = (EFI_FILE_SET_INFO)stub_unsupported;
    gTestRootDir.Flush = test_root_close;

    gTestSfs.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    gTestSfs.OpenVolume = test_sfs_openvolume;

    s = gBS->ReinstallProtocolInterface(part_handle, &sfs_guid, old_fat_sfs, &gTestSfs);
    if (EFI_ERROR(s)) {
        uprintf("[TEST 1] gBS->ReinstallProtocolInterface FAILED: 0x%llx\n", (unsigned long long)s);
        return ufail("TEST 1 FAILED", s);
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *current_sfs = NULL;
    s = gBS->HandleProtocol(part_handle, &sfs_guid, (void **)&current_sfs);
    if (EFI_ERROR(s) || current_sfs != &gTestSfs) {
        uprintf("[TEST 1] HandleProtocol verification failed: current=%p expected=%p\n",
                current_sfs, &gTestSfs);
        return ufail("TEST 1 FAILED: HandleProtocol does not return test_sfs", EFI_DEVICE_ERROR);
    }
    uprint("TEST 1: ReinstallProtocolInterface on active boot handle: PASS\n");

    /* =========================================================================
     * PROOF 2: store_fh continues to read and write without error
     * ========================================================================= */
    uprint("\n-----------------------------------------------------------------\n");
    uprint("[TEST 2] Verifying raw FAT store_fh read/write after reinstallation...\n");

    static const UINT8 post_magic[16] = "RWY_POST_REINST";
    s = raw_write_at(store_fh, 4096, post_magic, sizeof(post_magic));
    if (EFI_ERROR(s)) {
        uprintf("[TEST 2] raw_write_at(4096) failed: 0x%llx\n", (unsigned long long)s);
        return ufail("TEST 2 FAILED: Write failed", s);
    }

    /* Read back post_magic from offset 4096 */
    UINT8 read_buf[16];
    memset(read_buf, 0, sizeof(read_buf));
    s = raw_read_at(store_fh, 4096, read_buf, sizeof(read_buf));
    if (EFI_ERROR(s)) {
        uprintf("[TEST 2] raw_read_at(4096) failed: 0x%llx\n", (unsigned long long)s);
        return ufail("TEST 2 FAILED: Read failed", s);
    }
    if (memcmp(read_buf, post_magic, sizeof(post_magic)) != 0) {
        return ufail("TEST 2 FAILED: Post-reinstallation data mismatch", EFI_CRC_ERROR);
    }

    /* Also read back pre_magic from offset 0 */
    memset(read_buf, 0, sizeof(read_buf));
    s = raw_read_at(store_fh, 0, read_buf, sizeof(read_buf));
    if (EFI_ERROR(s)) {
        return ufail("[TEST 2] raw_read_at(0) failed", s);
    }
    if (memcmp(read_buf, pre_magic, sizeof(pre_magic)) != 0) {
        return ufail("TEST 2 FAILED: Pre-reinstallation data mismatch", EFI_CRC_ERROR);
    }
    uprint("TEST 2: Already-opened store_fh remains functional: PASS\n");

    /* =========================================================================
     * PROOF 3: DevicePath remains unchanged and authentic GPT node is intact
     * ========================================================================= */
    uprint("\n-----------------------------------------------------------------\n");
    uprint("[TEST 3] Verifying DevicePath integrity & authentic GPT node...\n");

    EFI_DEVICE_PATH *dp_after = NULL;
    s = gBS->HandleProtocol(part_handle, &dp_guid, (void **)&dp_after);
    if (EFI_ERROR(s)) return ufail("[TEST 3] HandleProtocol(DevicePath) failed", s);

    UINTN dp_size_after = devpath_size(dp_after);
    uprintf("[TEST 3] DevicePath after:         %p (size %u bytes)\n",
            dp_after, (unsigned int)dp_size_after);

    if (dp_size_after != dp_size_before) {
        uprintf("[TEST 3] Size mismatch: before=%u after=%u\n",
                (unsigned int)dp_size_before, (unsigned int)dp_size_after);
        return ufail("TEST 3 FAILED: DevicePath size changed", EFI_DEVICE_ERROR);
    }

    if (memcmp(dp_after, dp_copy, dp_size_after) != 0) {
        return ufail("TEST 3 FAILED: DevicePath binary content modified", EFI_DEVICE_ERROR);
    }

    BOOLEAN found_gpt_hd_after = FALSE;
    cur_node = dp_after;
    while (!IsDevicePathEnd(cur_node)) {
        if (DevicePathType(cur_node) == MEDIA_DEVICE_PATH &&
            DevicePathSubType(cur_node) == MEDIA_HARDDRIVE_DP) {
            HARDDRIVE_DEVICE_PATH *hd = (HARDDRIVE_DEVICE_PATH *)cur_node;
            if (hd->MBRType == 2 && hd->SignatureType == 2 &&
                hd->PartitionNumber == gpt_hd_before.PartitionNumber &&
                hd->PartitionStart == gpt_hd_before.PartitionStart &&
                hd->PartitionSize == gpt_hd_before.PartitionSize &&
                memcmp(hd->Signature, gpt_hd_before.Signature, 16) == 0) {
                found_gpt_hd_after = TRUE;
            }
        }
        cur_node = NextDevicePathNode(cur_node);
    }
    if (!found_gpt_hd_after) {
        return ufail("TEST 3 FAILED: GPT HARDDRIVE_DEVICE_PATH corrupted or missing", EFI_DEVICE_ERROR);
    }
    uprint("TEST 3: DevicePath unchanged & authentic GPT node preserved: PASS\n");

    /* =========================================================================
     * PROOF 4: Firmware LoadImage resolves file paths against test_sfs
     * ========================================================================= */
    uprint("\n-----------------------------------------------------------------\n");
    uprint("[TEST 4] Testing firmware gBS->LoadImage() resolution via test_sfs...\n");

    EFI_DEVICE_PATH *child_dp = make_file_device_path(dp_after, L"\\INTERCEPTED.EFI");
    if (!child_dp) return ufail("[TEST 4] Failed to create file device path", EFI_OUT_OF_RESOURCES);

    EFI_HANDLE child_img = NULL;
    /* SourceBuffer=NULL and SourceSize=0 forces firmware to resolve through SFS on DeviceHandle */
    s = gBS->LoadImage(FALSE, ImageHandle, child_dp, NULL, 0, &child_img);
    if (EFI_ERROR(s)) {
        uprintf("[TEST 4] LoadImage failed: 0x%llx\n", (unsigned long long)s);
        return ufail("TEST 4 FAILED: LoadImage could not resolve file", s);
    }
    uprintf("[TEST 4] LoadImage returned SUCCESS, child handle: %p\n", child_img);

    uprintf("[TEST 4] Protocol instrumentation:\n");
    uprintf("         OpenVolume invocations: %u\n", (unsigned int)gTestSfsOpenVolumeCalls);
    uprintf("         File Open invocations:  %u\n", (unsigned int)gTestSfsFileOpenCalls);
    uprintf("         File Read invocations:  %u\n", (unsigned int)gTestSfsFileReadCalls);

    if (gTestSfsOpenVolumeCalls == 0 || gTestSfsFileOpenCalls == 0 || gTestSfsFileReadCalls == 0) {
        return ufail("TEST 4 FAILED: LoadImage bypassed test_sfs", EFI_PROTOCOL_ERROR);
    }

    uprint("[TEST 4] Executing child image via gBS->StartImage...\n");
    s = gBS->StartImage(child_img, NULL, NULL);
    uprintf("[TEST 4] Child StartImage exit status: 0x%llx\n", (unsigned long long)s);
    if (EFI_ERROR(s)) return ufail("TEST 4 FAILED: Child execution failed", s);

    EFI_LOADED_IMAGE *child_li = NULL;
    s = gBS->HandleProtocol(child_img, &li_guid, (void **)&child_li);
    if (!EFI_ERROR(s) && child_li) {
        uprintf("[TEST 4] Child LoadedImage.DeviceHandle = %p\n", child_li->DeviceHandle);
        if (child_li->DeviceHandle != part_handle) {
            return ufail("TEST 4 FAILED: Child DeviceHandle mismatch", EFI_PROTOCOL_ERROR);
        }
        uprint("[TEST 4] Child DeviceHandle matches authentic partition handle: OK\n");
    }

    uprint("TEST 4: Firmware LoadImage resolves via reinstalled protocol: PASS\n");

    /* Clean up / restore original firmware FAT protocol */
    s = gBS->ReinstallProtocolInterface(part_handle, &sfs_guid, &gTestSfs, old_fat_sfs);
    if (EFI_ERROR(s)) {
        uprintf("[CLEANUP] Warning: Reinstalling original SFS returned 0x%llx\n",
                (unsigned long long)s);
    } else {
        uprint("[CLEANUP] Restored original firmware FAT SFS: OK\n");
    }

    uprint("\n=================================================================\n");
    uprint(" ALL 4 PHASE 0 REINSTALL PROTOCOL PROOFS PASSED!\n");
    uprint("=================================================================\n\n");

    flush_and_stall(500000);
    gST->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    return EFI_SUCCESS;
}

/* Entry point entered via SysV by crt0-efi-x86_64.o */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    return reinstall_probe_main(ImageHandle, SystemTable);
}
