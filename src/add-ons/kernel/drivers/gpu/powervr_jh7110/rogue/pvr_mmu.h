/*
 * Copyright 2026, The RenkuOS authors.
 * Copyright (c) 2023 Imagination Technologies Ltd.
 * SPDX-License-Identifier: GPL-2.0-only OR MIT
 *
 * GPU MMU context of the PowerVR Rogue GPU (page catalogue -> page directory
 * -> page table, 4 KB device pages), ported from the Linux drm/imagination
 * driver (pvr_mmu.c, pvr_mmu.h, pvr_vm.c: heap table).
 *
 * Every table is one physically contiguous 4 KB page obtained through the
 * gpu_alloc hook. The CPU fills the tables through cached mappings; after
 * each pvr_mmu_map()/pvr_mmu_unmap() the modified table pages are passed to
 * the sync hook (Linux: pvr_mmu_backing_page_sync() -> dma_sync), and the
 * MMU cache flush flags the change requires are accumulated in the context
 * (Linux: pvr_mmu_set_flush_flags()); they are consumed by the MMUCACHE
 * KCCB command (pvr_context.h: pvr_mmu_flush_exec()).
 */
#ifndef PVR_MMU_H
#define PVR_MMU_H


#include <SupportDefs.h>

#include "pvr_fwif_init.h"
#include "pvr_rogue_heap_config.h"


/* pvr_mmu.h */
#define PVR_DEVICE_PAGE_SIZE			4096u
#define PVR_DEVICE_PAGE_SHIFT			12
#define PVR_PAGE_TABLE_ADDR_SPACE_SIZE	SZ_1T
#define PVR_PAGE_TABLE_ADDR_BITS		40
#define PVR_PAGE_TABLE_ADDR_MASK		(PVR_PAGE_TABLE_ADDR_SPACE_SIZE - 1)

/* pvr_mmu.c: flush flags of a change at each level */
#define PVR_MMU_SYNC_LEVEL_0_FLAGS \
	(ROGUE_FWIF_MMUCACHEDATA_FLAGS_PT \
		| ROGUE_FWIF_MMUCACHEDATA_FLAGS_INTERRUPT \
		| ROGUE_FWIF_MMUCACHEDATA_FLAGS_TLB)
#define PVR_MMU_SYNC_LEVEL_1_FLAGS \
	(PVR_MMU_SYNC_LEVEL_0_FLAGS | ROGUE_FWIF_MMUCACHEDATA_FLAGS_PD)
#define PVR_MMU_SYNC_LEVEL_2_FLAGS \
	(PVR_MMU_SYNC_LEVEL_1_FLAGS | ROGUE_FWIF_MMUCACHEDATA_FLAGS_PC)

/*
 * pvr_mmu_map() flags. Linux (pvr_mmu_map(), pvr_mmu.c:2579) derives the PTE
 * flags from the buffer object flags: DRM_PVR_BO_BYPASS_DEVICE_CACHE sets
 * SLC_BYPASS_CTRL, DRM_PVR_BO_PM_FW_PROTECT sets PM_META_PROTECT; READ_ONLY
 * and CC (cache coherent) are always clear.
 */
#define PVR_MMU_MAP_BYPASS_DEVICE_CACHE	0x01	/* PTE SLC_BYPASS_CTRL */
#define PVR_MMU_MAP_PM_FW_PROTECT		0x02	/* PTE PM_META_PROTECT */
#define PVR_MMU_MAP_READ_ONLY			0x04
	/* extension, never set by Linux: PTE READ_ONLY */
#define PVR_MMU_MAP_ANY_ADDRESS			0x80
	/* extension: skip the heap check of pvr_find_heap_containing() */


/*
 * Allocates size bytes of physically contiguous, zeroed memory aligned to
 * align (both PVR_DEVICE_PAGE_SIZE here), below 2^phys_bus_width.
 */
typedef status_t (*pvr_gpu_alloc_hook)(void* cookie, size_t size,
	size_t align, void** _cpu, uint64* _phys);
/* frees what pvr_gpu_alloc_hook returned */
typedef void (*pvr_gpu_free_hook)(void* cookie, void* cpu, uint64 phys,
	size_t size);
/*
 * Writes back and invalidates the CPU caches of a physical range (on the
 * JH7110: the L2 Flush64 loop of the driver).
 */
typedef void (*pvr_cache_sync_hook)(void* cookie, uint64 phys, size_t size);


struct pvr_mmu_env {
	void*				cookie;
	pvr_gpu_alloc_hook	gpu_alloc;
	pvr_gpu_free_hook	gpu_free;
		// may be NULL: the tables are then left to the caller
	pvr_cache_sync_hook	sync;
		// may be NULL: the caller then syncs every range of
		// pvr_mmu_get_table() before using the context
	pvr_fwif_log_hook	log;
		// may be NULL
	uint32				phys_bus_width;
		// device feature phys_bus_width (36); 0 means 40
};


struct pvr_heap {
	const char*	name;
	uint64		base;
	uint64		size;
};

/* pvr_vm.c: pvr_heaps[] (without the region header heap: no BRN 63142) */
extern const pvr_heap kPvrHeaps[];
extern const uint32 kPvrHeapCount;

const pvr_heap* pvr_find_heap_containing(uint64 start, uint64 size);


struct pvr_mmu_context;


status_t pvr_mmu_context_create(const pvr_mmu_env& env,
	pvr_mmu_context** _context);
/*!	Frees the mirror and, with a gpu_free hook, the table pages. The GPU must
	no longer use the context (after pvr_context_cleanup() of every context
	using it and a completed MMU flush).
*/
void pvr_mmu_context_destroy(pvr_mmu_context* context);

/*!	Physical address of the page catalogue (Linux:
	pvr_mmu_get_root_table_dma_addr()), the value of
	rogue_fwif_fwmemcontext::pc_dev_paddr.
*/
uint64 pvr_mmu_get_root_table_phys(const pvr_mmu_context* context);

/*!	Maps size bytes (a multiple of 4 KB) of physically contiguous memory at
	phys to deviceAddr. Fails with B_BUSY if a page is already mapped, as
	pvr_page_create() (-EEXIST); nothing stays mapped on failure.
*/
status_t pvr_mmu_map(pvr_mmu_context* context, uint64 deviceAddr,
	uint64 phys, uint64 size, uint32 flags);
/*!	Clears the PTEs of [deviceAddr, deviceAddr + size). Unmapped pages are
	skipped. Tables are kept (they are freed by pvr_mmu_context_destroy()).
	Linux flushes and waits after an unmap (pvr_mmu_op_context_destroy()):
	the caller sends pvr_mmu_flush_exec(..., wait) afterwards.
*/
status_t pvr_mmu_unmap(pvr_mmu_context* context, uint64 deviceAddr,
	uint64 size);

/*!	Walks the CPU copy of the tables like the GPU would; returns false if
	the address is not mapped. For self tests.
*/
bool pvr_mmu_translate(const pvr_mmu_context* context, uint64 deviceAddr,
	uint64* _phys, uint64* _pte);

/*!	Flush flags accumulated since the last call (Linux:
	atomic_xchg(&mmu_flush_cache_flags, 0)).
*/
uint32 pvr_mmu_take_flush_flags(pvr_mmu_context* context);
/*!	pvr_mmu_flush_request_all(): every level, e.g. after mapping new
	firmware objects into the MIPS page table (pvr_vm_mips.c:199).
*/
void pvr_mmu_flush_request_all(pvr_mmu_context* context);

/*!	Table pages, for a caller without a sync hook or for dumps:
	index 0 is the page catalogue.
*/
uint32 pvr_mmu_table_count(const pvr_mmu_context* context);
bool pvr_mmu_get_table(const pvr_mmu_context* context, uint32 index,
	void** _cpu, uint64* _phys, uint32* _level);


#endif	// PVR_MMU_H
