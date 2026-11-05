/* iobench_result — append the benchmark report to the results store, the ESP or serial. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "efi_util.h"
#include "iobench.h"
#include "iobench_stats.h"
#include "rwy_fs.h"
#include "rwy_crypto.h"
#include "rwy_format.h"

#define RESULT_GUID \
    { 0x5d0a7d6e, 0x3b1c, 0x4c1e, { 0x9a, 0x42, 0x6f, 0x1b, 0x8e, 0x70, 0x21, 0xc7 } }
#define MAX_SAMPLES 1024
#define RUN_LINE 2048
#define TIMING_NOTE \
    "reads may be warm in the firmware or device cache; FAT file reads may be cached by the FAT driver"
#define SAMPLE_LINE 1024

typedef enum { RESULT_NONE, RESULT_STORE, RESULT_ESP, RESULT_SERIAL } result_mode_t;

static result_mode_t g_mode;
static EFI_HANDLE g_image;
static rwy_fs_instance_t *g_inst;
static EFI_FILE_HANDLE g_root, g_dir;
static iobench_sample_t g_samples[MAX_SAMPLES];
static UINTN g_sample_count;
static UINTN g_round;
static int g_warmup;
static int g_samples_lost;
static char g_run[32];
static char g_disk[160];
static char g_started[32];
static char g_config_text[BENCH_CONFIG_MAX];
static const bench_config_t *g_config;

static UINTN slen(const char *s) {
    UINTN n = 0;

    while (s && s[n]) n++;
    return n;
}

static void copy_str(char *dst, UINTN cap, const char *src) {
    UINTN n = 0;

    while (src && src[n] && n + 1 < cap) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
}

static void append_str(char *out, UINTN cap, UINTN *n, const char *s) {
    while (s && *s && *n + 1 < cap) out[(*n)++] = *s++;
    out[*n] = 0;
}

static void append_u64(char *out, UINTN cap, UINTN *n, UINT64 v) {
    char b[24];
    UINTN i = sizeof(b) - 1;

    b[i] = 0;
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    append_str(out, cap, n, &b[i]);
}

static void append_two(char *out, UINTN cap, UINTN *n, UINTN v) {
    if (v < 10) append_str(out, cap, n, "0");
    append_u64(out, cap, n, v % 100);
}

void bench_time_str(char *out, UINTN cap) {
    EFI_TIME t;
    UINTN n = 0;

    out[0] = 0;
    if (EFI_ERROR(gRT->GetTime(&t, NULL))) {
        copy_str(out, cap, "unknown");
        return;
    }
    append_u64(out, cap, &n, t.Year);
    append_str(out, cap, &n, "-");
    append_two(out, cap, &n, t.Month);
    append_str(out, cap, &n, "-");
    append_two(out, cap, &n, t.Day);
    append_str(out, cap, &n, "T");
    append_two(out, cap, &n, t.Hour);
    append_str(out, cap, &n, ":");
    append_two(out, cap, &n, t.Minute);
    append_str(out, cap, &n, ":");
    append_two(out, cap, &n, t.Second);
    append_str(out, cap, &n, "Z");
}

/* ---------- the results store ---------- */

static int is_result_partition(EFI_HANDLE h, const EFI_GUID *want) {
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

static EFI_HANDLE find_result_partition(const EFI_GUID *guid) {
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE *h = NULL, found = NULL;
    UINTN n = 0, i;
    EFI_BLOCK_IO_PROTOCOL *bio = NULL;

    if (EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol, &bio_guid, NULL, &n, &h))) return NULL;
    for (i = 0; i < n && !found; i++) {
        if (EFI_ERROR(gBS->HandleProtocol(h[i], &bio_guid, (void **)&bio))) continue;
        if (bio->Media->LogicalPartition && is_result_partition(h[i], guid)) found = h[i];
    }
    gBS->FreePool(h);
    return found;
}

static EFI_STATUS store_size(const rwy_block_dev_t *dev, UINT64 *size) {
    UINT8 blk[RWY_BLOCK_SIZE];
    rwy_superblock_t sb;

    for (UINTN slot = 0; slot < 2; slot++) {
        if (dev->read(dev->ctx, slot * RWY_BLOCK_SIZE, blk, sizeof(blk))) continue;
        if (rwy_validate_sb_block(blk, &sb) != RWY_OK) continue;
        *size = sb.total_size;
        return EFI_SUCCESS;
    }
    return EFI_NOT_FOUND;
}

static EFI_STATUS store_mount(void) {
    EFI_GUID guid = RESULT_GUID;
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE h = find_result_partition(&guid);
    EFI_BLOCK_IO_PROTOCOL *bio = NULL;
    rwy_backend_t be;
    UINT64 size = 0;
    EFI_STATUS s;

    if (!h) return EFI_NOT_FOUND;
    s = gBS->HandleProtocol(h, &bio_guid, (void **)&bio);
    if (EFI_ERROR(s)) return s;
    if (!bio->Media->MediaPresent) return EFI_MEDIA_CHANGED;
    s = rwy_fs_block_backend(bio, &be);
    if (!EFI_ERROR(s)) s = store_size(&be.dev, &size);
    if (!EFI_ERROR(s)) s = rwy_fs_block_limit(&be, size);
    if (EFI_ERROR(s)) return s;
    s = rwy_fs_mount_group(&be, &h, 1, NULL, &g_inst);
    if (!EFI_ERROR(s) && rwy_in_txn(&g_inst->store)) rwy_abort(&g_inst->store);
    return s;
}

static int store_ensure_dir(void) {
    rwy_store_t *s = &g_inst->store;
    rwy_attr_t a;
    int rc = rwy_stat(s, "PERF", &a);

    if (rc == RWY_ERR_NOENT) {
        rc = rwy_begin(s);
        if (!rc) rc = rwy_mkdir(s, "PERF", RWY_S_IFDIR | 0755, 0, 0);
        if (!rc) rc = rwy_commit(s);
        else if (rwy_in_txn(s)) rwy_abort(s);
        return rc;
    }
    if (!rc) return RWY_S_ISDIR(a.mode) ? RWY_OK : RWY_ERR_INVAL;
    return rc;
}

static int store_append(const char *path, const char *head, const char *data, UINTN len) {
    rwy_store_t *s = &g_inst->store;
    rwy_attr_t a;
    UINT64 off = 0;
    int rc = rwy_stat(s, path, &a);

    if (rc == RWY_ERR_NOENT) {
        rc = rwy_create(s, path, RWY_S_IFREG | 0644, 0, 0);
        if (!rc && head && *head) rc = rwy_write_path(s, path, 0, head, slen(head));
        off = head ? slen(head) : 0;
    } else if (!rc) {
        off = a.size;
    }
    if (!rc && len) rc = rwy_write_path(s, path, off, data, len);
    return rc;
}

static int store_prepare(void) {
    rwy_store_t *s = &g_inst->store;
    int rc = rwy_begin(s);

    if (!rc) rc = store_append("PERF/RUNS.CSV", iobench_runs_header(), "", 0);
    if (!rc) rc = store_append("PERF/SAMPLES.CSV", iobench_samples_header(), "", 0);
    if (!rc) rc = rwy_commit(s);
    else if (rwy_in_txn(s)) rwy_abort(s);
    return rc;
}

static int store_replace(const char *path, const char *data, UINTN len) {
    rwy_store_t *s = &g_inst->store;
    rwy_attr_t a;
    int rc = rwy_stat(s, path, &a);

    if (rc == RWY_ERR_NOENT) rc = rwy_create(s, path, RWY_S_IFREG | 0644, 0, 0);
    else if (!rc) rc = rwy_truncate(s, path, 0);
    if (!rc && len) rc = rwy_write_path(s, path, 0, data, len);
    return rc;
}

static void store_drop_marker(void) {
    rwy_store_t *s = &g_inst->store;
    int rc = rwy_begin(s);

    if (rc) return;
    rc = rwy_unlink(s, "PERF/RUNNING");
    if (!rc || rc == RWY_ERR_NOENT) rc = rwy_commit(s);
    if (rc && rwy_in_txn(s)) rwy_abort(s);
}

/* ---------- the FAT fallback ---------- */

static EFI_STATUS esp_open(void) {
    EFI_STATUS s;

    if (g_dir) return EFI_SUCCESS;
    if (!g_root) {
        s = bench_esp_open(g_image, &g_root);
        if (EFI_ERROR(s)) return s;
    }
    s = g_root->Open(g_root, &g_dir, L"\\PERF",
                     EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                     EFI_FILE_DIRECTORY);
    if (EFI_ERROR(s)) {
        g_root->Close(g_root);
        g_root = NULL;
    }
    return s;
}

static EFI_STATUS esp_size(EFI_FILE_HANDLE f, UINT64 *size) {
    EFI_GUID info_guid = EFI_FILE_INFO_ID;
    UINT8 info[512];
    UINTN info_size = sizeof(info);
    EFI_STATUS s = f->GetInfo(f, &info_guid, &info_size, info);

    *size = EFI_ERROR(s) ? 0 : ((EFI_FILE_INFO *)info)->FileSize;
    return s;
}

static EFI_STATUS esp_append(CHAR16 *name, const char *head, const char *data, UINTN len) {
    EFI_FILE_HANDLE f = NULL;
    UINT64 size = 0;
    EFI_STATUS s;

    s = g_dir->Open(g_dir, &f, name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(s)) return s;
    s = esp_size(f, &size);
    if (!EFI_ERROR(s) && size == 0 && head && *head) {
        UINTN n = slen(head);

        s = f->Write(f, &n, (void *)head);
        size = n;
    }
    if (!EFI_ERROR(s) && len) {
        UINTN n = len;

        s = f->SetPosition(f, size);
        if (!EFI_ERROR(s)) s = f->Write(f, &n, (void *)data);
    }
    if (!EFI_ERROR(s)) s = f->Flush(f);
    f->Close(f);
    return s;
}

static EFI_STATUS esp_prepare(void) {
    EFI_STATUS s = esp_append(L"RUNS.CSV", iobench_runs_header(), "", 0);

    if (!EFI_ERROR(s)) s = esp_append(L"SAMPLES.CSV", iobench_samples_header(), "", 0);
    return s;
}

static EFI_STATUS esp_marker(const char *data, UINTN len) {
    EFI_FILE_HANDLE f = NULL;
    EFI_STATUS s;

    s = g_dir->Open(g_dir, &f, L"RUNNING",
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (!EFI_ERROR(s)) {
        s = f->Delete(f);
        f = NULL;
    }
    if (EFI_ERROR(s)) return s;
    s = g_dir->Open(g_dir, &f, L"RUNNING",
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(s)) return s;
    {
        UINTN n = len;

        s = f->Write(f, &n, (void *)data);
    }
    if (!EFI_ERROR(s)) s = f->Flush(f);
    f->Close(f);
    return s;
}

/* Write a whole file in the report directory of the ESP, replacing the old one. */
static EFI_STATUS esp_replace(CHAR16 *name, const char *data, UINTN len) {
    EFI_FILE_HANDLE f = NULL;
    EFI_STATUS s = g_dir->Open(g_dir, &f, name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);

    if (!EFI_ERROR(s)) {
        s = f->Delete(f);
        f = NULL;
    }
    if (EFI_ERROR(s)) return s;
    s = g_dir->Open(g_dir, &f, name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(s)) return s;
    if (len) {
        UINTN n = len;

        s = f->Write(f, &n, (void *)data);
    }
    if (!EFI_ERROR(s)) s = f->Flush(f);
    f->Close(f);
    return s;
}

static void esp_drop_marker(void) {
    EFI_FILE_HANDLE f = NULL;
    EFI_STATUS s = g_dir->Open(g_dir, &f, L"RUNNING", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);

    if (!EFI_ERROR(s)) f->Delete(f);
}

/* ---------- mode changes ---------- */

static const char *mode_name(result_mode_t m) {
    if (m == RESULT_STORE) return "store";
    if (m == RESULT_ESP) return "esp";
    if (m == RESULT_SERIAL) return "serial";
    return "none";
}

static void use_esp_or_serial(void) {
    g_mode = EFI_ERROR(esp_open()) ? RESULT_SERIAL : RESULT_ESP;
}

static void store_failed(const char *what, int rc) {
    if (g_mode != RESULT_STORE) return;
    uprintf("iobench: results store %s failed (%s)\n", what, rwy_strerror(rc));
    store_drop_marker();
    use_esp_or_serial();
    uprintf("iobench: results=%s\n", mode_name(g_mode));
}

/* ---------- report rows ---------- */

static int format_sample(UINTN i, char *line, UINTN cap) {
    size_t need = iobench_format_sample(line, cap, &g_samples[i]);

    return need >= cap ? -1 : 0;
}

static int store_report(const char *run_line) {
    rwy_store_t *s = &g_inst->store;
    char line[SAMPLE_LINE];
    int rc = rwy_begin(s);

    if (!rc) rc = store_append("PERF/RUNS.CSV", iobench_runs_header(), run_line, slen(run_line));
    for (UINTN i = 0; !rc && i < g_sample_count; i++) {
        if (format_sample(i, line, sizeof(line))) rc = RWY_ERR_INVAL;
        else rc = store_append("PERF/SAMPLES.CSV", iobench_samples_header(), line, slen(line));
    }
    if (!rc) {
        rc = rwy_unlink(s, "PERF/RUNNING");
        if (rc == RWY_ERR_NOENT) rc = RWY_OK;
    }
    if (!rc) rc = rwy_commit(s);
    else if (rwy_in_txn(s)) rwy_abort(s);
    return rc;
}

static EFI_STATUS esp_report(const char *run_line) {
    char line[SAMPLE_LINE];
    EFI_STATUS s = esp_append(L"RUNS.CSV", iobench_runs_header(), run_line, slen(run_line));

    for (UINTN i = 0; !EFI_ERROR(s) && i < g_sample_count; i++) {
        if (format_sample(i, line, sizeof(line))) s = EFI_DEVICE_ERROR;
        else s = esp_append(L"SAMPLES.CSV", iobench_samples_header(), line, slen(line));
    }
    esp_drop_marker();
    return s;
}

static void serial_report(const char *run_line) {
    char line[SAMPLE_LINE];

    uprintf("IOBENCH CSV %s", run_line);
    for (UINTN i = 0; i < g_sample_count; i++)
        if (!format_sample(i, line, sizeof(line))) uprintf("IOBENCH CSV %s", line);
}

static void firmware(char *out, UINTN cap) {
    UINTN n = 0;

    for (UINTN i = 0; gST->FirmwareVendor && gST->FirmwareVendor[i] && n + 1 < cap; i++)
        out[n++] = gST->FirmwareVendor[i] < 128 ? (char)gST->FirmwareVendor[i] : '?';
    out[n] = 0;
}

static void write_run(const char *run_line) {
    if (g_mode == RESULT_STORE) {
        int rc = store_report(run_line);

        if (!rc) return;
        store_failed("report", rc);
    }
    if (g_mode == RESULT_ESP) {
        EFI_STATUS s = esp_report(run_line);

        if (!EFI_ERROR(s)) return;
        uprintf("iobench: ESP report failed (0x%llx)\n", (unsigned long long)s);
        esp_drop_marker();
        g_mode = RESULT_SERIAL;
    }
    serial_report(run_line);
}

/* ---------- interface ---------- */

void bench_result_open(EFI_HANDLE ih, const bench_config_t *cfg) {
    EFI_STATUS s;
    int try_store = cfg->result == BENCH_RESULT_AUTO || cfg->result == BENCH_RESULT_STORE;
    int try_esp = cfg->result == BENCH_RESULT_AUTO || cfg->result == BENCH_RESULT_ESP;

    g_image = ih;
    g_config = cfg;
    g_mode = RESULT_NONE;
    g_sample_count = 0;
    g_samples_lost = 0;
    bench_config_text(g_config_text, sizeof(g_config_text), cfg);
    if (try_store) {
        s = store_mount();
        if (!EFI_ERROR(s)) {
            int rc = store_ensure_dir();

            if (!rc) rc = store_prepare();
            if (!rc) g_mode = RESULT_STORE;
            else uprintf("iobench: results store is not usable (%s)\n", rwy_strerror(rc));
        }
    }
    if (g_mode == RESULT_NONE && try_esp) {
        use_esp_or_serial();
        if (g_mode == RESULT_ESP && EFI_ERROR(esp_prepare())) g_mode = RESULT_SERIAL;
    }
    if (g_mode == RESULT_NONE) g_mode = RESULT_SERIAL;
    uprintf("iobench: results=%s\n", mode_name(g_mode));
}

/* Copy a report file of the results store to the ESP, so a machine without a Runway tool can read it. */
static void esp_copy_from_store(const char *path, CHAR16 *name) {
    rwy_store_t *s = &g_inst->store;
    rwy_attr_t a;
    void *buf = NULL;
    size_t n;
    EFI_STATUS st;

    if (rwy_stat(s, path, &a) || !a.size) return;
    n = (size_t)a.size;
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, n, &buf))) return;
    if (!rwy_read_path(s, path, 0, buf, &n) && n == (size_t)a.size) {
        st = esp_replace(name, buf, n);
        if (EFI_ERROR(st)) uprintf("iobench: %s not copied to the ESP (0x%llx)\n", path, (unsigned long long)st);
    }
    gBS->FreePool(buf);
}

/* The console text of this run goes to the results store and to the ESP, so it can be read after the screen is gone.
 * The report files of the store are copied to the ESP as well. */
void bench_result_log(EFI_HANDLE ih, const char *text, UINTN len) {
    int rc = RWY_OK;
    EFI_STATUS s;

    g_image = ih;
    if (g_mode == RESULT_STORE && g_inst) {
        rc = rwy_begin(&g_inst->store);
        if (!rc) rc = store_replace("PERF/CONSOLE.LOG", text, len);
        if (!rc) rc = rwy_commit(&g_inst->store);
        else if (rwy_in_txn(&g_inst->store)) rwy_abort(&g_inst->store);
        if (rc) uprintf("iobench: console log not saved in the results store (%s)\n", rwy_strerror(rc));
    }
    s = esp_open();
    if (!EFI_ERROR(s) && g_mode == RESULT_STORE && g_inst) {
        esp_copy_from_store("PERF/RUNS.CSV", L"RUNS.CSV");
        esp_copy_from_store("PERF/SAMPLES.CSV", L"SAMPLES.CSV");
    }
    if (!EFI_ERROR(s)) s = esp_replace(L"CONSOLE.LOG", text, len);
    if (EFI_ERROR(s)) uprintf("iobench: console log not saved on the ESP (0x%llx)\n", (unsigned long long)s);
}

void bench_result_close(void) {
    if (g_dir) {
        g_dir->Close(g_dir);
        g_dir = NULL;
    }
    if (g_root) {
        g_root->Close(g_root);
        g_root = NULL;
    }
    g_mode = RESULT_NONE;
}

void bench_result_disk_begin(const bench_disk_info_t *d) {
    char msg[256];
    UINTN n = 0;

    g_sample_count = 0;
    g_round = 0;
    g_warmup = 0;
    g_samples_lost = 0;
    copy_str(g_disk, sizeof(g_disk), d->devpath);
    bench_time_str(g_started, sizeof(g_started));
    copy_str(g_run, sizeof(g_run), g_started);
    append_str(msg, sizeof(msg), &n, "run=");
    append_str(msg, sizeof(msg), &n, g_run);
    append_str(msg, sizeof(msg), &n, "\ndisk=");
    append_str(msg, sizeof(msg), &n, g_disk);
    append_str(msg, sizeof(msg), &n, "\nstarted=");
    append_str(msg, sizeof(msg), &n, g_started);
    append_str(msg, sizeof(msg), &n, "\n");

    if (g_mode == RESULT_STORE) {
        int rc = rwy_begin(&g_inst->store);

        if (!rc) rc = store_replace("PERF/RUNNING", msg, slen(msg));
        if (!rc) rc = rwy_commit(&g_inst->store);
        else if (rwy_in_txn(&g_inst->store)) rwy_abort(&g_inst->store);
        if (rc) store_failed("marker", rc);
    }
    if (g_mode == RESULT_ESP) {
        EFI_STATUS s = esp_marker(msg, slen(msg));

        if (EFI_ERROR(s)) {
            uprintf("iobench: ESP marker failed (0x%llx)\n", (unsigned long long)s);
            esp_drop_marker();
            g_mode = RESULT_SERIAL;
        }
    }
    if (g_mode == RESULT_SERIAL) uprintf("IOBENCH RUNNING %s\n", g_disk);
}

void bench_result_round(UINTN round, int warmup) {
    g_round = round;
    g_warmup = warmup;
}

void bench_result_add_sample(const bench_dev_t *d, const bench_measure_t *m) {
    iobench_sample_t *s;
    UINT64 calls;

    if (g_warmup) return;
    if (g_sample_count >= MAX_SAMPLES) {
        if (!g_samples_lost) uprint("iobench: sample buffer is full\n");
        g_samples_lost = 1;
        return;
    }
    s = &g_samples[g_sample_count++];
    s->run = g_run;
    s->disk = g_disk;
    s->dev = d->name;
    s->op = m->op;
    s->size = m->size;
    s->sample = g_round;
    s->stat = "sample";
    s->seconds_us = m->seconds_us;
    s->bytes = m->bytes;
    s->calls_core = m->calls_core;
    s->calls_dev = m->calls_dev;
    calls = m->calls_core ? m->calls_core : m->calls_dev;
    s->us_per_call_x100 = calls ? (m->seconds_us * 100 + calls / 2) / calls : 0;
    s->flush_us = m->flush_us;
    s->extents = m->extents;
    s->encrypted = d->encrypted;
    s->result = m->ok ? "ok" : "failed";
}

static void write_row(const bench_disk_info_t *d, const char *status, const char *reason) {
    iobench_run_t r = { 0 };
    char run_line[RUN_LINE], finished[32], fw[64];
    size_t need;

    bench_time_str(finished, sizeof(finished));
    firmware(fw, sizeof(fw));
    r.run = g_run;
    r.disk = d->devpath;
    r.status = status;
#if defined(__x86_64__)
    r.arch = "x86_64";
#elif defined(__aarch64__)
    r.arch = "aarch64";
#else
    r.arch = "unknown";
#endif
    r.build = "local";
    r.bench_version = "1";
    r.firmware = fw;
    r.system_model = "-";
    r.devpath = d->devpath;
    r.media_id = d->media_id;
    r.disk_bytes = d->bytes;
    r.block_size = d->block_size;
    r.ioalign = d->ioalign;
    r.write_cache = d->write_cache;
    r.store_uuid = d->store_uuid;
    r.store_kind = "plain";
    r.disk_set = g_config ? bench_config_disk_set_name(g_config) : "boot";
    r.config = g_config_text[0] ? g_config_text : "default";
    r.started = g_started;
    r.finished = finished;
    r.reason = reason ? reason : "";
    r.tsc_hz = tsc_hz();
    r.timing_note = TIMING_NOTE;
    r.aes = rwy_crypto_hw_aes() ? "hardware" : "portable";
    need = iobench_format_run(run_line, sizeof(run_line), &r);
    if (need >= sizeof(run_line)) {
        uprint("iobench: report row is too long\n");
        serial_report("");
        return;
    }
    write_run(run_line);
}

void bench_result_disk_done(const bench_disk_info_t *d, int ok, const char *reason) {
    g_sample_count += iobench_stats_summarize(g_samples, g_sample_count, &g_samples[g_sample_count],
                                              MAX_SAMPLES - g_sample_count);
    write_row(d, ok ? "ok" : "failed", reason);
}

void bench_result_skip(const bench_disk_info_t *d, const char *reason) {
    g_sample_count = 0;
    g_round = 0;
    g_warmup = 0;
    g_samples_lost = 0;
    copy_str(g_disk, sizeof(g_disk), d->devpath);
    bench_time_str(g_started, sizeof(g_started));
    copy_str(g_run, sizeof(g_run), g_started);
    write_row(d, "skipped", reason);
}

