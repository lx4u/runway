/* tools/anchor_linux.h: the anti-rollback anchor on a Linux TPM, shared by rwyctl and rwy-fuse. */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ANCHOR_LINUX_H
#define ANCHOR_LINUX_H

#include "rwy_core.h"
#include "tpm_linux.h"

#define ANCHOR_PCR 7

/* The context of the counter callbacks: the TPM and the NV index. */
typedef struct {
    rwy_tpm_xport_t x;
    uint32_t index;
} anchor_tpm_t;

int anchor_tpm_read(void *ctx, uint64_t *v);
int anchor_tpm_increment(void *ctx);
/* An increment that does nothing, for a look at the counter. */
int anchor_tpm_peek(void *ctx);
/* The word for a RWY_ANCHOR_* result. */
const char *anchor_word(int r);
/* The state of the counter, without touching it. A store that is not enrolled never opens the TPM. */
const char *anchor_state(rwy_store_t *s);
/* Check the counter of an enrolled store and attach it, so commits raise the epoch. Returns 0 when the store is
 * not enrolled or the counter is trusted, -1 when the TPM cannot be opened, else the RWY_ANCHOR_* result.
 * Nothing is attached on a failure. The TPM stays open until anchor_release() or the end of the process. */
int anchor_gate(rwy_store_t *s);
/* Return the active anchor context if anchor_gate() has the TPM open, else NULL. */
anchor_tpm_t *anchor_active(void);
/* Close the TPM that anchor_gate() keeps open. Does nothing when it is not open. The caller must detach the
 * anchor from its store first. anchor_gate() may be called again. */
void anchor_release(void);

#endif
