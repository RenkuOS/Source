/*
 * Copyright 2026, The RenkuOS authors.
 * Copyright (c) 2023 Imagination Technologies Ltd.
 * SPDX-License-Identifier: GPL-2.0-only OR MIT
 *
 * Render side of the PowerVR Rogue driver, ported from the Linux
 * drm/imagination driver:
 *  - contexts created from the UAPI static state streams (pvr_context.c:
 *    pvr_context_create(), init_*_fw_objs(), process_static_context_state()),
 *  - free lists (pvr_free_list.c), including the FWCCB grow and
 *    reconstruction requests,
 *  - HWRT data sets (pvr_hwrt.c),
 *  - jobs (pvr_job.c: create_job(), pvr_*_job_fw_cmd_init(),
 *    can_combine_jobs(); pvr_queue.c: pvr_queue_job_init(),
 *    pvr_queue_submit_job_to_cccb(), pvr_queue_run_job()).
 *
 * There is no DRM scheduler: a job is written into the client CCB(s) and
 * kicked at once, or not at all. The caller (the driver) owns handles,
 * reference counts, locking, sync objects and waiting:
 *  - every function here runs with the caller's device lock held (it
 *    serializes all KCCB and CCCB writers, as Linux' locks and the
 *    drm_sched model do);
 *  - before a submission the caller makes the client's page table changes
 *    visible (pvr_mmu_flush_exec(), as pvr_submit_jobs() does);
 *  - after creating any object with firmware objects (contexts, free
 *    lists, HWRT data sets) the caller flushes the MMU caches with
 *    PVR_MMU_SYNC_LEVEL_2_FLAGS before the firmware may use them (new
 *    pages in the MIPS page table, pvr_vm_mips.c:199), and writes back the
 *    MIPS page table entries its fw_alloc hook created;
 *  - it waits for the timeline UFO values of the jobs (pvr_gpu_context
 *    ufo) to know when they are done.
 *
 * Lifetimes (Linux keeps these with reference counts):
 *  - a job references its context(s) and HWRT data until its timeline UFO
 *    reached its value;
 *  - a HWRT data set references its two free lists: destroy every HWRT
 *    data set using a free list before the free list
 *    (pvr_free_list_destroy() refuses with B_BUSY otherwise);
 *  - destruction is two steps: *_cleanup() asks the firmware to release
 *    the object (it answers busy while it still uses it), *_destroy()
 *    frees the memory. Never free after a failed cleanup: the firmware may
 *    still access the object; keep it until the firmware is stopped.
 */
#ifndef PVR_RENDER_H
#define PVR_RENDER_H


#include "pvr_context.h"
#include "pvr_stream.h"


/* drm_pvr_ctx_type */
#define PVR_UAPI_CTX_TYPE_RENDER				0
#define PVR_UAPI_CTX_TYPE_COMPUTE				1
#define PVR_UAPI_CTX_TYPE_TRANSFER_FRAG			2

/* drm_pvr_job_type */
#define PVR_JOB_TYPE_GEOMETRY					0
#define PVR_JOB_TYPE_FRAGMENT					1
#define PVR_JOB_TYPE_COMPUTE					2
#define PVR_JOB_TYPE_TRANSFER_FRAG				3

/* drm_pvr_job::flags, the DRM_PVR_SUBMIT_JOB_* values */
#define PVR_SUBMIT_JOB_GEOM_CMD_FIRST			(1U << 0)
#define PVR_SUBMIT_JOB_GEOM_CMD_LAST			(1U << 1)
#define PVR_SUBMIT_JOB_GEOM_CMD_SINGLE_CORE		(1U << 2)
#define PVR_SUBMIT_JOB_GEOM_CMD_FLAGS_MASK		0x7U

#define PVR_SUBMIT_JOB_FRAG_CMD_SINGLE_CORE		(1U << 0)
#define PVR_SUBMIT_JOB_FRAG_CMD_DEPTHBUFFER		(1U << 1)
#define PVR_SUBMIT_JOB_FRAG_CMD_STENCILBUFFER	(1U << 2)
#define PVR_SUBMIT_JOB_FRAG_CMD_PREVENT_CDM_OVERLAP (1U << 3)
#define PVR_SUBMIT_JOB_FRAG_CMD_SCRATCHBUFFER	(1U << 4)
#define PVR_SUBMIT_JOB_FRAG_CMD_GET_VIS_RESULTS	(1U << 5)
#define PVR_SUBMIT_JOB_FRAG_CMD_PARTIAL_RENDER	(1U << 6)
#define PVR_SUBMIT_JOB_FRAG_CMD_DISABLE_PIXELMERGE (1U << 7)
#define PVR_SUBMIT_JOB_FRAG_CMD_FLAGS_MASK		0xFFU

#define PVR_SUBMIT_JOB_COMPUTE_CMD_PREVENT_ALL_OVERLAP (1U << 0)
#define PVR_SUBMIT_JOB_COMPUTE_CMD_SINGLE_CORE	(1U << 1)
#define PVR_SUBMIT_JOB_COMPUTE_CMD_FLAGS_MASK	0x3U

#define PVR_SUBMIT_JOB_TRANSFER_CMD_SINGLE_CORE	(1U << 0)
#define PVR_SUBMIT_JOB_TRANSFER_CMD_FLAGS_MASK	0x1U

/* pvr_free_list.c */
#define PVR_FREE_LIST_ENTRY_SIZE				sizeof(uint32)
#define PVR_FREE_LIST_ALIGNMENT \
	((ROGUE_BIF_PM_FREELIST_BASE_ADDR_ALIGNSIZE / PVR_FREE_LIST_ENTRY_SIZE) \
		- 1)
	/* page counts must be multiples of 4 */


//	#pragma mark - device derived sizes


/*!	pvr_get_free_list_min_pages(): 25 on roguexe cores (40 with BRN 66011),
	50 otherwise. The local free list of a HWRT data set needs at least
	that many pages.
*/
uint32 pvr_get_free_list_min_pages(const pvr_device_desc& desc);
/*!	get_frag_ctx_state_size(): ISP store registers of a fragment context
	state (4 on BXE-4-32: 1 raster pipe * (1 + 3 XPU slaves)).
*/
uint32 pvr_frag_ctx_isp_store_registers(const pvr_device_desc& desc);
/*!	get_xfer_ctx_state_size(): idem for a transfer context (1 with
	xe_memory_hierarchy, else num_isp_ipp_pipes).
*/
uint32 pvr_xfer_ctx_isp_store_registers(const pvr_device_desc& desc);


//	#pragma mark - contexts


/* drm_pvr_ioctl_create_context_args, resolved by the caller */
struct pvr_context_create_args {
	uint32			type;
		// PVR_UAPI_CTX_TYPE_*
	uint32			priority;
		// PVR_CTX_PRIORITY_* (the caller maps the DRM priorities and checks
		// the right to HIGH, as remap_priority())
	uint32			ctx_id;
		// server_common_context_id: device unique, Linux allocates it from 1
	uint32			pid;
	uint32			cccb_size_log2;
		// 0: Linux' 15 (32 KB)
	const void*		static_state;
		// kernel copy of the static_context_state stream of
		// drm_pvr_ioctl_create_context_args
	uint32			static_state_len;
		// render and compute: required; transfer: must be 0
	uint64			callstack_addr;
		// render only; must be 0 for the other types
};

/*!	DRM_PVR_CTX_TYPE_RENDER: parses the static render context state stream
	and creates the render context (pvr_render_context_create()).
*/
status_t pvr_render_context_create_uapi(const pvr_ctx_env& env,
	const pvr_device_desc& desc, pvr_fw_mem_context* memContext,
	const pvr_context_create_args& args, pvr_render_context* context);
/*!	DRM_PVR_CTX_TYPE_COMPUTE (parses the static compute context state
	stream) or DRM_PVR_CTX_TYPE_TRANSFER_FRAG (no static state): creates a
	pvr_gpu_context. Destroy with pvr_gpu_context_cleanup()/_destroy().
*/
status_t pvr_gpu_context_create_uapi(const pvr_ctx_env& env,
	const pvr_device_desc& desc, pvr_fw_mem_context* memContext,
	const pvr_context_create_args& args, pvr_gpu_context* context);


//	#pragma mark - free lists


/*
 * Free list memory, as in Linux:
 *  - the page list is a buffer of the client (allocated by userspace with
 *    DRM_PVR_BO_PM_FW_PROTECT, not CPU accessible to userspace) mapped in
 *    the client's GPU address space at free_list_gpu_addr; the kernel
 *    writes into it one u32 per PM page: the physical page frame number
 *    (physical address >> 12). The parameter manager reads it through the
 *    client's page tables. The caller looks the buffer up, checks its flags
 *    and size (>= max_num_pages * 4), keeps it alive for the lifetime of
 *    the free list and gives it to this module as an opaque page_list
 *    cookie, written through page_list_write;
 *  - the PM pages themselves (Linux: one GEM object per grow, never mapped
 *    in any GPU VM) are only referenced by physical address. They are
 *    allocated through pm_pages_alloc; they need not be contiguous and are
 *    NOT mapped in the GPU MMU.
 *
 * All hooks are called with the caller's lock held, from create, the grow
 * request, reconstruction and destroy.
 */
struct pvr_free_list_hooks {
	void*		cookie;

	/*!	Allocates count 4 KB pages for the parameter manager: zeroed, the
		CPU caches written back (the GPU reads them uncached), each below
		2^44 (the PFN is a u32; the bus is 36 bit here). Returns an opaque
		block. B_NO_MEMORY makes the grow fail (the firmware is then told
		it got 0 pages, as Linux).
	*/
	status_t	(*pm_pages_alloc)(void* cookie, uint32 count, void** _block);
	/*!	Physical address of page index (0 .. count - 1) of block. */
	uint64		(*pm_page_phys)(void* cookie, void* block, uint32 index);
	/*!	Frees a block (after the firmware released the free list). */
	void		(*pm_pages_free)(void* cookie, void* block);

	/*!	Writes count entries at byteOffset into the page list buffer and
		makes them visible to the GPU before returning (Linux writes
		through a write-combined mapping, then wmb()). The parameter manager
		writes entries of the same buffer while the GPU runs, so the write
		must not write back any other byte of the buffer: use an uncached or
		write-combined kernel mapping, not a cached one with a cache line
		write back. byteOffset is 16 byte aligned, count a multiple of 4.
	*/
	status_t	(*page_list_write)(void* cookie, void* pageList,
					uint32 byteOffset, const uint32* entries, uint32 count);
};


struct pvr_free_list_node;
struct pvr_hwrt_data;

struct pvr_free_list {
	pvr_free_list_hooks			hooks;
	void*						page_list;
	uint64						free_list_gpu_addr;

	pvr_fw_obj					fw_obj;
	struct rogue_fwif_freelist*	fw_data;
	uint32						fw_id;

	uint32						current_pages;
	uint32						max_pages;
	uint32						grow_pages;
	uint32						grow_threshold;
	uint32						ready_pages;
	uint32						initial_num_pages;

	pvr_free_list_node*			nodes;		// mem_block_list, in order
	pvr_free_list_node*			last_node;
	pvr_hwrt_data*				hwrt_list;	// HWRT data using it as local
	uint32						hwrt_count;	// HWRT data sets referencing it

	bool						reply_pending;
	struct rogue_fwif_kccb_cmd	reply;
};

/* drm_pvr_ioctl_create_free_list_args, resolved by the caller */
struct pvr_free_list_create_args {
	uint64		free_list_gpu_addr;
	uint32		initial_num_pages;
	uint32		max_num_pages;
	uint32		grow_num_pages;
	uint32		grow_threshold;
		// percent, <= 100
	uint32		fw_id;
		// device unique ID the firmware uses in FREELIST_GROW and
		// FREELISTS_RECONSTRUCTION requests (Linux: xa_alloc from 1)
	void*		page_list;
		// opaque, for hooks.page_list_write
	uint64		page_list_size;
		// size of the buffer mapped at free_list_gpu_addr
};

/*!	pvr_free_list_create(): validates the arguments as
	free_list_create_kernel_structure(), creates and syncs the FW free list
	object, then grows the list by initial_num_pages (PM pages + page list
	entries). The caller then flushes the MMU caches (new FW object).
	On failure everything is undone (Linux sends a FREELIST cleanup on that
	path; the firmware never saw the object here, so none is needed).
*/
status_t pvr_free_list_create(const pvr_ctx_env& env,
	const pvr_free_list_hooks& hooks, const pvr_free_list_create_args& args,
	pvr_free_list* freeList);

/*!	ROGUE_FWIF_CLEANUP_FREELIST, retried until timeout. Linux drains the
	FWCCB once when the firmware answers busy (pending HWR/reconstruction
	requests may hold the free list) and tries again: the caller does the
	same between two calls.
*/
status_t pvr_free_list_cleanup(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_free_list* freeList, bigtime_t timeout);
/*!	Frees the PM pages, the node bookkeeping and the FW object. B_BUSY (and
	nothing freed) while HWRT data sets still use the free list. Only after
	pvr_free_list_cleanup() returned B_OK (or after a failed create). The
	caller releases its page list buffer afterwards.
*/
status_t pvr_free_list_destroy(const pvr_ctx_env& env,
	pvr_free_list* freeList);

/*!	pvr_free_list_process_grow_req(): the reply to
	ROGUE_FWIF_FWCCB_CMD_FREELIST_GROW (the driver reads cmd_data as
	rogue_fwif_fwccb_cmd_freelist_gs_data and looks the free list up by
	freelist_id; with no such free list Linux only warns and sends no
	reply). Takes back the ready pages the firmware consumed, grows the
	list by grow_pages (0 if that fails or would exceed max_pages), then
	sends ROGUE_FWIF_KCCB_CMD_FREELIST_GROW_UPDATE (freelist FW address,
	delta, new and ready page counts).
	B_BUSY: the grow was done but the KCCB was full; the reply is kept in
	the free list: call pvr_free_list_send_pending_reply() until it returns
	B_OK, before handling further FWCCB commands. Never call this function
	again for the same request.
*/
status_t pvr_free_list_process_grow_req(const pvr_ctx_env& env,
	pvr_fwif_state* fwif, pvr_free_list* freeList);
/*!	Resends a reply kept by pvr_free_list_process_grow_req(); B_OK if none
	is pending.
*/
status_t pvr_free_list_send_pending_reply(const pvr_ctx_env& env,
	pvr_fwif_state* fwif, pvr_free_list* freeList);

/*!	Looks a free list up by its fw_id (NULL if unknown); for the
	reconstruction request, which names several free lists.
*/
typedef pvr_free_list* (*pvr_free_list_lookup_hook)(void* cookie,
	uint32 fwId);

/*!	pvr_free_list_process_reconstruct_req(): the reply to
	ROGUE_FWIF_FWCCB_CMD_FREELISTS_RECONSTRUCTION after a hardware
	recovery. Rebuilds the page list of each named free list from its PM
	pages, resets its FW state (stack top, allocated counts) and puts its
	HWRT data in state HWR, then sends
	ROGUE_FWIF_KCCB_CMD_FREELISTS_RECONSTRUCTION_UPDATE with the same IDs.
	These host writes go to FW objects the firmware also writes; it does
	not touch them while it waits for this reply. B_BUSY: the KCCB was full
	and the reconstruction was done; the caller resends the reply itself
	with pvr_kccb_send_cmd(*_reply) (filled whenever _reply is not NULL).
*/
status_t pvr_free_list_process_reconstruct_req(const pvr_ctx_env& env,
	pvr_fwif_state* fwif,
	const struct rogue_fwif_fwccb_cmd_freelists_reconstruction_data* request,
	pvr_free_list_lookup_hook lookup, void* lookupCookie,
	struct rogue_fwif_kccb_cmd* _reply);


//	#pragma mark - HWRT data sets


struct pvr_hwrt_dataset;

struct pvr_hwrt_data {
	pvr_hwrt_dataset*			dataset;
	pvr_fw_obj					fw_obj;		// struct rogue_fwif_hwrtdata
	pvr_fw_obj					srtc_obj;	// only with layers > 1
	pvr_fw_obj					raa_obj;	// only with layers > 1
	pvr_hwrt_data*				free_list_next;
};

struct pvr_hwrt_dataset {
	pvr_fw_obj					common_obj;	// rogue_fwif_hwrtdata_common
	pvr_hwrt_data				data[ROGUE_FWIF_NUM_RTDATAS];
	uint32						data_count;	// created
	pvr_free_list*				free_lists[ROGUE_FWIF_NUM_RTDATA_FREELISTS];
	uint32						max_rts;
};

/* drm_pvr_ioctl_create_hwrt_dataset_args, resolved by the caller */
struct pvr_hwrt_create_args {
	// drm_pvr_create_hwrt_geom_data_args
	uint64			tpc_dev_addr;
	uint32			tpc_size;
	uint32			tpc_stride;
	uint64			vheap_table_dev_addr;
	uint64			rtc_dev_addr;
	// drm_pvr_create_hwrt_rt_data_args
	struct {
		uint64		pm_mlist_dev_addr;
		uint64		macrotile_array_dev_addr;
		uint64		region_header_dev_addr;
	} rt_data[ROGUE_FWIF_NUM_RTDATAS];
	// free_list_handles, looked up: [ROGUE_FW_LOCAL_FREELIST],
	// [ROGUE_FW_GLOBAL_FREELIST]
	pvr_free_list*	free_lists[ROGUE_FWIF_NUM_RTDATA_FREELISTS];
	uint32			width;
	uint32			height;
	uint32			samples;
	uint32			layers;
	uint32			isp_merge_lower_x;
	uint32			isp_merge_lower_y;
	uint32			isp_merge_scale_x;
	uint32			isp_merge_scale_y;
	uint32			isp_merge_upper_x;
	uint32			isp_merge_upper_y;
	uint32			region_header_size;
};

/*!	pvr_hwrt_dataset_create(): checks the local free list has at least
	pvr_get_free_list_min_pages() pages, builds and syncs the HWRT common
	object and, for both render targets, the HWRT data object (plus the
	shadow RT cache and RT array partial render arrays when layers > 1),
	and links the data to the local free list. Takes a reference on both
	free lists (hwrt_count). The caller then flushes the MMU caches.
*/
status_t pvr_hwrt_dataset_create(const pvr_ctx_env& env,
	const pvr_device_desc& desc, const pvr_hwrt_create_args& args,
	pvr_hwrt_dataset* dataset);
/*!	ROGUE_FWIF_CLEANUP_HWRTDATA of data[1], then data[0] (retried until
	timeout each). Busy while a kicked job still references the data (see
	pvr_gpu_context_kick_hwrt()).
*/
status_t pvr_hwrt_dataset_cleanup(const pvr_ctx_env& env,
	pvr_fwif_state* fwif, pvr_hwrt_dataset* dataset, bigtime_t timeout);
/*!	Unlinks and frees the data objects, the common object, and drops the
	free list references. Only after pvr_hwrt_dataset_cleanup() returned
	B_OK (or after a failed create).
*/
void pvr_hwrt_dataset_destroy(const pvr_ctx_env& env,
	pvr_hwrt_dataset* dataset);

/*!	FW address of the cleanup control of a HWRT data, for the kicks. */
uint32 pvr_hwrt_data_cleanup_ctl_fw_addr(const pvr_hwrt_data* data);


//	#pragma mark - jobs


/* drm_pvr_job, resolved by the caller */
struct pvr_job_args {
	uint32					type;
		// PVR_JOB_TYPE_*
	uint32					flags;
		// PVR_SUBMIT_JOB_*_CMD_* of the type
	const void*				stream;
		// kernel copy of cmd_stream
	uint32					stream_size;
	pvr_render_context*		render;
		// geometry and fragment jobs
	pvr_gpu_context*		context;
		// compute and transfer jobs (of the matching context type)
	pvr_hwrt_data*			hwrt;
		// &dataset->data[data_index]: required for geometry and fragment
		// jobs, must be NULL for the others
	uint32					id;
		// job ID for the firmware traces (ext/int job reference)
};

struct pvr_job {
	uint32					type;
	uint32					flags;
	uint32					id;
	uint32					fw_ccb_cmd_type;
	uint32					cmd_len;
	pvr_render_context*		render;
	pvr_gpu_context*		queue;
		// the queue the job goes to (render->geom, render->frag or context)
	pvr_hwrt_data*			hwrt;

	// set by the caller before submitting
	uint32					value;
		// timeline value written to queue->ufo when the job is done. Per
		// queue the values must increase by exactly 1 per job (the dma_fence
		// seqno of Linux): the partial render fence of a geometry job is
		// value - 1, the value of the previous geometry job.
	const struct rogue_fwif_ufo* waits;
	uint32					wait_count;
		// UFOs (addr, value) the job waits for (Linux: the not yet signalled
		// native dependencies), e.g. the timeline UFO of another queue; any
		// count, written as FENCE_PR commands of up to 64 UFOs
	bool					depends_on_previous;
		// fragment job with an explicit dependency on the job before it in
		// the submit array (drm_sched_job_has_dependency() on its scheduled
		// fence): makes pvr_jobs_submit() pair it with that geometry job

	union {
		struct rogue_fwif_cmd_geom		geom;
		struct rogue_fwif_cmd_frag		frag;
		struct rogue_fwif_cmd_compute	compute;
		struct rogue_fwif_cmd_transfer	transfer;
	} cmd;
};

/*!	create_job() + pvr_job_fw_cmd_init() + pvr_queue_job_init(): checks the
	type, flags, context and HWRT, parses the stream into job->cmd, sets the
	kernel owned fields (frame number 0, converted flags, HWRT data FW
	address) and the CCCB command type (FRAG_PR for a fragment job with
	PVR_SUBMIT_JOB_FRAG_CMD_PARTIAL_RENDER), and checks the job can fit in
	its CCCB at all. value/waits are cleared. B_BAD_VALUE on any error.
*/
status_t pvr_job_init(const pvr_device_desc& desc, const pvr_job_args& args,
	pvr_job* job);

/*!	can_combine_jobs() without the dependency test: geometry job a and
	fragment job b of the same render context and HWRT data. Linux only
	combines when b also has an explicit dependency on a (Mesa's partial
	render job always has one, through its geom-to-PR sync object): the
	caller checks that before calling pvr_job_submit_paired(). Pairing is
	done for consecutive jobs of the submit array (a = jobs[i],
	b = jobs[i + 1], then i += 2), as pvr_jobs_link_geom_frag().
*/
bool pvr_jobs_can_combine(const pvr_job* a, const pvr_job* b);

/*!	pvr_queue_submit_job_to_cccb() + kick of a job that is not paired:
	[FENCE_PR(waits)], the DM command, UPDATE(queue UFO := value), then
	KICK with the HWRT cleanup control when the job has HWRT data (Linux
	attaches it for any job with HWRT data on this path, partial render
	included). All or nothing: B_WOULD_BLOCK, nothing written, when the
	commands do not fit in the CCCB or no KCCB slot is free now; the caller
	retries once the firmware made progress.
*/
status_t pvr_job_submit(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_job* job, uint32* _kccbSlot);

/*!	pvr_queue_run_job() of a geometry job paired with a fragment job
	(normally the partial render job that follows it in the submit array):
	 - geometry CCCB: [FENCE_PR(geom waits)], GEOM with
	   partial_render_geom_frag_fence = (geometry UFO, geom->value - 1),
	   UPDATE(geometry UFO := geom->value);
	 - fragment CCCB: [FENCE_PR(frag waits..., then (geometry UFO,
	   geom->value))], FRAG or FRAG_PR, UPDATE(fragment UFO := frag->value);
	 - one COMBINED_GEOM_FRAG_KICK, with the HWRT cleanup control on the
	   geometry side and, unless the fragment command is FRAG_PR, on the
	   fragment side.
	The real fragment job that follows (third job of Mesa's render submit)
	is submitted on its own with pvr_job_submit(): it goes after the
	partial render job in the same fragment CCCB. All or nothing as
	pvr_job_submit() (both CCCBs and the KCCB are checked first).
*/
status_t pvr_job_submit_paired(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_job* geom, pvr_job* frag, uint32* _kccbSlot);

/*!	pvr_submit_jobs() for an array of initialized jobs (value, waits and
	depends_on_previous set): pairs jobs[i] and jobs[i + 1] when
	pvr_jobs_can_combine() and jobs[i + 1].depends_on_previous
	(pvr_jobs_link_geom_frag()), checks that the commands of all jobs fit
	in their CCCBs together (summed per queue) and that the KCCB has a slot
	for every kick, and only then writes and kicks them in array order.
	Mesa's render submit (geometry, partial render, fragment) thus becomes
	one combined kick plus one fragment kick, or nothing.
	B_WOULD_BLOCK: nothing was submitted (the caller must not consume the
	timeline values; it retries later). Any other error after the checks
	(only possible if someone else wrote the KCCB meanwhile) leaves the
	first *_submitted jobs submitted.
*/
status_t pvr_jobs_submit(const pvr_ctx_env& env, pvr_fwif_state* fwif,
	pvr_job* jobs, uint32 count, uint32* _submitted);


#endif	// PVR_RENDER_H
