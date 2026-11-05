/*
 * rwy_loader_fsm.c — Loader state record and stub state machine.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rwy_loader_fsm.h"
#include "rwy_internal.h"
#include "rwy_string.h"

enum {
    FSM_STATE_INIT = 0,
    FSM_STATE_VERIFY,
    FSM_STATE_WAIT_CHILD,
    FSM_STATE_DONE
};

const char *rwy_loader_reason_str(uint32_t reason) {
    switch (reason) {
    case RWY_LOADER_REASON_NONE:
        return "none";
    case RWY_LOADER_REASON_BAD_HEADER:
        return "bad header";
    case RWY_LOADER_REASON_BAD_RANGE:
        return "bad range";
    case RWY_LOADER_REASON_BAD_HASH:
        return "bad hash";
    case RWY_LOADER_REASON_START_FAILED:
        return "start failed";
    case RWY_LOADER_REASON_TRIES_EXHAUSTED:
        return "tries exhausted";
    case RWY_LOADER_REASON_RECORD_IO:
        return "record io";
    default:
        return "unknown";
    }
}

int rwy_loader_record_check(const rwy_loader_record_t *rec) {
    if (!rec) return RWY_ERR_INVAL;
    if (rec->magic != RWY_BLK_MAGIC || rec->version != RWY_LOADER_RECORD_VERSION)
        return RWY_ERR_CORRUPT;
    if (rec->good > 1 || rec->trial > 2)
        return RWY_ERR_CORRUPT;
    if (rwy_crc32c(rec, offsetof(rwy_loader_record_t, crc32c)) != rec->crc32c)
        return RWY_ERR_CORRUPT;
    return RWY_OK;
}

typedef struct {
    const rwy_block_dev_t *dev;
    uint32_t loader_start;
    rwy_loader_record_t rec[2];
} record_pick_ctx_t;

static int loader_record_copy_check(void *ctx, unsigned c, uint64_t *seq) {
    record_pick_ctx_t *sc = (record_pick_ctx_t *)ctx;
    uint64_t off = (uint64_t)(sc->loader_start + c) * RWY_BLOCK_SIZE;
    int rc = sc->dev->read(sc->dev->ctx, off, &sc->rec[c], sizeof(rwy_loader_record_t));
    if (rc) return RWY_ERR_IO;
    rc = rwy_loader_record_check(&sc->rec[c]);
    if (rc) return rc;
    *seq = sc->rec[c].sequence;
    return RWY_OK;
}

int rwy_loader_record_pick(const rwy_block_dev_t *dev, uint32_t loader_start,
                           rwy_loader_record_t *rec, int *active_idx) {
    record_pick_ctx_t sc;
    unsigned best = 0;
    int rc;

    if (!dev || !rec) return RWY_ERR_INVAL;
    sc.dev = dev;
    sc.loader_start = loader_start;
    rc = rwy_pick_newest(2, loader_record_copy_check, &sc, &best);
    if (rc) {
        if (active_idx) *active_idx = -1;
        return rc;
    }
    *rec = sc.rec[best];
    if (active_idx) *active_idx = (int)best;
    return RWY_OK;
}

int rwy_loader_record_write(const rwy_block_dev_t *dev, uint32_t loader_start,
                            int active_idx, rwy_loader_record_t *rec,
                            int *new_active_idx) {
    int target_copy;
    uint64_t off;
    int rc;

    if (!dev || !rec) return RWY_ERR_INVAL;
    target_copy = (active_idx == 0) ? 1 : 0;
    rec->magic = RWY_BLK_MAGIC;
    rec->version = RWY_LOADER_RECORD_VERSION;
    rec->sequence = (rec->sequence == 0) ? 1 : (rec->sequence + 1);
    memset(rec->reserved, 0, sizeof(rec->reserved));
    rec->crc32c = rwy_crc32c(rec, offsetof(rwy_loader_record_t, crc32c));

    off = (uint64_t)(loader_start + (uint32_t)target_copy) * RWY_BLOCK_SIZE;
    rc = dev->write(dev->ctx, off, rec, sizeof(*rec));
    if (rc) return RWY_ERR_IO;
    rc = rwy_dev_flush(dev);
    if (rc) return RWY_ERR_IO;

    if (new_active_idx) *new_active_idx = target_copy;
    return RWY_OK;
}

int rwy_loader_fsm_init(rwy_loader_fsm_t *fsm, const rwy_block_dev_t *dev,
                        uint32_t loader_start, uint32_t loader_blocks) {
    if (!fsm || !dev || loader_blocks == 0) return RWY_ERR_INVAL;
    memset(fsm, 0, sizeof(*fsm));
    fsm->dev = dev;
    fsm->loader_start = loader_start;
    fsm->loader_blocks = loader_blocks;
    fsm->record_idx = -1;
    fsm->current_target = -1;
    fsm->current_is_trial = 0;
    fsm->state = FSM_STATE_INIT;
    fsm->final_status = RWY_OK;
    return RWY_OK;
}

static uint32_t bundle_error_to_reason(int rc) {
    switch (rc) {
    case RWY_BUNDLE_BAD_RANGE:
        return RWY_LOADER_REASON_BAD_RANGE;
    case RWY_BUNDLE_BAD_HASH:
        return RWY_LOADER_REASON_BAD_HASH;
    case RWY_BUNDLE_IO:
        return RWY_LOADER_REASON_RECORD_IO;
    case RWY_BUNDLE_BAD_HEADER:
    default:
        return RWY_LOADER_REASON_BAD_HEADER;
    }
}

static void arbitrate_trial(rwy_loader_fsm_t *fsm) {
    rwy_loader_record_t *rec = &fsm->rec;
    int rc;

    if (rec->trial != RWY_LOADER_TRIAL_NONE && rec->tries == 0) {
        unsigned trial_copy = rec->trial - 1;
        rec->bad |= (uint8_t)(1U << trial_copy);
        rec->reason = RWY_LOADER_REASON_TRIES_EXHAUSTED;
        rec->trial = RWY_LOADER_TRIAL_NONE;
        rwy_loader_record_write(fsm->dev, fsm->loader_start, fsm->record_idx,
                                rec, &fsm->record_idx);
        fsm->current_target = rec->good;
        fsm->current_is_trial = 0;
        return;
    }

    if (rec->trial != RWY_LOADER_TRIAL_NONE && rec->tries > 0) {
        rec->tries -= 1;
        rc = rwy_loader_record_write(fsm->dev, fsm->loader_start, fsm->record_idx,
                                     rec, &fsm->record_idx);
        if (rc) {
            rec->reason = RWY_LOADER_REASON_RECORD_IO;
            fsm->current_target = rec->good;
            fsm->current_is_trial = 0;
            return;
        }
        fsm->current_target = rec->trial - 1;
        fsm->current_is_trial = 1;
        return;
    }

    fsm->current_target = rec->good;
    fsm->current_is_trial = 0;
}

static int verify_and_select(rwy_loader_fsm_t *fsm, rwy_bundle_header_t *hdr) {
    while (1) {
        uint32_t base;
        int rc, alt;

        if (fsm->rec.bad & (1U << fsm->current_target)) {
            if (fsm->current_is_trial) {
                fsm->current_target = fsm->rec.good;
                fsm->current_is_trial = 0;
                continue;
            }
            alt = 1 - fsm->current_target;
            if ((fsm->rec.bad & (1U << alt)) == 0) {
                fsm->current_target = alt;
                fsm->current_is_trial = 0;
                continue;
            }
            return RWY_ERR_CORRUPT;
        }

        base = fsm->loader_start + 2 + (uint32_t)fsm->current_target * fsm->loader_blocks;
        rc = rwy_bundle_check_copy(fsm->dev, base, fsm->loader_blocks,
                                   &rwy_loader_bundle_spec, hdr);
        if (rc == RWY_BUNDLE_OK) return RWY_OK;

        fsm->rec.bad |= (uint8_t)(1U << fsm->current_target);
        fsm->rec.reason = bundle_error_to_reason(rc);

        if (fsm->current_is_trial) {
            fsm->rec.trial = RWY_LOADER_TRIAL_NONE;
            rwy_loader_record_write(fsm->dev, fsm->loader_start, fsm->record_idx,
                                    &fsm->rec, &fsm->record_idx);
            fsm->current_target = fsm->rec.good;
            fsm->current_is_trial = 0;
            continue;
        }

        rwy_loader_record_write(fsm->dev, fsm->loader_start, fsm->record_idx,
                                &fsm->rec, &fsm->record_idx);
        alt = 1 - fsm->current_target;
        if ((fsm->rec.bad & (1U << alt)) == 0) {
            fsm->current_target = alt;
            fsm->current_is_trial = 0;
            continue;
        }
        return RWY_ERR_CORRUPT;
    }
}

int rwy_loader_fsm_next(rwy_loader_fsm_t *fsm, rwy_loader_fsm_plan_t *plan) {
    rwy_bundle_header_t hdr;
    int rc;

    if (!fsm || !plan) return RWY_LOADER_FSM_ACTION_STOP;

    if (fsm->state == FSM_STATE_INIT) {
        rc = rwy_loader_record_pick(fsm->dev, fsm->loader_start, &fsm->rec, &fsm->record_idx);
        if (rc) {
            fsm->final_status = rc;
            fsm->state = FSM_STATE_DONE;
            return RWY_LOADER_FSM_ACTION_STOP;
        }
        arbitrate_trial(fsm);
        fsm->state = FSM_STATE_VERIFY;
    }

    if (fsm->state == FSM_STATE_VERIFY) {
        rc = verify_and_select(fsm, &hdr);
        if (rc) {
            fsm->final_status = rc;
            fsm->state = FSM_STATE_DONE;
            return RWY_LOADER_FSM_ACTION_STOP;
        }

        plan->copy = (unsigned)fsm->current_target;
        plan->is_trial = fsm->current_is_trial;
        plan->timeout = fsm->rec.timeout;
        plan->bundle_base = fsm->loader_start + 2 + (uint32_t)fsm->current_target * fsm->loader_blocks;
        plan->bundle_blocks = fsm->loader_blocks;
        plan->header = hdr;
        plan->handshake.copy = (uint8_t)fsm->current_target;
        plan->handshake.trial = (uint8_t)fsm->current_is_trial;
        plan->handshake.reserved = 0;
        plan->handshake.timeout = fsm->current_is_trial ? fsm->rec.timeout : 0;
        memset(plan->handshake.wdt_armed, 0, sizeof(plan->handshake.wdt_armed));

        fsm->state = FSM_STATE_WAIT_CHILD;
        return RWY_LOADER_FSM_ACTION_BOOT;
    }

    return RWY_LOADER_FSM_ACTION_STOP;
}

void rwy_loader_fsm_on_child_return(rwy_loader_fsm_t *fsm, int child_rc) {
    if (!fsm || fsm->state != FSM_STATE_WAIT_CHILD) return;

    if (fsm->current_is_trial) {
        fsm->rec.bad |= (uint8_t)(1U << fsm->current_target);
        fsm->rec.reason = RWY_LOADER_REASON_START_FAILED;
        fsm->rec.trial = RWY_LOADER_TRIAL_NONE;
        rwy_loader_record_write(fsm->dev, fsm->loader_start, fsm->record_idx,
                                &fsm->rec, &fsm->record_idx);
        fsm->current_target = fsm->rec.good;
        fsm->current_is_trial = 0;
        fsm->state = FSM_STATE_VERIFY;
        return;
    }

    fsm->final_status = child_rc;
    fsm->state = FSM_STATE_DONE;
}

int rwy_loader_fsm_run(rwy_loader_fsm_t *fsm, rwy_loader_child_fn child_fn, void *ctx) {
    rwy_loader_fsm_plan_t plan;
    int action;

    if (!fsm) return RWY_ERR_INVAL;
    while ((action = rwy_loader_fsm_next(fsm, &plan)) == RWY_LOADER_FSM_ACTION_BOOT) {
        int child_rc = child_fn ? child_fn(ctx, &plan) : 0;
        rwy_loader_fsm_on_child_return(fsm, child_rc);
    }
    return fsm->final_status;
}
