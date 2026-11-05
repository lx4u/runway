/*
 * rwy_slotboot.c — Select a slot of the boot store and load its payload.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_boot.h"
#include "rwy_flight.h"
#include "rwy_loopguard.h"

static void native_log(void *ctx, const char *line) {
    (void)ctx;
    uprint(line);
    uprint("\n");
}

/* Read EFI/RUNWAY/<slot>/<file> (following links) through the core. Absent is not an error. */
EFI_STATUS native_read(rwy_store_t *st, const char *slot, const char *file, blob_t *b) {
    char path[64];
    uint64_t oid;
    rwy_attr_t a;
    size_t n = 0, got;
    const char *p;
    b->buf = NULL;
    b->size = 0;
    for (p = "EFI/RUNWAY/"; *p; p++) path[n++] = *p;
    for (p = slot; *p; p++) path[n++] = *p;
    path[n++] = '/';
    for (p = file; *p; p++) path[n++] = *p;
    path[n] = 0;
    if (rwy_lookup_follow(st, path, 1, &oid) != RWY_OK) return EFI_NOT_FOUND;
    if (rwy_getattr(st, oid, &a) != RWY_OK) return EFI_DEVICE_ERROR;
    if (a.size == 0) return EFI_SUCCESS;
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, (UINTN)a.size, (void **)&b->buf))) return EFI_OUT_OF_RESOURCES;
    got = (size_t)a.size;
    if (rwy_read(st, oid, 0, b->buf, &got) != RWY_OK || got != (size_t)a.size) {
        gBS->FreePool(b->buf);
        b->buf = NULL;
        return EFI_DEVICE_ERROR;
    }
    b->size = (UINTN)a.size;
    return EFI_SUCCESS;
}

void native_free(native_img_t *img) {
    blob_t *b[] = { &img->kernel, &img->initrd, &img->dtb, &img->args };
    for (UINTN i = 0; i < 4; i++)
        if (b[i]->buf) gBS->FreePool(b[i]->buf);
    if (img->child) gBS->UnloadImage(img->child);
    memset(img, 0, sizeof(*img));
}

/* Load the image in img->kernel (the Secure Boot verdict included). */
EFI_STATUS native_image_load(native_img_t *img, EFI_HANDLE parent, EFI_HANDLE fat_fs_handle) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID, dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH *path = NULL;
    EFI_STATUS s = gBS->HandleProtocol(fat_fs_handle, &dp_guid, (void **)&path);

    if (!EFI_ERROR(s)) s = gBS->LoadImage(FALSE, parent, path, img->kernel.buf, img->kernel.size, &img->child);
    if (!EFI_ERROR(s)) s = gBS->HandleProtocol(img->child, &li_guid, (void **)&img->li);
    return s;
}

/* Read the slot's KERNEL and load it, then read INITRD, DTB and ARGS.
 * Must run while the firmware handles are pristine. On an error nothing is left allocated. */
static EFI_STATUS native_load(rwy_store_t *st, const char *slot, EFI_HANDLE parent, EFI_HANDLE fat_fs_handle,
                              native_img_t *img) {
    EFI_STATUS s;
    memset(img, 0, sizeof(*img));
    s = native_read(st, slot, "KERNEL", &img->kernel);
    if (!EFI_ERROR(s) && !img->kernel.size) s = EFI_LOAD_ERROR;
    if (!EFI_ERROR(s)) s = native_image_load(img, parent, fat_fs_handle);
    if (EFI_ERROR(s)) {
        native_free(img);
        return s;
    }
    (void)native_read(st, slot, "INITRD", &img->initrd);
    (void)native_read(st, slot, "DTB", &img->dtb);
    (void)native_read(st, slot, "ARGS", &img->args);
    return EFI_SUCCESS;
}

/* Commit tries after the first one, before RUNWAY.EFI boots without recording the selection. */
#define COMMIT_RETRIES 3

/* The store does not match its counter, or its tree is older than its record: no slot boots. RECOVERY
 * still can, loaded without a write, since it has no state that a rollback could undo. */
static EFI_STATUS native_rolled_back(rwy_store_t *bst, EFI_HANDLE self, EFI_HANDLE fat_fs_handle, native_img_t *img,
                                     rwy_boot_choice_t *choice, int *unrecorded) {
    int rc;
    anchor_report_rollback(bst);
    if (rwy_in_txn(bst)) rwy_abort(bst);
    rwy_set_actor(bst, RWY_ACTOR_SYSTEM);
    rc = rwy_bootcfg_fallback(bst, choice, NULL);
    rwy_set_actor(bst, RWY_ACTOR_USER);
    if (rc == RWY_ERR_ROLLBACK && choice->booted && !EFI_ERROR(native_load(bst, choice->slot, self, fat_fs_handle, img))) {
        *unrecorded = 1;
        return EFI_SUCCESS;
    }
    flush_and_stall(3000000);
    return EFI_NOT_FOUND;
}

/* The slot to boot when the selection cannot be committed, loaded in advance: after a failed write the
 * firmware file system may answer nothing but errors. `fb` holds the first fallback() result; walk down
 * from it until a payload loads. fb->booted is 0 when none does. */
static void native_load_fallback(rwy_store_t *bst, EFI_HANDLE self, EFI_HANDLE fat_fs_handle,
                                 rwy_boot_choice_t *fb, native_img_t *fbimg) {
    char after[sizeof(fb->slot)];
    unsigned i;
    while (fb->booted) {
        EFI_STATUS s = native_load(bst, fb->slot, self, fat_fs_handle, fbimg);
        if (!EFI_ERROR(s)) return;
        RWY_WARN("%s cannot be started (0x%llx), not usable as a fallback\n", fb->slot, (unsigned long long)s);
        if (fb->slot[0] != 'C') break;   /* RECOVERY is the last one */
        for (i = 0; i < sizeof(after); i++) after[i] = fb->slot[i];
        rwy_set_actor(bst, RWY_ACTOR_SYSTEM);
        if (rwy_bootcfg_fallback(bst, fb, after) != RWY_OK) break;
        rwy_set_actor(bst, RWY_ACTOR_USER);
    }
    rwy_set_actor(bst, RWY_ACTOR_USER);
    fb->booted = 0;
}

#define GUARD_ASKED_MAX 8u

typedef struct {
    rwy_store_t *bst;
    EFI_HANDLE self, fat_fs_handle;
    native_img_t *img;
    rwy_boot_choice_t *choice;
    int *unrecorded;
    native_img_t fbimg;            /* payload of the slot to fall back on */
    rwy_boot_choice_t pre, fb;    /* what selection will choose, and the fallback */
    char pre_slot[sizeof(((rwy_boot_choice_t *)0)->slot)];
    int commit_err, retries, have_pre, fb_is_pre;
    struct { uint64_t rev; int demote; } asked[GUARD_ASKED_MAX];   /* the guard counts an entry once, retries reuse it */
    unsigned nasked;
} nsel_t;

/* Read what may be needed before the commit: the slot selection will choose, and the one to fall back on.
 * *rolled is set when the tree is older than the record. */
static void native_preload(nsel_t *n, int *rolled) {
    rwy_store_t *bst = n->bst;
    int rc;

    *rolled = 0;
    if (rwy_in_txn(bst)) rwy_abort(bst);
    anchor_attach(bst);
    rwy_set_actor(bst, RWY_ACTOR_SYSTEM);
    rc = rwy_bootcfg_preview(bst, &n->pre);
    if (rc == RWY_ERR_ROLLBACK) {
        rwy_set_actor(bst, RWY_ACTOR_USER);
        *rolled = 1;
        return;
    }
    if (rc == RWY_OK && n->pre.engaged && n->pre.booted) {
        if (!EFI_ERROR(native_load(bst, n->pre.slot, n->self, n->fat_fs_handle, n->img))) {
            n->have_pre = 1;
            for (unsigned i = 0; i < sizeof(n->pre_slot); i++) n->pre_slot[i] = n->pre.slot[i];
        }
        if (rwy_bootcfg_fallback(bst, &n->fb, NULL) != RWY_OK) n->fb.booted = 0;
        n->fb_is_pre = n->have_pre && n->fb.booted && !rwy_strcmp(n->fb.slot, n->pre.slot);
        if (!n->fb_is_pre) native_load_fallback(bst, n->self, n->fat_fs_handle, &n->fb, &n->fbimg);
    }
    rwy_set_actor(bst, RWY_ACTOR_USER);
}

/* Count the entry into the OK slot `rev` once per boot: a retried selection gets the same answer. */
static int native_guard(void *ctx, uint64_t rev, const rwy_loop_guard_t *g) {
    nsel_t *n = ctx;
    unsigned i;
    for (i = 0; i < n->nasked; i++)
        if (n->asked[i].rev == rev) return n->asked[i].demote;
    if (n->nasked == GUARD_ASKED_MAX) return 0;
    n->asked[n->nasked].rev = rev;
    n->asked[n->nasked].demote = guard_check(rev, g) == RWY_GUARD_DEMOTE;
    return n->asked[n->nasked++].demote;
}

/* One selection and the commit that records it; returns the core's result. */
static int native_select_once(nsel_t *n) {
    rwy_store_t *bst = n->bst;
    rwy_boot_choice_t *choice = n->choice;
    int rc;

    /* The adapter keeps an (empty, nothing staged yet) transaction open; selection runs its own. */
    if (rwy_in_txn(bst)) rwy_abort(bst);
    rwy_set_actor(bst, RWY_ACTOR_SYSTEM);
    rc = rwy_bootcfg_select_guarded(bst, choice, native_log, NULL, native_guard, n);
    rwy_set_actor(bst, RWY_ACTOR_USER);
    if (rc == RWY_OK && choice->loop_slot[0]) guard_clear();
    /* After a failed write the store may read as empty, or every slot as incomplete: neither is a legacy
     * boot nor a store with nothing to run. */
    if (rc == RWY_OK && n->commit_err && (!choice->engaged || !choice->booted)) rc = n->commit_err;
    if (rc == RWY_OK) rc = rwy_begin(bst);
    return rc;
}

/* The slot was demoted for a boot loop but the demotion could not be recorded: the loaded fallback is that
 * slot itself. Take the one below it. 0 when there is none that loads. */
static int native_loop_fallback(nsel_t *n) {
    rwy_boot_choice_t fb;
    int rc;

    rwy_set_actor(n->bst, RWY_ACTOR_SYSTEM);
    rc = rwy_bootcfg_fallback(n->bst, &fb, n->choice->loop_slot);
    rwy_set_actor(n->bst, RWY_ACTOR_USER);
    if (rc != RWY_OK || !fb.booted) return 0;
    native_free(&n->fbimg);
    native_load_fallback(n->bst, n->self, n->fat_fs_handle, &fb, &n->fbimg);
    if (!fb.booted) return 0;
    n->fb = fb;
    n->fb_is_pre = 0;
    return 1;
}

/* The commit kept failing: boot the fallback without recording it, or leave to the firmware. */
static EFI_STATUS native_unrecorded(nsel_t *n, int rc) {
    if (n->choice->loop_slot[0] && !native_loop_fallback(n)) {
        /* Nothing else loads: the looping slot still beats a device that stops. */
        if (n->have_pre) {
            n->fb = n->pre;
            n->fb_is_pre = 1;
        } else {
            n->fb.booted = 0;
        }
    }
    if (n->fb_is_pre) {
        *n->choice = n->fb;
    } else if (n->fb.booted) {
        native_free(n->img);
        *n->img = n->fbimg;
        *n->choice = n->fb;
    } else {
        /* Nothing to boot without the store: leave to the firmware. */
        RWY_ERROR("cannot update boot state (%s), not booting\n", rwy_strerror(rc));
        flush_and_stall(3000000);
        return EFI_NOT_FOUND;
    }
    RWY_WARN("cannot update boot state (%s), booting %s without recording it\n", rwy_strerror(rc),
            n->choice->slot);
    *n->unrecorded = 1;
    return EFI_SUCCESS;
}

static void native_label_suffix(const rwy_boot_choice_t *c) {
    if (c->has_label && c->label[0]) {
        uprint(" \"");
        uprint(c->label);
        uprint("\"");
    }
    uprint("\n");
}

/* Load the chosen payload. A slot that cannot be started is marked FAILED and *again asks for the next
 * selection. */
static EFI_STATUS native_start(nsel_t *n, int *again) {
    rwy_store_t *bst = n->bst;
    rwy_boot_choice_t *choice = n->choice;
    EFI_STATUS s;
    int rc;

    *again = 0;
    if (choice->engaged && !choice->booted) {
        RWY_ERROR("no bootable config, no RECOVERY\n");
        flush_and_stall(3000000);
        return EFI_NOT_FOUND;
    }
    if (!choice->engaged) return EFI_SUCCESS;
    /* Use the payload loaded in advance when selection chose that slot; otherwise load it now. */
    if (n->have_pre && !rwy_strcmp(choice->slot, n->pre_slot)) {
        s = EFI_SUCCESS;
    } else {
        native_free(n->img);
        s = native_load(bst, choice->slot, n->self, n->fat_fs_handle, n->img);
    }
    n->have_pre = 0;
    if (!EFI_ERROR(s)) return EFI_SUCCESS;
    /* A slot that cannot be started is FAILED, then the next slot (or RECOVERY) is chosen. */
    RWY_ERROR("%s cannot be started (0x%llx)", choice->slot, (unsigned long long)s);
    if (choice->slot[0] != 'C') {   /* RECOVERY has no lifecycle to condemn */
        (void)rwy_flight_record(bst, NULL, "RECOVERY", RWY_FLIGHT_EVENT_CONDEMN, RWY_SS_REASON_NONE, bst->rec_seq);
        uprint(", not booting");
        native_label_suffix(choice);
        flush_and_stall(3000000);
        return EFI_NOT_FOUND;
    }
    if (rwy_in_txn(bst)) rwy_abort(bst);
    rwy_set_actor(bst, RWY_ACTOR_SYSTEM);
    rc = rwy_bootcfg_condemn(bst, choice->slot);
    rwy_set_actor(bst, RWY_ACTOR_USER);
    if (rc == RWY_OK) rc = rwy_begin(bst);
    if (rc != RWY_OK) {
        uprintf(", cannot mark it FAILED (%s), not booting", rwy_strerror(rc));
        native_label_suffix(choice);
        flush_and_stall(3000000);
        return EFI_NOT_FOUND;
    }
    uprint(", marked FAILED");
    native_label_suffix(choice);
    *again = 1;
    return EFI_SUCCESS;
}

/* Select the slot to boot and load its payload, before any firmware handle is touched.
 * EFI_SUCCESS: img holds the payload, or img->child is NULL and choice->engaged is 0 for a legacy boot;
 * *unrecorded is 1 when the selection could not be committed and the payload is the fallback.
 * EFI_NOT_FOUND: nothing to boot, the message is printed and the caller returns to the firmware. */
EFI_STATUS native_select(rwy_store_t *bst, EFI_HANDLE self, EFI_HANDLE fat_fs_handle, native_img_t *img,
                                rwy_boot_choice_t *choice, int *unrecorded) {
    nsel_t n;
    EFI_STATUS s;
    int rc, again, rolled;

    memset(&n, 0, sizeof(n));
    n.bst = bst; n.self = self; n.fat_fs_handle = fat_fs_handle;
    n.img = img; n.choice = choice; n.unrecorded = unrecorded;
    memset(img, 0, sizeof(*img));
    *unrecorded = 0;

    native_preload(&n, &rolled);
    if (rolled) return native_rolled_back(bst, self, fat_fs_handle, img, choice, unrecorded);
    for (;;) {
        rc = native_select_once(&n);
        if (rc == RWY_ERR_ROLLBACK) return native_rolled_back(bst, self, fat_fs_handle, img, choice, unrecorded);
        if (rc != RWY_OK) {
            n.commit_err = rc;
            if (n.retries++ < COMMIT_RETRIES) {
                RWY_WARN("cannot update boot state (%s), retrying\n", rwy_strerror(rc));
                flush_and_stall(500000);
                continue;
            }
            return native_unrecorded(&n, rc);
        }
        s = native_start(&n, &again);
        if (EFI_ERROR(s)) return s;
        if (!again) break;
    }
    native_free(&n.fbimg);
    return EFI_SUCCESS;
}
