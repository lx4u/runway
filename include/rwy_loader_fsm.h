/*
 * rwy_loader_fsm.h — Loader state record and stub state machine.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RWY_LOADER_FSM_H
#define RWY_LOADER_FSM_H

#include "rwy_core.h"
#include "rwy_bundle.h"
#include "rwy_format.h"

/* Verify state record magic, version, and crc32c. */
int rwy_loader_record_check(const rwy_loader_record_t *rec);

/* Pick newest valid state record copy (0 or 1). Returns RWY_OK or error. */
int rwy_loader_record_pick(const rwy_block_dev_t *dev, uint32_t loader_start,
                           rwy_loader_record_t *rec, int *active_idx);

/* Write state record to the inactive copy and flush. Updates *active_idx. */
int rwy_loader_record_write(const rwy_block_dev_t *dev, uint32_t loader_start,
                            int active_idx, rwy_loader_record_t *rec,
                            int *new_active_idx);

/* Text representation of a loader failure reason. */
const char *rwy_loader_reason_str(uint32_t reason);

/* Actions returned by rwy_loader_fsm_next. */
#define RWY_LOADER_FSM_ACTION_BOOT  1
#define RWY_LOADER_FSM_ACTION_STOP  2

/* Child boot plan produced by the FSM. */
typedef struct {
    unsigned               copy;          /* 0 = A, 1 = B */
    int                    is_trial;      /* 1 for trial boot, 0 for normal */
    uint32_t               timeout;       /* watchdog timeout in seconds */
    uint32_t               bundle_base;   /* starting block on dev */
    uint32_t               bundle_blocks; /* blocks per copy */
    rwy_loader_handshake_t handshake;     /* handshake variable data */
    rwy_bundle_header_t    header;        /* verified bundle header */
} rwy_loader_fsm_plan_t;

/* Stub state machine context. */
typedef struct {
    const rwy_block_dev_t *dev;
    uint32_t               loader_start;
    uint32_t               loader_blocks;
    int                    record_idx;
    rwy_loader_record_t    rec;
    int                    current_target;
    int                    current_is_trial;
    int                    state;
    int                    final_status;
} rwy_loader_fsm_t;

/* Initialize state machine. */
int rwy_loader_fsm_init(rwy_loader_fsm_t *fsm, const rwy_block_dev_t *dev,
                        uint32_t loader_start, uint32_t loader_blocks);

/* Compute next action (boot child or stop). */
int rwy_loader_fsm_next(rwy_loader_fsm_t *fsm, rwy_loader_fsm_plan_t *plan);

/* Handle child return status. */
void rwy_loader_fsm_on_child_return(rwy_loader_fsm_t *fsm, int child_rc);

/* Child launcher callback signature. */
typedef int (*rwy_loader_child_fn)(void *ctx, const rwy_loader_fsm_plan_t *plan);

/* Run state machine to completion using a child launcher callback. */
int rwy_loader_fsm_run(rwy_loader_fsm_t *fsm, rwy_loader_child_fn child_fn, void *ctx);

#endif /* RWY_LOADER_FSM_H */
