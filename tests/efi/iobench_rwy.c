/*
 * iobench_rwy — Runway on a partition and on RWYBENCH.DAT, timed through the same devices as the baselines.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The store is mounted by the code of the loader. A wrapper around the firmware protocol counts the calls
 * that reach the firmware, by size. The call counters of the store instance count what reaches its mirror.
 */
#include <efi.h>
#include <efilib.h>
#include <string.h>
#include "efi_util.h"
#include "iobench.h"
#include "rwy_fs.h"

#define OBJECT_RUN 1048576u
#define OBJECT_SEED 0x22 /* the pattern mkstore gives the object */

typedef struct {
    EFI_BLOCK_IO_PROTOCOL proto; /* MUST be first */
    EFI_BLOCK_IO_PROTOCOL *real;
    bench_counts_t *counts;
} fw_bio_t;

typedef struct {
    EFI_FILE_PROTOCOL proto; /* MUST be first */
    EFI_FILE_HANDLE real;
    bench_counts_t *counts;
} fw_file_t;

struct bench_rwy {
    bench_counts_t fw;
    fw_bio_t bio;
    fw_file_t file;
    rwy_fs_instance_t *inst;
    UINT64 oid;
    rwy_io_stats_t io0;     /* core counters when the counting started */
    UINT64 mount_t0, mount_ticks, mount_core, mount_dev;
};

/* Calls of the core in one direction: the product counter of the store, summed over kinds and size classes. */
static UINT64 core_calls(const rwy_io_stats_t *io, int write) {
    UINT64 n = 0;

    for (UINTN k = 0; k < RWY_IO_KINDS; k++)
        for (UINTN b = 0; b < RWY_IO_BUCKETS; b++) n += io->calls[write][k][b];
    return n;
}

/* ---------- counting the calls to the firmware ---------- */

static void count_size(UINT64 *hist, UINTN len) {
    UINTN i = 0;

    while (i < BENCH_BUCKETS - 1 && len > (4096u << (2 * i))) i++;
    hist[i]++;
}

static void note_slow(bench_slow_t *slow, UINT64 ticks, UINT64 index, EFI_LBA lba, UINTN len) {
    if (ticks <= slow->ticks) return;
    slow->ticks = ticks;
    slow->index = index;
    slow->lba = lba;
    slow->len = len;
}

static EFI_STATUS EFIAPI fw_read_blocks(EFI_BLOCK_IO_PROTOCOL *This, UINT32 id, EFI_LBA lba, UINTN len, VOID *buf) {
    fw_bio_t *w = (fw_bio_t *)This;

    UINT64 t0 = tsc_now();
    EFI_STATUS s = w->real->ReadBlocks(w->real, id, lba, len, buf);

    UINT64 took = tsc_now() - t0;

    w->counts->read_ticks += took;
    note_slow(&w->counts->slow_read, took, w->counts->reads, lba, len);
    w->counts->reads++;
    count_size(w->counts->read_size, len);
    return s;
}

static EFI_STATUS EFIAPI fw_write_blocks(EFI_BLOCK_IO_PROTOCOL *This, UINT32 id, EFI_LBA lba, UINTN len, VOID *buf) {
    fw_bio_t *w = (fw_bio_t *)This;

    UINT64 t0 = tsc_now();
    EFI_STATUS s = w->real->WriteBlocks(w->real, id, lba, len, buf);

    UINT64 took = tsc_now() - t0;

    w->counts->write_ticks += took;
    note_slow(&w->counts->slow_write, took, w->counts->writes, lba, len);
    w->counts->writes++;
    count_size(w->counts->write_size, len);
    return s;
}

static EFI_STATUS EFIAPI fw_flush_blocks(EFI_BLOCK_IO_PROTOCOL *This) {
    fw_bio_t *w = (fw_bio_t *)This;

    UINT64 t0 = tsc_now();
    EFI_STATUS s = w->real->FlushBlocks(w->real);

    w->counts->flush_ticks += tsc_now() - t0;
    w->counts->flushes++;
    return s;
}

static EFI_STATUS EFIAPI fw_setpos(EFI_FILE_PROTOCOL *This, UINT64 pos) {
    fw_file_t *w = (fw_file_t *)This;

    w->counts->seeks++;
    return w->real->SetPosition(w->real, pos);
}

static EFI_STATUS EFIAPI fw_read(EFI_FILE_PROTOCOL *This, UINTN *len, VOID *buf) {
    fw_file_t *w = (fw_file_t *)This;

    UINT64 t0 = tsc_now();
    UINTN n = *len;
    EFI_STATUS s = w->real->Read(w->real, len, buf);

    w->counts->read_ticks += tsc_now() - t0;
    w->counts->reads++;
    count_size(w->counts->read_size, n);
    return s;
}

static EFI_STATUS EFIAPI fw_write(EFI_FILE_PROTOCOL *This, UINTN *len, VOID *buf) {
    fw_file_t *w = (fw_file_t *)This;

    UINT64 t0 = tsc_now();
    UINTN n = *len;
    EFI_STATUS s = w->real->Write(w->real, len, buf);

    w->counts->write_ticks += tsc_now() - t0;
    w->counts->writes++;
    count_size(w->counts->write_size, n);
    return s;
}

static EFI_STATUS EFIAPI fw_flush(EFI_FILE_PROTOCOL *This) {
    fw_file_t *w = (fw_file_t *)This;

    UINT64 t0 = tsc_now();
    EFI_STATUS s = w->real->Flush(w->real);

    w->counts->flush_ticks += tsc_now() - t0;
    w->counts->flushes++;
    return s;
}

static EFI_STATUS EFIAPI fw_getinfo(EFI_FILE_PROTOCOL *This, EFI_GUID *type, UINTN *sz, VOID *buf) {
    fw_file_t *w = (fw_file_t *)This;

    return w->real->GetInfo(w->real, type, sz, buf);
}

static void fw_bio_init(struct bench_rwy *r, EFI_BLOCK_IO_PROTOCOL *real) {
    r->bio.real = real;
    r->bio.counts = &r->fw;
    r->bio.proto.Revision = real->Revision;
    r->bio.proto.Media = real->Media;
    r->bio.proto.ReadBlocks = fw_read_blocks;
    r->bio.proto.WriteBlocks = fw_write_blocks;
    r->bio.proto.FlushBlocks = fw_flush_blocks;
}

static void fw_file_init(struct bench_rwy *r, EFI_FILE_HANDLE real) {
    r->file.real = real;
    r->file.counts = &r->fw;
    r->file.proto.Revision = real->Revision;
    r->file.proto.SetPosition = fw_setpos;
    r->file.proto.Read = fw_read;
    r->file.proto.Write = fw_write;
    r->file.proto.Flush = fw_flush;
    r->file.proto.GetInfo = fw_getinfo;
}

static void print_sizes(const UINT64 *hist) {
    static const char *const label[BENCH_BUCKETS] = { "4K", "16K", "64K", "256K", "1M", "big" };
    const char *sep = "";

    uprint("{");
    for (UINTN i = 0; i < BENCH_BUCKETS; i++) {
        if (!hist[i]) continue;
        uprintf("%s%s:%llu", sep, label[i], (unsigned long long)hist[i]);
        sep = " ";
    }
    uprint("}");
}

void bench_calls_reset(bench_dev_t *d) {
    struct bench_rwy *r = d->rwy;

    if (!r) return;
    memset(&r->fw, 0, sizeof(r->fw));
    if (r->inst) r->io0 = r->inst->store.io;
    else memset(&r->io0, 0, sizeof(r->io0));
}

/* Only a call of 10 ms or more is worth a line. */
static void print_slow(const char *dev, const char *op, UINTN run, const char *dir, const bench_slow_t *slow,
                       UINT64 calls) {
    UINT64 us = (UINT64)(tsc_to_seconds(slow->ticks) * 1e6 + 0.5);

    if (us < 10000) return;
    uprintf("IOBENCH slow %-8s %-5s %4u KiB: %s call %llu of %llu took %llu us (lba %llu, %llu bytes)\n", dev, op,
            (unsigned)(run / 1024), dir, (unsigned long long)slow->index, (unsigned long long)calls,
            (unsigned long long)us, (unsigned long long)slow->lba, (unsigned long long)slow->len);
}

void bench_calls_print(const bench_dev_t *d, const char *op, UINTN run) {
    const struct bench_rwy *r = d->rwy;
    UINT64 store_r, store_w;

    if (!r) return;
    store_r = r->inst ? core_calls(&r->inst->store.io, 0) - core_calls(&r->io0, 0) : 0;
    store_w = r->inst ? core_calls(&r->inst->store.io, 1) - core_calls(&r->io0, 1) : 0;
    uprintf("IOBENCH calls %-8s %-5s ", d->name, op);
    if (run) uprintf("%4u KiB: ", (unsigned)(run / 1024));
    else uprint("   open: ");
    uprintf("store r=%llu w=%llu, firmware r=%llu ", (unsigned long long)store_r, (unsigned long long)store_w,
            (unsigned long long)r->fw.reads);
    print_sizes(r->fw.read_size);
    uprintf(" w=%llu ", (unsigned long long)r->fw.writes);
    print_sizes(r->fw.write_size);
    uprintf(" f=%llu s=%llu\n", (unsigned long long)r->fw.flushes, (unsigned long long)r->fw.seeks);
    print_slow(d->name, op, run, "read", &r->fw.slow_read, r->fw.reads);
    print_slow(d->name, op, run, "write", &r->fw.slow_write, r->fw.writes);
}

void bench_fw_emit(const bench_dev_t *d, const char *op, UINTN run) {
    const struct bench_rwy *r = d->rwy;

    if (r) bench_sample_emit(d, op, run, op[0] == 'w' ? r->fw.write_ticks : r->fw.read_ticks, 0, 1);
}

void bench_calls_get(const bench_dev_t *d, int writing, UINT64 *core, UINT64 *dev) {
    const struct bench_rwy *r = d->rwy;

    *core = 0;
    *dev = 0;
    if (!r) return;
    if (r->inst) *core = core_calls(&r->inst->store.io, writing) - core_calls(&r->io0, writing);
    *dev = writing ? r->fw.writes : r->fw.reads;
}

void bench_calls_all(const bench_dev_t *d, UINT64 *core, UINT64 *dev) {
    const struct bench_rwy *r = d->rwy;

    *core = 0;
    *dev = 0;
    if (!r) return;
    if (r->inst) *core = core_calls(&r->inst->store.io, 0) + core_calls(&r->inst->store.io, 1) - core_calls(&r->io0, 0) - core_calls(&r->io0, 1);
    *dev = r->fw.reads + r->fw.writes;
}

void bench_rwy_mount_cost(const bench_dev_t *d, UINT64 *ticks, UINT64 *core, UINT64 *dev) {
    *ticks = d->rwy->mount_ticks;
    *core = d->rwy->mount_core;
    *dev = d->rwy->mount_dev;
}

rwy_store_t *bench_rwy_store(const bench_dev_t *d) {
    return &d->rwy->inst->store;
}

/* ---------- the store as a device ---------- */

static EFI_STATUS rwy_status(int rc) {
    return rc == RWY_OK ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

static EFI_STATUS rwy_dev_write(bench_dev_t *d, UINT64 off, UINTN len, void *buf) {
    return rwy_status(rwy_write(&d->rwy->inst->store, d->rwy->oid, off, buf, len));
}

static EFI_STATUS rwy_dev_read(bench_dev_t *d, UINT64 off, UINTN len, void *buf) {
    size_t n = len;
    int rc = rwy_read(&d->rwy->inst->store, d->rwy->oid, off, buf, &n);

    return rwy_status(rc == RWY_OK && n != len ? RWY_ERR_IO : rc);
}

/* A commit, then the next transaction as the file protocol opens it. */
static EFI_STATUS rwy_dev_flush(bench_dev_t *d) {
    rwy_store_t *s = &d->rwy->inst->store;
    int rc = rwy_commit(s);

    return rwy_status(rc == RWY_OK ? rwy_begin(s) : rc);
}

/* ---------- opening a store ---------- */

/* The size a store says it has, from the first superblock slot that is valid. */
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

static EFI_STATUS mount_store(bench_dev_t *d, const rwy_backend_t *be, EFI_HANDLE h, const UINT8 *dek) {
    EFI_STATUS s = rwy_fs_mount_group(be, &h, 1, dek, &d->rwy->inst);

    if (EFI_ERROR(s)) return s;
    d->rwy->mount_ticks = tsc_now() - d->rwy->mount_t0;
    d->rwy->mount_core = core_calls(&d->rwy->inst->store.io, 0) + core_calls(&d->rwy->inst->store.io, 1);
    d->rwy->mount_dev = d->rwy->fw.reads + d->rwy->fw.writes;
    d->write = rwy_dev_write;
    d->read = rwy_dev_read;
    d->flush = rwy_dev_flush;
    bench_calls_print(d, "open", 0);
    return EFI_SUCCESS;
}

static struct bench_rwy *new_state(bench_dev_t *d, const char *name) {
    struct bench_rwy *r = NULL;

    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, sizeof(*r), (void **)&r))) return NULL;
    memset(r, 0, sizeof(*r));
    r->mount_t0 = tsc_now();
    memset(d, 0, sizeof(*d));
    d->name = name;
    d->rwy = r;
    return r;
}

EFI_STATUS bench_rwy_block(bench_dev_t *d, EFI_HANDLE part, const UINT8 *dek) {
    EFI_GUID bio_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_BLOCK_IO_PROTOCOL *real = NULL;
    struct bench_rwy *r = new_state(d, dek ? "rwy-enc" : "rwy-blk");
    rwy_backend_t be;
    UINT64 size;
    EFI_STATUS s;

    if (!r) return EFI_OUT_OF_RESOURCES;
    d->encrypted = dek != NULL;
    s = gBS->HandleProtocol(part, &bio_guid, (void **)&real);
    if (EFI_ERROR(s)) return s;
    fw_bio_init(r, real);
    s = rwy_fs_block_backend(&r->bio.proto, &be);
    if (EFI_ERROR(s)) return s;
    s = store_size(&be.dev, &size);
    if (!EFI_ERROR(s)) s = rwy_fs_block_limit(&be, size);
    if (!EFI_ERROR(s)) s = mount_store(d, &be, part, dek);
    if (EFI_ERROR(s)) rwy_fs_block_close(&be);
    return s;
}

EFI_STATUS bench_rwy_file(bench_dev_t *d, EFI_HANDLE esp, EFI_FILE_HANDLE root) {
    EFI_GUID info_guid = EFI_FILE_INFO_ID;
    struct bench_rwy *r = new_state(d, "rwy-file");
    EFI_FILE_HANDLE fh = NULL;
    UINT8 info[256];
    UINTN info_size = sizeof(info);
    rwy_backend_t be;
    EFI_STATUS s;

    if (!r) return EFI_OUT_OF_RESOURCES;
    s = root->Open(root, &fh, L"\\RWYBENCH.DAT", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(s)) return s;
    d->fh = fh;
    s = fh->GetInfo(fh, &info_guid, &info_size, info);
    if (EFI_ERROR(s)) return s;
    fw_file_init(r, fh);
    s = rwy_fs_file_backend(&r->file.proto, ((EFI_FILE_INFO *)info)->FileSize, &be);
    if (EFI_ERROR(s)) return s;
    s = mount_store(d, &be, esp, NULL);
    if (EFI_ERROR(s)) rwy_fs_file_close(&be);
    return s;
}

UINT64 bench_rwy_store_size(const bench_dev_t *d) {
    return d->rwy->inst->store.sb.total_size;
}

void bench_dev_close(bench_dev_t *d) {
    if (!d) return;
    if (d->fh) {
        d->fh->Close(d->fh);
        d->fh = NULL;
    }
    if (d->rwy) {
        if (d->rwy->inst) {
            if (rwy_in_txn(&d->rwy->inst->store)) rwy_abort(&d->rwy->inst->store);
            rwy_fs_unmount_group(d->rwy->inst);
        }
        gBS->FreePool(d->rwy);
        d->rwy = NULL;
    }
}

EFI_STATUS bench_rwy_read_path(bench_dev_t *d, const char *path, char *buf, UINTN cap, UINTN *len) {
    rwy_store_t *s;
    rwy_attr_t a;
    uint64_t oid, off = 0;

    if (!d || !d->rwy || !d->rwy->inst) return EFI_NOT_READY;
    s = &d->rwy->inst->store;
    if (rwy_stat(s, path, &a) != RWY_OK) return EFI_NOT_FOUND;
    if (a.size >= cap) return EFI_BUFFER_TOO_SMALL;
    if (rwy_lookup(s, path, &oid) != RWY_OK) return EFI_NOT_FOUND;
    while (off < a.size) {
        size_t n = (size_t)(a.size - off);

        if (rwy_read(s, oid, off, buf + off, &n) != RWY_OK || !n) return EFI_DEVICE_ERROR;
        off += n;
    }
    buf[off] = 0;
    if (len) *len = (UINTN)off;
    return EFI_SUCCESS;
}

void bench_rwy_uuid(const bench_dev_t *d, char out[37]) {
    out[0] = 0;
    if (d->rwy && d->rwy->inst) rwy_format_uuid(d->rwy->inst->uuid, out);
}

/* Read the large object of the seed store the way a loader reads a payload. */
static EFI_STATUS read_object(bench_dev_t *d, UINT8 *buf) {
    rwy_store_t *s = &d->rwy->inst->store;
    EFI_STATUS st = EFI_SUCCESS;
    uint64_t oid, off;
    UINT64 t0, ticks;

    if (rwy_lookup(s, BENCH_OBJECT, &oid) != RWY_OK) return EFI_NOT_FOUND;
    bench_calls_reset(d);
    t0 = tsc_now();
    for (off = 0; off < AREA && !EFI_ERROR(st); off += OBJECT_RUN) {
        size_t n = OBJECT_RUN;

        if (rwy_read(s, oid, off, buf + off, &n) != RWY_OK || n != OBJECT_RUN) st = EFI_DEVICE_ERROR;
    }
    ticks = tsc_now() - t0;
    for (UINT64 i = 0; i < AREA && !EFI_ERROR(st); i++)
        if (buf[i] != (UINT8)(i + OBJECT_SEED)) st = EFI_COMPROMISED_DATA;
    bench_report(d->name, "obj", OBJECT_RUN, ticks, st);
    bench_calls_print(d, "obj", OBJECT_RUN);
    bench_sample_emit(d, "obj", OBJECT_RUN, ticks, 0, !EFI_ERROR(st));
    return st;
}

EFI_STATUS bench_rwy_prepare(bench_dev_t *d, UINT8 *buf) {
    rwy_store_t *s = &d->rwy->inst->store;
    EFI_STATUS st = bench_work_clean(d);

    if (!EFI_ERROR(st)) st = read_object(d, buf);

    if (EFI_ERROR(st)) return st;
    if (rwy_create(s, BENCH_FILE, RWY_S_IFREG | 0644, 0, 0) != RWY_OK ||
        rwy_lookup(s, BENCH_FILE, &d->rwy->oid) != RWY_OK)
        return EFI_DEVICE_ERROR;
    for (UINT64 off = 0; off < AREA && !EFI_ERROR(st); off += OBJECT_RUN)
        st = rwy_dev_write(d, off, OBJECT_RUN, buf + off);
    return EFI_ERROR(st) ? st : rwy_dev_flush(d);
}
