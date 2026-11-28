/*
 * Copyright 2026, The RenkuOS authors.
 * Copyright (c) 2023 Imagination Technologies Ltd.
 * SPDX-License-Identifier: GPL-2.0-only OR MIT
 *
 * Firmware side of a GPU context of the PowerVR Rogue firmware, ported from
 * the Linux drm/imagination driver:
 *  - firmware memory context (pvr_vm.c: pvr_vm_create_context(),
 *    fw_mem_context_init()),
 *  - transfer or compute context with its queue (pvr_context.c:
 *    pvr_context_create(), pvr_queue.c: pvr_queue_create(),
 *    init_fw_context(); pvr_cccb.c: pvr_cccb_init()),
 *  - client CCB commands and the KCCB kick (pvr_cccb.c:
 *    pvr_cccb_write_command_with_header(), pvr_cccb_send_kccb_kick();
 *    pvr_queue.c: pvr_queue_submit_job_to_cccb()),
 *  - KCCB send/wait, MMU cache flush and FW object cleanup (pvr_ccb.c:
 *    pvr_kccb_send_cmd_reserved_powered(), pvr_kccb_wait_for_completion();
 *    pvr_mmu.c: pvr_mmu_flush_exec(); pvr_fw.c: pvr_fw_structure_cleanup()).
 *
 * The firmware objects are allocated through the FW heap hook of
 * pvr_fwif_init.h, with the firmware running: Linux then requests a flush
 * of all MMU caches (pvr_vm_mips.c:199), executed before the next job
 * (pvr_job.c:742). The caller does the same with pvr_mmu_flush_exec().
 *
 * Host caches (non-coherent DMA). Linux maps the firmware objects
 * write-combined; here they are cached and synced through the sync hook.
 * A sync writes back whole cache lines, so the host must not write into a
 * line the firmware writes once that line has been handed to the firmware.
 * This module only writes such lines before the object is first used by
 * the firmware (creation), plus the KCCB slot, its return slot and the KCCB
 * write offset, which it syncs immediately.
 */
#ifndef PVR_CONTEXT_H
#define PVR_CONTEXT_H


#include "pvr_mmu.h"


/* pvr_queue.c */
#define PVR_CTX_CCCB_SIZE_LOG2_LINUX		15	/* CTX_*_CCCB_SIZE_LOG2 */
#define PVR_CTX_MAX_DEADLINE_MS				30000	/* MAX_DEADLINE_MS */

/* pvr_context.h: enum pvr_context_priority */
#define PVR_CTX_PRIORITY_LOW				0
#define PVR_CTX_PRIORITY_MEDIUM				1
	/* DRM_PVR_CTX_PRIORITY_NORMAL */
#define PVR_CTX_PRIORITY_HIGH				2

/* pvr_rogue_cr_defs.h, for pvr_gpu_dump_mmu_state() */
#define PVR_CR_MTS_SCHEDULE					0x0B00
#define PVR_CR_BIF_CAT_BASE(n)				(0x1200 + (n) * 0x0008)
#define PVR_CR_BIF_CAT_BASE_COUNT			8
#define PVR_CR_BIF_FAULT_BANK0_MMU_STATUS	0x12B0
#define PVR_CR_BIF_FAULT_BANK0_REQ_STATUS	0x12B8
#define PVR_CR_BIF_FAULT_BANK1_MMU_STATUS	0x12C0
#define PVR_CR_BIF_FAULT_BANK1_REQ_STATUS	0x12C8
#define PVR_CR_BIF_FAULT_READ				0x13E0
#define PVR_CR_MMU_FAULT_STATUS				0xE150
#define PVR_CR_MMU_FAULT_STATUS_META		0xE158

/* pvr_ccb.c:294: every KCCB command is kicked with the GP data master */
#define PVR_MTS_SCHEDULE_KCCB				PVR_FWIF_DM_GP


enum pvr_ctx_type {
	PVR_CTX_TYPE_TRANSFER_FRAG,
		// DRM_PVR_CTX_TYPE_TRANSFER_FRAG: no static state, DM FRAG
	PVR_CTX_TYPE_COMPUTE,
		// DRM_PVR_CTX_TYPE_COMPUTE: zeroed static CDM state, DM CDM
};


/* frees what pvr_fwif_alloc_hook returned */
typedef void (*pvr_fw_free_hook)(void* cookie, void* cpu, uint64 phys,
	uint32 fwAddr, size_t size);
/* writes ROGUE_CR_MTS_SCHEDULE (32 bit), then reads it back */
typedef void (*pvr_mts_schedule_hook)(void* cookie, uint32 value);
/* reads a 64 bit GPU register; for pvr_gpu_dump_mmu_state() only */
typedef uint64 (*pvr_cr_read64_hook)(void* cookie, uint32 offset);


struct pvr_ctx_env {
	void*					cookie;
	pvr_fwif_alloc_hook		fw_alloc;
	pvr_fw_free_hook		fw_free;
		// may be NULL: the objects are then left to the caller
	pvr_cache_sync_hook		sync;
	pvr_mts_schedule_hook	mts_schedule;
	pvr_cr_read64_hook		cr_read64;
		// may be NULL
	pvr_fwif_log_hook		log;
		// may be NULL
};


/* one firmware object (pvr_fw_object) */
struct pvr_fw_obj {
	const char*		name;
	void*			cpu;
	uint64			phys;
	uint32			fw_addr;
	size_t			size;
		// page granular allocation size
};


/* pvr_vm_context: the MMU context and its firmware memory context */
struct pvr_fw_mem_context {
	pvr_mmu_context*				mmu;
	pvr_fw_obj						obj;
	struct rogue_fwif_fwmemcontext*	fw;
};


struct pvr_ctx_params {
	pvr_ctx_type	type;
	uint32			priority;
		// PVR_CTX_PRIORITY_*, Linux default MEDIUM
	uint32			cccb_size_log2;
		// Linux: 15 (32 KB); 0 selects that
	uint32			ctx_id;
		// server_common_context_id (Linux: xa_alloc, from 1)
	uint32			pid;
	uint32			num_isp_store_registers;
		// transfer state size (pvr_queue.c: get_xfer_ctx_state_size()):
		// 1 with feature xe_memory_hierarchy (this core), else
		// num_isp_ipp_pipes; 0 selects 1
};


#define PVR_CTX_OBJ_CCCB_CTRL	0
#define PVR_CTX_OBJ_CCCB		1
#define PVR_CTX_OBJ_REG_STATE	2
#define PVR_CTX_OBJ_UFO			3
#define PVR_CTX_OBJ_FW_CONTEXT	4
#define PVR_CTX_OBJ_COUNT		5

/* pvr_context + its one pvr_queue */
struct pvr_gpu_context {
	pvr_ctx_type					type;
	pvr_fw_mem_context*				mem;
	uint32							dm;
	uint32							ctx_offset;
		// of the common context in the FW context object (0 for both)

	// in creation order (pvr_queue_create(), then pvr_context_create())
	pvr_fw_obj						objs[PVR_CTX_OBJ_COUNT];
	uint32							obj_count;

	// struct pvr_cccb
	struct rogue_fwif_cccb_ctl*		cccb_ctrl;
	uint8*							cccb;
	uint32							cccb_size;
	uint32							cccb_write_offset;
		// host shadow; the firmware learns it from the KICK command
	uint32							cccb_wrap_mask;

	// queue->timeline_ufo
	volatile uint32*				ufo;
	uint32							ufo_fw_addr;

	struct rogue_fwif_fwcommoncontext*	fw_common;
	uint32							fw_common_addr;
		// pvr_context_get_fw_addr() + ctx_offset: the KICK target
};


struct pvr_gpu_context_status {
	uint32	cccb_write_offset;		// host shadow
	uint32	cccb_ctrl_write_offset;	// rogue_fwif_cccb_ctl, firmware view
	uint32	cccb_read_offset;
	uint32	cccb_dep_offset;
	uint32	ufo_value;
	uint32	page_cat_base_reg_set;	// of the FW memory context
	uint64	pc_dev_paddr;			// as read back
	uint32	fw_com_ctx_flags;
	uint32	last_failed_ufo_addr;
	uint32	last_failed_ufo_value;
};


/* NULL+UPDATE or UPDATE only for pvr_context_submit_update() */
#define PVR_SUBMIT_WITH_NULL_COMMAND	0x1


//	#pragma mark - KCCB (pvr_ccb.c)


/*!	Copies cmd into the next KCCB slot, clears its return slot, advances the
	write offset and kicks the MTS (DM GP), syncing on the way.
	B_BUSY if the KCCB is full.
*/
status_t pvr_kccb_send_cmd(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	const struct rogue_fwif_kccb_cmd* cmd, uint32* _slot);
/*!	Whether the return slot has ROGUE_FWIF_KCCB_RTN_SLOT_CMD_EXECUTED
	(invalidates it first).
*/
bool pvr_kccb_slot_executed(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	uint32 slot, uint32* _rtn);
/*!	pvr_kccb_wait_for_completion(), polling every 100 us. */
status_t pvr_kccb_wait_for_completion(const pvr_ctx_env& env,
	pvr_fwif_state* fwif, uint32 slot, bigtime_t timeout, uint32* _rtn);


//	#pragma mark - MMU cache (pvr_mmu.c)


/*!	pvr_mmu_flush_exec(): sends ROGUE_FWIF_KCCB_CMD_MMUCACHE with the flags
	accumulated in mmu plus extraFlags (nothing is sent if both are 0),
	mmu_cache_sync_fw_addr = the "mmucache sync" object of fwif and update
	value 0, then waits up to 1 s (HZ) for the return slot, as Linux always
	does. mmu may be NULL (extraFlags only). The optional _slot/_rtn report
	the KCCB slot and its return value.
*/
status_t pvr_mmu_flush_exec(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_mmu_context* mmu, uint32 extraFlags, uint32* _slot = NULL,
	uint32* _rtn = NULL);


//	#pragma mark - contexts


/*!	pvr_vm_create_context(is_userspace_context = true): creates the FW
	memory context object pointing at the page catalogue of mmu.
*/
status_t pvr_fw_mem_context_create(const pvr_ctx_env& env,
	pvr_mmu_context* mmu, pvr_fw_mem_context* memContext);
/*!	Frees the FW memory context object (not the MMU context). Only once no
	context uses it any more (after pvr_gpu_context_destroy()).
*/
void pvr_fw_mem_context_destroy(const pvr_ctx_env& env,
	pvr_fw_mem_context* memContext);

/*!	pvr_context_create() of a transfer or compute context: CCCB control and
	buffer, context state, timeline UFO, then the FW context object with its
	common context. Everything is synced. Afterwards the caller requests an
	MMU flush (pvr_mmu_flush_exec() with PVR_MMU_SYNC_LEVEL_2_FLAGS) before
	the first kick, because new pages were added to the MIPS page table.
	The MIPS page table entries written by the fw_alloc hook are not synced
	by this module: the hook or the caller writes them back before that
	flush, or the firmware cannot reach the new objects.
	A compute context gets a zeroed static CDM state: Linux refuses an
	empty one (init_compute_fw_objs() -> -EINVAL). This is a deviation:
	without a context switch state the compute context cannot be
	preempted yet.
*/
status_t pvr_gpu_context_create(const pvr_ctx_env& env,
	pvr_fw_mem_context* memContext, const pvr_ctx_params& params,
	pvr_gpu_context* context);

/*!	pvr_cccb_write_command_with_header(): appends one command (a padding
	command first if it would not fit before the end). Not synced: call
	pvr_gpu_context_kick() after the last command.
*/
status_t pvr_cccb_write_command(const pvr_ctx_env& env,
	pvr_gpu_context* context, uint32 cmdType, const void* data, uint32 size,
	uint32 extJobRef, uint32 intJobRef);

/*!	pvr_cccb_send_kccb_kick(): syncs the CCCB and sends
	ROGUE_FWIF_KCCB_CMD_KICK with the context's FW address and the CCCB
	write offset (MTS kick with DM GP, pvr_ccb.c:294).
*/
status_t pvr_gpu_context_kick(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_gpu_context* context, uint32* _kccbSlot);

/*!	Firmware-only job: [NULL,] UPDATE(timeline UFO := value) and kick. As
	the tail of every Linux job (pvr_queue_submit_job_to_cccb()), but
	without a data master command: Linux never submits that on its own.
*/
status_t pvr_gpu_context_submit_update(const pvr_ctx_env& env,
	pvr_fwif_state* fwif, pvr_gpu_context* context, uint32 value,
	uint32 submitFlags, uint32* _kccbSlot);

/*!	Compute job: CDM command (ROGUE_FWIF_CCB_CMD_TYPE_CDM, cmd as built by
	the caller, frame_num and flags as pvr_compute_job_fw_cmd_init()), then
	UPDATE(timeline UFO := value), then kick. Compute contexts only.
*/
status_t pvr_gpu_context_submit_compute(const pvr_ctx_env& env,
	pvr_fwif_state* fwif, pvr_gpu_context* context,
	const struct rogue_fwif_cmd_compute* cmd, uint32 value,
	uint32* _kccbSlot);

/*!	Invalidates and reads back the CCCB control, the UFO, the common context
	and the FW memory context.
*/
void pvr_gpu_context_get_status(const pvr_ctx_env& env,
	pvr_gpu_context* context, pvr_gpu_context_status* status);
/*!	CCCB consumed up to the host write offset and UFO == value. */
bool pvr_gpu_context_done(const pvr_ctx_env& env, pvr_gpu_context* context,
	uint32 value);

/*!	pvr_queue_cleanup_fw_context() -> pvr_fw_structure_cleanup(
	ROGUE_FWIF_CLEANUP_FWCOMMONCONTEXT): retried while the firmware answers
	CLEANUP_BUSY, until timeout. B_OK once the firmware released it.
*/
status_t pvr_gpu_context_cleanup(const pvr_ctx_env& env,
	pvr_fwif_state* fwif, pvr_gpu_context* context, bigtime_t timeout);
/*!	Frees the objects in the order of pvr_queue_destroy() and
	pvr_context_release(): UFO, context state, CCCB, then the FW context.
	Only after pvr_gpu_context_cleanup() returned B_OK.
*/
void pvr_gpu_context_destroy(const pvr_ctx_env& env,
	pvr_gpu_context* context);


//	#pragma mark - diagnostics


/*!	Logs (env.log) the MMU fault registers of both register sets, the BIF
	catalogue base register the firmware assigned to memContext (if any),
	the HWR info buffer and the pending FWCCB commands.
*/
void pvr_gpu_dump_mmu_state(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_fw_mem_context* memContext);


#endif	// PVR_CONTEXT_H
