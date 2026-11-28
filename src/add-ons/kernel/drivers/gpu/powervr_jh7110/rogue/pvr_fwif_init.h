/*
 * Copyright 2026, The RenkuOS authors.
 * Copyright (c) 2023 Imagination Technologies Ltd.
 * SPDX-License-Identifier: GPL-2.0-only OR MIT
 *
 * Creation of the firmware interface (FWIF) structures of the PowerVR Rogue
 * firmware for a MIPS firmware processor, ported from the Linux
 * drm/imagination driver (pvr_fw.c: pvr_fw_create_structures() and the
 * fw_*_init() callbacks, pvr_ccb.c: pvr_kccb_init()/pvr_fwccb_init(),
 * pvr_fw_trace.c: pvr_fw_trace_init()).
 *
 * The firmware code and data objects (FW heap offset 0 and the fixed address
 * of the private data section) are not created here: the caller loads them
 * as before, then calls pvr_fwif_create(), flushes every range listed in the
 * state, starts the MIPS and polls pvr_fwif_firmware_started().
 */
#ifndef PVR_FWIF_INIT_H
#define PVR_FWIF_INIT_H


#include <SupportDefs.h>

#include "pvr_rogue_fwif.h"


/* MIPS firmware heap (pvr_fw_mips.c, pvr_fw.c: pvr_fw_heap_info_init()) */
#define PVR_FWIF_MIPS_HEAP_BASE			0xC0000000u
#define PVR_FWIF_HEAP_SIZE				0x01000000u	/* 16 MB */
#define PVR_FWIF_HEAP_OFFSET_MASK		(PVR_FWIF_HEAP_SIZE - 1)
#define PVR_FWIF_CONFIG_HEAP_GRANULARITY	0x10000u
#define PVR_FWIF_CONFIG_HEAP_SIZE		(3 * PVR_FWIF_CONFIG_HEAP_GRANULARITY)
#define PVR_FWIF_RESERVED_SIZE			0x00100000u	/* 1 MB */

/*
 * Objects without a fixed offset are allocated by Linux in the main heap,
 * [0, PVR_FWIF_MAIN_HEAP_END), outside the code and data objects.
 */
#define PVR_FWIF_MAIN_HEAP_END \
	(PVR_FWIF_HEAP_SIZE - PVR_FWIF_CONFIG_HEAP_SIZE - PVR_FWIF_RESERVED_SIZE)

/* config area at the top of the heap (pvr_fw.c) */
#define PVR_FWIF_CONFIG_OFFSET \
	(PVR_FWIF_HEAP_SIZE - PVR_FWIF_CONFIG_HEAP_SIZE)
#define PVR_FWIF_CONNECTION_CTL_OFFSET	(PVR_FWIF_CONFIG_OFFSET)
#define PVR_FWIF_OSINIT_OFFSET \
	(PVR_FWIF_CONNECTION_CTL_OFFSET + PVR_FWIF_CONFIG_HEAP_GRANULARITY)
#define PVR_FWIF_SYSINIT_OFFSET \
	(PVR_FWIF_OSINIT_OFFSET + PVR_FWIF_CONFIG_HEAP_GRANULARITY)

/* heapOffset value of an allocation that may go anywhere in the main heap */
#define PVR_FWIF_ANY_OFFSET				0xFFFFFFFFu

/* every object is page granular, as Linux' GEM objects */
#define PVR_FWIF_PAGE_SIZE				4096u

#define PVR_FWIF_MAX_RANGES				24
#define PVR_FWIF_TRACE_BUFFERS			ROGUE_FW_THREAD_MAX


/*
 * Allocates size bytes (a multiple of PVR_FWIF_PAGE_SIZE) of physically
 * contiguous memory, aligned to align, and maps it uncached into the MIPS
 * page table at heapOffset, or anywhere in [0, PVR_FWIF_MAIN_HEAP_END) when
 * heapOffset is PVR_FWIF_ANY_OFFSET. Returns the kernel address, the
 * physical address and the firmware address (PVR_FWIF_MIPS_HEAP_BASE | heap
 * offset). The memory is filled by the CPU; the caller flushes it before
 * starting the firmware.
 */
typedef status_t (*pvr_fwif_alloc_hook)(void* cookie, size_t size,
	size_t align, uint32 heapOffset, void** _cpu, uint64* _phys,
	uint32* _fwAddr);

typedef void (*pvr_fwif_log_hook)(void* cookie, const char* format, ...)
	__attribute__((format(printf, 2, 3)));


struct pvr_fwif_env {
	void*				cookie;
	pvr_fwif_alloc_hook	alloc;
	pvr_fwif_log_hook	log;
		// may be NULL

	// clk_get_rate(core_clk): sysinit and runtime_cfg
	uint32				core_clock_hz;
	// device feature values (from the firmware's device info)
	uint32				slc_size_in_kilobytes;
		// < 4 sets ROGUE_FWIF_INICFG_DISABLE_DM_OVERLAP in sysdata
	uint32				num_clusters;
		// runtime_cfg->default_dusts_num_init
	// FW trace groups (Linux' init_fw_trace_mask module parameter, default
	// 0 = no trace), a subset of ROGUE_FWIF_LOG_TYPE_GROUP_MASK
	uint32				trace_group_mask;
};


struct pvr_fwif_range {
	const char*			name;
	void*				cpu;
	uint64				phys;
	uint32				fw_addr;
	size_t				size;
};


struct pvr_fwif_ccb {
	struct rogue_fwif_ccb_ctl*	ctrl;
	uint64				ctrl_phys;
	uint32				ctrl_fw_addr;
	void*				ring;
	uint64				ring_phys;
	uint32				ring_fw_addr;
	uint32				num_cmds;
	uint32				cmd_size;
};


struct pvr_fwif_state {
	struct rogue_fwif_sysinit*	sysinit;
	uint64				sysinit_phys;
	uint32				sysinit_fw_addr;

	struct rogue_fwif_osinit*	osinit;
	uint64				osinit_phys;
	uint32				osinit_fw_addr;
	struct rogue_fwif_connection_ctl* connection_ctl;
	uint64				connection_ctl_phys;
	uint32				connection_ctl_fw_addr;

	struct pvr_fwif_ccb	kccb;
	uint32*				kccb_rtn;
	uint64				kccb_rtn_phys;
	uint32				kccb_rtn_fw_addr;
	struct pvr_fwif_ccb	fwccb;

	uint32*				power_sync;
	uint32				power_sync_fw_addr;
	uint32*				mmucache_sync;
	uint32				mmucache_sync_fw_addr;

	struct rogue_fwif_sysdata*	sysdata;
	struct rogue_fwif_osdata*	osdata;
	struct rogue_fwif_hwrinfobuf* hwrinfobuf;
	struct rogue_fwif_gpu_util_fwcb* gpu_util_fwcb;
	struct rogue_fwif_runtime_cfg* runtime_cfg;
	uint64				fault_page_phys;

	struct rogue_fwif_tracebuf*	tracebuf_ctrl;
	uint32*				trace_buffers[PVR_FWIF_TRACE_BUFFERS];

	// everything allocated, in allocation order (also on failure)
	struct pvr_fwif_range	ranges[PVR_FWIF_MAX_RANGES];
	uint32				range_count;
};


status_t pvr_fwif_create(const pvr_fwif_env& env, pvr_fwif_state* state);

/*!	Whether the firmware has set sysinit->firmware_started. The caller
	invalidates the CPU caches of the sysinit range before calling.
*/
bool pvr_fwif_firmware_started(const pvr_fwif_state* state);


#endif	// PVR_FWIF_INIT_H
