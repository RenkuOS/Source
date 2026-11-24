/*
 * Copyright 2026, rations.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	x86_64_debug_registers_test: a thread's own hardware breakpoints on x86_64.

	THREAD_SET_DEBUG_REGISTERS sets breakpoints for the calling thread only. A
	breakpoint that is hit raises SIGTRAP, and the signal context holds DR6
	with the breakpoints that were hit. Wine needs that to give Windows
	programs the debug registers of their threads.

	Run without arguments. "x86_64_debug_registers_test --exec-child <address>"
	is the half of the exec() test that runs in the new image.
*/


#include <OS.h>

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <arch_thread_defs.h>
#include <syscalls.h>


// DR7 fields (Intel SDM Vol. 3B, Debug Control Register (DR7))
#define DR7_LOCAL(slot)		(1ull << ((slot) * 2))
#define DR7_GLOBAL(slot)	(2ull << ((slot) * 2))
#define DR7_RW(slot, rw)	((uint64)(rw) << (16 + (slot) * 4))
#define DR7_LEN(slot, len)	((uint64)(len) << (18 + (slot) * 4))
#define DR7_LE				(1ull << 8)
#define DR7_GE				(1ull << 9)
#define DR7_BIT10			(1ull << 10)
#define DR7_GD				(1ull << 13)

#define RW_EXECUTE			0
#define RW_WRITE			1
#define RW_IO				2
#define RW_READ_WRITE		3

#define LEN_1				0
#define LEN_2				1
#define LEN_8				2
#define LEN_4				3

#define DR6_B(slot)			(1ull << (slot))
#define EFLAGS_RF			(1ull << 16)

#define DEBUG_EXCEPTION		1
#define KERNEL_ADDRESS		0xffffffff80000000ull

#define MAX_THREADS			32
#define THREAD_ROUNDS		5000
#define FIXED_ADDRESSES		0x50000000


static int sFailures;

#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			printf("  FAIL line %d: ", __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
			sFailures++; \
		} \
	} while (0)


// What the SIGTRAP handler saw, per thread. Volatile, so that the compiler
// does not read them before the access that raises the signal.
static __thread volatile int tTraps;
static __thread volatile int tTrapsBySlot[4];
static __thread volatile int tCode;
static __thread void* volatile tAddress;
static __thread volatile uint64 tDR6;
static __thread volatile uint64 tFlags;
static __thread volatile int tTrapNumber;

static __thread volatile int tUsr1;
static __thread volatile uint64 tUsr1DR6;

static volatile int sCalls;
static volatile uint64 sWatched8;
static volatile uint32 sWatched4;
static volatile uint16 sWatched2;
static volatile uint8 sWatched1;
static volatile uint64 sThreadVariables[MAX_THREADS];


static void __attribute__((noinline))
target_function(void)
{
	sCalls++;
	__asm__ volatile("" ::: "memory");
}


static void __attribute__((noinline))
never_called_function(void)
{
	sCalls += 1000;
	__asm__ volatile("" ::: "memory");
}


static status_t
set_debug_registers(uint64 address0, uint64 address1, uint64 address2,
	uint64 address3, uint64 dr7)
{
	struct thread_debug_registers registers = {
		{ address0, address1, address2, address3 }, dr7
	};
	return _kern_generic_syscall(THREAD_SYSCALLS, THREAD_SET_DEBUG_REGISTERS,
		&registers, sizeof(registers));
}


static status_t
set_one(int slot, uint64 address, int rw, int len)
{
	uint64 addresses[4] = {};
	addresses[slot] = address;
	return set_debug_registers(addresses[0], addresses[1], addresses[2],
		addresses[3], DR7_LOCAL(slot) | DR7_RW(slot, rw) | DR7_LEN(slot, len));
}


static status_t
clear_all(void)
{
	return set_debug_registers(0, 0, 0, 0, 0);
}


static void
trap_handler(int signal, siginfo_t* info, void* _context)
{
	ucontext_t* context = (ucontext_t*)_context;
	tTraps++;
	tCode = info->si_code;
	tAddress = info->si_addr;
	tDR6 = context->uc_mcontext.fpu.fp_fxsave.dr6;
	tFlags = context->uc_mcontext.rflags;
	tTrapNumber = context->uc_mcontext.fpu.fp_fxsave.trap_number;
	for (int slot = 0; slot < 4; slot++) {
		if ((tDR6 & DR6_B(slot)) != 0)
			tTrapsBySlot[slot]++;
	}
}


static void
usr1_handler(int signal, siginfo_t* info, void* _context)
{
	ucontext_t* context = (ucontext_t*)_context;
	tUsr1++;
	tUsr1DR6 = context->uc_mcontext.fpu.fp_fxsave.dr6;
}


static void
install_handler(int signal, void (*handler)(int, siginfo_t*, void*))
{
	struct sigaction action = {};
	action.sa_sigaction = handler;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	sigaction(signal, &action, NULL);
}


static void
reset_traps(void)
{
	tTraps = 0;
	for (int slot = 0; slot < 4; slot++)
		tTrapsBySlot[slot] = 0;
	tCode = 0;
	tAddress = NULL;
	tDR6 = 0;
	tFlags = 0;
	tTrapNumber = 0;
}


/*!	Creates an area at \a base, or with \a base 0 at the first free one of
	some fixed addresses, so that another image can ask for the same.
*/
static void*
create_fixed_area(addr_t base)
{
	addr_t first = base != 0 ? base : FIXED_ADDRESSES;
	addr_t last = base != 0 ? base : 0xf0000000;
	for (addr_t candidate = first; candidate <= last;
			candidate += 0x10000000) {
		void* address = (void*)candidate;
		if (create_area("debug registers test", &address, B_EXACT_ADDRESS,
				B_PAGE_SIZE, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA) >= 0) {
			return address;
		}
	}
	return NULL;
}


//	#pragma mark - tests


static void
test_calls(void)
{
	printf("calls: what is refused\n");

	struct thread_debug_registers registers = {};
	CHECK(_kern_generic_syscall(THREAD_SYSCALLS, THREAD_SET_DEBUG_REGISTERS,
			&registers, sizeof(registers) - 1) == B_BAD_VALUE,
		"a short buffer is accepted");
	CHECK(_kern_generic_syscall(THREAD_SYSCALLS, THREAD_SET_DEBUG_REGISTERS,
			(void*)KERNEL_ADDRESS, sizeof(registers)) == B_BAD_ADDRESS,
		"a kernel buffer is accepted");

	const uint64 target = (uint64)(addr_t)&target_function;
	const uint64 data = (uint64)(addr_t)&sWatched8;

	CHECK(set_debug_registers(0, 0, 0, 0, DR7_GD) == B_BAD_VALUE,
		"GD is accepted");
	const int otherBits[] = { 11, 12, 14, 15, 32, 63 };
	for (size_t i = 0; i < sizeof(otherBits) / sizeof(otherBits[0]); i++) {
		CHECK(set_debug_registers(target, 0, 0, 0,
				DR7_LOCAL(0) | (1ull << otherBits[i])) == B_BAD_VALUE,
			"DR7 bit %d is accepted", otherBits[i]);
	}
	CHECK(set_one(0, data, RW_IO, LEN_4) == B_BAD_VALUE,
		"an I/O breakpoint is accepted");
	CHECK(set_one(0, target, RW_EXECUTE, LEN_4) == B_BAD_VALUE,
		"an instruction breakpoint of length 4 is accepted");
	CHECK(set_one(1, data + 2, RW_WRITE, LEN_4) == B_BAD_VALUE,
		"a misaligned watchpoint is accepted");
	CHECK(set_one(2, data + 4, RW_WRITE, LEN_8) == B_BAD_VALUE,
		"a misaligned 8-byte watchpoint is accepted");
	CHECK(set_one(3, KERNEL_ADDRESS, RW_EXECUTE, LEN_1) == B_BAD_VALUE,
		"a kernel address is accepted");
	CHECK(set_one(3, KERNEL_ADDRESS, RW_READ_WRITE, LEN_8) == B_BAD_VALUE,
		"a kernel watchpoint is accepted");

	// Only enabled breakpoints are checked, and the bits a Windows debug
	// context may hold besides the breakpoints are ignored.
	CHECK(set_debug_registers(KERNEL_ADDRESS, 1, 2, 3, DR7_LE | DR7_GE
			| DR7_BIT10) == B_OK, "disabled breakpoints are checked");
	CHECK(set_debug_registers(KERNEL_ADDRESS, target, 0, 0, DR7_LE | DR7_GE
			| DR7_BIT10 | DR7_GLOBAL(1)) == B_OK,
		"a global enable with LE, GE and bit 10 is refused");

	// A refused call leaves the breakpoints that were set.
	reset_traps();
	sCalls = 0;
	CHECK(set_debug_registers(0, 0, 0, 0, DR7_GD) == B_BAD_VALUE,
		"GD is accepted");
	target_function();
	CHECK(tTraps == 1 && (tDR6 & 0xf) == DR6_B(1), "the global breakpoint"
		" in slot 1: %d trap(s), DR6 %#" B_PRIx64, tTraps, tDR6);
	CHECK(clear_all() == B_OK, "clearing fails");
	target_function();
	CHECK(tTraps == 1 && sCalls == 2, "%d trap(s) and %d call(s) after the"
		" breakpoints were removed", tTraps, sCalls);
}


static void
test_instruction_breakpoint(void)
{
	printf("instruction breakpoint: SIGTRAP, and the instruction runs when the"
		" handler returns\n");

	const uint64 target = (uint64)(addr_t)&target_function;
	reset_traps();
	sCalls = 0;
	CHECK(set_one(0, target, RW_EXECUTE, LEN_1) == B_OK, "refused");
	for (int i = 0; i < 3; i++)
		target_function();
	CHECK(sCalls == 3 && tTraps == 3, "%d call(s), %d trap(s)", sCalls,
		tTraps);
	CHECK(tCode == TRAP_BRKPT, "si_code %d", tCode);
	CHECK(tAddress == (void*)&target_function, "si_addr %p, the function is"
		" at %p", tAddress, (void*)&target_function);
	CHECK((tDR6 & 0xf) == DR6_B(0), "DR6 %#" B_PRIx64, tDR6);
	CHECK(tTrapNumber == DEBUG_EXCEPTION, "trap number %d", tTrapNumber);
	CHECK((tFlags & EFLAGS_RF) != 0, "RF is not set in the context's flags"
		" (%#" B_PRIx64 ")", tFlags);
	CHECK(clear_all() == B_OK, "clearing fails");
}


static void
test_watchpoints(void)
{
	printf("watchpoints: each length, write and read/write\n");

	reset_traps();
	CHECK(set_debug_registers((uint64)(addr_t)&sWatched8,
			(uint64)(addr_t)&sWatched4, (uint64)(addr_t)&sWatched2,
			(uint64)(addr_t)&sWatched1,
			DR7_LOCAL(0) | DR7_RW(0, RW_WRITE) | DR7_LEN(0, LEN_8)
			| DR7_LOCAL(1) | DR7_RW(1, RW_READ_WRITE) | DR7_LEN(1, LEN_4)
			| DR7_LOCAL(2) | DR7_RW(2, RW_WRITE) | DR7_LEN(2, LEN_2)
			| DR7_LOCAL(3) | DR7_RW(3, RW_READ_WRITE) | DR7_LEN(3, LEN_1))
		== B_OK, "refused");

	// A read does not hit a write watchpoint.
	uint64 value = sWatched8;
	CHECK(tTraps == 0, "reading the 8-byte variable: %d trap(s)", tTraps);
	sWatched8 = value + 1;
	CHECK(tTraps == 1 && tTrapsBySlot[0] == 1 && tCode == TRAP_BRKPT,
		"writing the 8-byte variable: %d trap(s), DR6 %#" B_PRIx64, tTraps,
		tDR6);
	CHECK(sWatched8 == value + 1, "the write was lost");

	value = sWatched4;
	CHECK(tTraps == 2 && tTrapsBySlot[1] == 1, "reading the 4-byte variable:"
		" %d trap(s), DR6 %#" B_PRIx64, tTraps, tDR6);
	sWatched4 = (uint32)value + 1;
	CHECK(tTraps == 3 && tTrapsBySlot[1] == 2, "writing the 4-byte variable:"
		" %d trap(s), DR6 %#" B_PRIx64, tTraps, tDR6);

	sWatched2 = 2;
	CHECK(tTraps == 4 && tTrapsBySlot[2] == 1, "writing the 2-byte variable:"
		" %d trap(s), DR6 %#" B_PRIx64, tTraps, tDR6);

	value = sWatched1;
	CHECK(tTraps == 5 && tTrapsBySlot[3] == 1, "reading the 1-byte variable:"
		" %d trap(s), DR6 %#" B_PRIx64, tTraps, tDR6);

	CHECK(clear_all() == B_OK, "clearing fails");
	sWatched8 = 0;
	sWatched4 = 0;
	CHECK(tTraps == 5, "%d trap(s) after the watchpoints were removed",
		tTraps);
}


static void
test_kernel_access(void)
{
	printf("kernel access: the kernel reading and writing watched memory does"
		" not trap\n");

	int pipeFDs[2];
	CHECK(pipe(pipeFDs) == 0, "no pipe");

	reset_traps();
	CHECK(set_one(0, (uint64)(addr_t)&sWatched8, RW_READ_WRITE, LEN_8)
		== B_OK, "refused");

	// The kernel writes the variable, then reads it.
	_kern_generic_syscall(THREAD_SYSCALLS, THREAD_GET_GS_BASE,
		(void*)&sWatched8, sizeof(sWatched8));
	CHECK(write(pipeFDs[1], (const void*)&sWatched8, sizeof(sWatched8))
		== sizeof(sWatched8), "write() fails");
	CHECK(read(pipeFDs[0], (void*)&sWatched8, sizeof(sWatched8))
		== sizeof(sWatched8), "read() fails");
	CHECK(tTraps == 0, "%d trap(s)", tTraps);

	// The thread itself still does.
	uint64 value = sWatched8;
	CHECK(tTraps == 1, "%d trap(s) on a read of %#" B_PRIx64, tTraps, value);

	CHECK(clear_all() == B_OK, "clearing fails");
	close(pipeFDs[0]);
	close(pipeFDs[1]);
}


static void
test_signal_frames(void)
{
	printf("signal frames: only the breakpoint's SIGTRAP has DR6\n");

	reset_traps();
	tUsr1 = 0;
	tUsr1DR6 = ~0ull;
	CHECK(set_one(2, (uint64)(addr_t)&target_function, RW_EXECUTE, LEN_1)
		== B_OK, "refused");
	target_function();
	CHECK(tTraps == 1 && (tDR6 & 0xf) == DR6_B(2), "%d trap(s), DR6 %#"
		B_PRIx64, tTraps, tDR6);

	send_signal(find_thread(NULL), SIGUSR1);
	CHECK(tUsr1 == 1 && tUsr1DR6 == 0, "SIGUSR1 %d time(s), DR6 %#" B_PRIx64,
		tUsr1, tUsr1DR6);

	// A SIGTRAP that is not a breakpoint's has none either, even right after
	// one that is.
	target_function();
	send_signal(find_thread(NULL), SIGTRAP);
	CHECK(tTraps == 3 && tDR6 == 0 && tCode != TRAP_BRKPT, "%d trap(s), DR6 %#"
		B_PRIx64 ", si_code %d", tTraps, tDR6, tCode);

	CHECK(clear_all() == B_OK, "clearing fails");
}


static void
test_team_breakpoints(void)
{
	printf("team breakpoints: setting and clearing one keeps the thread's"
		" own\n");

	reset_traps();
	sCalls = 0;
	CHECK(set_one(0, (uint64)(addr_t)&target_function, RW_EXECUTE, LEN_1)
		== B_OK, "refused");

	// The team breakpoint is never hit: the function is not called.
	status_t status = _kern_set_debugger_breakpoint(
		(void*)&never_called_function, 0, 0, false);
	CHECK(status == B_OK, "set_debugger_breakpoint() fails: %s",
		strerror(status));
	target_function();
	CHECK(tTraps == 1, "%d trap(s) while a team breakpoint is set", tTraps);

	status = _kern_clear_debugger_breakpoint((void*)&never_called_function,
		false);
	CHECK(status == B_OK, "clear_debugger_breakpoint() fails: %s",
		strerror(status));
	target_function();
	CHECK(tTraps == 2 && sCalls == 2, "%d trap(s) after the team breakpoint"
		" was cleared", tTraps);

	CHECK(clear_all() == B_OK, "clearing fails");
}


struct thread_result {
	int index;
	int traps;
	int bySlot[4];
	status_t status;
};


static status_t
breakpoint_thread(void* data)
{
	struct thread_result* result = (struct thread_result*)data;
	const int instructionSlot = result->index % 4;
	const int watchSlot = (result->index + 1) % 4;

	uint64 addresses[4] = {};
	addresses[instructionSlot] = (uint64)(addr_t)&target_function;
	addresses[watchSlot]
		= (uint64)(addr_t)&sThreadVariables[result->index];
	result->status = set_debug_registers(addresses[0], addresses[1],
		addresses[2], addresses[3],
		DR7_LOCAL(instructionSlot) | DR7_LOCAL(watchSlot)
			| DR7_RW(watchSlot, RW_WRITE) | DR7_LEN(watchSlot, LEN_8));

	for (int round = 0; round < THREAD_ROUNDS; round++) {
		target_function();
		sThreadVariables[result->index]++;
		if (round % 64 == 0)
			snooze(1);
	}

	result->traps = tTraps;
	for (int slot = 0; slot < 4; slot++)
		result->bySlot[slot] = tTrapsBySlot[slot];
	return B_OK;
}


static void
test_threads(int32 cpuCount)
{
	int count = cpuCount * 2;
	if (count > MAX_THREADS)
		count = MAX_THREADS;
	if (count < 4)
		count = 4;
	printf("threads: %d threads with breakpoints of their own, %d rounds\n",
		count, THREAD_ROUNDS);

	struct thread_result results[MAX_THREADS] = {};
	thread_id threads[MAX_THREADS];
	for (int i = 0; i < count; i++) {
		results[i].index = i;
		threads[i] = spawn_thread(&breakpoint_thread, "breakpoints",
			B_NORMAL_PRIORITY, &results[i]);
		resume_thread(threads[i]);
	}

	// The main thread has none: it neither stops at the function nor at the
	// threads' variables.
	reset_traps();
	for (int round = 0; round < THREAD_ROUNDS; round++) {
		target_function();
		(void)sThreadVariables[round % count];
	}
	CHECK(tTraps == 0, "the main thread got %d trap(s)", tTraps);

	for (int i = 0; i < count; i++) {
		status_t exitValue;
		wait_for_thread(threads[i], &exitValue);
		const int instructionSlot = i % 4;
		const int watchSlot = (i + 1) % 4;
		CHECK(results[i].status == B_OK && results[i].traps == 2 * THREAD_ROUNDS
			&& results[i].bySlot[instructionSlot] == THREAD_ROUNDS
			&& results[i].bySlot[watchSlot] == THREAD_ROUNDS,
			"thread %d: status %#" B_PRIx32 ", %d trap(s), %d in slot %d, %d in"
			" slot %d", i, results[i].status, results[i].traps,
			results[i].bySlot[instructionSlot], instructionSlot,
			results[i].bySlot[watchSlot], watchSlot);
		CHECK(sThreadVariables[i] == THREAD_ROUNDS, "thread %d: its variable"
			" is %" B_PRIu64, i, sThreadVariables[i]);
	}
}


static void
test_fork(void)
{
	printf("fork: the child has no breakpoints, the parent keeps its own\n");

	reset_traps();
	CHECK(set_one(0, (uint64)(addr_t)&target_function, RW_EXECUTE, LEN_1)
		== B_OK, "refused");

	pid_t child = fork();
	if (child == 0) {
		target_function();
		_exit(tTraps == 0 ? 0 : 1);
	}

	int status = 0;
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the child reported"
		" %d (1: it hit the parent's breakpoint)", WEXITSTATUS(status));

	target_function();
	CHECK(tTraps == 1, "%d trap(s) in the parent", tTraps);
	CHECK(clear_all() == B_OK, "clearing fails");
}


static void
test_exec(const char* self)
{
	printf("exec: the new image has no breakpoints\n");

	pid_t child = fork();
	if (child == 0) {
		reset_traps();
		volatile uint64* watched = (volatile uint64*)create_fixed_area(0);
		if (watched == NULL)
			_exit(3);
		if (set_one(0, (uint64)(addr_t)watched, RW_READ_WRITE, LEN_8) != B_OK)
			_exit(4);

		char address[32];
		snprintf(address, sizeof(address), "%p", (void*)watched);
		*watched = 1;
		if (tTraps != 1)
			_exit(5);

		execl(self, self, "--exec-child", address, (char*)NULL);
		_exit(6);
	}

	int status = 0;
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the child reported"
		" %d (1: the new image hit the old one's watchpoint, 2: no area there,"
		" 3-6: the old image could not prepare)", WEXITSTATUS(status));
}


static int
exec_child(const char* addressString)
{
	install_handler(SIGTRAP, &trap_handler);

	volatile uint64* watched = (volatile uint64*)create_fixed_area(
		(addr_t)strtoull(addressString, NULL, 0));
	if (watched == NULL)
		return 2;

	*watched = 2;
	return tTraps == 0 ? 0 : 1;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc > 2 && strcmp(argv[1], "--exec-child") == 0)
		return exec_child(argv[2]);

	if (clear_all() != B_OK) {
		printf("This kernel lacks THREAD_SET_DEBUG_REGISTERS; nothing to"
			" test.\n");
		return 1;
	}

	install_handler(SIGTRAP, &trap_handler);
	install_handler(SIGUSR1, &usr1_handler);

	system_info info;
	get_system_info(&info);

	test_calls();
	test_instruction_breakpoint();
	test_watchpoints();
	test_kernel_access();
	test_signal_frames();
	test_team_breakpoints();
	test_threads(info.cpu_count);
	test_fork();
	test_exec(argv[0]);

	if (sFailures != 0) {
		printf("%d check(s) failed\n", sFailures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
