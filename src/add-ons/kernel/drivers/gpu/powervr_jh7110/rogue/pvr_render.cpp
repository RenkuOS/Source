/*
 * Copyright 2026, The RenkuOS authors.
 * Copyright (c) 2023 Imagination Technologies Ltd.
 * SPDX-License-Identifier: GPL-2.0-only OR MIT
 *
 * Port of pvr_free_list.c, pvr_hwrt.c, the context creation of
 * pvr_context.c and the job submission of pvr_job.c and pvr_queue.c of the
 * Linux drm/imagination driver, without the DRM scheduler and fences.
 */


#include "pvr_render.h"

#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <OS.h>


#define LOG(env, format, ...) \
	do { \
		if ((env).log != NULL) \
			(env).log((env).cookie, format, ##__VA_ARGS__); \
	} while (false)

#define KCCB_REPLY_TIMEOUT		1000	/* us of retries on a full KCCB */

/* pvr_free_list.c */
#define FREE_LIST_MIN_PAGES				50
#define FREE_LIST_MIN_PAGES_BRN66011	40
#define FREE_LIST_MIN_PAGES_ROGUEXE		25

/* pvr_hwrt.c */
#define SRTC_ENTRY_SIZE		sizeof(u32)
#define RAA_ENTRY_SIZE		sizeof(u32)

/* pvr_rogue_cr_defs_client.h */
#define ROGUE_CR_TE_AA_Y2_EN				0x00000008U
#define ROGUE_CR_TE_AA_Y_EN					0x00000004U
#define ROGUE_CR_TE_AA_X_EN					0x00000002U
#define ROGUE_CR_TE_AA_X2_EN				0x00000001U
#define ROGUE_CR_TE_MTILE1_X1_SHIFT			18
#define ROGUE_CR_TE_MTILE1_X1_CLRMSK		0xF803FFFFU
#define ROGUE_CR_TE_MTILE1_X2_SHIFT			9U
#define ROGUE_CR_TE_MTILE1_X2_CLRMSK		0xFFFC01FFU
#define ROGUE_CR_TE_MTILE1_X3_SHIFT			0
#define ROGUE_CR_TE_MTILE1_X3_CLRMSK		0xFFFFFE00U
#define ROGUE_CR_TE_MTILE2_Y1_SHIFT			18
#define ROGUE_CR_TE_MTILE2_Y1_CLRMSK		0xF803FFFFU
#define ROGUE_CR_TE_MTILE2_Y2_SHIFT			9
#define ROGUE_CR_TE_MTILE2_Y2_CLRMSK		0xFFFC01FFU
#define ROGUE_CR_TE_MTILE2_Y3_SHIFT			0
#define ROGUE_CR_TE_MTILE2_Y3_CLRMSK		0xFFFFFE00U
#define ROGUE_CR_TE_SCREEN_YMAX_SHIFT		12
#define ROGUE_CR_TE_SCREEN_YMAX_CLRMSK		0xFFE00FFFU
#define ROGUE_CR_TE_SCREEN_XMAX_SHIFT		0
#define ROGUE_CR_TE_SCREEN_XMAX_CLRMSK		0xFFFFFE00U
#define ROGUE_CR_PPP_SCREEN_PIXYMAX_SHIFT	16
#define ROGUE_CR_PPP_SCREEN_PIXYMAX_CLRMSK	0x8000FFFFU
#define ROGUE_CR_PPP_SCREEN_PIXXMAX_SHIFT	0
#define ROGUE_CR_PPP_SCREEN_PIXXMAX_CLRMSK	0xFFFF8000U
#define ROGUE_CR_ISP_MTILE_SIZE_X_SHIFT		16
#define ROGUE_CR_ISP_MTILE_SIZE_X_CLRMSK	0xFC00FFFFU
#define ROGUE_CR_ISP_MTILE_SIZE_Y_SHIFT		0
#define ROGUE_CR_ISP_MTILE_SIZE_Y_CLRMSK	0xFFFFFC00U


struct pvr_free_list_node {
	pvr_free_list_node*	next;
	void*				block;
	uint32				num_pages;
};


static inline void
memory_barrier()
{
	__asm__ __volatile__("fence rw, rw" ::: "memory");
}


static inline uint32
div_round_up(uint32 value, uint32 divisor)
{
	return (value + divisor - 1) / divisor;
}


static inline uint32
round_up(uint32 value, uint32 multiple)
{
	return div_round_up(value, multiple) * multiple;
}


/*!	Sends a KCCB command, retrying briefly while the KCCB is full (Linux'
	pvr_kccb_send_cmd() sleeps until a slot is free).
*/
static status_t
send_kccb_cmd(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	const rogue_fwif_kccb_cmd* cmd)
{
	bigtime_t start = system_time();
	while (true) {
		status_t status = pvr_kccb_send_cmd(env, fwif, cmd, NULL);
		if (status != B_BUSY || system_time() - start >= KCCB_REPLY_TIMEOUT)
			return status;
		snooze(20);
	}
}


//	#pragma mark - device derived sizes


uint32
pvr_get_free_list_min_pages(const pvr_device_desc& desc)
{
	if (desc.roguexe) {
		if (desc.brn66011)
			return FREE_LIST_MIN_PAGES_BRN66011;
		return FREE_LIST_MIN_PAGES_ROGUEXE;
	}
	return FREE_LIST_MIN_PAGES;
}


uint32
pvr_frag_ctx_isp_store_registers(const pvr_device_desc& desc)
{
	if (desc.xe_memory_hierarchy) {
		uint32 count = desc.num_raster_pipes;
		if (desc.gpu_multicore_support)
			count *= 1 + desc.xpu_max_slaves;
		return count;
	}
	return desc.num_isp_ipp_pipes;
}


uint32
pvr_xfer_ctx_isp_store_registers(const pvr_device_desc& desc)
{
	if (desc.xe_memory_hierarchy)
		return 1;
	return desc.num_isp_ipp_pipes;
}


//	#pragma mark - contexts


status_t
pvr_render_context_create_uapi(const pvr_ctx_env& env,
	const pvr_device_desc& desc, pvr_fw_mem_context* memContext,
	const pvr_context_create_args& args, pvr_render_context* context)
{
	memset(context, 0, sizeof(*context));
	if (args.type != PVR_UAPI_CTX_TYPE_RENDER)
		return B_BAD_VALUE;

	// init_render_fw_objs(): a static state is required
	if (args.static_state_len == 0)
		return B_BAD_VALUE;

	rogue_fwif_geom_registers_caswitch staticState;
	status_t status = pvr_stream_parse_static_render_state(desc,
		args.static_state, args.static_state_len, &staticState);
	if (status != B_OK)
		return status;

	uint32 isp = pvr_frag_ctx_isp_store_registers(desc);
	if (isp == 0)
		return B_BAD_VALUE;

	pvr_render_ctx_params params;
	memset(&params, 0, sizeof(params));
	params.priority = args.priority;
	params.cccb_size_log2 = args.cccb_size_log2;
	params.ctx_id = args.ctx_id;
	params.pid = args.pid;
	params.callstack_addr = args.callstack_addr;
	params.num_frag_isp_store_registers = isp;
	params.static_state = &staticState;
	return pvr_render_context_create(env, memContext, params, context);
}


status_t
pvr_gpu_context_create_uapi(const pvr_ctx_env& env,
	const pvr_device_desc& desc, pvr_fw_mem_context* memContext,
	const pvr_context_create_args& args, pvr_gpu_context* context)
{
	memset(context, 0, sizeof(*context));

	// pvr_queue_create(): a call stack is only valid for render contexts
	if (args.callstack_addr != 0)
		return B_BAD_VALUE;

	pvr_ctx_params params;
	memset(&params, 0, sizeof(params));
	params.priority = args.priority;
	params.cccb_size_log2 = args.cccb_size_log2;
	params.ctx_id = args.ctx_id;
	params.pid = args.pid;

	rogue_fwif_static_computecontext_state staticState;
	switch (args.type) {
		case PVR_UAPI_CTX_TYPE_COMPUTE:
		{
			// init_compute_fw_objs()
			if (args.static_state_len == 0)
				return B_BAD_VALUE;
			memset(&staticState, 0, sizeof(staticState));
			status_t status = pvr_stream_parse_static_compute_state(desc,
				args.static_state, args.static_state_len,
				&staticState.ctxswitch_regs);
			if (status != B_OK)
				return status;
			params.type = PVR_CTX_TYPE_COMPUTE;
			params.compute_static_state = &staticState;
			break;
		}
		case PVR_UAPI_CTX_TYPE_TRANSFER_FRAG:
			// init_transfer_fw_objs()
			if (args.static_state_len != 0)
				return B_BAD_VALUE;
			params.type = PVR_CTX_TYPE_TRANSFER_FRAG;
			params.num_isp_store_registers
				= pvr_xfer_ctx_isp_store_registers(desc);
			if (params.num_isp_store_registers == 0)
				return B_BAD_VALUE;
			break;
		default:
			return B_BAD_VALUE;
	}

	status_t status = pvr_gpu_context_create(env, memContext, params,
		context);
	if (status != B_OK)
		pvr_gpu_context_destroy(env, context);
	return status;
}


//	#pragma mark - free lists


/*!	calculate_free_list_ready_pages_locked(): pages kept in reserve so that
	the firmware can handle an out of memory quickly and ask for a grow.
*/
static uint32
calculate_ready_pages(const pvr_free_list* freeList, uint32 pages)
{
	uint32 readyPages = (uint32)((uint64)pages * freeList->grow_threshold
		/ 100);

	// fewer than the grow size, a multiple of the free list alignment
	if (readyPages > freeList->grow_pages)
		readyPages = freeList->grow_pages;
	return readyPages & ~(uint32)PVR_FREE_LIST_ALIGNMENT;
}


/*!	pvr_free_list_insert_pages_locked() + pvr_free_list_insert_node_locked():
	writes the PFNs of the node's pages below the pages already in the list.
*/
static status_t
insert_node(pvr_free_list* freeList, pvr_free_list_node* node)
{
	// Linux does not check this; an overflow would write before the list
	if ((uint64)freeList->current_pages + node->num_pages
			> freeList->max_pages) {
		return B_BAD_VALUE;
	}

	uint32 startPage = freeList->max_pages - freeList->current_pages
		- node->num_pages;
	uint32 offset = (startPage * PVR_FREE_LIST_ENTRY_SIZE)
		& ~(uint32)(ROGUE_BIF_PM_FREELIST_BASE_ADDR_ALIGNSIZE - 1);

	const pvr_free_list_hooks& hooks = freeList->hooks;
	uint32 entries[64];
	uint32 count = 0;
	for (uint32 i = 0; i < node->num_pages; i++) {
		uint64 phys = hooks.pm_page_phys(hooks.cookie, node->block, i);
		uint64 pfn = phys >> ROGUE_BIF_PM_PHYSICAL_PAGE_ALIGNSHIFT;
		if ((pfn >> 32) != 0
			|| (phys & (ROGUE_BIF_PM_PHYSICAL_PAGE_SIZE - 1)) != 0) {
			return B_BAD_VALUE;
		}
		entries[count++] = (uint32)pfn;

		if (count == ARRAY_SIZE(entries) || i + 1 == node->num_pages) {
			status_t status = hooks.page_list_write(hooks.cookie,
				freeList->page_list, offset, entries, count);
			if (status != B_OK)
				return status;
			offset += count * PVR_FREE_LIST_ENTRY_SIZE;
			count = 0;
		}
	}

	freeList->current_pages += node->num_pages;
	return B_OK;
}


/*!	pvr_free_list_grow(). */
static status_t
free_list_grow(pvr_free_list* freeList, uint32 numPages)
{
	if ((numPages & PVR_FREE_LIST_ALIGNMENT) != 0 || numPages == 0)
		return B_BAD_VALUE;
	if ((uint64)freeList->current_pages + freeList->ready_pages + numPages
			> freeList->max_pages) {
		return B_BAD_VALUE;
	}

	pvr_free_list_node* node
		= (pvr_free_list_node*)calloc(1, sizeof(pvr_free_list_node));
	if (node == NULL)
		return B_NO_MEMORY;
	node->num_pages = numPages;

	const pvr_free_list_hooks& hooks = freeList->hooks;
	status_t status = hooks.pm_pages_alloc(hooks.cookie, numPages,
		&node->block);
	if (status != B_OK) {
		free(node);
		return status;
	}

	status = insert_node(freeList, node);
	if (status != B_OK) {
		hooks.pm_pages_free(hooks.cookie, node->block);
		free(node);
		return status;
	}

	// mem_block_list: append
	if (freeList->last_node != NULL)
		freeList->last_node->next = node;
	else
		freeList->nodes = node;
	freeList->last_node = node;

	// reserve ready pages for the firmware to handle OOM quickly
	freeList->ready_pages = calculate_ready_pages(freeList,
		freeList->current_pages);
	freeList->current_pages -= freeList->ready_pages;
	return B_OK;
}


static void
free_list_free_nodes(pvr_free_list* freeList)
{
	pvr_free_list_node* node = freeList->nodes;
	while (node != NULL) {
		pvr_free_list_node* next = node->next;
		freeList->hooks.pm_pages_free(freeList->hooks.cookie, node->block);
		free(node);
		node = next;
	}
	freeList->nodes = freeList->last_node = NULL;
}


status_t
pvr_free_list_create(const pvr_ctx_env& env, const pvr_free_list_hooks& hooks,
	const pvr_free_list_create_args& args, pvr_free_list* freeList)
{
	memset(freeList, 0, sizeof(*freeList));
	if (hooks.pm_pages_alloc == NULL || hooks.pm_page_phys == NULL
		|| hooks.pm_pages_free == NULL || hooks.page_list_write == NULL)
		return B_BAD_VALUE;

	// free_list_create_kernel_structure()
	if (args.grow_threshold > 100
		|| args.initial_num_pages > args.max_num_pages
		|| args.grow_num_pages > args.max_num_pages
		|| args.max_num_pages == 0
		|| (args.initial_num_pages < args.max_num_pages
			&& args.grow_num_pages == 0)
		|| (args.initial_num_pages == args.max_num_pages
			&& args.grow_num_pages != 0))
		return B_BAD_VALUE;

	if ((args.initial_num_pages & PVR_FREE_LIST_ALIGNMENT) != 0
		|| (args.max_num_pages & PVR_FREE_LIST_ALIGNMENT) != 0
		|| (args.grow_num_pages & PVR_FREE_LIST_ALIGNMENT) != 0)
		return B_BAD_VALUE;

	// the flag checks of the buffer object are the caller's
	if (args.page_list_size
			< (uint64)args.max_num_pages * PVR_FREE_LIST_ENTRY_SIZE)
		return B_BAD_VALUE;

	freeList->hooks = hooks;
	freeList->page_list = args.page_list;
	freeList->free_list_gpu_addr = args.free_list_gpu_addr;
	freeList->fw_id = args.fw_id;
	freeList->current_pages = 0;
	freeList->max_pages = args.max_num_pages;
	freeList->grow_pages = args.grow_num_pages;
	freeList->grow_threshold = args.grow_threshold;
	freeList->initial_num_pages = args.initial_num_pages;

	// free_list_create_fw_structure() -> free_list_fw_init()
	status_t status = pvr_fw_object_create(env, "free list",
		sizeof(rogue_fwif_freelist), &freeList->fw_obj);
	if (status != B_OK)
		return status;

	rogue_fwif_freelist* fwData = (rogue_fwif_freelist*)freeList->fw_obj.cpu;
	freeList->fw_data = fwData;
	uint32 readyPages = calculate_ready_pages(freeList,
		freeList->initial_num_pages);
	fwData->max_pages = freeList->max_pages;
	fwData->current_pages = freeList->initial_num_pages - readyPages;
	fwData->grow_pages = freeList->grow_pages;
	fwData->ready_pages = readyPages;
	fwData->freelist_id = freeList->fw_id;
	fwData->grow_pending = false;
	fwData->current_stack_top = fwData->current_pages - 1;
	fwData->freelist_dev_addr = freeList->free_list_gpu_addr;
	fwData->current_dev_addr = (fwData->freelist_dev_addr
			+ ((uint64)(fwData->max_pages - fwData->current_pages)
				* PVR_FREE_LIST_ENTRY_SIZE))
		& ~((uint64)ROGUE_BIF_PM_FREELIST_BASE_ADDR_ALIGNSIZE - 1);
	memory_barrier();
	pvr_fw_object_sync(env, freeList->fw_obj);

	// pvr_free_list_grow(initial_num_pages): 0 pages fails there too
	// (pvr_gem_object_create() rejects a size of 0)
	status = free_list_grow(freeList, args.initial_num_pages);
	if (status != B_OK) {
		free_list_free_nodes(freeList);
		pvr_fw_object_destroy(env, &freeList->fw_obj);
		freeList->fw_data = NULL;
		return status;
	}

	LOG(env, "pvr_render: free list %" B_PRIu32 " at fw %#" B_PRIx32
		", list %#" B_PRIx64 ", %" B_PRIu32 "+%" B_PRIu32 " of %" B_PRIu32
		" pages\n", freeList->fw_id, freeList->fw_obj.fw_addr,
		freeList->free_list_gpu_addr, freeList->current_pages,
		freeList->ready_pages, freeList->max_pages);
	return B_OK;
}


status_t
pvr_free_list_cleanup(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_free_list* freeList, bigtime_t timeout)
{
	if (freeList->fw_obj.cpu == NULL)
		return B_OK;
	return pvr_fw_structure_cleanup(env, fwif, ROGUE_FWIF_CLEANUP_FREELIST,
		freeList->fw_obj.fw_addr, timeout);
}


status_t
pvr_free_list_destroy(const pvr_ctx_env& env, pvr_free_list* freeList)
{
	// free_list_destroy_kernel_structure(): WARN_ON(!list_empty(hwrt_list))
	if (freeList->hwrt_count != 0 || freeList->hwrt_list != NULL)
		return B_BUSY;

	free_list_free_nodes(freeList);
	pvr_fw_object_destroy(env, &freeList->fw_obj);
	memset(freeList, 0, sizeof(*freeList));
	return B_OK;
}


status_t
pvr_free_list_send_pending_reply(const pvr_ctx_env& env,
	pvr_fwif_state* fwif, pvr_free_list* freeList)
{
	if (!freeList->reply_pending)
		return B_OK;

	status_t status = send_kccb_cmd(env, fwif, &freeList->reply);
	if (status == B_OK)
		freeList->reply_pending = false;
	return status;
}


status_t
pvr_free_list_process_grow_req(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_free_list* freeList)
{
	if (freeList == NULL || freeList->fw_obj.cpu == NULL)
		return B_BAD_VALUE;
	if (freeList->reply_pending)
		return B_BUSY;

	// the firmware made the request: it consumed the ready pages already
	freeList->current_pages += freeList->ready_pages;
	freeList->ready_pages = 0;

	uint32 growPages = 0;
	status_t growStatus = free_list_grow(freeList, freeList->grow_pages);
	if (growStatus == B_OK)
		growPages = freeList->grow_pages;

	rogue_fwif_kccb_cmd& cmd = freeList->reply;
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_type = ROGUE_FWIF_KCCB_CMD_FREELIST_GROW_UPDATE;
	rogue_fwif_freelist_gs_data* reply
		= (rogue_fwif_freelist_gs_data*)cmd.cmd_data.raw;
	reply->freelist_fw_addr = freeList->fw_obj.fw_addr;
	reply->delta_pages = growPages;
	reply->new_pages = freeList->current_pages + freeList->ready_pages;
	reply->ready_pages = freeList->ready_pages;
	freeList->reply_pending = true;

	LOG(env, "pvr_render: free list %" B_PRIu32 " grow by %" B_PRIu32
		" pages (%#" B_PRIx32 "): %" B_PRIu32 " pages, %" B_PRIu32 " ready\n",
		freeList->fw_id, growPages, (uint32)growStatus, reply->new_pages,
		reply->ready_pages);

	return pvr_free_list_send_pending_reply(env, fwif, freeList);
}


/*!	pvr_free_list_reconstruct(). */
static void
free_list_reconstruct(const pvr_ctx_env& env, pvr_free_list* freeList)
{
	// rebuild the page list from the memory blocks
	freeList->current_pages = 0;
	for (pvr_free_list_node* node = freeList->nodes; node != NULL;
			node = node->next) {
		status_t status = insert_node(freeList, node);
		if (status != B_OK) {
			LOG(env, "pvr_render: free list %" B_PRIu32 ": reinsert failed: "
				"%#" B_PRIx32 "\n", freeList->fw_id, (uint32)status);
		}
	}

	// remove the ready pages again
	freeList->current_pages -= freeList->ready_pages;

	// the firmware is waiting for the reply: it does not write the object
	pvr_fw_object_sync(env, freeList->fw_obj);
	volatile rogue_fwif_freelist* fwData = freeList->fw_data;
	fwData->current_stack_top = fwData->current_pages - 1;
	fwData->allocated_page_count = 0;
	fwData->allocated_mmu_page_count = 0;
	memory_barrier();
	pvr_fw_object_sync(env, freeList->fw_obj);

	// reset the state of the associated HWRT data
	for (pvr_hwrt_data* data = freeList->hwrt_list; data != NULL;
			data = data->free_list_next) {
		pvr_fw_object_sync(env, data->fw_obj);
		volatile rogue_fwif_hwrtdata* hwrt
			= (volatile rogue_fwif_hwrtdata*)data->fw_obj.cpu;
		hwrt->state = ROGUE_FWIF_RTDATA_STATE_HWR;
		hwrt->hwrt_data_flags &= ~(uint32)HWRTDATA_HAS_LAST_GEOM;
		memory_barrier();
		pvr_fw_object_sync(env, data->fw_obj);
	}
}


status_t
pvr_free_list_process_reconstruct_req(const pvr_ctx_env& env,
	pvr_fwif_state* fwif,
	const rogue_fwif_fwccb_cmd_freelists_reconstruction_data* request,
	pvr_free_list_lookup_hook lookup, void* lookupCookie,
	rogue_fwif_kccb_cmd* _reply)
{
	if (request == NULL || lookup == NULL)
		return B_BAD_VALUE;

	rogue_fwif_kccb_cmd cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_type = ROGUE_FWIF_KCCB_CMD_FREELISTS_RECONSTRUCTION_UPDATE;
	rogue_fwif_freelists_reconstruction_data* reply
		= (rogue_fwif_freelists_reconstruction_data*)cmd.cmd_data.raw;

	uint32 count = request->freelist_count;
	if (count > ARRAY_SIZE(request->freelist_ids)) {
		LOG(env, "pvr_render: reconstruction of %" B_PRIu32 " free lists, "
			"limited to %u\n", count,
			(unsigned)ARRAY_SIZE(request->freelist_ids));
		count = ARRAY_SIZE(request->freelist_ids);
	}

	for (uint32 i = 0; i < count; i++) {
		pvr_free_list* freeList = lookup(lookupCookie,
			request->freelist_ids[i]);
		if (freeList != NULL && freeList->fw_obj.cpu != NULL)
			free_list_reconstruct(env, freeList);
	}

	reply->freelist_count = count;
	memcpy(reply->freelist_ids, request->freelist_ids,
		count * sizeof(reply->freelist_ids[0]));

	if (_reply != NULL)
		*_reply = cmd;
	return send_kccb_cmd(env, fwif, &cmd);
}


//	#pragma mark - HWRT data sets


/* struct pvr_rt_mtile_info */
struct rt_mtile_info {
	uint32	mtile_x[3];
	uint32	mtile_y[3];
	uint32	tile_max_x;
	uint32	tile_max_y;
	uint32	tile_size_x;
	uint32	tile_size_y;
	uint32	num_tiles_x;
	uint32	num_tiles_y;
};


static status_t
get_cr_isp_mtile_size_val(const pvr_device_desc& desc, uint32 samples,
	const rt_mtile_info& info, uint32* _value)
{
	uint32 x = info.mtile_x[0];
	uint32 y = info.mtile_y[0];

	switch (desc.isp_samples_per_pixel) {
		case 1:
			if (samples >= 4)
				x <<= 1;
			if (samples >= 2)
				y <<= 1;
			break;
		case 2:
			if (samples >= 8)
				x <<= 1;
			if (samples >= 4)
				y <<= 1;
			break;
		case 4:
			if (samples >= 8)
				y <<= 1;
			break;
		default:
			return B_BAD_VALUE;
	}

	*_value = ((x << ROGUE_CR_ISP_MTILE_SIZE_X_SHIFT)
			& ~ROGUE_CR_ISP_MTILE_SIZE_X_CLRMSK)
		| ((y << ROGUE_CR_ISP_MTILE_SIZE_Y_SHIFT)
			& ~ROGUE_CR_ISP_MTILE_SIZE_Y_CLRMSK);
	return B_OK;
}


static status_t
get_cr_multisamplectl_val(uint32 samples, bool yFlip, uint64* _value)
{
	static const struct {
		uint8 x[8];
		uint8 y[8];
	} kSamplePositions[4] = {
		// 1 sample
		{ { 8 }, { 8 } },
		// 2 samples
		{ { 12, 4 }, { 12, 4 } },
		// 4 samples
		{ { 6, 14, 2, 10 }, { 2, 6, 10, 14 } },
		// 8 samples
		{ { 9, 7, 13, 5, 3, 1, 11, 15 }, { 5, 11, 9, 3, 13, 7, 15, 1 } },
	};

	// fls(samples) - 1
	int index = -1;
	for (uint32 value = samples; value != 0; value >>= 1)
		index++;
	if (index < 0 || index > 3)
		return B_BAD_VALUE;

	uint64 value = 0;
	for (uint32 i = 0; i < 8; i++) {
		value |= ((uint64)kSamplePositions[index].x[i]) << (i * 8);
		if (yFlip) {
			value |= ((uint64)(16 - kSamplePositions[index].y[i]) & 0xf)
				<< (i * 8 + 4);
		} else {
			value |= ((uint64)kSamplePositions[index].y[i]) << (i * 8 + 4);
		}
	}

	*_value = value;
	return B_OK;
}


static status_t
get_cr_te_aa_val(const pvr_device_desc& desc, uint32 samples, uint32* _value)
{
	uint32 value = 0;

	switch (desc.isp_samples_per_pixel) {
		case 1:
			if (samples >= 2)
				value |= ROGUE_CR_TE_AA_Y_EN;
			if (samples >= 4)
				value |= ROGUE_CR_TE_AA_X_EN;
			break;
		case 2:
			if (samples >= 2)
				value |= ROGUE_CR_TE_AA_X2_EN;
			if (samples >= 4)
				value |= ROGUE_CR_TE_AA_Y_EN;
			if (samples >= 8)
				value |= ROGUE_CR_TE_AA_X_EN;
			break;
		case 4:
			if (samples >= 2)
				value |= ROGUE_CR_TE_AA_X2_EN;
			if (samples >= 4)
				value |= ROGUE_CR_TE_AA_Y2_EN;
			if (samples >= 8)
				value |= ROGUE_CR_TE_AA_Y_EN;
			break;
		default:
			return B_BAD_VALUE;
	}

	*_value = value;
	return B_OK;
}


/*!	hwrt_init_common_fw_structure(): computes the common data. */
static status_t
hwrt_common_init(const pvr_device_desc& desc,
	const pvr_hwrt_create_args& args, rogue_fwif_hwrtdata_common* common)
{
	rt_mtile_info info;
	memset(&info, 0, sizeof(info));
	info.tile_size_x = desc.tile_size_x;
	info.tile_size_y = desc.tile_size_y;
	if (info.tile_size_x == 0 || info.tile_size_y == 0)
		return B_BAD_VALUE;

	info.num_tiles_x = div_round_up(args.width, info.tile_size_x);
	info.num_tiles_y = div_round_up(args.height, info.tile_size_y);

	if (desc.has_simple_parameter_format_version) {
		// Linux warns if the version is not 2
		if (desc.simple_parameter_format_version != 2)
			return B_BAD_VALUE;

		// 16 macrotiles with a multiple of 2x2 tiles per macrotile, aligned
		// to a tile group
		info.mtile_x[0] = div_round_up(info.num_tiles_x, 8) * 2;
		info.mtile_y[0] = div_round_up(info.num_tiles_y, 8) * 2;
		info.mtile_x[1] = 0;
		info.mtile_y[1] = 0;
		info.mtile_x[2] = 0;
		info.mtile_y[2] = 0;
		info.tile_max_x = round_up(info.num_tiles_x, 2) - 1;
		info.tile_max_y = round_up(info.num_tiles_y, 2) - 1;
	} else {
		// 16 macrotiles with a multiple of 4x4 tiles per macrotile
		info.mtile_x[0] = round_up(div_round_up(info.num_tiles_x, 4), 4);
		info.mtile_y[0] = round_up(div_round_up(info.num_tiles_y, 4), 4);
		info.mtile_x[1] = info.mtile_x[0] * 2;
		info.mtile_y[1] = info.mtile_y[0] * 2;
		info.mtile_x[2] = info.mtile_x[0] * 3;
		info.mtile_y[2] = info.mtile_y[0] * 3;
		info.tile_max_x = info.num_tiles_x - 1;
		info.tile_max_y = info.num_tiles_y - 1;
	}

	common->geom_caches_need_zeroing = false;

	common->isp_merge_lower_x = args.isp_merge_lower_x;
	common->isp_merge_lower_y = args.isp_merge_lower_y;
	common->isp_merge_upper_x = args.isp_merge_upper_x;
	common->isp_merge_upper_y = args.isp_merge_upper_y;
	common->isp_merge_scale_x = args.isp_merge_scale_x;
	common->isp_merge_scale_y = args.isp_merge_scale_y;

	uint64 multiSampleCtl;
	status_t status = get_cr_multisamplectl_val(args.samples, false,
		&multiSampleCtl);
	if (status != B_OK)
		return status;
	common->multi_sample_ctl = multiSampleCtl;

	status = get_cr_multisamplectl_val(args.samples, true, &multiSampleCtl);
	if (status != B_OK)
		return status;
	common->flipped_multi_sample_ctl = multiSampleCtl;

	common->mtile_stride = info.mtile_x[0] * info.mtile_y[0];

	status = get_cr_te_aa_val(desc, args.samples, &common->teaa);
	if (status != B_OK)
		return status;

	common->screen_pixel_max
		= (((args.width - 1) << ROGUE_CR_PPP_SCREEN_PIXXMAX_SHIFT)
			& ~ROGUE_CR_PPP_SCREEN_PIXXMAX_CLRMSK)
		| (((args.height - 1) << ROGUE_CR_PPP_SCREEN_PIXYMAX_SHIFT)
			& ~ROGUE_CR_PPP_SCREEN_PIXYMAX_CLRMSK);

	common->te_screen
		= ((info.tile_max_x << ROGUE_CR_TE_SCREEN_XMAX_SHIFT)
			& ~ROGUE_CR_TE_SCREEN_XMAX_CLRMSK)
		| ((info.tile_max_y << ROGUE_CR_TE_SCREEN_YMAX_SHIFT)
			& ~ROGUE_CR_TE_SCREEN_YMAX_CLRMSK);
	common->te_mtile1
		= ((info.mtile_x[0] << ROGUE_CR_TE_MTILE1_X1_SHIFT)
			& ~ROGUE_CR_TE_MTILE1_X1_CLRMSK)
		| ((info.mtile_x[1] << ROGUE_CR_TE_MTILE1_X2_SHIFT)
			& ~ROGUE_CR_TE_MTILE1_X2_CLRMSK)
		| ((info.mtile_x[2] << ROGUE_CR_TE_MTILE1_X3_SHIFT)
			& ~ROGUE_CR_TE_MTILE1_X3_CLRMSK);
	common->te_mtile2
		= ((info.mtile_y[0] << ROGUE_CR_TE_MTILE2_Y1_SHIFT)
			& ~ROGUE_CR_TE_MTILE2_Y1_CLRMSK)
		| ((info.mtile_y[1] << ROGUE_CR_TE_MTILE2_Y2_SHIFT)
			& ~ROGUE_CR_TE_MTILE2_Y2_CLRMSK)
		| ((info.mtile_y[2] << ROGUE_CR_TE_MTILE2_Y3_SHIFT)
			& ~ROGUE_CR_TE_MTILE2_Y3_CLRMSK);

	status = get_cr_isp_mtile_size_val(desc, args.samples, info,
		&common->isp_mtile_size);
	if (status != B_OK)
		return status;

	common->tpc_stride = args.tpc_stride;
	common->tpc_size = args.tpc_size;

	common->rgn_header_size = args.region_header_size;
	return B_OK;
}


static void
hwrt_data_fini(const pvr_ctx_env& env, pvr_hwrt_dataset* dataset,
	pvr_hwrt_data* data)
{
	// pvr_free_list_remove_hwrt(free_lists[0])
	pvr_free_list* local = dataset->free_lists[ROGUE_FW_LOCAL_FREELIST];
	for (pvr_hwrt_data** link = &local->hwrt_list; *link != NULL;
			link = &(*link)->free_list_next) {
		if (*link == data) {
			*link = data->free_list_next;
			break;
		}
	}
	data->free_list_next = NULL;

	pvr_fw_object_destroy(env, &data->raa_obj);
	pvr_fw_object_destroy(env, &data->srtc_obj);
	pvr_fw_object_destroy(env, &data->fw_obj);
}


/*!	hwrt_data_init_fw_structure(). */
static status_t
hwrt_data_init(const pvr_ctx_env& env, pvr_hwrt_dataset* dataset,
	const pvr_hwrt_create_args& args, uint32 index, pvr_hwrt_data* data)
{
	data->dataset = dataset;

	rogue_fwif_rta_ctl rtaCtl;
	memset(&rtaCtl, 0, sizeof(rtaCtl));
	rtaCtl.render_target_index = 0;
	rtaCtl.active_render_targets = 0;
	rtaCtl.valid_render_targets_fw_addr = 0;
	rtaCtl.rta_num_partial_renders_fw_addr = 0;
	rtaCtl.max_rts = args.layers;

	status_t status = B_OK;
	if (args.layers > 1) {
		status = pvr_fw_object_create(env, "hwrt srtc",
			args.layers * SRTC_ENTRY_SIZE, &data->srtc_obj);
		if (status == B_OK) {
			rtaCtl.valid_render_targets_fw_addr = data->srtc_obj.fw_addr;
			status = pvr_fw_object_create(env, "hwrt raa",
				args.layers * RAA_ENTRY_SIZE, &data->raa_obj);
		}
		if (status == B_OK)
			rtaCtl.rta_num_partial_renders_fw_addr = data->raa_obj.fw_addr;
	}
	if (status == B_OK) {
		status = pvr_fw_object_create(env, "hwrt data",
			sizeof(rogue_fwif_hwrtdata), &data->fw_obj);
	}
	if (status != B_OK) {
		pvr_fw_object_destroy(env, &data->raa_obj);
		pvr_fw_object_destroy(env, &data->srtc_obj);
		pvr_fw_object_destroy(env, &data->fw_obj);
		return status;
	}

	rogue_fwif_hwrtdata* fw = (rogue_fwif_hwrtdata*)data->fw_obj.cpu;
	fw->hwrt_data_common_fw_addr = dataset->common_obj.fw_addr;
	for (uint32 i = 0; i < ROGUE_FWIF_NUM_RTDATA_FREELISTS; i++)
		fw->freelists_fw_addr[i] = dataset->free_lists[i]->fw_obj.fw_addr;

	fw->tail_ptrs_dev_addr = args.tpc_dev_addr;
	fw->vheap_table_dev_addr = args.vheap_table_dev_addr;
	fw->rtc_dev_addr = args.rtc_dev_addr;

	fw->pm_mlist_dev_addr = args.rt_data[index].pm_mlist_dev_addr;
	fw->macrotile_array_dev_addr
		= args.rt_data[index].macrotile_array_dev_addr;
	fw->rgn_header_dev_addr = args.rt_data[index].region_header_dev_addr;
	fw->rta_ctl = rtaCtl;

	memory_barrier();
	pvr_fw_object_sync(env, data->srtc_obj);
	pvr_fw_object_sync(env, data->raa_obj);
	pvr_fw_object_sync(env, data->fw_obj);

	// pvr_free_list_add_hwrt(free_lists[0]): append
	pvr_hwrt_data** link
		= &dataset->free_lists[ROGUE_FW_LOCAL_FREELIST]->hwrt_list;
	while (*link != NULL)
		link = &(*link)->free_list_next;
	*link = data;
	data->free_list_next = NULL;
	return B_OK;
}


status_t
pvr_hwrt_dataset_create(const pvr_ctx_env& env, const pvr_device_desc& desc,
	const pvr_hwrt_create_args& args, pvr_hwrt_dataset* dataset)
{
	memset(dataset, 0, sizeof(*dataset));

	// hwrt_init_kernel_structure()
	for (uint32 i = 0; i < ROGUE_FWIF_NUM_RTDATA_FREELISTS; i++) {
		if (args.free_lists[i] == NULL
			|| args.free_lists[i]->fw_obj.cpu == NULL)
			return B_BAD_VALUE;
	}
	if (args.free_lists[ROGUE_FW_LOCAL_FREELIST]->current_pages
			< pvr_get_free_list_min_pages(desc)) {
		return B_BAD_VALUE;
	}
	// Linux keeps the layer count in a u16 (max_rts)
	if (args.layers > 0xFFFF)
		return B_BAD_VALUE;

	rogue_fwif_hwrtdata_common common;
	memset(&common, 0, sizeof(common));
	status_t status = hwrt_common_init(desc, args, &common);
	if (status != B_OK)
		return status;

	for (uint32 i = 0; i < ROGUE_FWIF_NUM_RTDATA_FREELISTS; i++)
		dataset->free_lists[i] = args.free_lists[i];
	dataset->max_rts = args.layers;

	status = pvr_fw_object_create(env, "hwrt common",
		sizeof(rogue_fwif_hwrtdata_common), &dataset->common_obj);
	if (status != B_OK) {
		memset(dataset, 0, sizeof(*dataset));
		return status;
	}
	memcpy(dataset->common_obj.cpu, &common, sizeof(common));
	memory_barrier();
	pvr_fw_object_sync(env, dataset->common_obj);

	for (uint32 i = 0; i < ROGUE_FWIF_NUM_RTDATAS; i++) {
		status = hwrt_data_init(env, dataset, args, i, &dataset->data[i]);
		if (status != B_OK)
			break;
		dataset->data_count++;
	}
	if (status != B_OK) {
		while (dataset->data_count > 0) {
			dataset->data_count--;
			hwrt_data_fini(env, dataset, &dataset->data[dataset->data_count]);
		}
		pvr_fw_object_destroy(env, &dataset->common_obj);
		memset(dataset, 0, sizeof(*dataset));
		return status;
	}

	// pvr_free_list_lookup() references
	for (uint32 i = 0; i < ROGUE_FWIF_NUM_RTDATA_FREELISTS; i++)
		dataset->free_lists[i]->hwrt_count++;

	LOG(env, "pvr_render: HWRT data set %" B_PRIu32 "x%" B_PRIu32 " x%"
		B_PRIu32 ", %" B_PRIu32 " layers: data fw %#" B_PRIx32 " %#" B_PRIx32
		"\n", args.width, args.height, args.samples, args.layers,
		dataset->data[0].fw_obj.fw_addr, dataset->data[1].fw_obj.fw_addr);
	return B_OK;
}


status_t
pvr_hwrt_dataset_cleanup(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_hwrt_dataset* dataset, bigtime_t timeout)
{
	// pvr_hwrt_dataset_release(): last data first
	for (uint32 i = dataset->data_count; i-- > 0;) {
		status_t status = pvr_fw_structure_cleanup(env, fwif,
			ROGUE_FWIF_CLEANUP_HWRTDATA, dataset->data[i].fw_obj.fw_addr,
			timeout);
		if (status != B_OK)
			return status;
	}
	return B_OK;
}


void
pvr_hwrt_dataset_destroy(const pvr_ctx_env& env, pvr_hwrt_dataset* dataset)
{
	if (dataset->free_lists[ROGUE_FW_LOCAL_FREELIST] == NULL)
		return;

	while (dataset->data_count > 0) {
		dataset->data_count--;
		hwrt_data_fini(env, dataset, &dataset->data[dataset->data_count]);
	}
	pvr_fw_object_destroy(env, &dataset->common_obj);

	// hwrt_fini_kernel_structure(): pvr_free_list_put()
	for (uint32 i = 0; i < ROGUE_FWIF_NUM_RTDATA_FREELISTS; i++) {
		if (dataset->free_lists[i]->hwrt_count > 0)
			dataset->free_lists[i]->hwrt_count--;
	}
	memset(dataset, 0, sizeof(*dataset));
}


uint32
pvr_hwrt_data_cleanup_ctl_fw_addr(const pvr_hwrt_data* data)
{
	return data->fw_obj.fw_addr
		+ offsetof(rogue_fwif_hwrtdata, cleanup_state);
}


//	#pragma mark - jobs


static uint32
convert_geom_flags(uint32 flags)
{
	uint32 out = 0;
	if ((flags & PVR_SUBMIT_JOB_GEOM_CMD_FIRST) != 0)
		out |= ROGUE_GEOM_FLAGS_FIRSTKICK;
	if ((flags & PVR_SUBMIT_JOB_GEOM_CMD_LAST) != 0)
		out |= ROGUE_GEOM_FLAGS_LASTKICK;
	if ((flags & PVR_SUBMIT_JOB_GEOM_CMD_SINGLE_CORE) != 0)
		out |= ROGUE_GEOM_FLAGS_SINGLE_CORE;
	return out;
}


static uint32
convert_frag_flags(uint32 flags)
{
	uint32 out = 0;
	if ((flags & PVR_SUBMIT_JOB_FRAG_CMD_SINGLE_CORE) != 0)
		out |= ROGUE_FRAG_FLAGS_SINGLE_CORE;
	if ((flags & PVR_SUBMIT_JOB_FRAG_CMD_DEPTHBUFFER) != 0)
		out |= ROGUE_FRAG_FLAGS_DEPTHBUFFER;
	if ((flags & PVR_SUBMIT_JOB_FRAG_CMD_STENCILBUFFER) != 0)
		out |= ROGUE_FRAG_FLAGS_STENCILBUFFER;
	if ((flags & PVR_SUBMIT_JOB_FRAG_CMD_PREVENT_CDM_OVERLAP) != 0)
		out |= ROGUE_FRAG_FLAGS_PREVENT_CDM_OVERLAP;
	if ((flags & PVR_SUBMIT_JOB_FRAG_CMD_SCRATCHBUFFER) != 0)
		out |= ROGUE_FRAG_FLAGS_SCRATCHBUFFER;
	if ((flags & PVR_SUBMIT_JOB_FRAG_CMD_GET_VIS_RESULTS) != 0)
		out |= ROGUE_FRAG_FLAGS_GET_VIS_RESULTS;
	if ((flags & PVR_SUBMIT_JOB_FRAG_CMD_DISABLE_PIXELMERGE) != 0)
		out |= ROGUE_FRAG_FLAGS_DISABLE_PIXELMERGE;
	return out;
}


static uint32
convert_compute_flags(uint32 flags)
{
	uint32 out = 0;
	if ((flags & PVR_SUBMIT_JOB_COMPUTE_CMD_PREVENT_ALL_OVERLAP) != 0)
		out |= ROGUE_COMPUTE_FLAG_PREVENT_ALL_OVERLAP;
	if ((flags & PVR_SUBMIT_JOB_COMPUTE_CMD_SINGLE_CORE) != 0)
		out |= ROGUE_COMPUTE_FLAG_SINGLE_CORE;
	return out;
}


static uint32
convert_transfer_flags(uint32 flags)
{
	uint32 out = 0;
	if ((flags & PVR_SUBMIT_JOB_TRANSFER_CMD_SINGLE_CORE) != 0)
		out |= ROGUE_TRANSFER_FLAGS_SINGLE_CORE;
	return out;
}


/* ufo_cmds_size() */
static uint32
ufo_cmds_size(uint32 count)
{
	uint32 fullCount = count / ROGUE_FWIF_CCB_CMD_MAX_UFOS;
	uint32 remaining = count % ROGUE_FWIF_CCB_CMD_MAX_UFOS;
	uint32 size = fullCount * pvr_cccb_cmd_size(ROGUE_FWIF_CCB_CMD_MAX_UFOS
		* sizeof(rogue_fwif_ufo));
	if (remaining != 0)
		size += pvr_cccb_cmd_size(remaining * sizeof(rogue_fwif_ufo));
	return size;
}


/* job_cmds_size() */
static uint32
job_cmds_size(const pvr_job* job, uint32 waitCount)
{
	return ufo_cmds_size(waitCount) + pvr_cccb_cmd_size(job->cmd_len)
		+ ufo_cmds_size(1);
}


status_t
pvr_job_init(const pvr_device_desc& desc, const pvr_job_args& args,
	pvr_job* job)
{
	memset(job, 0, sizeof(*job));

	// create_job()
	if (args.stream == NULL || args.stream_size == 0)
		return B_BAD_VALUE;
	if (args.type != PVR_JOB_TYPE_GEOMETRY
		&& args.type != PVR_JOB_TYPE_FRAGMENT && args.hwrt != NULL)
		return B_BAD_VALUE;

	job->type = args.type;
	job->flags = args.flags;
	job->id = args.id;
	job->hwrt = args.hwrt;

	status_t status;
	switch (args.type) {
		case PVR_JOB_TYPE_GEOMETRY:
		{
			// pvr_geom_job_fw_cmd_init()
			if ((args.flags & ~PVR_SUBMIT_JOB_GEOM_CMD_FLAGS_MASK) != 0
				|| args.render == NULL || args.hwrt == NULL
				|| args.hwrt->fw_obj.cpu == NULL)
				return B_BAD_VALUE;
			job->render = args.render;
			job->queue = &args.render->geom;
			job->fw_ccb_cmd_type = ROGUE_FWIF_CCB_CMD_TYPE_GEOM;
			job->cmd_len = sizeof(rogue_fwif_cmd_geom);
			status = pvr_stream_parse_geom(desc, args.stream,
				args.stream_size, &job->cmd.geom);
			if (status != B_OK)
				return status;
			rogue_fwif_cmd_geom& cmd = job->cmd.geom;
			cmd.cmd_shared.cmn.frame_num = 0;
			cmd.flags = convert_geom_flags(args.flags);
			cmd.cmd_shared.hwrt_data_fw_addr = args.hwrt->fw_obj.fw_addr;
			break;
		}
		case PVR_JOB_TYPE_FRAGMENT:
		{
			// pvr_frag_job_fw_cmd_init()
			if ((args.flags & ~PVR_SUBMIT_JOB_FRAG_CMD_FLAGS_MASK) != 0
				|| args.render == NULL || args.hwrt == NULL
				|| args.hwrt->fw_obj.cpu == NULL)
				return B_BAD_VALUE;
			job->render = args.render;
			job->queue = &args.render->frag;
			job->fw_ccb_cmd_type
				= (args.flags & PVR_SUBMIT_JOB_FRAG_CMD_PARTIAL_RENDER) != 0
					? ROGUE_FWIF_CCB_CMD_TYPE_FRAG_PR
					: ROGUE_FWIF_CCB_CMD_TYPE_FRAG;
			job->cmd_len = sizeof(rogue_fwif_cmd_frag);
			status = pvr_stream_parse_frag(desc, args.stream,
				args.stream_size, &job->cmd.frag);
			if (status != B_OK)
				return status;
			rogue_fwif_cmd_frag& cmd = job->cmd.frag;
			cmd.cmd_shared.cmn.frame_num = 0;
			cmd.flags = convert_frag_flags(args.flags);
			cmd.cmd_shared.hwrt_data_fw_addr = args.hwrt->fw_obj.fw_addr;
			break;
		}
		case PVR_JOB_TYPE_COMPUTE:
		{
			// pvr_compute_job_fw_cmd_init()
			if ((args.flags & ~PVR_SUBMIT_JOB_COMPUTE_CMD_FLAGS_MASK) != 0
				|| args.context == NULL
				|| args.context->type != PVR_CTX_TYPE_COMPUTE
				|| args.context->fw_common == NULL)
				return B_BAD_VALUE;
			job->queue = args.context;
			job->fw_ccb_cmd_type = ROGUE_FWIF_CCB_CMD_TYPE_CDM;
			job->cmd_len = sizeof(rogue_fwif_cmd_compute);
			status = pvr_stream_parse_compute(desc, args.stream,
				args.stream_size, &job->cmd.compute);
			if (status != B_OK)
				return status;
			job->cmd.compute.common.frame_num = 0;
			job->cmd.compute.flags = convert_compute_flags(args.flags);
			break;
		}
		case PVR_JOB_TYPE_TRANSFER_FRAG:
		{
			// pvr_transfer_job_fw_cmd_init()
			if ((args.flags & ~PVR_SUBMIT_JOB_TRANSFER_CMD_FLAGS_MASK) != 0
				|| args.context == NULL
				|| args.context->type != PVR_CTX_TYPE_TRANSFER_FRAG
				|| args.context->fw_common == NULL)
				return B_BAD_VALUE;
			job->queue = args.context;
			job->fw_ccb_cmd_type = ROGUE_FWIF_CCB_CMD_TYPE_TQ_3D;
			job->cmd_len = sizeof(rogue_fwif_cmd_transfer);
			status = pvr_stream_parse_transfer(desc, args.stream,
				args.stream_size, &job->cmd.transfer);
			if (status != B_OK)
				return status;
			job->cmd.transfer.common.frame_num = 0;
			job->cmd.transfer.flags = convert_transfer_flags(args.flags);
			break;
		}
		default:
			return B_BAD_VALUE;
	}

	if (job->queue->fw_common == NULL)
		return B_BAD_VALUE;

	// pvr_queue_job_init(): fragment jobs need at least one native fence
	// wait on the geometry job fence; Linux returns -E2BIG
	uint32 minWaits = job->type == PVR_JOB_TYPE_FRAGMENT ? 1 : 0;
	if (!pvr_cccb_cmdseq_can_fit(job->queue, job_cmds_size(job, minWaits)))
		return B_BAD_VALUE;

	return B_OK;
}


bool
pvr_jobs_can_combine(const pvr_job* a, const pvr_job* b)
{
	return a != NULL && b != NULL
		&& a->type == PVR_JOB_TYPE_GEOMETRY
		&& b->type == PVR_JOB_TYPE_FRAGMENT
		&& a->render == b->render && a->render != NULL
		&& a->hwrt == b->hwrt;
}


/*!	pvr_queue_submit_job_to_cccb(). The caller checked that everything fits.
	pairedGeom: the geometry job this fragment job is paired with, or NULL.
*/
static status_t
submit_job_to_cccb(const pvr_ctx_env& env, pvr_job* job,
	const pvr_job* pairedGeom, const pvr_job* pairedFrag)
{
	pvr_gpu_context* queue = job->queue;
	rogue_fwif_ufo ufos[ROGUE_FWIF_CCB_CMD_MAX_UFOS];
	uint32 ufoCount = 0;
	status_t status;

	for (uint32 i = 0; i < job->wait_count; i++) {
		ufos[ufoCount++] = job->waits[i];
		if (ufoCount == ARRAY_SIZE(ufos)) {
			status = pvr_cccb_write_command(env, queue,
				ROGUE_FWIF_CCB_CMD_TYPE_FENCE_PR, ufos, sizeof(ufos), 0, 0);
			if (status != B_OK)
				return status;
			ufoCount = 0;
		}
	}

	if (job->type == PVR_JOB_TYPE_FRAGMENT && pairedGeom != NULL) {
		// the paired geometry job's done fence, last, as partial render
		// fence
		ufos[ufoCount].addr = pairedGeom->queue->ufo_fw_addr;
		ufos[ufoCount++].value = pairedGeom->value;
	}

	if (ufoCount != 0) {
		status = pvr_cccb_write_command(env, queue,
			ROGUE_FWIF_CCB_CMD_TYPE_FENCE_PR, ufos,
			sizeof(ufos[0]) * ufoCount, 0, 0);
		if (status != B_OK)
			return status;
	}

	if (job->type == PVR_JOB_TYPE_GEOMETRY && pairedFrag != NULL) {
		// reference value of the partial render test: the current queue
		// fence seqno minus one
		rogue_fwif_cmd_geom& cmd = job->cmd.geom;
		cmd.partial_render_geom_frag_fence.addr = queue->ufo_fw_addr;
		cmd.partial_render_geom_frag_fence.value = job->value - 1;
	}

	// the job itself
	status = pvr_cccb_write_command(env, queue, job->fw_ccb_cmd_type,
		&job->cmd, job->cmd_len, job->id, job->id);
	if (status != B_OK)
		return status;

	// update command signalling the job fence
	ufos[0].addr = queue->ufo_fw_addr;
	ufos[0].value = job->value;
	return pvr_cccb_write_command(env, queue,
		ROGUE_FWIF_CCB_CMD_TYPE_UPDATE, ufos, sizeof(ufos[0]), 0, 0);
}


/*!	CCCB bytes of a job; a fragment job paired with a geometry job also
	waits on it.
*/
static uint32
job_size(const pvr_job* job, bool pairedFrag)
{
	return job_cmds_size(job, job->wait_count + (pairedFrag ? 1 : 0));
}


static bool
job_valid(const pvr_job* job)
{
	return job != NULL && job->queue != NULL
		&& (job->wait_count == 0 || job->waits != NULL);
}


/*!	Writes and kicks a job that is not paired; nothing stays written on
	failure. The caller checked the space.
*/
static status_t
job_write_and_kick(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_job* job, uint32* _kccbSlot)
{
	pvr_gpu_context* queue = job->queue;
	uint32 start = queue->cccb_write_offset;
	status_t status = submit_job_to_cccb(env, job, NULL, NULL);
	if (status == B_OK) {
		status = pvr_gpu_context_kick_hwrt(env, fwif, queue,
			job->hwrt != NULL ? pvr_hwrt_data_cleanup_ctl_fw_addr(job->hwrt)
				: 0, _kccbSlot);
	}
	if (status != B_OK) {
		// the firmware never saw these commands: drop them
		queue->cccb_write_offset = start;
		return status == B_BUSY ? B_WOULD_BLOCK : status;
	}
	return B_OK;
}


/*!	Writes a paired geometry and fragment job and sends the combined kick;
	nothing stays written on failure. The caller checked the space.
*/
static status_t
pair_write_and_kick(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_job* geom, pvr_job* frag, uint32* _kccbSlot)
{
	pvr_gpu_context* geomQueue = geom->queue;
	pvr_gpu_context* fragQueue = frag->queue;
	uint32 geomStart = geomQueue->cccb_write_offset;
	uint32 fragStart = fragQueue->cccb_write_offset;

	status_t status = submit_job_to_cccb(env, geom, NULL, frag);
	if (status == B_OK)
		status = submit_job_to_cccb(env, frag, geom, NULL);
	if (status == B_OK) {
		uint32 cleanup = pvr_hwrt_data_cleanup_ctl_fw_addr(geom->hwrt);
		// a partial render job does not attach the HWRT data: the
		// geometry job retains it already
		bool fragIsPr
			= frag->fw_ccb_cmd_type == ROGUE_FWIF_CCB_CMD_TYPE_FRAG_PR;
		status = pvr_gpu_context_kick_combined(env, fwif, geomQueue,
			fragQueue, cleanup, fragIsPr ? 0 : cleanup, _kccbSlot);
	}
	if (status != B_OK) {
		geomQueue->cccb_write_offset = geomStart;
		fragQueue->cccb_write_offset = fragStart;
		return status == B_BUSY ? B_WOULD_BLOCK : status;
	}
	return B_OK;
}


status_t
pvr_job_submit(const pvr_ctx_env& env, pvr_fwif_state* fwif, pvr_job* job,
	uint32* _kccbSlot)
{
	if (!job_valid(job))
		return B_BAD_VALUE;

	// pvr_queue_prepare_job(): CCCB space, then a KCCB slot
	if (!pvr_cccb_cmdseq_fits(env, job->queue, job_size(job, false))
		|| !pvr_kccb_slot_available(env, fwif)) {
		return B_WOULD_BLOCK;
	}

	return job_write_and_kick(env, fwif, job, _kccbSlot);
}


status_t
pvr_job_submit_paired(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_job* geom, pvr_job* frag, uint32* _kccbSlot)
{
	// pvr_queue_run_job(): paired jobs are geometry + fragment of the same
	// context and HWRT data
	if (!pvr_jobs_can_combine(geom, frag) || !job_valid(geom)
		|| !job_valid(frag))
		return B_BAD_VALUE;

	// CCCB space of both queues (the fragment job also waits on the
	// paired geometry job), then the one KCCB slot of the combined kick
	if (!pvr_cccb_cmdseq_fits(env, geom->queue, job_size(geom, false))
		|| !pvr_cccb_cmdseq_fits(env, frag->queue, job_size(frag, true))
		|| !pvr_kccb_slot_available(env, fwif)) {
		return B_WOULD_BLOCK;
	}

	return pair_write_and_kick(env, fwif, geom, frag, _kccbSlot);
}


static inline bool
pair_at(const pvr_job* jobs, uint32 count, uint32 index)
{
	return index + 1 < count && pvr_jobs_can_combine(&jobs[index],
		&jobs[index + 1]) && jobs[index + 1].depends_on_previous;
}


status_t
pvr_jobs_submit(const pvr_ctx_env& env, pvr_fwif_state* fwif, pvr_job* jobs,
	uint32 count, uint32* _submitted)
{
	if (_submitted != NULL)
		*_submitted = 0;
	if (jobs == NULL || count == 0)
		return B_BAD_VALUE;

	// pvr_jobs_link_geom_frag(): count the kicks
	uint32 kicks = 0;
	for (uint32 i = 0; i < count; i++) {
		if (!job_valid(&jobs[i]))
			return B_BAD_VALUE;
		if (pair_at(jobs, count, i)) {
			if (!job_valid(&jobs[i + 1]))
				return B_BAD_VALUE;
			i++;
		}
		kicks++;
	}

	// space of all jobs of a queue, at the first job of that queue
	for (uint32 i = 0; i < count; i++) {
		pvr_gpu_context* queue = jobs[i].queue;
		bool seen = false;
		for (uint32 j = 0; j < i && !seen; j++)
			seen = jobs[j].queue == queue;
		if (seen)
			continue;

		uint64 size = 0;
		bool pairedFrag = false;
		for (uint32 j = 0; j < count; j++) {
			bool frag = pairedFrag;
			pairedFrag = pair_at(jobs, count, j);
			if (jobs[j].queue == queue)
				size += job_size(&jobs[j], frag);
		}
		if (size > 0xFFFFFFFFu
			|| !pvr_cccb_cmdseq_can_fit(queue, (uint32)size)) {
			// could never fit at once
			return B_BAD_VALUE;
		}
		if (!pvr_cccb_cmdseq_fits(env, queue, (uint32)size))
			return B_WOULD_BLOCK;
	}

	if (pvr_kccb_free_slots(env, fwif) < kicks)
		return B_WOULD_BLOCK;

	for (uint32 i = 0; i < count; i++) {
		status_t status;
		if (pair_at(jobs, count, i)) {
			status = pair_write_and_kick(env, fwif, &jobs[i], &jobs[i + 1],
				NULL);
			if (status != B_OK)
				return status;
			i++;
		} else {
			status = job_write_and_kick(env, fwif, &jobs[i], NULL);
			if (status != B_OK)
				return status;
		}
		if (_submitted != NULL)
			*_submitted = i + 1;
	}
	return B_OK;
}
