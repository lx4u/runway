/* bootcfgtest: boot configuration schema, selection, recovery and checker tests. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "bootcfgtest.h"

static void run(const char *base) {
    file_t fl;
    rwy_block_dev_t dev;
    const char *img = tpath("rwy_test_bootcfg.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_inactive();
    test_subtree_create();
    test_names_types();
    test_mode_owner();
    test_scalars();
    test_finalized();
    test_links();
    test_class_cache();
    test_content_names();
    test_booted_and_root();
    test_recovery();
    test_wdt();
    test_wdt_hint();
    test_rename_rmdir();
    test_entry_helpers();
    test_meta_helpers();
    test_transplog_payload_digest();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes");
    test_fault_injection(base);
    test_sweep(base);
    test_blob_check(base);

    img = tpath("rwy_test_bootcfg_fin.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_finalize();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after finalize tests");
    fi_finalize(base, "counter present", 1);
    fi_finalize(base, "counter missing", 0);
    fi_bridge(base, 0);
    fi_bridge(base, 1);

    img = tpath("rwy_test_bootcfg_sel.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_select();
    fclose(fl.f);
    /* Selection tolerates what the checker reports: the deliberately broken slots of the table. */
    CHECK(run_checker_args("", img, "") == 4 && chk_has("CONFIG6: KERNEL missing") && chk_has("CONFIG7: KERNEL invalid") &&
          chk_has("CONFIG9: REVISION invalid"), "checker reports the broken slots strictly");
    img = tpath("rwy_test_bootcfg_sel2.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_select_watchdog_version();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after WATCHDOG tests");

    img = tpath("rwy_test_bootcfg_idx.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_index();
    fclose(fl.f);
    CHECK(run_checker_args("", img, "") == 0, "checker ignores INDEX");

    img = tpath("rwy_test_bootcfg_reads.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_dir_reads();
    fclose(fl.f);
    CHECK(run_checker_args("", img, "") == 0, "checker passes after the directory read tests");

    img = tpath("rwy_test_bootcfg_lnk.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_link_corners();
    fclose(fl.f);
    CHECK(run_checker_args("", img, "") == 4 && chk_has("KERNEL missing"), "checker reports the slot whose KERNEL was removed");

    img = tpath("rwy_test_bootcfg_nam.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_slot_names();
    fclose(fl.f);
    CHECK(run_checker_args("", img, "") == 0, "checker passes with corner slot names");

    img = tpath("rwy_test_bootcfg_lim.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_slot_limit();
    fclose(fl.f);
    CHECK(run_checker_args("", img, "") == 4 && chk_has("66 slots, more than 64"), "checker flags more than 64 slots");
    CHECK(run_checker_args("--repair", img, "") == 4 && chk_has("repair: REFUSED"), "repair refuses it");

    img = tpath("rwy_test_bootcfg_slots.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_slots_scalar();
    fclose(fl.f);
    CHECK(run_checker_args("", img, "") == 4 && chk_has("SLOTS: not a number from 1 to 64"), "checker flags a bad SLOTS");

    img = tpath("rwy_test_bootcfg_delay.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_boot_delay();
    fclose(fl.f);
    CHECK(run_checker_args("", img, "") == 4 && chk_has("BOOT-DELAY: not a number from 0 to 30"), "checker flags a bad BOOT-DELAY");

    img = tpath("rwy_test_bootcfg_quota.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_quota_scalar();
    fclose(fl.f);
    CHECK(run_checker_args("", img, "") == 4 && chk_has("QUOTA: not a number from 10 to 95"), "checker flags a bad QUOTA");

    img = tpath("rwy_test_bootcfg_loop.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_loop_guard();
    fclose(fl.f);
    CHECK(run_checker_args("", img, "") == 4 && chk_has("LOOP-WINDOW: not a number from 30 to 86400") &&
          chk_has("LOOP-GUARD: not on or off"), "checker flags bad loop guard settings");

    img = tpath("rwy_test_bootcfg_lp_demote.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_loop_demote_to_older();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after the boot loop test");

    img = tpath("rwy_test_bootcfg_lp_demote.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_loop_demote_to_recovery();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after the boot loop test");

    img = tpath("rwy_test_bootcfg_lp_no_oth.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_loop_no_other_way();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after the boot loop test");

    img = tpath("rwy_test_bootcfg_lp_not_fo.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_loop_not_for_trial_or_preview();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after the boot loop test");

    img = tpath("rwy_test_bootcfg_rcy.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_recycle_pick();
    fclose(fl.f);

    img = tpath("rwy_test_bootcfg_evb.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_evict_blocks();
    fclose(fl.f);

    img = tpath("rwy_test_bootcfg_evp.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_evict_plan();
    fclose(fl.f);

    img = tpath("rwy_test_bootcfg_evx.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_evict_execute();
    fclose(fl.f);

    img = tpath("rwy_test_bootcfg_cnd.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_condemn();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after condemn");

    img = tpath("rwy_test_bootcfg_lbl.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_select_label();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after selection label tests");

    img = tpath("rwy_test_bootcfg_entry.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_select_entry();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after selection entry tests");

    img = tpath("rwy_test_bootcfg_entries.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_entries_list();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after the entry list test");

    img = tpath("rwy_test_bootcfg_full.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_full_store();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after the full-store install tests");

    img = tpath("rwy_test_bootcfg_fst.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_state_on_full_store();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after the full-store state changes");

    test_failed_commit(base);
    fi_select(base, 0);
    fi_select(base, 1);
    fi_condemn(base);
    fi_make_room(base);
    test_make_room_compacts(base);
    test_read_error(base);
    test_read_error_sweep(base);

    img = tpath("rwy_test_bootcfg_rec.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_recovery_unlock();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after unlock tests");
    img = tpath("rwy_test_bootcfg_rec2.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_select_recovery();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after recovery selection tests");
    img = tpath("rwy_test_bootcfg_rec3.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_select_recovery_only();
    fclose(fl.f);
    CHECK(run_checker_args("", img, "") == 4 && chk_has("RECOVERY: KERNEL missing"), "checker reports the invalid RECOVERY");
    img = tpath("rwy_test_bootcfg_fb.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_fallback();
    fclose(fl.f);
    img = tpath("rwy_test_bootcfg_pv.img");
    copy_file(base, img);
    open_store(&S, &dev, &fl, img);
    test_preview();
    fclose(fl.f);
    CHECK(run_checker(img) == 0, "checker passes after preview tests");
    CHECK(run_checker(img) == 0, "checker passes after fallback tests");
    fi_recovery(base, 1);
    fi_recovery(base, 0);

    test_check_repair(base);
    test_check_record(base);
    test_record_damaged(base);
    test_slotstate();
    test_slotstate_health();
    test_slotstate_roster();
    test_anchor(base);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: bootcfgtest STORE [CHECKER]\n"); return 2; }
    if (argc > 2) g_checkbin = argv[2];
    run(argv[1]);
    if (failures) { printf("BOOTCFGTEST: FAIL (%d)\n", failures); return 1; }
    printf("BOOTCFGTEST: PASS\n");
    return 0;
}
