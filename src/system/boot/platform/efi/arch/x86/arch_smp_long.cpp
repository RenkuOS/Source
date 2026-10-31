/*
 * Copyright 2022 Haiku, Inc. All rights reserved.
 * Copyright 2026 Akakor. All rights reserved.
 * Released under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


/*!	Application processor startup for a 64-bit kernel that is started from the
	32-bit EFI loader. The trampoline in smp_trampoline_long.S takes the
	processor from real mode to long mode, using the PML4 that
	arch_long_mmu.cpp generated, and calls the entry point of the kernel.
*/


#include "arch_smp.h"

#include <string.h>

#include <kernel.h>
#include <arch_kernel.h>
#include <boot/stage2.h>


#ifdef KERNEL_FIXUP_FOR_LONG_MODE


extern "C" void hybrid_smp_trampoline(void);
extern "C" void hybrid_smp_trampoline_args(void);
extern "C" void hybrid_smp_trampoline_end(void);

// From arch_long_mmu.cpp
extern void arch_mmu_get_long_gdt(uint64 *base, uint16 *limit);


// Temporary GDT, used until the processor is in long mode.
struct temporary_gdt {
	uint16 limit;
	uint32 base;
	uint8 null[8];
	uint8 code[8];
	uint8 data[8];
	uint8 code64[8];
} _PACKED;

// Operand of lgdt in 64-bit mode.
struct long_gdtr {
	uint16 limit;
	uint64 base;
} _PACKED;

// Arguments passed to the SMP trampoline. The order of the fields is known by
// the trampoline.
struct hybrid_trampoline_args {
	uint32 trampoline;			// Trampoline address
	uint32 gdt32;				// Temporary GDTR
	uint32 pml4;				// 64-bit PML4
	uint32 padding;
	uint64 gdt64;				// GDTR with the boot GDT of the loader
	uint64 kernel_entry;		// Kernel entry point
	uint64 kernel_args;			// Kernel arguments
	uint64 current_cpu;			// CPU number
	uint64 stack_top;			// Kernel stack
	volatile uint64 sentinel;	// Sentinel, AP sets to 0 when finished

	temporary_gdt gdtr;
	long_gdtr bootGDTR;
};


void
copy_hybrid_trampoline_code(uint64 trampolineCode)
{
	memcpy((char *)trampolineCode, (const void *)&hybrid_smp_trampoline,
		(uint64)&hybrid_smp_trampoline_end - (uint64)&hybrid_smp_trampoline);
}


void
prepare_hybrid_trampoline_args(uint64 trampolineCode, uint64 trampolineStack,
	uint32 pagedir, uint64 kernelEntry, addr_t virtKernelArgs,
	uint32 currentCpu)
{
	hybrid_trampoline_args *args = (hybrid_trampoline_args *)trampolineStack;
	args->trampoline = trampolineCode;

	args->gdt32 = (uint32)(addr_t)&args->gdtr;
	args->gdtr.limit = sizeof(args->gdtr.null) + sizeof(args->gdtr.code)
		+ sizeof(args->gdtr.data) + sizeof(args->gdtr.code64) - 1;
	args->gdtr.base = (uint32)(addr_t)args->gdtr.null;
	#define COPY_ARRAY(A, X0, X1, X2, X3, X4, X5, X6, X7) \
		{ A[0] = X0; A[1] = X1; A[2] = X2; A[3] = X3; A[4] = X4; A[5] = X5; \
			A[6] = X6; A[7] = X7; }
	COPY_ARRAY(args->gdtr.null, 0, 0, 0, 0, 0, 0, 0, 0);
	COPY_ARRAY(args->gdtr.code, 0xff, 0xff, 0, 0, 0, 0x9a, 0xcf, 0);
	COPY_ARRAY(args->gdtr.data, 0xff, 0xff, 0, 0, 0, 0x92, 0xcf, 0);
	COPY_ARRAY(args->gdtr.code64, 0xff, 0xff, 0, 0, 0, 0x9a, 0xaf, 0);
	#undef COPY_ARRAY

	// The boot GDT of the loader is in memory that is identity mapped.
	uint64 gdtBase;
	uint16 gdtLimit;
	arch_mmu_get_long_gdt(&gdtBase, &gdtLimit);
	args->bootGDTR.base = gdtBase;
	args->bootGDTR.limit = gdtLimit;
	args->gdt64 = (uint64)(addr_t)&args->bootGDTR;

	args->pml4 = pagedir;
	// The kernel sees the addresses of the loader at the top of its address
	// space, see arch_enter_kernel_long.
	args->kernel_entry = (uint64)(addr_t)kernelEntry
		+ KERNEL_FIXUP_FOR_LONG_MODE;
	args->kernel_args = (uint64)virtKernelArgs + KERNEL_FIXUP_FOR_LONG_MODE;
	args->current_cpu = currentCpu;
	// This already is a 64-bit address, see arch_smp_init_other_cpus().
	args->stack_top = gKernelArgs.cpu_kstack[currentCpu].start
		+ gKernelArgs.cpu_kstack[currentCpu].size;
	args->sentinel = 1;

	// put the args in the right place
	uint32 *argsPointer = (uint32 *)(trampolineCode
		+ (uint64)hybrid_smp_trampoline_args - (uint64)hybrid_smp_trampoline);
	*argsPointer = (uint32)trampolineStack;
}


uint32
get_hybrid_sentinel(uint64 trampolineStack)
{
	hybrid_trampoline_args *args = (hybrid_trampoline_args *)trampolineStack;
	return (uint32)args->sentinel;
}


#endif	// KERNEL_FIXUP_FOR_LONG_MODE
