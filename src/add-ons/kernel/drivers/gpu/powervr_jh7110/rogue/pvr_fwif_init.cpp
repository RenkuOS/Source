/*
 * Copyright 2026, The RenkuOS authors.
 * Copyright (c) 2023 Imagination Technologies Ltd.
 * SPDX-License-Identifier: GPL-2.0-only OR MIT
 *
 * Port of the FWIF structure creation of the Linux drm/imagination driver
 * for a MIPS firmware processor: pvr_kccb_init(), pvr_fwccb_init() (pvr_ccb.c),
 * pvr_fw_create_structures() and its fw_*_init() callbacks (pvr_fw.c),
 * pvr_fw_trace_init() (pvr_fw_trace.c). Objects are created in the same
 * order as in pvr_fw_init(), at the same fixed heap offsets.
 */


#include "pvr_fwif_init.h"

#include <string.h>


#define PVR_SYNC_OBJ_SIZE			sizeof(u32)
#define PVR_ROGUE_FAULT_PAGE_SIZE	SZ_4K
#define ROGUE_FWIF_SLC_MIN_SIZE_FOR_DM_OVERLAP_KB	4

#define LOG(format, ...) \
	do { \
		if (env.log != NULL) \
			env.log(env.cookie, format, ##__VA_ARGS__); \
	} while (false)


static status_t
fw_object_create(const pvr_fwif_env& env, pvr_fwif_state* state,
	const char* name, size_t size, uint32 heapOffset, pvr_fwif_range** _range)
{
	if (state->range_count >= PVR_FWIF_MAX_RANGES)
		return B_NO_MEMORY;

	size_t allocSize = (size + PVR_FWIF_PAGE_SIZE - 1)
		& ~(size_t)(PVR_FWIF_PAGE_SIZE - 1);
	void* cpu = NULL;
	uint64 phys = 0;
	uint32 fwAddr = 0;
	status_t status = env.alloc(env.cookie, allocSize, PVR_FWIF_PAGE_SIZE,
		heapOffset, &cpu, &phys, &fwAddr);
	if (status != B_OK) {
		LOG("pvr_fwif: cannot allocate %s (%#zx bytes): %#" B_PRIx32 "\n",
			name, allocSize, (uint32)status);
		return status;
	}

	// recorded first, so that the caller can free it even if it is refused
	pvr_fwif_range* range = &state->ranges[state->range_count++];
	range->name = name;
	range->cpu = cpu;
	range->phys = phys;
	range->fw_addr = fwAddr;
	range->size = allocSize;

	if (cpu == NULL
		|| (fwAddr & ~PVR_FWIF_HEAP_OFFSET_MASK) != PVR_FWIF_MIPS_HEAP_BASE
		|| (heapOffset != PVR_FWIF_ANY_OFFSET
			&& (fwAddr & PVR_FWIF_HEAP_OFFSET_MASK) != heapOffset)) {
		LOG("pvr_fwif: %s got an unexpected address (fw %#" B_PRIx32 ")\n",
			name, fwAddr);
		return B_BAD_VALUE;
	}

	// pvr_gem_object_create() returns zeroed memory
	memset(cpu, 0, allocSize);

	LOG("pvr_fwif: %-16s fw %#010" B_PRIx32 " phys %#" B_PRIx64 " size %#zx"
		" (%#zx used)\n", name, fwAddr, phys, allocSize, size);

	*_range = range;
	return B_OK;
}


//	#pragma mark - pvr_ccb.c


static void
ccb_ctrl_init(rogue_fwif_ccb_ctl* ctrl, const pvr_fwif_ccb& ccb)
{
	ctrl->write_offset = 0;
	ctrl->read_offset = 0;
	ctrl->wrap_mask = ccb.num_cmds - 1;
	ctrl->cmd_size = ccb.cmd_size;
}


static status_t
pvr_ccb_init(const pvr_fwif_env& env, pvr_fwif_state* state,
	pvr_fwif_ccb* ccb, const char* ctrlName, const char* ringName,
	uint32 numCmdsLog2, size_t cmdSize)
{
	ccb->num_cmds = 1u << numCmdsLog2;
	ccb->cmd_size = cmdSize;

	pvr_fwif_range* ctrl;
	status_t status = fw_object_create(env, state, ctrlName,
		sizeof(rogue_fwif_ccb_ctl), PVR_FWIF_ANY_OFFSET, &ctrl);
	if (status != B_OK)
		return status;
	ccb->ctrl = (rogue_fwif_ccb_ctl*)ctrl->cpu;
	ccb->ctrl_phys = ctrl->phys;
	ccb->ctrl_fw_addr = ctrl->fw_addr;
	ccb_ctrl_init(ccb->ctrl, *ccb);

	pvr_fwif_range* ring;
	status = fw_object_create(env, state, ringName, ccb->num_cmds * cmdSize,
		PVR_FWIF_ANY_OFFSET, &ring);
	if (status != B_OK)
		return status;
	ccb->ring = ring->cpu;
	ccb->ring_phys = ring->phys;
	ccb->ring_fw_addr = ring->fw_addr;

	return B_OK;
}


static status_t
pvr_kccb_init(const pvr_fwif_env& env, pvr_fwif_state* state)
{
	const uint32 numSlotsLog2 = ROGUE_FWIF_KCCB_NUMCMDS_LOG2_DEFAULT;
	const uint32 numSlots = 1u << numSlotsLog2;

	status_t status = pvr_ccb_init(env, state, &state->kccb, "kccb ctrl",
		"kccb", numSlotsLog2, sizeof(rogue_fwif_kccb_cmd));
	if (status != B_OK)
		return status;

	// KCCB return slots
	pvr_fwif_range* rtn;
	status = fw_object_create(env, state, "kccb rtn slots",
		numSlots * sizeof(u32), PVR_FWIF_ANY_OFFSET, &rtn);
	if (status != B_OK)
		return status;
	state->kccb_rtn = (uint32*)rtn->cpu;
	state->kccb_rtn_phys = rtn->phys;
	state->kccb_rtn_fw_addr = rtn->fw_addr;

	return B_OK;
}


static status_t
pvr_fwccb_init(const pvr_fwif_env& env, pvr_fwif_state* state)
{
	return pvr_ccb_init(env, state, &state->fwccb, "fwccb ctrl", "fwccb",
		ROGUE_FWIF_FWCCB_NUMCMDS_LOG2, sizeof(rogue_fwif_fwccb_cmd));
}


//	#pragma mark - pvr_fw_trace.c


static inline u32
build_log_type(const u32 groupMask)
{
	if (groupMask == 0)
		return ROGUE_FWIF_LOG_TYPE_NONE;

	return groupMask | ROGUE_FWIF_LOG_TYPE_TRACE;
}


static status_t
pvr_fw_trace_init(const pvr_fwif_env& env, pvr_fwif_state* state)
{
	if ((env.trace_group_mask & ~ROGUE_FWIF_LOG_TYPE_GROUP_MASK) != 0) {
		LOG("pvr_fwif: invalid fw_trace group mask %#" B_PRIx32 "\n",
			env.trace_group_mask);
		return B_BAD_VALUE;
	}

	static const char* const kNames[PVR_FWIF_TRACE_BUFFERS] = {
		"trace buffer 0", "trace buffer 1"
	};
	uint32 bufferFwAddr[PVR_FWIF_TRACE_BUFFERS];
	for (uint32 thread = 0; thread < PVR_FWIF_TRACE_BUFFERS; thread++) {
		pvr_fwif_range* buffer;
		status_t status = fw_object_create(env, state, kNames[thread],
			ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS * sizeof(u32),
			PVR_FWIF_ANY_OFFSET, &buffer);
		if (status != B_OK)
			return status;
		state->trace_buffers[thread] = (uint32*)buffer->cpu;
		bufferFwAddr[thread] = buffer->fw_addr;
	}

	pvr_fwif_range* ctrl;
	status_t status = fw_object_create(env, state, "tracebuf ctrl",
		sizeof(rogue_fwif_tracebuf), PVR_FWIF_ANY_OFFSET, &ctrl);
	if (status != B_OK)
		return status;

	// tracebuf_ctrl_init()
	rogue_fwif_tracebuf* tracebufCtrl = (rogue_fwif_tracebuf*)ctrl->cpu;
	static_assert(ARRAY_SIZE(tracebufCtrl->tracebuf) == PVR_FWIF_TRACE_BUFFERS,
		"one trace buffer per firmware thread");
	tracebufCtrl->tracebuf_size_in_dwords
		= ROGUE_FW_TRACE_BUF_DEFAULT_SIZE_IN_DWORDS;
	tracebufCtrl->tracebuf_flags = 0;
	tracebufCtrl->log_type = build_log_type(env.trace_group_mask);
	for (uint32 thread = 0; thread < PVR_FWIF_TRACE_BUFFERS; thread++) {
		tracebufCtrl->tracebuf[thread].trace_buffer_fw_addr
			= bufferFwAddr[thread];
		tracebufCtrl->tracebuf[thread].trace_pointer = 0;
	}
	state->tracebuf_ctrl = tracebufCtrl;

	return B_OK;
}


//	#pragma mark - pvr_fw.c


static void
fw_fault_page_init(u32* faultPage)
{
	for (uint32 i = 0; i < PVR_ROGUE_FAULT_PAGE_SIZE / sizeof(*faultPage); i++)
		faultPage[i] = 0xdeadbee0;
}


static void
fw_sysdata_init(rogue_fwif_sysdata* sysdata, const pvr_fwif_env& env)
{
	u32 configFlags = 0;
	if (env.slc_size_in_kilobytes < ROGUE_FWIF_SLC_MIN_SIZE_FOR_DM_OVERLAP_KB)
		configFlags |= ROGUE_FWIF_INICFG_DISABLE_DM_OVERLAP;

	sysdata->config_flags = configFlags;
}


static void
fw_runtime_cfg_init(rogue_fwif_runtime_cfg* runtimeCfg,
	const pvr_fwif_env& env)
{
	runtimeCfg->core_clock_speed = env.core_clock_hz;
	runtimeCfg->active_pm_latency_ms = 0;
	runtimeCfg->active_pm_latency_persistant = true;
	runtimeCfg->default_dusts_num_init = env.num_clusters;

	// keep the watchdog timer disabled
	runtimeCfg->wdg_period_us = 0;
}


static void
fw_gpu_util_fwcb_init(rogue_fwif_gpu_util_fwcb* gpuUtilFwcb)
{
	gpuUtilFwcb->last_word = PVR_FWIF_GPU_UTIL_STATE_IDLE;
}


static void
fw_osdata_init(rogue_fwif_osdata* osdata, const pvr_fwif_state* state)
{
	osdata->power_sync_fw_addr = state->power_sync_fw_addr;
}


static void
fw_osinit_init(rogue_fwif_osinit* osinit, const pvr_fwif_state* state,
	uint32 hwrinfobufFwAddr, uint32 osdataFwAddr)
{
	osinit->kernel_ccbctl_fw_addr = state->kccb.ctrl_fw_addr;
	osinit->kernel_ccb_fw_addr = state->kccb.ring_fw_addr;
	osinit->kernel_ccb_rtn_slots_fw_addr = state->kccb_rtn_fw_addr;

	osinit->firmware_ccbctl_fw_addr = state->fwccb.ctrl_fw_addr;
	osinit->firmware_ccb_fw_addr = state->fwccb.ring_fw_addr;

	osinit->work_est_firmware_ccbctl_fw_addr = 0;
	osinit->work_est_firmware_ccb_fw_addr = 0;

	osinit->rogue_fwif_hwr_info_buf_ctl_fw_addr = hwrinfobufFwAddr;
	osinit->fw_os_data_fw_addr = osdataFwAddr;

	osinit->hwr_debug_dump_limit = 0;

	rogue_fwif_compchecks_bvnc_init(&osinit->rogue_comp_checks.hw_bvnc);
	rogue_fwif_compchecks_bvnc_init(&osinit->rogue_comp_checks.fw_bvnc);
}


static void
fw_sysinit_init(rogue_fwif_sysinit* sysinit, const pvr_fwif_env& env,
	const pvr_fwif_state* state, uint32 runtimeCfgFwAddr,
	uint32 tracebufCtrlFwAddr, uint32 sysdataFwAddr,
	uint32 gpuUtilFwcbFwAddr)
{
	sysinit->fault_phys_addr = state->fault_page_phys;

	sysinit->pds_exec_base = ROGUE_PDSCODEDATA_HEAP_BASE;
	sysinit->usc_exec_base = ROGUE_USCCODE_HEAP_BASE;

	sysinit->runtime_cfg_fw_addr = runtimeCfgFwAddr;
	sysinit->trace_buf_ctl_fw_addr = tracebufCtrlFwAddr;
	sysinit->fw_sys_data_fw_addr = sysdataFwAddr;
	sysinit->gpu_util_fw_cb_ctl_fw_addr = gpuUtilFwcbFwAddr;
	// no core memory data section with a MIPS firmware: coremem_data_store
	// stays zero

	// currently unsupported
	sysinit->counter_dump_ctl.buffer_fw_addr = 0;
	sysinit->counter_dump_ctl.size_in_dwords = 0;

	// skip alignment checks
	sysinit->align_checks = 0;

	sysinit->filter_flags = 0;
	sysinit->hw_perf_filter = 0;
	sysinit->firmware_perf = FW_PERF_CONF_NONE;
	sysinit->initial_core_clock_speed = env.core_clock_hz;
	sysinit->active_pm_latency_ms = 0;
	sysinit->gpio_validation_mode = ROGUE_FWIF_GPIO_VAL_OFF;
	sysinit->firmware_started = false;
	sysinit->marker_val = 1;

	memset(&sysinit->bvnc_km_feature_flags, 0,
		sizeof(sysinit->bvnc_km_feature_flags));
}


static status_t
pvr_fw_create_structures(const pvr_fwif_env& env, pvr_fwif_state* state)
{
	pvr_fwif_range* connectionCtl;
	status_t status = fw_object_create(env, state, "connection ctl",
		sizeof(rogue_fwif_connection_ctl), PVR_FWIF_CONNECTION_CTL_OFFSET,
		&connectionCtl);
	if (status != B_OK)
		return status;
	state->connection_ctl = (rogue_fwif_connection_ctl*)connectionCtl->cpu;
	state->connection_ctl_phys = connectionCtl->phys;
	state->connection_ctl_fw_addr = connectionCtl->fw_addr;

	pvr_fwif_range* powerSync;
	status = fw_object_create(env, state, "power sync", sizeof(u32),
		PVR_FWIF_ANY_OFFSET, &powerSync);
	if (status != B_OK)
		return status;
	state->power_sync = (uint32*)powerSync->cpu;
	state->power_sync_fw_addr = powerSync->fw_addr;

	pvr_fwif_range* hwrinfobuf;
	status = fw_object_create(env, state, "hwrinfobuf",
		sizeof(rogue_fwif_hwrinfobuf), PVR_FWIF_ANY_OFFSET, &hwrinfobuf);
	if (status != B_OK)
		return status;
	state->hwrinfobuf = (rogue_fwif_hwrinfobuf*)hwrinfobuf->cpu;

	pvr_fwif_range* mmucacheSync;
	status = fw_object_create(env, state, "mmucache sync", PVR_SYNC_OBJ_SIZE,
		PVR_FWIF_ANY_OFFSET, &mmucacheSync);
	if (status != B_OK)
		return status;
	state->mmucache_sync = (uint32*)mmucacheSync->cpu;
	state->mmucache_sync_fw_addr = mmucacheSync->fw_addr;

	pvr_fwif_range* sysdata;
	status = fw_object_create(env, state, "sysdata",
		sizeof(rogue_fwif_sysdata), PVR_FWIF_ANY_OFFSET, &sysdata);
	if (status != B_OK)
		return status;
	state->sysdata = (rogue_fwif_sysdata*)sysdata->cpu;
	fw_sysdata_init(state->sysdata, env);

	pvr_fwif_range* faultPage;
	status = fw_object_create(env, state, "fault page",
		PVR_ROGUE_FAULT_PAGE_SIZE, PVR_FWIF_ANY_OFFSET, &faultPage);
	if (status != B_OK)
		return status;
	fw_fault_page_init((u32*)faultPage->cpu);
	state->fault_page_phys = faultPage->phys;

	pvr_fwif_range* gpuUtilFwcb;
	status = fw_object_create(env, state, "gpu util fwcb",
		sizeof(rogue_fwif_gpu_util_fwcb), PVR_FWIF_ANY_OFFSET, &gpuUtilFwcb);
	if (status != B_OK)
		return status;
	state->gpu_util_fwcb = (rogue_fwif_gpu_util_fwcb*)gpuUtilFwcb->cpu;
	fw_gpu_util_fwcb_init(state->gpu_util_fwcb);

	pvr_fwif_range* runtimeCfg;
	status = fw_object_create(env, state, "runtime cfg",
		sizeof(rogue_fwif_runtime_cfg), PVR_FWIF_ANY_OFFSET, &runtimeCfg);
	if (status != B_OK)
		return status;
	state->runtime_cfg = (rogue_fwif_runtime_cfg*)runtimeCfg->cpu;
	fw_runtime_cfg_init(state->runtime_cfg, env);

	status = pvr_fw_trace_init(env, state);
	if (status != B_OK)
		return status;
	// the tracebuf ctrl is the last range pvr_fw_trace_init() created
	uint32 tracebufCtrlFwAddr
		= state->ranges[state->range_count - 1].fw_addr;

	pvr_fwif_range* osdata;
	status = fw_object_create(env, state, "osdata",
		sizeof(rogue_fwif_osdata), PVR_FWIF_ANY_OFFSET, &osdata);
	if (status != B_OK)
		return status;
	state->osdata = (rogue_fwif_osdata*)osdata->cpu;
	fw_osdata_init(state->osdata, state);

	pvr_fwif_range* osinit;
	status = fw_object_create(env, state, "osinit",
		sizeof(rogue_fwif_osinit), PVR_FWIF_OSINIT_OFFSET, &osinit);
	if (status != B_OK)
		return status;
	state->osinit = (rogue_fwif_osinit*)osinit->cpu;
	state->osinit_phys = osinit->phys;
	state->osinit_fw_addr = osinit->fw_addr;
	fw_osinit_init(state->osinit, state, hwrinfobuf->fw_addr,
		osdata->fw_addr);

	pvr_fwif_range* sysinit;
	status = fw_object_create(env, state, "sysinit",
		sizeof(rogue_fwif_sysinit), PVR_FWIF_SYSINIT_OFFSET, &sysinit);
	if (status != B_OK)
		return status;
	state->sysinit = (rogue_fwif_sysinit*)sysinit->cpu;
	state->sysinit_phys = sysinit->phys;
	state->sysinit_fw_addr = sysinit->fw_addr;
	fw_sysinit_init(state->sysinit, env, state, runtimeCfg->fw_addr,
		tracebufCtrlFwAddr, sysdata->fw_addr, gpuUtilFwcb->fw_addr);

	return B_OK;
}


//	#pragma mark - public


status_t
pvr_fwif_create(const pvr_fwif_env& env, pvr_fwif_state* state)
{
	memset(state, 0, sizeof(*state));

	if (env.alloc == NULL)
		return B_BAD_VALUE;
	if (env.core_clock_hz == 0)
		LOG("pvr_fwif: warning: core clock speed is 0\n");

	// same order as pvr_fw_init(), after pvr_fw_process()
	status_t status = pvr_kccb_init(env, state);
	if (status == B_OK)
		status = pvr_fwccb_init(env, state);
	if (status == B_OK)
		status = pvr_fw_create_structures(env, state);
	if (status != B_OK) {
		LOG("pvr_fwif: creation failed after %" B_PRIu32 " objects\n",
			state->range_count);
		return status;
	}

	LOG("pvr_fwif: %" B_PRIu32 " objects, sysinit at fw %#" B_PRIx32 "\n",
		state->range_count, state->sysinit_fw_addr);
	return B_OK;
}


bool
pvr_fwif_firmware_started(const pvr_fwif_state* state)
{
	if (state == NULL || state->sysinit == NULL)
		return false;

	// pvr_wait_for_fw_boot(): READ_ONCE(fwif_sysinit->firmware_started)
	return *(volatile const uint8*)&state->sysinit->firmware_started != 0;
}
