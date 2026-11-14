/*
 * Copyright 2026, Bryan Irvine
 * All rights reserved. Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */
#ifndef _USB_DMA_H
#define _USB_DMA_H


#include <KernelExport.h>
#include <vm/vm.h>


/*!	Some platforms (the Raspberry Pi 4 for one) have USB controllers that do
	not snoop the CPU caches. Memory shared with the controller is therefore
	mapped uncached (normal, non-cacheable) there. Any lines still in the cache
	from the cacheable mapping are written back and dropped first.
*/
static inline void
usb_prepare_dma_area(area_id area, void* address, size_t size)
{
#ifdef __aarch64__
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	size_t line = 4 << ((ctr >> 16) & 0xf);
	for (addr_t p = (addr_t)address; p < (addr_t)address + size; p += line)
		asm volatile("dc civac, %0" : : "r"(p) : "memory");
	asm volatile("dsb sy" : : : "memory");

	physical_entry entry;
	if (get_memory_map(address, B_PAGE_SIZE, &entry, 1) == B_OK) {
		vm_set_area_memory_type(area, entry.address,
			B_WRITE_COMBINING_MEMORY);
	}
#endif
}


#endif // _USB_DMA_H
