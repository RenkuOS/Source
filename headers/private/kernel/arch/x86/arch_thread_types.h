/*
 * Copyright 2002-2006, The Haiku Team. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */
#ifndef _KERNEL_ARCH_x86_THREAD_TYPES_H
#define _KERNEL_ARCH_x86_THREAD_TYPES_H


#include <SupportDefs.h>

#ifdef __x86_64__
#	include <arch/x86/64/iframe.h>
#else
#	include <arch/x86/32/iframe.h>
#endif


namespace BKernel {
    struct Thread;
}


#define _ALIGNED(bytes) __attribute__((aligned(bytes)))
	// move this to somewhere else, maybe BeBuild.h?


#ifndef __x86_64__
struct farcall {
	uint32* esp;
	uint32* ss;
};
#endif


// architecture specific thread info
struct arch_thread {
#ifdef __x86_64__
	// Back pointer to the containing Thread structure. The GS segment base is
	// pointed here, used to get the current thread.
	BKernel::Thread* thread;

	// RSP for kernel entry used by SYSCALL, and temporary scratch space.
	uint64*			syscall_rsp;
	uint64*			user_rsp;

	uintptr_t*		current_stack;
	uintptr_t		instruction_pointer;

	uint64			user_gs_base;

	// The user FS, DS and ES selectors, and the FS base used while the FS
	// selector is 0 (0: the thread's TLS). Only 32-bit code needs anything
	// but 0 in them, and then they must survive context switches.
	uint64			user_fs_base;
	uint16			user_fs_selector;
	uint16			user_ds_selector;
	uint16			user_es_selector;
#else
	struct farcall	current_stack;
	struct farcall	interrupt_stack;
#endif

#ifndef __x86_64__
	// 512 byte floating point save point - this must be 16 byte aligned
	uint8			fpu_state[512] _ALIGNED(16);
#else
	// floating point save point - this must be 64 byte aligned for xsave and
	// have enough space for all the registers.
	// This is sufficient for AVX, AVX-512 (which we conditionally enable)
	// and PT (may be force-enabled by the host when we are running under VMs).
	uint8			user_fpu_state[2688] _ALIGNED(64);
#endif

	addr_t			GetFramePointer() const;
} _ALIGNED(16);


struct arch_team {
#ifdef __x86_64__
	// The team's local descriptor table, made by the first
	// THREAD_SET_LDT_ENTRY. It is never resized, and an entry once set always
	// holds a segment that can be loaded, so the kernel can reload any
	// selector of it a thread had loaded. NULL while the team has none.
	uint64*			ldt;
	int32			ldt_loaded;
		// set once every CPU running a thread of the team has loaded ldt
#else
	// gcc treats empty structures as zero-length in C, but as if they contain
	// a char in C++. So we have to put a dummy in to be able to use the struct
	// from both in a consistent way.
	char			dummy;
#endif
};


struct arch_fork_arg {
	struct iframe	iframe;
};


#ifdef __x86_64__


inline addr_t
arch_thread::GetFramePointer() const
{
	return current_stack[0];
}


#else


inline addr_t
arch_thread::GetFramePointer() const
{
	return current_stack.esp[2];
}


#endif	// __x86_64__

#endif	// _KERNEL_ARCH_x86_THREAD_TYPES_H
