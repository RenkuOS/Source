/*
 * Copyright 2003-2011, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *      Adrien Destugues, pulkomandy@pulkomandy.tk
 */


#include <interrupts.h>
#include <cpu.h>
#include <thread.h>
#include <vm/vm_priv.h>
#include <ksyscalls.h>
#include <syscall_numbers.h>
#include <arch_cpu_defs.h>
#include <arch_thread_types.h>
#include <arch/debug.h>
#include <util/AutoLock.h>
#include <Htif.h>
#include <Plic.h>
#include <Clint.h>
#include <AutoDeleterDrivers.h>
#include <ScopeExit.h>
#include "RISCV64VMTranslationMap.h"

#include <algorithm>


static uint32 sPlicContexts[SMP_MAX_CPUS];

// The PLIC sources, which are the I/O interrupt vectors reserved in
// arch_int_init_post_vm(). Vectors above them (MSI, for instance) are dispatched by
// their own controllers and never reach the PLIC registers here.
static const int32 kPlicVectorCount = 128;

// Each source is enabled in the PLIC context of one CPU only, the one it is
// assigned to (CPU 0 until the kernel assigns it), so that only that CPU
// takes it. The enable registers are shared by all CPUs: the lock guards
// their read-modify-write.
static spinlock sPlicLock = B_SPINLOCK_INITIALIZER;
static int32 sPlicSourceCpu[kPlicVectorCount];
static uint32 sPlicSourceEnabled[kPlicVectorCount / 32];


//#pragma mark -

static void
SendSignal(debug_exception_type type, uint32 signalNumber, int32 signalCode,
	addr_t signalAddress = 0, int32 signalError = B_ERROR)
{
	if (SstatusReg{.val = Sstatus()}.spp == modeU) {
		struct sigaction action;
		Thread* thread = thread_get_current_thread();

		enable_interrupts();

		// If the thread has a signal handler for the signal, we simply send it
		// the signal. Otherwise we notify the user debugger first.
		if ((sigaction(signalNumber, NULL, &action) == 0
				&& action.sa_handler != SIG_DFL
				&& action.sa_handler != SIG_IGN)
			|| user_debug_exception_occurred(type, signalNumber)) {
			Signal signal(signalNumber, signalCode, signalError,
				thread->team->id);
			signal.SetAddress((void*)signalAddress);
			send_signal_to_thread(thread, signal, 0);
		}
	} else {
		panic("Unexpected exception occurred in kernel mode!");
	}
}


static void
AfterInterrupt()
{
	if (debug_debugger_running())
		return;

	Thread* thread = thread_get_current_thread();
	cpu_status state = disable_interrupts();
	if (thread->post_interrupt_callback != NULL) {
		void (*callback)(void*) = thread->post_interrupt_callback;
		void* data = thread->post_interrupt_data;

		thread->post_interrupt_callback = NULL;
		thread->post_interrupt_data = NULL;

		restore_interrupts(state);

		callback(data);
	} else if (thread->cpu->invoke_scheduler) {
		SpinLocker schedulerLocker(thread->scheduler_lock);
		scheduler_reschedule(B_THREAD_READY);
		schedulerLocker.Unlock();
		restore_interrupts(state);
	}
}


static bool
SetAccessedFlags(addr_t addr, bool isWrite, addr_t epc, uint64 cause)
{
	VMAddressSpacePutter addressSpace;
	if (IS_KERNEL_ADDRESS(addr))
		addressSpace.SetTo(VMAddressSpace::GetKernel());
	else if (IS_USER_ADDRESS(addr))
		addressSpace.SetTo(VMAddressSpace::GetCurrent());

	if(!addressSpace.IsSet())
		return false;

	RISCV64VMTranslationMap* map
		= (RISCV64VMTranslationMap*)addressSpace->TranslationMap();

	phys_addr_t physAdr;
	uint32 pageFlags;
	map->QueryInterrupt(addr, &physAdr, &pageFlags);

	if ((PAGE_PRESENT & pageFlags) == 0)
		return false;

	bool permitted;
	uint32 neededFlags;
	if (isWrite) {
		permitted = ((B_WRITE_AREA | B_KERNEL_WRITE_AREA) & pageFlags) != 0;
		neededFlags = PAGE_ACCESSED | PAGE_MODIFIED;
	} else {
		permitted = ((B_READ_AREA | B_KERNEL_READ_AREA) & pageFlags) != 0;
		neededFlags = PAGE_ACCESSED;
	}
	if (!permitted)
		return false;

	if ((neededFlags & pageFlags) != neededFlags) {
		map->SetFlags(addr, neededFlags);
		return true;
	}

	// The page is present, allowed and its accessed/modified flags are set:
	// the fault came from a stale TLB entry of this CPU. SetFlags() only
	// flushes the TLB of the CPU that set the flags, so another CPU can still
	// hold the entry from before, without them (as Linux, retry after a local
	// flush). The same fault again right away -- same page, same instruction,
	// same kind of access -- is a real one; a different fault on the page,
	// such as a write after a read, is retried too.
	static struct {
		addr_t		page;
		addr_t		epc;
		uint64		cause;
		bigtime_t	time;
	} sLastSpuriousFault[SMP_MAX_CPUS];
	int32 cpu = smp_get_current_cpu();
	addr_t page = ROUNDDOWN(addr, B_PAGE_SIZE);
	bigtime_t now = system_time();
	if (sLastSpuriousFault[cpu].page == page
		&& sLastSpuriousFault[cpu].epc == epc
		&& sLastSpuriousFault[cpu].cause == cause
		&& now - sLastSpuriousFault[cpu].time < 1000) {
		sLastSpuriousFault[cpu].page = 0;
		return false;
	}
	sLastSpuriousFault[cpu].page = page;
	sLastSpuriousFault[cpu].epc = epc;
	sLastSpuriousFault[cpu].cause = cause;
	sLastSpuriousFault[cpu].time = now;

	if (IS_KERNEL_ADDRESS(addr))
		FlushTlbPage(addr);
	else
		FlushTlbPageAsid(addr, 0);
	return true;
}


extern "C" void SVec();
extern "C" void SVecRet();
extern "C" void SVecU();
extern "C" void SVecURet();


/*!	A fault taken while SVec or SVecU push the trap frame cannot be recovered:
	the nested trap has overwritten sepc, scause, stval and sstatus of the trap
	being entered, so the outer frame records the nested fault instead.
*/
static void
CheckTrapEntryFault(iframe* frame)
{
	addr_t pc = frame->epc;
	if ((pc >= (addr_t)&SVec && pc < (addr_t)&SVecRet)
		|| (pc >= (addr_t)&SVecU && pc < (addr_t)&SVecURet)) {
		panic("fault in the trap entry code: cause %" B_PRIu64 ", tval %#"
			B_PRIx64 ", epc %#" B_PRIx64 ", sp %#" B_PRIx64 "; the trap being "
			"entered is lost", frame->cause, frame->tval, frame->epc,
			frame->sp);
	}
}


extern "C" void
STrap(iframe* frame)
{
	if (frame->cause < 16)
		CheckTrapEntryFault(frame);

	switch (frame->cause) {
		case causeExecPageFault:
		case causeLoadPageFault:
		case causeStorePageFault: {
			if (SetAccessedFlags(Stval(), frame->cause == causeStorePageFault,
					frame->epc, frame->cause)) {
				return;
			}
		}
	}

	if (SstatusReg{.val = frame->status}.spp == modeU) {
		thread_get_current_thread()->arch_info.userFrame = frame;
		thread_get_current_thread()->arch_info.oldA0 = frame->a0;
		thread_at_kernel_entry(system_time());
	}
	const auto& kernelExit = ScopeExit([&]() {
		if (SstatusReg{.val = frame->status}.spp == modeU) {
			disable_interrupts();
			atomic_and(&thread_get_current_thread()->flags, ~THREAD_FLAGS_SYSCALL_RESTARTED);
			if ((thread_get_current_thread()->flags
				& (THREAD_FLAGS_SIGNALS_PENDING
				| THREAD_FLAGS_DEBUG_THREAD
				| THREAD_FLAGS_TRAP_FOR_CORE_DUMP)) != 0) {
				enable_interrupts();
				thread_at_kernel_exit();
			} else {
				thread_at_kernel_exit_no_signals();
			}
			if ((THREAD_FLAGS_RESTART_SYSCALL & thread_get_current_thread()->flags) != 0) {
				atomic_and(&thread_get_current_thread()->flags, ~THREAD_FLAGS_RESTART_SYSCALL);
				atomic_or(&thread_get_current_thread()->flags, THREAD_FLAGS_SYSCALL_RESTARTED);

				frame->a0 = thread_get_current_thread()->arch_info.oldA0;
				frame->epc -= 4;
			}
			thread_get_current_thread()->arch_info.userFrame = NULL;
		}
	});

	switch (frame->cause) {
		case causeIllegalInst: {
			return SendSignal(B_INVALID_OPCODE_EXCEPTION, SIGILL, ILL_ILLOPC,
				frame->epc);
		}
		case causeExecMisalign:
		case causeLoadMisalign:
		case causeStoreMisalign: {
			return SendSignal(B_ALIGNMENT_EXCEPTION, SIGBUS, BUS_ADRALN,
				Stval());
		}
		case causeBreakpoint: {
			if (SstatusReg{.val = frame->status}.spp == modeU) {
				user_debug_breakpoint_hit(false);
			} else {
				panic("hit kernel breakpoint");
			}
			return;
		}
		case causeExecAccessFault:
		case causeLoadAccessFault:
		case causeStoreAccessFault: {
			return SendSignal(B_SEGMENT_VIOLATION, SIGBUS, BUS_ADRERR,
				Stval());
		}
		case causeExecPageFault:
		case causeLoadPageFault:
		case causeStorePageFault: {
			uint64 stval = Stval();

			if (debug_debugger_running()) {
				Thread* thread = thread_get_current_thread();
				if (thread != NULL) {
					cpu_ent* cpu = &gCPU[smp_get_current_cpu()];
					if (cpu->fault_handler != 0) {
						debug_set_page_fault_info(stval, frame->epc,
							(frame->cause == causeStorePageFault)
								? DEBUG_PAGE_FAULT_WRITE : 0);
						frame->epc = cpu->fault_handler;
						frame->sp = cpu->fault_handler_stack_pointer;
						return;
					}

					if (thread->fault_handler != 0) {
						kprintf("ERROR: thread::fault_handler used in kernel "
							"debugger!\n");
						debug_set_page_fault_info(stval, frame->epc,
							frame->cause == causeStorePageFault
								? DEBUG_PAGE_FAULT_WRITE : 0);
						frame->epc = (addr_t)thread->fault_handler;
						return;
					}
				}

				panic("page fault in debugger without fault handler! Touching "
					"address %p from ip %p\n", (void*)stval, (void*)frame->epc);
				return;
			}

			if (SstatusReg{.val = frame->status}.pie == 0) {
				// user_memcpy() failure
				Thread* thread = thread_get_current_thread();
				if (thread != NULL && thread->fault_handler != 0) {
					addr_t handler = (addr_t)(thread->fault_handler);
					if (frame->epc != handler) {
						frame->epc = handler;
						return;
					}
				}
				panic("page fault with interrupts disabled@!dump_virt_page %#" B_PRIx64, stval);
			}

			addr_t newIP = 0;
			enable_interrupts();

			vm_page_fault(stval, frame->epc, frame->cause == causeStorePageFault,
				frame->cause == causeExecPageFault,
				SstatusReg{.val = frame->status}.spp == modeU, &newIP);

			if (newIP != 0)
				frame->epc = newIP;

			return;
		}
		case causeInterrupt + sSoftInt: {
			ClearBitsSip(1 << sSoftInt);
			// dprintf("sSoftInt(%" B_PRId32 ")\n", smp_get_current_cpu());
			smp_intercpu_interrupt_handler(smp_get_current_cpu());
			AfterInterrupt();
			return;
		}
		case causeInterrupt + sTimerInt: {
			ClearBitsSie(1 << sTimerInt);
			// dprintf("sTimerInt(%" B_PRId32 ")\n", smp_get_current_cpu());
			timer_interrupt();
			AfterInterrupt();
			return;
		}
		case causeInterrupt + sExternInt: {
			uint64 irq = gPlicRegs->contexts[sPlicContexts[smp_get_current_cpu()]].claimAndComplete;
			io_interrupt_handler(irq, B_LEVEL_TRIGGERED);
			gPlicRegs->contexts[sPlicContexts[smp_get_current_cpu()]].claimAndComplete = irq;
			AfterInterrupt();
			return;
		}
		case causeUEcall: {
			frame->epc += 4; // skip ecall
			uint64 syscall = frame->t0;
			uint64 args[20];
			if (syscall < (uint64)kSyscallCount) {
				uint32 argCnt = kExtendedSyscallInfos[syscall].parameter_count;
				memcpy(&args[0], &frame->a0,
					sizeof(uint64)*std::min<uint32>(argCnt, 8));
				if (argCnt > 8) {
					if (status_t res = user_memcpy(&args[8], (void*)frame->sp,
						sizeof(uint64)*(argCnt - 8)) < B_OK) {
						dprintf("can't read syscall arguments on user "
							"stack\n");
						frame->a0 = res;
						return;
					}
				}
			}

			enable_interrupts();
			uint64 returnValue = 0;
			syscall_dispatcher(syscall, (void*)args, &returnValue);
			frame->a0 = returnValue;
			return;
		}
	}
	panic("unhandled STrap");
}


//#pragma mark -

status_t
arch_int_init(kernel_args* args)
{
	dprintf("arch_int_init()\n");

	for (uint32 i = 0; i < args->num_cpus; i++) {
		dprintf("  CPU %" B_PRIu32 ":\n", i);
		dprintf("    hartId: %" B_PRIu32 "\n", args->arch_args.hartIds[i]);
		dprintf("    plicContext: %" B_PRIu32 "\n", args->arch_args.plicContexts[i]);
	}

	for (uint32 i = 0; i < args->num_cpus; i++)
		sPlicContexts[i] = args->arch_args.plicContexts[i];

	for (uint32 i = 0; i < args->num_cpus; i++)
		gPlicRegs->contexts[sPlicContexts[i]].priorityThreshold = 0;

	return B_OK;
}


status_t
arch_int_init_post_vm(kernel_args* args)
{
	// Not in arch_int_init(): interrupts_init_post_vm() initializes every
	// vector before it calls this, which would turn the reserved PLIC vectors
	// back into INTERRUPT_TYPE_UNKNOWN. The kernel only assigns IRQ vectors
	// to CPUs and balances their load.
	// TODO: read from FDT
	reserve_io_interrupt_vectors(kPlicVectorCount, 0, INTERRUPT_TYPE_IRQ);
	return B_OK;
}


status_t
arch_int_init_post_device_manager(struct kernel_args* args)
{
	return B_OK;
}


status_t
arch_int_init_io(kernel_args* args)
{
	return B_OK;
}


static inline void
plic_set_enabled(int32 irq, int32 cpu, bool enabled)
{
	volatile uint32& enable = gPlicRegs->enable[sPlicContexts[cpu]][irq / 32];
	if (enabled)
		enable |= 1U << (irq % 32);
	else
		enable &= ~(1U << (irq % 32));
}


void
arch_int_enable_io_interrupt(int32 irq)
{
	dprintf("arch_int_enable_io_interrupt(%" B_PRId32 ")\n", irq);
	if (irq < 0 || irq >= kPlicVectorCount)
		return;

	InterruptsSpinLocker locker(sPlicLock);
	gPlicRegs->priority[irq] = 1;
	plic_set_enabled(irq, sPlicSourceCpu[irq], true);
	sPlicSourceEnabled[irq / 32] |= 1U << (irq % 32);
}


void
arch_int_disable_io_interrupt(int32 irq)
{
	dprintf("arch_int_disable_io_interrupt(%" B_PRId32 ")\n", irq);
	if (irq < 0 || irq >= kPlicVectorCount)
		return;

	InterruptsSpinLocker locker(sPlicLock);
	gPlicRegs->priority[irq] = 0;
	plic_set_enabled(irq, sPlicSourceCpu[irq], false);
	sPlicSourceEnabled[irq / 32] &= ~(1U << (irq % 32));
}


/*!	Moves a PLIC source to the context of \a cpu. An enabled source is
	enabled for the new CPU before it is disabled for the old one, so that a
	pending interrupt always has a CPU to take it; one already claimed is
	completed by the CPU that claimed it, through that CPU's context.
*/
int32
arch_int_assign_to_cpu(int32 irq, int32 cpu)
{
	if (irq < 0 || irq >= kPlicVectorCount)
		return 0;
	if (cpu < 0 || cpu >= smp_get_num_cpus())
		cpu = 0;

	InterruptsSpinLocker locker(sPlicLock);
	int32 oldCpu = sPlicSourceCpu[irq];
	if (oldCpu == cpu)
		return cpu;

	if ((sPlicSourceEnabled[irq / 32] & (1U << (irq % 32))) != 0) {
		plic_set_enabled(irq, cpu, true);
		plic_set_enabled(irq, oldCpu, false);
	}
	sPlicSourceCpu[irq] = cpu;
	return cpu;
}


#undef arch_int_enable_interrupts
#undef arch_int_disable_interrupts
#undef arch_int_restore_interrupts
#undef arch_int_are_interrupts_enabled


extern "C" void
arch_int_enable_interrupts()
{
	arch_int_enable_interrupts_inline();
}


extern "C" int
arch_int_disable_interrupts()
{
	return arch_int_disable_interrupts_inline();
}


extern "C" void
arch_int_restore_interrupts(int oldState)
{
	arch_int_restore_interrupts_inline(oldState);
}


extern "C" bool
arch_int_are_interrupts_enabled()
{
	return arch_int_are_interrupts_enabled_inline();
}
