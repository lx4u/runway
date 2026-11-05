/*
 * iobench — time the same extent pattern through EFI_FILE_PROTOCOL and
 * through EFI_BLOCK_IO, and through Runway on the file and on the partition,
 * on every prepared disk and one firmware.
 * SPDX-License-Identifier: Apache-2.0
 *
 * A disk is prepared when it has the benchmark GUIDs and a valid PERF/READY
 * marker. The file is IOBENCH.DAT on the volume the image was loaded from. The
 * raw area is in the GPT partition whose unique GUID is IOBENCH_PART_GUID,
 * after the store that fills its first IOBENCH_RAW_BASE bytes. Runway runs on
 * that store and on an encrypted store of the partition IOBENCH_ENC_GUID: no
 * other partition is ever written. Each run size (4 KiB, 64 KiB, 1 MiB,
 * 16 MiB) writes and reads back the same 16 MiB, then the data is checked.
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"
#include "iobench.h"
#include "rwy_crypto.h"
#define MIB_X100(bytes, sec) ((sec) > 1e-9 ? (UINT64)((double)(bytes) * 100.0 / (1048576.0 * (sec))) : 0)

/* Same value as the partition made by tests/qemu/mkiobench.sh. */
#define IOBENCH_PART_GUID \
    { 0x5d0a7d6e, 0x3b1c, 0x4c1e, { 0x9a, 0x42, 0x6f, 0x1b, 0x8e, 0x70, 0x21, 0xc5 } }
#define IOBENCH_ENC_GUID \
    { 0x5d0a7d6e, 0x3b1c, 0x4c1e, { 0x9a, 0x42, 0x6f, 0x1b, 0x8e, 0x70, 0x21, 0xc6 } }

/* The store of mkiobench.sh takes this many bytes from the start of the partition. */
#define IOBENCH_RAW_BASE (96ull * 1048576u)

#define MAX_DISKS 16
#define DISK_REASON 96

typedef struct {
    EFI_HANDLE part;
    EFI_HANDLE enc;
    EFI_HANDLE parent;
    EFI_HANDLE esp;
    bench_disk_info_t info;
    int is_boot;
    int is_usb;
    int use;
    int ambiguous;
    char reason[DISK_REASON];
    char note[DISK_REASON];
} bench_disk_t;

static const UINTN run_sizes[] = { 4096, 65536, 1048576, AREA };

/* ---------- block backend ---------- */

static EFI_STATUS block_write(bench_dev_t *d, UINT64 off, UINTN len, void *buf) {
    EFI_BLOCK_IO_PROTOCOL *b = d->bio;
    return b->WriteBlocks(b, b->Media->MediaId, (d->base + off) / b->Media->BlockSize, len, buf);
}

static EFI_STATUS block_read(bench_dev_t *d, UINT64 off, UINTN len, void *buf) {
    EFI_BLOCK_IO_PROTOCOL *b = d->bio;
    return b->ReadBlocks(b, b->Media->MediaId, (d->base + off) / b->Media->BlockSize, len, buf);
}

static EFI_STATUS block_flush(bench_dev_t *d) {
    return d->bio->FlushBlocks(d->bio);
}

/* ---------- file backend ---------- */

static EFI_STATUS file_seek(bench_dev_t *d, UINT64 off) {
    EFI_STATUS s = EFI_SUCCESS;
    if (d->pos != off) s = d->fh->SetPosition(d->fh, off);
    d->pos = off;
    return s;
}

static EFI_STATUS file_write(bench_dev_t *d, UINT64 off, UINTN len, void *buf) {
    UINTN n = len;
    EFI_STATUS s = file_seek(d, off);
    if (!EFI_ERROR(s)) s = d->fh->Write(d->fh, &n, buf);
    if (!EFI_ERROR(s) && n != len) s = EFI_DEVICE_ERROR;
    d->pos += len;
    return s;
}

static EFI_STATUS file_read(bench_dev_t *d, UINT64 off, UINTN len, void *buf) {
    UINTN n = len;
    EFI_STATUS s = file_seek(d, off);
    if (!EFI_ERROR(s)) s = d->fh->Read(d->fh, &n, buf);
    if (!EFI_ERROR(s) && n != len) s = EFI_DEVICE_ERROR;
    d->pos += len;
    return s;
}

static EFI_STATUS file_flush(bench_dev_t *d) {
    return d->fh->Flush(d->fh);
}

/* ---------- data pattern ---------- */

void bench_fill_pattern(UINT8 *buf, UINTN seed) {
    UINT64 *p = (UINT64 *)buf;
    UINTN i;
    for (i = 0; i < AREA / 8; i++) p[i] = ((UINT64)i + seed * 0x10001ULL + 1) * 0x9E3779B97F4A7C15ULL;
}

UINT64 bench_first_mismatch(const UINT8 *a, const UINT8 *b) {
    UINT64 i;
    for (i = 0; i < AREA; i++)
        if (a[i] != b[i]) return i;
    return AREA;
}

/* ---------- timing ---------- */

void bench_sample_emit(const bench_dev_t *d, const char *op, UINTN run, UINT64 ticks,
                       UINT64 flush_ticks, int ok) {
    bench_measure_t m = { op, run, AREA, 0, 0, 0, 0, 0, ok };

    bench_calls_get(d, op[0] == 'w', &m.calls_core, &m.calls_dev);
    if (!d->rwy) m.calls_dev = run ? AREA / run : 0;
    m.seconds_us = (UINT64)(tsc_to_seconds(ticks) * 1e6 + 0.5);
    m.flush_us = (UINT64)(tsc_to_seconds(flush_ticks) * 1e6 + 0.5);
    bench_result_add_sample(d, &m);
}

void bench_work_emit(const bench_dev_t *d, const char *op, UINT64 bytes, UINT64 extents,
                     UINT64 ticks, UINT64 flush_ticks, EFI_STATUS s) {
    bench_measure_t m = { op, 0, bytes, extents, 0, 0, 0, 0, !EFI_ERROR(s) };
    double sec = tsc_to_seconds(ticks);

    bench_calls_all(d, &m.calls_core, &m.calls_dev);
    m.seconds_us = (UINT64)(sec * 1e6 + 0.5);
    m.flush_us = (UINT64)(tsc_to_seconds(flush_ticks) * 1e6 + 0.5);
    bench_result_add_sample(d, &m);
    uprintf("IOBENCH work %-8s %-9s: %s %llu us, flush %llu us, %llu extents\n", d->name, op,
            EFI_ERROR(s) ? "FAIL" : "ok", (unsigned long long)m.seconds_us,
            (unsigned long long)m.flush_us, (unsigned long long)extents);
}

void bench_report(const char *dev, const char *op, UINTN run, UINT64 ticks, EFI_STATUS s) {
    double sec = tsc_to_seconds(ticks);
    UINT64 x = MIB_X100(AREA, sec);
    UINT64 calls = AREA / run;
    UINT64 us_x10 = (calls && sec > 0) ? (UINT64)(sec * 1e7 / (double)calls) : 0;

    if (EFI_ERROR(s)) {
        uprintf("IOBENCH %-8s %-5s %4u KiB: FAIL status 0x%llx\n", dev, op,
                (unsigned)(run / 1024), (unsigned long long)s);
        return;
    }
    uprintf("IOBENCH %-8s %-5s %4u KiB: %llu.%03llu s  %llu.%02llu MiB/s  %llu calls  %llu.%llu us/call\n",
            dev, op, (unsigned)(run / 1024), (unsigned long long)(UINT64)sec,
            (unsigned long long)((UINT64)(sec * 1000.0) % 1000u), (unsigned long long)(x / 100u),
            (unsigned long long)(x % 100u), (unsigned long long)calls, (unsigned long long)(us_x10 / 10u),
            (unsigned long long)(us_x10 % 10u));
}

static EFI_STATUS time_runs(bench_dev_t *d, int writing, UINTN run, UINT8 *buf,
                            UINT64 *ticks, UINT64 *flush_ticks) {
    UINT64 off, t0;
    EFI_STATUS s = EFI_SUCCESS;

    *flush_ticks = 0;
    bench_calls_reset(d);
    t0 = tsc_now();
    for (off = 0; off < AREA && !EFI_ERROR(s); off += run)
        s = writing ? d->write(d, off, run, buf + off) : d->read(d, off, run, buf + off);
    if (writing && !EFI_ERROR(s)) {
        UINT64 f0 = tsc_now();

        s = d->flush(d);
        *flush_ticks = tsc_now() - f0;
    }
    *ticks = tsc_now() - t0;
    return s;
}

/* One run size: write, read back, check. Returns 1 when all three passed. */
static int bench_one(bench_dev_t *d, UINTN run, UINTN seed, UINT8 *want, UINT8 *got) {
    UINT64 ticks, flush_ticks, bad;
    EFI_STATUS s;

    bench_fill_pattern(want, seed);
    s = time_runs(d, 1, run, want, &ticks, &flush_ticks);
    bench_report(d->name, "write", run, ticks, s);
    bench_calls_print(d, "write", run);
    bench_sample_emit(d, "write", run, ticks, flush_ticks, !EFI_ERROR(s));
    bench_fw_emit(d, "write-fw", run);
    if (EFI_ERROR(s)) return 0;
    d->pos = (UINT64)-1; /* force a seek before the read */
    s = time_runs(d, 0, run, got, &ticks, &flush_ticks);
    bench_report(d->name, "read", run, ticks, s);
    bench_calls_print(d, "read", run);
    if (EFI_ERROR(s)) {
        bench_sample_emit(d, "read", run, ticks, 0, 0);
        return 0;
    }
    bad = bench_first_mismatch(want, got);
    bench_sample_emit(d, "read", run, ticks, 0, bad == AREA);
    bench_fw_emit(d, "read-fw", run);
    if (bad != AREA) {
        uprintf("IOBENCH %s %u KiB: data differs at offset %llu\n", d->name, (unsigned)(run / 1024),
                (unsigned long long)bad);
        return 0;
    }
    return 1;
}

static int bench_dev(bench_dev_t *d, UINT8 *want, UINT8 *got) {
    int ok = 1;
    UINTN i;
    for (i = 0; i < sizeof(run_sizes) / sizeof(run_sizes[0]); i++)
        ok &= bench_one(d, run_sizes[i], i + (d->bio ? 100 : 0), want, got);
    return ok;
}

/* ---------- finding the devices ---------- */

static int is_bench_partition(EFI_HANDLE h, const EFI_GUID *want) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH *dp = NULL;

    if (EFI_ERROR(gBS->HandleProtocol(h, &dp_guid, (void **)&dp)) || !dp) return 0;
    for (; !IsDevicePathEnd(dp); dp = NextDevicePathNode(dp)) {
        HARDDRIVE_DEVICE_PATH *hd = (HARDDRIVE_DEVICE_PATH *)dp;
        if (DevicePathType(dp) != MEDIA_DEVICE_PATH || DevicePathSubType(dp) != MEDIA_HARDDRIVE_DP) continue;
        if (hd->SignatureType == 2 && guid_eq((EFI_GUID *)hd->Signature, want)) return 1;
    }
    return 0;
}

static EFI_BLOCK_IO_PROTOCOL *partition_bio(EFI_HANDLE h) {
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO_PROTOCOL *bio = NULL;

    if (!h || EFI_ERROR(gBS->HandleProtocol(h, &bio_guid, (void **)&bio))) return NULL;
    return bio;
}

/* The whole-disk block handle whose device path is the prefix of `part`. */
static EFI_HANDLE parent_disk(EFI_HANDLE part) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE *hs = NULL, best = NULL;
    EFI_DEVICE_PATH *pp = NULL, *dp = NULL;
    EFI_BLOCK_IO_PROTOCOL *bio = NULL;
    UINTN n = 0, i, best_len = 0, plen;

    if (!part || EFI_ERROR(gBS->HandleProtocol(part, &dp_guid, (void **)&pp)) || !pp) return NULL;
    plen = DevicePathSize(pp);
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &n, &hs))) return NULL;
    for (i = 0; i < n; i++) {
        UINTN len;

        if (EFI_ERROR(gBS->HandleProtocol(hs[i], &bio_guid, (void **)&bio))) continue;
        if (bio->Media->LogicalPartition) continue;
        if (EFI_ERROR(gBS->HandleProtocol(hs[i], &dp_guid, (void **)&dp)) || !dp) continue;
        len = DevicePathSize(dp);
        if (len < sizeof(EFI_DEVICE_PATH_PROTOCOL) || len > plen) continue;
        len -= sizeof(EFI_DEVICE_PATH_PROTOCOL);
        if (len > best_len && !CompareMem(dp, pp, len)) {
            best = hs[i];
            best_len = len;
        }
    }
    gBS->FreePool(hs);
    return best;
}

/* A FAT volume on the disk: the file cells run there, so they time this disk. */
static EFI_HANDLE find_disk_esp(EFI_HANDLE parent) {
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_HANDLE *hs = NULL, found = NULL;
    UINTN n = 0;

    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &sfs_guid, NULL, &n, &hs))) return NULL;
    for (UINTN i = 0; i < n && !found; i++) {
        EFI_BLOCK_IO_PROTOCOL *bio = partition_bio(hs[i]);

        if (bio && bio->Media->LogicalPartition && parent_disk(hs[i]) == parent) found = hs[i];
    }
    gBS->FreePool(hs);
    return found;
}

static void dev_path_str(EFI_HANDLE h, char *out, UINTN cap) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH *dp = NULL;
    CHAR16 *s = NULL;
    UINTN i;

    out[0] = 0;
    if (!h || EFI_ERROR(gBS->HandleProtocol(h, &dp_guid, (void **)&dp)) || !dp) return;
    s = DevicePathToStr(dp);
    if (!s) return;
    for (i = 0; s[i] && i + 1 < cap; i++) out[i] = s[i] < 128 ? (char)s[i] : '?';
    out[i] = 0;
    gBS->FreePool(s);
}

static void fill_disk_info(bench_disk_info_t *di, EFI_HANDLE disk, EFI_HANDLE fallback) {
    EFI_BLOCK_IO_PROTOCOL *bio;

    if (!disk) disk = fallback;
    dev_path_str(disk, di->devpath, sizeof(di->devpath));
    bio = partition_bio(disk);
    if (!bio) return;
    di->media_id = bio->Media->MediaId;
    di->bytes = (bio->Media->LastBlock + 1) * bio->Media->BlockSize;
    di->block_size = bio->Media->BlockSize;
    di->ioalign = bio->Media->IoAlign;
    di->write_cache = bio->Media->WriteCaching;
}

/* ---------- prepared disks ---------- */

static UINTN text_len(const char *s) {
    UINTN n = 0;

    while (s && s[n]) n++;
    return n;
}

static void text_copy(char *dst, UINTN cap, const char *src) {
    UINTN n = 0;

    while (src && src[n] && n + 1 < cap) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
}

static int text_cmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void disk_reason(bench_disk_t *d, const char *reason) {
    d->use = 0;
    text_copy(d->reason, sizeof(d->reason), reason);
}

static int marker_get(const char *text, const char *key, char *out, UINTN cap) {
    const char *p = text;
    UINTN klen = text_len(key);

    while (*p) {
        const char *end = p;
        UINTN n;

        while (*end && *end != '\n' && *end != '\r') end++;
        n = (UINTN)(end - p);
        if (n > klen && p[klen] == '=' && !CompareMem(p, key, klen)) {
            const char *v = p + klen + 1;
            UINTN vlen = (UINTN)(end - v);

            if (vlen >= cap) vlen = cap - 1;
            for (UINTN i = 0; i < vlen; i++) out[i] = v[i];
            out[vlen] = 0;
            return 1;
        }
        p = end;
        while (*p == '\r' || *p == '\n') p++;
    }
    return 0;
}

static void put_hex(char *out, UINTN *j, UINT32 v, UINTN digits) {
    static const char hex[] = "0123456789ABCDEF";

    for (UINTN i = 0; i < digits; i++) {
        out[(*j)++] = hex[(v >> (4 * (digits - 1 - i))) & 0xF];
    }
}

static void format_gpt_guid(const UINT8 *p, char out[37]) {
    UINT32 a = (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
    UINT32 b = (UINT32)p[4] | ((UINT32)p[5] << 8);
    UINT32 c = (UINT32)p[6] | ((UINT32)p[7] << 8);
    UINTN j = 0;

    put_hex(out, &j, a, 8);
    out[j++] = '-';
    put_hex(out, &j, b, 4);
    out[j++] = '-';
    put_hex(out, &j, c, 4);
    out[j++] = '-';
    put_hex(out, &j, p[8], 2);
    put_hex(out, &j, p[9], 2);
    out[j++] = '-';
    for (UINTN i = 10; i < 16; i++) put_hex(out, &j, p[i], 2);
    out[j] = 0;
}

static int read_disk_guid(EFI_HANDLE disk, char out[37]) {
    EFI_BLOCK_IO_PROTOCOL *bio = partition_bio(disk);
    UINT8 *buf = NULL;
    UINTN bs;

    out[0] = 0;
    if (!bio || !bio->Media || bio->Media->BlockSize < 92) return 0;
    bs = bio->Media->BlockSize;
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, bs, (void **)&buf))) return 0;
    if (!EFI_ERROR(bio->ReadBlocks(bio, bio->Media->MediaId, 1, bs, buf)) &&
        !CompareMem(buf, "EFI PART", 8))
        format_gpt_guid(buf + 56, out);
    gBS->FreePool(buf);
    return out[0] != 0;
}

static int path_is_usb(EFI_HANDLE h) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH *dp = NULL;

    if (!h || EFI_ERROR(gBS->HandleProtocol(h, &dp_guid, (void **)&dp)) || !dp) return 0;
    for (; !IsDevicePathEnd(dp); dp = NextDevicePathNode(dp)) {
        if (DevicePathType(dp) != MESSAGING_DEVICE_PATH) continue;
        if (DevicePathSubType(dp) == MSG_USB_DP ||
            DevicePathSubType(dp) == MSG_USB_WWID_DP ||
            DevicePathSubType(dp) == MSG_USB_CLASS_DP)
            return 1;
    }
    return 0;
}

static int disk_index(const bench_disk_t *list, UINTN n, EFI_HANDLE parent) {
    for (UINTN i = 0; i < n; i++)
        if (list[i].parent == parent) return (int)i;
    return -1;
}

static int add_disk(bench_disk_t *list, UINTN *n, UINTN max, EFI_HANDLE parent, EFI_HANDLE boot_parent) {
    bench_disk_t *d;

    if (*n >= max) return -1;
    d = &list[(*n)++];
    memset(d, 0, sizeof(*d));
    d->parent = parent;
    d->is_boot = parent == boot_parent;
    d->is_usb = path_is_usb(parent);
    if (!d->is_boot) d->esp = find_disk_esp(parent);
    fill_disk_info(&d->info, parent, NULL);
    return (int)(*n - 1);
}

static void collect_disks(EFI_HANDLE boot_parent, bench_disk_t *list, UINTN max, UINTN *n) {
    EFI_GUID plain_guid = IOBENCH_PART_GUID, enc_guid = IOBENCH_ENC_GUID;
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE *hs = NULL;
    UINTN count = 0;

    *n = 0;
    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &count, &hs))) return;
    for (int pass = 0; pass < 2; pass++) {
        const EFI_GUID *want = pass ? &enc_guid : &plain_guid;

        for (UINTN i = 0; i < count; i++) {
            EFI_BLOCK_IO_PROTOCOL *bio = NULL;
            EFI_HANDLE parent;
            int idx;

            if (EFI_ERROR(gBS->HandleProtocol(hs[i], &bio_guid, (void **)&bio))) continue;
            if (!bio->Media->LogicalPartition || !is_bench_partition(hs[i], want)) continue;
            parent = parent_disk(hs[i]);
            if (!parent) continue;
            idx = disk_index(list, *n, parent);
            if (idx < 0) idx = add_disk(list, n, max, parent, boot_parent);
            if (idx < 0) continue;
            if (!pass) {
                if (!list[idx].part) list[idx].part = hs[i];
                else list[idx].ambiguous = 1;
            } else {
                if (!list[idx].enc) list[idx].enc = hs[i];
                else list[idx].ambiguous = 1;
            }
        }
    }
    gBS->FreePool(hs);
}

static void sort_disks(bench_disk_t *list, UINTN n) {
    for (UINTN i = 1; i < n; i++) {
        bench_disk_t tmp = list[i];
        UINTN j = i;

        while (j > 0 && text_cmp(list[j - 1].info.devpath, tmp.info.devpath) > 0) {
            list[j] = list[j - 1];
            j--;
        }
        list[j] = tmp;
    }
}

static int disk_wanted(const bench_config_t *cfg, const bench_disk_t *d) {
    if (cfg->disks == BENCH_DISKS_BOOT) return d->is_boot;
    if (cfg->disks == BENCH_DISKS_ALL) return 1;
    if (d->is_boot) return cfg->include_boot;
    return !d->is_usb;
}

static int check_ready(bench_disk_t *d) {
    char text[256], layout[16], guid[40], actual[37];
    bench_dev_t dev = { 0 };
    UINTN len = 0;
    EFI_STATUS s;

    if (!d->part) {
        disk_reason(d, "no plain store");
        return 0;
    }
    s = bench_rwy_block(&dev, d->part, NULL);
    if (EFI_ERROR(s)) {
        bench_dev_close(&dev);
        disk_reason(d, "store mount failed");
        return 0;
    }
    s = bench_rwy_read_path(&dev, "PERF/READY", text, sizeof(text), &len);
    bench_dev_close(&dev);
    if (EFI_ERROR(s)) {
        disk_reason(d, "no marker");
        return 0;
    }
    if (!marker_get(text, "layout", layout, sizeof(layout)) || text_cmp(layout, "1")) {
        disk_reason(d, "bad marker");
        return 0;
    }
    if (!marker_get(text, "disk_guid", guid, sizeof(guid))) {
        uprintf("iobench: %s has no disk identity\n", d->info.devpath);
        text_copy(d->note, sizeof(d->note), "missing disk identity");
        return 1;
    }
    if (!read_disk_guid(d->parent, actual)) {
        uprintf("iobench: cannot read the disk identity of %s\n", d->info.devpath);
        text_copy(d->note, sizeof(d->note), "disk identity unavailable");
        return 1;
    }
    if (text_cmp(guid, actual)) {
        disk_reason(d, "disk mismatch");
        return 0;
    }
    return 1;
}

static EFI_HANDLE esp_handle(EFI_HANDLE ih);

static EFI_HANDLE g_boot_esp;

static int root_has_config(EFI_FILE_HANDLE root) {
    UINT8 *buf = NULL;
    UINTN size = 0;

    if (EFI_ERROR(fat_read_file(root, L"\\IOBENCH.CFG", &buf, &size))) return 0;
    gBS->FreePool(buf);
    return 1;
}

/* The ESP that holds IOBENCH.CFG; Runway repoints our DeviceHandle at its own FS. */
static EFI_STATUS open_boot_root(EFI_HANDLE ih, EFI_FILE_HANDLE *out_root) {
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    EFI_LOADED_IMAGE *li = NULL;
    EFI_FILE_HANDLE root = NULL;
    EFI_HANDLE *handles = NULL;
    UINTN n = 0;
    EFI_STATUS s;

    if (g_boot_esp) {
        if (!EFI_ERROR(gBS->HandleProtocol(g_boot_esp, &sfs_guid, (VOID **)&sfs)))
            return sfs->OpenVolume(sfs, out_root);
        g_boot_esp = NULL;
    }
    s = fat_open_our_root(ih, &root);
    if (!EFI_ERROR(s)) {
        if (root_has_config(root)) {
            if (!EFI_ERROR(gBS->HandleProtocol(ih, &li_guid, (VOID **)&li)))
                g_boot_esp = li->DeviceHandle;
            *out_root = root;
            return EFI_SUCCESS;
        }
        root->Close(root);
    }
    s = gBS->LocateHandleBuffer(ByProtocol, &sfs_guid, NULL, &n, &handles);
    if (EFI_ERROR(s)) return s;
    for (UINTN i = 0; i < n && !g_boot_esp; i++) {
        sfs = NULL;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &sfs_guid, (VOID **)&sfs))) continue;
        if (EFI_ERROR(sfs->OpenVolume(sfs, &root))) continue;
        if (root_has_config(root)) {
            g_boot_esp = handles[i];
            *out_root = root;
        } else {
            root->Close(root);
        }
    }
    gBS->FreePool(handles);
    if (g_boot_esp) return EFI_SUCCESS;
    return fat_open_our_root(ih, out_root);
}

EFI_STATUS bench_esp_open(EFI_HANDLE ih, EFI_FILE_HANDLE *out_root) {
    return open_boot_root(ih, out_root);
}

/* Firmware may connect only the boot device. Connect every controller so all disks show up. */
static void connect_all_controllers(void) {
    EFI_HANDLE *hs = NULL;
    UINTN count = 0;

    if (EFI_ERROR(gBS->LocateHandleBuffer(AllHandles, NULL, NULL, &count, &hs))) return;
    for (UINTN i = 0; i < count; i++) gBS->ConnectController(hs[i], NULL, NULL, TRUE);
    gBS->FreePool(hs);
}

static UINTN discover_disks(EFI_HANDLE ih, const bench_config_t *cfg, bench_disk_t *list, UINTN max) {
    EFI_HANDLE boot = esp_handle(ih);
    EFI_HANDLE boot_parent = parent_disk(boot);
    UINTN n;

    connect_all_controllers();
    if (!boot_parent) boot_parent = boot;
    collect_disks(boot_parent, list, max, &n);
    sort_disks(list, n);
    for (UINTN i = 0; i < n; i++) {
        bench_disk_t *d = &list[i];

        if (!d->part) disk_reason(d, "no plain store");
        else if (!disk_wanted(cfg, d)) disk_reason(d, "not selected");
        else if (!check_ready(d)) continue;
        else {
            d->use = 1;
            if (d->ambiguous && !d->note[0])
                text_copy(d->note, sizeof(d->note), "multiple benchmark sets; first used");
        }
    }
    return n;
}

static int check_block_area(EFI_BLOCK_IO_PROTOCOL *bio) {
    EFI_BLOCK_IO_MEDIA *m = bio->Media;
    UINT64 size = (m->LastBlock + 1) * m->BlockSize;

    uprintf("block area: block size %u, io align %u, %llu MiB, read-only %d, write cache %d\n",
            (unsigned)m->BlockSize, (unsigned)m->IoAlign, (unsigned long long)(size / 1048576u),
            (int)m->ReadOnly, (int)m->WriteCaching);
    if (!m->MediaPresent || m->ReadOnly || size < IOBENCH_RAW_BASE + AREA || AREA % m->BlockSize) {
        uprint("block area: not usable\n");
        return 0;
    }
    return 1;
}

static EFI_STATUS open_disk_root(EFI_HANDLE ih, const bench_disk_t *d, EFI_FILE_HANDLE *root);

static EFI_STATUS open_file_area(EFI_HANDLE ih, const bench_disk_t *d, EFI_FILE_HANDLE *out) {
    EFI_FILE_HANDLE root = NULL;
    EFI_STATUS s = open_disk_root(ih, d, &root);
    if (EFI_ERROR(s)) return s;
    s = root->Open(root, out, L"\\IOBENCH.DAT", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    root->Close(root);
    return s;
}

static UINT8 *alloc_area(void) {
    EFI_PHYSICAL_ADDRESS a = 0;
    if (EFI_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, AREA / 4096, &a))) return NULL;
    return (UINT8 *)(UINTN)a;
}

/* The file is written once, untimed, so the timed writes overwrite and do not extend it. */
static EFI_STATUS prepare_file(bench_dev_t *d, UINT8 *buf) {
    UINT64 off;
    EFI_STATUS s = EFI_SUCCESS;

    bench_fill_pattern(buf, 0);
    for (off = 0; off < AREA && !EFI_ERROR(s); off += 1048576u) s = file_write(d, off, 1048576u, buf + off);
    if (!EFI_ERROR(s)) s = file_flush(d);
    return s;
}

/* The volume the image was loaded from. */
static EFI_HANDLE esp_handle(EFI_HANDLE ih) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *li = NULL;

    if (g_boot_esp) return g_boot_esp;
    if (EFI_ERROR(gBS->HandleProtocol(ih, &li_guid, (void **)&li))) return NULL;
    return li->DeviceHandle;
}

/* The boot disk uses the ESP the image came from; another disk needs an ESP of its own. */
static EFI_HANDLE disk_esp_handle(EFI_HANDLE ih, const bench_disk_t *d) {
    return d->is_boot ? esp_handle(ih) : d->esp;
}

static int disk_has_esp(const bench_disk_t *d) {
    return d->is_boot || d->esp;
}

static EFI_STATUS open_disk_root(EFI_HANDLE ih, const bench_disk_t *d, EFI_FILE_HANDLE *root) {
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs = NULL;
    EFI_STATUS s;

    if (d->is_boot) return open_boot_root(ih, root);
    if (!d->esp) return EFI_NOT_FOUND;
    s = gBS->HandleProtocol(d->esp, &sfs_guid, (VOID **)&sfs);
    if (EFI_ERROR(s)) return s;
    return sfs->OpenVolume(sfs, root);
}

static EFI_STATUS load_config(EFI_HANDLE ih, bench_config_t *cfg) {
    EFI_FILE_HANDLE root = NULL;
    UINT8 *buf = NULL;
    UINTN size = 0;
    EFI_STATUS s;

    bench_config_defaults(cfg);
    s = open_boot_root(ih, &root);
    if (EFI_ERROR(s)) return s;
    s = fat_read_file(root, L"\\IOBENCH.CFG", &buf, &size);
    root->Close(root);
    if (s == EFI_NOT_FOUND) {
        uprint("iobench: no IOBENCH.CFG, using defaults\n");
        return EFI_SUCCESS;
    }
    if (EFI_ERROR(s)) return s;
    if (bench_config_parse((char *)buf, cfg)) {
        gBS->FreePool(buf);
        uprint("iobench: bad IOBENCH.CFG\n");
        return EFI_INVALID_PARAMETER;
    }
    gBS->FreePool(buf);
    return EFI_SUCCESS;
}

/* The benchmark key stays on the boot ESP, whatever disk is measured. */
static EFI_STATUS read_dek(EFI_HANDLE ih, const bench_config_t *cfg, UINT8 **dek) {
    EFI_FILE_HANDLE root = NULL;
    UINTN size = 0;
    EFI_STATUS s;

    *dek = NULL;
    if (!(cfg->devices & BENCH_DEV_RWY_ENC)) return EFI_SUCCESS;
    s = open_boot_root(ih, &root);
    if (EFI_ERROR(s)) return s;
    s = fat_read_file(root, L"\\BENCH.DEK", dek, &size);
    root->Close(root);
    if (!EFI_ERROR(s) && size != 32) s = EFI_BAD_BUFFER_SIZE;
    return s;
}

static EFI_STATUS open_rwy_file(EFI_HANDLE ih, const bench_disk_t *d, bench_dev_t *rw) {
    EFI_FILE_HANDLE root = NULL;
    EFI_STATUS s = open_disk_root(ih, d, &root);

    if (EFI_ERROR(s)) return s;
    s = bench_rwy_file(rw, disk_esp_handle(ih, d), root);
    root->Close(root);
    return s;
}

static EFI_STATUS open_rwy_stores(EFI_HANDLE ih, const bench_disk_t *d, const bench_config_t *cfg,
                                  bench_dev_t rw[3], const UINT8 *dek) {
    EFI_STATUS s = EFI_SUCCESS;

    const char *what = "the plain store";

    if (cfg->devices & BENCH_DEV_RWY_BLOCK) s = bench_rwy_block(&rw[0], d->part, NULL);
    if (!EFI_ERROR(s) && (cfg->devices & BENCH_DEV_RWY_FILE)) {
        what = "the store on the ESP";
        s = open_rwy_file(ih, d, &rw[1]);
    }
    if (!EFI_ERROR(s) && (cfg->devices & BENCH_DEV_RWY_ENC)) {
        what = "the encrypted store (is BENCH.DEK its key?)";
        s = bench_rwy_block(&rw[2], d->enc, dek);
    }
    if (!EFI_ERROR(s) && (cfg->devices & BENCH_DEV_RWY_BLOCK) &&
        bench_rwy_store_size(&rw[0]) > IOBENCH_RAW_BASE) {
        what = "the plain store (it overlaps the raw area)";
        s = EFI_VOLUME_CORRUPTED;
    }
    if (EFI_ERROR(s)) uprintf("iobench: cannot open %s (status 0x%llx)\n", what, (unsigned long long)s);
    return s;
}

static EFI_STATUS prepare_rwy_stores(const bench_config_t *cfg, bench_dev_t rw[3], UINT8 *buf) {
    static const unsigned bits[3] = { BENCH_DEV_RWY_BLOCK, BENCH_DEV_RWY_FILE, BENCH_DEV_RWY_ENC };
    EFI_STATUS s = EFI_SUCCESS;

    for (UINTN i = 0; i < 3 && !EFI_ERROR(s); i++)
        if (cfg->devices & bits[i]) s = bench_rwy_prepare(&rw[i], buf);
    return s;
}

/* Mount the selected Runway stores and prepare them; `buf` is scratch. */
static EFI_STATUS open_runway(EFI_HANDLE ih, const bench_disk_t *d, bench_dev_t rw[3],
                              UINT8 *buf, const bench_config_t *cfg) {
    UINT8 *dek = NULL;
    EFI_STATUS s;

    if (!(cfg->devices & (BENCH_DEV_RWY_BLOCK | BENCH_DEV_RWY_FILE | BENCH_DEV_RWY_ENC)))
        return EFI_SUCCESS;
    s = read_dek(ih, cfg, &dek);
    if (!EFI_ERROR(s)) s = open_rwy_stores(ih, d, cfg, rw, dek);
    if (!EFI_ERROR(s)) s = prepare_rwy_stores(cfg, rw, buf);
    if (dek) gBS->FreePool(dek);
    return s;
}

static EFI_STATUS setup_baseline(EFI_HANDLE ih, const bench_config_t *cfg, const bench_disk_t *d,
                                 bench_dev_t *blk, bench_dev_t *fil, UINT8 *want) {
    EFI_STATUS s = EFI_SUCCESS;

    if (cfg->devices & (BENCH_DEV_BLOCK | BENCH_DEV_RWY_BLOCK | BENCH_DEV_PROBE)) {
        blk->bio = partition_bio(d->part);
        if (!blk->bio || !check_block_area(blk->bio)) return EFI_NOT_FOUND;
    }
    if (cfg->devices & BENCH_DEV_FILE) {
        s = open_file_area(ih, d, &fil->fh);
        if (!EFI_ERROR(s)) s = prepare_file(fil, want);
    }
    if (EFI_ERROR(s) && fil->fh) {
        fil->fh->Close(fil->fh);
        fil->fh = NULL;
    }
    return s;
}

/* B1 on every store; B2 only on the stores of a partition, as the file store sits on the ESP. */
static int run_work(const bench_config_t *cfg, bench_dev_t rw[3], UINT8 *want, UINT8 *got) {
    int ok = 1;

    if (cfg->devices & BENCH_DEV_RWY_BLOCK) ok &= bench_work_run(&rw[0], 1, want, got);
    if (cfg->devices & BENCH_DEV_RWY_FILE) ok &= bench_work_run(&rw[1], 0, want, got);
    if (cfg->devices & BENCH_DEV_RWY_ENC) ok &= bench_work_run(&rw[2], 1, want, got);
    return ok;
}

static int run_bench(const bench_config_t *cfg, bench_dev_t *blk, bench_dev_t *fil,
                     bench_dev_t rw[3], UINT8 *want, UINT8 *got) {
    int ok = 1;

    if (cfg->devices & BENCH_DEV_RWY_ENC) ok &= bench_cipher_run(want, got);
    if (cfg->devices & BENCH_DEV_BLOCK) ok &= bench_wpath_run(blk, want);
    if (cfg->devices & BENCH_DEV_BLOCK) ok &= bench_dev(blk, want, got);
    if (cfg->devices & BENCH_DEV_RWY_BLOCK) ok &= bench_dev(&rw[0], want, got);
    if (cfg->devices & BENCH_DEV_FILE) ok &= bench_dev(fil, want, got);
    if (cfg->devices & BENCH_DEV_RWY_FILE) ok &= bench_dev(&rw[1], want, got);
    if (cfg->devices & BENCH_DEV_RWY_ENC) ok &= bench_dev(&rw[2], want, got);
    ok &= run_work(cfg, rw, want, got);
    if (cfg->devices & BENCH_DEV_PROBE) ok &= bench_probe_run(blk, want, got);
    return ok;
}

/* One pass over the devices of a disk: open them, time every cell once and close them. */
static EFI_STATUS run_pass(EFI_HANDLE ih, const bench_config_t *cfg, bench_disk_t *d,
                           UINT8 *want, UINT8 *got, int *ok) {
    bench_dev_t blk = { "block", block_write, block_read, block_flush, NULL, NULL, 0, IOBENCH_RAW_BASE, NULL, 0 };
    bench_dev_t fil = { "file", file_write, file_read, file_flush, NULL, NULL, 0, 0, NULL, 0 };
    bench_dev_t rw[3] = { { 0 } };
    int use_rwy = (cfg->devices & (BENCH_DEV_RWY_BLOCK | BENCH_DEV_RWY_FILE | BENCH_DEV_RWY_ENC)) != 0;
    EFI_STATUS s;

    s = setup_baseline(ih, cfg, d, &blk, &fil, want);
    if (!EFI_ERROR(s) && use_rwy) s = open_runway(ih, d, rw, got, cfg);
    if (!EFI_ERROR(s)) {
        if (cfg->devices & BENCH_DEV_RWY_BLOCK) bench_rwy_uuid(&rw[0], d->info.store_uuid);
        *ok &= run_bench(cfg, &blk, &fil, rw, want, got);
    }
    bench_dev_close(&fil);
    for (UINTN i = 0; i < 3; i++) bench_dev_close(&rw[i]);
    return s;
}

/* A disk without an ESP has no file to time: leave the file cells out instead of timing another disk. */
static void skip_file_cells(bench_config_t *cfg, bench_disk_t *d) {
    const unsigned file_cells = BENCH_DEV_FILE | BENCH_DEV_RWY_FILE;

    if (disk_has_esp(d) || !(cfg->devices & file_cells)) return;
    cfg->devices &= ~file_cells;
    uprintf("iobench: %s has no ESP, the file cells are skipped\n", d->info.devpath);
    if (!d->note[0]) text_copy(d->note, sizeof(d->note), "no ESP: file cells skipped");
}

/* The warm-up pass, then the timed rounds. Each round runs every cell once. */
static EFI_STATUS run_disk(EFI_HANDLE ih, const bench_config_t *cfg, bench_disk_t *d,
                           UINT8 *want, UINT8 *got) {
    bench_config_t dc = *cfg;
    int ok = 1;
    EFI_STATUS s = EFI_SUCCESS;

    bench_result_disk_begin(&d->info);
    skip_file_cells(&dc, d);
    for (UINTN pass = cfg->warmup ? 0 : 1; pass <= cfg->rounds && !EFI_ERROR(s); pass++) {
        bench_result_round(pass ? pass - 1 : 0, pass == 0);
        s = run_pass(ih, &dc, d, want, got, &ok);
    }
    if (EFI_ERROR(s)) {
        uprintf("iobench: setup failed on %s (status 0x%llx)\n", d->info.devpath,
                (unsigned long long)s);
        bench_result_disk_done(&d->info, 0, "setup failed");
        return s;
    }
    bench_result_disk_done(&d->info, ok, ok ? d->note : "a check failed");
    return ok ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

#define CONSOLE_LOG_MAX (256u * 1024u)

static void wait_for_key(void) {
    EFI_INPUT_KEY key;
    UINTN idx;

    uprint("\nPress any key to power off.\n");
    if (!gST->ConIn) return;
    gST->ConIn->Reset(gST->ConIn, FALSE);
    if (!EFI_ERROR(gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &idx)))
        gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
}

/* Keep the console text, wait for a key when the configuration asks for it, and power off. */
static EFI_STATUS finish(EFI_HANDLE ih, const bench_config_t *cfg, int ok) {
    const char *text = NULL;
    UINTN len;

    uprint(ok ? "IOBENCH DONE\n" : "IOBENCH FAIL\n");
    len = uprint_captured(&text);
    if (len) bench_result_log(ih, text, len);
    bench_result_close();
    if (cfg->pause == BENCH_PAUSE_ALWAYS || (cfg->pause == BENCH_PAUSE_FAIL && !ok)) wait_for_key();
    flush_and_stall(500000);
    gST->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    bench_disk_t disks[MAX_DISKS];
    bench_config_t cfg;
    UINT8 *want, *got;
    char config_text[BENCH_CONFIG_MAX];
    UINTN n;
    int ok = 1, ran = 0;
    char *log = NULL;
    EFI_STATUS s;

    util_init(ImageHandle, SystemTable);
    if (!EFI_ERROR(gBS->AllocatePool(EfiLoaderData, CONSOLE_LOG_MAX, (void **)&log))) uprint_capture(log, CONSOLE_LOG_MAX);
    tsc_calibrate();
    uprint("iobench: start\n");
    s = load_config(ImageHandle, &cfg);
    if (EFI_ERROR(s)) {
        /* Nobody read the pause setting: keep the screen for the operator. */
        bench_config_defaults(&cfg);
        cfg.pause = BENCH_PAUSE_FAIL;
        uprintf("iobench: cannot use IOBENCH.CFG (status 0x%llx)\n", (unsigned long long)s);
        return finish(ImageHandle, &cfg, 0);
    }
    bench_config_text(config_text, sizeof(config_text), &cfg);
    uprintf("iobench: config %s\n", config_text);
    uprintf("iobench: AES %s\n", rwy_crypto_hw_aes() ? "hardware" : "portable");
    bench_result_open(ImageHandle, &cfg);

    want = alloc_area();
    got = alloc_area();
    if (!want || !got) {
        uprint("iobench: no memory for the payload\n");
        return finish(ImageHandle, &cfg, 0);
    }
    n = discover_disks(ImageHandle, &cfg, disks, MAX_DISKS);
    uprintf("iobench: %u prepared disk(s)\n", (unsigned)n);
    for (UINTN i = 0; i < n; i++) {
        if (!disks[i].use) {
            uprintf("iobench: skip %s: %s\n", disks[i].info.devpath, disks[i].reason);
            bench_result_skip(&disks[i].info, disks[i].reason);
            continue;
        }
        ran = 1;
        if (EFI_ERROR(run_disk(ImageHandle, &cfg, &disks[i], want, got))) ok = 0;
    }
    if (!ran) ok = 0;

    return finish(ImageHandle, &cfg, ok);
}
