/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License for the native code.
 * Genode-modeled portions retain the license in COPYING.Genode.
 * Copyright (C) 2017-2024 Genode Labs GmbH (referenced portions).
 *
 * SPDX-License-Identifier: LicenseRef-Genode-AGPL-3.0-with-linking-exception AND MIT
 *
 * Gen9 GPU render node with Haiku-area GEM objects, a private GGTT
 * region, per-context 48-bit PPGTT, execlist submission and sync objects.
 * RCS submissions complete synchronously; engine-map extensions are rejected
 * so iris creates separate render and compute contexts.
 *
 * Hardware layout/initialization references include Intel PRMs and Genode's
 * Intel driver. See README.render-node.md and COPYING.Genode for provenance,
 * experimental limits, and the required license review before integration.
 */


#include "driver.h"
#include "render_node.h"

#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <OS.h>
#include <team.h>
#include <vm/vm.h>

#include "gen9_gt_regs.h"
#include "render_abi.h"
#include "drm_uapi.h"


/* Linux errno values, so Mesa's libdrm/iris error handling sees the
 * negatives it expects. The ioctl return value is passed straight back to
 * userspace on Haiku.
 *
 * NOTE: do not use the bare EINVAL/ENOMEM/... names. Haiku's <Errors.h>
 * (pulled in by <OS.h>) already defines them as *negative* BeOS status
 * codes, so negating one would turn an error into a positive return (and
 * negating B_NO_MEMORY, which is INT_MIN, is undefined). These explicit
 * negative values are passed through unchanged. */
#define DRM_EINVAL		(-22)
#define DRM_ENOSYS		(-38)
#define DRM_ENODEV		(-19)
#define DRM_ENOENT		(-2)
#define DRM_ENOMEM		(-12)
#define DRM_ETIMEDOUT	(-110)
#define DRM_EAGAIN		(-11)


// #define TRACE_RENDER  // Enable only for bring-up; per-ioctl serial logging is costly.
#ifdef TRACE_RENDER
#	define TRACE(x...) dprintf("intel_render_9x: " x)
#else
#	define TRACE(x...) do { if (false) dprintf("intel_render_9x: " x); } while (0)
#endif

#define ERROR(x...) dprintf("intel_render_9x: " x)
#define CALLED(x...) TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


#define RENDER_OPEN_COOKIE_MAGIC	'irnc'

#define MAX_HANDLES					256
#define MAX_BO_SIZE					(64 * 1024 * 1024)
#define MAX_CONTEXTS				64

// GGTT offsets below this share of the aperture belong to the
// display/GART side (see DESIGN.md); never touched by the render node.
#define APERTURE_DISPLAY_SHARE		4

// Bottom of the render region is reserved for kernel objects (HW status
// page, context images, rings). User binds start above it.
#define KERNEL_ZONE_SIZE			(4 * 1024 * 1024)

// Logical ring context geometry (Genode context.h Rcs_context)
#define CONTEXT_PAGES				23	// 22 context + 1 GuC (skipped)
#define RING_PAGES					4
#define RING_SIZE					(RING_PAGES * B_PAGE_SIZE)

// register image starts at LRCA + 0x1000; LRCA = context BO page 1
#define CONTEXT_REG_IMAGE_PAGE		2

// global HW status page layout (Genode context.h Hardware_status_page)
#define HWS_SEQNO_OFFSET			0xC0	// qword, written by PIPE_CONTROL
#define HWS_SEMAPHORE_OFFSET		0xC8	// dword, 0 = run, 1 = parked

// dwords emitted per submission (Genode _setup_ring_vram, Gen9 path)
#define RING_SEQUENCE_DWORDS		40


// The render GT registers live in the REGS_FLAT block, so bypass the
// register_blocks indirection of read32()/write32().
static inline uint32
render_read32(intel_info& info, uint32 reg)
{
	return *(volatile uint32*)(info.registers + reg);
}


static inline void
render_write32(intel_info& info, uint32 reg, uint32 value)
{
	*(volatile uint32*)(info.registers + reg) = value;
}


/*!	Debug: log the GPU page-fault state. The TLB fault data registers hold the
	faulting address; a non-zero value that appears only after iris work means
	iris's render hit an address our PPGTT does not map. */
static void
render_log_faults(intel_info& info, const char* tag)
{
#ifdef TRACE_RENDER
	uint32 ringFault = render_read32(info, GEN9_RING_FAULT_REG);
	uint32 gamFault = render_read32(info, GEN9_GAM_FAULT_REG);
	uint32 data0 = render_read32(info, GEN9_FAULT_TLB_RB_DATA0);
	uint32 data1 = render_read32(info, GEN9_FAULT_TLB_RB_DATA1);
	uint64 va = ((uint64)data1 << 32) | data0;
	TRACE("FAULT[%s] ring=0x%08" B_PRIx32 " gam=0x%08" B_PRIx32
		" data0=0x%08" B_PRIx32 " data1=0x%08" B_PRIx32
		" va=0x%llx va12=0x%llx\n", tag, ringFault, gamFault, data0, data1,
		(unsigned long long)va, (unsigned long long)(va << 12));
#endif
}


//	The PPGTT tables are WB-cached kernel memory; flush each updated entry so
//	the GPU (which reads them through physical addresses) sees the change.
static inline void
render_clflush(const void* address)
{
	asm volatile("clflush (%0)" :: "r"(address) : "memory");
}


//	Flush every cache line of a freshly filled page table, then order the
//	flushes before the GPU fetch.
static inline void
render_clflush_page(const void* address)
{
	for (uint32 offset = 0; offset < B_PAGE_SIZE; offset += 64)
		render_clflush((const uint8*)address + offset);
	asm volatile("mfence" ::: "memory");
}


struct render_gem_object {
	uint32			handle;
	area_id			area;
	void*			address;	// kernel mapping of the area
	size_t			size;		// in bytes, page aligned
	uint32			page_count;
	phys_addr_t*	pages;		// per-page scatter/gather list
	int32			refcount;
	bool			bound;
	bool			kernelBound;	// bound from the kernel GGTT zone
	uint64			ggtt_offset;
	uint64			mapped_va;	// PPGTT VA this object is mapped at (0 = none)
	void*			owner;		// open cookie that created the object,
									// NULL for kernel-internal objects
};


struct render_open_cookie;
struct render_ppgtt;


struct render_context {
	uint32			id;
	render_gem_object* contextBo;	// 23 pages
	render_gem_object* ringBo;		// 4 pages
	uint32*			image;			// kernel VA of the register image
	uint32*			ring;			// kernel VA of the ring buffer
	uint32			lrca;			// GGTT offset of context page 1
	uint32			ringGgtt;		// GGTT offset of the ring
	uint32			ringTail;		// bytes appended so far
	uint32			lastSubmitted;
	uint32			lastDescriptorLo;	// low dword of last submitted descriptor
	uint32			semSlot;		// index into the node's sem pool
	render_ppgtt*	ppgtt;			// per-context 48 bit page tables
	render_open_cookie* owner;		// client that created this context
	bool			dead;
};


//	Seqno-backed sync objects. iris waits for GPU completion with
//	SYNCOBJ_WAIT and orders submissions with the EXECBUFFER2 fence array, so
//	a syncobj is simply a flag plus the seqno it completes on.
#define MAX_SYNCOBJS	256

struct render_syncobj {
	uint32			handle;
	uint32			seqno;		// seqno this syncobj signals (0 = none)
	bool			signaled;
	render_open_cookie* owner;
};


//	Per-context PPGTT: 4-level IA-32e page tables (PML4/PDP/PD/PT) made of
//	4 KiB pages. The GPU reaches the tables through their physical addresses,
//	so every table page's physical address is captured at allocation. A
//	"scratch" chain backs every unmapped entry so a stray GPU access lands on
//	a scratch page instead of faulting the engine (Genode ppgtt.h). Objects
//	softpin at their iris-zone virtual addresses; the leaf PTE is pa|P|RW.

#define PPGTT_POOL_PAGES	1024
#define PPGTT_PTE_PRESENT	(1ULL << 0)
#define PPGTT_PTE_RW		(1ULL << 1)
#define PPGTT_PTE_FLAGS		(PPGTT_PTE_PRESENT | PPGTT_PTE_RW)
// Gen9 physical addresses are 39 bits wide here (Genode uses a 36-bit Pa
// field, bits 47:12); mask bits 51:12 and ignore anything above
#define PPGTT_ADDRESS_MASK	((uint64)0x000FFFFFFFFFF000ULL)

struct render_ppgtt {
	area_id			pool;
	uint8*			address;		// kernel VA of the pool
	phys_addr_t		page_phys[PPGTT_POOL_PAGES];
	uint32			used;

	uint64			scratch_page;	// phys of the leaf scratch page
	uint64			scratch_pt;		// phys of the scratch PT
	uint64			scratch_pd;		// phys of the scratch PD
	uint64			scratch_pdp;	// phys of the scratch PDP
	uint64*			pml4;			// kernel VA of the PML4
	uint64			pml4_phys;

	status_t		Init();
	void			Destroy();
	uint64*			TableAt(uint64 phys);
	uint64*			EnsureTable(uint64* entry, uint64 scratchPhys,
						uint64 fillPhys);
	status_t		Map(uint64 va, const phys_addr_t* pages, uint32 count);
	status_t		MapRange(render_gem_object* object, uint64 va);
	status_t		Unmap(uint64 va, uint32 count);
};


struct RenderNode {
	intel_info*		info;
	mutex			lock;
	status_t		status;		// latched init result
	bool			initialized;
	bool			interruptsEnabled;
	int32			open_count;

	// GGTT
	area_id			ggtt_area;
	volatile uint64* ggtt;
	uint64			ggtt_size;		// in bytes
	uint64			ggtt_entries;
	uint64			aperture_base;	// physical base of BAR2
	uint64			aperture_size;
	uint64			watermark;		// first GGTT byte offset we may use
	uint64			region_end;		// exclusive end of our GGTT region

	// kernel GGTT zone (context/ring/HWS objects) is a bump allocator; freed
	// ranges are kept here and reused so repeated context creation does not
	// exhaust the zone
	enum { kKernelFreeMax = 256 };
	uint64			kernelFreeOffset[kKernelFreeMax];
	uint64			kernelFreeSize[kKernelFreeMax];
	int32			kernelFreeCount;
	uint64			kernel_zone_end;// user binds start here
	uint64			kernel_bump;	// next free offset in the kernel zone

	area_id			scratch_area;
	void*			scratch_address;
	phys_addr_t		scratch_phys;

	// global hardware status page
	render_gem_object* hwsObject;
	uint8*			hwsCpu;
	uint32			hwsGgtt;

	bool			forcewake_held;
	uint32			ggc;

	render_gem_object* objects[MAX_HANDLES];
	uint32			next_handle;

	render_syncobj*	syncobjs[MAX_SYNCOBJS];
	uint32			next_syncobj;

	render_context*	contexts[MAX_CONTEXTS];
	// one wait semaphore per context slot, created on first use and only
	// deleted at driver uninit (never deleted while the IRQ handler could
	// still see a context)
	sem_id			waitSems[MAX_CONTEXTS];

	uint32			last_submitted;
	volatile uint32	last_completed;
	volatile uint32	irqCount;		// render_node_interrupt invocations

	status_t Init();
	status_t Reset();
	void FlushGgtt();
	render_gem_object* FindObject(uint32 handle);
	status_t CreateSyncobjLocked(render_open_cookie* cookie, bool signaled,
		uint32* _handle);
	render_syncobj* FindSyncobj(uint32 handle);
	void DestroySyncobjLocked(render_syncobj* syncobj);
	bool SyncobjSignaled(render_syncobj* syncobj);
	status_t CreateGemLocked(size_t size, void* owner,
		render_gem_object** _object);
	void DestroyObjectLocked(render_gem_object* object);
	status_t BindAtLocked(render_gem_object* object, uint64 offset);
	status_t BindObject(render_gem_object* object, uint64 offset);
	status_t UnbindObject(render_gem_object* object);
	status_t KernelBindLocked(render_gem_object* object);
	render_context* EnsureContextLocked(render_open_cookie* cookie);
	render_context* CreateContextLocked(render_open_cookie* cookie);
	render_context* ContextByIdLocked(render_open_cookie* cookie, uint32 id);
	void DestroyContextLocked(render_open_cookie* cookie);
	void DestroyContextObjLocked(render_context* context);
	void DestroyContextByIdLocked(render_open_cookie* cookie, uint32 id);
	void SetupContextImage(render_context* context);
	status_t SubmitLocked(render_context* context, uint64 batchAddr,
		uint32 batchLength, bool ppgtt, uint32* _seqno);
	bool BatchValidLocked(render_open_cookie* cookie, uint64 addr,
		uint32 length);
};


struct render_open_cookie {
	uint32			magic;
	intel_info*		info;
	RenderNode*		node;
	render_context*	context;		// created lazily
};


static RenderNode* sRenderNodes[MAX_CARDS];


//	#pragma mark - hardware helpers


static bool
wait_for_forcewake(intel_info& info, uint32 ackRegister)
{
	// Genode waits 50 x 1 ms for the ack
	for (int32 i = 0; i < 50; i++) {
		if ((render_read32(info, ackRegister) & GEN9_FORCEWAKE_ACK_KERNEL)
				!= 0)
			return true;
		snooze(1000);
	}
	return false;
}


static void
enable_forcewake(intel_info& info)
{
	// Gen9 forcewake reset (Genode mmio.h _fw_reset_gen9)
	render_write32(info, GEN9_FORCEWAKE_MEDIA, GEN9_FORCEWAKE_RESET);
	render_write32(info, GEN9_FORCEWAKE_RENDER, GEN9_FORCEWAKE_RESET);
	render_write32(info, GEN9_FORCEWAKE_GT, GEN9_FORCEWAKE_RESET);
	render_read32(info, GEN9_MISC_CTRL0);
		// posting read, same cache line as the forcewake registers

	// enable GT and RENDER forcewake, hold it (v1 never releases it)
	uint32 request = GEN9_FORCEWAKE_KERNEL_MASK | GEN9_FORCEWAKE_KERNEL;

	render_write32(info, GEN9_FORCEWAKE_GT, request);
	render_read32(info, GEN9_MISC_CTRL0);
	if (!wait_for_forcewake(info, GEN9_FORCEWAKE_GT_ACK))
		ERROR("forcewake GT ack timeout (0x%08" B_PRIx32 ")\n",
			render_read32(info, GEN9_FORCEWAKE_GT_ACK));

	render_write32(info, GEN9_FORCEWAKE_RENDER, request);
	render_read32(info, GEN9_MISC_CTRL0);
	if (!wait_for_forcewake(info, GEN9_FORCEWAKE_RENDER_ACK))
		ERROR("forcewake RENDER ack timeout (0x%08" B_PRIx32 ")\n",
			render_read32(info, GEN9_FORCEWAKE_RENDER_ACK));
}


static void
apply_gaps_tsv(intel_info& info)
{
	// mandatory Gen9 erratum (WaEnableGapsTsvCreditFix): without this the
	// GPU hangs arbitrarily during batch execution; re-apply after resets
	render_write32(info, GEN9_GAPS_TSV,
		render_read32(info, GEN9_GAPS_TSV) | GEN9_GAPS_TSV_ENABLE);
}


//	#pragma mark - RenderNode


status_t
RenderNode::Reset()
{
	// Genode reset.h: stop rings, ready-for-reset, full GDRST, unready.
	// Structured as one function so the watchdog can call it later.

	// stop the RCS ring: MI_MODE stop_rings with its mask bit, then poll
	// rings_idle (Genode _stop_engine)
	bool idle = false;
	for (int32 i = 0; i < 10; i++) {
		if ((render_read32(*info, GEN9_RCS_MI_MODE)
				& GEN9_MI_MODE_RINGS_IDLE) != 0) {
			idle = true;
			break;
		}
		render_write32(*info, GEN9_RCS_MI_MODE, GEN9_MI_MODE_STOP_RINGS_MASK
			| GEN9_MI_MODE_STOP_RINGS);
		render_read32(*info, GEN9_RCS_MI_MODE);
		snooze(10000);
	}
	if (!idle)
		ERROR("card %" B_PRId32 ": RCS stop rings timeout\n", info->id);

	// catastrophic error: bypass ready-for-reset, poll it away
	uint32 resetCtrl = render_read32(*info, GEN9_RCS_RESET_CTRL);
	if ((resetCtrl & GEN9_RESET_CTRL_CATASTROPHIC) != 0) {
		for (int32 i = 0; i < 7; i++) {
			if ((render_read32(*info, GEN9_RCS_RESET_CTRL)
					& GEN9_RESET_CTRL_CATASTROPHIC) == 0)
				break;
			snooze(100000);
		}
	} else if ((resetCtrl & GEN9_RESET_CTRL_READY) == 0) {
		// request reset, poll ready_for_reset
		render_write32(*info, GEN9_RCS_RESET_CTRL, GEN9_RESET_CTRL_MASK_BIT
			| GEN9_RESET_CTRL_REQUEST);
		render_read32(*info, GEN9_RCS_RESET_CTRL);
		for (int32 i = 0; i < 7; i++) {
			if ((render_read32(*info, GEN9_RCS_RESET_CTRL)
					& GEN9_RESET_CTRL_READY) != 0)
				break;
			snooze(100000);
		}
	}

	// full GPU soft reset; the bit clears when the reset completes
	render_write32(*info, GEN9_GDRST, GEN9_GDRST_FULL);
	for (int32 i = 0; i < 2; i++) {
		if ((render_read32(*info, GEN9_GDRST) & GEN9_GDRST_FULL) == 0)
			break;
		snooze(200000);
	}
	if ((render_read32(*info, GEN9_GDRST) & GEN9_GDRST_FULL) != 0) {
		ERROR("card %" B_PRId32 ": GDRST did not complete\n", info->id);
		return B_ERROR;
	}

	// unready
	render_write32(*info, GEN9_RCS_RESET_CTRL, GEN9_RESET_CTRL_MASK_BIT);
	render_read32(*info, GEN9_RCS_RESET_CTRL);

	// some devices still show volatile state (Genode reset.h)
	snooze(50);

	apply_gaps_tsv(*info);

	TRACE("card %" B_PRId32 ": GDRST done, GAPS TSV (0xB004) = 0x%08"
		B_PRIx32 "\n", info->id, render_read32(*info, GEN9_GAPS_TSV));
	return B_OK;
}


status_t
RenderNode::Init()
{
	// called with lock held
	if (initialized)
		return status;

	ggtt_area = -1;
	scratch_area = -1;
	status = B_OK;

	if (info->device_type.Generation() != 9) {
		ERROR("card %" B_PRId32 ": not a Gen9 device\n", info->id);
		status = B_NOT_SUPPORTED;
		initialized = true;
		return status;
	}

	// probe the GGTT size: GGC (PCI config 0x50) bits 7:6,
	// size = (1 << GGMS) MB (Genode main.cc _ggtt_size)
	ggc = get_pci_config(info->pci, GEN9_PCI_GGC, 2);
	uint32 ggms = (ggc & GEN9_GGC_GGMS_MASK) >> 6;
	ggtt_size = (uint64)1 << (20 + ggms);
	if (ggms == 0)
		ERROR("card %" B_PRId32 ": GGC GGMS is 0, assuming 1 MB GGTT\n",
			info->id);

	// BAR0 (GTTMMADR): the GGTT lives in its upper half
	phys_addr_t bar0 = info->pci->u.h0.base_registers[0];
	uint64 bar0Size = info->pci->u.h0.base_register_sizes[0];
	if ((info->pci->u.h0.base_register_flags[0] & PCI_address_type)
			== PCI_address_type_64) {
		bar0 |= (uint64)info->pci->u.h0.base_registers[1] << 32;
		bar0Size |= (uint64)info->pci->u.h0.base_register_sizes[1] << 32;
	}

	// BAR2 (GMADR): the CPU-mappable aperture
	aperture_base = info->pci->u.h0.base_registers[2];
	aperture_size = info->pci->u.h0.base_register_sizes[2];
	if ((info->pci->u.h0.base_register_flags[2] & PCI_address_type)
			== PCI_address_type_64) {
		aperture_base |= (uint64)info->pci->u.h0.base_registers[3] << 32;
		aperture_size |= (uint64)info->pci->u.h0.base_register_sizes[3] << 32;
	}

	if (ggtt_size > bar0Size / 2) {
		ERROR("card %" B_PRId32 ": GGTT size (%" B_PRIu64 ") exceeds half "
			"the MMIO BAR, clamping\n", info->id, ggtt_size);
		ggtt_size = bar0Size / 2;
	}
	ggtt_entries = ggtt_size / 8;

	TRACE("card %" B_PRId32 ": GGC 0x%04" B_PRIx32 ", GGTT %" B_PRIu64
		" KB (%" B_PRIu64 " entries), BAR0 %" B_PRIu64 " MB at 0x%"
		B_PRIxPHYSADDR "\n", info->id, ggc, ggtt_size >> 10, ggtt_entries,
		bar0Size >> 20, bar0);
	TRACE("card %" B_PRId32 ": aperture %" B_PRIu64 " MB at phys 0x%"
		B_PRIx64 "\n", info->id, aperture_size >> 20, aperture_base);

	// quiet the GT0 interrupt block (MASTER_INT_CTL stays owned by the
	// display side)
	render_write32(*info, GEN9_GT0_IMR, ~0u);
	render_write32(*info, GEN9_GT0_IER, 0);
	render_write32(*info, GEN9_GT0_IIR, ~0u);
	render_write32(*info, GEN9_GT0_IIR, ~0u);

	// enable forcewake before touching GT registers (GFX_FLSH_CNTL)
	enable_forcewake(*info);
	forcewake_held = true;

	apply_gaps_tsv(*info);

	// map the GGTT (our own mapping of BAR0 + barSize / 2)
	ggtt_area = map_physical_memory("intel render ggtt",
		bar0 + bar0Size / 2, ggtt_size,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&ggtt);
	if (ggtt_area < B_OK) {
		ERROR("card %" B_PRId32 ": could not map GGTT: %s\n", info->id,
			strerror(ggtt_area));
		status = ggtt_area;
		initialized = true;
		return status;
	}

	// allocate a scratch page for unused PTEs
	scratch_area = create_area("intel render scratch", &scratch_address,
		B_ANY_KERNEL_ADDRESS, B_PAGE_SIZE, B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (scratch_area < B_OK) {
		ERROR("card %" B_PRId32 ": could not create scratch page: %s\n",
			info->id, strerror(scratch_area));
		status = scratch_area;
		initialized = true;
		return status;
	}
	memset(scratch_address, 0, B_PAGE_SIZE);

	physical_entry entry;
	if (get_memory_map(scratch_address, B_PAGE_SIZE, &entry, 1) != B_OK) {
		ERROR("card %" B_PRId32 ": get_memory_map on scratch failed\n",
			info->id);
		status = B_ERROR;
		initialized = true;
		return status;
	}
	scratch_phys = entry.address;

	// render region: top 3/4 of the aperture, never below the stolen
	// memory size (BIOS maps stolen memory at the bottom of the GGTT)
	watermark = aperture_size / APERTURE_DISPLAY_SHARE;
	watermark = (watermark + B_PAGE_SIZE - 1) & ~(uint64)(B_PAGE_SIZE - 1);

	uint32 gms = (ggc & GEN9_GGC_GMS_MASK) >> 8;
	uint64 stolenSize = 0;
	if (gms >= 240)
		stolenSize = (uint64)(gms - 236) * 4 * 1024 * 1024;
	else if (gms >= 1 && gms <= 16)
		stolenSize = (uint64)gms * 32 * 1024 * 1024;
	else if (gms == 32)
		stolenSize = (uint64)1024 * 1024 * 1024;
	else if (gms == 48)
		stolenSize = (uint64)1536 * 1024 * 1024;

	TRACE("card %" B_PRId32 ": stolen memory ~%" B_PRIu64 " MB (GMS 0x%"
		B_PRIx32 ")\n", info->id, stolenSize >> 20, gms);

	if (stolenSize > watermark) {
		watermark = (stolenSize + B_PAGE_SIZE - 1)
			& ~(uint64)(B_PAGE_SIZE - 1);
		ERROR("card %" B_PRId32 ": stolen memory exceeds the 1/4-aperture "
			"watermark, raised to 0x%" B_PRIx64 "\n", info->id, watermark);
	}

	// the last GGTT entry is left alone (hardware reserved)
	region_end = aperture_size;
	if (region_end > (ggtt_entries - 1) * (uint64)B_PAGE_SIZE)
		region_end = (ggtt_entries - 1) * (uint64)B_PAGE_SIZE;

	// the bottom of the render region is the kernel zone (HW status page,
	// context images, rings); user binds live above it
	kernel_zone_end = watermark + KERNEL_ZONE_SIZE;
	if (kernel_zone_end > region_end)
		kernel_zone_end = region_end;
	kernel_bump = watermark;
	kernelFreeCount = 0;

	if (watermark >= region_end || kernel_zone_end >= region_end) {
		ERROR("card %" B_PRId32 ": no GGTT space left for the render node "
			"(watermark 0x%" B_PRIx64 ", kernel zone end 0x%" B_PRIx64
			", end 0x%" B_PRIx64 ")\n", info->id, watermark, kernel_zone_end,
			region_end);
		status = B_ERROR;
		initialized = true;
		return status;
	}

	// fill the render region with scratch PTEs
	uint64 firstEntry = watermark / B_PAGE_SIZE;
	uint64 endEntry = region_end / B_PAGE_SIZE;
	uint64 scratchPte = scratch_phys | GEN9_GGTT_PTE_PRESENT;
	for (uint64 i = firstEntry; i < endEntry; i++)
		ggtt[i] = scratchPte;
	FlushGgtt();

	TRACE("card %" B_PRId32 ": render GGTT region entries [%"
		B_PRIu64 ", %" B_PRIu64 "), watermark 0x%" B_PRIx64
		", scratch page phys 0x%" B_PRIxPHYSADDR "\n", info->id, firstEntry,
		endEntry, watermark, scratch_phys);

	//	---------------------------------------------------------------
	//	GT init/reset (Genode mmio.h reset() + init() order)
	//	---------------------------------------------------------------

	status = Reset();
	if (status != B_OK) {
		initialized = true;
		return status;
	}

	// zero the 32 fence registers (64 bit each, at 0x100000 + i * 8)
	for (int32 i = 0; i < GEN9_FENCE_REG_COUNT; i++) {
		render_write32(*info, GEN9_FENCE_REG_BASE + i * 8, 0);
		render_write32(*info, GEN9_FENCE_REG_BASE + i * 8 + 4, 0);
		render_read32(*info, GEN9_FENCE_REG_BASE + i * 8);
	}

	// disable the north display engine handshake (bit 4)
	render_write32(*info, GEN9_NDE_RSTWRN_OPT,
		render_read32(*info, GEN9_NDE_RSTWRN_OPT)
			& ~GEN9_RST_PCH_HANDSHAKE_EN);

	// page attribute table (values from Genode _set_page_attributes)
	render_write32(*info, GEN9_PAT_INDEX_L, GEN9_PAT_INDEX_L_VALUE);
	render_write32(*info, GEN9_PAT_INDEX_H, GEN9_PAT_INDEX_H_VALUE);

	// disable RPS/turbo (Genode _disable_rps)
	render_write32(*info, GEN9_RP_FREQ_NORMAL, GEN9_RP_FREQ_TURBO_DISABLE);
	render_write32(*info, GEN9_RP_CTRL, 0);

	// enable execlists (Genode enable_execlist): mask everything in
	// HWSTAM for now, clear stop_rings (masked write: mask bit set, value
	// bit clear — writing 0 is a no-op on this register), execlist enable
	// with mask
	render_write32(*info, GEN9_RCS_HWSTAM, ~0u);
	render_read32(*info, GEN9_RCS_HWSTAM);
	render_write32(*info, GEN9_RCS_MI_MODE, GEN9_MI_MODE_STOP_RINGS_MASK);
	render_write32(*info, GEN9_GFX_MODE, GEN9_GFX_MODE_EXECLIST_MASK
		| GEN9_GFX_MODE_EXECLIST_ENABLE);
	TRACE("card %" B_PRId32 ": GFX_MODE = 0x%08" B_PRIx32 "\n", info->id,
		render_read32(*info, GEN9_GFX_MODE));

	// global hardware status page: 4 KiB BO in the kernel zone
	status = CreateGemLocked(B_PAGE_SIZE, NULL, &hwsObject);
	if (status == B_OK)
		status = KernelBindLocked(hwsObject);
	if (status != B_OK) {
		ERROR("card %" B_PRId32 ": could not set up HW status page: %s\n",
			info->id, strerror(status));
		initialized = true;
		return status;
	}
	hwsCpu = (uint8*)hwsObject->address;
	hwsGgtt = (uint32)hwsObject->ggtt_offset;
	memset(hwsCpu, 0, B_PAGE_SIZE);
		// seqno qword and park semaphore start at 0 (ring not parked)

	render_write32(*info, GEN9_RCS_HWS_PGA, hwsGgtt);
	uint32 hwsReadback = render_read32(*info, GEN9_RCS_HWS_PGA);
	TRACE("card %" B_PRId32 ": HWS_PGA = 0x%08" B_PRIx32 " (wrote 0x%"
		B_PRIx32 ")\n", info->id, hwsReadback, hwsGgtt);
	if (hwsReadback != hwsGgtt)
		ERROR("card %" B_PRId32 ": HWS_PGA readback mismatch!\n", info->id);

	// enable render interrupts (Genode enable_intr): RCS error mask,
	// clear pending, IER = MI_USER_INTERRUPT | CTX_SWITCH, IMR mirror,
	// HWSTAM mirror
	render_write32(*info, GEN9_RCS_EMR, 0xffffff00);
	render_write32(*info, GEN9_GT0_IIR, ~0u);
	uint32 ier = GEN9_GT_CS_MI_USER_INTERRUPT
		| GEN9_GT_CS_CTX_SWITCH_INTERRUPT;
	render_write32(*info, GEN9_GT0_IER, ier);
	render_write32(*info, GEN9_GT0_IMR, ~ier);
	render_read32(*info, GEN9_GT0_IMR);
	render_write32(*info, GEN9_RCS_HWSTAM, ~ier);
	render_read32(*info, GEN9_RCS_HWSTAM);

	// enable the master interrupt without touching the other bits (the
	// display side owns the register, set-only)
	render_write32(*info, GEN9_MASTER_INT_CTL,
		render_read32(*info, GEN9_MASTER_INT_CTL) | GEN9_MASTER_INT_ENABLE);

	interruptsEnabled = true;

	TRACE("card %" B_PRId32 ": render node ready, GT0_IER = 0x%08"
		B_PRIx32 ", MASTER_INT_CTL = 0x%08" B_PRIx32 "\n", info->id,
		render_read32(*info, GEN9_GT0_IER),
		render_read32(*info, GEN9_MASTER_INT_CTL));

	render_log_faults(*info, "init");

	last_submitted = 0;
	last_completed = 0;
	initialized = true;
	status = B_OK;
	return B_OK;
}


void
RenderNode::FlushGgtt()
{
	// flush the GFX TLB in the system agent after PTE writes
	render_write32(*info, GEN9_GFX_FLSH_CNTL, GEN9_GFX_FLSH_CNTL_FLUSH);
	render_read32(*info, GEN9_GFX_FLSH_CNTL);
	asm volatile("" ::: "memory");
}


render_gem_object*
RenderNode::FindObject(uint32 handle)
{
	if (handle == 0 || handle > MAX_HANDLES)
		return NULL;
	return objects[handle - 1];
}


status_t
RenderNode::CreateSyncobjLocked(render_open_cookie* cookie, bool signaled,
	uint32* _handle)
{
	render_syncobj* syncobj = new render_syncobj;
	if (syncobj == NULL)
		return B_NO_MEMORY;
	syncobj->seqno = 0;
	syncobj->signaled = signaled;
	syncobj->owner = cookie;

	uint32 handle = 0;
	for (int32 i = 0; i < MAX_SYNCOBJS; i++) {
		uint32 candidate = next_syncobj;
		next_syncobj = next_syncobj % MAX_SYNCOBJS + 1;
		if (syncobjs[candidate - 1] == NULL) {
			handle = candidate;
			break;
		}
	}
	if (handle == 0) {
		delete syncobj;
		return B_NO_MEMORY;
	}

	syncobj->handle = handle;
	syncobjs[handle - 1] = syncobj;
	*_handle = handle;
	return B_OK;
}


render_syncobj*
RenderNode::FindSyncobj(uint32 handle)
{
	if (handle == 0 || handle > MAX_SYNCOBJS)
		return NULL;
	return syncobjs[handle - 1];
}


void
RenderNode::DestroySyncobjLocked(render_syncobj* syncobj)
{
	syncobjs[syncobj->handle - 1] = NULL;
	delete syncobj;
}


bool
RenderNode::SyncobjSignaled(render_syncobj* syncobj)
{
	if (syncobj->signaled)
		return true;
	return syncobj->seqno != 0 && last_completed >= syncobj->seqno;
}


status_t
RenderNode::CreateGemLocked(size_t size, void* owner,
	render_gem_object** _object)
{
	render_gem_object* object = new render_gem_object;
	if (object == NULL)
		return B_NO_MEMORY;
	memset(object, 0, sizeof(render_gem_object));

	uint32 pageCount = size / B_PAGE_SIZE;
	object->pages = (phys_addr_t*)malloc(pageCount * sizeof(phys_addr_t));
	if (object->pages == NULL) {
		delete object;
		return B_NO_MEMORY;
	}

	object->area = create_area("intel render gem", &object->address,
		B_ANY_KERNEL_ADDRESS, size, B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA);
	if (object->area < B_OK) {
		status_t status = object->area;
		free(object->pages);
		delete object;
		return status;
	}

	// per-page scatter/gather list; no contiguity required
	for (uint32 i = 0; i < pageCount; i++) {
		physical_entry entry;
		if (get_memory_map((uint8*)object->address + i * B_PAGE_SIZE,
				B_PAGE_SIZE, &entry, 1) != B_OK || entry.size < B_PAGE_SIZE) {
			ERROR("get_memory_map failed for GEM page %" B_PRIu32 "\n", i);
			delete_area(object->area);
			free(object->pages);
			delete object;
			return B_ERROR;
		}
		object->pages[i] = entry.address;
	}

	object->size = size;
	object->page_count = pageCount;
	object->refcount = 1;
	object->bound = false;
	object->owner = owner;

	uint32 handle = 0;
	for (int32 i = 0; i < MAX_HANDLES; i++) {
		uint32 candidate = next_handle;
		next_handle = next_handle % MAX_HANDLES + 1;
		if (objects[candidate - 1] == NULL) {
			handle = candidate;
			break;
		}
	}
	if (handle == 0) {
		delete_area(object->area);
		free(object->pages);
		delete object;
		return B_NO_MEMORY;
	}

	object->handle = handle;
	objects[handle - 1] = object;
	*_object = object;
	return B_OK;
}


void
RenderNode::DestroyObjectLocked(render_gem_object* object)
{
	if (object->bound)
		UnbindObject(object);

	// revoke the object's PPGTT mappings in every context before its pages
	// are freed, so the GPU can no longer reach them
	if (object->mapped_va != 0) {
		for (int32 i = 0; i < MAX_CONTEXTS; i++) {
			if (contexts[i] != NULL && contexts[i]->ppgtt != NULL)
				contexts[i]->ppgtt->Unmap(object->mapped_va,
					object->page_count);
		}
		object->mapped_va = 0;
	}

	delete_area(object->area);
	free(object->pages);
	objects[object->handle - 1] = NULL;
	delete object;
}


status_t
RenderNode::BindAtLocked(render_gem_object* object, uint64 offset)
{
	uint64 entry = offset / B_PAGE_SIZE;
	for (uint32 i = 0; i < object->page_count; i++)
		ggtt[entry + i] = object->pages[i] | GEN9_GGTT_PTE_PRESENT;
	FlushGgtt();

	object->bound = true;
	object->ggtt_offset = offset;
	return B_OK;
}


status_t
RenderNode::BindObject(render_gem_object* object, uint64 offset)
{
	if (object->bound)
		return B_BUSY;
	if ((offset & (B_PAGE_SIZE - 1)) != 0)
		return B_BAD_VALUE;
	// user binds must not collide with the kernel zone
	if (offset < kernel_zone_end || offset + object->size > region_end) {
		ERROR("GEM bind: offset 0x%" B_PRIx64 " + 0x%" B_PRIxSIZE
			" outside user render region [0x%" B_PRIx64 ", 0x%" B_PRIx64
			")\n", offset, object->size, kernel_zone_end, region_end);
		return B_BAD_VALUE;
	}

	return BindAtLocked(object, offset);
}


status_t
RenderNode::KernelBindLocked(render_gem_object* object)
{
	// reuse a freed kernel-zone range of adequate size if one exists
	uint64 offset = 0;
	bool reused = false;
	for (int32 i = 0; i < kernelFreeCount; i++) {
		if (kernelFreeSize[i] >= object->size) {
			offset = kernelFreeOffset[i];
			if (kernelFreeSize[i] > object->size) {
				// keep the unused remainder so nothing is leaked
				kernelFreeOffset[i] += object->size;
				kernelFreeSize[i] -= object->size;
			} else {
				kernelFreeOffset[i] = kernelFreeOffset[kernelFreeCount - 1];
				kernelFreeSize[i] = kernelFreeSize[kernelFreeCount - 1];
				kernelFreeCount--;
			}
			reused = true;
			break;
		}
	}
	if (!reused) {
		if (kernel_bump + object->size > kernel_zone_end)
			return B_NO_MEMORY;
		offset = kernel_bump;
		kernel_bump += object->size;
	}

	object->kernelBound = true;
	TRACE("card %" B_PRId32 ": kernel object %u pages at GGTT 0x%"
		B_PRIx64 "%s\n", info->id, object->page_count, offset,
		reused ? " (reused)" : "");
	return BindAtLocked(object, offset);
}


status_t
RenderNode::UnbindObject(render_gem_object* object)
{
	if (!object->bound)
		return B_BAD_VALUE;

	// if this object lives in the kernel GGTT zone, return its range to the
	// free list so repeated context creation does not exhaust the zone
	if (object->kernelBound) {
		if (kernelFreeCount < kKernelFreeMax) {
			kernelFreeOffset[kernelFreeCount] = object->ggtt_offset;
			kernelFreeSize[kernelFreeCount] = object->size;
			kernelFreeCount++;
		}
		object->kernelBound = false;
	}

	uint64 entry = object->ggtt_offset / B_PAGE_SIZE;
	uint64 scratchPte = scratch_phys | GEN9_GGTT_PTE_PRESENT;
	for (uint32 i = 0; i < object->page_count; i++)
		ggtt[entry + i] = scratchPte;
	FlushGgtt();

	object->bound = false;
	object->ggtt_offset = 0;
	return B_OK;
}


//	#pragma mark - PPGTT


status_t
render_ppgtt::Init()
{
	pool = create_area("intel render ppgtt", (void**)&address,
		B_ANY_KERNEL_ADDRESS, PPGTT_POOL_PAGES * B_PAGE_SIZE, B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (pool < B_OK)
		return pool;

	for (uint32 i = 0; i < PPGTT_POOL_PAGES; i++) {
		physical_entry entry;
		if (get_memory_map(address + i * B_PAGE_SIZE, B_PAGE_SIZE, &entry, 1)
				!= B_OK || entry.size < B_PAGE_SIZE) {
			delete_area(pool);
			pool = -1;
			return B_ERROR;
		}
		page_phys[i] = entry.address;
	}

	// scratch chain: page -> pt -> pd -> pdp, each level pointing at the next
	uint64* page = (uint64*)(address + 0 * B_PAGE_SIZE);
	uint64* pt = (uint64*)(address + 1 * B_PAGE_SIZE);
	uint64* pd = (uint64*)(address + 2 * B_PAGE_SIZE);
	uint64* pdp = (uint64*)(address + 3 * B_PAGE_SIZE);
	pml4 = (uint64*)(address + 4 * B_PAGE_SIZE);

	scratch_page = page_phys[0];
	scratch_pt = page_phys[1];
	scratch_pd = page_phys[2];
	scratch_pdp = page_phys[3];
	pml4_phys = page_phys[4];
	used = 5;

	memset(page, 0, B_PAGE_SIZE);
	for (uint32 i = 0; i < 512; i++) {
		pt[i] = scratch_page | PPGTT_PTE_FLAGS;
		pd[i] = scratch_pt | PPGTT_PTE_FLAGS;
		pdp[i] = scratch_pd | PPGTT_PTE_FLAGS;
		pml4[i] = scratch_pdp | PPGTT_PTE_FLAGS;
	}
	render_clflush_page(pt);
	render_clflush_page(pd);
	render_clflush_page(pdp);
	render_clflush_page(pml4);
	return B_OK;
}


void
render_ppgtt::Destroy()
{
	if (pool >= 0)
		delete_area(pool);
	pool = -1;
}


uint64*
render_ppgtt::TableAt(uint64 phys)
{
	for (uint32 i = 5; i < used; i++) {
		if (page_phys[i] == phys)
			return (uint64*)(address + i * B_PAGE_SIZE);
	}
	return NULL;
}


uint64*
render_ppgtt::EnsureTable(uint64* entry, uint64 scratchPhys, uint64 fillPhys)
{
	uint64 phys = *entry & PPGTT_ADDRESS_MASK;
	if (phys != scratchPhys)
		return TableAt(phys);

	// the entry still points at the scratch table: allocate a real one,
	// primed with the next level's scratch entries
	if (used >= PPGTT_POOL_PAGES)
		return NULL;

	uint32 index = used++;
	uint64* table = (uint64*)(address + index * B_PAGE_SIZE);
	for (uint32 i = 0; i < 512; i++)
		table[i] = fillPhys | PPGTT_PTE_FLAGS;
	render_clflush_page(table);

	*entry = page_phys[index] | PPGTT_PTE_FLAGS;
	render_clflush(entry);
	return table;
}


status_t
render_ppgtt::Map(uint64 va, const phys_addr_t* pages, uint32 count)
{
	for (uint32 n = 0; n < count; n++) {
		uint64 entry = va + (uint64)n * B_PAGE_SIZE;

		uint32 i4 = (entry >> 39) & 0x1ff;
		uint32 i3 = (entry >> 30) & 0x1ff;
		uint32 i2 = (entry >> 21) & 0x1ff;
		uint32 i1 = (entry >> 12) & 0x1ff;

		uint64* pdp = EnsureTable(pml4 + i4, scratch_pdp, scratch_pd);
		if (pdp == NULL)
			return B_NO_MEMORY;
		uint64* pd = EnsureTable(pdp + i3, scratch_pd, scratch_pt);
		if (pd == NULL)
			return B_NO_MEMORY;
		uint64* pt = EnsureTable(pd + i2, scratch_pt, scratch_page);
		if (pt == NULL)
			return B_NO_MEMORY;

		pt[i1] = (pages[n] & PPGTT_ADDRESS_MASK) | PPGTT_PTE_FLAGS;
		render_clflush(pt + i1);
	}
	return B_OK;
}


status_t
render_ppgtt::MapRange(render_gem_object* object, uint64 va)
{
	status_t status = Map(va, object->pages, object->page_count);
	if (status == B_OK)
		object->mapped_va = va;
	return status;
}


//	Point the object's PTEs back at the scratch page so a freed BO is no
//	longer GPU-reachable. Only walks existing tables (never allocates).
status_t
render_ppgtt::Unmap(uint64 va, uint32 count)
{
	for (uint32 n = 0; n < count; n++) {
		uint64 entry = va + (uint64)n * B_PAGE_SIZE;
		uint32 i4 = (entry >> 39) & 0x1ff;
		uint32 i3 = (entry >> 30) & 0x1ff;
		uint32 i2 = (entry >> 21) & 0x1ff;
		uint32 i1 = (entry >> 12) & 0x1ff;

		if ((pml4[i4] & PPGTT_ADDRESS_MASK) == scratch_pdp)
			continue;
		uint64* pdp = TableAt(pml4[i4] & PPGTT_ADDRESS_MASK);
		if (pdp == NULL || (pdp[i3] & PPGTT_ADDRESS_MASK) == scratch_pd)
			continue;
		uint64* pd = TableAt(pdp[i3] & PPGTT_ADDRESS_MASK);
		if (pd == NULL || (pd[i2] & PPGTT_ADDRESS_MASK) == scratch_pt)
			continue;
		uint64* pt = TableAt(pd[i2] & PPGTT_ADDRESS_MASK);
		if (pt == NULL)
			continue;

		pt[i1] = scratch_page | PPGTT_PTE_FLAGS;
		render_clflush(pt + i1);
	}
	return B_OK;
}


//	#pragma mark - contexts & submission


void
RenderNode::SetupContextImage(render_context* context)
{
	// Logical ring context register image, transcribed from Genode
	// context.h Rcs_context (RCS). The image starts at page 2 of the
	// context BO; page 1 (the LRCA page) is the per-process HW status
	// page and stays zeroed.
	uint32* image = context->image;
	uint32 ringStart = context->ringGgtt;

	// execlist part: LRI header + 14 register pairs
	image[0x0000] = MI_NOOP;
	image[0x0001] = 0x1100101B;
		// MI_LOAD_REGISTER_IMM(14) | MI_LRI_FORCE_POSTED
	image[0x0002] = GEN9_RCS_CTX_CONTROL;
	image[0x0003] = 0x000F0009;
		// restore-inhibit + inhibit-sync-ctx-switch, masks set
		// (Genode Execlist_context::setup)
	image[0x0004] = GEN9_RCS_RING_HEAD;
	image[0x0005] = 0;
	image[0x0006] = GEN9_RCS_RING_TAIL;
	image[0x0007] = 0;	// tail updated on each submission
	image[0x0008] = GEN9_RCS_RING_START;
	image[0x0009] = ringStart;
	image[0x000A] = GEN9_RCS_RING_CTL;
	image[0x000B] = ((RING_PAGES - 1) << 12) | GEN9_RING_CTL_ENABLE;
	image[0x000C] = GEN9_RCS_BB_ADDR_UDW;
	image[0x000D] = 0;
	image[0x000E] = GEN9_RCS_BB_ADDR;
	image[0x000F] = 0;
	image[0x0010] = GEN9_RCS_BB_STATE;
	image[0x0011] = 0;	// ASI bit5 = 0: GGTT (Genode sets 1 for PPGTT)
	image[0x0012] = GEN9_RCS_SBB_ADDR_UDW;
	image[0x0013] = 0;
	image[0x0014] = GEN9_RCS_SBB_ADDR;
	image[0x0015] = 0;
	image[0x0016] = GEN9_RCS_SBB_STATE;
	image[0x0017] = 0;
	image[0x0018] = GEN9_RCS_BB_PER_CTX_PTR;
	image[0x0019] = 0;
	image[0x001A] = GEN9_RCS_INDIRECT_CTX;
	image[0x001B] = 0;
	image[0x001C] = GEN9_RCS_INDIRECT_CTX_OFFSET;
	image[0x001D] = 0;
	image[0x001E] = 0;	// RCS noops
	image[0x001F] = 0;

	// PPGTT part: LRI header + 9 pairs. The four PDP pairs all point at the
	// context's PML4 so any of the four 512 GiB quadrants can be used (i915
	// sets all four; Genode only needs PDP0 because its VAs stay low).
	uint32 pml4Ldw = (uint32)(context->ppgtt->pml4_phys
		| PPGTT_PTE_PRESENT);
	uint32 pml4Udw = (uint32)(context->ppgtt->pml4_phys >> 32);

	image[0x0020] = MI_NOOP;
	image[0x0021] = 0x11001011;
		// MI_LOAD_REGISTER_IMM(9) without force-posted (Genode
		// Ppgtt_context::setup writes the same header)
	image[0x0022] = GEN9_RCS_CS_CTX_TIMESTAMP;
	image[0x0023] = 0;
	image[0x0024] = GEN9_RCS_PDP3_UDW;
	image[0x0025] = pml4Udw;
	image[0x0026] = GEN9_RCS_PDP3_LDW;
	image[0x0027] = pml4Ldw;
	image[0x0028] = GEN9_RCS_PDP2_UDW;
	image[0x0029] = pml4Udw;
	image[0x002A] = GEN9_RCS_PDP2_LDW;
	image[0x002B] = pml4Ldw;
	image[0x002C] = GEN9_RCS_PDP1_UDW;
	image[0x002D] = pml4Udw;
	image[0x002E] = GEN9_RCS_PDP1_LDW;
	image[0x002F] = pml4Ldw;
	image[0x0030] = GEN9_RCS_PDP0_UDW;
	image[0x0031] = pml4Udw;
	image[0x0032] = GEN9_RCS_PDP0_LDW;
	image[0x0033] = pml4Ldw;
	// dwords 0x0034-0x0040 stay 0 (noops)
	image[0x0041] = 0x11000001;
		// MI_LOAD_REGISTER_IMM(1)
	image[0x0042] = GEN9_R_PWR_CLK_STATE;
	image[0x0043] = 0;
	// dwords 0x0044-0x004F stay 0 (GPGPU_CSR_BASE + noops)
	// the rest of the context image (engine/extended state) stays zeroed
}


render_context*
RenderNode::CreateContextLocked(render_open_cookie* cookie)
{
	// find a free context slot
	int32 slot = -1;
	for (int32 i = 0; i < MAX_CONTEXTS; i++) {
		if (contexts[i] == NULL) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return NULL;

	render_context* context = new render_context;
	if (context == NULL)
		return NULL;
	memset(context, 0, sizeof(render_context));

	if (waitSems[slot] < B_OK) {
		waitSems[slot] = create_sem(0, "intel render wait");
		if (waitSems[slot] < B_OK) {
			delete context;
			return NULL;
		}
	}

	if (CreateGemLocked(CONTEXT_PAGES * B_PAGE_SIZE, cookie,
			&context->contextBo) != B_OK
		|| KernelBindLocked(context->contextBo) != B_OK
		|| CreateGemLocked(RING_PAGES * B_PAGE_SIZE, cookie,
			&context->ringBo) != B_OK
		|| KernelBindLocked(context->ringBo) != B_OK) {
		ERROR("card %" B_PRId32 ": context allocation failed\n", info->id);
		if (context->ringBo != NULL)
			DestroyObjectLocked(context->ringBo);
		if (context->contextBo != NULL)
			DestroyObjectLocked(context->contextBo);
		delete context;
		return NULL;
	}

	context->ppgtt = new render_ppgtt;
	if (context->ppgtt != NULL) {
		memset(context->ppgtt, 0, sizeof(render_ppgtt));
		context->ppgtt->pool = -1;
	}
	if (context->ppgtt == NULL || context->ppgtt->Init() != B_OK) {
		ERROR("card %" B_PRId32 ": PPGTT allocation failed\n", info->id);
		if (context->ppgtt != NULL) {
			context->ppgtt->Destroy();
			delete context->ppgtt;
		}
		DestroyObjectLocked(context->ringBo);
		DestroyObjectLocked(context->contextBo);
		delete context;
		return NULL;
	}

	context->id = slot + 1;
	context->semSlot = slot;
	context->lrca = (uint32)context->contextBo->ggtt_offset + B_PAGE_SIZE;
	context->image = (uint32*)((uint8*)context->contextBo->address
		+ CONTEXT_REG_IMAGE_PAGE * B_PAGE_SIZE);
	context->ring = (uint32*)context->ringBo->address;
	context->ringGgtt = (uint32)context->ringBo->ggtt_offset;
	context->ringTail = 0;
	context->lastSubmitted = 0;
	context->dead = false;

	context->owner = cookie;
	SetupContextImage(context);

	contexts[slot] = context;

	TRACE("card %" B_PRId32 ": context %u created, LRCA 0x%" B_PRIx32
		", ring at GGTT 0x%" B_PRIx32 "\n", info->id, context->id,
		context->lrca, context->ringGgtt);
	return context;
}


render_context*
RenderNode::EnsureContextLocked(render_open_cookie* cookie)
{
	if (cookie->context != NULL)
		return cookie->context;

	render_context* context = CreateContextLocked(cookie);
	if (context != NULL)
		cookie->context = context;
	return context;
}


render_context*
RenderNode::ContextByIdLocked(render_open_cookie* cookie, uint32 id)
{
	if (id == 0 || id > MAX_CONTEXTS)
		return NULL;
	render_context* context = contexts[id - 1];
	if (context == NULL || context->owner != cookie)
		return NULL;
	return context;
}


void
RenderNode::DestroyContextObjLocked(render_context* context)
{
	if (context == NULL)
		return;

	render_open_cookie* cookie = context->owner;

	contexts[context->id - 1] = NULL;
	if (cookie != NULL && cookie->context == context)
		cookie->context = NULL;

	// drain in-flight work before the context memory goes away
	bigtime_t deadline = system_time() + 250000;
	while (last_completed < context->lastSubmitted
			&& system_time() < deadline)
		snooze(1000);
	if (last_completed < context->lastSubmitted) {
		ERROR("destroy: context %" B_PRIu32 " still in flight (completed "
			"%" B_PRIu32 ", submitted %" B_PRIu32 "), leaking its objects\n",
			context->id, last_completed, context->lastSubmitted);
		// strip ownership so the live context/ring images are not freed
		if (context->contextBo != NULL)
			context->contextBo->owner = NULL;
		if (context->ringBo != NULL)
			context->ringBo->owner = NULL;
	}

	if (context->contextBo != NULL && context->contextBo->owner == cookie)
		DestroyObjectLocked(context->contextBo);
	if (context->ringBo != NULL && context->ringBo->owner == cookie)
		DestroyObjectLocked(context->ringBo);
	if (context->ppgtt != NULL) {
		context->ppgtt->Destroy();
		delete context->ppgtt;
		context->ppgtt = NULL;
	}
	delete context;
}


void
RenderNode::DestroyContextLocked(render_open_cookie* cookie)
{
	DestroyContextObjLocked(cookie->context);
}


void
RenderNode::DestroyContextByIdLocked(render_open_cookie* cookie, uint32 id)
{
	DestroyContextObjLocked(ContextByIdLocked(cookie, id));
}


bool
RenderNode::BatchValidLocked(render_open_cookie* cookie, uint64 addr,
	uint32 length)
{
	if (length == 0 || (addr & 7) != 0)
		return false;

	for (int32 i = 0; i < MAX_HANDLES; i++) {
		render_gem_object* object = objects[i];
		if (object != NULL && object->owner == cookie && object->bound
			&& addr >= object->ggtt_offset
			&& addr + length <= object->ggtt_offset + object->size)
			return true;
	}
	return false;
}


status_t
RenderNode::SubmitLocked(render_context* context, uint64 batchAddr,
	uint32 batchLength, bool ppgtt, uint32* _seqno)
{
	if (context->dead)
		return B_ERROR;

	// Submissions complete before returning (see below), so the consumed ring
	// is reusable. Pad to its end with NOOPs rather than resetting HEAD; the
	// command streamer wraps naturally to the new commands at offset zero.
	if (context->ringTail + RING_SEQUENCE_DWORDS * 4 >= RING_SIZE) {
		for (uint32 offset = context->ringTail; offset < RING_SIZE; offset += 4)
			context->ring[offset / 4] = MI_NOOP;
		context->ringTail = 0;
	}

	// watchdog stub: complain about suspiciously old in-flight work
	if (last_submitted - last_completed > 4)
		ERROR("card %" B_PRId32 ": %u submissions in flight "
			"(completed %u, submitted %u)\n", info->id,
			last_submitted - last_completed, last_completed, last_submitted);

	uint32 seqno = last_submitted + 1;

	uint32* ring = context->ring;
	uint32 tail = context->ringTail;

	#define EMIT(value) ring[tail / 4] = (value); tail += 4

	// empty pipe control (Gen9, gen8_emit_flush_rcs)
	EMIT(MI_PIPE_CONTROL_GEN9);
	EMIT(0);
	EMIT(0);
	EMIT(0);
	EMIT(0);
	EMIT(0);

	// prolog: invalidate caches, write PPHWSP dword 52 (0xD0)
	// (STORE_DATA_INDEX without GLOBAL_GTT_IVB = PPHWSP-relative)
	EMIT(MI_PIPE_CONTROL_GEN9);
	EMIT(PIPE_CONTROL_CS_STALL | PIPE_CONTROL_TLB_INVALIDATE
		| PIPE_CONTROL_INSTRUCTION_INVALIDATE
		| PIPE_CONTROL_TEXTURE_INVALIDATE | PIPE_CONTROL_VF_CACHE_INVALIDATE
		| PIPE_CONTROL_CONST_CACHE_INVALIDATE
		| PIPE_CONTROL_STATE_CACHE_INVALIDATE | PIPE_CONTROL_QW_WRITE
		| PIPE_CONTROL_STORE_DATA_INDEX);
	EMIT(0x34 * 4);
	EMIT(0);
	EMIT(0);
	EMIT(0);

	// batch buffer start (gen8_emit_bb_start, Gen9): ARB on, batch with
	// the address-space indicator in bit 8 (0 = GGTT, 1 = PPGTT), ARB off
	EMIT(MI_ARB_ON_OFF(1));
	EMIT(MI_BATCH_BUFFER_START | 1
		| (ppgtt ? MI_BATCH_BUFFER_START_PPGTT : 0));
		// 0x18800001 (GGTT) or 0x18800101 (PPGTT): 3 dwords
	EMIT((uint32)(batchAddr & 0xffffffff));
	EMIT((uint32)((batchAddr >> 32) & 0xffff));
	EMIT(MI_ARB_ON_OFF(0));
	EMIT(MI_NOOP);

	// epilog 1/3 (gen8_emit_pipe_control): flush render/depth caches
	EMIT(MI_PIPE_CONTROL_GEN9);
	EMIT(PIPE_CONTROL_RENDER_TARGET_FLUSH | PIPE_CONTROL_DEPTH_CACHE_FLUSH
		| PIPE_CONTROL_DC_FLUSH_ENABLE);
	EMIT(0);
	EMIT(0);
	EMIT(0);
	EMIT(0);

	// epilog 2/3 (gen8_emit_ggtt_write_rcs): write the seqno qword to
	// the global HW status page at +0xC0
	EMIT(MI_PIPE_CONTROL_GEN9);
	EMIT(PIPE_CONTROL_CS_STALL | PIPE_CONTROL_FLUSH_ENABLE
		| PIPE_CONTROL_GLOBAL_GTT_IVB | PIPE_CONTROL_QW_WRITE
		| PIPE_CONTROL_STORE_DATA_INDEX);
	EMIT(HWS_SEQNO_OFFSET);
	EMIT(0);
	EMIT(seqno);
	EMIT(0);

	// park point (emit_preempt_busywait): spin while the HW status page
	// semaphore at +0xC8 is non-zero
	EMIT(MI_ARB_CHECK);
	EMIT(MI_SEMAPHORE_WAIT);
	EMIT(0);					// compare data
	EMIT(hwsGgtt + HWS_SEMAPHORE_OFFSET);
	EMIT(0);
	EMIT(MI_NOOP);

	// completion interrupt
	EMIT(MI_ARB_ON_OFF(1));
	EMIT(MI_USER_INTERRUPT);

	// epilog 3/3 (gen8_emit_fini_breadcrumb_tail)
	EMIT(MI_ARB_CHECK);
	EMIT(MI_NOOP);

	#undef EMIT

	if ((tail & 7) != 0) {
		ERROR("card %" B_PRId32 ": ring tail misaligned\n", info->id);
		return B_ERROR;
	}

	context->ringTail = tail;

	// update the ring tail in the context image (byte offset, bits 20:3),
	// and advertise this submission's addressing mode in BB_STATE (ASI)
	context->image[0x0011] = ppgtt ? (1u << 5) : 0;
	context->image[0x0007] = tail;

	last_submitted = seqno;
	context->lastSubmitted = seqno;

	// submit (Genode _submit_execlist / _schedule_current_vgpu):
	// TLB flush, warn if the execlist is not idle, then write the two
	// elements to the submit port (elem1 high, elem1 low, elem0 high,
	// elem0 low)
	FlushGgtt();

	uint32 elStatus = 0;
	for (int32 i = 0; i < 100; i++) {
		elStatus = render_read32(*info, GEN9_RCS_EXECLIST_STATUS);
		if ((elStatus & (GEN9_EXECLIST_STATUS_0_VALID
				| GEN9_EXECLIST_STATUS_1_VALID)) == 0)
			break;
	}
	if ((elStatus & (GEN9_EXECLIST_STATUS_0_VALID
			| GEN9_EXECLIST_STATUS_1_VALID)) != 0)
		ERROR("card %" B_PRId32 ": execlist still busy (0x%08" B_PRIx32
			"), submitting anyway\n", info->id, elStatus);

	// context descriptor: LRCA | privilege | addressing mode | force
	// restore | force PD restore | valid. The context id occupies bits
	// 51:32 (i915 GEN8_CTX_ID_SHIFT == 32), i.e. the low 20 bits of the
	// high dword, so the high dword is the raw id. (Previously id << 8
	// put it at bits 39:8, which the hardware reads as id 0 - an invalid
	// context id, so the 3D context state was never restored.)
	uint32 descLo = context->lrca | GEN9_CTX_DESC_PRIVILEGE
		| (ppgtt ? GEN9_CTX_DESC_LEGACY_64 : GEN9_CTX_DESC_LEGACY_32)
		| GEN9_CTX_DESC_FORCE_RESTORE | GEN9_CTX_DESC_FORCE_PD_RESTORE
		| GEN9_CTX_DESC_VALID;
	uint32 descHi = context->id & 0xfffff;
	context->lastDescriptorLo = descLo;

	asm volatile("mfence" ::: "memory");
	render_write32(*info, GEN9_RCS_EXECLIST_SUBMITPORT, 0);
	render_write32(*info, GEN9_RCS_EXECLIST_SUBMITPORT, 0);
	render_write32(*info, GEN9_RCS_EXECLIST_SUBMITPORT, descHi);
	render_write32(*info, GEN9_RCS_EXECLIST_SUBMITPORT, descLo);

	TRACE("card %" B_PRId32 ": submitted seqno %u (batch 0x%" B_PRIx64
		", %u bytes)\n", info->id, seqno, batchAddr, batchLength);

	// This minimal driver has no request scheduler or per-BO reservation
	// fences. Complete each submission before returning, under node->lock,
	// so GEM_BUSY/WAIT, CPU mappings, BO destruction and context switching
	// cannot race unfinished writes. A short spin avoids a scheduler tick
	// for the usual small batch; long work yields. Wait for context switch
	// completion too, before the next submit edits the saved context image.
	asm volatile("mfence" ::: "memory");
	bigtime_t waitStart = system_time();
	while (true) {
		uint32 completed = *(volatile uint32*)(hwsCpu + HWS_SEQNO_OFFSET);
		uint32 active = render_read32(*info, GEN9_RCS_EXECLIST_STATUS);
		if (completed == seqno
			&& (active & (GEN9_EXECLIST_STATUS_0_VALID
				| GEN9_EXECLIST_STATUS_1_VALID)) == 0) {
			last_completed = completed;
			asm volatile("mfence" ::: "memory");
			break;
		}
		bigtime_t elapsed = system_time() - waitStart;
		if (elapsed >= 1000000) {
			context->dead = true;
			ERROR("submission %u timed out (completed %u, execlist %#x)\n",
				seqno, completed, active);
			return B_TIMED_OUT;
		}
		if (elapsed < 100)
			asm volatile("pause" ::: "memory");
		else
			snooze(50);
	}

	*_seqno = seqno;
	return B_OK;
}


//	#pragma mark - device hooks


static status_t
render_open(const char* name, uint32 /*flags*/, void** _cookie)
{
	CALLED();

	// find the card for this render device name
	int32 cardIndex = -1;
	for (int32 i = 0; gRenderDeviceNames[i] != NULL; i++) {
		if (strcmp(name, gRenderDeviceNames[i]) == 0) {
			cardIndex = gRenderDeviceCards[i];
			break;
		}
	}
	if (cardIndex < 0)
		return B_BAD_VALUE;

	intel_info* info = gDeviceInfo[cardIndex];
	if (info == NULL)
		return B_BAD_VALUE;

	// the render node shares the display driver's register mapping and
	// interrupt handler: make sure the base device is initialized, and
	// hold a reference so device_free() cannot uninit it under our feet
	mutex_lock(&gLock);
	if (info->open_count == 0)
		info->init_status = intel_extreme_init(*info);
	if (info->init_status == B_OK)
		info->open_count++;
	status_t status = info->init_status;
	mutex_unlock(&gLock);

	if (status != B_OK) {
		ERROR("open %s: base device init failed: %s\n", name,
			strerror(status));
		return status;
	}

	// get or create the render node for this card
	mutex_lock(&gLock);
	RenderNode* node = sRenderNodes[cardIndex];
	if (node == NULL) {
		node = new RenderNode();
		if (node != NULL) {
			memset(node, 0, sizeof(RenderNode));
			node->info = info;
			node->status = B_NO_INIT;
			node->ggtt_area = -1;
			node->scratch_area = -1;
			node->next_handle = 1;
			node->next_syncobj = 1;
			mutex_init(&node->lock, "intel render node");
			for (int32 i = 0; i < MAX_CONTEXTS; i++)
				node->waitSems[i] = B_BAD_SEM_ID;
			sRenderNodes[cardIndex] = node;
		}
	}
	mutex_unlock(&gLock);

	if (node == NULL) {
		status = B_NO_MEMORY;
		goto err_release_device;
	}

	mutex_lock(&node->lock);
	if (!node->initialized)
		node->Init();
	status = node->status;
	mutex_unlock(&node->lock);

	if (status != B_OK) {
		ERROR("open %s: render node init failed: %s\n", name,
			strerror(status));
		goto err_release_device;
	}

	{
		render_open_cookie* cookie
			= (render_open_cookie*)malloc(sizeof(render_open_cookie));
		if (cookie == NULL) {
			status = B_NO_MEMORY;
			goto err_release_device;
		}
		cookie->magic = RENDER_OPEN_COOKIE_MAGIC;
		cookie->info = info;
		cookie->node = node;
		cookie->context = NULL;

		mutex_lock(&node->lock);
		node->open_count++;
		mutex_unlock(&node->lock);

		*_cookie = cookie;
	}
	return B_OK;

err_release_device:
	mutex_lock(&gLock);
	if (info->open_count-- == 1) {
		info->init_status = B_NO_INIT;
		intel_extreme_uninit(*info);
	}
	mutex_unlock(&gLock);
	return status;
}


static status_t
render_close(void* /*data*/)
{
	CALLED();
	return B_OK;
}


static status_t
render_free(void* data)
{
	render_open_cookie* cookie = (render_open_cookie*)data;
	if (cookie == NULL || cookie->magic != RENDER_OPEN_COOKIE_MAGIC)
		return B_BAD_VALUE;

	RenderNode* node = cookie->node;
	intel_info* info = cookie->info;

	if (cookie->context != NULL) {
		// remove it from the visible list, drain in-flight work, then free
		// the context, its PPGTT and its context/ring objects
		mutex_lock(&node->lock);
		node->DestroyContextLocked(cookie);
		mutex_unlock(&node->lock);
	}

	// drop any remaining DRM contexts this client created but did not destroy
	mutex_lock(&node->lock);
	for (int32 i = 0; i < MAX_CONTEXTS; i++) {
		render_context* context = node->contexts[i];
		if (context != NULL && context->owner == cookie)
			node->DestroyContextObjLocked(context);
	}
	mutex_unlock(&node->lock);

	// drop GEM objects this client left behind
	mutex_lock(&node->lock);
	for (int32 i = 0; i < MAX_HANDLES; i++) {
		render_gem_object* object = node->objects[i];
		if (object != NULL && object->owner == cookie) {
			ERROR("free: destroying leftover GEM handle %" B_PRIu32 "\n",
				object->handle);
			node->DestroyObjectLocked(object);
		}
	}
	for (int32 i = 0; i < MAX_SYNCOBJS; i++) {
		render_syncobj* syncobj = node->syncobjs[i];
		if (syncobj != NULL && syncobj->owner == cookie)
			node->DestroySyncobjLocked(syncobj);
	}
	node->open_count--;
	mutex_unlock(&node->lock);

	cookie->magic = 0;
	free(cookie);

	mutex_lock(&gLock);
	if (info->open_count-- == 1) {
		info->init_status = B_NO_INIT;
		intel_extreme_uninit(*info);
	}
	mutex_unlock(&gLock);
	return B_OK;
}


//	#pragma mark - Linux-encoded DRM ioctls (libdrm/iris compatibility)

#define DRM_IS_IOCTL(op)	(((op) & 0xff00) == ((uint32)DRM_IOCTL_BASE << 8))


/*!	Look up a GEM object that belongs to this open instance; kernel-internal
	objects (HW status page, context/ring images) are not reachable from
	userspace ioctls. Call with node->lock held. */
static render_gem_object*
render_find_owned_object(RenderNode* node, render_open_cookie* cookie,
	uint32 handle)
{
	render_gem_object* object = node->FindObject(handle);
	if (object != NULL && object->owner != cookie)
		return NULL;
	return object;
}


/*!	Read the Gen9 slice/subslice/EU enable fuses exactly like the i915
	driver does (FUSE2 at 0x09120 plus the EU_DISABLE bytes at 0x9134),
	so iris can size its subslice/EU bookkeeping. */
static void
render_gen9_topology(intel_info& info, uint32* sliceMask, uint32* subsliceMask,
	uint32* euTotal)
{
	enum {
		kSliceMax = 3,
		kSubsliceMax = 4,
		kEusPerSubslice = 8
	};

	uint32 fuse2 = render_read32(info, 0x09120);
	uint32 slices = (fuse2 >> 25) & 0x7;
	uint32 subslices = (~(fuse2 >> 20)) & 0xf;

	uint32 eus = 0;
	for (uint32 byte = 0; byte < 12; byte++) {
		uint32 bits = byte * 8;
		if (bits >= kSliceMax * kSubsliceMax * kEusPerSubslice)
			break;

		uint32 slice = bits / (kSubsliceMax * kEusPerSubslice);
		uint32 subslice = bits / kEusPerSubslice;
		if ((subslices & (1u << subslice)) == 0)
			continue;
		if ((slices & (1u << slice)) == 0)
			continue;

		uint32 disabled = render_read32(info, 0x9134 + byte * 4);
		for (uint32 b = 0; b < 8; b++) {
			if ((disabled & (1u << b)) == 0)
				eus++;
		}
	}

	*sliceMask = slices;
	*subsliceMask = subslices;
	*euTotal = eus;
}


static status_t
render_drm_getparam(RenderNode* node, render_open_cookie* cookie, void* buffer)
{
	drm_i915_getparam args;
	if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
		return B_BAD_ADDRESS;
	if (args.value == NULL)
		return DRM_EINVAL;

	int32 value = 0;
	intel_info* info = cookie->info;
	bool unsupported = false;

	switch (args.param) {
		case I915_PARAM_CHIPSET_ID:
			value = info->pci->device_id;
			break;
		case I915_PARAM_REVISION:
			value = info->pci->revision;
			break;
		case I915_PARAM_MMAP_VERSION:
		case I915_PARAM_MMAP_GTT_VERSION:
			value = 0;	// forces legacy GEM_MMAP (Haiku has no GTT mmap)
			break;
		case I915_PARAM_HAS_GEM:
		case I915_PARAM_HAS_EXECBUF2:
		case I915_PARAM_HAS_LLC:
		case I915_PARAM_HAS_CONTEXT_ISOLATION:	// hard gate in iris_screen.c
		case I915_PARAM_HAS_EXEC_NO_RELOC:
		case I915_PARAM_HAS_EXEC_HANDLE_LUT:
		case I915_PARAM_HAS_EXEC_SOFTPIN:
		case I915_PARAM_HAS_EXEC_BATCH_FIRST:
		case I915_PARAM_HAS_EXEC_FENCE_ARRAY:
		case I915_PARAM_HAS_RELAXED_FENCING:
		case I915_PARAM_HAS_RELAXED_DELTA:
		case I915_PARAM_HAS_GPU_RESET:
			value = 1;
			break;
		case I915_PARAM_CS_TIMESTAMP_FREQUENCY:
			// must be non-zero: iris divides by it (timer_resolution)
			value = 12000000;
			break;
		case I915_PARAM_EU_TOTAL:
		case I915_PARAM_SLICE_MASK:
		case I915_PARAM_SUBSLICE_MASK:
		case I915_PARAM_SUBSLICE_TOTAL:
		{
			uint32 sliceMask, subsliceMask, euTotal;
			render_gen9_topology(*info, &sliceMask, &subsliceMask, &euTotal);
			switch (args.param) {
				case I915_PARAM_SLICE_MASK:
					value = (int32)sliceMask;
					break;
				case I915_PARAM_SUBSLICE_MASK:
					value = (int32)subsliceMask;
					break;
				case I915_PARAM_SUBSLICE_TOTAL:
					value = __builtin_popcount(subsliceMask);
					break;
				default:
					value = (int32)euTotal;
					break;
			}
			break;
		}
		case I915_PARAM_HAS_BSD:
		case I915_PARAM_HAS_BLT:
		case I915_PARAM_HAS_VEBOX:
		case I915_PARAM_HAS_WAIT_TIMEOUT:
		case I915_PARAM_HAS_RESOURCE_STREAMER:
		case I915_PARAM_HAS_USERPTR_PROBE:
		case I915_PARAM_PERF_REVISION:
		case I915_PARAM_HAS_EXEC_SUBMIT_FENCE:
		case I915_PARAM_HAS_EXEC_TIMELINE_FENCES:
		case I915_PARAM_HAS_ALIASING_PPGTT:
			value = 0;
			break;
		default:
			TRACE("GETPARAM: unsupported param %d\n", (int)args.param);
			unsupported = true;
			break;
	}

	if (unsupported)
		return DRM_EINVAL;

	if (user_memcpy(args.value, &value, sizeof(value)) < B_OK)
		return B_BAD_ADDRESS;
	return B_OK;
}


#define DRM_MAX_QUERY_ITEMS	16


/*!	Handle DRM_IOCTL_I915_QUERY. iris needs a render engine from
	ENGINE_INFO, so that one is answered with a single Gen9 render engine.
	Every other query is reported as unsupported by storing a negative length
	in the item -- the i915 uAPI convention for a per-item error -- which makes
	Mesa fall back to its non-query paths (GETPARAM masks, os_ memory sizing,
	"no GuC"). Note: Haiku's ioctl() negates a negative driver status while
	setting errno, so returning an error here would look like success to Mesa;
	the negative length is the only clean signal. */
static status_t
render_drm_query(RenderNode* /*node*/, render_open_cookie* /*cookie*/,
	void* buffer)
{
	drm_i915_query args;
	if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
		return B_BAD_ADDRESS;
	if (args.num_items == 0 || args.num_items > DRM_MAX_QUERY_ITEMS
		|| args.items_ptr == 0)
		return DRM_EINVAL;

	uint8* items = (uint8*)(addr_t)args.items_ptr;
	for (uint32 i = 0; i < args.num_items; i++) {
		drm_i915_query_item item;
		if (user_memcpy(&item, items + i * sizeof(item), sizeof(item)) < B_OK)
			return B_BAD_ADDRESS;

		if (item.query_id == DRM_I915_QUERY_ENGINE_INFO) {
			const size_t size = sizeof(drm_i915_query_engine_info)
				+ sizeof(drm_i915_engine_info);
			if (item.data_ptr == 0) {
				/* size probe */
				item.length = (int32)size;
			} else if (item.length >= (int32)size) {
				drm_i915_query_engine_info info;
				memset(&info, 0, sizeof(info));
				info.num_engines = 1;
				if (user_memcpy((void*)(addr_t)item.data_ptr, &info,
						sizeof(info)) < B_OK)
					return B_BAD_ADDRESS;

				drm_i915_engine_info engine;
				memset(&engine, 0, sizeof(engine));
				engine.engine.engine_class = I915_ENGINE_CLASS_RENDER;
				engine.engine.engine_instance = 0;
				if (user_memcpy((uint8*)(addr_t)item.data_ptr + sizeof(info),
						&engine, sizeof(engine)) < B_OK)
					return B_BAD_ADDRESS;
				item.length = (int32)size;
			} else {
				item.length = -1;
			}
		} else {
			item.length = -1;
		}

		if (user_memcpy(items + i * sizeof(item), &item, sizeof(item)) < B_OK)
			return B_BAD_ADDRESS;
	}

	return B_OK;
}


#define DRM_MAX_EXEC_OBJECTS	64


/*!	Translate a Haiku status into the negative Linux errno a libdrm client
	expects. Small negatives are already DRM_* codes and pass through. */
static status_t
render_drm_errno(status_t status)
{
	if (status == B_OK || status >= -4096)
		return status;
	if (status == B_NO_MEMORY)
		return DRM_ENOMEM;
	if (status == B_TIMED_OUT)
		return DRM_ETIMEDOUT;
	if (status == B_BUSY)
		return DRM_EAGAIN;
	return DRM_EINVAL;
}


/*!	Wait until all (or any) of the given syncobjs are signaled, or the
	timeout expires. Returns B_OK, B_TIMED_OUT, or DRM_EINVAL. */
static status_t
render_drm_wait_syncobjs(RenderNode* node, render_open_cookie* cookie,
	const uint32* handles, uint32 count, bool waitAll, int64 timeoutNsec)
{
	bigtime_t deadline = 0;
	// 0 means "poll once and report timeout" (non-blocking); a negative
	// value is the u64 U64_MAX sentinel and waits indefinitely
	bool pollOnce = false;
	bool waitForever = false;
	if (timeoutNsec < 0)
		waitForever = true;
	else if (timeoutNsec == 0)
		pollOnce = true;
	else
		deadline = system_time() + timeoutNsec / 1000;

	while (true) {
		mutex_lock(&node->lock);
		uint32 signaled = 0;
		status_t status = B_OK;
		for (uint32 i = 0; i < count; i++) {
			render_syncobj* syncobj = node->FindSyncobj(handles[i]);
			if (syncobj == NULL || syncobj->owner != cookie) {
				status = DRM_EINVAL;
				break;
			}
			if (node->SyncobjSignaled(syncobj))
				signaled++;
		}
		mutex_unlock(&node->lock);

		if (status != B_OK)
			return status;
		if (waitAll ? signaled == count : signaled > 0)
			return B_OK;
		if (pollOnce)
			return B_TIMED_OUT;
		if (!waitForever && deadline != 0 && system_time() >= deadline)
			return B_TIMED_OUT;
		snooze(1000);
	}
}


/*!	DRM_IOCTL_I915_GEM_EXECBUFFER2. Objects softpin at 48-bit PPGTT
	addresses (EXEC_OBJECT_PINNED); the batch is objects[0] (BATCH_FIRST, as
	iris submits). The fence array orders the submission with seqno-backed
	syncobjs. */
static status_t
render_drm_execbuffer2(RenderNode* node, render_open_cookie* cookie,
	void* buffer)
{
	drm_i915_gem_execbuffer2 args;
	if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
		return B_BAD_ADDRESS;
	render_log_faults(*cookie->info, "pre");
	if (args.buffer_count == 0 || args.buffer_count > DRM_MAX_EXEC_OBJECTS)
		return DRM_EINVAL;
	if ((args.flags & I915_EXEC_BATCH_FIRST) == 0)
		return DRM_EINVAL;	// v1: the batch must be objects[0]

	drm_i915_gem_exec_object2 objects[DRM_MAX_EXEC_OBJECTS];
	uint32 count = args.buffer_count;
	if (user_memcpy(objects, (void*)(addr_t)args.buffers_ptr,
			count * sizeof(drm_i915_gem_exec_object2)) < B_OK)
		return B_BAD_ADDRESS;

	// fence array (I915_EXEC_FENCE_ARRAY): cliprects_ptr/num_cliprects
	drm_i915_gem_exec_fence fences[DRM_MAX_EXEC_OBJECTS];
	uint32 fenceCount = 0;
	if ((args.flags & I915_EXEC_FENCE_ARRAY) != 0 && args.num_cliprects > 0) {
		fenceCount = args.num_cliprects;
		if (fenceCount > DRM_MAX_EXEC_OBJECTS)
			return DRM_EINVAL;
		if (user_memcpy(fences, (void*)(addr_t)args.cliprects_ptr,
				fenceCount * sizeof(drm_i915_gem_exec_fence)) < B_OK)
			return B_BAD_ADDRESS;
	}

	// wait on every WAIT fence before touching the GPU. This runs with no
	// lock held and no context pointer live, so the blocking wait cannot
	// race a concurrent context destroy.
	for (uint32 i = 0; i < fenceCount; i++) {
		if ((fences[i].flags & I915_EXEC_FENCE_WAIT) == 0)
			continue;
		status_t waitStatus = render_drm_wait_syncobjs(node, cookie,
			&fences[i].handle, 1, true, 1000000000LL);
		if (waitStatus != B_OK)
			return waitStatus;
	}

	// from here the lock is held across ensure + map + submit + signal, so
	// the context cannot be freed underneath us
	mutex_lock(&node->lock);
	// i915 passes the context id in rsvd1; fall back to the client's default
	// context when it is 0 (native v1 clients do not set it). Bounds-check the
	// full 64-bit value so a high-bit id cannot alias a valid one.
	render_context* context = (args.rsvd1 != 0 && args.rsvd1 <= MAX_CONTEXTS)
		? node->ContextByIdLocked(cookie, (uint32)args.rsvd1)
		: (args.rsvd1 == 0 ? node->EnsureContextLocked(cookie) : NULL);
	if (context == NULL) {
		mutex_unlock(&node->lock);
		return DRM_ENOMEM;
	}
	if (context->ppgtt == NULL) {
		mutex_unlock(&node->lock);
		return DRM_EINVAL;
	}
	status_t status = B_OK;
	for (uint32 i = 0; i < count && status == B_OK; i++) {
		render_gem_object* object = render_find_owned_object(node, cookie,
			objects[i].handle);
		if (object == NULL
			|| (objects[i].flags & I915_GEM_EXEC_OBJECT_PINNED) == 0
			|| objects[i].offset == 0)
			status = DRM_EINVAL;
		else
			status = context->ppgtt->MapRange(object, objects[i].offset);
		// debug: dump the first dwords of write-back objects (render targets)
		// so a clear that lands can be seen across successive submissions
		if (object != NULL && object->address != NULL
			&& (objects[i].flags & I915_GEM_EXEC_OBJECT_WRITE) != 0) {
			volatile uint32* w = (volatile uint32*)object->address;
			TRACE("execwr[%u]: h=%u off=0x%llx sz=%zu : %08" B_PRIx32
				" %08" B_PRIx32 " %08" B_PRIx32 " %08" B_PRIx32 "\n",
				i, objects[i].handle, (unsigned long long)objects[i].offset,
				object->size, w[0], w[1], w[2], w[3]);
		}
	}

	uint32 seqno = 0;
	if (status == B_OK) {
		render_gem_object* batch = render_find_owned_object(node, cookie,
			objects[0].handle);
		if (batch == NULL || (args.batch_start_offset & 7) != 0
			|| args.batch_start_offset >= batch->size
			|| args.batch_len > batch->size - args.batch_start_offset) {
			status = DRM_EINVAL;
		} else {
			uint64 batchAddr = objects[0].offset + args.batch_start_offset;
			uint32 batchLength = args.batch_len != 0 ? args.batch_len
				: (uint32)(batch->size - args.batch_start_offset);
			if (batch->address != NULL) {
				volatile uint32* b = (volatile uint32*)
					((uint8*)batch->address + args.batch_start_offset);
				uint32 n = batchLength / 4;
				if (n > 12)
					n = 12;
				TRACE("execbatch[%u words]:", n);
				for (uint32 d = 0; d < n; d++)
					TRACE(" %08" B_PRIx32, b[d]);
				TRACE("\n");
			}
			status = node->SubmitLocked(context, batchAddr, batchLength,
				true, &seqno);
			render_log_faults(*cookie->info, "post");
		}
	}

	if (status == B_OK) {
		for (uint32 i = 0; i < fenceCount; i++) {
			if ((fences[i].flags & I915_EXEC_FENCE_SIGNAL) == 0)
				continue;
			render_syncobj* syncobj = node->FindSyncobj(fences[i].handle);
			if (syncobj != NULL && syncobj->owner == cookie) {
				syncobj->seqno = seqno;
				syncobj->signaled = false;
			}
		}
	}
	mutex_unlock(&node->lock);
	return render_drm_errno(status);
}


/*!	Linux-encoded (DRM_IOCTL_*) ioctls on the same render device. The
	opcode carries the DRM command number in its low byte and the
	argument size in bits 16..29, so structs are sized from the opcode and
	the passed length is ignored (Haiku passes 0 through the ioctl macro).
*/
static status_t
render_drm_control_impl(render_open_cookie* cookie, uint32 op, void* buffer)
{
	RenderNode* node = cookie->node;
	intel_info* info = cookie->info;

	switch (op) {
		case DRM_IOCTL_VERSION:
		{
			drm_version version;
			if (user_memcpy(&version, buffer, sizeof(version)) < B_OK)
				return B_BAD_ADDRESS;

			static const char kName[] = "i915";
			static const char kDate[] = "20260101";
			static const char kDesc[] = "Haiku intel_render_9x Gen9 render node";

			if (version.name != NULL && version.name_len > 0) {
				size_t len = sizeof(kName);
				if (len > version.name_len)
					len = version.name_len;
				if (user_memcpy(version.name, kName, len) < B_OK)
					return B_BAD_ADDRESS;
			}
			if (version.date != NULL && version.date_len > 0) {
				size_t len = sizeof(kDate);
				if (len > version.date_len)
					len = version.date_len;
				if (user_memcpy(version.date, kDate, len) < B_OK)
					return B_BAD_ADDRESS;
			}
			if (version.desc != NULL && version.desc_len > 0) {
				size_t len = sizeof(kDesc);
				if (len > version.desc_len)
					len = version.desc_len;
				if (user_memcpy(version.desc, kDesc, len) < B_OK)
					return B_BAD_ADDRESS;
			}
			version.version_major = 1;
			version.version_minor = 6;
			version.version_patchlevel = 0;
			version.name_len = sizeof(kName);
			version.date_len = sizeof(kDate);
			version.desc_len = sizeof(kDesc);
			if (user_memcpy(buffer, &version, sizeof(version)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case DRM_IOCTL_GEM_CLOSE:
		{
			drm_gem_close args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			mutex_lock(&node->lock);
			render_gem_object* object = render_find_owned_object(node, cookie,
				args.handle);
			// never let a client close a kernel-internal object (HW status
			// page, context/ring images) by guessing its handle
			if (object != NULL && object->owner != cookie)
				object = NULL;
			status_t status = DRM_EINVAL;
			if (object != NULL) {
				if (--object->refcount <= 0)
					node->DestroyObjectLocked(object);
				status = B_OK;
			}
			mutex_unlock(&node->lock);
			return status;
		}

		case DRM_IOCTL_GEM_FLINK:
		case DRM_IOCTL_GEM_OPEN:
		case DRM_IOCTL_PRIME_HANDLE_TO_FD:
		case DRM_IOCTL_PRIME_FD_TO_HANDLE:
			// no cross-device sharing in v1
			return DRM_ENOSYS;

		case DRM_IOCTL_I915_GETPARAM:
			return render_drm_getparam(node, cookie, buffer);

		case DRM_IOCTL_I915_GEM_CREATE:
		{
			drm_i915_gem_create args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			if (args.size == 0 || args.size > MAX_BO_SIZE)
				return DRM_EINVAL;
			size_t size = (args.size + B_PAGE_SIZE - 1)
				& ~(uint64)(B_PAGE_SIZE - 1);

			mutex_lock(&node->lock);
			render_gem_object* object = NULL;
			status_t status = node->CreateGemLocked(size, cookie, &object);
			mutex_unlock(&node->lock);
			if (status != B_OK)
				return status;

			args.handle = object->handle;
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK) {
				mutex_lock(&node->lock);
				if (--object->refcount <= 0)
					node->DestroyObjectLocked(object);
				mutex_unlock(&node->lock);
				return B_BAD_ADDRESS;
			}
			return B_OK;
		}

		case DRM_IOCTL_I915_GEM_CREATE_EXT:
		{
			drm_i915_gem_create_ext args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			// flags must be zero; extensions (memory regions, needs-cpu
			// access) are accepted but not honoured -- every object is
			// ordinary LLC-coherent system memory in v1
			if (args.flags != 0)
				return DRM_EINVAL;
			if (args.size == 0 || args.size > MAX_BO_SIZE)
				return DRM_EINVAL;
			size_t size = (args.size + B_PAGE_SIZE - 1)
				& ~(uint64)(B_PAGE_SIZE - 1);

			mutex_lock(&node->lock);
			render_gem_object* object = NULL;
			status_t status = node->CreateGemLocked(size, cookie, &object);
			mutex_unlock(&node->lock);
			if (status != B_OK)
				return status;

			args.handle = object->handle;
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK) {
				mutex_lock(&node->lock);
				if (--object->refcount <= 0)
					node->DestroyObjectLocked(object);
				mutex_unlock(&node->lock);
				return B_BAD_ADDRESS;
			}
			return B_OK;
		}

		case DRM_IOCTL_I915_GEM_MMAP:
		{
			drm_i915_gem_mmap args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;

			mutex_lock(&node->lock);
			render_gem_object* object = render_find_owned_object(node,
				cookie, args.handle);
			if (object == NULL) {
				mutex_unlock(&node->lock);
				return DRM_EINVAL;
			}
			if (args.offset >= object->size) {
				mutex_unlock(&node->lock);
				return DRM_EINVAL;
			}
			if (args.size == 0 || args.size > object->size - args.offset)
				args.size = object->size - args.offset;
			area_id source = object->area;
			object->refcount++;	// pin across the (unlocked) clone
			mutex_unlock(&node->lock);

			// Map the object's area into the *caller's* team and return its
			// user address; the hardware/CPU share the LLC on SKL. Note:
			// clone_area() always clones into the kernel team, so the
			// team-taking vm_clone_area(B_CURRENT_TEAM) is required (same
			// pattern as the framebuffer/vesa drivers). The extra reference
			// keeps the source area id valid until the clone is made.
			void* address = NULL;
			area_id clone = vm_clone_area(B_CURRENT_TEAM,
				"intel render mmap", &address, B_ANY_ADDRESS,
				B_READ_AREA | B_WRITE_AREA, 0, source, true);

			mutex_lock(&node->lock);
			if (--object->refcount <= 0)
				node->DestroyObjectLocked(object);
			mutex_unlock(&node->lock);
			if (clone < B_OK)
				return clone;

			args.addr_ptr = (uint64)(addr_t)address;
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK) {
				// the address never reached userspace: drop the clone
				// (delete_area() would target the kernel team)
				vm_delete_area(team_get_current_team_id(), clone, true);
				return B_BAD_ADDRESS;
			}
			return B_OK;
		}

		case DRM_IOCTL_I915_GEM_MMAP_GTT:
		case DRM_IOCTL_I915_GEM_MMAP_OFFSET:
			// no GTT mmap on Haiku; MMAP_GTT_VERSION=0 makes iris use
			// GEM_MMAP instead, so this path should not be reached
			return DRM_ENOSYS;

		case DRM_IOCTL_I915_GEM_SET_DOMAIN:
		{
			drm_i915_gem_set_domain args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			mutex_lock(&node->lock);
			status_t status = render_find_owned_object(node, cookie,
				args.handle) != NULL ? B_OK : DRM_EINVAL;
			mutex_unlock(&node->lock);
			return status;
		}

		case DRM_IOCTL_I915_GEM_SET_CACHING:
		{
			drm_i915_gem_caching args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			// only coherent WB memory exists in v1; accept any request
			// (the objects are always LLC-coherent system memory)
			(void)args;
			return B_OK;
		}

		case DRM_IOCTL_I915_GEM_SET_TILING:
		{
			drm_i915_gem_set_tiling args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			mutex_lock(&node->lock);
			render_gem_object* object = render_find_owned_object(node,
				cookie, args.handle);
			mutex_unlock(&node->lock);
			if (object == NULL)
				return DRM_EINVAL;
			// no tiling in v1: demote every request to NONE
			args.tiling_mode = I915_TILING_NONE;
			args.stride = 0;
			args.swizzle_mode = 0;
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case DRM_IOCTL_I915_GEM_GET_TILING:
		{
			drm_i915_gem_get_tiling args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			mutex_lock(&node->lock);
			render_gem_object* object = render_find_owned_object(node,
				cookie, args.handle);
			mutex_unlock(&node->lock);
			if (object == NULL)
				return DRM_EINVAL;
			args.tiling_mode = I915_TILING_NONE;
			args.swizzle_mode = 0;
			args.phys_swizzle_mode = 0;
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case DRM_IOCTL_I915_GEM_MADVISE:
		{
			drm_i915_gem_madvise args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			mutex_lock(&node->lock);
			render_gem_object* object = render_find_owned_object(node,
				cookie, args.handle);
			mutex_unlock(&node->lock);
			if (object == NULL)
				return DRM_EINVAL;
			// nothing to reclaim: the object is always retained
			args.retained = 1;
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case DRM_IOCTL_I915_GEM_GET_APERTURE:
		{
			drm_i915_gem_get_aperture args;
			mutex_lock(&node->lock);
			args.aper_size = node->region_end;
			args.aper_available_size = node->region_end
				- node->kernel_zone_end;
			mutex_unlock(&node->lock);
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case DRM_IOCTL_I915_GEM_BUSY:
		{
			drm_i915_gem_busy args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			mutex_lock(&node->lock);
			render_gem_object* object = render_find_owned_object(node,
				cookie, args.handle);
			mutex_unlock(&node->lock);
			if (object == NULL)
				return DRM_EINVAL;
			args.busy = 0;
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case DRM_IOCTL_I915_GEM_WAIT:
		{
			drm_i915_gem_wait args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			mutex_lock(&node->lock);
			render_gem_object* object = render_find_owned_object(node,
				cookie, args.bo_handle);
			mutex_unlock(&node->lock);
			if (object == NULL)
				return DRM_EINVAL;
			// EXECBUFFER2 completes under node->lock before returning.
			// Taking that lock above also drains any concurrent submission.
			args.timeout_ns = 0;
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case DRM_IOCTL_I915_GET_RESET_STATS:
		{
			drm_i915_reset_stats args;
			memset(&args, 0, sizeof(args));
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case DRM_IOCTL_I915_GEM_CONTEXT_CREATE:
		{
			drm_i915_gem_context_create args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;

			mutex_lock(&node->lock);
			render_context* context = node->CreateContextLocked(cookie);
			mutex_unlock(&node->lock);
			if (context == NULL)
				return DRM_ENOMEM;

			args.ctx_id = context->id;
			args.pad = 0;
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK) {
				mutex_lock(&node->lock);
				node->DestroyContextObjLocked(context);
				mutex_unlock(&node->lock);
				return B_BAD_ADDRESS;
			}
			return B_OK;
		}

		case DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT:
		{
			drm_i915_gem_context_create_ext args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;

			// Engine-map extensions require one hardware context per logical
			// engine slot, even when the slots use the same physical RCS.
			// Ignoring them aliases render/compute state. Return EINVAL so
			// iris falls back to distinct legacy contexts instead.
			if (args.flags != 0 || args.extensions != 0)
				return DRM_EINVAL;

			mutex_lock(&node->lock);
			render_context* context = node->CreateContextLocked(cookie);
			mutex_unlock(&node->lock);
			if (context == NULL)
				return DRM_ENOMEM;

			args.ctx_id = context->id;
			// Only extension-free contexts are supported.
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK) {
				mutex_lock(&node->lock);
				node->DestroyContextObjLocked(context);
				mutex_unlock(&node->lock);
				return B_BAD_ADDRESS;
			}
			return B_OK;
		}

		case DRM_IOCTL_I915_GEM_CONTEXT_DESTROY:
		{
			drm_i915_gem_context_destroy args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			mutex_lock(&node->lock);
			status_t status = DRM_EINVAL;
			render_context* context
				= node->ContextByIdLocked(cookie, args.ctx_id);
			if (context != NULL) {
				node->DestroyContextObjLocked(context);
				status = B_OK;
			}
			mutex_unlock(&node->lock);
			return status;
		}

		case DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM:
		{
			drm_i915_gem_context_param args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			switch (args.param) {
				case I915_CONTEXT_PARAM_GTT_SIZE:
				{
					/* 'value' is an inline u64 in the i915 uAPI, not a
					   pointer, and iris leaves 'size' as 0 */
					args.value = ((uint64)1 << 48) - B_PAGE_SIZE;
					if (user_memcpy(buffer, &args, sizeof(args)) < B_OK)
						return B_BAD_ADDRESS;
					return B_OK;
				}
				default:
					TRACE("CONTEXT_GETPARAM: unsupported param %d\n",
						(int)args.param);
					return DRM_EINVAL;
			}
		}

		case DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM:
		{
			drm_i915_gem_context_param args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			switch (args.param) {
				case I915_CONTEXT_PARAM_PRIORITY:
				case I915_CONTEXT_PARAM_RECOVERABLE:
				case I915_CONTEXT_PARAM_VM:
				case I915_CONTEXT_PARAM_NO_ERROR_CAPTURE:
					// accepted, no effect on the single-engine v1 driver
					return B_OK;
				default:
					TRACE("CONTEXT_SETPARAM: unsupported param %d\n",
						(int)args.param);
					return DRM_EINVAL;
			}
		}

		case DRM_IOCTL_I915_REG_READ:
			return DRM_EINVAL;

		case DRM_IOCTL_I915_QUERY:
			return render_drm_query(node, cookie, buffer);

		case DRM_IOCTL_I915_GEM_USERPTR:
			return DRM_ENOSYS;

		case DRM_IOCTL_I915_GEM_EXECBUFFER2:
			return render_drm_execbuffer2(node, cookie, buffer);

		case DRM_IOCTL_SYNCOBJ_CREATE:
		{
			drm_syncobj_create args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			uint32 handle = 0;
			mutex_lock(&node->lock);
			status_t status = node->CreateSyncobjLocked(cookie,
				(args.flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0, &handle);
			mutex_unlock(&node->lock);
			if (status != B_OK)
				return status;
			args.handle = handle;
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case DRM_IOCTL_SYNCOBJ_DESTROY:
		{
			drm_syncobj_destroy args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			mutex_lock(&node->lock);
			render_syncobj* syncobj = node->FindSyncobj(args.handle);
			status_t status = DRM_EINVAL;
			if (syncobj != NULL && syncobj->owner == cookie) {
				node->DestroySyncobjLocked(syncobj);
				status = B_OK;
			}
			mutex_unlock(&node->lock);
			return status;
		}

		case DRM_IOCTL_SYNCOBJ_WAIT:
		{
			drm_syncobj_wait args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			if (args.count_handles == 0
				|| args.count_handles > DRM_MAX_EXEC_OBJECTS)
				return DRM_EINVAL;
			uint32 handles[DRM_MAX_EXEC_OBJECTS];
			if (user_memcpy(handles, (void*)(addr_t)args.handles,
					args.count_handles * sizeof(uint32)) < B_OK)
				return B_BAD_ADDRESS;
			status_t status = render_drm_wait_syncobjs(node, cookie,
				handles, args.count_handles,
				(args.flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0,
				args.timeout_nsec);
			return status == B_TIMED_OUT ? DRM_ETIMEDOUT : status;
		}

		case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT:
		{
			drm_syncobj_timeline_wait args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			if (args.count_handles == 0
				|| args.count_handles > DRM_MAX_EXEC_OBJECTS)
				return DRM_EINVAL;
			uint32 handles[DRM_MAX_EXEC_OBJECTS];
			if (user_memcpy(handles, (void*)(addr_t)args.handles,
					args.count_handles * sizeof(uint32)) < B_OK)
				return B_BAD_ADDRESS;
			status_t status = render_drm_wait_syncobjs(node, cookie,
				handles, args.count_handles,
				(args.flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0,
				args.timeout_nsec);
			return status == B_TIMED_OUT ? DRM_ETIMEDOUT : status;
		}

		case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD:
		case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE:
			return DRM_ENOSYS;

		default:
			(void)info;
			ERROR("drm ioctl: unknown op 0x%08" B_PRIx32 "\n", op);
			return DRM_EINVAL;
	}
}


/*!	Bring-up tracing: log every failing DRM ioctl and the successful ones on
	the control paths iris walks during screen and context creation.
	Successful high-frequency ioctls stay quiet to keep the syslog readable. */
static bool
render_trace_drm_op(uint32 op)
{
	switch (op) {
		case DRM_IOCTL_I915_GETPARAM:
		case DRM_IOCTL_I915_GEM_CREATE:
		case DRM_IOCTL_I915_GEM_CREATE_EXT:
		case DRM_IOCTL_I915_GEM_CONTEXT_CREATE:
		case DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT:
		case DRM_IOCTL_I915_GEM_CONTEXT_DESTROY:
		case DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM:
		case DRM_IOCTL_I915_QUERY:
		case DRM_IOCTL_I915_REG_READ:
		case DRM_IOCTL_SYNCOBJ_CREATE:
		case DRM_IOCTL_SYNCOBJ_DESTROY:
			return true;
		default:
			/* bring-up: trace every op so the full iris sequence is
			   visible in the syslog */
			return true;
	}
}


static status_t
render_drm_control(render_open_cookie* cookie, uint32 op, void* buffer)
{
	TRACE("drm ioctl 0x%08" B_PRIx32 " enter\n", op);
	status_t status = render_drm_control_impl(cookie, op, buffer);
	if (status < B_OK) {
		TRACE("drm ioctl 0x%08" B_PRIx32 " FAILED -> 0x%08" B_PRIx32 "\n",
			op, (uint32)status);
	} else if (render_trace_drm_op(op)) {
		TRACE("drm ioctl 0x%08" B_PRIx32 " ok\n", op);
	}
	return status;
}


static status_t
render_control(void* data, uint32 op, void* buffer, size_t /*length*/)
{
	render_open_cookie* cookie = (render_open_cookie*)data;
	if (cookie == NULL || cookie->magic != RENDER_OPEN_COOKIE_MAGIC)
		return B_BAD_VALUE;

	RenderNode* node = cookie->node;

	if (buffer == NULL)
		return B_BAD_VALUE;

	if (DRM_IS_IOCTL(op))
		return render_drm_control(cookie, op, buffer);

	switch (op) {
		case INTEL_RENDER_GET_INFO:
		{
			intel_render_info renderInfo;
			memset(&renderInfo, 0, sizeof(renderInfo));
			mutex_lock(&node->lock);
			renderInfo.chipset = cookie->info->pci->device_id;
			renderInfo.gen = cookie->info->device_type.Generation();
			renderInfo.aperture_base = node->aperture_base;
			renderInfo.aperture_size = node->aperture_size;
			renderInfo.ggtt_entries = node->ggtt_entries;
			renderInfo.eu_total = 0;
				// FUSE2/EU_DISABLE probing is a later phase
			renderInfo.cs_timestamp_freq = 0;
			renderInfo.last_completed = node->last_completed;

			uint32* debug = renderInfo.debug;
			debug[INTEL_RENDER_DEBUG_GGC] = node->ggc;
			debug[INTEL_RENDER_DEBUG_GGTT_SIZE] = (uint32)node->ggtt_size;
			debug[INTEL_RENDER_DEBUG_APERTURE_SIZE]
				= (uint32)node->aperture_size;
			debug[INTEL_RENDER_DEBUG_WATERMARK] = (uint32)node->watermark;
			debug[INTEL_RENDER_DEBUG_FORCEWAKE_GT_ACK]
				= render_read32(*cookie->info, GEN9_FORCEWAKE_GT_ACK);
			debug[INTEL_RENDER_DEBUG_FORCEWAKE_RENDER_ACK]
				= render_read32(*cookie->info, GEN9_FORCEWAKE_RENDER_ACK);
			debug[INTEL_RENDER_DEBUG_GAPS_TSV]
				= render_read32(*cookie->info, GEN9_GAPS_TSV);
			debug[INTEL_RENDER_DEBUG_GFX_MODE]
				= render_read32(*cookie->info, GEN9_GFX_MODE);
			debug[INTEL_RENDER_DEBUG_EXECLIST_STATUS]
				= render_read32(*cookie->info, GEN9_RCS_EXECLIST_STATUS);
			debug[INTEL_RENDER_DEBUG_HWS_PGA]
				= render_read32(*cookie->info, GEN9_RCS_HWS_PGA);
			debug[INTEL_RENDER_DEBUG_GT0_IER]
				= render_read32(*cookie->info, GEN9_GT0_IER);
			debug[INTEL_RENDER_DEBUG_MASTER_INT_CTL]
				= render_read32(*cookie->info, GEN9_MASTER_INT_CTL);
			debug[INTEL_RENDER_DEBUG_LAST_COMPLETED] = node->last_completed;
			debug[INTEL_RENDER_DEBUG_LAST_SUBMITTED] = node->last_submitted;
			debug[INTEL_RENDER_DEBUG_SCRATCH_PHYS]
				= (uint32)node->scratch_phys;
			debug[INTEL_RENDER_DEBUG_REGION_END] = (uint32)node->region_end;
			debug[INTEL_RENDER_DEBUG_RING_HEAD]
				= render_read32(*cookie->info, GEN9_RCS_RING_HEAD);
			debug[INTEL_RENDER_DEBUG_RING_TAIL]
				= render_read32(*cookie->info, GEN9_RCS_RING_TAIL);
			debug[INTEL_RENDER_DEBUG_GT0_IIR]
				= render_read32(*cookie->info, GEN9_GT0_IIR);
			if (node->hwsCpu != NULL) {
				debug[INTEL_RENDER_DEBUG_HWS_SEQNO_LO]
					= *(volatile uint32*)(node->hwsCpu + HWS_SEQNO_OFFSET);
				debug[INTEL_RENDER_DEBUG_HWS_SEQNO_HI]
					= *(volatile uint32*)(node->hwsCpu + HWS_SEQNO_OFFSET + 4);
			}
			debug[INTEL_RENDER_DEBUG_IRQ_COUNT] = node->irqCount;
			debug[INTEL_RENDER_DEBUG_FAULT_RING]
				= render_read32(*cookie->info, GEN9_RING_FAULT_REG);
			debug[INTEL_RENDER_DEBUG_FAULT_GAM]
				= render_read32(*cookie->info, GEN9_GAM_FAULT_REG);
			debug[INTEL_RENDER_DEBUG_FAULT_VA_LO]
				= render_read32(*cookie->info, GEN9_FAULT_TLB_RB_DATA0);
			debug[INTEL_RENDER_DEBUG_FAULT_VA_HI]
				= render_read32(*cookie->info, GEN9_FAULT_TLB_RB_DATA1);
			debug[INTEL_RENDER_DEBUG_EXECLIST_STATUS_HI]
				= render_read32(*cookie->info, GEN9_RCS_EXECLIST_STATUS + 4);
			debug[INTEL_RENDER_DEBUG_MI_MODE]
				= render_read32(*cookie->info, GEN9_RCS_MI_MODE);
			debug[INTEL_RENDER_DEBUG_GT0_IMR]
				= render_read32(*cookie->info, GEN9_GT0_IMR);
			debug[INTEL_RENDER_DEBUG_HWSTAM]
				= render_read32(*cookie->info, GEN9_RCS_HWSTAM);
			if (cookie->context != NULL) {
				debug[INTEL_RENDER_DEBUG_CTX_TAIL]
					= cookie->context->image[7];
				debug[INTEL_RENDER_DEBUG_LAST_DESCRIPTOR]
					= cookie->context->lastDescriptorLo;
			}
			mutex_unlock(&node->lock);

			if (user_memcpy(buffer, &renderInfo, sizeof(renderInfo)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case INTEL_RENDER_GEM_CREATE:
		{
			intel_render_gem_create args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;

			if (args.size == 0 || args.size > MAX_BO_SIZE)
				return B_BAD_VALUE;
			size_t size = (args.size + B_PAGE_SIZE - 1)
				& ~(uint64)(B_PAGE_SIZE - 1);

			mutex_lock(&node->lock);
			render_gem_object* object = NULL;
			status_t status = node->CreateGemLocked(size, cookie, &object);
			mutex_unlock(&node->lock);
			if (status != B_OK)
				return status;

			args.handle = object->handle;
			args.size = size;
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case INTEL_RENDER_GEM_MAP:
		{
			intel_render_gem_map args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;

			mutex_lock(&node->lock);
			render_gem_object* object = render_find_owned_object(node, cookie,
				args.handle);
			args.area_id = object != NULL ? object->area : B_BAD_VALUE;
			mutex_unlock(&node->lock);

			if (object == NULL)
				return B_BAD_VALUE;
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case INTEL_RENDER_GEM_BIND:
		{
			intel_render_gem_bind args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;

			mutex_lock(&node->lock);
			render_gem_object* object = render_find_owned_object(node, cookie,
				args.handle);
			status_t status = object != NULL
				? node->BindObject(object, args.ggtt_offset) : B_BAD_VALUE;
			mutex_unlock(&node->lock);
			return status;
		}

		case INTEL_RENDER_GEM_UNBIND:
		case INTEL_RENDER_GEM_CLOSE:
		{
			intel_render_gem_handle args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;

			mutex_lock(&node->lock);
			render_gem_object* object = render_find_owned_object(node, cookie,
				args.handle);
			status_t status = B_BAD_VALUE;
			if (object != NULL) {
				if (op == INTEL_RENDER_GEM_UNBIND)
					status = node->UnbindObject(object);
				else {
					if (--object->refcount == 0)
						node->DestroyObjectLocked(object);
					status = B_OK;
				}
			}
			mutex_unlock(&node->lock);
			return status;
		}

		case INTEL_RENDER_CONTEXT_CREATE:
		{
			intel_render_context_create args;
			memset(&args, 0, sizeof(args));

			mutex_lock(&node->lock);
			render_context* context = node->EnsureContextLocked(cookie);
			if (context != NULL)
				args.context_id = context->id;
			mutex_unlock(&node->lock);

			if (context == NULL)
				return B_NO_MEMORY;
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case INTEL_RENDER_EXEC:
		{
			intel_render_exec args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;

			mutex_lock(&node->lock);
			render_context* context = node->EnsureContextLocked(cookie);
			status_t status = B_NO_MEMORY;
			if (context != NULL) {
				if (!node->BatchValidLocked(cookie, args.batch_ggtt_addr,
						args.batch_length)) {
					ERROR("EXEC: batch 0x%" B_PRIx64 " + %u not in a bound "
						"object of this client\n", args.batch_ggtt_addr,
						args.batch_length);
					status = B_BAD_VALUE;
				} else {
					status = node->SubmitLocked(context,
						args.batch_ggtt_addr, args.batch_length, false,
						&args.seqno);
				}
			}
			mutex_unlock(&node->lock);

			if (status != B_OK)
				return status;
			if (user_memcpy(buffer, &args, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case INTEL_RENDER_WAIT:
		{
			intel_render_wait args;
			if (user_memcpy(&args, buffer, sizeof(args)) < B_OK)
				return B_BAD_ADDRESS;

			render_context* context = cookie->context;

			bigtime_t deadline = system_time() + args.timeout_us;
			while (true) {
				// completion is authoritative in the HW status page;
				// poll it directly so WAIT works even if the interrupt
				// path is broken (the IRQ handler is an accelerator)
				if (node->hwsCpu != NULL) {
					uint32 completed = *(volatile uint32*)(node->hwsCpu
						+ HWS_SEQNO_OFFSET);
					if (completed > node->last_completed)
						node->last_completed = completed;
				}
				if (node->last_completed >= args.seqno)
					return B_OK;

				bigtime_t now = system_time();
				if (now >= deadline)
					return B_TIMED_OUT;
				if (context == NULL) {
					snooze(1000);
					continue;
				}
				status_t status = acquire_sem_etc(
					node->waitSems[context->semSlot], 1, B_RELATIVE_TIMEOUT,
					deadline - now);
				if (status != B_OK && status != B_TIMED_OUT)
					return status;
			}
		}

		default:
			ERROR("ioctl() unknown message %" B_PRIu32 "\n", op);
			break;
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
render_read(void* /*data*/, off_t /*pos*/, void* /*buffer*/,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
render_write(void* /*data*/, off_t /*pos*/, const void* /*buffer*/,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


device_hooks gRenderDeviceHooks = {
	render_open,
	render_close,
	render_free,
	render_control,
	render_read,
	render_write,
	NULL,
	NULL,
	NULL,
	NULL
};


//	#pragma mark - interrupt handling


extern "C" int32
render_node_interrupt(intel_info* info)
{
	// called from the display driver's gen8 interrupt handler when
	// MASTER_INT_CTL reports a render-pending interrupt; may run before
	// the render node was ever opened
	if (info->device_type.Generation() != 9)
		return B_UNHANDLED_INTERRUPT;

	RenderNode* node = sRenderNodes[info->id];
	if (node == NULL || !node->initialized || node->status != B_OK
		|| !node->interruptsEnabled)
		return B_UNHANDLED_INTERRUPT;

	uint32 iir = render_read32(*info, GEN9_GT0_IIR);
	if ((iir & (GEN9_GT_CS_MI_USER_INTERRUPT
			| GEN9_GT_CS_CTX_SWITCH_INTERRUPT)) == 0)
		return B_UNHANDLED_INTERRUPT;

	node->irqCount++;

	// write back to clear
	render_write32(*info, GEN9_GT0_IIR, iir);

	int32 result = B_HANDLED_INTERRUPT;

	if ((iir & GEN9_GT_CS_MI_USER_INTERRUPT) != 0) {
		// the batch epilog wrote the seqno qword to HWS + 0xC0
		node->last_completed
			= *(volatile uint32*)(node->hwsCpu + HWS_SEQNO_OFFSET);

		// wake all waiting contexts (same pattern as release_vblank_sem).
		// Index the semaphore pool directly (slot i <=> waitSems[i]) and do
		// not dereference contexts[i], which a concurrent destroy may free.
		for (int32 i = 0; i < MAX_CONTEXTS; i++) {
			if (node->waitSems[i] < B_OK)
				continue;
			int32 count;
			if (get_sem_count(node->waitSems[i], &count) == B_OK
					&& count < 0) {
				release_sem_etc(node->waitSems[i], -count,
					B_DO_NOT_RESCHEDULE);
				result = B_INVOKE_SCHEDULER;
			}
		}
	}

	return result;
}


//	#pragma mark - driver glue


void
render_node_uninit_all()
{
	for (int32 i = 0; i < MAX_CARDS; i++) {
		RenderNode* node = sRenderNodes[i];
		if (node == NULL)
			continue;
		sRenderNodes[i] = NULL;

		// release forcewake if the register mapping is still alive
		if (node->forcewake_held && node->info->init_status == B_OK) {
			render_write32(*node->info, GEN9_FORCEWAKE_MEDIA,
				GEN9_FORCEWAKE_RESET);
			render_write32(*node->info, GEN9_FORCEWAKE_RENDER,
				GEN9_FORCEWAKE_RESET);
			render_write32(*node->info, GEN9_FORCEWAKE_GT,
				GEN9_FORCEWAKE_RESET);
		}

		for (int32 j = 0; j < MAX_CONTEXTS; j++) {
			render_context* context = node->contexts[j];
			if (context != NULL) {
				if (context->ppgtt != NULL) {
					context->ppgtt->Destroy();
					delete context->ppgtt;
				}
				delete context;
				node->contexts[j] = NULL;
			}
			if (node->waitSems[j] >= B_OK)
				delete_sem(node->waitSems[j]);
		}

		for (int32 j = 0; j < MAX_HANDLES; j++) {
			if (node->objects[j] != NULL)
				node->DestroyObjectLocked(node->objects[j]);
		}
		for (int32 j = 0; j < MAX_SYNCOBJS; j++) {
			if (node->syncobjs[j] != NULL)
				node->DestroySyncobjLocked(node->syncobjs[j]);
		}

		if (node->ggtt_area >= 0)
			delete_area(node->ggtt_area);
		if (node->scratch_area >= 0)
			delete_area(node->scratch_area);
		mutex_destroy(&node->lock);
		delete node;
	}
}
