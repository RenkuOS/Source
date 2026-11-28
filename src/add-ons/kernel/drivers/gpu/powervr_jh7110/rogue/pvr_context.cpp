/*
 * Copyright 2026, The RenkuOS authors.
 * Copyright (c) 2023 Imagination Technologies Ltd.
 * SPDX-License-Identifier: GPL-2.0-only OR MIT
 *
 * Port of the context, client CCB and KCCB handling of the Linux
 * drm/imagination driver: pvr_vm.c (fw_mem_context_init()), pvr_context.c,
 * pvr_queue.c (pvr_queue_create(), init_fw_context(),
 * pvr_queue_submit_job_to_cccb()), pvr_cccb.c, pvr_ccb.c
 * (pvr_kccb_send_cmd_reserved_powered(), pvr_kccb_wait_for_completion()),
 * pvr_mmu.c (pvr_mmu_flush_exec()) and pvr_fw.c
 * (pvr_fw_structure_cleanup()), without the DRM scheduler and fences.
 */


#include "pvr_context.h"

#include <string.h>

#include <KernelExport.h>
#include <OS.h>


#define LOG(env, format, ...) \
	do { \
		if ((env).log != NULL) \
			(env).log((env).cookie, format, ##__VA_ARGS__); \
	} while (false)

#define PADDING_COMMAND_SIZE	sizeof(struct rogue_fwif_ccb_cmd_header)
#define KCCB_TIMEOUT			1000000	/* HZ */
#define CACHE_LINE				64

static_assert(sizeof(struct rogue_fwif_kccb_cmd)
	== 8 + PVR_FWIF_CCB_CMD_DATA_SIZE, "KCCB command layout");


static inline void
memory_barrier()
{
	__asm__ __volatile__("fence rw, rw" ::: "memory");
}


static inline uint32
align8(uint32 size)
{
	return (size + 7) & ~(uint32)7;
}


static void
sync_obj(const pvr_ctx_env& env, const pvr_fw_obj& obj)
{
	if (obj.cpu != NULL)
		env.sync(env.cookie, obj.phys, obj.size);
}


static status_t
fw_object_create(const pvr_ctx_env& env, const char* name, size_t size,
	pvr_fw_obj* obj)
{
	size_t allocSize = (size + PVR_FWIF_PAGE_SIZE - 1)
		& ~(size_t)(PVR_FWIF_PAGE_SIZE - 1);

	memset(obj, 0, sizeof(*obj));
	status_t status = env.fw_alloc(env.cookie, allocSize, PVR_FWIF_PAGE_SIZE,
		PVR_FWIF_ANY_OFFSET, &obj->cpu, &obj->phys, &obj->fw_addr);
	if (status != B_OK) {
		LOG(env, "pvr_ctx: cannot allocate %s (%#zx bytes): %#" B_PRIx32 "\n",
			name, allocSize, (uint32)status);
		obj->cpu = NULL;
		return status;
	}
	obj->name = name;
	obj->size = allocSize;

	if (obj->cpu == NULL
		|| (obj->fw_addr & ~PVR_FWIF_HEAP_OFFSET_MASK)
			!= PVR_FWIF_MIPS_HEAP_BASE) {
		LOG(env, "pvr_ctx: %s got an unexpected address (fw %#" B_PRIx32
			")\n", name, obj->fw_addr);
		return B_BAD_VALUE;
	}

	// pvr_gem_object_create() returns zeroed memory
	memset(obj->cpu, 0, allocSize);

	LOG(env, "pvr_ctx: %-16s fw %#010" B_PRIx32 " phys %#" B_PRIx64
		" size %#zx\n", name, obj->fw_addr, obj->phys, allocSize);
	return B_OK;
}


static void
fw_object_destroy(const pvr_ctx_env& env, pvr_fw_obj* obj)
{
	if (obj->cpu != NULL && env.fw_free != NULL)
		env.fw_free(env.cookie, obj->cpu, obj->phys, obj->fw_addr, obj->size);
	memset(obj, 0, sizeof(*obj));
}


static const pvr_fwif_range*
fwif_find_range(const pvr_fwif_state* fwif, const void* cpu)
{
	for (uint32 i = 0; i < fwif->range_count; i++) {
		if (fwif->ranges[i].cpu == cpu)
			return &fwif->ranges[i];
	}
	return NULL;
}


//	#pragma mark - KCCB (pvr_ccb.c)


status_t
pvr_kccb_send_cmd(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	const struct rogue_fwif_kccb_cmd* cmd, uint32* _slot)
{
	pvr_fwif_ccb& kccb = fwif->kccb;
	rogue_fwif_ccb_ctl* ctrl = kccb.ctrl;
	volatile rogue_fwif_ccb_ctl* vctrl = ctrl;

	// the read offset is written by the firmware, in its own cache line
	env.sync(env.cookie,
		kccb.ctrl_phys + offsetof(rogue_fwif_ccb_ctl, read_offset),
		CACHE_LINE);

	// pvr_ccb_slot_available_locked()
	uint32 oldWriteOffset = vctrl->write_offset;
	uint32 newWriteOffset = (oldWriteOffset + 1) & vctrl->wrap_mask;
	if (newWriteOffset == vctrl->read_offset)
		return B_BUSY;

	// pvr_kccb_send_cmd_reserved_powered()
	uint8* slotAddress = (uint8*)kccb.ring + oldWriteOffset * kccb.cmd_size;
	memcpy(slotAddress, cmd, sizeof(*cmd));

	// the return slots are written by the firmware: drop stale lines first
	uint64 rtnPhys = fwif->kccb_rtn_phys + oldWriteOffset * sizeof(uint32);
	env.sync(env.cookie, rtnPhys, sizeof(uint32));
	((volatile uint32*)fwif->kccb_rtn)[oldWriteOffset]
		= ROGUE_FWIF_KCCB_RTN_SLOT_NO_RESPONSE;

	memory_barrier();
	env.sync(env.cookie, kccb.ring_phys + oldWriteOffset * kccb.cmd_size,
		kccb.cmd_size);
	env.sync(env.cookie, rtnPhys, sizeof(uint32));

	vctrl->write_offset = newWriteOffset;
	memory_barrier();
	env.sync(env.cookie, kccb.ctrl_phys, sizeof(uint32));

	// pvr_fw_mts_schedule(PVR_FWIF_DM_GP & ~ROGUE_CR_MTS_SCHEDULE_DM_CLRMSK)
	memory_barrier();
	env.mts_schedule(env.cookie, PVR_MTS_SCHEDULE_KCCB);

	if (_slot != NULL)
		*_slot = oldWriteOffset;
	return B_OK;
}


bool
pvr_kccb_slot_executed(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	uint32 slot, uint32* _rtn)
{
	env.sync(env.cookie, fwif->kccb_rtn_phys + slot * sizeof(uint32),
		sizeof(uint32));
	uint32 rtn = ((volatile uint32*)fwif->kccb_rtn)[slot];
	if (_rtn != NULL)
		*_rtn = rtn;
	return (rtn & ROGUE_FWIF_KCCB_RTN_SLOT_CMD_EXECUTED) != 0;
}


status_t
pvr_kccb_wait_for_completion(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	uint32 slot, bigtime_t timeout, uint32* _rtn)
{
	bigtime_t start = system_time();
	while (true) {
		if (pvr_kccb_slot_executed(env, fwif, slot, _rtn))
			return B_OK;
		if (system_time() - start >= timeout)
			return B_TIMED_OUT;
		snooze(100);
	}
}


//	#pragma mark - MMU cache (pvr_mmu.c)


status_t
pvr_mmu_flush_exec(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_mmu_context* mmu, uint32 extraFlags, uint32* _slot, uint32* _rtn)
{
	rogue_fwif_kccb_cmd cmd;
	memset(&cmd, 0, sizeof(cmd));
	rogue_fwif_mmucachedata* data
		= (rogue_fwif_mmucachedata*)cmd.cmd_data.raw;

	data->cache_flags = extraFlags;
	if (mmu != NULL)
		data->cache_flags |= pvr_mmu_take_flush_flags(mmu);
	if (data->cache_flags == 0)
		return B_OK;

	cmd.cmd_type = ROGUE_FWIF_KCCB_CMD_MMUCACHE;
	data->mmu_cache_sync_fw_addr = fwif->mmucache_sync_fw_addr;
	data->mmu_cache_sync_update_value = 0;

	uint32 slot;
	status_t status = pvr_kccb_send_cmd(env, fwif, &cmd, &slot);
	if (status != B_OK)
		return status;
	if (_slot != NULL)
		*_slot = slot;

	status = pvr_kccb_wait_for_completion(env, fwif, slot, KCCB_TIMEOUT,
		_rtn);
	LOG(env, "pvr_ctx: MMUCACHE %#" B_PRIx32 " in slot %" B_PRIu32 ": %s\n",
		data->cache_flags, slot, status == B_OK ? "done" : "timed out");
	return status;
}


//	#pragma mark - contexts


status_t
pvr_fw_mem_context_create(const pvr_ctx_env& env, pvr_mmu_context* mmu,
	pvr_fw_mem_context* memContext)
{
	memset(memContext, 0, sizeof(*memContext));
	if (env.fw_alloc == NULL || env.sync == NULL || mmu == NULL)
		return B_BAD_VALUE;
	memContext->mmu = mmu;

	// pvr_vm_create_context(): PVR_BO_FW_FLAGS_DEVICE_UNCACHED
	status_t status = fw_object_create(env, "fw mem context",
		sizeof(rogue_fwif_fwmemcontext), &memContext->obj);
	if (status != B_OK)
		return status;

	// fw_mem_context_init()
	rogue_fwif_fwmemcontext* fw = (rogue_fwif_fwmemcontext*)memContext->obj.cpu;
	fw->pc_dev_paddr = pvr_mmu_get_root_table_phys(mmu);
	fw->page_cat_base_reg_set = ROGUE_FW_BIF_INVALID_PCSET;
	memContext->fw = fw;

	memory_barrier();
	sync_obj(env, memContext->obj);
	return B_OK;
}


void
pvr_fw_mem_context_destroy(const pvr_ctx_env& env,
	pvr_fw_mem_context* memContext)
{
	fw_object_destroy(env, &memContext->obj);
	memContext->fw = NULL;
}


static pvr_fw_obj*
context_add_obj(pvr_gpu_context* context, uint32 index)
{
	if (context->obj_count != index)
		return NULL;
	return &context->objs[context->obj_count++];
}


status_t
pvr_gpu_context_create(const pvr_ctx_env& env, pvr_fw_mem_context* memContext,
	const pvr_ctx_params& params, pvr_gpu_context* context)
{
	memset(context, 0, sizeof(*context));
	if (env.fw_alloc == NULL || env.sync == NULL || env.mts_schedule == NULL
		|| memContext == NULL || memContext->fw == NULL
		|| params.priority > PVR_CTX_PRIORITY_HIGH)
		return B_BAD_VALUE;

	uint32 cccbSizeLog2 = params.cccb_size_log2 != 0
		? params.cccb_size_log2 : PVR_CTX_CCCB_SIZE_LOG2_LINUX;
	if (cccbSizeLog2 < 8 || cccbSizeLog2 > 16)
		return B_BAD_VALUE;

	// get_fw_obj_size(), get_ctx_state_size(), get_dm_type()
	size_t fwContextSize;
	size_t stateSize;
	switch (params.type) {
		case PVR_CTX_TYPE_TRANSFER_FRAG:
		{
			uint32 isp = params.num_isp_store_registers != 0
				? params.num_isp_store_registers : 1;
			fwContextSize = sizeof(rogue_fwif_fwtransfercontext);
			// struct_size_t(rogue_fwif_frag_ctx_state, frag_reg_isp_store,
			// num_isp_store_registers)
			stateSize = offsetof(rogue_fwif_frag_ctx_state,
				frag_reg_isp_store) + isp * sizeof(u32);
			context->dm = PVR_FWIF_DM_FRAG;
			context->ctx_offset
				= offsetof(rogue_fwif_fwtransfercontext, tq_context);
			break;
		}
		case PVR_CTX_TYPE_COMPUTE:
			fwContextSize = sizeof(rogue_fwif_fwcomputecontext);
			stateSize = sizeof(rogue_fwif_compute_ctx_state);
			context->dm = PVR_FWIF_DM_CDM;
			context->ctx_offset
				= offsetof(rogue_fwif_fwcomputecontext, cdm_context);
			break;
		default:
			return B_BAD_VALUE;
	}
	context->type = params.type;
	context->mem = memContext;

	// pvr_queue_create() -> pvr_cccb_init()
	context->cccb_size = 1u << cccbSizeLog2;
	context->cccb_write_offset = 0;
	context->cccb_wrap_mask = context->cccb_size - 1;

	pvr_fw_obj* obj = context_add_obj(context, PVR_CTX_OBJ_CCCB_CTRL);
	status_t status = fw_object_create(env, "cccb ctrl",
		sizeof(rogue_fwif_cccb_ctl), obj);
	if (status != B_OK)
		return status;
	// cccb_ctrl_init()
	context->cccb_ctrl = (rogue_fwif_cccb_ctl*)obj->cpu;
	context->cccb_ctrl->write_offset = 0;
	context->cccb_ctrl->read_offset = 0;
	context->cccb_ctrl->dep_offset = 0;
	context->cccb_ctrl->wrap_mask = context->cccb_wrap_mask;

	obj = context_add_obj(context, PVR_CTX_OBJ_CCCB);
	status = fw_object_create(env, "cccb", context->cccb_size, obj);
	if (status != B_OK)
		return status;
	context->cccb = (uint8*)obj->cpu;

	// context state (queue->reg_state_obj); reg_state_init() only fills
	// geometry state
	obj = context_add_obj(context, PVR_CTX_OBJ_REG_STATE);
	status = fw_object_create(env, "ctx state", stateSize, obj);
	if (status != B_OK)
		return status;
	uint32 stateFwAddr = obj->fw_addr;

	// queue->timeline_ufo
	obj = context_add_obj(context, PVR_CTX_OBJ_UFO);
	status = fw_object_create(env, "timeline ufo", sizeof(u32), obj);
	if (status != B_OK)
		return status;
	context->ufo = (volatile uint32*)obj->cpu;
	context->ufo_fw_addr = obj->fw_addr;

	// pvr_context_create(): the FW context object, whose content Linux
	// builds in ctx->data (init_fw_context(), init_fw_objs()) and copies
	obj = context_add_obj(context, PVR_CTX_OBJ_FW_CONTEXT);
	status = fw_object_create(env,
		params.type == PVR_CTX_TYPE_COMPUTE ? "compute ctx" : "transfer ctx",
		fwContextSize, obj);
	if (status != B_OK)
		return status;

	// init_fw_context()
	rogue_fwif_fwcommoncontext* common = (rogue_fwif_fwcommoncontext*)
		((uint8*)obj->cpu + context->ctx_offset);
	common->ccbctl_fw_addr = context->objs[PVR_CTX_OBJ_CCCB_CTRL].fw_addr;
	common->ccb_fw_addr = context->objs[PVR_CTX_OBJ_CCCB].fw_addr;
	common->dm = context->dm;
	common->priority = params.priority;
	common->priority_seq_num = 0;
	common->max_deadline_ms = PVR_CTX_MAX_DEADLINE_MS;
	common->pid = params.pid;
	common->server_common_context_id = params.ctx_id;
	common->fw_mem_context_fw_addr = memContext->obj.fw_addr;
	common->context_state_addr = stateFwAddr;

	// init_compute_fw_objs(): Linux copies the static CDM context switch
	// state from userspace; it stays zero here. init_transfer_fw_objs():
	// nothing.

	context->fw_common = common;
	context->fw_common_addr = obj->fw_addr + context->ctx_offset;

	memory_barrier();
	for (uint32 i = 0; i < context->obj_count; i++)
		sync_obj(env, context->objs[i]);

	LOG(env, "pvr_ctx: %s context at fw %#" B_PRIx32 ", DM %" B_PRIu32
		", CCCB %" B_PRIu32 " bytes\n",
		params.type == PVR_CTX_TYPE_COMPUTE ? "compute" : "transfer",
		context->fw_common_addr, context->dm, context->cccb_size);
	return B_OK;
}


/* get_ccb_space() */
static inline uint32
get_ccb_space(uint32 writeOffset, uint32 readOffset, uint32 ccbSize)
{
	return ((readOffset - writeOffset) + (ccbSize - 1)) & (ccbSize - 1);
}


status_t
pvr_cccb_write_command(const pvr_ctx_env& env, pvr_gpu_context* context,
	uint32 cmdType, const void* data, uint32 size, uint32 extJobRef,
	uint32 intJobRef)
{
	// pvr_cccb_write_command_with_header()
	uint32 sizeWithHeader = sizeof(rogue_fwif_ccb_cmd_header) + align8(size);
	rogue_fwif_ccb_cmd_header header;
	memset(&header, 0, sizeof(header));
	header.cmd_type = cmdType;
	header.cmd_size = align8(size);
	header.ext_job_ref = extJobRef;
	header.int_job_ref = intJobRef;

	uint32 remaining = context->cccb_size - context->cccb_write_offset;
	uint32 requiredSize = sizeWithHeader;
	if (remaining < sizeWithHeader + PADDING_COMMAND_SIZE)
		requiredSize = sizeWithHeader + remaining;

	env.sync(env.cookie, context->objs[PVR_CTX_OBJ_CCCB_CTRL].phys,
		sizeof(rogue_fwif_cccb_ctl));
	uint32 readOffset
		= ((volatile rogue_fwif_cccb_ctl*)context->cccb_ctrl)->read_offset;
	uint32 space = get_ccb_space(context->cccb_write_offset, readOffset,
		context->cccb_size);
	if (space < requiredSize) {
		LOG(env, "pvr_ctx: CCCB full (write %#" B_PRIx32 ", read %#"
			B_PRIx32 ")\n", context->cccb_write_offset, readOffset);
		return B_BUSY;
	}

	if (requiredSize != sizeWithHeader) {
		// padding command up to the end of the CCCB
		rogue_fwif_ccb_cmd_header padding;
		memset(&padding, 0, sizeof(padding));
		padding.cmd_type = ROGUE_FWIF_CCB_CMD_TYPE_PADDING;
		padding.cmd_size = remaining - sizeof(padding);
		memcpy(context->cccb + context->cccb_write_offset, &padding,
			sizeof(padding));
		context->cccb_write_offset = 0;
	}

	uint8* target = context->cccb + context->cccb_write_offset;
	memcpy(target, &header, sizeof(header));
	if (size > 0)
		memcpy(target + sizeof(header), data, size);
	context->cccb_write_offset += sizeWithHeader;
	// room for a padding command is always kept, so the offset never
	// reaches cccb_size; the mask is only defensive
	context->cccb_write_offset &= context->cccb_wrap_mask;
	return B_OK;
}


status_t
pvr_gpu_context_kick(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_gpu_context* context, uint32* _kccbSlot)
{
	// "Make sure the writes to the CCCB are flushed before sending the KICK"
	memory_barrier();
	sync_obj(env, context->objs[PVR_CTX_OBJ_CCCB]);

	// pvr_cccb_send_kccb_kick() -> fill_cmd_kick_data() (no HWRT)
	rogue_fwif_kccb_cmd cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_type = ROGUE_FWIF_KCCB_CMD_KICK;
	rogue_fwif_kccb_cmd_kick_data* kick
		= (rogue_fwif_kccb_cmd_kick_data*)cmd.cmd_data.raw;
	kick->context_fw_addr = context->fw_common_addr;
	kick->client_woff_update = context->cccb_write_offset;
	kick->client_wrap_mask_update = context->cccb_wrap_mask;
	kick->num_cleanup_ctl = 0;
	kick->work_est_cmd_header_offset = 0;

	uint32 slot;
	status_t status = pvr_kccb_send_cmd(env, fwif, &cmd, &slot);
	if (status == B_OK) {
		LOG(env, "pvr_ctx: KICK ctx fw %#" B_PRIx32 " woff %#" B_PRIx32
			" in KCCB slot %" B_PRIu32 "\n", kick->context_fw_addr,
			kick->client_woff_update, slot);
		if (_kccbSlot != NULL)
			*_kccbSlot = slot;
	}
	return status;
}


static status_t
write_update(const pvr_ctx_env& env, pvr_gpu_context* context, uint32 value)
{
	// update command to signal the job fence
	rogue_fwif_ufo ufo;
	ufo.addr = context->ufo_fw_addr;
	ufo.value = value;
	return pvr_cccb_write_command(env, context,
		ROGUE_FWIF_CCB_CMD_TYPE_UPDATE, &ufo, sizeof(ufo), 0, 0);
}


status_t
pvr_gpu_context_submit_update(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_gpu_context* context, uint32 value, uint32 submitFlags,
	uint32* _kccbSlot)
{
	uint32 start = context->cccb_write_offset;
	status_t status = B_OK;
	if ((submitFlags & PVR_SUBMIT_WITH_NULL_COMMAND) != 0) {
		status = pvr_cccb_write_command(env, context,
			ROGUE_FWIF_CCB_CMD_TYPE_NULL, NULL, 0, 0, 0);
	}
	if (status == B_OK)
		status = write_update(env, context, value);
	if (status != B_OK) {
		context->cccb_write_offset = start;
		return status;
	}

	return pvr_gpu_context_kick(env, fwif, context, _kccbSlot);
}


status_t
pvr_gpu_context_submit_compute(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_gpu_context* context, const struct rogue_fwif_cmd_compute* cmd,
	uint32 value, uint32* _kccbSlot)
{
	if (context->type != PVR_CTX_TYPE_COMPUTE || cmd == NULL)
		return B_BAD_VALUE;

	// pvr_compute_job_fw_cmd_init()
	rogue_fwif_cmd_compute compute = *cmd;
	compute.common.frame_num = 0;

	uint32 start = context->cccb_write_offset;
	status_t status = pvr_cccb_write_command(env, context,
		ROGUE_FWIF_CCB_CMD_TYPE_CDM, &compute, sizeof(compute), 1, 1);
	if (status == B_OK)
		status = write_update(env, context, value);
	if (status != B_OK) {
		context->cccb_write_offset = start;
		return status;
	}

	return pvr_gpu_context_kick(env, fwif, context, _kccbSlot);
}


void
pvr_gpu_context_get_status(const pvr_ctx_env& env, pvr_gpu_context* context,
	pvr_gpu_context_status* status)
{
	memset(status, 0, sizeof(*status));
	sync_obj(env, context->objs[PVR_CTX_OBJ_CCCB_CTRL]);
	sync_obj(env, context->objs[PVR_CTX_OBJ_UFO]);
	sync_obj(env, context->objs[PVR_CTX_OBJ_FW_CONTEXT]);
	sync_obj(env, context->mem->obj);

	volatile rogue_fwif_cccb_ctl* ctrl = context->cccb_ctrl;
	status->cccb_write_offset = context->cccb_write_offset;
	status->cccb_ctrl_write_offset = ctrl->write_offset;
	status->cccb_read_offset = ctrl->read_offset;
	status->cccb_dep_offset = ctrl->dep_offset;
	status->ufo_value = *context->ufo;

	volatile rogue_fwif_fwmemcontext* mem = context->mem->fw;
	status->page_cat_base_reg_set = mem->page_cat_base_reg_set;
	status->pc_dev_paddr = mem->pc_dev_paddr;

	volatile rogue_fwif_fwcommoncontext* common = context->fw_common;
	status->fw_com_ctx_flags = common->fw_com_ctx_flags;
	status->last_failed_ufo_addr = common->last_failed_ufo.addr;
	status->last_failed_ufo_value = common->last_failed_ufo.value;
}


bool
pvr_gpu_context_done(const pvr_ctx_env& env, pvr_gpu_context* context,
	uint32 value)
{
	pvr_gpu_context_status status;
	pvr_gpu_context_get_status(env, context, &status);
	return status.cccb_read_offset == context->cccb_write_offset
		&& status.ufo_value == value;
}


status_t
pvr_gpu_context_cleanup(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_gpu_context* context, bigtime_t timeout)
{
	if (context->obj_count <= PVR_CTX_OBJ_FW_CONTEXT)
		return B_OK;

	// pvr_fw_structure_cleanup(ROGUE_FWIF_CLEANUP_FWCOMMONCONTEXT)
	rogue_fwif_kccb_cmd cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_type = ROGUE_FWIF_KCCB_CMD_CLEANUP;
	cmd.kccb_flags = 0;
	rogue_fwif_cleanup_request* request
		= (rogue_fwif_cleanup_request*)cmd.cmd_data.raw;
	request->cleanup_type = ROGUE_FWIF_CLEANUP_FWCOMMONCONTEXT;
	request->cleanup_data.context_fw_addr = context->fw_common_addr;

	bigtime_t start = system_time();
	while (true) {
		uint32 slot;
		uint32 rtn = 0;
		status_t status = pvr_kccb_send_cmd(env, fwif, &cmd, &slot);
		if (status == B_OK) {
			status = pvr_kccb_wait_for_completion(env, fwif, slot,
				KCCB_TIMEOUT, &rtn);
		}
		if (status == B_OK
			&& (rtn & ROGUE_FWIF_KCCB_RTN_SLOT_CLEANUP_BUSY) == 0) {
			LOG(env, "pvr_ctx: context fw %#" B_PRIx32 " cleaned up (rtn %#"
				B_PRIx32 ")\n", context->fw_common_addr, rtn);
			return B_OK;
		}
		if (status == B_OK)
			status = B_BUSY;
		if (status != B_BUSY || system_time() - start >= timeout) {
			LOG(env, "pvr_ctx: cleanup of context fw %#" B_PRIx32 " failed: %#"
				B_PRIx32 " (rtn %#" B_PRIx32 ")\n", context->fw_common_addr,
				(uint32)status, rtn);
			return status;
		}
		snooze(1000);
	}
}


void
pvr_gpu_context_destroy(const pvr_ctx_env& env, pvr_gpu_context* context)
{
	// pvr_queue_destroy(): timeline UFO, context state, pvr_cccb_fini()
	// (CCCB, then its control); pvr_context_release(): the FW context
	static const uint32 kOrder[] = {
		PVR_CTX_OBJ_UFO, PVR_CTX_OBJ_REG_STATE, PVR_CTX_OBJ_CCCB,
		PVR_CTX_OBJ_CCCB_CTRL, PVR_CTX_OBJ_FW_CONTEXT
	};
	for (uint32 i = 0; i < sizeof(kOrder) / sizeof(kOrder[0]); i++) {
		if (kOrder[i] < context->obj_count)
			fw_object_destroy(env, &context->objs[kOrder[i]]);
	}
	memset(context, 0, sizeof(*context));
}


//	#pragma mark - diagnostics


void
pvr_gpu_dump_mmu_state(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_fw_mem_context* memContext)
{
	uint64 pcPhys = 0;
	uint32 pcSet = ROGUE_FW_BIF_INVALID_PCSET;
	if (memContext != NULL && memContext->fw != NULL) {
		sync_obj(env, memContext->obj);
		volatile rogue_fwif_fwmemcontext* mem = memContext->fw;
		pcPhys = pvr_mmu_get_root_table_phys(memContext->mmu);
		pcSet = mem->page_cat_base_reg_set;
		LOG(env, "pvr_ctx: fw mem context: pc %#" B_PRIx64 " (ours %#"
			B_PRIx64 "), page_cat_base_reg_set %#" B_PRIx32 "\n",
			(uint64)mem->pc_dev_paddr, pcPhys, pcSet);
	}

	if (env.cr_read64 != NULL) {
		static const struct { uint32 offset; const char* name; } kRegs[] = {
			{ PVR_CR_MMU_FAULT_STATUS, "MMU_FAULT_STATUS" },
			{ PVR_CR_MMU_FAULT_STATUS_META, "MMU_FAULT_STATUS_META" },
			{ PVR_CR_BIF_FAULT_BANK0_MMU_STATUS, "BIF_FAULT_BANK0_MMU_STATUS" },
			{ PVR_CR_BIF_FAULT_BANK0_REQ_STATUS, "BIF_FAULT_BANK0_REQ_STATUS" },
			{ PVR_CR_BIF_FAULT_BANK1_MMU_STATUS, "BIF_FAULT_BANK1_MMU_STATUS" },
			{ PVR_CR_BIF_FAULT_BANK1_REQ_STATUS, "BIF_FAULT_BANK1_REQ_STATUS" },
			{ PVR_CR_BIF_FAULT_READ, "BIF_FAULT_READ" },
		};
		for (uint32 i = 0; i < sizeof(kRegs) / sizeof(kRegs[0]); i++) {
			LOG(env, "pvr_ctx: %-27s %#018" B_PRIx64 "\n", kRegs[i].name,
				env.cr_read64(env.cookie, kRegs[i].offset));
		}
		if (pcSet < PVR_CR_BIF_CAT_BASE_COUNT) {
			uint64 catBase = env.cr_read64(env.cookie,
				PVR_CR_BIF_CAT_BASE(pcSet));
			LOG(env, "pvr_ctx: BIF_CAT_BASE%" B_PRIu32 " %#" B_PRIx64 " (%s "
				"our page catalogue)\n", pcSet, catBase,
				(catBase & 0x000000FFFFFFF000ULL) == pcPhys ? "is" : "is not");
		}
	}

	// HWR info buffer: the firmware records MMU faults there
	const pvr_fwif_range* range = fwif_find_range(fwif, fwif->hwrinfobuf);
	if (range != NULL) {
		env.sync(env.cookie, range->phys, range->size);
		volatile rogue_fwif_hwrinfobuf* hwr = fwif->hwrinfobuf;
		uint32 counter = hwr->hwr_counter;
		uint32 writeIndex = hwr->write_index;
		LOG(env, "pvr_ctx: HWR counter %" B_PRIu32 ", write index %" B_PRIu32
			"\n", counter, writeIndex);
		if (counter != 0) {
			uint32 last = (writeIndex + ROGUE_FWIF_HWINFO_MAX - 1)
				% ROGUE_FWIF_HWINFO_MAX;
			volatile rogue_hwrinfo* info = &hwr->hwr_info[last];
			LOG(env, "pvr_ctx: last HWR: type %" B_PRIu32 ", HWR number %"
				B_PRIu32 ", event status %#" B_PRIx32 ", data %#" B_PRIx64
				" %#" B_PRIx64 " %#" B_PRIx64 "\n", (uint32)info->hwr_type,
				info->hwr_number, info->event_status,
				(uint64)info->hwr_data.mmu_info.mmu_status[0],
				(uint64)info->hwr_data.mmu_info.mmu_status[1],
				(uint64)info->hwr_data.mmu_info.pc_address);
		}
	}

	// FWCCB: commands the firmware sent to the host (context reset, page
	// fault notifications...), not consumed by this test driver
	pvr_fwif_ccb& fwccb = fwif->fwccb;
	if (fwccb.ctrl != NULL) {
		env.sync(env.cookie, fwccb.ctrl_phys, sizeof(rogue_fwif_ccb_ctl));
		env.sync(env.cookie, fwccb.ring_phys, fwccb.num_cmds * fwccb.cmd_size);
		volatile rogue_fwif_ccb_ctl* ctrl = fwccb.ctrl;
		uint32 write = ctrl->write_offset;
		uint32 read = ctrl->read_offset;
		LOG(env, "pvr_ctx: FWCCB write %" B_PRIu32 ", read %" B_PRIu32 "\n",
			write, read);
		for (uint32 i = read; i != write; i = (i + 1) & (fwccb.num_cmds - 1)) {
			const volatile uint32* words = (const volatile uint32*)
				((uint8*)fwccb.ring + i * fwccb.cmd_size);
			LOG(env, "pvr_ctx:   [%" B_PRIu32 "] type %#010" B_PRIx32 " data %#"
				B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32 "\n", i,
				words[0], words[2], words[3], words[4], words[5]);
		}
	}
}
