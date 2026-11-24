/*
 * Copyright 2005-2016, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */


#include <arch/user_debugger.h>

#include <signal.h>
#include <string.h>

#include <debugger.h>
#include <driver_settings.h>
#include <interrupts.h>
#include <ksignal.h>
#include <team.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <util/ThreadAutoLock.h>


//#define TRACE_ARCH_USER_DEBUGGER
#ifdef TRACE_ARCH_USER_DEBUGGER
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif

#define B_NO_MORE_BREAKPOINTS				B_BUSY
#define B_NO_MORE_WATCHPOINTS				B_BUSY
#define B_BAD_WATCHPOINT_ALIGNMENT			B_BAD_VALUE
#define B_WATCHPOINT_TYPE_NOT_SUPPORTED		B_NOT_SUPPORTED
#define B_WATCHPOINT_LENGTH_NOT_SUPPORTED	B_NOT_SUPPORTED
#define B_BREAKPOINT_NOT_FOUND				B_NAME_NOT_FOUND
#define B_WATCHPOINT_NOT_FOUND				B_NAME_NOT_FOUND
	// TODO: Make those real error codes.


#ifdef __x86_64__
extern bool gHasXsave;
#else
extern bool gHasSSE;
#endif

// The software breakpoint instruction (int3).
const uint8 kX86SoftwareBreakpoint[1] = { 0xcc };

// maps breakpoint slot index to LEN_i LSB number
static const size_t sDR7Len[4] = {
	X86_DR7_LEN0_LSB, X86_DR7_LEN1_LSB, X86_DR7_LEN2_LSB, X86_DR7_LEN3_LSB
};

// maps breakpoint slot index to R/W_i LSB number
static const size_t sDR7RW[4] = {
	X86_DR7_RW0_LSB, X86_DR7_RW1_LSB, X86_DR7_RW2_LSB, X86_DR7_RW3_LSB
};

// maps breakpoint slot index to L_i bit number
static const size_t sDR7L[4] = {
	X86_DR7_L0, X86_DR7_L1, X86_DR7_L2, X86_DR7_L3
};

// maps breakpoint slot index to G_i bit number
static const size_t sDR7G[4] = {
	X86_DR7_G0, X86_DR7_G1, X86_DR7_G2, X86_DR7_G3
};

// maps breakpoint slot index to B_i bit number
static const size_t sDR6B[4] = {
	X86_DR6_B0, X86_DR6_B1, X86_DR6_B2, X86_DR6_B3
};

// Enables a hack to make single stepping work under qemu. Set via kernel
// driver settings.
static bool sQEmuSingleStepHack = false;


#ifdef __x86_64__


static void
get_iframe_registers(const iframe* frame, debug_cpu_state* cpuState)
{
	// Get general purpose registers.
	cpuState->r15 = frame->r15;
	cpuState->r14 = frame->r14;
	cpuState->r13 = frame->r13;
	cpuState->r12 = frame->r12;
	cpuState->r11 = frame->r11;
	cpuState->r10 = frame->r10;
	cpuState->r9 = frame->r9;
	cpuState->r8 = frame->r8;
	cpuState->rbp = frame->bp;
	cpuState->rsi = frame->si;
	cpuState->rdi = frame->di;
	cpuState->rdx = frame->dx;
	cpuState->rcx = frame->cx;
	cpuState->rbx = frame->bx;
	cpuState->rax = frame->ax;
	cpuState->vector = frame->vector;
	cpuState->error_code = frame->error_code;
	cpuState->rip = frame->ip;
	cpuState->cs = frame->cs;
	cpuState->rflags = frame->flags;
	cpuState->rsp = frame->sp;
	cpuState->ss = frame->ss;

	// Other segment registers are not saved or changed on interrupts, so
	// get their value here.
	uint16 seg;
	__asm__ volatile ("movw %%ds, %0" : "=r" (seg));
	cpuState->ds = seg;
	__asm__ volatile ("movw %%es, %0" : "=r" (seg));
	cpuState->es = seg;
	__asm__ volatile ("movw %%fs, %0" : "=r" (seg));
	cpuState->fs = seg;
	__asm__ volatile ("movw %%gs, %0" : "=r" (seg));
	cpuState->gs = seg;
}


static void
set_iframe_registers(iframe* frame, const debug_cpu_state* cpuState)
{
	frame->r15 = cpuState->r15;
	frame->r14 = cpuState->r14;
	frame->r13 = cpuState->r13;
	frame->r12 = cpuState->r12;
	frame->r11 = cpuState->r11;
	frame->r10 = cpuState->r10;
	frame->r9 = cpuState->r9;
	frame->r8 = cpuState->r8;
	frame->bp = cpuState->rbp;
	frame->si = cpuState->rsi;
	frame->di = cpuState->rdi;
	frame->dx = cpuState->rdx;
	frame->cx = cpuState->rcx;
	frame->bx = cpuState->rbx;
	frame->ax = cpuState->rax;

	// The debugger can set any RIP, but IRET faults in kernel mode on one
	// that is not a user address, so keep the old one in that case. CS is
	// not settable, so the frame's own CS decides what is valid.
	if (x86_64_is_user_ip(frame->cs, cpuState->rip))
		frame->ip = cpuState->rip;
	else {
		dprintf("set_iframe_registers(): ignoring invalid rip %#" B_PRIx64
			"\n", cpuState->rip);
	}

	frame->flags = (frame->flags & ~X86_EFLAGS_USER_SETTABLE_FLAGS)
		| (cpuState->rflags & X86_EFLAGS_USER_SETTABLE_FLAGS);
	frame->sp = cpuState->rsp;
}


#else	// __x86_64__


static void
get_iframe_registers(const iframe* frame, debug_cpu_state* cpuState)
{
	cpuState->gs = frame->gs;
	cpuState->fs = frame->fs;
	cpuState->es = frame->es;
	cpuState->ds = frame->ds;
	cpuState->edi = frame->di;
	cpuState->esi = frame->si;
	cpuState->ebp = frame->bp;
	cpuState->esp = frame->sp;
	cpuState->ebx = frame->bx;
	cpuState->edx = frame->orig_edx;
	cpuState->ecx = frame->cx;
	cpuState->eax = frame->orig_eax;
	cpuState->vector = frame->vector;
	cpuState->error_code = frame->error_code;
	cpuState->eip = frame->ip;
	cpuState->cs = frame->cs;
	cpuState->eflags = frame->flags;
	cpuState->user_esp = frame->user_sp;
	cpuState->user_ss = frame->user_ss;
}


static void
set_iframe_registers(iframe* frame, const debug_cpu_state* cpuState)
{
//	frame->gs = cpuState->gs;
//	frame->fs = cpuState->fs;
//	frame->es = cpuState->es;
//	frame->ds = cpuState->ds;
	frame->di = cpuState->edi;
	frame->si = cpuState->esi;
	frame->bp = cpuState->ebp;
//	frame->esp = cpuState->esp;
	frame->bx = cpuState->ebx;
	frame->dx = cpuState->edx;
	frame->cx = cpuState->ecx;
	frame->ax = cpuState->eax;
//	frame->vector = cpuState->vector;
//	frame->error_code = cpuState->error_code;
	frame->ip = cpuState->eip;
//	frame->cs = cpuState->cs;
	frame->flags = (frame->flags & ~X86_EFLAGS_USER_SETTABLE_FLAGS)
		| (cpuState->eflags & X86_EFLAGS_USER_SETTABLE_FLAGS);
	frame->user_sp = cpuState->user_esp;
//	frame->user_ss = cpuState->user_ss;
}


#endif	// __x86_64__


static void
get_cpu_state(Thread* thread, iframe* frame, debug_cpu_state* cpuState)
{
	// For the floating point state to be correct the calling function must
	// not use these registers (not even indirectly).
#ifdef __x86_64__
	memset(&cpuState->extended_registers, 0,
		sizeof(cpuState->extended_registers));

	if (frame->fpu != nullptr) {
		if (gHasXsave) {
			// TODO check the xsave header to know the actual size of the
			// register context depending on what is saved. For now we assume
			// there is only the YMM AVX registers
			memcpy(&cpuState->extended_registers, frame->fpu,
				sizeof(cpuState->extended_registers));
		} else {
			// Only the "legacy area" saved by fxsave is available
			memcpy(&cpuState->extended_registers, frame->fpu,
				sizeof(cpuState->extended_registers.fp_fxsave));
		}
	}
#else
	Thread* thisThread = thread_get_current_thread();
	if (gHasSSE) {
		if (thread == thisThread) {
			// Since fxsave requires 16-byte alignment and this isn't guaranteed
			// for the passed buffer, we use our thread's fpu_state field as
			// temporary buffer. We need to disable interrupts to make use of
			// it.
			Thread* thread = thread_get_current_thread();
			InterruptsLocker locker;
			x86_fxsave(thread->arch_info.fpu_state);
				// unlike fnsave, fxsave doesn't reinit the FPU state
		}
		memcpy(&cpuState->extended_registers, thread->arch_info.fpu_state,
			sizeof(cpuState->extended_registers));
	} else {
		if (thread == thisThread) {
			x86_fnsave(&cpuState->extended_registers);
			// fnsave reinits the FPU state after saving, so we need to
			// load it again
			x86_frstor(&cpuState->extended_registers);
		} else {
			memcpy(&cpuState->extended_registers, thread->arch_info.fpu_state,
				sizeof(cpuState->extended_registers));
		}
		// TODO: Convert to fxsave format!
	}
#endif
	get_iframe_registers(frame, cpuState);
}


static inline void
install_breakpoints(const arch_team_debug_info& teamInfo)
{
	// set breakpoints
	asm("mov %0, %%dr0" : : "r"(teamInfo.breakpoints[0].address));
	asm("mov %0, %%dr1" : : "r"(teamInfo.breakpoints[1].address));
	asm("mov %0, %%dr2" : : "r"(teamInfo.breakpoints[2].address));
	asm("mov %0, %%dr3" : : "r"(teamInfo.breakpoints[3].address));

	// enable breakpoints
	asm("mov %0, %%dr7" : : "r"(teamInfo.dr7));
}


static inline void
disable_breakpoints()
{
	asm("mov %0, %%dr7" : : "r"((size_t)X86_BREAKPOINTS_DISABLED_DR7));
}


/*! Sets a break-/watchpoint in the given team info.
	Interrupts must be disabled and the team debug info lock be held.
*/
static inline status_t
set_breakpoint(arch_team_debug_info& info, void* address, size_t type,
	size_t length, bool setGlobalFlag)
{
	// check, if there is already a breakpoint at that address
	bool alreadySet = false;
	for (int32 i = 0; i < X86_BREAKPOINT_COUNT; i++) {
		if (info.breakpoints[i].address == address
			&& info.breakpoints[i].type == type) {
			alreadySet = true;
			break;
		}
	}

	if (!alreadySet) {
		// find a free slot
		int32 slot = -1;
		for (int32 i = 0; i < X86_BREAKPOINT_COUNT; i++) {
			if (!info.breakpoints[i].address) {
				slot = i;
				break;
			}
		}

		// init the breakpoint
		if (slot >= 0) {
			info.breakpoints[slot].address = address;
			info.breakpoints[slot].type = type;
			info.breakpoints[slot].length = length;

			info.dr7 |= (length << sDR7Len[slot])
				| (type << sDR7RW[slot])
				| (1 << sDR7L[slot]);
			if (setGlobalFlag)
				info.dr7 |= (1 << sDR7G[slot]);
		} else {
			if (type == X86_INSTRUCTION_BREAKPOINT)
				return B_NO_MORE_BREAKPOINTS;
			else
				return B_NO_MORE_WATCHPOINTS;
		}
	}

	return B_OK;
}


/*! Clears a break-/watchpoint in the given team info.
	Interrupts must be disabled and the team debug info lock be held.
*/
static inline status_t
clear_breakpoint(arch_team_debug_info& info, void* address, bool watchpoint)
{
	// find the breakpoint
	int32 slot = -1;
	for (int32 i = 0; i < X86_BREAKPOINT_COUNT; i++) {
		if (info.breakpoints[i].address == address
			&& (watchpoint
				!= (info.breakpoints[i].type == X86_INSTRUCTION_BREAKPOINT))) {
			slot = i;
			break;
		}
	}

	// clear the breakpoint
	if (slot >= 0) {
		info.breakpoints[slot].address = NULL;

		info.dr7 &= ~((0x3 << sDR7Len[slot])
			| (0x3 << sDR7RW[slot])
			| (1 << sDR7L[slot])
			| (1 << sDR7G[slot]));
	} else {
		if (watchpoint)
			return B_WATCHPOINT_NOT_FOUND;
		else
			return B_BREAKPOINT_NOT_FOUND;
	}

	return B_OK;
}


static status_t
set_breakpoint(void* address, size_t type, size_t length)
{
	if (!address)
		return B_BAD_VALUE;

	Thread* thread = thread_get_current_thread();

	cpu_status state = disable_interrupts();
	GRAB_TEAM_DEBUG_INFO_LOCK(thread->team->debug_info);

	status_t error = set_breakpoint(thread->team->debug_info.arch_info, address,
		type, length, false);

	RELEASE_TEAM_DEBUG_INFO_LOCK(thread->team->debug_info);
	restore_interrupts(state);

	return error;
}


static status_t
clear_breakpoint(void* address, bool watchpoint)
{
	if (!address)
		return B_BAD_VALUE;

	Thread* thread = thread_get_current_thread();

	cpu_status state = disable_interrupts();
	GRAB_TEAM_DEBUG_INFO_LOCK(thread->team->debug_info);

	status_t error = clear_breakpoint(thread->team->debug_info.arch_info,
		address, watchpoint);

	RELEASE_TEAM_DEBUG_INFO_LOCK(thread->team->debug_info);
	restore_interrupts(state);

	return error;
}


#if KERNEL_BREAKPOINTS


static void
install_breakpoints_per_cpu(void* /*cookie*/, int cpu)
{
	Team* kernelTeam = team_get_kernel_team();

	GRAB_TEAM_DEBUG_INFO_LOCK(kernelTeam->debug_info);

	install_breakpoints(kernelTeam->debug_info.arch_info);

	RELEASE_TEAM_DEBUG_INFO_LOCK(kernelTeam->debug_info);
}


static status_t
set_kernel_breakpoint(void* address, size_t type, size_t length)
{
	if (!address)
		return B_BAD_VALUE;

	Team* kernelTeam = team_get_kernel_team();

	cpu_status state = disable_interrupts();
	GRAB_TEAM_DEBUG_INFO_LOCK(kernelTeam->debug_info);

	status_t error = set_breakpoint(kernelTeam->debug_info.arch_info, address,
		type, length, true);

	RELEASE_TEAM_DEBUG_INFO_LOCK(kernelTeam->debug_info);

	call_all_cpus(install_breakpoints_per_cpu, NULL);

	restore_interrupts(state);

	return error;
}


static status_t
clear_kernel_breakpoint(void* address, bool watchpoint)
{
	if (!address)
		return B_BAD_VALUE;

	Team* kernelTeam = team_get_kernel_team();

	cpu_status state = disable_interrupts();
	GRAB_TEAM_DEBUG_INFO_LOCK(kernelTeam->debug_info);

	status_t error = clear_breakpoint(kernelTeam->debug_info.arch_info,
		address, watchpoint);

	RELEASE_TEAM_DEBUG_INFO_LOCK(kernelTeam->debug_info);

	call_all_cpus(install_breakpoints_per_cpu, NULL);

	restore_interrupts(state);

	return error;
}

#endif	// KERNEL_BREAKPOINTS


static inline status_t
check_watch_point_parameters(void* address, uint32 type, int32 length,
	size_t& archType, size_t& archLength)
{
	// check type
	switch (type) {
		case B_DATA_WRITE_WATCHPOINT:
			archType = X86_DATA_WRITE_BREAKPOINT;
			break;
		case B_DATA_READ_WRITE_WATCHPOINT:
			archType = X86_DATA_READ_WRITE_BREAKPOINT;
			break;
		case B_DATA_READ_WATCHPOINT:
		default:
			return B_WATCHPOINT_TYPE_NOT_SUPPORTED;
			break;
	}

	// check length and alignment
	switch (length) {
		case 1:
			archLength = X86_BREAKPOINT_LENGTH_1;
			break;
		case 2:
			if ((addr_t)address & 0x1)
				return B_BAD_WATCHPOINT_ALIGNMENT;
			archLength = X86_BREAKPOINT_LENGTH_2;
			break;
		case 4:
			if ((addr_t)address & 0x3)
				return B_BAD_WATCHPOINT_ALIGNMENT;
			archLength = X86_BREAKPOINT_LENGTH_4;
			break;
		default:
			return B_WATCHPOINT_LENGTH_NOT_SUPPORTED;
	}

	return B_OK;
}


// #pragma mark - kernel debugger commands


#if KERNEL_BREAKPOINTS

static int
debugger_breakpoints(int argc, char** argv)
{
	Team* kernelTeam = team_get_kernel_team();
	arch_team_debug_info& info = kernelTeam->debug_info.arch_info;

	for (int32 i = 0; i < X86_BREAKPOINT_COUNT; i++) {
		kprintf("breakpoint[%" B_PRId32 "] ", i);

		if (info.breakpoints[i].address != NULL) {
			kprintf("%p ", info.breakpoints[i].address);
			switch (info.breakpoints[i].type) {
				case X86_INSTRUCTION_BREAKPOINT:
					kprintf("instruction");
					break;
				case X86_IO_READ_WRITE_BREAKPOINT:
					kprintf("io read/write");
					break;
				case X86_DATA_WRITE_BREAKPOINT:
					kprintf("data write");
					break;
				case X86_DATA_READ_WRITE_BREAKPOINT:
					kprintf("data read/write");
					break;
			}

			int length = 1;
			switch (info.breakpoints[i].length) {
				case X86_BREAKPOINT_LENGTH_1:
					length = 1;
					break;
				case X86_BREAKPOINT_LENGTH_2:
					length = 2;
					break;
				case X86_BREAKPOINT_LENGTH_4:
					length = 4;
					break;
			}

			if (info.breakpoints[i].type != X86_INSTRUCTION_BREAKPOINT)
				kprintf(" %d byte%s", length, (length > 1 ? "s" : ""));
		} else
			kprintf("unused");

		kprintf("\n");
	}

	return 0;
}


static int
debugger_breakpoint(int argc, char** argv)
{
	// get arguments

	if (argc < 2 || argc > 3)
		return print_debugger_command_usage(argv[0]);

	addr_t address = strtoul(argv[1], NULL, 0);
	if (address == 0)
		return print_debugger_command_usage(argv[0]);

	bool clear = false;
	if (argc == 3) {
		if (strcmp(argv[2], "clear") == 0)
			clear = true;
		else
			return print_debugger_command_usage(argv[0]);
	}

	// set/clear breakpoint

	arch_team_debug_info& info = team_get_kernel_team()->debug_info.arch_info;

	status_t error;

	if (clear) {
		error = clear_breakpoint(info, (void*)address, false);
	} else {
		error = set_breakpoint(info, (void*)address, X86_INSTRUCTION_BREAKPOINT,
			X86_BREAKPOINT_LENGTH_1, true);
	}

	if (error == B_OK)
		call_all_cpus_sync(install_breakpoints_per_cpu, NULL);
	else
		kprintf("Failed to install breakpoint: %s\n", strerror(error));

	return 0;
}


static int
debugger_watchpoint(int argc, char** argv)
{
	// get arguments

	if (argc < 2 || argc > 4)
		return print_debugger_command_usage(argv[0]);

	addr_t address = strtoul(argv[1], NULL, 0);
	if (address == 0)
		return print_debugger_command_usage(argv[0]);

	bool clear = false;
	bool readWrite = false;
	int argi = 2;
	int length = 1;
	if (argc >= 3) {
		if (strcmp(argv[argi], "clear") == 0) {
			clear = true;
			argi++;
		} else if (strcmp(argv[argi], "rw") == 0) {
			readWrite = true;
			argi++;
		}

		if (!clear && argi < argc)
			length = strtoul(argv[argi++], NULL, 0);

		if (length == 0 || argi < argc)
			return print_debugger_command_usage(argv[0]);
	}

	// set/clear breakpoint

	arch_team_debug_info& info = team_get_kernel_team()->debug_info.arch_info;

	status_t error;

	if (clear) {
		error = clear_breakpoint(info, (void*)address, true);
	} else {
		uint32 type = readWrite ? B_DATA_READ_WRITE_WATCHPOINT
			: B_DATA_WRITE_WATCHPOINT;

		size_t archType, archLength;
		error = check_watch_point_parameters((void*)address, type, length,
			archType, archLength);

		if (error == B_OK) {
			error = set_breakpoint(info, (void*)address, archType, archLength,
				true);
		}
	}

	if (error == B_OK)
		call_all_cpus_sync(install_breakpoints_per_cpu, NULL);
	else
		kprintf("Failed to install breakpoint: %s\n", strerror(error));

	return 0;
}


static int
debugger_single_step(int argc, char** argv)
{
	// TODO: Since we need an iframe, this doesn't work when KDL wasn't entered
	// via an exception.

	iframe* frame = x86_get_current_iframe();
	if (frame == NULL) {
		kprintf("Failed to get the current iframe!\n");
		return 0;
	}

	frame->flags |= (1 << X86_EFLAGS_TF);

	return B_KDEBUG_QUIT;
}


#endif	// KERNEL_BREAKPOINTS


#ifdef __x86_64__

// DR6 as the CPU resets it. Debug exceptions set its status bits, which the
// CPU does not clear otherwise (Intel SDM Vol. 3B, "Debug Status Register
// (DR6)"): it is reset wherever it is read for a debug exception from
// userland, and whenever a thread's own breakpoints are loaded, so that the
// bits it holds are always those of the current exception.
static const size_t kDR6ResetValue = 0xffff0ff0;


static inline bool
user_debugger_installed(Thread* thread)
{
	return (atomic_get(&thread->team->debug_info.flags)
		& B_TEAM_DEBUG_DEBUGGER_INSTALLED) != 0;
}


/*!	Loads the thread's own breakpoints. Interrupts must be disabled.
*/
static inline void
install_thread_breakpoints(const arch_thread& info)
{
	asm("mov %0, %%dr0" : : "r"(info.user_breakpoints[0]));
	asm("mov %0, %%dr1" : : "r"(info.user_breakpoints[1]));
	asm("mov %0, %%dr2" : : "r"(info.user_breakpoints[2]));
	asm("mov %0, %%dr3" : : "r"(info.user_breakpoints[3]));
	asm("mov %0, %%dr6" : : "r"(kDR6ResetValue));
	asm("mov %0, %%dr7" : : "r"(info.user_dr7));
}


/*!	Sets the current thread's own breakpoints and updates its
	THREAD_FLAGS_BREAKPOINTS_DEFINED bit. The team lock makes this safe against
	a debugger updating the bit for all threads of the team.
*/
static void
set_thread_breakpoints(const uint64 breakpoints[X86_BREAKPOINT_COUNT],
	uint64 dr7)
{
	Thread* thread = thread_get_current_thread();
	TeamLocker teamLocker(thread->team);

	// Only the thread itself reads them, on its way to userland.
	memcpy(thread->arch_info.user_breakpoints, breakpoints,
		sizeof(thread->arch_info.user_breakpoints));
	thread->arch_info.user_dr7 = dr7;

	if (dr7 != 0 || arch_has_breakpoints(&thread->team->debug_info.arch_info))
		atomic_or(&thread->flags, THREAD_FLAGS_BREAKPOINTS_DEFINED);
	else
		atomic_and(&thread->flags, ~THREAD_FLAGS_BREAKPOINTS_DEFINED);
}


/*!	Sets the current thread's own hardware breakpoints, which it gets SIGTRAP
	for. They are loaded when the thread returns to userland, unless a user
	debugger is installed for the team: it owns the registers then, and the
	thread's own breakpoints wait until it is gone.
	\param address DR0-DR3. Only those of enabled breakpoints are checked and
		used.
	\param dr7 The enable bits, and the R/W and LEN fields, as in DR7. Local
		and global enable mean the same. LE, GE and bit 10, which a Windows
		debug context may contain, are ignored; any other bit is refused.
*/
status_t
x86_set_thread_breakpoints(const uint64 address[4], uint64 dr7)
{
	const uint64 kAllowed = 0xffff00ff | (1 << X86_DR7_LE)
		| (1 << X86_DR7_GE) | (1 << 10);
	if ((dr7 & ~kAllowed) != 0)
		return B_BAD_VALUE;

	uint64 breakpoints[X86_BREAKPOINT_COUNT] = {};
	uint64 newDR7 = 0;
	for (int32 i = 0; i < X86_BREAKPOINT_COUNT; i++) {
		if ((dr7 & ((1 << sDR7L[i]) | (1 << sDR7G[i]))) == 0)
			continue;

		const uint64 type = (dr7 >> sDR7RW[i]) & 0x3;
		const uint64 length = (dr7 >> sDR7Len[i]) & 0x3;
		// LEN 2 is 8 bytes, where the CPU supports it (Intel SDM Vol. 3B,
		// "Debug Control Register (DR7)"); elsewhere the breakpoint may not
		// work, which only concerns the thread itself.
		const uint64 size = length == X86_BREAKPOINT_LENGTH_1 ? 1
			: length == X86_BREAKPOINT_LENGTH_2 ? 2
			: length == X86_BREAKPOINT_LENGTH_4 ? 4 : 8;
		if (type == X86_IO_READ_WRITE_BREAKPOINT
			|| (type == X86_INSTRUCTION_BREAKPOINT && size != 1)
			|| address[i] % size != 0
			|| !IS_USER_ADDRESS(address[i])
			|| !IS_USER_ADDRESS(address[i] + size - 1)) {
			return B_BAD_VALUE;
		}

		breakpoints[i] = address[i];
		newDR7 |= (1 << sDR7L[i]) | (type << sDR7RW[i])
			| (length << sDR7Len[i]);
	}
	if (newDR7 != 0)
		newDR7 |= X86_BREAKPOINTS_DISABLED_DR7;

	set_thread_breakpoints(breakpoints, newDR7);
	return B_OK;
}


/*!	Removes the current thread's own hardware breakpoints, for exec().
*/
void
x86_reset_thread_breakpoints()
{
	const uint64 breakpoints[X86_BREAKPOINT_COUNT] = {};
	set_thread_breakpoints(breakpoints, 0);
	thread_get_current_thread()->arch_info.user_dr6 = 0;
}


/*!	Whether a debug or breakpoint exception from userland goes to the current
	thread as SIGTRAP: no user debugger is installed for the team, and the
	thread has a handler for the signal. Otherwise it is a debugger event as
	before, for which the debug_server is installed if there is no debugger.
	Interrupts must be enabled.
*/
static bool
thread_catches_traps(Thread* thread)
{
	if (user_debugger_installed(thread))
		return false;

	struct sigaction action;
	return sigaction(SIGTRAP, NULL, &action) == 0
		&& action.sa_handler != SIG_DFL && action.sa_handler != SIG_IGN;
}


/*!	Sends the current thread SIGTRAP for a debug or breakpoint exception from
	userland. Like the signal of a page fault, it is not blocked or ignored:
	the team is killed then. Interrupts must be enabled.
	\param code \c TRAP_BRKPT or \c TRAP_TRACE.
	\param dr6 The status bits of DR6 for the signal frame.
*/
static void
send_trap_signal(Thread* thread, iframe* frame, int32 code, uint64 dr6)
{
	thread->arch_info.user_dr6 = dr6;

	Signal signal(SIGTRAP, code, B_OK, thread->team->id);
	signal.SetAddress((void*)frame->ip);
	send_signal_to_thread_forced(thread, signal);
}


/*!	Handles a debug exception from userland that one of the thread's own
	breakpoints raised, with SIGTRAP. Interrupts must be disabled; if this
	returns \c true, they have been enabled.
	\param dr6 DR6 when the thread entered the kernel.
	\return Whether the exception was one of the thread's own breakpoints.
*/
static bool
handle_thread_breakpoint(Thread* thread, iframe* frame, size_t dr6)
{
	// The thread's own breakpoints cannot raise it if they were not loaded,
	// and they are not changed while the thread runs in userland.
	if (!thread->arch_info.user_breakpoints_loaded)
		return false;

	const uint64 dr7 = thread->arch_info.user_dr7;
	uint64 hit = 0;
	bool instruction = false;
	for (int32 i = 0; i < X86_BREAKPOINT_COUNT; i++) {
		if ((dr6 & (1 << sDR6B[i])) == 0 || (dr7 & (1 << sDR7L[i])) == 0)
			continue;

		hit |= 1 << sDR6B[i];
		if (((dr7 >> sDR7RW[i]) & 0x3) == X86_INSTRUCTION_BREAKPOINT)
			instruction = true;
	}
	if (hit == 0)
		return false;

	// An instruction breakpoint is a fault: the instruction runs when the
	// thread returns to it with RF set, which the signal context keeps.
	if (instruction)
		frame->flags |= X86_EFLAGS_RESUME;

	enable_interrupts();

	// BS is passed on too: a single step may have been pending as well.
	send_trap_signal(thread, frame, TRAP_BRKPT,
		hit | (dr6 & (1 << X86_DR6_BS)));
	return true;
}


/*!	Returns whether \a dr6 reports a breakpoint that \a dr7 enables. DR6 may
	report one that is not enabled as well.
*/
static bool
enabled_breakpoint_hit(size_t dr6, size_t dr7)
{
	for (int32 i = 0; i < X86_BREAKPOINT_COUNT; i++) {
		if ((dr6 & (1 << sDR6B[i])) != 0
			&& (dr7 & ((1 << sDR7L[i]) | (1 << sDR7G[i]))) != 0) {
			return true;
		}
	}
	return false;
}


/*!	Handles any other debug exception from userland with SIGTRAP, if the
	thread catches it (thread_catches_traps()): a single step, when the
	thread set the trap flag itself, or ICEBP. Interrupts must be disabled;
	they are enabled if this gets past the breakpoint and debugger checks.
	\param dr6 The status bits of DR6 for this exception.
	\param dr7 DR7 when the thread entered the kernel, which is saved only
		while breakpoints are installed.
	\return Whether SIGTRAP was sent.
*/
static bool
handle_thread_debug_trap(Thread* thread, iframe* frame, size_t dr6,
	size_t dr7)
{
	// A break- or watchpoint of the team's, which can be set without a
	// debugger (_user_set_debugger_breakpoint()), is left to the debugger
	// path, which sets RF for an instruction breakpoint: as SIGTRAP without
	// it, the instruction would raise the exception again at once, forever.
	// The thread's own ones were handled already. Breakpoints are installed
	// whenever the flag is set.
	if (((thread->flags & THREAD_FLAGS_BREAKPOINTS_DEFINED) != 0
			&& enabled_breakpoint_hit(dr6, dr7))
		|| user_debugger_installed(thread)) {
		return false;
	}

	enable_interrupts();

	if (!thread_catches_traps(thread))
		return false;

	send_trap_signal(thread, frame, TRAP_TRACE, dr6 & (1 << X86_DR6_BS));
	return true;
}

#endif	// __x86_64__


// #pragma mark - in-kernel public interface


void
arch_clear_team_debug_info(arch_team_debug_info* info)
{
	for (int32 i = 0; i < X86_BREAKPOINT_COUNT; i++)
		info->breakpoints[i].address = NULL;

	info->dr7 = X86_BREAKPOINTS_DISABLED_DR7;
}


void
arch_destroy_team_debug_info(arch_team_debug_info* info)
{
	arch_clear_team_debug_info(info);
}


void
arch_clear_thread_debug_info(arch_thread_debug_info* info)
{
	info->flags = 0;
}


void
arch_destroy_thread_debug_info(arch_thread_debug_info* info)
{
	arch_clear_thread_debug_info(info);
}


void
arch_update_thread_single_step()
{
	if (iframe* frame = x86_get_user_iframe()) {
		Thread* thread = thread_get_current_thread();

		// set/clear TF in EFLAGS depending on whether single stepping is
		// desired
		if (thread->debug_info.flags & B_THREAD_DEBUG_SINGLE_STEP)
			frame->flags |= (1 << X86_EFLAGS_TF);
		else
			frame->flags &= ~(1 << X86_EFLAGS_TF);
	}
}


void
arch_set_debug_cpu_state(const debug_cpu_state* cpuState)
{
	if (iframe* frame = x86_get_user_iframe()) {
		// For the floating point state to be correct the calling function must
		// not use these registers (not even indirectly).
#ifdef __x86_64__
		Thread* thread = thread_get_current_thread();
		memcpy(thread->arch_info.user_fpu_state, &cpuState->extended_registers,
			sizeof(cpuState->extended_registers));
		frame->fpu = &thread->arch_info.user_fpu_state;
#else
		if (gHasSSE) {
			// Since fxrstor requires 16-byte alignment and this isn't
			// guaranteed passed buffer, we use our thread's fpu_state field as
			// temporary buffer. We need to disable interrupts to make use of
			// it.
			Thread* thread = thread_get_current_thread();
			InterruptsLocker locker;
			memcpy(thread->arch_info.fpu_state, &cpuState->extended_registers,
				sizeof(cpuState->extended_registers));
			x86_fxrstor(thread->arch_info.fpu_state);
		} else {
			// TODO: Implement! We need to convert the format first.
//			x86_frstor(&cpuState->extended_registers);
		}
#endif
		set_iframe_registers(frame, cpuState);
	}
}


void
arch_get_debug_cpu_state(debug_cpu_state* cpuState)
{
	if (iframe* frame = x86_get_user_iframe())
		get_cpu_state(thread_get_current_thread(), frame, cpuState);
}


/*!	\brief Retrieves the CPU state for the given thread.
	The thread must not be running and the thread's scheduler spinlock must be
	held.
	\param thread The thread whose CPU state to retrieve.
	\param cpuState Pointer to pre-allocated storage for the CPU state.
	\return \c B_OK, if everything goes fine, another error code, if the CPU
		state could not be retrieved.
*/
status_t
arch_get_thread_debug_cpu_state(Thread* thread, debug_cpu_state* cpuState)
{
	iframe* frame = x86_get_thread_user_iframe(thread);
	if (frame == NULL)
		return B_BAD_VALUE;

	get_cpu_state(thread, frame, cpuState);
	return B_OK;
}


status_t
arch_set_breakpoint(void* address)
{
	return set_breakpoint(address, X86_INSTRUCTION_BREAKPOINT,
		X86_BREAKPOINT_LENGTH_1);
}


status_t
arch_clear_breakpoint(void* address)
{
	return clear_breakpoint(address, false);
}


status_t
arch_set_watchpoint(void* address, uint32 type, int32 length)
{
	size_t archType, archLength;
	status_t error = check_watch_point_parameters(address, type, length,
		archType, archLength);
	if (error != B_OK)
		return error;

	return set_breakpoint(address, archType, archLength);
}


status_t
arch_clear_watchpoint(void* address)
{
	return clear_breakpoint(address, true);
}


bool
arch_has_breakpoints(arch_team_debug_info* info)
{
	// Reading info->dr7 is atomically, so we don't need to lock. The caller
	// has to ensure, that the info doesn't go away.
	return (info->dr7 != X86_BREAKPOINTS_DISABLED_DR7);
}


#if KERNEL_BREAKPOINTS

status_t
arch_set_kernel_breakpoint(void* address)
{
	status_t error = set_kernel_breakpoint(address, X86_INSTRUCTION_BREAKPOINT,
		X86_BREAKPOINT_LENGTH_1);

	if (error != B_OK) {
		panic("arch_set_kernel_breakpoint() failed to set breakpoint: %s",
			strerror(error));
	}

	return error;
}


status_t
arch_clear_kernel_breakpoint(void* address)
{
	status_t error = clear_kernel_breakpoint(address, false);

	if (error != B_OK) {
		panic("arch_clear_kernel_breakpoint() failed to clear breakpoint: %s",
			strerror(error));
	}

	return error;
}


status_t
arch_set_kernel_watchpoint(void* address, uint32 type, int32 length)
{
	size_t archType, archLength;
	status_t error = check_watch_point_parameters(address, type, length,
		archType, archLength);

	if (error == B_OK)
		error = set_kernel_breakpoint(address, archType, archLength);

	if (error != B_OK) {
		panic("arch_set_kernel_watchpoint() failed to set watchpoint: %s",
			strerror(error));
	}

	return error;
}


status_t
arch_clear_kernel_watchpoint(void* address)
{
	status_t error = clear_kernel_breakpoint(address, true);

	if (error != B_OK) {
		panic("arch_clear_kernel_watchpoint() failed to clear watchpoint: %s",
			strerror(error));
	}

	return error;
}

#endif	// KERNEL_BREAKPOINTS


// #pragma mark - x86 implementation interface


/**
 *	Interrupts are disabled. \a frame is unused, i.e. can be \c NULL.
 */
void
x86_init_user_debug_at_kernel_exit(iframe* frame)
{
	Thread* thread = thread_get_current_thread();

	if (!(thread->flags & THREAD_FLAGS_BREAKPOINTS_DEFINED))
		return;

	// disable kernel breakpoints
	disable_breakpoints();

	// install the user breakpoints
	GRAB_TEAM_DEBUG_INFO_LOCK(thread->team->debug_info);

	arch_team_debug_info &teamInfo = thread->team->debug_info.arch_info;

#ifdef __x86_64__
	// The thread's own breakpoints, unless a user debugger owns the registers.
	thread->arch_info.user_breakpoints_loaded
		= arch_has_thread_breakpoints(thread)
			&& !user_debugger_installed(thread);
	if (thread->arch_info.user_breakpoints_loaded)
		install_thread_breakpoints(thread->arch_info);
	else
#endif
		install_breakpoints(teamInfo);

	atomic_or(&thread->flags, THREAD_FLAGS_BREAKPOINTS_INSTALLED);

	RELEASE_TEAM_DEBUG_INFO_LOCK(thread->team->debug_info);
}


/**
 *	Interrupts are disabled.
 */
void
x86_exit_user_debug_at_kernel_entry()
{
	Thread* thread = thread_get_current_thread();

	// We need to save the current values of dr6 and dr7 in the CPU structure,
	// since in case of a debug exception we might overwrite them before
	// x86_handle_debug_exception() is called. Debug exceptions occur when
	// hitting a hardware break/watchpoint or when single-stepping.
	asm("mov %%dr6, %0" : "=r"(thread->cpu->arch.dr6));
	asm("mov %%dr7, %0" : "=r"(thread->cpu->arch.dr7));
#ifdef __x86_64__
	// The bits are kept until x86_handle_debug_exception() takes them.
	asm("mov %0, %%dr6" : : "r"(kDR6ResetValue));
#endif

	// The remainder needs only be done, when user breakpoints are installed.
	if (!(thread->flags & THREAD_FLAGS_BREAKPOINTS_INSTALLED))
		return;

	// disable user breakpoints
	disable_breakpoints();

	// install kernel breakpoints
	Team* kernelTeam = team_get_kernel_team();

	GRAB_TEAM_DEBUG_INFO_LOCK(kernelTeam->debug_info);

	install_breakpoints(kernelTeam->debug_info.arch_info);

	atomic_and(&thread->flags, ~THREAD_FLAGS_BREAKPOINTS_INSTALLED);

	RELEASE_TEAM_DEBUG_INFO_LOCK(kernelTeam->debug_info);
}


/**
 *	Interrupts are disabled and will possibly be enabled by the function.
 */
void
x86_handle_debug_exception(iframe* frame)
{
	Thread* thread = thread_get_current_thread();

	// Get dr6 and dr7. If the given iframe is a userland frame, the exception
	// obviously occurred in userland. In that case
	// x86_exit_user_debug_at_kernel_entry() has already been invoked and dr6
	// and dr7 are stored in the cpu info. Otherwise we need to fetch the
	// current values from the registers.
	size_t dr6;
	size_t dr7;
	if (IFRAME_IS_USER(frame)) {
		dr6 = thread->cpu->arch.dr6;
		dr7 = thread->cpu->arch.dr7;
#ifdef __x86_64__
		// x86_exit_user_debug_at_kernel_entry() runs only while breakpoints
		// are installed or a debugger single-steps the thread; otherwise the
		// register holds the bits. Each was reset when its bits were last
		// taken, so together they hold only this exception's. Both are reset
		// now: stale bits would make the next single step on this CPU look
		// like a breakpoint, and a thread killed after a hit of its own
		// breakpoints never reloads them.
		size_t hardwareDR6;
		asm("mov %%dr6, %0" : "=r"(hardwareDR6));
		asm("mov %0, %%dr6" : : "r"(kDR6ResetValue));
		dr6 |= hardwareDR6;
		thread->cpu->arch.dr6 = kDR6ResetValue;
#endif
	} else {
		asm("mov %%dr6, %0" : "=r"(dr6));
		asm("mov %%dr7, %0" : "=r"(dr7));
	}

	TRACE(("x86_handle_debug_exception(): DR6: %lx, DR7: %lx\n", dr6, dr7));

#ifdef __x86_64__
	if (IFRAME_IS_USER(frame)) {
		if (handle_thread_breakpoint(thread, frame, dr6)
			|| handle_thread_debug_trap(thread, frame, dr6, dr7)) {
			return;
		}
	}
#endif

	// check, which exception condition applies
	if (dr6 & X86_DR6_BREAKPOINT_MASK) {
		// breakpoint

		// check which breakpoint was taken
		bool watchpoint = true;
		for (int32 i = 0; i < X86_BREAKPOINT_COUNT; i++) {
			if (dr6 & (1 << sDR6B[i])) {
				size_t type = (dr7 >> sDR7RW[i]) & 0x3;
				if (type == X86_INSTRUCTION_BREAKPOINT)
					watchpoint = false;
			}
		}
#ifdef __x86_64__
		// The kernel entry leaves RF to this handler: let the instruction run
		// when the thread continues.
		if (!watchpoint && IFRAME_IS_USER(frame))
			frame->flags |= X86_EFLAGS_RESUME;
#endif

		if (IFRAME_IS_USER(frame)) {
			// enable interrupts and notify the debugger
			enable_interrupts();

			if (watchpoint)
				user_debug_watchpoint_hit();
			else
				user_debug_breakpoint_hit(false);
		} else {
			panic("hit kernel %spoint: dr6: 0x%lx, dr7: 0x%lx",
				watchpoint ? "watch" : "break", dr6, dr7);
		}
	} else if (dr6 & (1 << X86_DR6_BD)) {
		// general detect exception
		// Occurs only, if GD in DR7 is set (which we don't do) and someone
		// tries to write to the debug registers.
		if (IFRAME_IS_USER(frame)) {
			dprintf("x86_handle_debug_exception(): ignoring spurious general "
				"detect exception\n");

			enable_interrupts();
		} else
			panic("spurious general detect exception in kernel mode");
	} else if ((dr6 & (1 << X86_DR6_BS)) || sQEmuSingleStepHack) {
		// single step

		if (IFRAME_IS_USER(frame)) {
			// enable interrupts and notify the debugger
			enable_interrupts();

			user_debug_single_stepped();
		} else {
			// Disable single-stepping -- the next "step" command will re-enable
			// it, but we don't want it when continuing otherwise.
			frame->flags &= ~(1 << X86_EFLAGS_TF);

			// Determine whether the exception occurred at a syscall/trap
			// kernel entry or whether this is genuine kernel single-stepping.
			bool inKernel = true;
			if (thread->team != team_get_kernel_team()
				&& x86_get_user_iframe() == NULL) {
				// TODO: This is not yet fully correct, since a newly created
				// thread that hasn't entered userland yet also has this
				// property.
				inKernel = false;
			}

			if (inKernel) {
				panic("kernel single step");
			} else {
				// The thread is a userland thread and it just entered the
				// kernel when the single-step exception occurred. This happens
				// e.g. when sysenter is called with single-stepping enabled.
				// We need to ignore the exception now and send a single-step
				// notification later, when the thread wants to return from the
				// kernel.
				InterruptsSpinLocker threadDebugInfoLocker(
					thread->debug_info.lock);

				// Check whether the team is still being debugged and set
				// the B_THREAD_DEBUG_NOTIFY_SINGLE_STEP and
				// B_THREAD_DEBUG_STOP flags, so that the thread will be
				// stopped when it is going to leave the kernel and notify the
				// debugger about the single-step event.
				int32 teamDebugFlags
					= atomic_get(&thread->team->debug_info.flags);
				if (teamDebugFlags & B_TEAM_DEBUG_DEBUGGER_INSTALLED) {
					atomic_or(&thread->debug_info.flags,
						B_THREAD_DEBUG_NOTIFY_SINGLE_STEP
							| B_THREAD_DEBUG_STOP);

					// also set the respective thread flag
					atomic_or(&thread->flags, THREAD_FLAGS_DEBUG_THREAD);
				}
			}
		}
	} else if (dr6 & (1 << X86_DR6_BT)) {
		// task switch
		// Occurs only, if T in EFLAGS is set (which we don't do).
		if (IFRAME_IS_USER(frame)) {
			dprintf("x86_handle_debug_exception(): ignoring spurious task switch "
				"exception\n");

			enable_interrupts();
		} else
			panic("spurious task switch exception in kernel mode");
	} else {
		if (IFRAME_IS_USER(frame)) {
			TRACE(("x86_handle_debug_exception(): ignoring spurious debug "
				"exception (no condition recognized)\n"));

			enable_interrupts();
		} else {
			panic("spurious debug exception in kernel mode (no condition "
				"recognized)");
		}
	}
}


/**
 *	Interrupts are disabled and will possibly be enabled by the function.
 */
void
x86_handle_breakpoint_exception(iframe* frame)
{
	TRACE(("x86_handle_breakpoint_exception()\n"));

#ifdef __x86_64__
	// The signal reports the address after the int3 instruction, where the
	// thread continues, as on other systems.
	if (IFRAME_IS_USER(frame)) {
		Thread* thread = thread_get_current_thread();
		enable_interrupts();
		if (thread_catches_traps(thread)) {
			send_trap_signal(thread, frame, TRAP_BRKPT, 0);
			return;
		}
	}
#endif

	// reset eip to the int3 instruction
	frame->ip--;

	if (!IFRAME_IS_USER(frame)) {
		panic("breakpoint exception in kernel mode");
		return;
	}

	enable_interrupts();

	user_debug_breakpoint_hit(true);
}


void
x86_init_user_debug()
{
	// get debug settings
	if (void* handle = load_driver_settings("kernel")) {
		sQEmuSingleStepHack = get_driver_boolean_parameter(handle,
			"qemu_single_step_hack", false, false);;

		unload_driver_settings(handle);
	}

#if KERNEL_BREAKPOINTS
	// install debugger commands
	add_debugger_command_etc("breakpoints", &debugger_breakpoints,
		"Lists current break-/watchpoints",
		"\n"
		"Lists the current kernel break-/watchpoints.\n", 0);
	add_debugger_command_alias("watchpoints", "breakpoints", NULL);
	add_debugger_command_etc("breakpoint", &debugger_breakpoint,
		"Set/clears a breakpoint",
		"<address> [ clear ]\n"
		"Sets respectively clears the breakpoint at address <address>.\n", 0);
	add_debugger_command_etc("watchpoint", &debugger_watchpoint,
		"Set/clears a watchpoint",
		"<address> ( [ rw ] [ <size> ] | clear )\n"
		"Sets respectively clears the watchpoint at address <address>.\n"
		"If \"rw\" is given the new watchpoint is a read/write watchpoint\n"
		"otherwise a write watchpoint only.\n", 0);
	add_debugger_command_etc("step", &debugger_single_step,
		"Single-steps to the next instruction",
		"\n"
		"Single-steps to the next instruction.\n", 0);
#endif
}

