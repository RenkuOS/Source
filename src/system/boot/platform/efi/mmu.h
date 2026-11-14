/*
 * Copyright 2014, Henry Harrington, henry.harrington@gmail.com.
 * Copyright 2019-2022, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MMU_H
#define MMU_H


#ifndef _ASSEMBLER


#include "efi_platform.h"

#include <arch_kernel.h>
#include <util/FixedWidthPointer.h>


#ifdef __cplusplus
extern "C" {
#endif

static const uint32 kDefaultPageFlags = 0x3;
    // present, R/W
static const uint64 kTableMappingFlags = 0x7;
    // present, R/W, user
static const uint64 kLargePageMappingFlags = 0x183;
    // present, R/W, user, global, large
static const uint64 kPageMappingFlags = 0x103;
    // present, R/W, user, global


extern addr_t get_next_virtual_address(size_t size);
extern addr_t get_current_virtual_address();

extern void mmu_init();

extern phys_addr_t mmu_allocate_page();

bool mmu_next_region(void** cookie, addr_t* vaddr, phys_addr_t* paddr, size_t* size);

extern addr_t mmu_map_physical_memory(addr_t physicalAddress, size_t size,
	uint32 flags);

extern status_t platform_assign_kernel_address_for_region(void *address, addr_t assign);

extern status_t platform_kernel_address_to_bootloader_address(addr_t address,
	void **_result);

extern status_t platform_bootloader_address_to_kernel_address(void *address,
	addr_t *_result);

extern status_t platform_allocate_region_below(void **_address,
	size_t size, phys_addr_t maxAddress);

extern status_t platform_add_allocated_region(void *address, size_t size);

#ifdef __cplusplus
}
#endif


/*! Convert a 32-bit address to a 64-bit address. */
inline uint64
fix_address(addr_t address)
{
	addr_t result;
	if (platform_bootloader_address_to_kernel_address((void *)address, &result)
		!= B_OK) {
		return address;
	}

#ifdef KERNEL_FIXUP_FOR_LONG_MODE
	// A 32-bit loader starting a 64-bit kernel: the kernel sees the 32-bit
	// kernel addresses at the top of its address space.
	if (gIsHybridPlatform)
		return (uint64)result + KERNEL_FIXUP_FOR_LONG_MODE;
#endif

	return result;
}


template<typename Type>
inline void
fix_address(FixedWidthPointer<Type>& p)
{
	if (p != NULL)
		p.SetTo(fix_address(p.Get()));
}


#endif	// !_ASSEMBLER

#endif	/* MMU_H */
