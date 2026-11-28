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
#define POWERVR_JH7110_API_VERSION	1

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

struct powervr_create_buffer {
	uint64		size;				// rounded up to pages
	uint32		flags;				// none yet
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

struct powervr_unmap_buffer {
	uint64		device_address;
	uint64		size;
};

enum {
	POWERVR_CONTEXT_COMPUTE = 1,
};

struct powervr_create_context {
	uint32		type;				// POWERVR_CONTEXT_*
	uint32		priority;			// 0 low, 1 normal, 2 high
	int32		handle;				// out
	uint32		reserved;
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

struct powervr_statistics {
	uint32		jobs_submitted;
	uint32		jobs_completed;
	uint32		interrupts;
	uint32		mmu_page_faults;
	uint32		hardware_recoveries;
	uint32		firmware_restarts;
	uint32		reserved[10];
};


#endif	// _POWERVR_JH7110_H
