/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * Interface of the driver of the Imagination PowerVR BXE-4-32 GPU of the
 * StarFive JH7110 (Milk-V Mars, VisionFive 2): /dev/gpu/powervr_jh7110.
 *
 * Every open file is a client with its own GPU address space (as a Linux
 * pvr VM context): buffers are mapped into it at addresses the client
 * chooses in the general heap, and its compute contexts run there. A buffer
 * is a kernel area the driver clones into the calling team; the CPU sees it
 * through the cache, and the driver writes the caches back around every job
 * (the JH7110 is not cache coherent).
 */
#ifndef _POWERVR_JH7110_H
#define _POWERVR_JH7110_H


#include <Drivers.h>
#include <OS.h>


#define POWERVR_JH7110_DEVICE		"/dev/gpu/powervr_jh7110"
#define POWERVR_JH7110_API_VERSION	2
	// 2: render and transfer contexts, free lists, HWRT data sets and
	// POWERVR_SUBMIT_JOBS; version 1 clients keep working

// general heap of the GPU address space (pvr_rogue_heap_config.h)
#define POWERVR_GENERAL_HEAP_BASE	0x8000000000ULL
#define POWERVR_GENERAL_HEAP_SIZE	0x2000000000ULL


enum {
	POWERVR_GET_INFO = B_DEVICE_OP_CODES_END + 0x5000,
	POWERVR_CREATE_BUFFER,
	POWERVR_DELETE_BUFFER,
	POWERVR_MAP_BUFFER,
	POWERVR_UNMAP_BUFFER,
	POWERVR_CREATE_CONTEXT,
	POWERVR_DELETE_CONTEXT,
	POWERVR_SUBMIT_COMPUTE,
	POWERVR_WAIT,
	POWERVR_GET_STATISTICS,
	POWERVR_CREATE_FREE_LIST,
	POWERVR_DELETE_FREE_LIST,
	POWERVR_CREATE_HWRT_DATASET,
	POWERVR_DELETE_HWRT_DATASET,
	POWERVR_SUBMIT_JOBS,
	POWERVR_SET_BUFFER_CPU_ACCESS,
	POWERVR_GET_PERF_COUNTERS,
};


struct powervr_info {
	uint32		api_version;
	uint32		core_clock_mhz;
	uint64		bvnc;				// 36.50.54.182 as 0x00240032003600b6
	uint16		firmware_major;
	uint16		firmware_minor;
	uint32		firmware_build;
	uint64		general_heap_base;
	uint64		general_heap_size;
	uint32		page_size;
	uint32		reserved[7];
};

#define POWERVR_BUFFER_SYNC_WHEN_MAPPED	0x01
	// the caches are only kept in step with the GPU while the client says
	// it accesses the buffer with the CPU (POWERVR_SET_BUFFER_CPU_ACCESS);
	// without it, every mapped buffer is synced around every job
#define POWERVR_BUFFER_UNCACHED			0x02
	// the clone in the caller's team bypasses the CPU caches (the uncached
	// port of the DDR): never synced, fast for the CPU to write, slow to read

struct powervr_create_buffer {
	uint64		size;				// rounded up to pages
	uint32		flags;				// POWERVR_BUFFER_*
	int32		handle;				// out
	area_id		area;				// out: cloned into the caller's team
	uint32		reserved;
	void*		address;			// out
};
	// The pages are below 4 GB.

struct powervr_map_buffer {
	int32		handle;
	uint32		flags;				// POWERVR_MAP_*
	uint64		offset;				// into the buffer, page aligned
	uint64		size;				// page granular, 0 = rest of the buffer
	uint64		device_address;		// page aligned, inside one of the heaps
									// of the Rogue layout: general, PDS
									// code and data, USC code, transfer
									// fragment, vis test
};

#define POWERVR_MAP_READ_ONLY		0x01

// The client starts (access != 0) or stops using a buffer created with
// POWERVR_BUFFER_SYNC_WHEN_MAPPED with the CPU; the calls nest.
struct powervr_buffer_cpu_access {
	int32		handle;
	uint32		access;
};

struct powervr_unmap_buffer {
	uint64		device_address;
	uint64		size;
};

enum {
	POWERVR_CONTEXT_COMPUTE = 1,
	POWERVR_CONTEXT_RENDER,			// geometry and fragment
	POWERVR_CONTEXT_TRANSFER,		// transfer fragment
};

struct powervr_create_context {
	uint32		type;				// POWERVR_CONTEXT_*
	uint32		priority;			// 0 low, 1 normal, 2 high
	int32		handle;				// out
	uint32		reserved;
	// version 2, optional for compute: the context's static state as
	// Mesa's pvr driver packs it (the stream of DRM_PVR_STATIC_*, as
	// drm_pvr_ioctl_create_context_args::static_context_state)
	const void*	static_state;
	uint32		static_state_size;
	uint32		reserved2;
	uint64		callstack_address;	// render: the VDM call stack
};

// A free list of the parameter buffer (drm_pvr_ioctl_create_free_list_args).
// The driver allocates the pages and writes their addresses into the list
// at free_list_address, a mapped range of at least max_pages entries.
struct powervr_create_free_list {
	uint64		free_list_address;
	uint32		initial_pages;
	uint32		max_pages;
	uint32		grow_pages;
	uint32		grow_threshold;		// percent
	int32		handle;				// out
	uint32		reserved;
};

#define POWERVR_HWRT_FREE_LIST_LOCAL	0
#define POWERVR_HWRT_FREE_LIST_GLOBAL	1

// Render target data (drm_pvr_ioctl_create_hwrt_dataset_args): two sets of
// render data sharing one geometry data, so two frames can be in flight.
struct powervr_create_hwrt_dataset {
	uint64		tpc_address;
	uint32		tpc_size;
	uint32		tpc_stride;
	uint64		vheap_table_address;
	uint64		rtc_address;
	struct {
		uint64	pm_mlist_address;
		uint64	macrotile_array_address;
		uint64	region_header_address;
	} rt_data[2];
	int32		free_lists[2];		// POWERVR_HWRT_FREE_LIST_*, both
									// required (the render target's and
									// the global one it grows from)
	uint32		width;
	uint32		height;
	uint32		samples;
	uint32		layers;
	uint32		isp_merge_lower_x;
	uint32		isp_merge_lower_y;
	uint32		isp_merge_scale_x;
	uint32		isp_merge_scale_y;
	uint32		isp_merge_upper_x;
	uint32		isp_merge_upper_y;
	uint32		region_header_size;
	int32		handle;				// out
};

// The CDM registers of a compute job (rogue_fwif_compute_regs), packed as
// the hardware wants them (Mesa csbgen/rogue/cr.xml).
struct powervr_compute_registers {
	uint64		tpu_border_colour_table;
	uint64		cdm_ctrl_stream_base;
	uint64		cdm_context_state_base;
	uint32		cdm_resume_pds1;
	uint32		cdm_item;
	uint32		compute_cluster;
	uint32		tpu_tag_cdm_ctrl;
};

#define POWERVR_SUBMIT_PREVENT_ALL_OVERLAP	0x04	// ROGUE_COMPUTE_FLAG_*
#define POWERVR_SUBMIT_SINGLE_CORE			0x20

struct powervr_submit_compute {
	int32		context;
	uint32		flags;				// POWERVR_SUBMIT_*
	struct powervr_compute_registers registers;
	uint32		sequence;			// out: wait for this value
	uint32		reserved;
};

#define POWERVR_WAIT_CONTEXT_RESET	0x01
	// a job of the context was stopped by a hardware recovery and skipped
#define POWERVR_WAIT_PAGE_FAULT		0x02
	// ... because of a page fault, at fault_address

enum {
	POWERVR_JOB_GEOMETRY = 0,		// drm_pvr_job_type
	POWERVR_JOB_FRAGMENT,
	POWERVR_JOB_COMPUTE,
	POWERVR_JOB_TRANSFER,
};

// flags of a job, as DRM_PVR_SUBMIT_JOB_*_CMD_*
#define POWERVR_GEOMETRY_FIRST				0x01
#define POWERVR_GEOMETRY_LAST				0x02
#define POWERVR_GEOMETRY_SINGLE_CORE		0x04
#define POWERVR_FRAGMENT_SINGLE_CORE		0x01
#define POWERVR_FRAGMENT_DEPTH_BUFFER		0x02
#define POWERVR_FRAGMENT_STENCIL_BUFFER		0x04
#define POWERVR_FRAGMENT_PREVENT_CDM_OVERLAP	0x08
#define POWERVR_FRAGMENT_SCRATCH_BUFFER		0x10
#define POWERVR_FRAGMENT_GET_VIS_RESULTS	0x20
#define POWERVR_FRAGMENT_PARTIAL_RENDER		0x40
#define POWERVR_FRAGMENT_DISABLE_PIXEL_MERGE	0x80
#define POWERVR_COMPUTE_PREVENT_ALL_OVERLAP	0x01
#define POWERVR_COMPUTE_SINGLE_CORE			0x02
#define POWERVR_TRANSFER_SINGLE_CORE		0x01

// One job (drm_pvr_job): stream is the command stream Mesa's pvr driver
// builds (pvr_rogue_fwif_stream.h); flags are DRM_PVR_SUBMIT_JOB_*_CMD_*.
// A render is a geometry job, a partial render fragment job (run only if
// the parameter buffer fills up) and optionally the fragment job, of the
// same context and data set, in one submit, as Mesa's pvr driver does.
struct powervr_job {
	uint32		type;				// POWERVR_JOB_*
	int32		context;
	uint32		flags;
	uint32		stream_size;
	const void*	stream;
	int32		hwrt_dataset;		// geometry and fragment, else -1
	uint32		hwrt_index;			// 0 or 1
	uint32		sequence;			// out: POWERVR_WAIT for this value
	uint32		reserved;
};

// Jobs are queued in order without waiting; a context's sequence numbers
// count all its jobs, and a completed sequence means every job of the
// context up to it has completed.
struct powervr_submit_jobs {
	struct powervr_job* jobs;
	uint32		count;
	uint32		reserved;
};

struct powervr_wait {
	int32		context;
	uint32		sequence;
	bigtime_t	timeout;			// relative, B_INFINITE_TIMEOUT allowed
	uint32		completed;			// out: last completed sequence
	uint32		flags;				// out: POWERVR_WAIT_*, since the last wait
	uint32		reset_reason;		// out: rogue_context_reset_reason
	uint32		reserved;
	uint64		fault_address;		// out
};

// The free running performance counters of the GPU (ROGUE_CR_PERF_*):
// 32 bit, they wrap around every few seconds at the core clock. Phases count
// the geometry (TA), fragment (3D) and compute jobs run, cycles the clock
// cycles the GPU spent in each, stalls the cycles a cache (SLC) bank waited
// for memory.
struct powervr_perf_counters {
	bigtime_t	time;				// when they were read
	uint32		core_clock_mhz;
	uint32		ta_phases;
	uint32		fragment_phases;
	uint32		compute_phases;
	uint32		ta_cycles;
	uint32		fragment_cycles;
	uint32		compute_cycles;
	uint32		ta_or_fragment_cycles;
	uint32		slc_read_stalls;
	uint32		slc_write_stalls;
	uint32		reserved[6];
};

struct powervr_statistics {
	uint32		jobs_submitted;
	uint32		jobs_completed;
	uint32		interrupts;
	uint32		mmu_page_faults;
	uint32		hardware_recoveries;
	uint32		firmware_restarts;
	uint32		free_list_grows;		// version 2: parameter buffer grows
	uint32		free_list_reconstructions;	// ... and rebuilds after a reset
	uint32		cache_sync_megabytes;	// written back around the jobs
	uint32		cache_sync_milliseconds;
	uint32		wait_milliseconds;		// clients blocked in POWERVR_WAIT
	uint32		reserved[5];
};


#endif	// _POWERVR_JH7110_H
