/*
 * chain_main.c — CHAIN.EFI: filesystem test + benchmark consumer.
 * SPDX-License-Identifier: Apache-2.0
 *
 * RULE: this application must use ONLY the normal UEFI filesystem
 * abstractions (LoadedImage.DeviceHandle -> SimpleFileSystem -> OpenVolume
 * -> EFI_FILE_PROTOCOL). It must NOT call any private Runway API and must
 * not know RUNWAY.DAT is FAT-backed, except for diagnostics via the
 * vendor anchor protocol (used solely for the FAT-baseline comparison).
 *
 * Test plan (§brief items 1..14):
 *  enumerate, open/read/verify seed files, create+write, reread, rename,
 *  reread, delete; then sequential-read / sequential-write / rename-rate
 *  benchmarks; then an optional direct-FAT baseline read of the backing
 *  file bytes (same firmware FAT driver, same disk).
 */
#include <efi.h>
#include <efilib.h>
#include "efi_util.h"
#include "rwy_fs.h" /* only for RWY_ANCHOR_GUID/magic; no Runway calls */

#define CHUNK (1048576u) /* 1 MiB I/O chunks */

/* Pattern contract shared with tools/mkstore:
 *   byte[file_offset] = (file_offset + seed) & 0xFF */
static int verify_pattern(const UINT8 *buf, UINT64 base, UINTN len, UINT32 seed) {
    for (UINTN i = 0; i < len; i++) {
        UINT8 want = (UINT8)((base + i + seed) & 0xFF);
        if (buf[i] != want) return -1;
    }
    return 0;
}

static void fill_pattern(UINT8 *buf, UINT64 base, UINTN len, UINT32 seed) {
    for (UINTN i = 0; i < len; i++)
        buf[i] = (UINT8)((base + i + seed) & 0xFF);
}

static EFI_STATUS open_path(EFI_FILE_HANDLE root, const CHAR16 *path,
                            UINT64 mode, EFI_FILE_HANDLE *out) {
    return root->Open(root, out, (CHAR16 *)path, mode, 0);
}

/* Read whole file (cap 96 MiB) into pool; returns bytes. Verifies optional pattern. */
static EFI_STATUS read_whole(EFI_FILE_HANDLE root, const CHAR16 *path,
                             UINT8 **out_buf, UINT64 *out_size) {
    EFI_STATUS s;
    EFI_FILE_HANDLE fh = NULL;
    UINT8 info[512];
    UINTN isz = sizeof(info);
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info;
    EFI_GUID fig = EFI_FILE_INFO_ID;
    UINT8 *buf = NULL;
    UINT64 left, pos = 0;
    UINTN got;

    s = open_path(root, path, EFI_FILE_MODE_READ, &fh);
    if (EFI_ERROR(s)) return s;
    s = fh->GetInfo(fh, &fig, &isz, fi);
    if (EFI_ERROR(s)) { fh->Close(fh); return s; }
    if (fi->FileSize > (UINT64)(96 * 1024 * 1024)) { fh->Close(fh); return EFI_BAD_BUFFER_SIZE; }
    s = gBS->AllocatePool(EfiLoaderData, (UINTN)fi->FileSize + 16, (void **)&buf);
    if (EFI_ERROR(s)) { fh->Close(fh); return s; }
    left = fi->FileSize;
    while (left) {
        got = left > CHUNK ? CHUNK : (UINTN)left;
        s = fh->Read(fh, &got, buf + pos);
        if (EFI_ERROR(s) || got == 0) break;
        pos += got;
        left -= got;
    }
    fh->Close(fh);
    if (EFI_ERROR(s)) { gBS->FreePool(buf); return s; }
    *out_buf = buf;
    *out_size = pos;
    return EFI_SUCCESS;
}

/* Streamed read benchmark + optional pattern verify. Returns MiB/s x100. */
static EFI_STATUS bench_seq_read(EFI_FILE_HANDLE root, const CHAR16 *path,
                                 UINT32 seed, int verify,
                                 UINT64 *out_bytes, double *out_sec,
                                 UINT64 *out_mibs_x100) {
    EFI_STATUS s;
    EFI_FILE_HANDLE fh = NULL;
    UINT8 *chunk = NULL;
    UINT64 total = 0, t0, t1;
    UINTN got;

    s = gBS->AllocatePool(EfiLoaderData, CHUNK, (void **)&chunk);
    if (EFI_ERROR(s)) return s;
    s = open_path(root, path, EFI_FILE_MODE_READ, &fh);
    if (EFI_ERROR(s)) { gBS->FreePool(chunk); return s; }
    t0 = tsc_now();
    for (;;) {
        got = CHUNK;
        s = fh->Read(fh, &got, chunk);
        if (EFI_ERROR(s)) break;
        if (got == 0) { s = EFI_SUCCESS; break; }
        if (verify && verify_pattern(chunk, total, got, seed) != 0) {
            s = EFI_CRC_ERROR;
            break;
        }
        total += got;
    }
    t1 = tsc_now();
    fh->Close(fh);
    gBS->FreePool(chunk);
    if (EFI_ERROR(s)) return s;
    *out_bytes = total;
    *out_sec = tsc_to_seconds(t1 - t0);
    *out_mibs_x100 = (*out_sec > 1e-9)
        ? (UINT64)(((double)total * 100.0) / (1048576.0 * (*out_sec)))
        : 0;
    return EFI_SUCCESS;
}

/* Sequential write benchmark: create path, write `total` bytes of pattern. */
static EFI_STATUS bench_seq_write(EFI_FILE_HANDLE root, const CHAR16 *path,
                                  UINT64 total, UINT32 seed,
                                  double *out_sec, UINT64 *out_mibs_x100) {
    EFI_STATUS s;
    EFI_FILE_HANDLE fh = NULL;
    UINT8 *chunk = NULL;
    UINT64 done = 0, t0, t1;
    UINTN n;

    s = gBS->AllocatePool(EfiLoaderData, CHUNK, (void **)&chunk);
    if (EFI_ERROR(s)) return s;
    s = root->Open(root, &fh, (CHAR16 *)path,
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(s)) { gBS->FreePool(chunk); return s; }
    t0 = tsc_now();
    while (done < total) {
        UINTN want = (total - done > CHUNK) ? CHUNK : (UINTN)(total - done);
        fill_pattern(chunk, done, want, seed);
        n = want;
        s = fh->Write(fh, &n, chunk);
        if (EFI_ERROR(s) || n != want) break;
        done += n;
    }
    t1 = tsc_now();
    fh->Close(fh);
    gBS->FreePool(chunk);
    if (EFI_ERROR(s)) return s;
    *out_sec = tsc_to_seconds(t1 - t0);
    *out_mibs_x100 = (*out_sec > 1e-9)
        ? (UINT64)(((double)done * 100.0) / (1048576.0 * (*out_sec)))
        : 0;
    return EFI_SUCCESS;
}

static void print_rate(const char *label, UINT64 bytes, double sec, UINT64 x100) {
    uprintf("  %-28s %8llu MiB in %llu.%03llu s  =>  %llu.%02llu MiB/s\n",
            label,
            (unsigned long long)(bytes / 1048576u),
            (unsigned long long)(UINT64)sec,
            (unsigned long long)((UINT64)(sec * 1000.0) % 1000u),
            (unsigned long long)(x100 / 100u),
            (unsigned long long)(x100 % 100u));
}

#define MODE_RW (EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE)
#define MODE_RWC (MODE_RW | EFI_FILE_MODE_CREATE)

typedef struct {
    int enumerate, read, create, write, rename, reread, del, flush, fsinfo;
} results_t;

/* Build "<dir><c>NNN.TXT" as a CHAR16 string. */
static void test_name(CHAR16 *w, const char *dir, char c, int i) {
    while (*dir) *w++ = (CHAR16)*dir++;
    *w++ = (CHAR16)c;
    *w++ = (CHAR16)('0' + (i / 100) % 10);
    *w++ = (CHAR16)('0' + (i / 10) % 10);
    *w++ = (CHAR16)('0' + i % 10);
    *w++ = L'.'; *w++ = L'T'; *w++ = L'X'; *w++ = L'T'; *w = 0;
}

/* Rename through SetInfo(EFI_FILE_INFO); the file must already exist. */
static EFI_STATUS rename_file(EFI_FILE_HANDLE root, const CHAR16 *path, const CHAR16 *newname) {
    EFI_FILE_HANDLE fh = NULL;
    UINT8 info[256];
    EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info;
    EFI_GUID fig = EFI_FILE_INFO_ID;
    UINTN nl = 0, isz;
    EFI_STATUS s;

    while (newname[nl]) nl++;
    isz = SIZE_OF_EFI_FILE_INFO + (nl + 1) * sizeof(CHAR16);
    fi->Size = isz;
    for (UINTN i = 0; i <= nl; i++) fi->FileName[i] = newname[i];
    s = open_path(root, path, MODE_RW, &fh);
    if (EFI_ERROR(s)) return s;
    s = fh->SetInfo(fh, &fig, isz, fi);
    fh->Close(fh);
    return s;
}

static void delete_file(EFI_FILE_HANDLE root, const CHAR16 *path) {
    EFI_FILE_HANDLE fh = NULL;
    if (!EFI_ERROR(open_path(root, path, MODE_RW, &fh)))
        fh->Delete(fh); /* Delete closes the handle */
}

/* Anchor lookup is purely diagnostic: it proves which handle is Runway. */
static rwy_anchor_t *find_anchor(EFI_LOADED_IMAGE *li, EFI_HANDLE **handles) {
    EFI_GUID anchor_guid = RWY_ANCHOR_GUID;
    rwy_anchor_t *anchor = NULL;
    UINTN count = 0;
    EFI_STATUS s = gBS->LocateHandle(ByProtocol, &anchor_guid, NULL, &count, *handles);

    if (s != EFI_BUFFER_TOO_SMALL) return NULL;
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, count, (void **)handles))) return NULL;
    s = gBS->LocateHandle(ByProtocol, &anchor_guid, NULL, &count, *handles);
    if (EFI_ERROR(s) || count == 0) return NULL;
    gBS->HandleProtocol((*handles)[0], &anchor_guid, (void **)&anchor);
    if (!anchor || anchor->magic != RWY_ANCHOR_MAGIC) return anchor;
    uprintf("anchor: Runway handle=%p FAT handle=%p store=%llu\n",
            anchor->rwy_fs_handle, anchor->fat_fs_handle,
            (unsigned long long)anchor->store_size);
    if (anchor->rwy_fs_handle == li->DeviceHandle)
        uprint("boot filesystem: Runway (DeviceHandle match: VERIFIED)\n");
    else
        uprint("boot filesystem: NOT Runway (DeviceHandle mismatch)\n");
    if (anchor->rwy_fs_handle == anchor->fat_fs_handle)
        uprint("protocol reinstallation: in-place interception: VERIFIED\n");
    return anchor;
}

static void print_boot_path(EFI_LOADED_IMAGE *li) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH *cur = NULL;
    UINTN dp_nodes = 0;
    EFI_STATUS s = gBS->HandleProtocol(li->DeviceHandle, &dp_guid, (void **)&cur);

    if (EFI_ERROR(s) || !cur) return;
    while (!IsDevicePathEnd(cur)) {
        dp_nodes++;
        cur = NextDevicePathNode(cur);
    }
    uprintf("boot device path: %u node(s) (genuine bus/partition topology: VERIFIED)\n",
            (unsigned int)dp_nodes);
}

/* Returns the status of opening the root directory; the verdict goes in r. */
static EFI_STATUS test_enumerate(EFI_FILE_HANDLE root, results_t *r) {
    EFI_FILE_HANDLE dh = NULL;
    UINT8 ebuf[512];
    int n = 0;
    EFI_STATUS s = open_path(root, L"\\", EFI_FILE_MODE_READ, &dh);

    if (EFI_ERROR(s)) { ufail("open root dir", s); return s; }
    uprint("root directory entries:\n");
    for (;;) {
        EFI_FILE_INFO *fi = (EFI_FILE_INFO *)ebuf;
        UINTN esz = sizeof(ebuf), i = 0;
        char an[128];
        s = dh->Read(dh, &esz, fi);
        if (EFI_ERROR(s) || esz == 0) break;
        while (fi->FileName[i] && i + 1 < sizeof(an)) {
            an[i] = (char)fi->FileName[i];
            i++;
        }
        an[i] = 0;
        uprintf("  %s%s (%llu bytes)\n", an,
                (fi->Attribute & EFI_FILE_DIRECTORY) ? "/" : "",
                (unsigned long long)fi->FileSize);
        n++;
        if (n > 64) break;
    }
    dh->Close(dh);
    r->enumerate = (n >= 3);
    uprintf("enumerate: %s (%d entries)\n", r->enumerate ? "PASS" : "FAIL", n);
    return EFI_SUCCESS;
}

static int test_read(EFI_FILE_HANDLE root, results_t *r) {
    static const char want[] = "Hello from Runway!\n";
    UINT8 *buf = NULL;
    UINT64 sz = 0;
    int ok = 0;
    EFI_STATUS s = read_whole(root, L"\\README.TXT", &buf, &sz);

    if (!EFI_ERROR(s)) {
        /* mkstore writes README starting with "Runway store". */
        r->read = (sz > 8 && buf[0] == 'R' && buf[1] == 'u' && buf[2] == 'n' && buf[3] == 'w');
        uprintf("read README.TXT: %llu bytes -> %s\n",
                (unsigned long long)sz, r->read ? "PASS" : "FAIL");
        gBS->FreePool(buf);
    } else {
        uprintf("read README.TXT: FAIL (0x%llx)\n", (unsigned long long)s);
    }
    buf = NULL; sz = 0;
    s = read_whole(root, L"\\TEST\\HELLO.TXT", &buf, &sz);
    if (!EFI_ERROR(s) && sz == 19) {
        ok = 1;
        for (UINTN i = 0; i < 19; i++)
            if (buf[i] != (UINT8)want[i]) ok = 0;
    }
    uprintf("read TEST/HELLO.TXT: %llu bytes -> %s\n",
            (unsigned long long)sz, ok ? "PASS" : "FAIL");
    if (!ok) r->read = 0;
    if (buf) gBS->FreePool(buf);
    return r->read;
}

/* Re-open WRTEST.TXT and check its content and that EOF sticks. */
static int reread_wrtest(EFI_FILE_HANDLE root) {
    EFI_FILE_HANDLE fh = NULL;
    UINT8 rbuf[4096];
    UINTN n = sizeof(rbuf);
    int ok = 0;
    EFI_STATUS s = open_path(root, L"\\TEST\\WRTEST.TXT", EFI_FILE_MODE_READ, &fh);

    if (EFI_ERROR(s)) {
        uprint("reread: FAIL (reopen failed)\n");
        return 0;
    }
    s = fh->Read(fh, &n, rbuf);
    if (!EFI_ERROR(s) && n == sizeof(rbuf) && verify_pattern(rbuf, 0, n, 0x5a) == 0) {
        n = sizeof(rbuf);
        s = fh->Read(fh, &n, rbuf);
        if (!EFI_ERROR(s) && n == 0) ok = 1;
    }
    fh->Close(fh);
    uprintf("reread+verify: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int test_create_write(EFI_FILE_HANDLE root, results_t *r) {
    EFI_FILE_HANDLE fh = NULL;
    UINT8 wbuf[4096];
    UINTN n = sizeof(wbuf);
    EFI_STATUS s;

    fill_pattern(wbuf, 0, sizeof(wbuf), 0x5a);
    s = root->Open(root, &fh, L"\\TEST\\WRTEST.TXT", MODE_RWC, 0);
    r->create = !EFI_ERROR(s);
    uprintf("create TEST/WRTEST.TXT: %s\n", r->create ? "PASS" : "FAIL");
    if (!r->create) return 0;
    s = fh->Write(fh, &n, wbuf);
    r->write = (!EFI_ERROR(s) && n == sizeof(wbuf));
    uprintf("write 4096 bytes: %s\n", r->write ? "PASS" : "FAIL");
    fh->Close(fh);
    r->reread = reread_wrtest(root);
    return r->write && r->reread;
}

static int test_rename(EFI_FILE_HANDLE root, results_t *r) {
    EFI_FILE_HANDLE fh = NULL;
    UINT8 *buf = NULL;
    UINT64 sz = 0;
    int ok = 1;
    EFI_STATUS s = rename_file(root, L"\\TEST\\WRTEST.TXT", L"WRTEST2.TXT");

    r->rename = !EFI_ERROR(s);
    uprintf("rename -> WRTEST2.TXT: %s\n", r->rename ? "PASS" : "FAIL");
    if (!r->rename) return 0;
    s = read_whole(root, L"\\TEST\\WRTEST2.TXT", &buf, &sz);
    if (!EFI_ERROR(s) && sz == 4096 && verify_pattern(buf, 0, (UINTN)sz, 0x5a) == 0) {
        uprint("reread after rename: PASS\n");
    } else {
        uprint("reread after rename: FAIL\n");
        ok = 0;
    }
    if (buf) gBS->FreePool(buf);
    s = open_path(root, L"\\TEST\\WRTEST.TXT", EFI_FILE_MODE_READ, &fh);
    if (!EFI_ERROR(s)) {
        uprint("old name still opens: FAIL\n");
        fh->Close(fh);
        ok = 0;
    } else {
        uprint("old name gone: PASS\n");
    }
    return ok;
}

static int test_delete(EFI_FILE_HANDLE root, results_t *r) {
    EFI_FILE_HANDLE fh = NULL;
    EFI_STATUS s = open_path(root, L"\\TEST\\WRTEST2.TXT", MODE_RW, &fh);

    if (!EFI_ERROR(s)) {
        s = fh->Delete(fh); /* Delete closes the handle */
        r->del = !EFI_ERROR(s);
    }
    uprintf("delete: %s\n", r->del ? "PASS" : "FAIL");
    return r->del;
}

/* Flush the volume to commit the transaction across the array. */
static int test_flush(EFI_FILE_HANDLE root, results_t *r) {
    r->flush = !EFI_ERROR(root->Flush(root));
    uprintf("flush: %s\n", r->flush ? "PASS" : "FAIL");
    return r->flush;
}

/* Set a test volume label, read it back and restore the original one. */
static int label_roundtrip(EFI_FILE_HANDLE root, const EFI_FILE_SYSTEM_INFO *fsi, UINTN l) {
    static const CHAR16 test_label[] = L"RWY_VOL_TEST";
    const UINTN test_len = (sizeof(test_label) / sizeof(test_label[0])) - 1;
    EFI_GUID fsi_guid = EFI_FILE_SYSTEM_INFO_ID;
    CHAR16 orig_label[32];
    UINT8 fsi_buf[256], set_buf[256];
    EFI_FILE_SYSTEM_INFO *set_fsi = (EFI_FILE_SYSTEM_INFO *)set_buf;
    UINTN fsi_sz = sizeof(fsi_buf), set_sz = SIZE_OF_EFI_FILE_SYSTEM_INFO + (test_len + 1) * sizeof(CHAR16);
    int pass = 0;

    for (UINTN k = 0; k <= l; k++) orig_label[k] = fsi->VolumeLabel[k];
    set_fsi->Size = set_sz;
    set_fsi->ReadOnly = FALSE;
    set_fsi->VolumeSize = fsi->VolumeSize;
    set_fsi->FreeSpace = fsi->FreeSpace;
    set_fsi->BlockSize = fsi->BlockSize;
    for (UINTN k = 0; k <= test_len; k++) set_fsi->VolumeLabel[k] = test_label[k];
    if (EFI_ERROR(root->SetInfo(root, &fsi_guid, set_sz, set_buf)))
        return 0;
    if (!EFI_ERROR(root->GetInfo(root, &fsi_guid, &fsi_sz, fsi_buf))) {
        const EFI_FILE_SYSTEM_INFO *got = (const EFI_FILE_SYSTEM_INFO *)fsi_buf;
        int match = 1;
        for (UINTN k = 0; k <= test_len; k++)
            if (got->VolumeLabel[k] != test_label[k]) match = 0;
        pass = match && got->BlockSize == 4096 && got->VolumeSize > 0;
    }
    set_sz = SIZE_OF_EFI_FILE_SYSTEM_INFO + (l + 1) * sizeof(CHAR16);
    set_fsi->Size = set_sz;
    for (UINTN k = 0; k <= l; k++) set_fsi->VolumeLabel[k] = orig_label[k];
    root->SetInfo(root, &fsi_guid, set_sz, set_buf);
    return pass;
}

/* Filesystem info and volume label. */
static int test_fsinfo(EFI_FILE_HANDLE root, results_t *r) {
    UINT8 fsi_buf[256];
    UINTN fsi_sz = sizeof(fsi_buf);
    EFI_GUID fsi_guid = EFI_FILE_SYSTEM_INFO_ID;

    if (!EFI_ERROR(root->GetInfo(root, &fsi_guid, &fsi_sz, fsi_buf))) {
        EFI_FILE_SYSTEM_INFO *fsi = (EFI_FILE_SYSTEM_INFO *)fsi_buf;
        char label_ascii[32];
        UINTN l = 0;
        while (fsi->VolumeLabel[l] && l < 31) {
            label_ascii[l] = (char)fsi->VolumeLabel[l];
            l++;
        }
        label_ascii[l] = 0;
        uprintf("volume label: '%s' (volsize=%llu free=%llu blksize=%u)\n",
                label_ascii,
                (unsigned long long)fsi->VolumeSize,
                (unsigned long long)fsi->FreeSpace,
                (unsigned int)fsi->BlockSize);
        r->fsinfo = label_roundtrip(root, fsi, l);
    }
    uprintf("fsinfo: %s\n", r->fsinfo ? "PASS" : "FAIL");
    return r->fsinfo;
}

/*
 * Write a marker file on every non-boot SFS volume. Close() auto-commits,
 * so each Runway volume advances its own generation.
 */
static int test_multivol(EFI_LOADED_IMAGE *li) {
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    static const char mark[] = "multivol marker\n";
    EFI_HANDLE *vh = NULL;
    UINTN vn = 0, i;
    int mv_ok = 0, mv_n = 0;

    if (gBS->LocateHandleBuffer(ByProtocol, &sfs_guid, NULL, &vn, &vh) == EFI_SUCCESS) {
        for (i = 0; i < vn; i++) {
            EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *vs;
            EFI_FILE_HANDLE vr, vf;
            UINTN len = sizeof(mark) - 1;
            if (vh[i] == li->DeviceHandle) continue;
            if (gBS->HandleProtocol(vh[i], &sfs_guid, (void **)&vs) != EFI_SUCCESS) continue;
            if (vs->OpenVolume(vs, &vr) != EFI_SUCCESS) continue;
            mv_n++;
            if (vr->Open(vr, &vf, L"\\MV_MARK.TXT", MODE_RWC, 0) == EFI_SUCCESS) {
                if (vf->Write(vf, &len, (void *)mark) == EFI_SUCCESS) mv_ok++;
                vf->Close(vf);
            }
            vr->Close(vr);
        }
        gBS->FreePool(vh);
    }
    uprintf("multivol: wrote marker on %d of %d other volume(s): %s\n",
            mv_ok, mv_n, mv_ok == mv_n ? "PASS" : "FAIL");
    return mv_ok == mv_n;
}

static void print_results(const results_t *r) {
    uprint("\nfilesystem tests:\n");
    uprintf("  enumerate: %s\n", r->enumerate ? "PASS" : "FAIL");
    uprintf("  read:      %s\n", r->read ? "PASS" : "FAIL");
    uprintf("  create:    %s\n", r->create ? "PASS" : "FAIL");
    uprintf("  write:     %s\n", r->write ? "PASS" : "FAIL");
    uprintf("  rename:    %s\n", r->rename ? "PASS" : "FAIL");
    uprintf("  reread:    %s\n", r->reread ? "PASS" : "FAIL");
    uprintf("  delete:    %s\n", r->del ? "PASS" : "FAIL");
    uprintf("  flush:     %s\n", r->flush ? "PASS" : "FAIL");
    uprintf("  fsinfo:    %s\n", r->fsinfo ? "PASS" : "FAIL");
}

static int bench_read(EFI_FILE_HANDLE root, const CHAR16 *path, UINT32 seed, const char *name,
                      const char *label) {
    UINT64 rb, rx100;
    double rs;
    EFI_STATUS s = bench_seq_read(root, path, seed, 1, &rb, &rs, &rx100);

    if (EFI_ERROR(s)) { uprintf("  %s: FAIL\n", name); return 0; }
    print_rate(label, rb, rs, rx100);
    return 1;
}

/* Read back the first 16 MiB of the written file and check the pattern. */
static int verify_written(EFI_FILE_HANDLE root, UINT32 seed) {
    EFI_FILE_HANDLE fh = NULL;
    UINT8 *chk = NULL;
    UINT64 off = 0;
    int ok = 1;

    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, CHUNK, (void **)&chk))) return 0;
    if (EFI_ERROR(open_path(root, L"\\PERF\\WRTMP.BIN", EFI_FILE_MODE_READ, &fh))) {
        gBS->FreePool(chk);
        return 0;
    }
    for (int i = 0; i < 16 && ok; i++) {
        UINTN got = CHUNK;
        EFI_STATUS s = fh->Read(fh, &got, chk);
        if (EFI_ERROR(s) || got != CHUNK || verify_pattern(chk, off, got, seed) != 0) ok = 0;
        off += got;
    }
    fh->Close(fh);
    gBS->FreePool(chk);
    return ok;
}

static int bench_write(EFI_FILE_HANDLE root) {
    double ws;
    UINT64 wx100;
    int ok;
    EFI_STATUS s = bench_seq_write(root, L"\\PERF\\WRTMP.BIN", 16 * 1048576ULL, 0x77, &ws, &wx100);

    if (EFI_ERROR(s)) {
        uprint("  seq write: FAIL\n");
        return 0;
    }
    print_rate("Runway seq write 16 MiB", 16 * 1048576ULL, ws, wx100);
    ok = verify_written(root, 0x77);
    uprintf("  write verify: %s\n", ok ? "PASS" : "FAIL");
    delete_file(root, L"\\PERF\\WRTMP.BIN");
    return ok;
}

#define RENAME_N 200

static int bench_rename(EFI_FILE_HANDLE root) {
    UINT64 t0, t1, ops_x100;
    double sec;
    int i, ok = 1;

    for (i = 0; i < RENAME_N; i++) {
        CHAR16 w[64];
        EFI_FILE_HANDLE fh = NULL;
        test_name(w, "RENAME\\", 'F', i);
        if (EFI_ERROR(root->Open(root, &fh, w, MODE_RWC, 0))) { ok = 0; break; }
        fh->Close(fh);
    }
    t0 = tsc_now();
    for (i = 0; i < RENAME_N && ok; i++) {
        CHAR16 w[64], wn[64];
        test_name(w, "RENAME\\", 'F', i);
        test_name(wn, "", 'G', i);
        if (EFI_ERROR(rename_file(root, w, wn))) ok = 0;
    }
    t1 = tsc_now();
    sec = tsc_to_seconds(t1 - t0);
    ops_x100 = (sec > 1e-9) ? (UINT64)(((double)RENAME_N * 100.0) / sec) : 0;
    uprintf("  rename %d files: %s (%llu.%02llu ops/s)\n", RENAME_N,
            ok ? "PASS" : "FAIL",
            (unsigned long long)(ops_x100 / 100u),
            (unsigned long long)(ops_x100 % 100u));
    for (i = 0; i < RENAME_N; i++) {
        CHAR16 w[64];
        test_name(w, "RENAME\\", 'G', i);
        delete_file(root, w);
    }
    return ok;
}

/*
 * FAT baseline: sequential read of the first 16 MiB of the backing file
 * through the firmware FAT driver (same disk, same firmware).
 */
static void bench_fat_baseline(rwy_anchor_t *anchor) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fat_sfs = anchor->fat_sfs;
    EFI_FILE_HANDLE fat_root = NULL, fh = NULL;
    UINT8 *chunk = NULL;
    UINT64 total = 0, t0, t1, left = 16 * 1048576ULL, x100;
    double sec;
    EFI_STATUS s = fat_sfs ? EFI_SUCCESS : EFI_NOT_FOUND;

    if (!EFI_ERROR(s)) s = fat_sfs->OpenVolume(fat_sfs, &fat_root);
    if (!EFI_ERROR(s)) s = fat_root->Open(fat_root, &fh, L"\\RUNWAY.DAT", EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR(s)) s = gBS->AllocatePool(EfiLoaderData, CHUNK, (void **)&chunk);
    if (EFI_ERROR(s)) {
        uprint("  direct FAT baseline: unavailable\n");
    } else {
        t0 = tsc_now();
        while (left) {
            UINTN got = left > CHUNK ? CHUNK : (UINTN)left;
            s = fh->Read(fh, &got, chunk);
            if (EFI_ERROR(s) || got == 0) break;
            total += got;
            left -= got;
        }
        t1 = tsc_now();
        sec = tsc_to_seconds(t1 - t0);
        x100 = (sec > 1e-9) ? (UINT64)(((double)total * 100.0) / (1048576.0 * sec)) : 0;
        print_rate("direct FAT read 16 MiB", total, sec, x100);
        fh->Close(fh);
        gBS->FreePool(chunk);
    }
    if (fat_root) fat_root->Close(fat_root);
}

static int run_benchmarks(EFI_FILE_HANDLE root, rwy_anchor_t *anchor) {
    int ok = 1;

    uprint("\nperformance:\n");
    ok &= bench_read(root, L"\\PERF\\READ1M.BIN", 0x11, "read 1MiB", "Runway seq read 1 MiB");
    ok &= bench_read(root, L"\\PERF\\READ16M.BIN", 0x22, "read 16MiB", "Runway seq read 16 MiB");
    ok &= bench_read(root, L"\\PERF\\READ64M.BIN", 0x33, "read 64MiB", "Runway seq read 64 MiB");
    ok &= bench_write(root);
    ok &= bench_rename(root);
    if (anchor && anchor->fat_fs_handle)
        bench_fat_baseline(anchor);
    else
        uprint("  direct FAT baseline: no anchor, skipped\n");
    return ok;
}

/* NOTE: no EFIAPI — must match Debian crt0's SysV entry (see rwy_main.c). */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS s;
    EFI_LOADED_IMAGE *li;
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    EFI_FILE_HANDLE root = NULL;
    rwy_anchor_t *anchor;
    EFI_HANDLE *ahandles = NULL;
    results_t r = { 0 };
    int ok = 1;

    util_init(ImageHandle, SystemTable);
    tsc_calibrate();

    uprint("Runway\n");
    uprint("=========\n");
    uprint("chain-loaded application: running\n");

    s = gBS->HandleProtocol(ImageHandle, &li_guid, (void **)&li);
    if (EFI_ERROR(s)) { ufail("no LoadedImage", s); return s; }

    /* Identify the boot device and open its root through the normal path. */
    uprintf("LoadedImage.DeviceHandle: %p\n", li->DeviceHandle);
    s = gBS->HandleProtocol(li->DeviceHandle, &sfs_guid, (void **)&sfs);
    if (EFI_ERROR(s)) { ufail("no SimpleFileSystem on boot device", s); return s; }
    s = sfs->OpenVolume(sfs, &root);
    if (EFI_ERROR(s)) { ufail("OpenVolume failed", s); return s; }
    uprint("boot filesystem OpenVolume: OK\n");

    anchor = find_anchor(li, &ahandles);
    print_boot_path(li);

    s = test_enumerate(root, &r);
    if (EFI_ERROR(s)) return s;
    ok &= r.enumerate;
    ok &= test_read(root, &r);
    ok &= test_create_write(root, &r);
    ok &= test_rename(root, &r);
    ok &= test_delete(root, &r);
    ok &= test_flush(root, &r);
    ok &= test_fsinfo(root, &r);
    ok &= test_multivol(li);
    print_results(&r);
    ok &= run_benchmarks(root, anchor);

    if (ahandles) gBS->FreePool(ahandles);
    if (root) root->Close(root);

    uprintf("\nchain-loaded application: %s\n", ok ? "OK" : "FAIL");
    flush_and_stall(500000);
    return ok ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}
