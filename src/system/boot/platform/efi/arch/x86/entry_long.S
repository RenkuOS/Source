/*
 * Copyright 2022 Haiku, Inc. All rights reserved.
 * Released under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include <asm_defs.h>

// The kernel selectors are the same in the 32-bit and the 64-bit layout, so the
// 32-bit header is fine here. See arch_long_mmu.cpp.
#include <arch/x86/descriptors.h>

.text
.code32

/*
Switches from 32-bit protected mode to long mode and enters the 64-bit kernel.
This code is copied to 0xa000 and has to be position independent otherwise, as
paging is disabled and enabled again while it runs, and it relies on the first
4GB being identity mapped by both the old and the new page tables.

extern "C" void arch_enter_kernel_long(uint32_t pageDirectory, addr_t kernelArgs,
	addr_t kernelEntry, addr_t kernelStackTop,
	struct gdt_idt_descr *gdtDescriptor);

The kernel arguments, entry point and stack top are the low 32 bits of kernel
virtual addresses. The upper 32 bits are all ones, see the end of this function.
*/
FUNCTION(arch_enter_kernel_long):
	cli
	cld

	movl	4(%esp), %edx	// pageDirectory
	movl	8(%esp), %edi	// kernelArgs
	movl	12(%esp), %ebx	// kernelEntry
	movl	16(%esp), %eax	// kernelStackTop
	movl	20(%esp), %esi	// gdtDescriptor

	// initialize stack
	movl	%eax, %esp

	// Currently running with 32-bit paging tables at an identity mapped
	// address. To switch to 64-bit paging we must first disable 32-bit paging,
	// otherwise loading the new CR3 will fault.

	// ensure that Paging is disabled
	movl	%cr0, %eax
	andl	$0x7fffffff, %eax
	movl	%eax, %cr0

	// ensure that PAE and PGE are enabled
	movl	%cr4, %eax
	orl		$0x000000a0, %eax
	movl	%eax, %cr4

	// Point CR3 to the kernel's PML4.
	movl	%edx, %eax
	movl	%eax, %cr3

	// Enable long mode by setting EFER.LME.
	movl	$0xc0000080, %ecx
	rdmsr
	orl		$0x00000100, %eax
	wrmsr

	// Re-enable paging, which will put us in compatibility mode as we are
	// currently in a 32-bit code segment.
	movl	%cr0, %eax
	orl		$0x80000001, %eax
	movl	%eax, %cr0

	// Load 64-bit enabled GDT
	movl	%esi, %eax
	lgdt	(%eax)

	// Set data segments
	movw	$KERNEL_DATA_SELECTOR, %ax
	movw	%ax, %ss
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs

	// Jump into the 64-bit code segment
	ljmp	$KERNEL_CODE_SELECTOR, $(.Llong_trampoline - arch_enter_kernel_long + 0xa000)

.align 8
.code64
.Llong_trampoline:
	// Only the low 32 bits of the registers are defined after the switch from
	// compatibility mode, so zero extend everything that holds an address.
	movl	%ebx, %eax		// kernelEntry
	movl	%edi, %edi		// kernelArgs
	movl	%esp, %esp		// kernel stack top

	// The kernel is mapped at the top of the address space, apply the upper
	// half of the addresses. Do this before touching the stack: until then
	// %rsp points to unrelated memory in the identity mapping.
	movq	$0xffffffff00000000, %rdx
	orq		%rdx, %rax
	orq		%rdx, %rdi
	orq		%rdx, %rsp

	// Clear the stack frame/RFLAGS.
	xorq	%rbp, %rbp
	pushq	$0
	popfq

	// Get arguments and call the kernel entry point.
	// kernelArgs already loaded in %rdi
	xorl	%esi, %esi		// current cpu

	call	*%rax

	//return
	movl	$-1, %eax
	ret
FUNCTION_END(arch_enter_kernel_long)
