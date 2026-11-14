/*
 * Copyright 2022 Haiku, Inc. All rights reserved.
 * Released under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


/*!	Memory setup for starting a 64-bit kernel from the 32-bit EFI loader.

	The loader itself keeps running with the 32-bit address layout. The page
	tables generated here map the same kernel regions to the top of the 64-bit
	address space (KERNEL_LOAD_BASE_64_BIT), plus a physical map area and an
	identity mapping that is needed while switching to long mode.
*/


#include <algorithm>

#include <kernel.h>
#include <arch_kernel.h>
#include <arch/x86/descriptors.h>
#include <boot/platform.h>
#include <boot/stage2.h>

#include "efi_platform.h"
#include "generic_mmu.h"
#include "mmu.h"


//#define TRACE_LONG_MMU
#ifdef TRACE_LONG_MMU
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif


// Ignore memory below 1M and above 512GB (a single PDPT is used).
#define PHYSICAL_MEMORY_LOW		0x00100000
#define PHYSICAL_MEMORY_HIGH	0x8000000000ull


struct gdt_idt_descr {
	uint16_t	limit;
	uint32_t	base;
} _PACKED;


// The 64-bit kernel replaces this GDT as soon as it runs, so only the segments
// that are needed to enter it exist. Their numbers are the same in the 32-bit
// and the 64-bit layout, entry_long.S depends on that.
static const int kLongKernelCodeSegment = 1;
static const int kLongKernelDataSegment = 2;
static const int kLongBootGDTSegmentCount = 3;

static uint64 *sLongBootGDT = NULL;


/*!	Builds a long mode code/data segment descriptor, the same as
	set_segment_descriptor() from arch/x86/64/descriptors.h. That header can
	not be used here, as it conflicts with the 32-bit one.
*/
static uint64
make_long_segment_descriptor(uint8 type, uint8 dpl)
{
	// The CPU ignores base and limit of code/data segments in 64-bit mode.
	uint64 descriptor = 0xffff;			// limit 15:0
	descriptor |= (uint64)type << 40;
	descriptor |= (uint64)DT_CODE_DATA_SEGMENT << 44;
	descriptor |= (uint64)dpl << 45;
	descriptor |= (uint64)1 << 47;		// present
	descriptor |= (uint64)0xf << 48;	// limit 19:16
	if ((type & DT_CODE_EXECUTE_ONLY) != 0)
		descriptor |= (uint64)1 << 53;	// long mode, for code segments only
	descriptor |= (uint64)1 << 55;		// granularity
	return descriptor;
}


void
arch_mmu_init_long_gdt(gdt_idt_descr &longBootGDTDescriptor)
{
	STATIC_ASSERT(KERNEL_CODE_SEGMENT == kLongKernelCodeSegment
		&& KERNEL_DATA_SEGMENT == kLongKernelDataSegment);

	uint64 *longBootGDT = NULL;

	if (platform_allocate_region((void **)&longBootGDT,
			kLongBootGDTSegmentCount * sizeof(uint64), 0) != B_OK) {
		panic("Failed to allocate GDT.\n");
	}

	sLongBootGDT = longBootGDT;

	longBootGDT[0] = 0;
	longBootGDT[kLongKernelCodeSegment]
		= make_long_segment_descriptor(DT_CODE_EXECUTE_ONLY, DPL_KERNEL);
	longBootGDT[kLongKernelDataSegment]
		= make_long_segment_descriptor(DT_DATA_WRITEABLE, DPL_KERNEL);

	// The GDT is loaded once the long mode page tables are active, which map
	// it at its physical address.
	longBootGDTDescriptor.limit = kLongBootGDTSegmentCount * sizeof(uint64) - 1;
	longBootGDTDescriptor.base = (uint32_t)(addr_t)longBootGDT;

	TRACE("gdt base = 0x%08" PRIx32 ", limit = %" PRIu16 "\n",
		longBootGDTDescriptor.base, longBootGDTDescriptor.limit);
}


/*!	Returns the GDT of arch_mmu_init_long_gdt(), for the application processors.
	It is in memory that is identity mapped.
*/
void
arch_mmu_get_long_gdt(uint64 *base, uint16 *limit)
{
	*base = (uint64)(addr_t)sLongBootGDT;
	*limit = kLongBootGDTSegmentCount * sizeof(uint64) - 1;
}


uint32_t
arch_long_mmu_generate_post_efi_page_tables(size_t memoryMapSize,
	efi_memory_descriptor *memoryMap, size_t descriptorSize,
	uint32_t descriptorVersion)
{
	build_physical_memory_list(memoryMapSize, memoryMap,
		descriptorSize, descriptorVersion,
		PHYSICAL_MEMORY_LOW, PHYSICAL_MEMORY_HIGH);

	// Generate page tables, matching bios_ia32/long.cpp and the 64-bit EFI
	// loader (arch/x86_64/arch_mmu.cpp).
	uint64_t *pdpt;
	uint64_t *pageDir;
	uint64_t *pageTable;

	// Allocate the top level PML4. It must be below 4GB, as phys_pgdir is a
	// uint32 and it is loaded into CR3 from 32-bit code. All memory the 32-bit
	// loader can address is, so the pointer is known to fit.
	uint64_t *pml4 = NULL;
	if (platform_allocate_region((void**)&pml4, B_PAGE_SIZE, 0) != B_OK)
		panic("Failed to allocate PML4.");
	memset(pml4, 0, B_PAGE_SIZE);

	// Store PML4 physical and virtual address in kernel args.
	gKernelArgs.arch_args.phys_pgdir = (uint32_t)(addr_t)pml4;
	addr_t virtualPML4;
	if (platform_bootloader_address_to_kernel_address(pml4, &virtualPML4) != B_OK)
		panic("Failed to find the kernel address of the PML4.");
	gKernelArgs.arch_args.vir_pgdir = (uint64)virtualPML4
		+ KERNEL_FIXUP_FOR_LONG_MODE;

	// Store the virtual memory usage information.
	gKernelArgs.virtual_allocated_range[0].start = KERNEL_LOAD_BASE_64_BIT;
	gKernelArgs.virtual_allocated_range[0].size
		= get_current_virtual_address() - KERNEL_LOAD_BASE;
	gKernelArgs.num_virtual_allocated_ranges = 1;
	gKernelArgs.arch_args.virtual_end = ROUNDUP(KERNEL_LOAD_BASE_64_BIT
		+ gKernelArgs.virtual_allocated_range[0].size, 0x200000);

	// Find the highest physical memory address. We map all physical memory
	// into the kernel address space, so we want to make sure we map everything
	// we have available.
	uint64 maxAddress = 0;
	for (size_t i = 0; i < memoryMapSize / descriptorSize; ++i) {
		efi_memory_descriptor *entry
			= (efi_memory_descriptor *)((addr_t)memoryMap + i * descriptorSize);
		switch (entry->Type) {
			case EfiLoaderCode:
			case EfiLoaderData:
			case EfiBootServicesCode:
			case EfiBootServicesData:
			case EfiConventionalMemory:
			case EfiRuntimeServicesCode:
			case EfiRuntimeServicesData:
			case EfiPersistentMemory:
			case EfiACPIReclaimMemory:
			case EfiACPIMemoryNVS:
				maxAddress = std::max(maxAddress,
					entry->PhysicalStart + entry->NumberOfPages * B_PAGE_SIZE);
				break;
			default:
				break;
		}
	}

	// Want to map at least 4GB, there may be stuff other than usable RAM that
	// could be in the first 4GB of physical address space.
	maxAddress = std::max(maxAddress, (uint64)0x100000000ll);
	maxAddress = ROUNDUP(maxAddress, 0x40000000);

	// Currently only use 1 PDPT (512GB). This will need to change if someone
	// wants to use Haiku on a box with more than 512GB of RAM but that's
	// probably not going to happen any time soon.
	if (maxAddress / 0x40000000 > 512)
		panic("Can't currently support more than 512GB of RAM!");

	// Create page tables for the physical map area. Also map this PDPT
	// temporarily at the bottom of the address space so that we are identity
	// mapped.

	pdpt = (uint64*)mmu_allocate_page();
	memset(pdpt, 0, B_PAGE_SIZE);
	pml4[510] = (addr_t)pdpt | kTableMappingFlags;
	pml4[0] = (addr_t)pdpt | kTableMappingFlags;

	for (uint64 i = 0; i < maxAddress; i += 0x40000000) {
		pageDir = (uint64*)mmu_allocate_page();
		memset(pageDir, 0, B_PAGE_SIZE);
		pdpt[i / 0x40000000] = (addr_t)pageDir | kTableMappingFlags;

		for (uint64 j = 0; j < 0x40000000; j += 0x200000)
			pageDir[j / 0x200000] = (i + j) | kLargePageMappingFlags;
	}

	// Allocate tables for the kernel mappings.

	pdpt = (uint64*)mmu_allocate_page();
	memset(pdpt, 0, B_PAGE_SIZE);
	pml4[511] = (addr_t)pdpt | kTableMappingFlags;

	pageDir = (uint64*)mmu_allocate_page();
	memset(pageDir, 0, B_PAGE_SIZE);
	pdpt[510] = (addr_t)pageDir | kTableMappingFlags;

	// We can now allocate page tables and duplicate the mappings across from
	// the 32-bit address space to them.
	pageTable = NULL; // shush, compiler.
	for (uint32 i = 0; i < gKernelArgs.virtual_allocated_range[0].size
			/ B_PAGE_SIZE; i++) {
		if ((i % 512) == 0) {
			pageTable = (uint64*)mmu_allocate_page();
			memset(pageTable, 0, B_PAGE_SIZE);
			pageDir[i / 512] = (addr_t)pageTable | kTableMappingFlags;
		}

		// Get the physical address to map.
		void *phys;
		if (platform_kernel_address_to_bootloader_address(
				KERNEL_LOAD_BASE + (i * B_PAGE_SIZE), &phys) != B_OK) {
			continue;
		}

		pageTable[i % 512] = (addr_t)phys | kPageMappingFlags;
	}

	return (uint32_t)(addr_t)pml4;
}
