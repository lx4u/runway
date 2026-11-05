/*
 * rwy_fs.c — EFI_FILE_PROTOCOL wrappers over the v2 transactional core.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapter policy (thin: no filesystem policy lives here):
 * - One Runway transaction stays open for the session. Mutating EFI ops
 *   (create/write/rename/delete/truncate/mkdir) stage into it; EFI Flush
 *   or Close (for mutating handles) commits (atomic root publication) and opens
 *   the next transaction. A crash before commit loses only unflushed staged
 *   work; the mounted state is always the last committed generation.
 * - EFI-created files/dirs get documented defaults: 0644/0755,
 *   uid=gid=0. Writes never touch mode/uid/gid (core guarantee).
 * - Every EFI_FILE_HANDLE we hand out is a `rwy_file_t` whose FIRST member
 *   is the EFI_FILE_PROTOCOL struct, so `This` casts directly.
 * - Multi-tenant instances: each mounted filesystem is encapsulated in a
 *   `rwy_fs_instance_t`, and handles store a back-pointer to their parent
 *   instance. Protocol methods dynamically dispatch via `f->instance->store`.
 *
 * Backing I/O: the v2 block device reads/writes 4096 B blocks through the
 * block device of each backing; the file backend uses the FAT file handle of
 * RUNWAY.DAT (SetPosition + Read/Write). Boot-services
 * code is single-threaded, so no locking.
 */
#include "rwy_fs_internal.h"
#include "efi_util.h"
#include "../include/rwy_core.h"
#include "../include/rwy_block.h"
#include "../include/rwy_crypto.h"

static rwy_fs_instance_t *gInstanceList = NULL;

rwy_fs_instance_t *rwy_fs_get_instances(void) {
    return gInstanceList;
}

void rwy_fs_counters(UINT64 *rb, UINT64 *wb, UINT64 *ro, UINT64 *wo) {
    UINT64 r_bytes = 0, w_bytes = 0, r_ops = 0, w_ops = 0;
    rwy_fs_instance_t *cur = gInstanceList;
    while (cur) {
        r_bytes += cur->read_bytes;
        w_bytes += cur->write_bytes;
        r_ops += cur->read_ops;
        w_ops += cur->write_ops;
        cur = cur->next;
    }
    if (rb) *rb = r_bytes;
    if (wb) *wb = w_bytes;
    if (ro) *ro = r_ops;
    if (wo) *wo = w_ops;
}

void rwy_fs_dev_stats(const rwy_fs_instance_t *inst, rwy_dev_stats_t *out) {
    UINTN i;

    *out = (rwy_dev_stats_t){ 0 };
    for (i = 0; i < inst->num_backings; i++)
        if (inst->backings[i].stats) rwy_dev_stats_add(out, inst->backings[i].stats);
}

static int backing_pread(void *ctx, UINT64 off, void *buf, size_t len) {
    rwy_fs_instance_t *inst = (rwy_fs_instance_t *)ctx;
    if (!inst) return -1;
    int rc = rwy_mirror_read(&inst->mirror_ctx, off, buf, len);
    if (rc == 0) {
        inst->read_bytes += len;
        inst->read_ops++;
    }
    return rc;
}

static int backing_pwrite(void *ctx, UINT64 off, const void *buf, size_t len) {
    rwy_fs_instance_t *inst = (rwy_fs_instance_t *)ctx;
    if (!inst) return -1;
    int rc = rwy_mirror_write(&inst->mirror_ctx, off, buf, len);
    if (rc == 0) {
        inst->write_bytes += len;
        inst->write_ops++;
    }
    return rc;
}

static int backing_flush(void *ctx) {
    rwy_fs_instance_t *inst = (rwy_fs_instance_t *)ctx;
    if (!inst) return -1;
    return rwy_mirror_flush(&inst->mirror_ctx);
}

/* Ensure a transaction is open for staged work. */
int ensure_txn(rwy_fs_instance_t *inst) {
    if (!inst || !inst->mounted) return RWY_ERR_IO;
    if (!rwy_in_txn(&inst->store)) return rwy_begin(&inst->store);
    return RWY_OK;
}

/* Commit staged work and open a fresh transaction. */
static EFI_STATUS commit_txn(rwy_fs_instance_t *inst) {
    int rc;
    if (!inst || !inst->mounted) return EFI_DEVICE_ERROR;
    if (!rwy_in_txn(&inst->store)) return EFI_SUCCESS;
    /* Nothing staged (a write-mode handle closed unchanged, a refused write): keep the
     * generation. The working copy equals the committed state right after rwy_begin. */
    {
        const rwy_store_t *s = &inst->store;
        UINTN i;
        for (i = 0; i < RWY_MAX_INODES; i++)
            if (CompareMem(&s->w_inodes[i], &s->inodes[i], sizeof(s->inodes[i])) != 0) break;
        if (i == RWY_MAX_INODES && CompareMem(s->w_bitmap, s->bitmap, sizeof(s->bitmap)) == 0)
            return EFI_SUCCESS;
    }
    rc = rwy_commit(&inst->store);
    if (rc != RWY_OK)
        return (rc == RWY_ERR_NOSPC) ? EFI_VOLUME_FULL : EFI_DEVICE_ERROR;
    rc = rwy_begin(&inst->store);
    if (rc != RWY_OK) return EFI_DEVICE_ERROR;
    return EFI_SUCCESS;
}

EFI_STATUS rwy_rc_to_efi(int rc) {
    switch (rc) {
    case RWY_OK: return EFI_SUCCESS;
    case RWY_ERR_NOENT: return EFI_NOT_FOUND;
    case RWY_ERR_EXIST: return EFI_ACCESS_DENIED;
    case RWY_ERR_NOSPC: return EFI_VOLUME_FULL;
    case RWY_ERR_INVAL:
    case RWY_ERR_NAMETOOLONG: return EFI_INVALID_PARAMETER;
    case RWY_ERR_NOTDIR: return EFI_INVALID_PARAMETER;
    case RWY_ERR_ISDIR: return EFI_ACCESS_DENIED;
    case RWY_ERR_NOTEMPTY: return EFI_ACCESS_DENIED;
    case RWY_ERR_PERM: return EFI_ACCESS_DENIED;
    default: return EFI_DEVICE_ERROR;
    }
}

/* ---------- file handles ---------- */

static EFI_STATUS rwy_alloc_file(rwy_fs_instance_t *inst, int kind, rwy_file_t **out);

/* Forward decls of protocol methods. */
static EFI_STATUS EFIAPI tf_open(EFI_FILE_HANDLE F, EFI_FILE_HANDLE *N,
                                 CHAR16 *Nm, UINT64 M, UINT64 A);
static EFI_STATUS EFIAPI tf_close(EFI_FILE_HANDLE F);
static EFI_STATUS EFIAPI tf_delete(EFI_FILE_HANDLE F);
static EFI_STATUS EFIAPI tf_read(EFI_FILE_HANDLE F, UINTN *S, VOID *B);
static EFI_STATUS EFIAPI tf_write(EFI_FILE_HANDLE F, UINTN *S, VOID *B);
static EFI_STATUS EFIAPI tf_getpos(EFI_FILE_HANDLE F, UINT64 *P);
static EFI_STATUS EFIAPI tf_setpos(EFI_FILE_HANDLE F, UINT64 P);
static EFI_STATUS EFIAPI tf_flush(EFI_FILE_HANDLE F);

static void fill_proto(EFI_FILE_PROTOCOL *p) {
    p->Revision = EFI_FILE_PROTOCOL_REVISION; /* v1: no Ex async entry points */
    p->Open = (EFI_FILE_OPEN)tf_open;
    p->Close = (EFI_FILE_CLOSE)tf_close;
    p->Delete = (EFI_FILE_DELETE)tf_delete;
    p->Read = (EFI_FILE_READ)tf_read;
    p->Write = (EFI_FILE_WRITE)tf_write;
    p->GetPosition = (EFI_FILE_GET_POSITION)tf_getpos;
    p->SetPosition = (EFI_FILE_SET_POSITION)tf_setpos;
    p->GetInfo = (EFI_FILE_GET_INFO)tf_getinfo;
    p->SetInfo = (EFI_FILE_SET_INFO)tf_setinfo;
    p->Flush = (EFI_FILE_FLUSH)tf_flush;
    p->OpenEx = NULL;
    p->ReadEx = NULL;
    p->WriteEx = NULL;
    p->FlushEx = NULL;
}

static EFI_STATUS rwy_alloc_file(rwy_fs_instance_t *inst, int kind, rwy_file_t **out) {
    EFI_STATUS s;
    rwy_file_t *f;
    s = gBS->AllocatePool(EfiLoaderData, sizeof(*f), (void **)&f);
    if (EFI_ERROR(s)) return s;
    {
        UINT8 *p = (UINT8 *)f;
        for (UINTN i = 0; i < sizeof(*f); i++) p[i] = 0;
    }
    fill_proto(&f->proto);
    f->instance = inst;
    f->kind = kind;
    *out = f;
    return EFI_SUCCESS;
}

/* ---------- protocol methods ---------- */

/* UEFI file creation creates missing intermediate directories (as the
 * firmware FAT driver does). Walk each prefix of `path` and mkdir what
 * is missing. EFI defaults: 0755, uid=gid=0. */
static int ensure_parents(rwy_fs_instance_t *inst, const char *path) {
    char prefix[RWY_MAX_PATH];
    UINTN i = 0, k;
    if (!inst) return RWY_ERR_IO;
    while (path[i]) {
        while (path[i] && path[i] != '/') i++;
        if (!path[i]) break; /* last component is the file itself */
        if (i >= sizeof(prefix)) return RWY_ERR_NAMETOOLONG;
        for (k = 0; k < i; k++) prefix[k] = path[k];
        prefix[i] = 0;
        {
            UINT64 oid;
            int rc = rwy_lookup(&inst->store, prefix, &oid);
            if (rc == RWY_ERR_NOENT) {
                rc = rwy_mkdir(&inst->store, prefix,
                                RWY_S_IFDIR | RWY_EFI_DEFAULT_MODE_DIR,
                                RWY_EFI_DEFAULT_UID, RWY_EFI_DEFAULT_GID);
                if (rc != RWY_OK && rc != RWY_ERR_EXIST) return rc;
            } else if (rc != RWY_OK) {
                return rc;
            }
        }
        i++; /* skip '/' */
    }
    return RWY_OK;
}

/* Open a path that exists: a directory or a file. */
static EFI_STATUS open_existing(rwy_fs_instance_t *inst, const char *full, UINT64 oid, UINT64 M,
                                EFI_FILE_HANDLE *N) {
    rwy_file_t *nf = NULL;
    rwy_attr_t a;
    EFI_STATUS s;

    if (rwy_getattr(&inst->store, oid, &a) != RWY_OK)
        return EFI_DEVICE_ERROR;
    if (a.type == RWY_TYPE_DIR) {
        s = rwy_alloc_file(inst, RWY_FH_DIR, &nf);
        if (EFI_ERROR(s)) return s;
        copy_str(nf->dirpath, sizeof(nf->dirpath), full);
        *N = &nf->proto;
        return EFI_SUCCESS;
    }
    s = rwy_alloc_file(inst, RWY_FH_FILE, &nf);
    if (EFI_ERROR(s)) return s;
    nf->oid = oid;
    nf->open_mode = M;
    nf->pos = 0;
    copy_str(nf->name, sizeof(nf->name), full);
    *N = &nf->proto;
    return EFI_SUCCESS;
}

/* Create a file, and the directories above it. */
static EFI_STATUS open_create(rwy_fs_instance_t *inst, const char *full, UINT64 M, EFI_FILE_HANDLE *N) {
    rwy_file_t *nf = NULL;
    UINT64 oid;
    EFI_STATUS s = rwy_alloc_file(inst, RWY_FH_FILE, &nf);
    int rc;

    if (EFI_ERROR(s)) return s;
    rc = ensure_parents(inst, full);
    if (rc != RWY_OK) {
        gBS->FreePool(nf);
        return (rc == RWY_ERR_NOSPC) ? EFI_VOLUME_FULL : EFI_INVALID_PARAMETER;
    }
    /* Distinguish mkdir intent: CREATE on EFI is file creation; dirs
     * appear implicitly via path prefixes (mkdir via explicit caller). */
    rc = rwy_create(&inst->store, full, RWY_S_IFREG | RWY_EFI_DEFAULT_MODE_FILE,
                     RWY_EFI_DEFAULT_UID, RWY_EFI_DEFAULT_GID);
    if (rc != RWY_OK) {
        gBS->FreePool(nf);
        return (rc == RWY_ERR_EXIST) ? EFI_ACCESS_DENIED : EFI_VOLUME_FULL;
    }
    if (rwy_lookup(&inst->store, full, &oid) != RWY_OK) {
        gBS->FreePool(nf);
        return EFI_DEVICE_ERROR;
    }
    nf->oid = oid;
    nf->open_mode = M;
    nf->pos = 0;
    copy_str(nf->name, sizeof(nf->name), full);
    *N = &nf->proto;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI tf_open(EFI_FILE_HANDLE F, EFI_FILE_HANDLE *N,
                                 CHAR16 *Nm, UINT64 M, UINT64 A) {
    rwy_file_t *dir = (rwy_file_t *)F;
    rwy_file_t *nf = NULL;
    char aname[RWY_MAX_PATH], full[RWY_MAX_PATH];
    EFI_STATUS s;
    UINT64 oid;
    int rc;
    (void)A;
    if (!F || !N || !Nm) return EFI_INVALID_PARAMETER;
    if (dir->kind != RWY_FH_DIR) return EFI_INVALID_PARAMETER;
    rwy_fs_instance_t *inst = dir->instance;
    if (!inst) return EFI_DEVICE_ERROR;

    s = c16_to_store(Nm, aname, sizeof(aname));
    if (EFI_ERROR(s)) return s;
    s = resolve_path(dir->dirpath, aname, full, sizeof(full));
    if (EFI_ERROR(s)) return s;
    rc = ensure_txn(inst);
    if (rc != RWY_OK) return EFI_DEVICE_ERROR;
    if (!full[0]) {
        s = rwy_alloc_file(inst, RWY_FH_DIR, &nf);
        if (EFI_ERROR(s)) return s;
        *N = &nf->proto;
        return EFI_SUCCESS;
    }
    /* FAT has no links: a final symlink (BOOTED) opens as what it points to */
    rc = rwy_lookup_follow(&inst->store, full, 1, &oid);
    if (rc == RWY_OK) return open_existing(inst, full, oid, M, N);
    if (M & EFI_FILE_MODE_CREATE) return open_create(inst, full, M, N);
    return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI tf_close(EFI_FILE_HANDLE F) {
    rwy_file_t *f = (rwy_file_t *)F;
    EFI_STATUS status = EFI_SUCCESS;
    if (!F) return EFI_INVALID_PARAMETER;
    if (f->kind == RWY_FH_FILE && (f->open_mode & EFI_FILE_MODE_WRITE)) {
        if (f->instance) {
            EFI_STATUS s = commit_txn(f->instance);
            if (EFI_ERROR(s)) status = s;
            if (backing_flush(f->instance) != 0 && !EFI_ERROR(status)) {
                status = EFI_DEVICE_ERROR;
            }
        }
    }
    gBS->FreePool(F);
    return status;
}

static EFI_STATUS EFIAPI tf_delete(EFI_FILE_HANDLE F) {
    rwy_file_t *f = (rwy_file_t *)F;
    int rc;
    if (!F) return EFI_INVALID_PARAMETER;
    rwy_fs_instance_t *inst = f->instance;
    if (!inst) { gBS->FreePool(F); return EFI_DEVICE_ERROR; }
    rc = ensure_txn(inst);
    if (rc != RWY_OK) { gBS->FreePool(F); return EFI_DEVICE_ERROR; }
    if (f->kind == RWY_FH_DIR) {
        char names[512];
        size_t cnt = 64;
        UINT64 doid;
        if (!f->dirpath[0]) return EFI_ACCESS_DENIED; /* never delete root */
        rc = rwy_readdir(&inst->store, f->dirpath, names, sizeof(names), &cnt, 0);
        if (rc != RWY_OK) { gBS->FreePool(F); return rwy_rc_to_efi(rc); }
        if (cnt != 0) { gBS->FreePool(F); return EFI_ACCESS_DENIED; }
        if (rwy_lookup(&inst->store, f->dirpath, &doid) == RWY_OK) {
            /* Explicitly created (empty) dir: remove its inode. */
            gBS->FreePool(F);
            rc = rwy_rmdir(&inst->store, f->dirpath);
            if (rc != RWY_OK) return rwy_rc_to_efi(rc);
            return commit_txn(inst);
        }
        /* Purely virtual dir (prefix only): nothing to unlink. */
        gBS->FreePool(F);
        return EFI_SUCCESS;
    }
    rc = rwy_unlink(&inst->store, f->name);
    gBS->FreePool(F);
    if (rc == RWY_ERR_NOENT) return EFI_NOT_FOUND;
    return (rc == RWY_OK) ? EFI_SUCCESS : rwy_rc_to_efi(rc);
}

/* Fill an EFI_FILE_INFO from the stat of directory entry `full`. */
static void fill_dir_info(rwy_fs_instance_t *inst, const char *full, EFI_FILE_INFO *fi) {
    rwy_attr_t a;

    fi->FileSize = 0;
    fi->PhysicalSize = 0;
    fi->Attribute = EFI_FILE_ARCHIVE;
    if (rwy_getattr_path(&inst->store, full, &a) == RWY_OK) {
        if (a.type == RWY_TYPE_DIR) fi->Attribute = EFI_FILE_DIRECTORY;
        else { fi->FileSize = a.size; fi->PhysicalSize = a.size; }
        unix_to_efitime(a.mtime_sec, &fi->ModificationTime);
        unix_to_efitime(a.atime_sec, &fi->LastAccessTime);
        fi->CreateTime = fi->ModificationTime;
    } else {
        fi->CreateTime.Year = 2026; fi->CreateTime.Month = 10; fi->CreateTime.Day = 7;
        fi->LastAccessTime = fi->CreateTime;
        fi->ModificationTime = fi->CreateTime;
    }
}

/* Read the next entry of an open directory as an EFI_FILE_INFO. */
static EFI_STATUS read_dir_entry(rwy_file_t *f, UINTN *S, VOID *B) {
    rwy_fs_instance_t *inst = f->instance;
    char base[RWY_NAME_MAX + 1], full[RWY_MAX_PATH];
    uint8_t type = 0;
    UINTN wl = 0, need, k = 0;
    EFI_FILE_INFO *fi;
    int rc;

    if (!B) return EFI_INVALID_PARAMETER;
    rc = rwy_readdir_nth(&inst->store, f->dirpath, f->enum_cursor, base, sizeof(base), &type);
    if (rc == RWY_ERR_NOENT) { *S = 0; return EFI_SUCCESS; }
    if (rc != RWY_OK) return EFI_DEVICE_ERROR;
    while (base[wl]) wl++;
    need = SIZE_OF_EFI_FILE_INFO + (wl + 1) * sizeof(CHAR16);
    if (*S < need) { *S = need; return EFI_BUFFER_TOO_SMALL; }
    if (f->dirpath[0]) {
        copy_str(full, sizeof(full), f->dirpath);
        while (full[k]) k++;
        if (k + 1 < sizeof(full)) full[k++] = '/';
    }
    for (UINTN i = 0; base[i] && k + 1 < sizeof(full); i++) full[k++] = base[i];
    full[k] = 0;
    fi = (EFI_FILE_INFO *)B;
    fi->Size = need;
    fill_dir_info(inst, full, fi);
    ascii_to_c16(base, fi->FileName, (*S - SIZE_OF_EFI_FILE_INFO) / sizeof(CHAR16));
    *S = need;
    f->enum_cursor++;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI tf_read(EFI_FILE_HANDLE F, UINTN *S, VOID *B) {
    rwy_file_t *f = (rwy_file_t *)F;
    size_t want, got;
    int rc;
    if (!F || !S) return EFI_INVALID_PARAMETER;
    rwy_fs_instance_t *inst = f->instance;
    if (!inst) return EFI_DEVICE_ERROR;

    if (f->kind == RWY_FH_DIR) return read_dir_entry(f, S, B);
    want = *S;
    got = want;
    if (want == 0) return EFI_SUCCESS;
    if (!B) return EFI_INVALID_PARAMETER;
    rc = rwy_read(&inst->store, f->oid, f->pos, B, &got);
    if (rc == RWY_ERR_NOENT) return EFI_NOT_FOUND;
    if (rc != RWY_OK) return EFI_DEVICE_ERROR;
    f->pos += got;
    *S = (UINTN)got;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI tf_write(EFI_FILE_HANDLE F, UINTN *S, VOID *B) {
    rwy_file_t *f = (rwy_file_t *)F;
    int rc;
    if (!F || !S) return EFI_INVALID_PARAMETER;
    rwy_fs_instance_t *inst = f->instance;
    if (!inst) return EFI_DEVICE_ERROR;
    if (f->kind != RWY_FH_FILE) return EFI_ACCESS_DENIED;
    if (!(f->open_mode & EFI_FILE_MODE_WRITE)) return EFI_ACCESS_DENIED;
    if (*S == 0) return EFI_SUCCESS;
    rc = ensure_txn(inst);
    if (rc != RWY_OK) return EFI_DEVICE_ERROR;
    rc = rwy_write(&inst->store, f->oid, f->pos, B, *S);
    if (rc == RWY_ERR_NOSPC) return EFI_VOLUME_FULL;
    if (rc != RWY_OK) return EFI_DEVICE_ERROR;
    f->pos += *S;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI tf_getpos(EFI_FILE_HANDLE F, UINT64 *P) {
    rwy_file_t *f = (rwy_file_t *)F;
    if (!F || !P) return EFI_INVALID_PARAMETER;
    if (f->kind != RWY_FH_FILE) return EFI_UNSUPPORTED;
    *P = f->pos;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI tf_setpos(EFI_FILE_HANDLE F, UINT64 P) {
    rwy_file_t *f = (rwy_file_t *)F;
    if (!F) return EFI_INVALID_PARAMETER;
    rwy_fs_instance_t *inst = f->instance;
    if (!inst) return EFI_DEVICE_ERROR;

    if (f->kind == RWY_FH_DIR) {
        if (P == 0) { f->enum_cursor = 0; return EFI_SUCCESS; }
        return EFI_UNSUPPORTED;
    }
    if (f->kind != RWY_FH_FILE) return EFI_UNSUPPORTED;
    if (P == 0xFFFFFFFFFFFFFFFFULL) {
        rwy_attr_t a;
        if (rwy_getattr(&inst->store, f->oid, &a) != RWY_OK) return EFI_DEVICE_ERROR;
        f->pos = a.size;
        return EFI_SUCCESS;
    }
    f->pos = P;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI tf_flush(EFI_FILE_HANDLE F) {
    rwy_file_t *f = (rwy_file_t *)F;
    EFI_STATUS s;
    if (!F) return EFI_INVALID_PARAMETER;
    rwy_fs_instance_t *inst = f->instance;
    if (!inst) return EFI_DEVICE_ERROR;
    s = commit_txn(inst);
    if (EFI_ERROR(s)) return s;
    if (backing_flush(inst) != 0) return EFI_DEVICE_ERROR;
    return EFI_SUCCESS;
}

/* ---------- SimpleFileSystem ---------- */

static EFI_STATUS EFIAPI tf_openvolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                       EFI_FILE_HANDLE *Root) {
    rwy_fs_instance_t *inst;
    rwy_file_t *f;
    EFI_STATUS s;
    if (!This || !Root) return EFI_INVALID_PARAMETER;
    inst = CONTAINER_OF(This, rwy_fs_instance_t, sfs);

    s = rwy_alloc_file(inst, RWY_FH_DIR, &f);
    if (EFI_ERROR(s)) return s;
    f->dirpath[0] = 0;
    *Root = &f->proto;
    return EFI_SUCCESS;
}

static void members_close(rwy_crypto_dev_ctx_t *cx, UINTN num_backings) {
    UINT8 *p = (UINT8 *)cx;
    if (!cx) return;
    for (UINTN j = 0; j < sizeof(*cx) * num_backings; j++) p[j] = 0;
    gBS->FreePool(cx);
}

/* The members as the store sees them: decrypted when there is a key. members_close() frees `*cx`. */
static EFI_STATUS members_open(const rwy_backend_t backings[], UINTN num_backings, const UINT8 *dek,
                               rwy_block_dev_t devs[], rwy_crypto_dev_ctx_t **cx) {
    EFI_STATUS s = EFI_SUCCESS;
    UINTN i;
    *cx = NULL;
    if (!backings || num_backings == 0 || num_backings > RWY_MIRROR_MAX_DEVS) return EFI_INVALID_PARAMETER;
    if (dek) {
        s = gBS->AllocatePool(EfiLoaderData, sizeof(**cx) * num_backings, (void **)cx);
        if (EFI_ERROR(s)) return s;
    }
    for (i = 0; i < num_backings; i++) {
        if (dek) {
            if (rwy_crypto_dev_wrap(&devs[i], &(*cx)[i], &backings[i].dev, dek, RWY_SB_BLOCKS) != RWY_CRYPTO_OK) {
                members_close(*cx, num_backings);
                *cx = NULL;
                return EFI_DEVICE_ERROR;
            }
        } else {
            devs[i] = backings[i].dev;
        }
    }
    return EFI_SUCCESS;
}

EFI_STATUS rwy_fs_sync_records(const rwy_backend_t backings[], UINTN num_backings, const UINT8 *dek,
                                UINT32 rec_start) {
    rwy_block_dev_t devs[RWY_MIRROR_MAX_DEVS];
    rwy_crypto_dev_ctx_t *cx = NULL;
    EFI_STATUS s = members_open(backings, num_backings, dek, devs, &cx);
    if (EFI_ERROR(s)) return s;
    if (rwy_record_sync(devs, num_backings, rec_start, 1, NULL) != RWY_OK) s = EFI_INVALID_PARAMETER;
    members_close(cx, num_backings);
    return s;
}

EFI_STATUS rwy_fs_note_events(rwy_fs_instance_t *inst, const rwy_backend_t backings[], const UINT32 events[],
                              UINTN num_members, const UINT8 *dek) {
    rwy_block_dev_t devs[RWY_MIRROR_MAX_DEVS];
    rwy_crypto_dev_ctx_t *cx = NULL;
    EFI_STATUS s = members_open(backings, num_members, dek, devs, &cx);
    int rc = RWY_OK;
    if (EFI_ERROR(s)) return s;
    for (UINTN i = 0; i < num_members; i++) {
        UINT8 uuid[16];
        if (!events[i] || !rwy_block_member_uuid(&backings[i].dev, uuid)) continue;
        for (int ev = RWY_SS_EV_NONE + 1; ev <= RWY_SS_EV_LAST; ev++)
            if (events[i] & (1u << ev)) rc |= rwy_record_note_health(&inst->store, devs, num_members, uuid, ev);
    }
    rc |= rwy_mirror_note_failures(&inst->store, &inst->mirror_ctx, devs, num_members);
    rc |= rwy_mirror_note_members(&inst->store, &inst->mirror_ctx, devs, num_members);
    members_close(cx, num_members);
    return rc ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

/* Remember each backing and its partition handle; a file backend also needs the firmware SFS of the handle. */
static EFI_STATUS bind_members(rwy_fs_instance_t *inst, const rwy_backend_t backings[], EFI_HANDLE handles[],
                               UINTN num_backings) {
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

    inst->num_backings = num_backings;
    for (UINTN i = 0; i < num_backings; i++) {
        if (backings[i].kind != RWY_BACKEND_FILE && backings[i].kind != RWY_BACKEND_BLOCK)
            return EFI_INVALID_PARAMETER;
        inst->backings[i] = backings[i];
        inst->member_handles[i] = handles[i];
        if (!handles[i]) return EFI_INVALID_PARAMETER;
        if (backings[i].kind == RWY_BACKEND_BLOCK) continue;
        if (EFI_ERROR(gBS->HandleProtocol(handles[i], &sfs_guid, (void **)&inst->member_old_sfs[i])) ||
            !inst->member_old_sfs[i])
            return EFI_INVALID_PARAMETER;
    }
    return EFI_SUCCESS;
}

void rwy_fs_select_view(rwy_fs_instance_t *inst, UINTN idx, EFI_HANDLE baseline) {
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_HANDLE h = inst->member_handles[idx];

    inst->fat_handle = inst->rwy_handle = h;
    inst->anchor.rwy_fs_handle = h;
    inst->old_sfs = inst->member_old_sfs[idx];
    if (inst->backings[idx].kind == RWY_BACKEND_FILE) {
        inst->anchor.fat_fs_handle = h;
        inst->anchor.fat_sfs = inst->old_sfs;
        return;
    }
    /* A partition without a firmware file system: the baseline is a FAT volume beside it, if any */
    inst->anchor.fat_fs_handle = baseline;
    inst->anchor.fat_sfs = NULL;
    if (baseline) (void)gBS->HandleProtocol(baseline, &sfs_guid, (void **)&inst->anchor.fat_sfs);
}

static UINT64 backing_size(const rwy_backend_t *be) {
    return be->dev.size ? be->dev.size : RWY_STORE_SIZE;
}

/* The mirror over all backings, and the block device that fronts it. */
static void setup_block_dev(rwy_fs_instance_t *inst, UINT64 actual_size) {
    rwy_mirror_init(&inst->mirror_ctx);
    for (UINTN i = 0; i < inst->num_backings; i++) {
        rwy_block_dev_t bdev = inst->backings[i].dev;
        bdev.size = actual_size;
        rwy_mirror_add(&inst->mirror_ctx, &bdev);
    }
    inst->block_dev.read = backing_pread;
    inst->block_dev.write = backing_pwrite;
    inst->block_dev.flush = backing_flush;
    inst->block_dev.ctx = inst;
    inst->block_dev.size = actual_size;
    inst->block_dev.block_size = RWY_BLOCK_SIZE;
    inst->block_dev.stats = NULL;
    inst->block_dev.dircache = NULL;
}

/* Take the volume label of the firmware FAT, or a default when it has none. A partition has no label to take. */
static void read_volume_label(rwy_fs_instance_t *inst, EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *old_sfs) {
    EFI_FILE_HANDLE old_root = NULL;
    UINT8 fsi_buf[256];
    UINTN fsi_sz = sizeof(fsi_buf);
    EFI_GUID fsi_id = EFI_FILE_SYSTEM_INFO_ID;

    inst->volume_label[0] = 0;
    if (!old_sfs) return;
    if (!EFI_ERROR(old_sfs->OpenVolume(old_sfs, &old_root)) && old_root) {
        if (!EFI_ERROR(old_root->GetInfo(old_root, &fsi_id, &fsi_sz, fsi_buf))) {
            EFI_FILE_SYSTEM_INFO *orig_fsi = (EFI_FILE_SYSTEM_INFO *)fsi_buf;
            if (orig_fsi->VolumeLabel[0] != 0)
                set_label(inst, orig_fsi->VolumeLabel);
        }
        old_root->Close(old_root);
    }
    if (inst->volume_label[0] == 0) {
        inst->volume_label[0] = (CHAR16)'T';
        inst->volume_label[1] = (CHAR16)'F';
        inst->volume_label[2] = (CHAR16)'A';
        inst->volume_label[3] = (CHAR16)'T';
        inst->volume_label[4] = 0;
    }
}

/* Members that are copies of one another get their own UUIDs. A failure leaves them as they are. */
static void name_mirror_members(rwy_fs_instance_t *inst) {
    if (inst->num_backings > 1 && rwy_mirror_name_members(&inst->mirror_ctx, efi_random))
        RWY_WARN("mirror: a member could not be given its UUID\n");
}

/* Mount the store on the mirror (decrypted when there is a key). */
static EFI_STATUS mount_store(rwy_fs_instance_t *inst, const UINT8 *dek) {
    rwy_block_dev_t *pdev = &inst->block_dev;

    if (dek) {
        int err = rwy_crypto_dev_wrap(&inst->crypto_dev, &inst->crypto_ctx, &inst->block_dev, dek, RWY_SB_BLOCKS);
        if (err != RWY_CRYPTO_OK) return EFI_DEVICE_ERROR;
        pdev = &inst->crypto_dev;
    }
    if (rwy_mount(&inst->store, pdev) != RWY_OK) return EFI_VOLUME_CORRUPTED;
    if (rwy_begin(&inst->store) != RWY_OK) return EFI_DEVICE_ERROR;
    inst->mounted = 1;
    return EFI_SUCCESS;
}

EFI_STATUS rwy_fs_mount_group(const rwy_backend_t backings[], EFI_HANDLE handles[],
                               UINTN num_backings, const UINT8 *dek,
                               rwy_fs_instance_t **out_inst) {
    EFI_STATUS s;
    rwy_fs_instance_t *inst = NULL;

    if (!backings || !handles || num_backings == 0 || num_backings > RWY_MIRROR_MAX_DEVS ||
        !out_inst || !handles[0])
        return EFI_INVALID_PARAMETER;

    s = gBS->AllocatePool(EfiLoaderData, sizeof(rwy_fs_instance_t), (void **)&inst);
    if (EFI_ERROR(s)) return s;
    {
        UINT8 *p = (UINT8 *)inst;
        for (UINTN j = 0; j < sizeof(*inst); j++) p[j] = 0;
    }
    s = bind_members(inst, backings, handles, num_backings);
    if (!EFI_ERROR(s)) {
        setup_block_dev(inst, backing_size(&backings[0]));
        name_mirror_members(inst);
        s = mount_store(inst, dek);
    }
    if (EFI_ERROR(s)) {
        gBS->FreePool(inst);
        return s;
    }

    for (UINTN i = 0; i < 16; i++) inst->uuid[i] = inst->store.sb.uuid[i];
    inst->anchor.magic = RWY_ANCHOR_MAGIC;
    inst->anchor.version = 2;
    inst->anchor.store_size = inst->store.sb.total_size;
    rwy_fs_select_view(inst, 0, NULL);
    read_volume_label(inst, inst->old_sfs);
    inst->is_boot_volume = (gInstanceList == NULL);

    inst->sfs.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    inst->sfs.OpenVolume = (EFI_VOLUME_OPEN)tf_openvolume;

    inst->next = gInstanceList;
    gInstanceList = inst;
    *out_inst = inst;
    return EFI_SUCCESS;
}

void rwy_fs_unmount_group(rwy_fs_instance_t *inst) {
    rwy_fs_instance_t **pp = &gInstanceList;

    if (!inst) return;
    for (UINTN i = 0; i < inst->num_backings; i++) {
        if (inst->backings[i].kind == RWY_BACKEND_BLOCK) rwy_fs_block_close(&inst->backings[i]);
        else if (inst->backings[i].kind == RWY_BACKEND_FILE) rwy_fs_file_close(&inst->backings[i]);
    }
    while (*pp && *pp != inst) pp = &(*pp)->next;
    if (*pp) *pp = inst->next;
    gBS->FreePool(inst);
}

/* Put the file protocol on a member handle: reinstalled over the firmware one, or installed on a partition
 * that has none. */
static EFI_STATUS install_view(rwy_fs_instance_t *inst, UINTN idx) {
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_HANDLE h = inst->member_handles[idx];

    if (inst->backings[idx].kind == RWY_BACKEND_BLOCK)
        return gBS->InstallProtocolInterface(&h, &sfs_guid, EFI_NATIVE_INTERFACE, &inst->sfs);
    return gBS->ReinstallProtocolInterface(h, &sfs_guid, inst->member_old_sfs[idx], &inst->sfs);
}

EFI_STATUS rwy_fs_install_group(rwy_fs_instance_t *inst) {
    EFI_STATUS s;
    EFI_GUID anchor_guid = RWY_ANCHOR_GUID;
    UINTN i;

    if (!inst || !inst->mounted) return EFI_INVALID_PARAMETER;

    /* Put the file protocol on ONE member: the instance's view handle (the boot partition for the
     * boot group). Secondary mirror members keep their firmware FAT: exposing one volume as N
     * identical SFS handles makes consumers that count volumes see N copies. */
    for (i = 0; i < inst->num_backings; i++) {
        if (inst->member_handles[i] != inst->fat_handle) continue;
        s = install_view(inst, i);
        if (EFI_ERROR(s)) return s;
        break;
    }
    inst->installed = TRUE;

    /* Install or reinstall Runway vendor anchor protocol on primary partition handle */
    {
        EFI_HANDLE h = inst->fat_handle;
        rwy_anchor_t *existing_anchor = NULL;
        if (EFI_ERROR(gBS->HandleProtocol(h, &anchor_guid, (void **)&existing_anchor))) {
            gBS->InstallProtocolInterface(&h, &anchor_guid, EFI_NATIVE_INTERFACE, &inst->anchor);
        } else {
            gBS->ReinstallProtocolInterface(h, &anchor_guid, existing_anchor, &inst->anchor);
        }
    }
    return EFI_SUCCESS;
}

EFI_STATUS rwy_fs_create_mirror(const rwy_backend_t backings[], UINTN num_backings,
                                 EFI_HANDLE fat_fs_handle, const UINT8 *dek,
                                 EFI_HANDLE *out_handle) {
    EFI_HANDLE handles[RWY_MIRROR_MAX_DEVS];
    rwy_fs_instance_t *inst = NULL;
    EFI_STATUS s;
    UINTN i;

    if (!out_handle || !fat_fs_handle || num_backings > RWY_MIRROR_MAX_DEVS)
        return EFI_INVALID_PARAMETER;
    for (i = 0; i < num_backings; i++) handles[i] = fat_fs_handle;
    /* Legacy single-handle form: only the primary partition is intercepted. */
    s = rwy_fs_mount_group(backings, handles, num_backings, dek, &inst);
    if (EFI_ERROR(s)) return s;
    inst->num_backings = num_backings; /* unchanged; install loop re-hits the same handle */
    inst->member_handles[0] = fat_fs_handle;
    {
        UINTN n = inst->num_backings;
        inst->num_backings = 1;
        s = rwy_fs_install_group(inst);
        inst->num_backings = n;
    }
    if (EFI_ERROR(s)) return s;
    *out_handle = fat_fs_handle;
    return EFI_SUCCESS;
}

EFI_STATUS rwy_fs_set_label(rwy_fs_instance_t *inst, const CHAR16 *label) {
    if (!inst || !label) return EFI_INVALID_PARAMETER;
    UINTN l = 0;
    while (label[l] && l < 31) {
        inst->volume_label[l] = label[l];
        l++;
    }
    inst->volume_label[l] = 0;
    return EFI_SUCCESS;
}

const CHAR16 *rwy_fs_get_label(const rwy_fs_instance_t *inst) {
    if (!inst) return NULL;
    return inst->volume_label;
}

EFI_STATUS rwy_fs_create_crypto(const rwy_backend_t *backing, EFI_HANDLE fat_fs_handle,
                                const UINT8 *dek, EFI_HANDLE *out_handle) {
    return rwy_fs_create_mirror(backing, 1, fat_fs_handle, dek, out_handle);
}

EFI_STATUS rwy_fs_create(const rwy_backend_t *backing, EFI_HANDLE fat_fs_handle,
                          EFI_HANDLE *out_handle) {
    return rwy_fs_create_mirror(backing, 1, fat_fs_handle, NULL, out_handle);
}
