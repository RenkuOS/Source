/*
 * Copyright 2007, François Revol, revol@free.fr.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2003-2005, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#include <KernelExport.h>

#include <arch/cpu.h>
#include <boot/kernel_args.h>
#include <vm/vm.h>


status_t
arch_cpu_preboot_init_percpu(kernel_args *args, int curr_cpu)
{
	return B_OK;
}


status_t
arch_cpu_init_percpu(kernel_args *args, int curr_cpu)
{
	if (curr_cpu != 0)
		panic("No SMP support on ARM yet!\n");

	return 0;
}


status_t
arch_cpu_init(kernel_args *args)
{
	return B_OK;
}


status_t
arch_cpu_init_post_vm(kernel_args *args)
{
	return B_OK;
}


status_t
arch_cpu_init_post_modules(kernel_args *args)
{
	return B_OK;
}


status_t
arch_cpu_shutdown(bool reboot)
{
	while(1)
		arch_cpu_idle();

	// never reached
	return B_ERROR;
}


void
arch_cpu_sync_icache(void *address, size_t len)
{
	uint32 Rd = 0;
	asm volatile ("mcr p15, 0, %[c7format], c7, c5, 0"
		: : [c7format] "r" (Rd) );
}


/*!	Makes the code in a physical page visible to the instruction fetches of
	every CPU. The page may just have been written through a data cache (by
	the file cache when a file is written and then mapped, a loader or a JIT),
	and the instruction caches may still hold what was at this address before.
	The page is cleaned to the point of unification by address, which on a
	multiprocessor is carried out by the other CPUs too, then the instruction
	caches and branch predictors of all CPUs are invalidated.
	The calling thread must be pinned to its CPU.
*/
void
arm_sync_icache_page(phys_addr_t physicalAddress)
{
	addr_t virtualAddress;
	void* handle;
	if (vm_get_physical_page_current_cpu(physicalAddress, &virtualAddress,
			&handle) != B_OK) {
		panic("arm_sync_icache_page(): could not map page %#" B_PRIxPHYSADDR,
			physicalAddress);
		return;
	}

	uint32 ctr;
	asm volatile ("mrc p15, 0, %0, c0, c0, 1" : "=r" (ctr));
	const addr_t lineSize = 4 << ((ctr >> 16) & 0xf);

	dsb();
	for (addr_t line = virtualAddress; line < virtualAddress + B_PAGE_SIZE;
			line += lineSize) {
		// DCCMVAU: clean to the point of unification
		asm volatile ("mcr p15, 0, %0, c7, c11, 1" : : "r" (line) : "memory");
	}
	dsb();
	vm_put_physical_page_current_cpu(virtualAddress, handle);

	// ICIALLUIS and BPIALLIS
	asm volatile ("mcr p15, 0, %0, c7, c1, 0" : : "r" (0) : "memory");
	asm volatile ("mcr p15, 0, %0, c7, c1, 6" : : "r" (0) : "memory");
	dsb();
	isb();
}


void
arch_cpu_invalidate_TLB_page(addr_t page)
{
	// ensure visibility of the update to translation table walks
	dsb();

	// TLBIMVAIS(page)
	asm volatile ("mcr p15, 0, %0, c8, c3, 1"
		: : "r" (page));

	// ensure completion of TLB invalidation
	dsb();
	isb();
}

void
arch_cpu_invalidate_tlb_range(intptr_t, addr_t start, addr_t end)
{
	// ensure visibility of the update to translation table walks
	dsb();

	int32 num_pages = end / B_PAGE_SIZE - start / B_PAGE_SIZE;
	while (num_pages-- >= 0) {
		asm volatile ("mcr p15, 0, %[c8format], c8, c6, 1"
			: : [c8format] "r" (start) );
		start += B_PAGE_SIZE;
	}

	// ensure completion of TLB invalidation
	dsb();
	isb();
}


void
arch_cpu_invalidate_tlb_list(intptr_t, addr_t pages[], int num_pages)
{
	// ensure visibility of the update to translation table walks
	dsb();

	for (int i = 0; i < num_pages; i++) {
		asm volatile ("mcr p15, 0, %[c8format], c8, c6, 1":
			: [c8format] "r" (pages[i]) );
	}

	// ensure completion of TLB invalidation
	dsb();
	isb();
}


void
arch_cpu_global_tlb_invalidate(void)
{
	// ensure visibility of the update to translation table walks
	dsb();

	uint32 Rd = 0;
	asm volatile ("mcr p15, 0, %[c8format], c8, c7, 0"
		: : [c8format] "r" (Rd) );

	// ensure completion of TLB invalidation
	dsb();
	isb();
}


void
arch_cpu_user_tlb_invalidate(intptr_t)
{/*
	cpu_ops.flush_insn_pipeline();
	cpu_ops.flush_atc_user();
	cpu_ops.flush_insn_pipeline();
*/
#warning WRITEME
}
