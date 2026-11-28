/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright (c) 2023 Imagination Technologies Ltd. */
/*
 * Layout checks of the ported FWIF structures, taken unchanged from
 * pvr_rogue_fwif_check.h and pvr_rogue_fwif_shared_check.h of the Linux
 * drm/imagination driver (only the lines of the structures ported to
 * pvr_rogue_fwif.h). Included by pvr_rogue_fwif.h.
 */

#ifndef PVR_ROGUE_FWIF_CHECK_H
#define PVR_ROGUE_FWIF_CHECK_H

OFFSET_CHECK(struct rogue_fwif_dma_addr, dev_addr, 0);
OFFSET_CHECK(struct rogue_fwif_dma_addr, fw_addr, 8);
SIZE_CHECK(struct rogue_fwif_dma_addr, 16);
OFFSET_CHECK(struct rogue_fwif_file_info_buf, path, 0);
OFFSET_CHECK(struct rogue_fwif_file_info_buf, info, 200);
OFFSET_CHECK(struct rogue_fwif_file_info_buf, line_num, 400);
SIZE_CHECK(struct rogue_fwif_file_info_buf, 408);

OFFSET_CHECK(struct rogue_fwif_tracebuf_space, trace_pointer, 0);
OFFSET_CHECK(struct rogue_fwif_tracebuf_space, trace_buffer_fw_addr, 4);
OFFSET_CHECK(struct rogue_fwif_tracebuf_space, trace_buffer, 8);
OFFSET_CHECK(struct rogue_fwif_tracebuf_space, assert_buf, 16);
SIZE_CHECK(struct rogue_fwif_tracebuf_space, 424);

OFFSET_CHECK(struct rogue_fwif_tracebuf, log_type, 0);
OFFSET_CHECK(struct rogue_fwif_tracebuf, tracebuf, 8);
OFFSET_CHECK(struct rogue_fwif_tracebuf, tracebuf_size_in_dwords, 856);
OFFSET_CHECK(struct rogue_fwif_tracebuf, tracebuf_flags, 860);
SIZE_CHECK(struct rogue_fwif_tracebuf, 864);

OFFSET_CHECK(struct rogue_fw_fault_info, cr_timer, 0);
OFFSET_CHECK(struct rogue_fw_fault_info, os_timer, 8);
OFFSET_CHECK(struct rogue_fw_fault_info, data, 16);
OFFSET_CHECK(struct rogue_fw_fault_info, reserved, 20);
OFFSET_CHECK(struct rogue_fw_fault_info, fault_buf, 24);
SIZE_CHECK(struct rogue_fw_fault_info, 432);

OFFSET_CHECK(struct rogue_fwif_sysdata, config_flags, 0);
OFFSET_CHECK(struct rogue_fwif_sysdata, config_flags_ext, 4);
OFFSET_CHECK(struct rogue_fwif_sysdata, pow_state, 8);
OFFSET_CHECK(struct rogue_fwif_sysdata, hw_perf_ridx, 12);
OFFSET_CHECK(struct rogue_fwif_sysdata, hw_perf_widx, 16);
OFFSET_CHECK(struct rogue_fwif_sysdata, hw_perf_wrap_count, 20);
OFFSET_CHECK(struct rogue_fwif_sysdata, hw_perf_size, 24);
OFFSET_CHECK(struct rogue_fwif_sysdata, hw_perf_drop_count, 28);
OFFSET_CHECK(struct rogue_fwif_sysdata, hw_perf_ut, 32);
OFFSET_CHECK(struct rogue_fwif_sysdata, first_drop_ordinal, 36);
OFFSET_CHECK(struct rogue_fwif_sysdata, last_drop_ordinal, 40);
OFFSET_CHECK(struct rogue_fwif_sysdata, os_runtime_flags_mirror, 44);
OFFSET_CHECK(struct rogue_fwif_sysdata, fault_info, 80);
OFFSET_CHECK(struct rogue_fwif_sysdata, fw_faults, 3536);
OFFSET_CHECK(struct rogue_fwif_sysdata, cr_poll_addr, 3540);
OFFSET_CHECK(struct rogue_fwif_sysdata, cr_poll_mask, 3548);
OFFSET_CHECK(struct rogue_fwif_sysdata, cr_poll_count, 3556);
OFFSET_CHECK(struct rogue_fwif_sysdata, start_idle_time, 3568);
OFFSET_CHECK(struct rogue_fwif_sysdata, hwr_state_flags, 3576);
OFFSET_CHECK(struct rogue_fwif_sysdata, hwr_recovery_flags, 3580);
OFFSET_CHECK(struct rogue_fwif_sysdata, fw_sys_data_flags, 3616);
OFFSET_CHECK(struct rogue_fwif_sysdata, mc_config, 3620);
SIZE_CHECK(struct rogue_fwif_sysdata, 3624);

OFFSET_CHECK(struct rogue_fwif_slr_entry, timestamp, 0);
OFFSET_CHECK(struct rogue_fwif_slr_entry, fw_ctx_addr, 8);
OFFSET_CHECK(struct rogue_fwif_slr_entry, num_ufos, 12);
OFFSET_CHECK(struct rogue_fwif_slr_entry, ccb_name, 16);
SIZE_CHECK(struct rogue_fwif_slr_entry, 48);

OFFSET_CHECK(struct rogue_fwif_osdata, fw_os_config_flags, 0);
OFFSET_CHECK(struct rogue_fwif_osdata, fw_sync_check_mark, 4);
OFFSET_CHECK(struct rogue_fwif_osdata, host_sync_check_mark, 8);
OFFSET_CHECK(struct rogue_fwif_osdata, forced_updates_requested, 12);
OFFSET_CHECK(struct rogue_fwif_osdata, slr_log_wp, 16);
OFFSET_CHECK(struct rogue_fwif_osdata, slr_log_first, 24);
OFFSET_CHECK(struct rogue_fwif_osdata, slr_log, 72);
OFFSET_CHECK(struct rogue_fwif_osdata, last_forced_update_time, 552);
OFFSET_CHECK(struct rogue_fwif_osdata, interrupt_count, 560);
OFFSET_CHECK(struct rogue_fwif_osdata, kccb_cmds_executed, 568);
OFFSET_CHECK(struct rogue_fwif_osdata, power_sync_fw_addr, 572);
OFFSET_CHECK(struct rogue_fwif_osdata, fw_os_data_flags, 576);
SIZE_CHECK(struct rogue_fwif_osdata, 584);

OFFSET_CHECK(struct rogue_bifinfo, bif_req_status, 0);
OFFSET_CHECK(struct rogue_bifinfo, bif_mmu_status, 8);
OFFSET_CHECK(struct rogue_bifinfo, pc_address, 16);
OFFSET_CHECK(struct rogue_bifinfo, reserved, 24);
SIZE_CHECK(struct rogue_bifinfo, 32);

OFFSET_CHECK(struct rogue_eccinfo, fault_gpu, 0);
SIZE_CHECK(struct rogue_eccinfo, 4);

OFFSET_CHECK(struct rogue_mmuinfo, mmu_status, 0);
OFFSET_CHECK(struct rogue_mmuinfo, pc_address, 16);
OFFSET_CHECK(struct rogue_mmuinfo, reserved, 24);
SIZE_CHECK(struct rogue_mmuinfo, 32);

OFFSET_CHECK(struct rogue_pollinfo, thread_num, 0);
OFFSET_CHECK(struct rogue_pollinfo, cr_poll_addr, 4);
OFFSET_CHECK(struct rogue_pollinfo, cr_poll_mask, 8);
OFFSET_CHECK(struct rogue_pollinfo, cr_poll_last_value, 12);
OFFSET_CHECK(struct rogue_pollinfo, reserved, 16);
SIZE_CHECK(struct rogue_pollinfo, 24);

OFFSET_CHECK(struct rogue_tlbinfo, bad_addr, 0);
OFFSET_CHECK(struct rogue_tlbinfo, entry_lo, 4);
SIZE_CHECK(struct rogue_tlbinfo, 8);

OFFSET_CHECK(struct rogue_hwrinfo, hwr_data, 0);
OFFSET_CHECK(struct rogue_hwrinfo, cr_timer, 32);
OFFSET_CHECK(struct rogue_hwrinfo, os_timer, 40);
OFFSET_CHECK(struct rogue_hwrinfo, frame_num, 48);
OFFSET_CHECK(struct rogue_hwrinfo, pid, 52);
OFFSET_CHECK(struct rogue_hwrinfo, active_hwrt_data, 56);
OFFSET_CHECK(struct rogue_hwrinfo, hwr_number, 60);
OFFSET_CHECK(struct rogue_hwrinfo, event_status, 64);
OFFSET_CHECK(struct rogue_hwrinfo, hwr_recovery_flags, 68);
OFFSET_CHECK(struct rogue_hwrinfo, hwr_type, 72);
OFFSET_CHECK(struct rogue_hwrinfo, dm, 76);
OFFSET_CHECK(struct rogue_hwrinfo, core_id, 80);
OFFSET_CHECK(struct rogue_hwrinfo, cr_time_of_kick, 88);
OFFSET_CHECK(struct rogue_hwrinfo, cr_time_hw_reset_start, 96);
OFFSET_CHECK(struct rogue_hwrinfo, cr_time_hw_reset_finish, 104);
OFFSET_CHECK(struct rogue_hwrinfo, cr_time_freelist_ready, 112);
OFFSET_CHECK(struct rogue_hwrinfo, reserved, 120);
SIZE_CHECK(struct rogue_hwrinfo, 136);

OFFSET_CHECK(struct rogue_fwif_hwrinfobuf, hwr_info, 0);
OFFSET_CHECK(struct rogue_fwif_hwrinfobuf, hwr_counter, 2176);
OFFSET_CHECK(struct rogue_fwif_hwrinfobuf, write_index, 2180);
OFFSET_CHECK(struct rogue_fwif_hwrinfobuf, dd_req_count, 2184);
OFFSET_CHECK(struct rogue_fwif_hwrinfobuf, hwr_info_buf_flags, 2188);
OFFSET_CHECK(struct rogue_fwif_hwrinfobuf, hwr_dm_locked_up_count, 2192);
OFFSET_CHECK(struct rogue_fwif_hwrinfobuf, hwr_dm_overran_count, 2228);
OFFSET_CHECK(struct rogue_fwif_hwrinfobuf, hwr_dm_recovered_count, 2264);
OFFSET_CHECK(struct rogue_fwif_hwrinfobuf, hwr_dm_false_detect_count, 2300);
SIZE_CHECK(struct rogue_fwif_hwrinfobuf, 2336);

OFFSET_CHECK(struct rogue_fwif_ccb_ctl, write_offset, 0);
OFFSET_CHECK(struct rogue_fwif_ccb_ctl, padding, 4);
OFFSET_CHECK(struct rogue_fwif_ccb_ctl, read_offset, 128);
OFFSET_CHECK(struct rogue_fwif_ccb_ctl, wrap_mask, 132);
OFFSET_CHECK(struct rogue_fwif_ccb_ctl, cmd_size, 136);
OFFSET_CHECK(struct rogue_fwif_ccb_ctl, padding2, 140);
SIZE_CHECK(struct rogue_fwif_ccb_ctl, 144);

OFFSET_CHECK(struct rogue_fwif_kccb_cmd, cmd_type, 0);
OFFSET_CHECK(struct rogue_fwif_kccb_cmd, kccb_flags, 4);
OFFSET_CHECK(struct rogue_fwif_kccb_cmd, cmd_data, 8);
SIZE_CHECK(struct rogue_fwif_kccb_cmd, 88);

OFFSET_CHECK(struct rogue_fwif_fwccb_cmd, cmd_type, 0);
OFFSET_CHECK(struct rogue_fwif_fwccb_cmd, fwccb_flags, 4);
OFFSET_CHECK(struct rogue_fwif_fwccb_cmd, cmd_data, 8);
SIZE_CHECK(struct rogue_fwif_fwccb_cmd, 88);

OFFSET_CHECK(struct rogue_fwif_runtime_cfg, active_pm_latency_ms, 0);
OFFSET_CHECK(struct rogue_fwif_runtime_cfg, runtime_cfg_flags, 4);
OFFSET_CHECK(struct rogue_fwif_runtime_cfg, active_pm_latency_persistant, 8);
OFFSET_CHECK(struct rogue_fwif_runtime_cfg, core_clock_speed, 12);
OFFSET_CHECK(struct rogue_fwif_runtime_cfg, default_dusts_num_init, 16);
OFFSET_CHECK(struct rogue_fwif_runtime_cfg, phr_mode, 20);
OFFSET_CHECK(struct rogue_fwif_runtime_cfg, hcs_deadline_ms, 24);
OFFSET_CHECK(struct rogue_fwif_runtime_cfg, wdg_period_us, 28);
OFFSET_CHECK(struct rogue_fwif_runtime_cfg, osid_priority, 32);
OFFSET_CHECK(struct rogue_fwif_runtime_cfg, hwperf_buf_fw_addr, 64);
OFFSET_CHECK(struct rogue_fwif_runtime_cfg, padding, 68);
SIZE_CHECK(struct rogue_fwif_runtime_cfg, 72);

OFFSET_CHECK(struct rogue_fwif_connection_ctl, connection_fw_state, 0);
OFFSET_CHECK(struct rogue_fwif_connection_ctl, connection_os_state, 4);
OFFSET_CHECK(struct rogue_fwif_connection_ctl, alive_fw_token, 8);
OFFSET_CHECK(struct rogue_fwif_connection_ctl, alive_os_token, 12);
SIZE_CHECK(struct rogue_fwif_connection_ctl, 16);

OFFSET_CHECK(struct rogue_fwif_compchecks_bvnc, layout_version, 0);
OFFSET_CHECK(struct rogue_fwif_compchecks_bvnc, bvnc, 8);
SIZE_CHECK(struct rogue_fwif_compchecks_bvnc, 16);

OFFSET_CHECK(struct rogue_fwif_init_options, os_count_support, 0);
SIZE_CHECK(struct rogue_fwif_init_options, 8);

OFFSET_CHECK(struct rogue_fwif_compchecks, hw_bvnc, 0);
OFFSET_CHECK(struct rogue_fwif_compchecks, fw_bvnc, 16);
OFFSET_CHECK(struct rogue_fwif_compchecks, fw_processor_version, 32);
OFFSET_CHECK(struct rogue_fwif_compchecks, ddk_version, 36);
OFFSET_CHECK(struct rogue_fwif_compchecks, ddk_build, 40);
OFFSET_CHECK(struct rogue_fwif_compchecks, build_options, 44);
OFFSET_CHECK(struct rogue_fwif_compchecks, init_options, 48);
OFFSET_CHECK(struct rogue_fwif_compchecks, updated, 56);
SIZE_CHECK(struct rogue_fwif_compchecks, 64);

OFFSET_CHECK(struct rogue_fwif_osinit, kernel_ccbctl_fw_addr, 0);
OFFSET_CHECK(struct rogue_fwif_osinit, kernel_ccb_fw_addr, 4);
OFFSET_CHECK(struct rogue_fwif_osinit, kernel_ccb_rtn_slots_fw_addr, 8);
OFFSET_CHECK(struct rogue_fwif_osinit, firmware_ccbctl_fw_addr, 12);
OFFSET_CHECK(struct rogue_fwif_osinit, firmware_ccb_fw_addr, 16);
OFFSET_CHECK(struct rogue_fwif_osinit, work_est_firmware_ccbctl_fw_addr, 20);
OFFSET_CHECK(struct rogue_fwif_osinit, work_est_firmware_ccb_fw_addr, 24);
OFFSET_CHECK(struct rogue_fwif_osinit, rogue_fwif_hwr_info_buf_ctl_fw_addr, 28);
OFFSET_CHECK(struct rogue_fwif_osinit, hwr_debug_dump_limit, 32);
OFFSET_CHECK(struct rogue_fwif_osinit, fw_os_data_fw_addr, 36);
OFFSET_CHECK(struct rogue_fwif_osinit, rogue_comp_checks, 40);
SIZE_CHECK(struct rogue_fwif_osinit, 104);

OFFSET_CHECK(struct rogue_fwif_sigbuf_ctl, buffer_fw_addr, 0);
OFFSET_CHECK(struct rogue_fwif_sigbuf_ctl, left_size_in_regs, 4);
SIZE_CHECK(struct rogue_fwif_sigbuf_ctl, 8);

OFFSET_CHECK(struct pdvfs_opp, volt, 0);
OFFSET_CHECK(struct pdvfs_opp, freq, 4);
SIZE_CHECK(struct pdvfs_opp, 8);

OFFSET_CHECK(struct rogue_fwif_pdvfs_opp, opp_values, 0);
OFFSET_CHECK(struct rogue_fwif_pdvfs_opp, min_opp_point, 128);
OFFSET_CHECK(struct rogue_fwif_pdvfs_opp, max_opp_point, 132);
SIZE_CHECK(struct rogue_fwif_pdvfs_opp, 136);

OFFSET_CHECK(struct rogue_fwif_counter_dump_ctl, buffer_fw_addr, 0);
OFFSET_CHECK(struct rogue_fwif_counter_dump_ctl, size_in_dwords, 4);
SIZE_CHECK(struct rogue_fwif_counter_dump_ctl, 8);

OFFSET_CHECK(struct rogue_hwperf_bvnc, bvnc_string, 0);
OFFSET_CHECK(struct rogue_hwperf_bvnc, bvnc_km_feature_flags, 24);
OFFSET_CHECK(struct rogue_hwperf_bvnc, num_bvnc_blocks, 28);
OFFSET_CHECK(struct rogue_hwperf_bvnc, bvnc_gpu_cores, 30);
OFFSET_CHECK(struct rogue_hwperf_bvnc, bvnc_blocks, 32);
SIZE_CHECK(struct rogue_hwperf_bvnc, 160);

OFFSET_CHECK(struct rogue_fwif_sysinit, fault_phys_addr, 0);
OFFSET_CHECK(struct rogue_fwif_sysinit, pds_exec_base, 8);
OFFSET_CHECK(struct rogue_fwif_sysinit, usc_exec_base, 16);
OFFSET_CHECK(struct rogue_fwif_sysinit, fbcdc_state_table_base, 24);
OFFSET_CHECK(struct rogue_fwif_sysinit, fbcdc_large_state_table_base, 32);
OFFSET_CHECK(struct rogue_fwif_sysinit, texture_heap_base, 40);
OFFSET_CHECK(struct rogue_fwif_sysinit, hw_perf_filter, 48);
OFFSET_CHECK(struct rogue_fwif_sysinit, slc3_fence_dev_addr, 56);
OFFSET_CHECK(struct rogue_fwif_sysinit, tpu_trilinear_frac_mask, 64);
OFFSET_CHECK(struct rogue_fwif_sysinit, sigbuf_ctl, 80);
OFFSET_CHECK(struct rogue_fwif_sysinit, pdvfs_opp_info, 152);
OFFSET_CHECK(struct rogue_fwif_sysinit, coremem_data_store, 288);
OFFSET_CHECK(struct rogue_fwif_sysinit, counter_dump_ctl, 304);
OFFSET_CHECK(struct rogue_fwif_sysinit, filter_flags, 312);
OFFSET_CHECK(struct rogue_fwif_sysinit, runtime_cfg_fw_addr, 316);
OFFSET_CHECK(struct rogue_fwif_sysinit, trace_buf_ctl_fw_addr, 320);
OFFSET_CHECK(struct rogue_fwif_sysinit, fw_sys_data_fw_addr, 324);
OFFSET_CHECK(struct rogue_fwif_sysinit, gpu_util_fw_cb_ctl_fw_addr, 328);
OFFSET_CHECK(struct rogue_fwif_sysinit, reg_cfg_fw_addr, 332);
OFFSET_CHECK(struct rogue_fwif_sysinit, hwperf_ctl_fw_addr, 336);
OFFSET_CHECK(struct rogue_fwif_sysinit, align_checks, 340);
OFFSET_CHECK(struct rogue_fwif_sysinit, initial_core_clock_speed, 344);
OFFSET_CHECK(struct rogue_fwif_sysinit, active_pm_latency_ms, 348);
OFFSET_CHECK(struct rogue_fwif_sysinit, firmware_started, 352);
OFFSET_CHECK(struct rogue_fwif_sysinit, marker_val, 356);
OFFSET_CHECK(struct rogue_fwif_sysinit, firmware_started_timestamp, 360);
OFFSET_CHECK(struct rogue_fwif_sysinit, jones_disable_mask, 364);
OFFSET_CHECK(struct rogue_fwif_sysinit, firmware_perf, 368);
OFFSET_CHECK(struct rogue_fwif_sysinit, core_clock_rate_fw_addr, 372);
OFFSET_CHECK(struct rogue_fwif_sysinit, gpio_validation_mode, 376);
OFFSET_CHECK(struct rogue_fwif_sysinit, bvnc_km_feature_flags, 380);
OFFSET_CHECK(struct rogue_fwif_sysinit, tfbc_compression_control, 540);
SIZE_CHECK(struct rogue_fwif_sysinit, 544);

OFFSET_CHECK(struct rogue_fwif_gpu_util_fwcb, time_corr, 0);
OFFSET_CHECK(struct rogue_fwif_gpu_util_fwcb, time_corr_seq_count, 10240);
OFFSET_CHECK(struct rogue_fwif_gpu_util_fwcb, gpu_util_flags, 10244);
OFFSET_CHECK(struct rogue_fwif_gpu_util_fwcb, last_word, 10248);
OFFSET_CHECK(struct rogue_fwif_gpu_util_fwcb, stats_counters, 10256);
SIZE_CHECK(struct rogue_fwif_gpu_util_fwcb, 10280);


/*
 * Context, client CCB and compute command structures (pvr_rogue_fwif_check.h,
 * pvr_rogue_fwif_shared_check.h, pvr_rogue_fwif_client_check.h)
 */
OFFSET_CHECK(struct rogue_fwif_ufo, addr, 0);
OFFSET_CHECK(struct rogue_fwif_ufo, value, 4);
SIZE_CHECK(struct rogue_fwif_ufo, 8);
OFFSET_CHECK(struct rogue_fwif_cccb_ctl, write_offset, 0);
OFFSET_CHECK(struct rogue_fwif_cccb_ctl, read_offset, 4);
OFFSET_CHECK(struct rogue_fwif_cccb_ctl, dep_offset, 8);
OFFSET_CHECK(struct rogue_fwif_cccb_ctl, wrap_mask, 12);
OFFSET_CHECK(struct rogue_fwif_cccb_ctl, read_offset2, 16);
OFFSET_CHECK(struct rogue_fwif_cccb_ctl, read_offset3, 20);
OFFSET_CHECK(struct rogue_fwif_cccb_ctl, read_offset4, 24);
SIZE_CHECK(struct rogue_fwif_cccb_ctl, 32);
OFFSET_CHECK(struct rogue_fwif_cdm_registers_cswitch, cdmreg_cdm_context_pds0,
	0);
OFFSET_CHECK(struct rogue_fwif_cdm_registers_cswitch, cdmreg_cdm_context_pds1,
	8);
OFFSET_CHECK(struct rogue_fwif_cdm_registers_cswitch, cdmreg_cdm_terminate_pds,
	16);
OFFSET_CHECK(struct rogue_fwif_cdm_registers_cswitch, cdmreg_cdm_terminate_pds1,
	24);
OFFSET_CHECK(struct rogue_fwif_cdm_registers_cswitch, cdmreg_cdm_resume_pds0,
	32);
OFFSET_CHECK(struct rogue_fwif_cdm_registers_cswitch, cdmreg_cdm_context_pds0_b,
	40);
OFFSET_CHECK(struct rogue_fwif_cdm_registers_cswitch, cdmreg_cdm_resume_pds0_b,
	48);
SIZE_CHECK(struct rogue_fwif_cdm_registers_cswitch, 56);
OFFSET_CHECK(struct rogue_fwif_static_computecontext_state, ctxswitch_regs, 0);
SIZE_CHECK(struct rogue_fwif_static_computecontext_state, 56);
OFFSET_CHECK(struct rogue_fwif_cmd_common, frame_num, 0);
SIZE_CHECK(struct rogue_fwif_cmd_common, 4);
OFFSET_CHECK(struct rogue_fwif_fwmemcontext, pc_dev_paddr, 0);
OFFSET_CHECK(struct rogue_fwif_fwmemcontext, page_cat_base_reg_set, 8);
OFFSET_CHECK(struct rogue_fwif_fwmemcontext, breakpoint_addr, 12);
OFFSET_CHECK(struct rogue_fwif_fwmemcontext, bp_handler_addr, 16);
OFFSET_CHECK(struct rogue_fwif_fwmemcontext, breakpoint_ctl, 20);
OFFSET_CHECK(struct rogue_fwif_fwmemcontext, fw_mem_ctx_flags, 24);
SIZE_CHECK(struct rogue_fwif_fwmemcontext, 32);
OFFSET_CHECK(struct rogue_fwif_frag_ctx_state,
	frag_reg_pm_deallocated_mask_status, 0);
OFFSET_CHECK(struct rogue_fwif_frag_ctx_state, frag_reg_dm_pds_mtilefree_status,
	4);
OFFSET_CHECK(struct rogue_fwif_frag_ctx_state, ctx_state_flags, 8);
OFFSET_CHECK(struct rogue_fwif_frag_ctx_state, frag_reg_isp_store, 12);
FLEX_ARRAY_CHECK(struct rogue_fwif_frag_ctx_state, frag_reg_isp_store);
OFFSET_CHECK(struct rogue_fwif_compute_ctx_state, ctx_state_flags, 0);
SIZE_CHECK(struct rogue_fwif_compute_ctx_state, 4);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, ccbctl_fw_addr, 0);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, ccb_fw_addr, 4);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, ccb_meta_dma_addr, 8);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, context_state_addr, 24);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, fw_com_ctx_flags, 28);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, priority, 32);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, priority_seq_num, 36);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, rf_cmd_addr, 40);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, stats_pending, 44);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, stats_num_stores, 48);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, stats_num_out_of_memory, 52);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, stats_num_partial_renders, 56);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, dm, 60);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, wait_signal_address, 64);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, wait_signal_node, 72);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, buf_stalled_node, 80);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, cbuf_queue_ctrl_addr, 88);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, robustness_address, 96);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, max_deadline_ms, 104);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, read_offset_needs_reset, 108);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, waiting_node, 112);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, run_node, 120);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, last_failed_ufo, 128);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, fw_mem_context_fw_addr, 136);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, server_common_context_id, 140);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, pid, 144);
OFFSET_CHECK(struct rogue_fwif_fwcommoncontext, geom_oom_disabled, 148);
SIZE_CHECK(struct rogue_fwif_fwcommoncontext, 152);
OFFSET_CHECK(struct rogue_fwif_kccb_cmd_kick_data, context_fw_addr, 0);
OFFSET_CHECK(struct rogue_fwif_kccb_cmd_kick_data, client_woff_update, 4);
OFFSET_CHECK(struct rogue_fwif_kccb_cmd_kick_data, client_wrap_mask_update, 8);
OFFSET_CHECK(struct rogue_fwif_kccb_cmd_kick_data, num_cleanup_ctl, 12);
OFFSET_CHECK(struct rogue_fwif_kccb_cmd_kick_data, cleanup_ctl_fw_addr, 16);
OFFSET_CHECK(struct rogue_fwif_kccb_cmd_kick_data, work_est_cmd_header_offset,
	28);
SIZE_CHECK(struct rogue_fwif_kccb_cmd_kick_data, 32);
OFFSET_CHECK(struct rogue_fwif_cleanup_request, cleanup_type, 0);
OFFSET_CHECK(struct rogue_fwif_cleanup_request, cleanup_data, 4);
SIZE_CHECK(struct rogue_fwif_cleanup_request, 8);
OFFSET_CHECK(struct rogue_fwif_ccb_cmd_header, cmd_type, 0);
OFFSET_CHECK(struct rogue_fwif_ccb_cmd_header, cmd_size, 4);
OFFSET_CHECK(struct rogue_fwif_ccb_cmd_header, ext_job_ref, 8);
OFFSET_CHECK(struct rogue_fwif_ccb_cmd_header, int_job_ref, 12);
OFFSET_CHECK(struct rogue_fwif_ccb_cmd_header, work_est_kick_data, 16);
SIZE_CHECK(struct rogue_fwif_ccb_cmd_header, 40);
OFFSET_CHECK(struct rogue_fwif_compute_regs, tpu_border_colour_table, 0);
OFFSET_CHECK(struct rogue_fwif_compute_regs, cdm_cb_queue, 8);
OFFSET_CHECK(struct rogue_fwif_compute_regs, cdm_cb_base, 16);
OFFSET_CHECK(struct rogue_fwif_compute_regs, cdm_cb, 24);
OFFSET_CHECK(struct rogue_fwif_compute_regs, cdm_ctrl_stream_base, 32);
OFFSET_CHECK(struct rogue_fwif_compute_regs, cdm_context_state_base_addr, 40);
OFFSET_CHECK(struct rogue_fwif_compute_regs, tpu, 48);
OFFSET_CHECK(struct rogue_fwif_compute_regs, cdm_resume_pds1, 52);
OFFSET_CHECK(struct rogue_fwif_compute_regs, cdm_item, 56);
OFFSET_CHECK(struct rogue_fwif_compute_regs, compute_cluster, 60);
OFFSET_CHECK(struct rogue_fwif_compute_regs, tpu_tag_cdm_ctrl, 64);
SIZE_CHECK(struct rogue_fwif_compute_regs, 72);
OFFSET_CHECK(struct rogue_fwif_cmd_compute, common, 0);
OFFSET_CHECK(struct rogue_fwif_cmd_compute, regs, 8);
OFFSET_CHECK(struct rogue_fwif_cmd_compute, flags, 80);
OFFSET_CHECK(struct rogue_fwif_cmd_compute, num_temp_regions, 84);
OFFSET_CHECK(struct rogue_fwif_cmd_compute, stream_start_offset, 88);
OFFSET_CHECK(struct rogue_fwif_cmd_compute, execute_count, 92);
SIZE_CHECK(struct rogue_fwif_cmd_compute, 96);

/*
 * Not checked by Linux; offsets derived from the Linux definitions, so that
 * a layout change of the port is caught.
 */
OFFSET_CHECK(struct rogue_fwif_fwcomputecontext, cdm_context, 0);
OFFSET_CHECK(struct rogue_fwif_fwcomputecontext, static_compute_context_state,
	152);
OFFSET_CHECK(struct rogue_fwif_fwcomputecontext, work_est_ccb_submitted, 208);
OFFSET_CHECK(struct rogue_fwif_fwcomputecontext, core_mask_b, 228);
SIZE_CHECK(struct rogue_fwif_fwcomputecontext, 232);
OFFSET_CHECK(struct rogue_fwif_fwtransfercontext, tq_context, 0);
SIZE_CHECK(struct rogue_fwif_fwtransfercontext, 152);
OFFSET_CHECK(struct rogue_fwif_workest_kick_data, return_data_index, 0);
OFFSET_CHECK(struct rogue_fwif_workest_kick_data, cycles_prediction, 8);
OFFSET_CHECK(struct rogue_fwif_workest_kick_data, deadline, 16);
SIZE_CHECK(struct rogue_fwif_workest_kick_data, 24);
OFFSET_CHECK(struct rogue_fwif_mmucachedata, mmu_cache_sync_update_value, 8);
SIZE_CHECK(struct rogue_fwif_mmucachedata, 12);

/*
 * Render contexts, free lists, HWRT data, geometry/fragment/transfer commands
 * (pvr_rogue_fwif_check.h, pvr_rogue_fwif_shared_check.h,
 * pvr_rogue_fwif_client_check.h; extracted unchanged)
 */
OFFSET_CHECK(struct rogue_fwif_cleanup_ctl, submitted_commands, 0);
OFFSET_CHECK(struct rogue_fwif_cleanup_ctl, executed_commands, 4);
SIZE_CHECK(struct rogue_fwif_cleanup_ctl, 8);

OFFSET_CHECK(struct rogue_fwif_cmd_geom_frag_shared, cmn, 0);
OFFSET_CHECK(struct rogue_fwif_cmd_geom_frag_shared, hwrt_data_fw_addr, 4);
OFFSET_CHECK(struct rogue_fwif_cmd_geom_frag_shared, pr_buffer_fw_addr, 8);
SIZE_CHECK(struct rogue_fwif_cmd_geom_frag_shared, 16);

OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_reg_vdm_context_state_base_addr, 0);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_reg_vdm_context_state_resume_addr, 8);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_reg_ta_context_state_base_addr, 16);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[0].geom_reg_vdm_context_store_task0, 24);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[0].geom_reg_vdm_context_store_task1, 32);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[0].geom_reg_vdm_context_store_task2, 40);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[0].geom_reg_vdm_context_resume_task0, 48);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[0].geom_reg_vdm_context_resume_task1, 56);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[0].geom_reg_vdm_context_resume_task2, 64);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[0].geom_reg_vdm_context_store_task3, 72);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[0].geom_reg_vdm_context_store_task4, 80);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[0].geom_reg_vdm_context_resume_task3, 88);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[0].geom_reg_vdm_context_resume_task4, 96);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[1].geom_reg_vdm_context_store_task0, 104);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[1].geom_reg_vdm_context_store_task1, 112);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[1].geom_reg_vdm_context_store_task2, 120);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[1].geom_reg_vdm_context_resume_task0, 128);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[1].geom_reg_vdm_context_resume_task1, 136);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[1].geom_reg_vdm_context_resume_task2, 144);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[1].geom_reg_vdm_context_store_task3, 152);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[1].geom_reg_vdm_context_store_task4, 160);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[1].geom_reg_vdm_context_resume_task3, 168);
OFFSET_CHECK(struct rogue_fwif_geom_registers_caswitch,
	geom_state[1].geom_reg_vdm_context_resume_task4, 176);
SIZE_CHECK(struct rogue_fwif_geom_registers_caswitch, 184);

OFFSET_CHECK(struct rogue_fwif_static_rendercontext_state, ctxswitch_regs, 0);
SIZE_CHECK(struct rogue_fwif_static_rendercontext_state, 368);

OFFSET_CHECK(struct rogue_fwif_geom_ctx_state_per_geom,
	geom_reg_vdm_call_stack_pointer, 0);
OFFSET_CHECK(struct rogue_fwif_geom_ctx_state_per_geom,
	geom_reg_vdm_call_stack_pointer_init, 8);
OFFSET_CHECK(struct rogue_fwif_geom_ctx_state_per_geom, geom_reg_vbs_so_prim,
	16);
OFFSET_CHECK(struct rogue_fwif_geom_ctx_state_per_geom, geom_current_idx, 32);
SIZE_CHECK(struct rogue_fwif_geom_ctx_state_per_geom, 40);

OFFSET_CHECK(struct rogue_fwif_geom_ctx_state, geom_core, 0);
SIZE_CHECK(struct rogue_fwif_geom_ctx_state, 160);

OFFSET_CHECK(struct rogue_fwif_kccb_cmd_combined_geom_frag_kick_data,
	geom_cmd_kick_data, 0);
OFFSET_CHECK(struct rogue_fwif_kccb_cmd_combined_geom_frag_kick_data,
	frag_cmd_kick_data, 32);
SIZE_CHECK(struct rogue_fwif_kccb_cmd_combined_geom_frag_kick_data, 64);

OFFSET_CHECK(struct rogue_fwif_freelist_gs_data, freelist_fw_addr, 0);
OFFSET_CHECK(struct rogue_fwif_freelist_gs_data, delta_pages, 4);
OFFSET_CHECK(struct rogue_fwif_freelist_gs_data, new_pages, 8);
OFFSET_CHECK(struct rogue_fwif_freelist_gs_data, ready_pages, 12);
SIZE_CHECK(struct rogue_fwif_freelist_gs_data, 16);

OFFSET_CHECK(struct rogue_fwif_freelists_reconstruction_data, freelist_count,
	0);
OFFSET_CHECK(struct rogue_fwif_freelists_reconstruction_data, freelist_ids, 4);
SIZE_CHECK(struct rogue_fwif_freelists_reconstruction_data, 76);

OFFSET_CHECK(struct rogue_fwif_rta_ctl, render_target_index, 0);
OFFSET_CHECK(struct rogue_fwif_rta_ctl, current_render_target, 4);
OFFSET_CHECK(struct rogue_fwif_rta_ctl, active_render_targets, 8);
OFFSET_CHECK(struct rogue_fwif_rta_ctl, cumul_active_render_targets, 12);
OFFSET_CHECK(struct rogue_fwif_rta_ctl, valid_render_targets_fw_addr, 16);
OFFSET_CHECK(struct rogue_fwif_rta_ctl, rta_num_partial_renders_fw_addr, 20);
OFFSET_CHECK(struct rogue_fwif_rta_ctl, max_rts, 24);
OFFSET_CHECK(struct rogue_fwif_rta_ctl, rta_ctl_flags, 28);
SIZE_CHECK(struct rogue_fwif_rta_ctl, 32);

OFFSET_CHECK(struct rogue_fwif_freelist, freelist_dev_addr, 0);
OFFSET_CHECK(struct rogue_fwif_freelist, current_dev_addr, 8);
OFFSET_CHECK(struct rogue_fwif_freelist, current_stack_top, 16);
OFFSET_CHECK(struct rogue_fwif_freelist, max_pages, 20);
OFFSET_CHECK(struct rogue_fwif_freelist, grow_pages, 24);
OFFSET_CHECK(struct rogue_fwif_freelist, current_pages, 28);
OFFSET_CHECK(struct rogue_fwif_freelist, allocated_page_count, 32);
OFFSET_CHECK(struct rogue_fwif_freelist, allocated_mmu_page_count, 36);
OFFSET_CHECK(struct rogue_fwif_freelist, freelist_id, 40);
OFFSET_CHECK(struct rogue_fwif_freelist, grow_pending, 44);
OFFSET_CHECK(struct rogue_fwif_freelist, ready_pages, 48);
OFFSET_CHECK(struct rogue_fwif_freelist, freelist_flags, 52);
OFFSET_CHECK(struct rogue_fwif_freelist, pm_global_pb, 56);
SIZE_CHECK(struct rogue_fwif_freelist, 64);

OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, geom_caches_need_zeroing, 0);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, screen_pixel_max, 4);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, multi_sample_ctl, 8);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, flipped_multi_sample_ctl, 16);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, tpc_stride, 24);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, tpc_size, 28);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, te_screen, 32);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, mtile_stride, 36);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, teaa, 40);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, te_mtile1, 44);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, te_mtile2, 48);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, isp_merge_lower_x, 52);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, isp_merge_lower_y, 56);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, isp_merge_upper_x, 60);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, isp_merge_upper_y, 64);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, isp_merge_scale_x, 68);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, isp_merge_scale_y, 72);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, rgn_header_size, 76);
OFFSET_CHECK(struct rogue_fwif_hwrtdata_common, isp_mtile_size, 80);
SIZE_CHECK(struct rogue_fwif_hwrtdata_common, 88);

OFFSET_CHECK(struct rogue_fwif_hwrtdata, pm_mlist_dev_addr, 0);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, vce_cat_base, 8);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, vce_last_cat_base, 40);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, te_cat_base, 72);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, te_last_cat_base, 104);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, alist_cat_base, 136);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, alist_last_cat_base, 144);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, pm_alist_stack_pointer, 152);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, pm_mlist_stack_pointer, 160);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, hwrt_data_common_fw_addr, 164);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, hwrt_data_flags, 168);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, state, 172);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, freelists_fw_addr, 176);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, freelist_hwr_snapshot, 188);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, vheap_table_dev_addr, 200);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, rta_ctl, 208);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, tail_ptrs_dev_addr, 240);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, macrotile_array_dev_addr, 248);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, rgn_header_dev_addr, 256);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, rtc_dev_addr, 264);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, owner_geom_not_used_by_host, 272);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, geom_caches_need_zeroing, 276);
OFFSET_CHECK(struct rogue_fwif_hwrtdata, cleanup_state, 320);
SIZE_CHECK(struct rogue_fwif_hwrtdata, 384);

OFFSET_CHECK(struct rogue_fwif_geom_regs, vdm_ctrl_stream_base, 0);
OFFSET_CHECK(struct rogue_fwif_geom_regs, tpu_border_colour_table, 8);
OFFSET_CHECK(struct rogue_fwif_geom_regs, vdm_draw_indirect0, 16);
OFFSET_CHECK(struct rogue_fwif_geom_regs, vdm_draw_indirect1, 24);
OFFSET_CHECK(struct rogue_fwif_geom_regs, ppp_ctrl, 28);
OFFSET_CHECK(struct rogue_fwif_geom_regs, te_psg, 32);
OFFSET_CHECK(struct rogue_fwif_geom_regs, tpu, 36);
OFFSET_CHECK(struct rogue_fwif_geom_regs, vdm_context_resume_task0_size, 40);
OFFSET_CHECK(struct rogue_fwif_geom_regs, vdm_context_resume_task3_size, 44);
OFFSET_CHECK(struct rogue_fwif_geom_regs, pds_ctrl, 48);
OFFSET_CHECK(struct rogue_fwif_geom_regs, view_idx, 52);
OFFSET_CHECK(struct rogue_fwif_geom_regs, pds_coeff_free_prog, 56);
SIZE_CHECK(struct rogue_fwif_geom_regs, 64);

OFFSET_CHECK(struct rogue_fwif_dummy_rgnhdr_init_geom_regs, te_psgregion_addr,
	0);
SIZE_CHECK(struct rogue_fwif_dummy_rgnhdr_init_geom_regs, 8);

OFFSET_CHECK(struct rogue_fwif_cmd_geom, cmd_shared, 0);
OFFSET_CHECK(struct rogue_fwif_cmd_geom, regs, 16);
OFFSET_CHECK(struct rogue_fwif_cmd_geom, flags, 80);
OFFSET_CHECK(struct rogue_fwif_cmd_geom, partial_render_geom_frag_fence, 84);
OFFSET_CHECK(struct rogue_fwif_cmd_geom, dummy_rgnhdr_init_geom_regs, 96);
OFFSET_CHECK(struct rogue_fwif_cmd_geom, brn61484_66333_live_rt, 104);
SIZE_CHECK(struct rogue_fwif_cmd_geom, 112);

OFFSET_CHECK(struct rogue_fwif_frag_regs, usc_pixel_output_ctrl, 0);
OFFSET_CHECK(struct rogue_fwif_frag_regs, usc_clear_register, 4);
OFFSET_CHECK(struct rogue_fwif_frag_regs, isp_bgobjdepth, 36);
OFFSET_CHECK(struct rogue_fwif_frag_regs, isp_bgobjvals, 40);
OFFSET_CHECK(struct rogue_fwif_frag_regs, isp_aa, 44);
OFFSET_CHECK(struct rogue_fwif_frag_regs, isp_xtp_pipe_enable, 48);
OFFSET_CHECK(struct rogue_fwif_frag_regs, isp_ctl, 52);
OFFSET_CHECK(struct rogue_fwif_frag_regs, tpu, 56);
OFFSET_CHECK(struct rogue_fwif_frag_regs, event_pixel_pds_info, 60);
OFFSET_CHECK(struct rogue_fwif_frag_regs, pixel_phantom, 64);
OFFSET_CHECK(struct rogue_fwif_frag_regs, view_idx, 68);
OFFSET_CHECK(struct rogue_fwif_frag_regs, event_pixel_pds_data, 72);
OFFSET_CHECK(struct rogue_fwif_frag_regs, brn65101_event_pixel_pds_data, 76);
OFFSET_CHECK(struct rogue_fwif_frag_regs, isp_oclqry_stride, 80);
OFFSET_CHECK(struct rogue_fwif_frag_regs, isp_zls_pixels, 84);
OFFSET_CHECK(struct rogue_fwif_frag_regs, rgx_cr_blackpearl_fix, 88);
OFFSET_CHECK(struct rogue_fwif_frag_regs, isp_scissor_base, 96);
OFFSET_CHECK(struct rogue_fwif_frag_regs, isp_dbias_base, 104);
OFFSET_CHECK(struct rogue_fwif_frag_regs, isp_oclqry_base, 112);
OFFSET_CHECK(struct rogue_fwif_frag_regs, isp_zlsctl, 120);
OFFSET_CHECK(struct rogue_fwif_frag_regs, isp_zload_store_base, 128);
OFFSET_CHECK(struct rogue_fwif_frag_regs, isp_stencil_load_store_base, 136);
OFFSET_CHECK(struct rogue_fwif_frag_regs, fb_cdc_zls, 144);
OFFSET_CHECK(struct rogue_fwif_frag_regs, pbe_word, 152);
OFFSET_CHECK(struct rogue_fwif_frag_regs, tpu_border_colour_table, 344);
OFFSET_CHECK(struct rogue_fwif_frag_regs, pds_bgnd, 352);
OFFSET_CHECK(struct rogue_fwif_frag_regs, pds_bgnd_brn65101, 376);
OFFSET_CHECK(struct rogue_fwif_frag_regs, pds_pr_bgnd, 400);
OFFSET_CHECK(struct rogue_fwif_frag_regs, isp_dummy_stencil_store_base, 424);
OFFSET_CHECK(struct rogue_fwif_frag_regs, isp_dummy_depth_store_base, 432);
OFFSET_CHECK(struct rogue_fwif_frag_regs, rgnhdr_single_rt_size, 440);
OFFSET_CHECK(struct rogue_fwif_frag_regs, rgnhdr_scratch_offset, 444);
SIZE_CHECK(struct rogue_fwif_frag_regs, 448);

OFFSET_CHECK(struct rogue_fwif_cmd_frag, cmd_shared, 0);
OFFSET_CHECK(struct rogue_fwif_cmd_frag, regs, 16);
OFFSET_CHECK(struct rogue_fwif_cmd_frag, flags, 464);
OFFSET_CHECK(struct rogue_fwif_cmd_frag, zls_stride, 468);
OFFSET_CHECK(struct rogue_fwif_cmd_frag, sls_stride, 472);
OFFSET_CHECK(struct rogue_fwif_cmd_frag, execute_count, 476);
SIZE_CHECK(struct rogue_fwif_cmd_frag, 480);

OFFSET_CHECK(struct rogue_fwif_transfer_regs, isp_bgobjvals, 0);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, usc_pixel_output_ctrl, 4);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, usc_clear_register0, 8);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, usc_clear_register1, 12);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, usc_clear_register2, 16);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, usc_clear_register3, 20);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, isp_mtile_size, 24);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, isp_render_origin, 28);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, isp_ctl, 32);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, isp_xtp_pipe_enable, 36);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, isp_aa, 40);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, event_pixel_pds_info, 44);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, event_pixel_pds_code, 48);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, event_pixel_pds_data, 52);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, isp_render, 56);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, isp_rgn, 60);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, frag_screen, 64);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, pds_bgnd0_base, 72);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, pds_bgnd1_base, 80);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, pds_bgnd3_sizeinfo, 88);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, isp_mtile_base, 96);
OFFSET_CHECK(struct rogue_fwif_transfer_regs, pbe_wordx_mrty, 104);
SIZE_CHECK(struct rogue_fwif_transfer_regs, 176);

OFFSET_CHECK(struct rogue_fwif_cmd_transfer, common, 0);
OFFSET_CHECK(struct rogue_fwif_cmd_transfer, regs, 8);
OFFSET_CHECK(struct rogue_fwif_cmd_transfer, flags, 184);
SIZE_CHECK(struct rogue_fwif_cmd_transfer, 192);

/*
 * Not checked by Linux; offsets derived from the Linux definitions.
 */
OFFSET_CHECK(struct rogue_fwif_fwrendercontext, geom_context, 0);
OFFSET_CHECK(struct rogue_fwif_fwrendercontext, frag_context, 152);
OFFSET_CHECK(struct rogue_fwif_fwrendercontext, static_render_context_state,
	304);
OFFSET_CHECK(struct rogue_fwif_fwrendercontext, work_est_ccb_submitted, 672);
OFFSET_CHECK(struct rogue_fwif_fwrendercontext, fw_render_ctx_flags, 676);
SIZE_CHECK(struct rogue_fwif_fwrendercontext, 680);
OFFSET_CHECK(struct rogue_fwif_fwccb_cmd_freelist_gs_data, freelist_id, 0);
SIZE_CHECK(struct rogue_fwif_fwccb_cmd_freelist_gs_data, 4);
OFFSET_CHECK(struct rogue_fwif_fwccb_cmd_freelists_reconstruction_data,
	freelist_count, 0);
OFFSET_CHECK(struct rogue_fwif_fwccb_cmd_freelists_reconstruction_data,
	hwr_counter, 4);
OFFSET_CHECK(struct rogue_fwif_fwccb_cmd_freelists_reconstruction_data,
	freelist_ids, 8);
SIZE_CHECK(struct rogue_fwif_fwccb_cmd_freelists_reconstruction_data, 80);

#endif /* PVR_ROGUE_FWIF_CHECK_H */
