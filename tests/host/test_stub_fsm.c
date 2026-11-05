/*
 * test_stub_fsm.c — Pure stub state machine unit tests.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rwy_crypto.h"
#include "rwy_bundle.h"
#include "rwy_format.h"
#include "rwy_loader_fsm.h"

#define BUNDLE_BLOCKS 4U
#define TOTAL_BLOCKS  (2U + 2U * BUNDLE_BLOCKS)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

typedef struct {
    uint8_t mem[TOTAL_BLOCKS * RWY_BLOCK_SIZE];
    int fail_flush;
} mem_t;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_t *m = ctx;
    if (off + len > sizeof(m->mem)) return -1;
    memcpy(buf, m->mem + off, len);
    return 0;
}

static int mem_write(void *ctx, uint64_t off, const void *buf, size_t len) {
    mem_t *m = ctx;
    if (off + len > sizeof(m->mem)) return -1;
    memcpy(m->mem + off, buf, len);
    return 0;
}

static int mem_flush(void *ctx) {
    mem_t *m = ctx;
    if (m->fail_flush) return -1;
    return 0;
}

static void mem_init(mem_t *m, rwy_block_dev_t *d) {
    memset(m, 0, sizeof(*m));
    d->read = mem_read;
    d->write = mem_write;
    d->flush = mem_flush;
    d->ctx = m;
    d->size = sizeof(m->mem);
    d->block_size = RWY_BLOCK_SIZE;
    d->stats = NULL;
    d->dircache = NULL;
}

static void write_dummy_bundle(const rwy_block_dev_t *dev, uint8_t seed) {
    uint8_t payload[2048];
    rwy_bundle_payload_t parts[1];

    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(seed + i);
    parts[0].kind = RWY_ENTRY_LOADER;
    parts[0].data = payload;
    parts[0].len = sizeof(payload);
    rwy_bundle_write(dev, 2, BUNDLE_BLOCKS, &rwy_loader_bundle_spec, parts, 1);
}

static void setup_test_area(mem_t *mem, rwy_block_dev_t *dev,
                            uint8_t good, uint8_t trial, uint8_t tries) {
    rwy_loader_record_t rec;
    int cur = -1;

    mem_init(mem, dev);

    /* Write copy A at block 2, copy B at block 6 */
    write_dummy_bundle(dev, 0x11);
    write_dummy_bundle(dev, 0x22);

    memset(&rec, 0, sizeof(rec));
    rec.good = good;
    rec.trial = trial;
    rec.tries = tries;
    rec.timeout = RWY_LOADER_TIMEOUT_DEFAULT;
    rwy_loader_record_write(dev, 0, cur, &rec, &cur);
}

static void test_normal_boot(void) {
    mem_t mem;
    rwy_block_dev_t dev;
    rwy_loader_fsm_t fsm;
    rwy_loader_fsm_plan_t plan;
    rwy_loader_record_t disk_rec;
    int action, rc;

    printf("== stub fsm: normal boot ==\n");
    setup_test_area(&mem, &dev, 0, RWY_LOADER_TRIAL_NONE, 0);

    rc = rwy_loader_fsm_init(&fsm, &dev, 0, BUNDLE_BLOCKS);
    CHECK(rc == RWY_OK, "fsm init");

    action = rwy_loader_fsm_next(&fsm, &plan);
    CHECK(action == RWY_LOADER_FSM_ACTION_BOOT, "action boot");
    CHECK(plan.copy == 0, "copy 0 (A) selected");
    CHECK(plan.is_trial == 0, "not trial");
    CHECK(plan.bundle_base == 2, "bundle base 2");

    rwy_loader_fsm_on_child_return(&fsm, 0);
    CHECK(fsm.final_status == 0, "final status 0");

    action = rwy_loader_fsm_next(&fsm, &plan);
    CHECK(action == RWY_LOADER_FSM_ACTION_STOP, "action stop");

    /* Record untouched */
    rwy_loader_record_pick(&dev, 0, &disk_rec, NULL);
    CHECK(disk_rec.sequence == 1, "record sequence untouched");
    CHECK(disk_rec.bad == 0, "bad mask untouched");
}

static void test_trial_decrement(void) {
    mem_t mem;
    rwy_block_dev_t dev;
    rwy_loader_fsm_t fsm;
    rwy_loader_fsm_plan_t plan;
    rwy_loader_record_t disk_rec;
    int action, rc;

    printf("== stub fsm: trial try decrement ==\n");
    setup_test_area(&mem, &dev, 0, RWY_LOADER_TRIAL_B, 3);

    rc = rwy_loader_fsm_init(&fsm, &dev, 0, BUNDLE_BLOCKS);
    CHECK(rc == RWY_OK, "fsm init");

    action = rwy_loader_fsm_next(&fsm, &plan);
    CHECK(action == RWY_LOADER_FSM_ACTION_BOOT, "action boot");
    CHECK(plan.copy == 1, "trial copy 1 (B) selected");
    CHECK(plan.is_trial == 1, "is trial");
    CHECK(plan.bundle_base == 6, "bundle base 6");
    CHECK(plan.handshake.trial == 1, "handshake trial 1");
    CHECK(plan.handshake.timeout == RWY_LOADER_TIMEOUT_DEFAULT, "handshake timeout");

    /* Check tries was decremented on disk */
    rwy_loader_record_pick(&dev, 0, &disk_rec, NULL);
    CHECK(disk_rec.sequence == 2, "record sequence incremented");
    CHECK(disk_rec.tries == 2, "tries decremented to 2");
    CHECK(disk_rec.trial == RWY_LOADER_TRIAL_B, "trial still B");
}

static void test_try_exhaustion(void) {
    mem_t mem;
    rwy_block_dev_t dev;
    rwy_loader_fsm_t fsm;
    rwy_loader_fsm_plan_t plan;
    rwy_loader_record_t disk_rec;
    int action, rc;

    printf("== stub fsm: trial try exhaustion ==\n");
    setup_test_area(&mem, &dev, 0, RWY_LOADER_TRIAL_B, 0);

    rc = rwy_loader_fsm_init(&fsm, &dev, 0, BUNDLE_BLOCKS);
    CHECK(rc == RWY_OK, "fsm init");

    action = rwy_loader_fsm_next(&fsm, &plan);
    CHECK(action == RWY_LOADER_FSM_ACTION_BOOT, "action boot");
    CHECK(plan.copy == 0, "fallback to good copy 0 (A)");
    CHECK(plan.is_trial == 0, "not trial");

    /* Check disk record marked copy B bad and cleared trial */
    rwy_loader_record_pick(&dev, 0, &disk_rec, NULL);
    CHECK(disk_rec.bad & RWY_LOADER_BAD_B, "copy B marked bad");
    CHECK(disk_rec.trial == RWY_LOADER_TRIAL_NONE, "trial cleared");
    CHECK(disk_rec.reason == RWY_LOADER_REASON_TRIES_EXHAUSTED, "reason tries exhausted");
}

static void test_verify_fallback(void) {
    mem_t mem;
    rwy_block_dev_t dev;
    rwy_loader_fsm_t fsm;
    rwy_loader_fsm_plan_t plan;
    rwy_loader_record_t disk_rec;
    int action, rc;

    printf("== stub fsm: fallback on verify failure ==\n");
    setup_test_area(&mem, &dev, 0, RWY_LOADER_TRIAL_B, 3);

    /* Corrupt bundle header magic of copy B (block 6) */
    mem.mem[6 * RWY_BLOCK_SIZE] = 0;

    rc = rwy_loader_fsm_init(&fsm, &dev, 0, BUNDLE_BLOCKS);
    CHECK(rc == RWY_OK, "fsm init");

    action = rwy_loader_fsm_next(&fsm, &plan);
    CHECK(action == RWY_LOADER_FSM_ACTION_BOOT, "action boot");
    CHECK(plan.copy == 0, "fell back to good copy 0 in same session");
    CHECK(plan.is_trial == 0, "not trial");

    /* Check disk record updated */
    rwy_loader_record_pick(&dev, 0, &disk_rec, NULL);
    CHECK(disk_rec.bad & RWY_LOADER_BAD_B, "copy B marked bad");
    CHECK(disk_rec.trial == RWY_LOADER_TRIAL_NONE, "trial cleared");
    CHECK(disk_rec.reason == RWY_LOADER_REASON_BAD_HEADER, "reason bad header");
}

static void test_child_return_fallback(void) {
    mem_t mem;
    rwy_block_dev_t dev;
    rwy_loader_fsm_t fsm;
    rwy_loader_fsm_plan_t plan;
    rwy_loader_record_t disk_rec;
    int action, rc;

    printf("== stub fsm: fallback on child return ==\n");
    setup_test_area(&mem, &dev, 0, RWY_LOADER_TRIAL_B, 3);

    rc = rwy_loader_fsm_init(&fsm, &dev, 0, BUNDLE_BLOCKS);
    CHECK(rc == RWY_OK, "fsm init");

    action = rwy_loader_fsm_next(&fsm, &plan);
    CHECK(action == RWY_LOADER_FSM_ACTION_BOOT, "action boot");
    CHECK(plan.copy == 1, "trial copy 1 launched");
    CHECK(plan.is_trial == 1, "is trial");

    /* Child returns error */
    rwy_loader_fsm_on_child_return(&fsm, -1);

    /* Check disk record was updated */
    rwy_loader_record_pick(&dev, 0, &disk_rec, NULL);
    CHECK(disk_rec.bad & RWY_LOADER_BAD_B, "trial copy B marked bad");
    CHECK(disk_rec.trial == RWY_LOADER_TRIAL_NONE, "trial cleared");
    CHECK(disk_rec.reason == RWY_LOADER_REASON_START_FAILED, "reason start failed");

    /* FSM immediately executes good copy in same session */
    action = rwy_loader_fsm_next(&fsm, &plan);
    CHECK(action == RWY_LOADER_FSM_ACTION_BOOT, "action boot");
    CHECK(plan.copy == 0, "good copy 0 launched");
    CHECK(plan.is_trial == 0, "is not trial");

    /* Good child returns success */
    rwy_loader_fsm_on_child_return(&fsm, 0);
    CHECK(fsm.final_status == 0, "final status 0");

    action = rwy_loader_fsm_next(&fsm, &plan);
    CHECK(action == RWY_LOADER_FSM_ACTION_STOP, "action stop");
}

static void test_preserve_good_error(void) {
    mem_t mem;
    rwy_block_dev_t dev;
    rwy_loader_fsm_t fsm;
    rwy_loader_fsm_plan_t plan;
    rwy_loader_record_t disk_rec;
    int action, rc;

    printf("== stub fsm: preservation of good loader error return ==\n");
    setup_test_area(&mem, &dev, 0, RWY_LOADER_TRIAL_NONE, 0);

    rc = rwy_loader_fsm_init(&fsm, &dev, 0, BUNDLE_BLOCKS);
    CHECK(rc == RWY_OK, "fsm init");

    action = rwy_loader_fsm_next(&fsm, &plan);
    CHECK(action == RWY_LOADER_FSM_ACTION_BOOT, "action boot");
    CHECK(plan.copy == 0, "copy 0 launched");

    /* Good child returns error */
    rwy_loader_fsm_on_child_return(&fsm, -42);
    CHECK(fsm.final_status == -42, "final status preserves error code");

    action = rwy_loader_fsm_next(&fsm, &plan);
    CHECK(action == RWY_LOADER_FSM_ACTION_STOP, "action stop");

    /* Record untouched */
    rwy_loader_record_pick(&dev, 0, &disk_rec, NULL);
    CHECK(disk_rec.sequence == 1, "record sequence untouched");
    CHECK(disk_rec.bad == 0, "bad mask untouched");
    CHECK(disk_rec.reason == RWY_LOADER_REASON_NONE, "reason untouched");
}

static void test_good_verify_fallback(void) {
    mem_t mem;
    rwy_block_dev_t dev;
    rwy_loader_fsm_t fsm;
    rwy_loader_fsm_plan_t plan;
    int action, rc;

    printf("== stub fsm: good copy verify fallback to other copy ==\n");
    setup_test_area(&mem, &dev, 0, RWY_LOADER_TRIAL_NONE, 0);

    /* Corrupt copy A */
    mem.mem[2 * RWY_BLOCK_SIZE] = 0;

    rc = rwy_loader_fsm_init(&fsm, &dev, 0, BUNDLE_BLOCKS);
    CHECK(rc == RWY_OK, "fsm init");

    action = rwy_loader_fsm_next(&fsm, &plan);
    CHECK(action == RWY_LOADER_FSM_ACTION_BOOT, "action boot");
    CHECK(plan.copy == 1, "other copy 1 (B) selected");
}

static void test_both_copies_invalid(void) {
    mem_t mem;
    rwy_block_dev_t dev;
    rwy_loader_fsm_t fsm;
    rwy_loader_fsm_plan_t plan;
    int action, rc;

    printf("== stub fsm: both copies invalid ==\n");
    setup_test_area(&mem, &dev, 0, RWY_LOADER_TRIAL_NONE, 0);

    /* Corrupt both copies */
    mem.mem[2 * RWY_BLOCK_SIZE] = 0;
    mem.mem[6 * RWY_BLOCK_SIZE] = 0;

    rc = rwy_loader_fsm_init(&fsm, &dev, 0, BUNDLE_BLOCKS);
    CHECK(rc == RWY_OK, "fsm init");

    action = rwy_loader_fsm_next(&fsm, &plan);
    CHECK(action == RWY_LOADER_FSM_ACTION_STOP, "action stop on both invalid");
    CHECK(fsm.final_status == RWY_ERR_CORRUPT, "status corrupt");
}

static void test_size_guard_io_failure(void) {
    mem_t mem;
    rwy_block_dev_t dev;
    rwy_loader_fsm_t fsm;
    rwy_loader_fsm_plan_t plan;
    int action, rc;

    printf("== stub fsm: record write IO / size guard failure ==\n");
    setup_test_area(&mem, &dev, 0, RWY_LOADER_TRIAL_B, 3);

    /* Flush fails during try decrement write */
    mem.fail_flush = 1;

    rc = rwy_loader_fsm_init(&fsm, &dev, 0, BUNDLE_BLOCKS);
    CHECK(rc == RWY_OK, "fsm init");

    action = rwy_loader_fsm_next(&fsm, &plan);
    CHECK(action == RWY_LOADER_FSM_ACTION_BOOT, "action boot");
    CHECK(plan.copy == 0, "aborted trial, fell back to good copy 0");
    CHECK(plan.is_trial == 0, "not trial");
}

static int mock_child_runner(void *ctx, const rwy_loader_fsm_plan_t *plan) {
    int *count = ctx;
    (*count)++;
    if (plan->is_trial) return -1;
    return 0;
}

static void test_fsm_run_driver(void) {
    mem_t mem;
    rwy_block_dev_t dev;
    rwy_loader_fsm_t fsm;
    int exec_count = 0, status, rc;

    printf("== stub fsm: run driver helper ==\n");
    setup_test_area(&mem, &dev, 0, RWY_LOADER_TRIAL_B, 3);

    rc = rwy_loader_fsm_init(&fsm, &dev, 0, BUNDLE_BLOCKS);
    CHECK(rc == RWY_OK, "fsm init");

    status = rwy_loader_fsm_run(&fsm, mock_child_runner, &exec_count);
    CHECK(status == 0, "fsm run completed with status 0");
    CHECK(exec_count == 2, "trial executed then fell back to good");
}

int main(void) {
    test_normal_boot();
    test_trial_decrement();
    test_try_exhaustion();
    test_verify_fallback();
    test_child_return_fallback();
    test_preserve_good_error();
    test_good_verify_fallback();
    test_both_copies_invalid();
    test_size_guard_io_failure();
    test_fsm_run_driver();

    if (failures) {
        printf("%d tests failed\n", failures);
        return 1;
    }
    printf("all stub fsm tests passed\n");
    return 0;
}
