/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * Userspace ABI of the intel_extreme Gen9 (Skylake) render node
 * (graphics/intel_render_9x_*). Shared between the kernel driver
 * (src/add-ons/kernel/drivers/graphics/intel_extreme/render_node.cpp)
 * and userspace clients.
 */
#ifndef INTEL_RENDER_ABI_H
#define INTEL_RENDER_ABI_H


#include <Drivers.h>
#include <SupportDefs.h>


/* The existing intel_extreme ioctls end at B_DEVICE_OP_CODES_END + 6, keep
 * well clear of them. */
#define INTEL_RENDER_IOCTL_BASE		(B_DEVICE_OP_CODES_END + 100)

enum {
	INTEL_RENDER_GET_INFO = INTEL_RENDER_IOCTL_BASE,
	INTEL_RENDER_GEM_CREATE,
	INTEL_RENDER_GEM_MAP,
	INTEL_RENDER_GEM_BIND,
	INTEL_RENDER_GEM_UNBIND,
	INTEL_RENDER_GEM_CLOSE,
	INTEL_RENDER_CONTEXT_CREATE,
	INTEL_RENDER_EXEC,
	INTEL_RENDER_WAIT
};


/* INTEL_RENDER_GET_INFO */
struct intel_render_info {
	uint32	chipset;			/* PCI device id */
	uint32	gen;				/* hardware generation (9) */
	uint64	aperture_base;		/* physical base of the GMADR aperture (BAR2) */
	uint64	aperture_size;		/* size of the CPU-mappable aperture */
	uint64	ggtt_entries;		/* number of 64-bit GGTT entries */
	uint32	eu_total;			/* execution units (0: not probed in v1) */
	uint32	cs_timestamp_freq;	/* CS timestamp frequency (0: not probed) */
	uint64	last_completed;		/* last completed exec seqno */
	/* v2: hardware debug window (dprintf does not reach syslog on all
	 * machines, this is the only way to observe the GPU state) */
	uint32	debug[32];
};

/* indices into intel_render_info.debug */
enum {
	INTEL_RENDER_DEBUG_GGC = 0,			/* raw PCI config 0x50 */
	INTEL_RENDER_DEBUG_GGTT_SIZE,		/* GGTT size in bytes (low 32) */
	INTEL_RENDER_DEBUG_APERTURE_SIZE,	/* aperture size (low 32) */
	INTEL_RENDER_DEBUG_WATERMARK,		/* render region start (low 32) */
	INTEL_RENDER_DEBUG_FORCEWAKE_GT_ACK,
	INTEL_RENDER_DEBUG_FORCEWAKE_RENDER_ACK,
	INTEL_RENDER_DEBUG_GAPS_TSV,		/* 0xB004 readback, bit 7 set */
	INTEL_RENDER_DEBUG_GFX_MODE,		/* 0x229C readback, bit 15 set */
	INTEL_RENDER_DEBUG_EXECLIST_STATUS,	/* 0x2234 low 32 */
	INTEL_RENDER_DEBUG_HWS_PGA,			/* 0x2080 readback */
	INTEL_RENDER_DEBUG_GT0_IER,			/* 0x4430C readback */
	INTEL_RENDER_DEBUG_MASTER_INT_CTL,	/* 0x44200 readback, bit 31 set */
	INTEL_RENDER_DEBUG_LAST_COMPLETED,
	INTEL_RENDER_DEBUG_LAST_SUBMITTED,
	INTEL_RENDER_DEBUG_SCRATCH_PHYS,
	INTEL_RENDER_DEBUG_REGION_END,		/* render region end (low 32) */
	INTEL_RENDER_DEBUG_RING_HEAD,		/* 0x2034 live: GPU ring head */
	INTEL_RENDER_DEBUG_RING_TAIL,		/* 0x2030 live: GPU ring tail */
	INTEL_RENDER_DEBUG_GT0_IIR,			/* 0x44308 live: pending GT irqs */
	INTEL_RENDER_DEBUG_HWS_SEQNO_LO,	/* HWS page +0xC0, kernel mapping */
	INTEL_RENDER_DEBUG_HWS_SEQNO_HI,	/* HWS page +0xC4 */
	INTEL_RENDER_DEBUG_IRQ_COUNT,		/* render_node_interrupt invocations */
	INTEL_RENDER_DEBUG_CTX_TAIL,		/* context image dword 7 (RING_TAIL) */
	INTEL_RENDER_DEBUG_LAST_DESCRIPTOR,	/* low dword of last submitted desc */
	INTEL_RENDER_DEBUG_FAULT_RING,		/* 0x2494: per-engine fault register */
	INTEL_RENDER_DEBUG_FAULT_GAM,		/* 0x4000: GAM fault register */
	INTEL_RENDER_DEBUG_FAULT_VA_LO,		/* 0x4B10: fault VA bits 43:12 */
	INTEL_RENDER_DEBUG_FAULT_VA_HI,		/* 0x4B14: fault VA bits 47:44 */
	INTEL_RENDER_DEBUG_EXECLIST_STATUS_HI, /* 0x2238: current context id */
	INTEL_RENDER_DEBUG_MI_MODE,			/* 0x209C readback: bit8 must be 0 */
	INTEL_RENDER_DEBUG_GT0_IMR,			/* 0x44304 readback */
	INTEL_RENDER_DEBUG_HWSTAM,			/* 0x2098 readback */
};


/* INTEL_RENDER_GEM_CREATE */
struct intel_render_gem_create {
	uint64	size;				/* in: requested size, out: allocated
								   (page-granular) size */
	uint32	flags;				/* in: reserved, pass 0 */
	uint32	handle;				/* out: GEM handle */
};


/* INTEL_RENDER_GEM_MAP: the client maps the object with clone_area() on the
 * returned area id. */
struct intel_render_gem_map {
	uint32	handle;				/* in */
	int32	area_id;			/* out */
};


/* INTEL_RENDER_GEM_BIND: softpin the object at a GGTT byte offset. The
 * driver rejects offsets below its reserved watermark, offsets inside the
 * kernel zone at the bottom of the render region (HW status page, context
 * images, rings), offsets that are not page aligned, and ranges outside
 * the render region. */
struct intel_render_gem_bind {
	uint32	handle;				/* in */
	uint32	pad;
	uint64	ggtt_offset;		/* in: GGTT byte offset, page aligned */
};


/* INTEL_RENDER_GEM_UNBIND / INTEL_RENDER_GEM_CLOSE */
struct intel_render_gem_handle {
	uint32	handle;				/* in */
	uint32	pad;
};


/* INTEL_RENDER_CONTEXT_CREATE: creates the (single) context of this open
 * instance; also created lazily on the first EXEC. */
struct intel_render_context_create {
	uint32	context_id;			/* out */
	uint32	pad;
};


/* INTEL_RENDER_EXEC: batch_ggtt_addr must point inside an object of this
 * open instance that is currently bound via INTEL_RENDER_GEM_BIND, must be
 * 8 byte aligned, and the batch must be terminated by MI_BATCH_BUFFER_END
 * (0x05000000). The batch executes with legacy 32-bit GGTT addressing. */
struct intel_render_exec {
	uint64	batch_ggtt_addr;	/* in: GGTT address of the batch buffer */
	uint32	batch_length;		/* in: batch length in bytes */
	uint32	flags;				/* in: reserved, pass 0 */
	uint32	seqno;				/* out: submission sequence number */
	uint32	pad;
};


/* INTEL_RENDER_WAIT: returns B_OK when last_completed >= seqno,
 * B_TIMED_OUT otherwise. */
struct intel_render_wait {
	uint32	seqno;				/* in */
	uint32	pad;
	uint64	timeout_us;			/* in */
};


#endif	/* INTEL_RENDER_ABI_H */
