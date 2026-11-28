/*
 * Copyright 2026, The RenkuOS authors.
 * Copyright (c) 2023 Imagination Technologies Ltd.
 * SPDX-License-Identifier: GPL-2.0-only OR MIT
 *
 * Port of the page table handling of the Linux drm/imagination driver
 * (pvr_mmu.c) for 4 KB device pages: the raw entry formats and index
 * computations are those of pvr_mmu.c, the mirror tables are simplified
 * (no op context; tables are only freed with the context).
 */


#include "pvr_mmu.h"

#include <stdlib.h>
#include <string.h>

#include "pvr_rogue_mmu_defs.h"


#define LOG(context, format, ...) \
	do { \
		if ((context)->env.log != NULL) \
			(context)->env.log((context)->env.cookie, format, \
				##__VA_ARGS__); \
	} while (false)

/* pvr_mmu.c, PVR_DEVICE_PAGE_SIZE == SZ_4K */
#define ROGUE_MMUCTRL_PAGE_SIZE_X			ROGUE_MMUCTRL_PAGE_SIZE_4KB
#define ROGUE_MMUCTRL_PAGE_X_RANGE_SHIFT	ROGUE_MMUCTRL_PAGE_4KB_RANGE_SHIFT
#define ROGUE_MMUCTRL_PAGE_X_RANGE_CLRMSK	ROGUE_MMUCTRL_PAGE_4KB_RANGE_CLRMSK
#define ROGUE_MMUCTRL_ENTRIES_PT_VALUE_X	ROGUE_MMUCTRL_ENTRIES_PT_VALUE

#define PVR_MMU_BACKING_PAGE_SIZE			SZ_4K

/* PVR_PAGE_TABLE_FIELD_PREP() of pvr_mmu.c, for a 32 or 64 bit entry */
#define FIELD_PREP(type, name, field, value) \
	((((type)(value)) << ROGUE_MMUCTRL_##name##_DATA_##field##_SHIFT) \
		& ~(type)ROGUE_MMUCTRL_##name##_DATA_##field##_CLRMSK)
#define FIELD_GET(name, field, value) \
	(((value) & ~ROGUE_MMUCTRL_##name##_DATA_##field##_CLRMSK) \
		>> ROGUE_MMUCTRL_##name##_DATA_##field##_SHIFT)

static_assert(sizeof(uint32) * 8 == ROGUE_MMUCTRL_ENTRY_SIZE_PC_VALUE,
	"PC entry size");
static_assert(sizeof(uint64) * 8 == ROGUE_MMUCTRL_ENTRY_SIZE_PD_VALUE,
	"PD entry size");
static_assert(sizeof(uint64) * 8 == ROGUE_MMUCTRL_ENTRY_SIZE_PT_VALUE,
	"PT entry size");
static_assert(ROGUE_MMUCTRL_ENTRIES_PC_VALUE * sizeof(uint32)
	== PVR_MMU_BACKING_PAGE_SIZE, "PC is one page");
static_assert(ROGUE_MMUCTRL_ENTRIES_PD_VALUE * sizeof(uint64)
	== PVR_MMU_BACKING_PAGE_SIZE, "PD is one page");
static_assert(ROGUE_MMUCTRL_ENTRIES_PT_VALUE_X * sizeof(uint64)
	<= PVR_MMU_BACKING_PAGE_SIZE, "PT fits in one page");


struct pvr_mmu_table {
	void*				cpu;
	uint64				phys;
	uint32				level;
		// 2: page catalogue, 1: page directory, 0: page table
	uint32				entry_count;
	bool				dirty;
	pvr_mmu_table**		children;
		// level 2 and 1 only
	pvr_mmu_table*		next;
		// all tables, page catalogue first
};

struct pvr_mmu_context {
	pvr_mmu_env			env;
	pvr_mmu_table*		root;
	pvr_mmu_table*		last;
	uint32				table_count;
	uint32				flush_flags;
};


const pvr_heap kPvrHeaps[] = {
	{ "general", ROGUE_GENERAL_HEAP_BASE, ROGUE_GENERAL_HEAP_SIZE },
	{ "pds code data", ROGUE_PDSCODEDATA_HEAP_BASE,
		ROGUE_PDSCODEDATA_HEAP_SIZE },
	{ "usc code", ROGUE_USCCODE_HEAP_BASE, ROGUE_USCCODE_HEAP_SIZE },
	{ "vis test", ROGUE_VISTEST_HEAP_BASE, ROGUE_VISTEST_HEAP_SIZE },
	{ "transfer frag", ROGUE_TRANSFER_FRAG_HEAP_BASE,
		ROGUE_TRANSFER_FRAG_HEAP_SIZE },
};
const uint32 kPvrHeapCount = sizeof(kPvrHeaps) / sizeof(kPvrHeaps[0]);


const pvr_heap*
pvr_find_heap_containing(uint64 start, uint64 size)
{
	// pvr_find_heap_containing(), pvr_heap_contains_range() (pvr_vm.c)
	uint64 end;
	if (size == 0 || __builtin_add_overflow(start, size - 1, &end))
		return NULL;

	for (uint32 i = 0; i < kPvrHeapCount; i++) {
		if (kPvrHeaps[i].base <= start
			&& end < kPvrHeaps[i].base + kPvrHeaps[i].size)
			return &kPvrHeaps[i];
	}
	return NULL;
}


//	#pragma mark - raw entries (pvr_mmu.c)


/* pvr_page_table_l2_entry_raw_set() */
static uint32
l2_entry(uint64 childTablePhys)
{
	childTablePhys >>= ROGUE_MMUCTRL_PC_DATA_PD_BASE_ALIGNSHIFT;

	return FIELD_PREP(uint32, PC, VALID, true)
		| FIELD_PREP(uint32, PC, ENTRY_PENDING, false)
		| FIELD_PREP(uint32, PC, PD_BASE, childTablePhys);
}


/* pvr_page_table_l1_entry_raw_set() */
static uint64
l1_entry(uint64 childTablePhys)
{
	return FIELD_PREP(uint64, PD, VALID, true)
		| FIELD_PREP(uint64, PD, ENTRY_PENDING, false)
		| FIELD_PREP(uint64, PD, PAGE_SIZE, ROGUE_MMUCTRL_PAGE_SIZE_X)
		| (childTablePhys & ~ROGUE_MMUCTRL_PT_BASE_4KB_RANGE_CLRMSK);
}


/* pvr_page_flags_raw_create() */
static uint64
page_flags(bool readOnly, bool cacheCoherent, bool slcBypass,
	bool pmFwProtect)
{
	return FIELD_PREP(uint64, PT, READ_ONLY, readOnly)
		| FIELD_PREP(uint64, PT, CC, cacheCoherent)
		| FIELD_PREP(uint64, PT, SLC_BYPASS_CTRL, slcBypass)
		| FIELD_PREP(uint64, PT, PM_META_PROTECT, pmFwProtect);
}


/* pvr_page_table_l0_entry_raw_set() */
static uint64
l0_entry(uint64 phys, uint64 flags)
{
	return FIELD_PREP(uint64, PT, VALID, true)
		| FIELD_PREP(uint64, PT, ENTRY_PENDING, false)
		| (phys & ~ROGUE_MMUCTRL_PAGE_X_RANGE_CLRMSK)
		| flags;
}


/* pvr_page_table_l2_idx(), pvr_page_table_l1_idx(), pvr_page_table_l0_idx() */
static uint32
l2_index(uint64 deviceAddr)
{
	return (deviceAddr & ~ROGUE_MMUCTRL_VADDR_PC_INDEX_CLRMSK)
		>> ROGUE_MMUCTRL_VADDR_PC_INDEX_SHIFT;
}


static uint32
l1_index(uint64 deviceAddr)
{
	return (deviceAddr & ~ROGUE_MMUCTRL_VADDR_PD_INDEX_CLRMSK)
		>> ROGUE_MMUCTRL_VADDR_PD_INDEX_SHIFT;
}


static uint32
l0_index(uint64 deviceAddr)
{
	return (deviceAddr & ~ROGUE_MMUCTRL_VADDR_PT_INDEX_CLRMSK)
		>> ROGUE_MMUCTRL_PAGE_X_RANGE_SHIFT;
}


//	#pragma mark - tables


static uint64
phys_limit(const pvr_mmu_context* context)
{
	uint32 width = context->env.phys_bus_width;
	if (width == 0 || width > PVR_PAGE_TABLE_ADDR_BITS)
		width = PVR_PAGE_TABLE_ADDR_BITS;
	return 1ULL << width;
}


/* pvr_mmu_backing_page_init() + pvr_page_table_l*_init() */
static pvr_mmu_table*
table_create(pvr_mmu_context* context, uint32 level)
{
	pvr_mmu_table* table = (pvr_mmu_table*)calloc(1, sizeof(pvr_mmu_table));
	if (table == NULL)
		return NULL;

	uint32 childCount = level == 2 ? ROGUE_MMUCTRL_ENTRIES_PC_VALUE
		: level == 1 ? ROGUE_MMUCTRL_ENTRIES_PD_VALUE : 0;
	if (childCount != 0) {
		table->children = (pvr_mmu_table**)calloc(childCount,
			sizeof(pvr_mmu_table*));
		if (table->children == NULL) {
			free(table);
			return NULL;
		}
	}

	status_t status = context->env.gpu_alloc(context->env.cookie,
		PVR_MMU_BACKING_PAGE_SIZE, PVR_MMU_BACKING_PAGE_SIZE, &table->cpu,
		&table->phys);
	if (status != B_OK || table->cpu == NULL
		|| (table->phys & (PVR_MMU_BACKING_PAGE_SIZE - 1)) != 0
		|| table->phys + PVR_MMU_BACKING_PAGE_SIZE > phys_limit(context)) {
		LOG(context, "pvr_mmu: cannot get a level %" B_PRIu32 " table "
			"(%#" B_PRIx32 ", phys %#" B_PRIx64 ")\n", level, (uint32)status,
			table->phys);
		if (status == B_OK && context->env.gpu_free != NULL) {
			context->env.gpu_free(context->env.cookie, table->cpu,
				table->phys, PVR_MMU_BACKING_PAGE_SIZE);
		}
		free(table->children);
		free(table);
		return NULL;
	}

	// alloc_page(__GFP_ZERO)
	memset(table->cpu, 0, PVR_MMU_BACKING_PAGE_SIZE);
	table->level = level;
	table->dirty = true;

	if (context->last != NULL)
		context->last->next = table;
	else
		context->root = table;
	context->last = table;
	context->table_count++;

	return table;
}


/*!	pvr_mmu_backing_page_sync() of every modified table, lowest level first
	(pvr_mmu_op_context_sync_manual()), accumulating the flush flags.
*/
static void
tables_sync(pvr_mmu_context* context)
{
	static const uint32 kFlags[3] = {
		PVR_MMU_SYNC_LEVEL_0_FLAGS,
		PVR_MMU_SYNC_LEVEL_1_FLAGS,
		PVR_MMU_SYNC_LEVEL_2_FLAGS
	};

	__asm__ __volatile__("fence rw, rw" ::: "memory");
	for (uint32 level = 0; level <= 2; level++) {
		for (pvr_mmu_table* table = context->root; table != NULL;
				table = table->next) {
			if (!table->dirty || table->level != level)
				continue;
			if (context->env.sync != NULL) {
				context->env.sync(context->env.cookie, table->phys,
					PVR_MMU_BACKING_PAGE_SIZE);
			}
			table->dirty = false;
			context->flush_flags |= kFlags[level];
		}
	}
}


/*!	pvr_page_table_l1_get_or_insert() / pvr_page_table_l0_get_or_insert()
	for one address: returns the page table, creating the directory and the
	page table if insert is true.
*/
static pvr_mmu_table*
page_table_get(pvr_mmu_context* context, uint64 deviceAddr, bool insert)
{
	pvr_mmu_table* l2 = context->root;
	uint32 index2 = l2_index(deviceAddr);
	pvr_mmu_table* l1 = l2->children[index2];
	if (l1 == NULL) {
		if (!insert)
			return NULL;
		l1 = table_create(context, 1);
		if (l1 == NULL)
			return NULL;
		// pvr_page_table_l2_insert()
		l2->children[index2] = l1;
		((volatile uint32*)l2->cpu)[index2] = l2_entry(l1->phys);
		l2->entry_count++;
		l2->dirty = true;
	}

	uint32 index1 = l1_index(deviceAddr);
	pvr_mmu_table* l0 = l1->children[index1];
	if (l0 == NULL) {
		if (!insert)
			return NULL;
		l0 = table_create(context, 0);
		if (l0 == NULL)
			return NULL;
		// pvr_page_table_l1_insert()
		l1->children[index1] = l0;
		((volatile uint64*)l1->cpu)[index1] = l1_entry(l0->phys);
		l1->entry_count++;
		l1->dirty = true;
	}

	return l0;
}


static void
page_clear(pvr_mmu_context* context, uint64 deviceAddr)
{
	pvr_mmu_table* l0 = page_table_get(context, deviceAddr, false);
	if (l0 == NULL)
		return;

	// pvr_page_destroy(): only if the page exists
	volatile uint64* entry = (volatile uint64*)l0->cpu + l0_index(deviceAddr);
	if (FIELD_GET(PT, VALID, *entry) == 0)
		return;

	// pvr_page_table_l0_entry_raw_clear()
	*entry = 0;
	l0->entry_count--;
	l0->dirty = true;
}


//	#pragma mark - public


status_t
pvr_mmu_context_create(const pvr_mmu_env& env, pvr_mmu_context** _context)
{
	if (env.gpu_alloc == NULL || _context == NULL)
		return B_BAD_VALUE;

	pvr_mmu_context* context = (pvr_mmu_context*)calloc(1,
		sizeof(pvr_mmu_context));
	if (context == NULL)
		return B_NO_MEMORY;
	context->env = env;

	// pvr_mmu_context_create() -> pvr_page_table_l2_init()
	if (table_create(context, 2) == NULL) {
		free(context);
		return B_NO_MEMORY;
	}
	tables_sync(context);

	LOG(context, "pvr_mmu: page catalogue at phys %#" B_PRIx64 "\n",
		context->root->phys);

	*_context = context;
	return B_OK;
}


void
pvr_mmu_context_destroy(pvr_mmu_context* context)
{
	if (context == NULL)
		return;

	pvr_mmu_table* table = context->root;
	while (table != NULL) {
		pvr_mmu_table* next = table->next;
		if (context->env.gpu_free != NULL) {
			context->env.gpu_free(context->env.cookie, table->cpu, table->phys,
				PVR_MMU_BACKING_PAGE_SIZE);
		}
		free(table->children);
		free(table);
		table = next;
	}
	free(context);
}


uint64
pvr_mmu_get_root_table_phys(const pvr_mmu_context* context)
{
	return context->root->phys;
}


status_t
pvr_mmu_map(pvr_mmu_context* context, uint64 deviceAddr, uint64 phys,
	uint64 size, uint32 flags)
{
	if (size == 0)
		return B_OK;

	// pvr_mmu_map(): (sgt_offset | size) & ~PVR_DEVICE_PAGE_MASK, and the
	// address checks of pvr_vm_map() / pvr_find_heap_containing()
	const uint64 pageMask = PVR_DEVICE_PAGE_SIZE - 1;
	if (((deviceAddr | phys | size) & pageMask) != 0
		|| deviceAddr + size > PVR_PAGE_TABLE_ADDR_SPACE_SIZE
		|| deviceAddr + size < deviceAddr
		|| phys + size > phys_limit(context)) {
		LOG(context, "pvr_mmu: bad mapping %#" B_PRIx64 " -> %#" B_PRIx64
			" (%#" B_PRIx64 " bytes)\n", deviceAddr, phys, size);
		return B_BAD_VALUE;
	}
	if ((flags & PVR_MMU_MAP_ANY_ADDRESS) == 0
		&& pvr_find_heap_containing(deviceAddr, size) == NULL) {
		LOG(context, "pvr_mmu: %#" B_PRIx64 " (%#" B_PRIx64 " bytes) is in "
			"no heap\n", deviceAddr, size);
		return B_BAD_VALUE;
	}

	// pvr_mmu_map(): read_only and cache_coherent are false in Linux
	uint64 pteFlags = page_flags((flags & PVR_MMU_MAP_READ_ONLY) != 0, false,
		(flags & PVR_MMU_MAP_BYPASS_DEVICE_CACHE) != 0,
		(flags & PVR_MMU_MAP_PM_FW_PROTECT) != 0);

	status_t status = B_OK;
	uint64 mapped = 0;
	for (; mapped < size; mapped += PVR_DEVICE_PAGE_SIZE) {
		uint64 address = deviceAddr + mapped;
		pvr_mmu_table* l0 = page_table_get(context, address, true);
		if (l0 == NULL) {
			status = B_NO_MEMORY;
			break;
		}

		// pvr_page_create(): do not replace an existing page (-EEXIST)
		volatile uint64* entry = (volatile uint64*)l0->cpu
			+ l0_index(address);
		if (FIELD_GET(PT, VALID, *entry) != 0) {
			status = B_BUSY;
			break;
		}

		// pvr_page_table_l0_insert()
		*entry = l0_entry(phys + mapped, pteFlags);
		l0->entry_count++;
		l0->dirty = true;
	}

	if (status != B_OK) {
		LOG(context, "pvr_mmu: mapping %#" B_PRIx64 " failed at +%#" B_PRIx64
			": %#" B_PRIx32 "\n", deviceAddr, mapped, (uint32)status);
		// pvr_mmu_map(): unmap what was mapped
		for (uint64 offset = 0; offset < mapped;
				offset += PVR_DEVICE_PAGE_SIZE)
			page_clear(context, deviceAddr + offset);
	}

	tables_sync(context);
	return status;
}


status_t
pvr_mmu_unmap(pvr_mmu_context* context, uint64 deviceAddr, uint64 size)
{
	const uint64 pageMask = PVR_DEVICE_PAGE_SIZE - 1;
	if (((deviceAddr | size) & pageMask) != 0
		|| deviceAddr + size > PVR_PAGE_TABLE_ADDR_SPACE_SIZE
		|| deviceAddr + size < deviceAddr)
		return B_BAD_VALUE;

	// pvr_mmu_unmap() -> pvr_mmu_op_context_unmap_curr_page()
	for (uint64 offset = 0; offset < size; offset += PVR_DEVICE_PAGE_SIZE)
		page_clear(context, deviceAddr + offset);

	tables_sync(context);
	return B_OK;
}


bool
pvr_mmu_translate(const pvr_mmu_context* context, uint64 deviceAddr,
	uint64* _phys, uint64* _pte)
{
	if (deviceAddr >= PVR_PAGE_TABLE_ADDR_SPACE_SIZE)
		return false;

	// decode the raw entries, not the mirror pointers
	uint32 pc = ((const volatile uint32*)context->root->cpu)
		[l2_index(deviceAddr)];
	if (FIELD_GET(PC, VALID, pc) == 0)
		return false;
	uint64 pdPhys = (uint64)FIELD_GET(PC, PD_BASE, pc)
		<< ROGUE_MMUCTRL_PC_DATA_PD_BASE_ALIGNSHIFT;

	pvr_mmu_table* l1 = context->root->children[l2_index(deviceAddr)];
	if (l1 == NULL || l1->phys != pdPhys)
		return false;
	uint64 pd = ((const volatile uint64*)l1->cpu)[l1_index(deviceAddr)];
	if (FIELD_GET(PD, VALID, pd) == 0
		|| FIELD_GET(PD, PAGE_SIZE, pd) != ROGUE_MMUCTRL_PAGE_SIZE_X)
		return false;
	uint64 ptPhys = pd & ~ROGUE_MMUCTRL_PT_BASE_4KB_RANGE_CLRMSK;

	pvr_mmu_table* l0 = l1->children[l1_index(deviceAddr)];
	if (l0 == NULL || l0->phys != ptPhys)
		return false;
	uint64 pte = ((const volatile uint64*)l0->cpu)[l0_index(deviceAddr)];
	if (FIELD_GET(PT, VALID, pte) == 0)
		return false;

	if (_phys != NULL) {
		*_phys = (pte & ~ROGUE_MMUCTRL_PAGE_X_RANGE_CLRMSK)
			| (deviceAddr & (PVR_DEVICE_PAGE_SIZE - 1));
	}
	if (_pte != NULL)
		*_pte = pte;
	return true;
}


uint32
pvr_mmu_take_flush_flags(pvr_mmu_context* context)
{
	uint32 flags = context->flush_flags;
	context->flush_flags = 0;
	return flags;
}


void
pvr_mmu_flush_request_all(pvr_mmu_context* context)
{
	context->flush_flags |= PVR_MMU_SYNC_LEVEL_2_FLAGS;
}


uint32
pvr_mmu_table_count(const pvr_mmu_context* context)
{
	return context->table_count;
}


bool
pvr_mmu_get_table(const pvr_mmu_context* context, uint32 index, void** _cpu,
	uint64* _phys, uint32* _level)
{
	pvr_mmu_table* table = context->root;
	for (; table != NULL && index > 0; index--)
		table = table->next;
	if (table == NULL)
		return false;

	if (_cpu != NULL)
		*_cpu = table->cpu;
	if (_phys != NULL)
		*_phys = table->phys;
	if (_level != NULL)
		*_level = table->level;
	return true;
}
