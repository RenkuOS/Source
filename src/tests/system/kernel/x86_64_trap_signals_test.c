/*
 * Copyright 2026, rations.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	x86_64_trap_signals_test: SIGTRAP for int3 and single steps on x86_64.

	Without a user debugger, a thread with a SIGTRAP handler gets the signal
	for an int3 instruction and for a single step with the trap flag it set
	itself; the trap number in the signal context says which. Without a
	handler, or with a debugger installed, the debugger gets them as before.
	Wine needs the signal to give Windows programs EXCEPTION_BREAKPOINT and
	EXCEPTION_SINGLE_STEP.

	Run without arguments.
*/


#include <OS.h>
#include <debugger.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <arch_thread_defs.h>
#include <syscalls.h>
#include <user_thread.h>


#define EFLAGS_TF			(1ull << 8)
#define DR6_BS				(1ull << 14)

#define DEBUG_EXCEPTION		1
#define BREAKPOINT_EXCEPTION	3

#define MAX_THREADS			32
#define MAX_RECORDED		16
#define LOOP_ROUNDS			10000


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


// The code that traps. Each label is the address a trap reports.
extern void int3_sequence(void);
extern char int3_short_end[], int3_long_end[];
extern void int3_then_nop(void);
extern char int3_then_nop_next[];
extern int step_sequence(void);
extern char step_1[], step_2[], step_3[], step_4[], step_end[];
extern thread_id step_call(thread_id (*function)(const char*));
extern char step_call_end[];
extern void step_loop(int rounds);
extern char step_loop_end[];

__asm__(
	".text\n"

	// int3, and int $3, the two-byte form
	".globl int3_sequence, int3_short_end, int3_long_end\n"
	"int3_sequence:\n"
	"	int3\n"
	"int3_short_end:\n"
	"	.byte 0xcd, 0x03\n"
	"int3_long_end:\n"
	"	ret\n"

	// int3, then an instruction for a breakpoint
	".globl int3_then_nop, int3_then_nop_next\n"
	"int3_then_nop:\n"
	"	int3\n"
	"int3_then_nop_next:\n"
	"	nop\n"
	"	ret\n"

	// The trap flag set with popfq: the first trap comes after the
	// instruction that follows, then one after each instruction.
	".globl step_sequence, step_1, step_2, step_3, step_4, step_end\n"
	"step_sequence:\n"
	"	pushfq\n"
	"	orq $0x100, (%rsp)\n"
	"	popfq\n"
	"	nop\n"
	"step_1:\n"
	"	movl $1, %eax\n"
	"step_2:\n"
	"	addl $2, %eax\n"
	"step_3:\n"
	"	leal 3(%rax), %eax\n"
	"step_4:\n"
	"	imull $7, %eax, %eax\n"
	"step_end:\n"
	"	ret\n"

	// Single steps through a call that makes a syscall.
	".globl step_call, step_call_end\n"
	"step_call:\n"
	"	pushq %rbx\n"
	"	movq %rdi, %rbx\n"
	"	xorl %edi, %edi\n"
	"	pushfq\n"
	"	orq $0x100, (%rsp)\n"
	"	popfq\n"
	"	call *%rbx\n"
	"step_call_end:\n"
	"	popq %rbx\n"
	"	ret\n"

	// 1 + 2 * rounds single steps.
	".globl step_loop, step_loop_end\n"
	"step_loop:\n"
	"	movl %edi, %ecx\n"
	"	pushfq\n"
	"	orq $0x100, (%rsp)\n"
	"	popfq\n"
	"	nop\n"
	"1:\n"
	"	decl %ecx\n"
	"	jnz 1b\n"
	"step_loop_end:\n"
	"	ret\n"
);


// What the SIGTRAP handler saw, per thread. Volatile, so that the compiler
// does not read them before the code that traps.
static __thread volatile int tTraps;
static __thread volatile int tCode;
static __thread void* volatile tAddress;
static __thread volatile uint64 tIP;
static __thread volatile uint64 tDR6;
static __thread volatile int tTrapNumber;
static __thread volatile uint64 tHandlerFlags;
static __thread volatile uint64 tRecorded[MAX_RECORDED];
static __thread volatile int tAfterSyscall;
static __thread void* volatile tStopAt;
	// where the handler clears the trap flag in the context


static void
trap_handler(int signal, siginfo_t* info, void* _context)
{
	ucontext_t* context = (ucontext_t*)_context;
	uint64 flags;
	__asm__ volatile("pushfq; popq %0" : "=r"(flags));
	tHandlerFlags |= flags;

	const uint64 ip = context->uc_mcontext.rip;
	if (tTraps < MAX_RECORDED)
		tRecorded[tTraps] = ip;
	tTraps++;
	tCode = info->si_code;
	tAddress = info->si_addr;
	tIP = ip;
	tDR6 = context->uc_mcontext.fpu.fp_fxsave.dr6;
	tTrapNumber = context->uc_mcontext.fpu.fp_fxsave.trap_number;

	// right after a syscall instruction (0f 05)
	const uint8* code = (const uint8*)(addr_t)ip;
	if (code[-2] == 0x0f && code[-1] == 0x05)
		tAfterSyscall++;

	if ((void*)(addr_t)ip == tStopAt)
		context->uc_mcontext.rflags &= ~EFLAGS_TF;
}


static void
install_handler(void)
{
	struct sigaction action = {};
	action.sa_sigaction = &trap_handler;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	sigaction(SIGTRAP, &action, NULL);
}


static void
reset_traps(void* stopAt)
{
	tTraps = 0;
	tCode = 0;
	tAddress = NULL;
	tIP = 0;
	tDR6 = 0;
	tTrapNumber = 0;
	tHandlerFlags = 0;
	tAfterSyscall = 0;
	for (int i = 0; i < MAX_RECORDED; i++)
		tRecorded[i] = 0;
	tStopAt = stopAt;
}


/*!	Sets the calling thread's own instruction breakpoint in DR0 at \a address,
	or removes its breakpoints if \a address is 0.
*/
static status_t
set_instruction_breakpoint(uint64 address)
{
	struct thread_debug_registers registers = {
		{ address, 0, 0, 0 }, address != 0 ? 1 : 0
	};
	return _kern_generic_syscall(THREAD_SYSCALLS, THREAD_SET_DEBUG_REGISTERS,
		&registers, sizeof(registers));
}


/*!	Waits up to \a seconds for \a child; kills it if it is still there.
	\return Whether it ended by itself.
*/
static bool
wait_for_child(pid_t child, int seconds, int* _status)
{
	for (int i = 0; i < seconds * 10; i++) {
		if (waitpid(child, _status, WNOHANG) == child)
			return true;
		usleep(100000);
	}
	kill(child, SIGKILL);
	waitpid(child, _status, 0);
	return false;
}


/*!	An int3 in a child with a handler. A kernel that does not send SIGTRAP
	gives it to the debug_server, which stops it.
*/
static bool
kernel_sends_sigtrap(void)
{
	pid_t child = fork();
	if (child == 0) {
		reset_traps(NULL);
		int3_sequence();
		_exit(tTraps == 2 ? 0 : 1);
	}

	int status = 0;
	return wait_for_child(child, 10, &status) && WIFEXITED(status)
		&& WEXITSTATUS(status) == 0;
}


//	#pragma mark - tests


static void
test_int3(void)
{
	printf("int3: SIGTRAP after the instruction, which the thread continues"
		" from\n");

	reset_traps(NULL);
	int3_sequence();
	CHECK(tTraps == 2, "%d trap(s)", tTraps);
	CHECK(tRecorded[0] == (uint64)(addr_t)int3_short_end
			&& tRecorded[1] == (uint64)(addr_t)int3_long_end,
		"the context's rip is %#" B_PRIx64 " and %#" B_PRIx64 ", the"
		" instructions end at %p and %p", tRecorded[0], tRecorded[1],
		int3_short_end, int3_long_end);
	CHECK(tCode == TRAP_BRKPT, "si_code %d", tCode);
	CHECK(tAddress == (void*)int3_long_end, "si_addr %p, expected %p",
		tAddress, int3_long_end);
	CHECK(tTrapNumber == BREAKPOINT_EXCEPTION, "trap number %d",
		tTrapNumber);
	CHECK(tDR6 == 0, "DR6 %#" B_PRIx64, tDR6);
}


/*!	An int3 is a trap: its signal context's rip is the next instruction
	already. Its return must not carry RF, which would make the CPU skip an
	instruction breakpoint there.
*/
static void
test_int3_breakpoint(void)
{
	printf("int3: an instruction breakpoint right after it still hits\n");

	const uint64 next = (uint64)(addr_t)int3_then_nop_next;
	status_t status = set_instruction_breakpoint(next);
	CHECK(status == B_OK, "THREAD_SET_DEBUG_REGISTERS: %s", strerror(status));
	if (status != B_OK)
		return;

	reset_traps(NULL);
	int3_then_nop();
	const int traps = tTraps;
	const int trapNumber = tTrapNumber;
	const uint64 dr6 = tDR6;
	set_instruction_breakpoint(0);

	CHECK(traps == 2, "%d trap(s), expected the int3's and the breakpoint's",
		traps);
	CHECK(tRecorded[0] == next && tRecorded[1] == next, "the traps are at %#"
		B_PRIx64 " and %#" B_PRIx64 ", expected %p", tRecorded[0],
		tRecorded[1], int3_then_nop_next);
	CHECK(trapNumber == DEBUG_EXCEPTION && (dr6 & 1) != 0, "the last trap"
		" has trap number %d and DR6 %#" B_PRIx64, trapNumber, dr6);
}


static void
test_single_step(void)
{
	printf("single step: SIGTRAP after each instruction while the handler"
		" keeps the trap flag\n");

	reset_traps(step_end);
	const int result = step_sequence();
	CHECK(result == 42, "the stepped code computed %d", result);

	const char* expected[] = { step_1, step_2, step_3, step_4, step_end };
	const int count = sizeof(expected) / sizeof(expected[0]);
	CHECK(tTraps == count, "%d trap(s), expected %d", tTraps, count);
	for (int i = 0; i < count && i < tTraps; i++) {
		CHECK(tRecorded[i] == (uint64)(addr_t)expected[i], "trap %d at %#"
			B_PRIx64 ", expected %p", i, tRecorded[i], expected[i]);
	}
	CHECK(tCode == TRAP_TRACE, "si_code %d", tCode);
	CHECK(tAddress == (void*)step_end, "si_addr %p, expected %p", tAddress,
		step_end);
	CHECK(tTrapNumber == DEBUG_EXCEPTION, "trap number %d", tTrapNumber);
	CHECK((tDR6 & DR6_BS) != 0 && (tDR6 & 0xf) == 0, "DR6 %#" B_PRIx64,
		tDR6);
	CHECK((tHandlerFlags & EFLAGS_TF) == 0, "the handler ran with the trap"
		" flag set");

	// Cleared in the context, it stays clear.
	const int traps = tTraps;
	step_sequence();
	CHECK(tTraps == traps + count, "%d trap(s) the second time",
		tTraps - traps);
	int sum = 0;
	for (int i = 0; i < 100; i++)
		sum += i;
	CHECK(tTraps == traps + count && sum == 4950, "%d trap(s) after the"
		" handler cleared the trap flag", tTraps - traps - count);
}


static void
test_step_syscall(void)
{
	printf("single step through a syscall\n");

	// The trap flag outlasts the syscall: stepping reaches the end.
	reset_traps(step_call_end);
	const thread_id result = step_call(&_kern_find_thread);
	const int traps = tTraps;
	CHECK(result == find_thread(NULL), "the call returned %" B_PRId32
		" instead of %" B_PRId32, result, find_thread(NULL));
	CHECK(tIP == (uint64)(addr_t)step_call_end, "the last trap is at %#"
		B_PRIx64 ", not after the call (%p)", tIP, step_call_end);
	CHECK(traps > 2 && traps < 100, "%d trap(s)", traps);
	printf("  %d single steps, %d of them right after the syscall"
		" instruction\n", traps, tAfterSyscall);
}


/*!	While libroot defers signals, in malloc() or the runtime loader for one,
	SIGTRAP must still come at once: deferred, it would come later, at another
	instruction.
*/
static void
test_deferred(void)
{
	printf("deferred signals: SIGTRAP still comes at the int3\n");

	reset_traps(NULL);
	defer_signals();
	int3_sequence();
	const int traps = tTraps;
	const uint64 first = tRecorded[0];
	undefer_signals();
	CHECK(traps == 2, "%d trap(s) while signals were deferred", traps);
	CHECK(first == (uint64)(addr_t)int3_short_end, "the first trap is at %#"
		B_PRIx64 ", the int3 ends at %p", first, int3_short_end);
	CHECK(tTraps == 2, "%d trap(s) in all", tTraps);
}


/*!	A child forked while the parent single-steps does not inherit the trap
	flag. The child is forked with the syscall itself, and only exits. It
	starts with signals deferred, as fork() needs, so a single step would
	not show as a trap at once: it exits with 100 if the trap flag is set,
	plus its trap count, which it starts with the parent's at the syscall.
*/
static void
test_fork(void)
{
	printf("fork: the child of a single-stepping thread does not step\n");

	reset_traps(step_call_end);
	const thread_id child = step_call((thread_id (*)(const char*))&_kern_fork);
	if (child == 0) {
		uint64 flags;
		__asm__ volatile("pushfq; popq %0" : "=r"(flags));
		_kern_exit_team(((flags & EFLAGS_TF) != 0 ? 100 : 0) + tTraps);
	}

	// the parent's trap right after the syscall instruction
	int atFork = -1;
	for (int i = 0; i < tTraps && i < MAX_RECORDED; i++) {
		const uint8* code = (const uint8*)(addr_t)tRecorded[i];
		if (code[-2] == 0x0f && code[-1] == 0x05) {
			atFork = i;
			break;
		}
	}
	CHECK(child > 0, "fork: %s", strerror(child));
	CHECK(atFork > 0, "the parent was not stepped through the syscall");
	if (child <= 0 || atFork <= 0)
		return;

	int status = 0;
	const bool ended = wait_for_child(child, 10, &status);
	CHECK(ended && WIFEXITED(status) && WEXITSTATUS(status) == atFork,
		"the child %s with status %#x; it exits with 100 for the trap flag"
		" plus its trap count, which is %d without traps of its own",
		ended ? "ended" : "was stopped", status, atFork);
}


struct thread_result {
	int		traps;
	int		stoppedAtEnd;
};


static status_t
step_thread(void* _result)
{
	struct thread_result* result = (struct thread_result*)_result;
	reset_traps(step_loop_end);
	step_loop(LOOP_ROUNDS);
	result->traps = tTraps;
	result->stoppedAtEnd = tIP == (uint64)(addr_t)step_loop_end;
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
	printf("threads: %d threads single-stepping %d instructions each\n",
		count, 1 + 2 * LOOP_ROUNDS);

	struct thread_result results[MAX_THREADS] = {};
	thread_id threads[MAX_THREADS];
	for (int i = 0; i < count; i++) {
		threads[i] = spawn_thread(&step_thread, "single steps",
			B_NORMAL_PRIORITY, &results[i]);
		resume_thread(threads[i]);
	}

	// The main thread gets none of theirs.
	reset_traps(NULL);
	for (int i = 0; i < count; i++) {
		status_t exitValue;
		wait_for_thread(threads[i], &exitValue);
		CHECK(results[i].traps == 1 + 2 * LOOP_ROUNDS
				&& results[i].stoppedAtEnd,
			"thread %d: %d trap(s), %s at the end", i, results[i].traps,
			results[i].stoppedAtEnd ? "stopped" : "not stopped");
	}
	CHECK(tTraps == 0, "the main thread got %d trap(s)", tTraps);
}


static void
test_blocked(void)
{
	printf("blocked: SIGTRAP for an int3 kills the team\n");

	pid_t child = fork();
	if (child == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigaddset(&set, SIGTRAP);
		sigprocmask(SIG_BLOCK, &set, NULL);
		int3_sequence();
		_exit(0);
	}

	int status = 0;
	const bool ended = wait_for_child(child, 10, &status);
	CHECK(ended && WIFSIGNALED(status) && WTERMSIG(status) == SIGTRAP,
		"the child %s, status %#x", ended ? "ended" : "was stopped", status);
}


static void
test_debugger(void)
{
	printf("debugger: with one installed, it gets the int3 and the handler"
		" does not\n");

	int pipes[2];
	if (pipe(pipes) != 0) {
		CHECK(false, "pipe() fails");
		return;
	}

	pid_t child = fork();
	if (child == 0) {
		close(pipes[1]);
		char go;
		if (read(pipes[0], &go, 1) != 1)
			_exit(10);
		reset_traps(NULL);
		int3_sequence();
		_exit(tTraps);
	}
	close(pipes[0]);

	port_id port = create_port(10, "trap signals test debugger");
	CHECK(port >= 0, "create_port() fails");
	port_id nubPort = install_team_debugger(child, port);
	CHECK(nubPort >= 0, "install_team_debugger() fails: %s",
		strerror(nubPort));
	write(pipes[1], "g", 1);
	close(pipes[1]);

	int32 code = 0;
	debug_debugger_message_data message;
	ssize_t size = read_port_etc(port, &code, &message, sizeof(message),
		B_RELATIVE_TIMEOUT, 10000000);
	CHECK(size >= 0 && code == B_DEBUGGER_MESSAGE_BREAKPOINT_HIT,
		"the debugger got message %" B_PRId32 " (read_port_etc(): %s)", code,
		size >= 0 ? "ok" : strerror(size));
	if (size >= 0 && code == B_DEBUGGER_MESSAGE_BREAKPOINT_HIT) {
		CHECK(message.breakpoint_hit.cpu_state.rip
				== (uint64)(addr_t)&int3_sequence,
			"the debugger sees rip %#" B_PRIx64 ", the int3 is at %p",
			message.breakpoint_hit.cpu_state.rip, (void*)&int3_sequence);
	}

	// Without the debugger, the thread runs the int3 again and the handler
	// gets both.
	remove_team_debugger(child);
	delete_port(port);

	int status = 0;
	const bool ended = wait_for_child(child, 10, &status);
	CHECK(ended && WIFEXITED(status) && WEXITSTATUS(status) == 2,
		"the child %s, status %#x (it exits with the number of traps its"
		" handler got)", ended ? "ended" : "was stopped", status);
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	install_handler();

	if (!kernel_sends_sigtrap()) {
		printf("This kernel does not send SIGTRAP for int3; nothing to"
			" test.\n");
		return 1;
	}

	system_info info;
	get_system_info(&info);

	test_int3();
	test_int3_breakpoint();
	test_single_step();
	test_step_syscall();
	test_deferred();
	test_fork();
	test_threads(info.cpu_count);
	test_blocked();
	test_debugger();

	if (sFailures != 0) {
		printf("%d check(s) failed\n", sFailures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
