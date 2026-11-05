/*
 * rwy_efianchor.c — Anti-rollback counter of the boot store: the TPM NV index under PCR 7.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <efi.h>
#include <efilib.h>
#include <stddef.h>
#include <string.h>
#include "efi_util.h"
#include "rwy_boot.h"
#include "rwy_tpm.h"
#include "../include/efi_tcg2.h"

#define ANCHOR_PCR 7

typedef struct { EFI_TCG2_PROTOCOL *tcg2; UINT32 index; } efi_anchor_t;
static efi_anchor_t gAnchor;

static int anchor_read(void *ctx, uint64_t *v) {
    efi_anchor_t *a = ctx;
    EFI_STATUS s = a->tcg2 ? rwy_tpm_nv_read(a->tcg2, a->index, ANCHOR_PCR, v) : EFI_UNSUPPORTED;
    return s == EFI_NOT_READY ? RWY_ANCHOR_READ_UNSET : s == EFI_NOT_FOUND ? RWY_ANCHOR_READ_ABSENT :
           s == EFI_SECURITY_VIOLATION ? RWY_ANCHOR_READ_DENIED : EFI_ERROR(s) ? RWY_ERR_IO : RWY_OK;
}

static int anchor_increment(void *ctx) {
    efi_anchor_t *a = ctx;
    return a->tcg2 && !EFI_ERROR(rwy_tpm_nv_increment(a->tcg2, a->index, ANCHOR_PCR)) ? RWY_OK : RWY_ERR_IO;
}

static const rwy_anchor_ops_t gAnchorOps = { anchor_read, anchor_increment, &gAnchor };

/* An enrolled store is checked against its counter from here on; a counter set up offline gets its first increment.
 * An encrypted store that is not enrolled is looked up too: its counter may exist (a copy from before the enrollment). */
void anchor_attach(rwy_store_t *bst) {
    uint64_t epoch, after;
    int enrolled;
    rwy_anchor_get(bst, &epoch, &enrolled);
    if (!enrolled && bst->sb.crypt_algo != RWY_CRYPT_AES_XTS_256) return;
    if (EFI_ERROR(rwy_tpm_locate(&gAnchor.tcg2))) gAnchor.tcg2 = NULL;
    gAnchor.index = rwy_anchor_index(bst);
    rwy_anchor_attach(bst, &gAnchorOps);
    (void)rwy_anchor_adopt(bst, &gAnchorOps);
    rwy_anchor_get(bst, &after, &enrolled);
    if (after != epoch) RWY_DEBUG("anchor enrolled (epoch %llu)\n", (unsigned long long)after);
}

/* Print why the store is taken for rolled back. */
void anchor_report_rollback(rwy_store_t *bst) {
    uint64_t epoch, counter = 0;
    int enrolled;
    rwy_anchor_get(bst, &epoch, &enrolled);
    if (enrolled && anchor_read(&gAnchor, &counter) != RWY_OK)
        RWY_ERROR("store rolled back (epoch %llu, counter unreadable), not booting slots\n", (unsigned long long)epoch);
    else if (enrolled && counter != epoch)
        RWY_ERROR("store rolled back (epoch %llu, counter %llu), not booting slots\n", (unsigned long long)epoch,
                (unsigned long long)counter);
    else if (!enrolled && rwy_anchor_check(bst, &gAnchorOps) == RWY_ANCHOR_STRAY)
        RWY_ERROR("store rolled back (the record says not enrolled, but the counter exists), not booting slots\n");
    else
        RWY_ERROR("store rolled back (the tree is older than the record), not booting slots\n");
}
