/*
 * Copyright 2018, Jérôme Duval, jerome.duval@gmail.com.
 * Copyright 2012, Alex Smith, alex@alex-smith.me.uk.
 * Copyright 2002-2008, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#include <arch/thread.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <arch_thread_defs.h>
#include <commpage.h>
#include <cpu.h>
#include <debug.h>
#include <generic_syscall.h>
#include <kernel.h>
#include <ksignal.h>
#include <interrupts.h>
#include <team.h>
#include <thread.h>
#include <tls.h>
#include <tracing.h>
#include <util/AutoLock.h>
#include <util/Random.h>
#include <util/atomic.h>
#include <vm/vm_types.h>
#include <vm/VMAddressSpace.h>

#include "paging/X86PagingStructures.h"
#include "paging/X86VMTranslationMap.h"


//#define TRACE_ARCH_THREAD
#ifdef TRACE_ARCH_THREAD
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif


#ifdef SYSCALL_TRACING

namespace SyscallTracing {

class RestartSyscall : public AbstractTraceEntry {
	public:
		RestartSyscall()
		{
			Initialized();
		}

		virtual void AddDump(TraceOutput& out)
		{
			out.Print("syscall restart");
		}
};

}

#	define TSYSCALL(x)	new(std::nothrow) SyscallTracing::x

#else
#	define TSYSCALL(x)
#endif	// SYSCALL_TRACING


extern "C" void x86_64_thread_entry();

// Initial thread saved state.
arch_thread gInitialState _ALIGNED(64);
extern uint64 gFPUSaveLength;
extern bool gHasXsave;
extern bool gHasXsavec;


void
x86_restart_syscall(iframe* frame)
{
	Thread* thread = thread_get_current_thread();

	atomic_and(&thread->flags, ~THREAD_FLAGS_RESTART_SYSCALL);
	atomic_or(&thread->flags, THREAD_FLAGS_SYSCALL_RESTARTED);

	// Get back the original system call number and modify the frame to
	// re-execute the syscall instruction.
	frame->ax = frame->orig_rax;
	frame->ip -= 2;

	TSYSCALL(RestartSyscall());
}


// Wine reads the FS selector of the interrupted code here.
static_assert(offsetof(fpu_state, fs) == 486,
	"the signal frame layout is part of the userland ABI");


static inline uint16
read_fs_selector()
{
	uint16 selector;
	asm volatile("movw %%fs, %0" : "=r" (selector));
	return selector;
}


static inline uint16
read_ds_selector()
{
	uint16 selector;
	asm volatile("movw %%ds, %0" : "=r" (selector));
	return selector;
}


static inline uint16
read_es_selector()
{
	uint16 selector;
	asm volatile("movw %%es, %0" : "=r" (selector));
	return selector;
}


/*!	Returns the FS base the thread uses while its FS selector is 0: the one
	set with THREAD_SET_FS_BASE, or else its TLS.
*/
static inline uint64
user_fs_base(Thread* thread)
{
	if (thread->arch_info.user_fs_base != 0)
		return thread->arch_info.user_fs_base;
	return thread->user_local_storage;
}


/*!	Returns \a selector, or 0 if it is one of the null selectors 0 to 3 (the
	RPL bits do not make another segment of it).

	The kernel keeps only 0 for them. Loading any of them leaves the segment
	base to the CPU: Intel CPUs clear it, while AMD CPUs before Zen 2 keep the
	base the register had (Linux's X86_BUG_NULL_SEG), which may be the one of
	another thread. For 0, the kernel writes the FS base it keeps for the
	thread.
*/
static inline uint16
normalized_selector(uint16 selector)
{
	return (selector & ~3) == 0 ? 0 : selector;
}


/*!	Loads the thread's saved FS state into the CPU. The team's LDT must be
	loaded already, as the selector may be one of it.
	Interrupts must be disabled.
*/
static void
load_user_fs(Thread* thread)
{
	// A selector other than 0 is loaded again even when FS holds it already:
	// FS keeps the descriptor it was loaded from, which may have changed
	// since.
	uint16 selector = normalized_selector(thread->arch_info.user_fs_selector);
	if (selector != 0 || read_fs_selector() != 0)
		asm volatile("movw %0, %%fs" : : "r" (selector));
	if (selector == 0)
		x86_write_msr(IA32_MSR_FS_BASE, user_fs_base(thread));
}


/*!	Returns whether \a value may be an entry of a team's LDT: a present,
	writable 32-bit data segment of DPL 3.

	Such a segment can be loaded into every data segment register and into SS,
	and an entry never holds anything else once set. The kernel relies on that
	whenever it loads a selector again that userland had loaded: on a context
	switch, on sigreturn, and in IRET, which would fault in kernel mode. 16-bit
	segments are refused because IRET to a 16-bit stack segment restores only
	the low 16 bits of ESP, leaving bits of the kernel stack pointer in the
	rest.
*/
static bool
is_valid_ldt_descriptor(uint64 value)
{
	segment_descriptor descriptor;
	memcpy(&descriptor, &value, sizeof(descriptor));
	return descriptor.present == 1
		&& descriptor.desc_type == DT_CODE_DATA_SEGMENT
		&& (descriptor.type & DT_CODE_EXECUTE_ONLY) == 0
		&& (descriptor.type & DT_DATA_WRITEABLE) != 0
		&& descriptor.dpl == DPL_USER
		&& descriptor.long_mode == 0
		&& descriptor.d_b == 1;
}


/*!	Returns whether userland can hold \a selector in a data segment register,
	so that the kernel can load it again: the null selector, a selector of a
	present data segment or readable code segment of DPL 3 in the GDT, or one
	of an entry of the team's LDT that has been set. (The RPL does not matter
	for such a segment.)
*/
static bool
is_valid_user_data_selector(Thread* thread, uint16 selector)
{
	if ((selector & ~3) == 0)
		return true;

	uint32 index = selector >> 3;
	if ((selector & 4) != 0) {
		// Every index is within the table. Once set, an entry stays valid,
		// and the CPU has the table loaded, see get_team_ldt().
		uint64* ldt = atomic_pointer_get(&thread->team->arch_info.ldt);
		return ldt != NULL
			&& is_valid_ldt_descriptor(atomic_get64((int64*)&ldt[index]));
	}

	struct {
		uint16	limit;
		uint64	base;
	} _PACKED gdtr;
	asm volatile("sgdt %0" : "=m" (gdtr));

	if (index * sizeof(uint64) + sizeof(uint64) - 1 > gdtr.limit)
		return false;

	segment_descriptor descriptor;
	memcpy(&descriptor, (uint8*)gdtr.base + index * sizeof(uint64),
		sizeof(descriptor));
	return descriptor.present == 1
		&& descriptor.desc_type == DT_CODE_DATA_SEGMENT
		&& descriptor.dpl == DPL_USER
		&& ((descriptor.type & DT_CODE_EXECUTE_ONLY) == 0
			|| (descriptor.type & DT_CODE_READABLE) == DT_CODE_READABLE);
}


/*!	Saves the user FS, DS and ES selectors of \a thread, which must be the
	one running on this CPU. The kernel never loads these registers itself, so
	they still hold what userland left in them. A null selector is saved as 0,
	see normalized_selector().
	Interrupts must be disabled.
*/
void
x86_save_tls_context(Thread* thread)
{
	thread->arch_info.user_fs_selector = normalized_selector(read_fs_selector());
	thread->arch_info.user_ds_selector = normalized_selector(read_ds_selector());
	thread->arch_info.user_es_selector = normalized_selector(read_es_selector());
}


/*!	Loads the user segment state of \a thread into the CPU.
	Interrupts must be disabled.
*/
void
x86_set_tls_context(Thread* thread)
{
	// The selectors may be ones of the team's LDT.
	x86_64_set_ldt(smp_get_current_cpu(),
		atomic_pointer_get(&thread->team->arch_info.ldt));

	uint16 selector = thread->arch_info.user_ds_selector;
	if (selector != 0 || read_ds_selector() != 0)
		asm volatile("movw %0, %%ds" : : "r" (selector));
	selector = thread->arch_info.user_es_selector;
	if (selector != 0 || read_es_selector() != 0)
		asm volatile("movw %0, %%es" : : "r" (selector));

	load_user_fs(thread);
	x86_write_msr(IA32_MSR_KERNEL_GS_BASE, thread->arch_info.user_gs_base);
}


/*!	Gives the current thread the segment state a new thread starts with:
	64-bit code with its TLS in FS, whatever an image it replaced with exec()
	had set.
*/
void
x86_64_reset_user_segments(Thread* thread)
{
	InterruptsLocker locker;
	thread->arch_info.user_fs_base = 0;
	thread->arch_info.user_fs_selector = 0;
	thread->arch_info.user_ds_selector = 0;
	thread->arch_info.user_es_selector = 0;
	thread->arch_info.user_gs_base = 0;
	x86_set_tls_context(thread);
}


//	#pragma mark - local descriptor table


/*!	Returns the descriptor stored for an entry that is cleared: a present,
	writable data segment with base 0 and limit 0. The entry must stay
	loadable, see is_valid_ldt_descriptor().
*/
static uint64
empty_ldt_descriptor()
{
	segment_descriptor descriptor = {};
	descriptor.type = DT_DATA_WRITEABLE;
	descriptor.desc_type = DT_CODE_DATA_SEGMENT;
	descriptor.dpl = DPL_USER;
	descriptor.present = 1;
	descriptor.d_b = 1;

	uint64 value;
	memcpy(&value, &descriptor, sizeof(value));
	return value;
}


static void
load_new_team_ldt(void* _team, int cpu)
{
	Team* team = (Team*)_team;
	if (thread_get_current_thread()->team == team)
		x86_64_set_ldt(cpu, atomic_pointer_get(&team->arch_info.ldt));
}


/*!	Returns the team's LDT, and makes it if the team has none yet. On return
	every CPU that runs a thread of the team has the table loaded, so a
	selector of a new entry works wherever the team's threads are.
*/
static uint64*
get_team_ldt(Team* team)
{
	uint64* ldt = atomic_pointer_get(&team->arch_info.ldt);
	if (ldt == NULL) {
		const size_t size = THREAD_LDT_ENTRY_COUNT * sizeof(uint64);
		uint64* newLDT = (uint64*)malloc(size);
		if (newLDT == NULL)
			return NULL;
		memset(newLDT, 0, size);

		ldt = atomic_pointer_test_and_set(&team->arch_info.ldt, newLDT,
			(uint64*)NULL);
		if (ldt != NULL) {
			// another thread of the team was faster
			free(newLDT);
		} else
			ldt = newLDT;
	}

	if (atomic_get(&team->arch_info.ldt_loaded) == 0) {
		// A thread switched in before the table existed runs without it. A
		// second call does no harm, so whoever gets here does it.
		call_all_cpus_sync(&load_new_team_ldt, team);
		atomic_set(&team->arch_info.ldt_loaded, 1);
	}
	return ldt;
}


static status_t
set_ldt_entry(const thread_ldt_entry& entry)
{
	if (entry.index >= THREAD_LDT_ENTRY_COUNT)
		return B_BAD_INDEX;

	uint64 descriptor = entry.descriptor[0]
		| ((uint64)entry.descriptor[1] << 32);
	if (descriptor == 0)
		descriptor = empty_ldt_descriptor();
	else if (!is_valid_ldt_descriptor(descriptor))
		return B_BAD_VALUE;

	uint64* ldt = get_team_ldt(thread_get_current_thread()->team);
	if (ldt == NULL)
		return B_NO_MEMORY;

	// A thread that has the entry loaded already keeps the old segment until
	// it loads the selector again, or the kernel does.
	atomic_set64((int64*)&ldt[entry.index], (int64)descriptor);
	return B_OK;
}


static void
delete_team_ldt(Team* team)
{
	uint64* ldt = atomic_pointer_get_and_set(&team->arch_info.ldt,
		(uint64*)NULL);
	atomic_set(&team->arch_info.ldt_loaded, 0);
	free(ldt);
}


void
arch_team_delete_team_struct(Team* team)
{
	delete_team_ldt(team);
}


void
arch_team_reset_for_exec(Team* team)
{
	// The old image's selectors may be ones of the LDT, which goes away. The
	// default ones a new image starts with need none. The LDT is detached
	// from the team first, so that loading them unloads it from this CPU too:
	// with the table still loaded, the new image could load a selector of it
	// once it is freed. Exec leaves the team only this thread that runs in
	// userland, and every other CPU loads the LDT of a thread's team before
	// it switches to that thread.
	uint64* ldt;
	{
		InterruptsLocker locker;
		ldt = atomic_pointer_get_and_set(&team->arch_info.ldt, (uint64*)NULL);
		atomic_set(&team->arch_info.ldt_loaded, 0);
		x86_64_reset_user_segments(thread_get_current_thread());
	}
	free(ldt);
}


static addr_t
arch_randomize_stack_pointer(addr_t value)
{
	static_assert(MAX_RANDOM_VALUE >= B_PAGE_SIZE - 1,
		"randomization range is too big");
	value -= random_value() & (B_PAGE_SIZE - 1);
	return (value & ~addr_t(0xf)) - 8;
		// This means, result % 16 == 8, which is what rsp should adhere to
		// when a function is entered for the stack to be considered aligned to
		// 16 byte.
}


static uint8*
get_signal_stack(Thread* thread, iframe* frame, struct sigaction* action,
	size_t spaceNeeded)
{
	// Use the alternate signal stack if we should and can.
	if (thread->signal_stack_enabled
			&& (action->sa_flags & SA_ONSTACK) != 0
			&& (frame->user_sp < thread->signal_stack_base
				|| frame->user_sp >= thread->signal_stack_base
					+ thread->signal_stack_size)) {
		addr_t stackTop = thread->signal_stack_base + thread->signal_stack_size;
		return (uint8*)arch_randomize_stack_pointer(stackTop - spaceNeeded);
	}

	// We are going to use the stack that we are already on. We must not touch
	// the red zone (128 byte area below the stack pointer, reserved for use
	// by functions to store temporary data and guaranteed not to be modified
	// by signal handlers).
	return (uint8*)((frame->user_sp - 128 - spaceNeeded) & ~addr_t(0xf)) - 8;
		// align stack pointer (cf. arch_randomize_stack_pointer())
}


static status_t
arch_thread_control(const char* subsystem, uint32 function, void* buffer,
	size_t bufferSize)
{
	switch (function) {
		case THREAD_SET_GS_BASE:
		{
			uint64 base;
			if (bufferSize != sizeof(base))
				return B_BAD_VALUE;

			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&base, buffer, sizeof(base)) < B_OK) {
				return B_BAD_ADDRESS;
			}

			// WRMSR faults in kernel mode on a non-canonical base. Every user
			// address is canonical, and no other base is of use to userland.
			if (base > USER_TOP)
				return B_BAD_VALUE;

			Thread* thread = thread_get_current_thread();
			thread->arch_info.user_gs_base = base;
			x86_write_msr(IA32_MSR_KERNEL_GS_BASE, base);
			return B_OK;
		}

		case THREAD_SET_LDT_ENTRY:
		{
			thread_ldt_entry entry;
			if (bufferSize != sizeof(entry))
				return B_BAD_VALUE;

			if (!is_user_address_range(buffer, sizeof(entry))
				|| user_memcpy(&entry, buffer, sizeof(entry)) < B_OK) {
				return B_BAD_ADDRESS;
			}

			return set_ldt_entry(entry);
		}

		case THREAD_SET_FS_BASE:
		{
			uint64 base;
			if (bufferSize != sizeof(base))
				return B_BAD_VALUE;

			if (!is_user_address_range(buffer, sizeof(base))
				|| user_memcpy(&base, buffer, sizeof(base)) < B_OK) {
				return B_BAD_ADDRESS;
			}

			// WRMSR faults in kernel mode on a non-canonical base. Every
			// user address is canonical, and no other base is of use.
			if (base > USER_TOP)
				return B_BAD_VALUE;

			Thread* thread = thread_get_current_thread();
			InterruptsLocker locker;
			thread->arch_info.user_fs_base = base;
			thread->arch_info.user_fs_selector = 0;
			load_user_fs(thread);
			return B_OK;
		}

		case THREAD_GET_FS_BASE:
		case THREAD_GET_GS_BASE:
		{
			if (bufferSize != sizeof(uint64))
				return B_BAD_VALUE;

			Thread* thread = thread_get_current_thread();
			uint64 base = function == THREAD_GET_FS_BASE
				? user_fs_base(thread) : thread->arch_info.user_gs_base;

			if (!is_user_address_range(buffer, sizeof(base))
				|| user_memcpy(buffer, &base, sizeof(base)) < B_OK) {
				return B_BAD_ADDRESS;
			}
			return B_OK;
		}
	}
	return B_BAD_HANDLER;
}


//	#pragma mark -


status_t
arch_thread_init(kernel_args* args)
{
	// Save one global valid FPU state; it will be copied in the arch dependent
	// part of each new thread.
	// Use 0xFFFFFFFF as the mask to save all supported state components.
	if (gHasXsave || gHasXsavec) {
		if (gHasXsavec) {
			asm volatile (
				"clts;"		\
				"fninit;"	\
				"fnclex;"	\
				"movl $0xFFFFFFFF,%%eax;"	\
				"movl $0xFFFFFFFF,%%edx;"	\
				"xsavec64 %0"
				:: "m" (gInitialState.user_fpu_state));
		} else {
			asm volatile (
				"clts;"		\
				"fninit;"	\
				"fnclex;"	\
				"movl $0xFFFFFFFF,%%eax;"	\
				"movl $0xFFFFFFFF,%%edx;"	\
				"xsave64 %0"
				:: "m" (gInitialState.user_fpu_state));
		}
	} else {
		asm volatile (
			"clts;"		\
			"fninit;"	\
			"fnclex;"	\
			"fxsaveq %0"
			:: "m" (gInitialState.user_fpu_state));
	}

	// FNINIT does not affect MXCSR or data registers, so we reset them in the state.
	savefpu* initialState = ((savefpu*)&gInitialState.user_fpu_state);
	initialState->fp_fxsave.mxcsr = 0x1F80; // __INITIAL_MXCSR__
	memset(initialState->fp_fxsave.fp, 0, sizeof(initialState->fp_fxsave.fp));
	memset(initialState->fp_fxsave.xmm, 0, sizeof(initialState->fp_fxsave.xmm));
	// Clear the rest of the state, which should all be data registers.
	char* remainingState = (char*)initialState + offsetof(savefpu, fp_ymm);
	size_t remainingStateSize = sizeof(gInitialState.user_fpu_state) - offsetof(savefpu, fp_ymm);
	memset(remainingState, 0, remainingStateSize);

	register_generic_syscall(THREAD_SYSCALLS, arch_thread_control, 1, 0);
	return B_OK;
}


status_t
arch_thread_init_thread_struct(Thread* thread)
{
	// Copy the initial saved FPU state to the new thread.
	memcpy(&thread->arch_info, &gInitialState, sizeof(arch_thread));

	// Initialise the current thread pointer.
	thread->arch_info.thread = thread;

	return B_OK;
}


/*!	Prepares the given thread's kernel stack for executing its entry function.

	\param thread The thread.
	\param stack The usable bottom of the thread's kernel stack.
	\param stackTop The usable top of the thread's kernel stack.
	\param function The entry function the thread shall execute.
	\param data Pointer to be passed to the entry function.
*/
void
arch_thread_init_kthread_stack(Thread* thread, void* _stack, void* _stackTop,
	void (*function)(void*), const void* data)
{
	uintptr_t* stackTop = static_cast<uintptr_t*>(_stackTop);

	TRACE("arch_thread_init_kthread_stack: stack top %p, function %p, data: "
		"%p\n", _stackTop, function, data);

	// Save the stack top for system call entry.
	thread->arch_info.syscall_rsp = (uint64*)thread->kernel_stack_top;

	thread->arch_info.instruction_pointer
		= reinterpret_cast<uintptr_t>(x86_64_thread_entry);

	*--stackTop = uintptr_t(data);
	*--stackTop = uintptr_t(function);
	*--stackTop = uintptr_t(thread);

	// Save the stack position.
	thread->arch_info.current_stack = stackTop;
}


void
arch_thread_dump_info(void* info)
{
	arch_thread* thread = (arch_thread*)info;

	kprintf("\trsp: %p\n", thread->current_stack);
	kprintf("\tsyscall_rsp: %p\n", thread->syscall_rsp);
	kprintf("\tuser_rsp: %p\n", thread->user_rsp);
	kprintf("\tuser_fpu_state at %p\n", thread->user_fpu_state);
}


/*!	Sets up initial thread context and enters user space
*/
status_t
arch_thread_enter_userspace(Thread* thread, addr_t entry, void* args1,
	void* args2)
{
	addr_t stackTop = thread->user_stack_base + thread->user_stack_size;
	addr_t codeAddr;

	TRACE("arch_thread_enter_userspace: entry %#lx, args %p %p, "
		"stackTop %#lx\n", entry, args1, args2, stackTop);

	stackTop = arch_randomize_stack_pointer(stackTop - sizeof(codeAddr));

	// Copy the address of the stub that calls exit_thread() when the thread
	// entry function returns to the top of the stack to act as the return
	// address. The stub is inside commpage.
	addr_t commPageAddress = (addr_t)thread->team->commpage_address;
	arch_cpu_enable_user_access();
	codeAddr = ((addr_t*)commPageAddress)[COMMPAGE_ENTRY_X86_THREAD_EXIT]
		+ commPageAddress;
	arch_cpu_disable_user_access();
	if (user_memcpy((void*)stackTop, (const void*)&codeAddr, sizeof(codeAddr))
			!= B_OK)
		return B_BAD_ADDRESS;

	// Prepare the user iframe.
	iframe frame = {};
	frame.type = IFRAME_TYPE_SYSCALL;
	frame.si = (uint64)args2;
	frame.di = (uint64)args1;
	frame.ip = entry;
	frame.cs = USER_CODE_SELECTOR;
	frame.flags = X86_EFLAGS_RESERVED1 | X86_EFLAGS_INTERRUPT;
	frame.sp = stackTop;
	frame.ss = USER_DATA_SELECTOR;

	// Return to userland. Never returns.
	x86_initial_return_to_userland(thread, &frame);

	return B_OK;
}


/*!	Sets up the user iframe for invoking a signal handler.

	The function fills in the remaining fields of the given \a signalFrameData,
	copies it to the thread's userland stack (the one on which the signal shall
	be handled), and sets up the user iframe so that when returning to userland
	a wrapper function is executed that calls the user-defined signal handler.
	When the signal handler returns, the wrapper function shall call the
	"restore signal frame" syscall with the (possibly modified) signal frame
	data.

	The following fields of the \a signalFrameData structure still need to be
	filled in:
	- \c context.uc_stack: The stack currently used by the thread.
	- \c context.uc_mcontext: The current userland state of the registers.
	- \c syscall_restart_return_value: Architecture specific use. On x86_64 the
		value of rax which is overwritten by the syscall return value.

	Furthermore the function needs to set \c thread->user_signal_context to the
	userland pointer to the \c ucontext_t on the user stack.

	\param thread The current thread.
	\param action The signal action specified for the signal to be handled.
	\param signalFrameData A partially initialized structure of all the data
		that need to be copied to userland.
	\return \c B_OK on success, another error code, if something goes wrong.
*/
status_t
arch_setup_signal_frame(Thread* thread, struct sigaction* action,
	struct signal_frame_data* signalFrameData)
{
	iframe* frame = x86_get_current_iframe();
	if (!IFRAME_IS_USER(frame)) {
		panic("arch_setup_signal_frame(): No user iframe!");
		return B_BAD_VALUE;
	}

	// Store the register state.
	signalFrameData->context.uc_mcontext.rax = frame->ax;
	signalFrameData->context.uc_mcontext.rbx = frame->bx;
	signalFrameData->context.uc_mcontext.rcx = frame->cx;
	signalFrameData->context.uc_mcontext.rdx = frame->dx;
	signalFrameData->context.uc_mcontext.rdi = frame->di;
	signalFrameData->context.uc_mcontext.rsi = frame->si;
	signalFrameData->context.uc_mcontext.rbp = frame->bp;
	signalFrameData->context.uc_mcontext.r8 = frame->r8;
	signalFrameData->context.uc_mcontext.r9 = frame->r9;
	signalFrameData->context.uc_mcontext.r10 = frame->r10;
	signalFrameData->context.uc_mcontext.r11 = frame->r11;
	signalFrameData->context.uc_mcontext.r12 = frame->r12;
	signalFrameData->context.uc_mcontext.r13 = frame->r13;
	signalFrameData->context.uc_mcontext.r14 = frame->r14;
	signalFrameData->context.uc_mcontext.r15 = frame->r15;
	signalFrameData->context.uc_mcontext.rsp = frame->user_sp;
	signalFrameData->context.uc_mcontext.rip = frame->ip;
	signalFrameData->context.uc_mcontext.rflags = frame->flags;

	uint64 userFPUSaveLength = min_c(gFPUSaveLength, sizeof(savefpu));

	if (frame->fpu != nullptr) {
		memcpy((void*)&signalFrameData->context.uc_mcontext.fpu, frame->fpu,
			userFPUSaveLength);
	} else {
		memcpy((void*)&signalFrameData->context.uc_mcontext.fpu,
			gInitialState.user_fpu_state, userFPUSaveLength);
	}

	signalFrameData->context.uc_mcontext.fpu.fp_fxsave.fault_address = x86_read_cr2();
	signalFrameData->context.uc_mcontext.fpu.fp_fxsave.error_code = frame->error_code;
	signalFrameData->context.uc_mcontext.fpu.fp_fxsave.cs = frame->cs;
	signalFrameData->context.uc_mcontext.fpu.fp_fxsave.ss = frame->ss;
	signalFrameData->context.uc_mcontext.fpu.fp_fxsave.trap_number = frame->vector;
	const uint16 fsSelector = read_fs_selector();
	signalFrameData->context.uc_mcontext.fpu.fp_fxsave.fs = fsSelector;

	// Fill in signalFrameData->context.uc_stack.
	signal_get_user_stack(frame->user_sp, &signalFrameData->context.uc_stack);

	// Store syscall_restart_return_value.
	signalFrameData->syscall_restart_return_value = frame->orig_rax;

	// Only a syscall can be restarted when the handler returns: restoring the
	// frame rewinds the instruction pointer by the size of the syscall
	// instruction, which in code interrupted elsewhere lands inside the
	// instruction before it (seen as a JavaScriptCore GC thread resuming two
	// bytes into a call, an invalid opcode).
	if ((frame->type & IFRAME_TYPE_MASK) != IFRAME_TYPE_SYSCALL) {
		signalFrameData->thread_flags &= ~(THREAD_FLAGS_RESTART_SYSCALL
			| THREAD_FLAGS_64_BIT_SYSCALL_RETURN);
	}

	// Get the stack to use and copy the frame data to it.
	uint8* userStack = get_signal_stack(thread, frame, action,
		sizeof(*signalFrameData) + sizeof(frame->ip));

	signal_frame_data* userSignalFrameData
		= (signal_frame_data*)(userStack + sizeof(frame->ip));

	if (user_memcpy(userSignalFrameData, signalFrameData,
			sizeof(*signalFrameData)) != B_OK) {
		return B_BAD_ADDRESS;
	}

	// Copy a return address to the stack so that backtraces will be correct.
	if (user_memcpy(userStack, &frame->ip, sizeof(frame->ip)) != B_OK)
		return B_BAD_ADDRESS;

	// Update Thread::user_signal_context, now that everything seems to have
	// gone fine.
	thread->user_signal_context = &userSignalFrameData->context;

	// Set up the iframe to execute the signal handler wrapper on our prepared
	// stack. First argument points to the frame data.
	addr_t* commPageAddress = (addr_t*)thread->team->commpage_address;
	frame->user_sp = (addr_t)userStack;
	arch_cpu_enable_user_access();
	frame->ip = commPageAddress[COMMPAGE_ENTRY_X86_SIGNAL_HANDLER]
		+ (addr_t)commPageAddress;
	arch_cpu_disable_user_access();
	frame->di = (addr_t)userSignalFrameData;
	frame->flags &= ~(uint64)(X86_EFLAGS_TRAP | X86_EFLAGS_DIRECTION);

	// The handler is 64-bit code, but the signal may have interrupted 32-bit
	// code. IRET with that CS and the handler's address would fault in kernel
	// mode. The signal frame keeps the interrupted CS and SS for sigreturn.
	frame->cs = USER_CODE_SELECTOR;
	frame->ss = USER_DATA_SELECTOR;

	// The handler also expects its TLS in FS, which the interrupted code may
	// have replaced: 32-bit code, or code that had loaded FS (loading even the
	// null selector clears the FS base on Intel CPUs). Returning from the
	// handler restores the interrupted FS.
	{
		InterruptsLocker locker;
		thread->arch_info.user_fs_selector = 0;
		load_user_fs(thread);
	}

	return B_OK;
}


/*!	Returns whether \a ip is a user address that IRET may load into RIP when
	it returns to the user code segment \a cs. IRET raises #GP in kernel mode
	when RIP is non-canonical (Intel SDM Vol. 2A, IRET, "64-Bit Mode
	Exceptions"), so an instruction pointer that comes from userland must
	pass this check before it is written into an iframe.
*/
bool
x86_64_is_user_ip(uint64 cs, uint64 ip)
{
	if (cs == USER32_CODE_SELECTOR)
		return ip <= 0xffffffff;

	return ip <= USER_TOP;
}


int64
arch_restore_signal_frame(struct signal_frame_data* signalFrameData)
{
	iframe* frame = x86_get_current_iframe();
	Thread* thread = thread_get_current_thread();

	// CS, SS, and RIP come from userland, and IRET loads them on the way
	// back. IRET faults in kernel mode on a value it refuses, after the user
	// GS base has already been swapped back in, so check them before any of
	// the frame is changed. The kernel only ever stores these values in a
	// signal frame, so a program that returns the frame it was given always
	// passes. FS is loaded here, in kernel mode, so it is checked the same
	// way.
	const uint16 cs = signalFrameData->context.uc_mcontext.fpu.fp_fxsave.cs;
	const uint16 ss = signalFrameData->context.uc_mcontext.fpu.fp_fxsave.ss;
	const uint16 fs = signalFrameData->context.uc_mcontext.fpu.fp_fxsave.fs;
	const uint64 ip = signalFrameData->context.uc_mcontext.rip;
	if ((cs != USER_CODE_SELECTOR && cs != USER32_CODE_SELECTOR)
		|| ss != USER_DATA_SELECTOR || !x86_64_is_user_ip(cs, ip)
		|| !is_valid_user_data_selector(thread, fs)) {
		dprintf("arch_restore_signal_frame(): thread %" B_PRId32 ": invalid "
			"cs %#x, ss %#x, fs %#x, or rip %#" B_PRIx64 ". Killing "
			"thread...\n", thread->id, cs, ss, fs, ip);
		kill_thread(thread->id);
		return B_BAD_VALUE;
	}

	frame->orig_rax = signalFrameData->syscall_restart_return_value;
	frame->ax = signalFrameData->context.uc_mcontext.rax;
	frame->bx = signalFrameData->context.uc_mcontext.rbx;
	frame->cx = signalFrameData->context.uc_mcontext.rcx;
	frame->dx = signalFrameData->context.uc_mcontext.rdx;
	frame->di = signalFrameData->context.uc_mcontext.rdi;
	frame->si = signalFrameData->context.uc_mcontext.rsi;
	frame->bp = signalFrameData->context.uc_mcontext.rbp;
	frame->r8 = signalFrameData->context.uc_mcontext.r8;
	frame->r9 = signalFrameData->context.uc_mcontext.r9;
	frame->r10 = signalFrameData->context.uc_mcontext.r10;
	frame->r11 = signalFrameData->context.uc_mcontext.r11;
	frame->r12 = signalFrameData->context.uc_mcontext.r12;
	frame->r13 = signalFrameData->context.uc_mcontext.r13;
	frame->r14 = signalFrameData->context.uc_mcontext.r14;
	frame->r15 = signalFrameData->context.uc_mcontext.r15;
	frame->user_sp = signalFrameData->context.uc_mcontext.rsp;
	frame->ip = ip;
	frame->flags = (frame->flags & ~(uint64)X86_EFLAGS_USER_FLAGS)
		| (signalFrameData->context.uc_mcontext.rflags & X86_EFLAGS_USER_FLAGS);

	// Note: the error_code and vector fields are not restored. These are provided to the signal
	// handler for information purposes only, and are not used after the signal handling is
	// complete.

	frame->cs = cs;
	frame->ss = ss;

	{
		InterruptsLocker locker;
		thread->arch_info.user_fs_selector = normalized_selector(fs);
		load_user_fs(thread);
	}

	memcpy(thread->arch_info.user_fpu_state,
		(void*)&signalFrameData->context.uc_mcontext.fpu, min_c(gFPUSaveLength, sizeof(savefpu)));
	frame->fpu = &thread->arch_info.user_fpu_state;

	// The syscall return code overwrites frame->ax with the return value of
	// the syscall, need to return it here to ensure the correct value is
	// restored.
	return frame->ax;
}
