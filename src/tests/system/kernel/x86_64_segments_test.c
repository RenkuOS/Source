/*
 * Copyright 2026, rations.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	x86_64_segments_test: the user segment state a thread keeps on x86_64.

	A thread's FS base set with THREAD_SET_FS_BASE, and FS, DS and ES selectors
	it loaded itself, must stay its own across context switches, signals,
	sigreturn and exec(). 32-bit code needs that: Wine, for one, runs 32-bit
	Windows programs with their TEB in FS, a segment of the team's local
	descriptor table (THREAD_SET_LDT_ENTRY). The LDT tests run 32-bit code
	that reads through such segments while it is preempted.

	While a thread runs with another FS than its TLS, it uses no libroot
	function that could reach the TLS: only raw syscalls.

	Run without arguments. "x86_64_segments_test --exec-child <selector>" is
	the half of the exec() test that runs in the new image.
*/


#include <OS.h>

#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <arch_thread_defs.h>
#include <libroot_private.h>
#include <signal_defs.h>
#include <syscalls.h>


// GDT selectors, from headers/private/kernel/arch/x86/64/descriptors.h
#define USER_DATA_SELECTOR	0x23
#define KERNEL_DATA_SELECTOR	0x10

#define THREAD_COUNT		4
#define SWITCH_ROUNDS		20000
#define NULL_FS_ROUNDS		1000
#define SIGNAL_ROUNDS		1000

// LDT entries the tests use, and their selectors (table bit and RPL 3)
#define LDT_FLAT			0
#define LDT_FIRST			1
#define LDT_THREAD(i)		(2 + (i))
#define LDT_CLEARED			50
#define LDT_UNSET			100
#define LDT_LAST			(THREAD_LDT_ENTRY_COUNT - 1)
#define LDT_SELECTOR(index)	((uint16)(((index) << 3) | 7))

#define MAX_COMPAT_THREADS	32
#define EXIT_ROUNDS			100
#define COMPAT_RUNS			200
#define COMPAT_ROUNDS		1000000


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


static status_t
thread_call(uint32 function, uint64* value)
{
	return _kern_generic_syscall(THREAD_SYSCALLS, function, value,
		sizeof(*value));
}


static status_t
set_fs_base(uint64 base)
{
	return thread_call(THREAD_SET_FS_BASE, &base);
}


static inline uint16
read_fs(void)
{
	uint16 selector;
	__asm__ volatile("movw %%fs, %0" : "=r" (selector));
	return selector;
}


static inline uint16
read_ds(void)
{
	uint16 selector;
	__asm__ volatile("movw %%ds, %0" : "=r" (selector));
	return selector;
}


static inline uint16
read_es(void)
{
	uint16 selector;
	__asm__ volatile("movw %%es, %0" : "=r" (selector));
	return selector;
}


static inline void
load_fs(uint16 selector)
{
	__asm__ volatile("movw %0, %%fs" : : "r" (selector));
}


static inline void
load_ds_es(uint16 selector)
{
	__asm__ volatile("movw %0, %%ds; movw %0, %%es" : : "r" (selector));
}


static inline uint64
read_fs_qword(uint64 offset)
{
	uint64 value;
	__asm__ volatile("movq %%fs:(%1), %0" : "=r" (value) : "r" (offset));
	return value;
}


//	#pragma mark - the calls


static void
test_calls(void)
{
	printf("calls: THREAD_SET_FS_BASE, THREAD_GET_FS_BASE, THREAD_GET_GS_BASE\n");

	uint64 base = 0;
	status_t status = thread_call(THREAD_GET_FS_BASE, &base);
	CHECK(status == B_OK, "THREAD_GET_FS_BASE: %s", strerror(status));
	if (status != B_OK)
		return;
	CHECK(base != 0, "the FS base of a thread is its TLS, not 0");

	static uint64 page[512] __attribute__((aligned(4096)));
	page[0] = 0x5a5a1234;
	CHECK(set_fs_base((uint64)(addr_t)page) == B_OK, "THREAD_SET_FS_BASE");
	uint64 value = read_fs_qword(0);
	uint64 got = 0;
	thread_call(THREAD_GET_FS_BASE, &got);
	set_fs_base(0);
	CHECK(value == page[0], "%%fs:0 read %#" B_PRIx64, value);
	CHECK(got == (uint64)(addr_t)page, "THREAD_GET_FS_BASE gave %#" B_PRIx64,
		got);

	thread_call(THREAD_GET_FS_BASE, &got);
	CHECK(got == base, "THREAD_SET_FS_BASE(0) gave %#" B_PRIx64 ", not the TLS"
		" %#" B_PRIx64, got, base);
	CHECK(find_thread(NULL) > 0, "the TLS works again");

	// Not a user address, so not canonical in every case: refused.
	CHECK(set_fs_base(0x800000000000ull) == B_BAD_VALUE,
		"an FS base outside userland is refused");

	uint64 gsBase = 0x12340000;
	CHECK(thread_call(THREAD_SET_GS_BASE, &gsBase) == B_OK,
		"THREAD_SET_GS_BASE");
	gsBase = 0;
	CHECK(thread_call(THREAD_GET_GS_BASE, &gsBase) == B_OK
		&& gsBase == 0x12340000, "THREAD_GET_GS_BASE gave %#" B_PRIx64, gsBase);
	gsBase = 0;
	thread_call(THREAD_SET_GS_BASE, &gsBase);
}


//	#pragma mark - context switches


struct switch_args {
	uint64	page[512] __attribute__((aligned(4096)));
	int		mode;
	int32	mismatches;
};


enum {
	MODE_FS_BASE,
	MODE_FS_SELECTOR,
};


/*!	Keeps a segment state of its own and checks it after every yield: in
	MODE_FS_BASE an FS base of its own, in MODE_FS_SELECTOR the flat user data
	segment in FS, DS and ES (base 0, so %fs:x reads x).
*/
static status_t
switch_worker(void* data)
{
	struct switch_args* args = (struct switch_args*)data;
	int32 mismatches = 0;

	if (args->mode == MODE_FS_BASE)
		set_fs_base((uint64)(addr_t)args->page);
	else {
		load_ds_es(USER_DATA_SELECTOR);
		load_fs(USER_DATA_SELECTOR);
	}

	for (int i = 0; i < SWITCH_ROUNDS; i++) {
		_kern_thread_yield();
		if (args->mode == MODE_FS_BASE) {
			if (read_fs_qword(0) != args->page[0] || read_fs() != 0)
				mismatches++;
		} else {
			if (read_fs() != USER_DATA_SELECTOR
				|| read_ds() != USER_DATA_SELECTOR
				|| read_es() != USER_DATA_SELECTOR
				|| read_fs_qword((uint64)(addr_t)args->page)
					!= args->page[0]) {
				mismatches++;
			}
		}
	}

	load_ds_es(0);
	set_fs_base(0);
	args->mismatches = mismatches;
	return B_OK;
}


static void
test_context_switches(int mode)
{
	printf("context switches: %s\n", mode == MODE_FS_BASE
		? "each thread its own FS base" : "FS, DS and ES selectors");

	static struct switch_args args[THREAD_COUNT];
	thread_id threads[THREAD_COUNT];
	for (int i = 0; i < THREAD_COUNT; i++) {
		args[i].page[0] = 0x1000 + i;
		args[i].mode = mode;
		args[i].mismatches = -1;
		threads[i] = spawn_thread(switch_worker, "segments worker",
			B_NORMAL_PRIORITY, &args[i]);
		CHECK(threads[i] >= 0, "spawn_thread");
	}
	for (int i = 0; i < THREAD_COUNT; i++)
		resume_thread(threads[i]);

	for (int i = 0; i < THREAD_COUNT; i++) {
		status_t result;
		wait_for_thread(threads[i], &result);
		CHECK(args[i].mismatches == 0, "thread %d: %" B_PRId32 " of %d rounds"
			" saw another segment state", i, args[i].mismatches,
			SWITCH_ROUNDS);
	}
}


//	#pragma mark - the null selector 3


struct null_fs_args {
	uint64		page[512] __attribute__((aligned(4096)));
	sem_id		sem;
	thread_id	thread;
	int32		mismatches;
};


static int32 sNullFsFaults;


/*!	Counts a read of %fs:0 by null_fs_worker() that faulted: FS was 0, but its
	base was not the thread's. Returning with FS 0 in the signal context makes
	the kernel load the thread's own FS base, so the read is done again and
	succeeds. The thread's FS base is not its TLS, so nothing here may use the
	TLS.
*/
static void
null_fs_fault_handler(int number, siginfo_t* info, void* _context)
{
	ucontext_t* context = (ucontext_t*)_context;
	if (atomic_add(&sNullFsFaults, 1) >= THREAD_COUNT * NULL_FS_ROUNDS) {
		static const char message[] = "  FAIL: faults go on in the null FS"
			" test\n";
		write(STDOUT_FILENO, message, sizeof(message) - 1);
		_exit(4);
	}
	context->uc_mcontext.fpu.fp_fxsave.fs = 0;
}


/*!	Has an FS base of its own, loads the null selector 3 into FS and waits on
	its semaphore, which test_null_fs() releases only after the thread has
	waited for a while: the thread has certainly been switched out with FS 3.
	The kernel keeps any null selector as 0, so the thread must come back with
	FS 0 and its own FS base.

	Loading 3 clears the base on Intel CPUs, while AMD CPUs keep the base of
	whatever FS held, so the base is read only once FS is 0 again. A thread
	that keeps FS 3 without being switched out has been seen to get FS 0 with
	that cleared base, without the kernel, so only switches are tested. A
	kernel that loses the base makes the read fault: null_fs_fault_handler()
	counts it.
*/
static status_t
null_fs_worker(void* data)
{
	struct null_fs_args* args = (struct null_fs_args*)data;
	int32 mismatches = 0;

	set_fs_base((uint64)(addr_t)args->page);
	for (int i = 0; i < NULL_FS_ROUNDS; i++) {
		load_fs(3);
		_kern_acquire_sem(args->sem);
		if (read_fs() != 0 || read_fs_qword(0) != args->page[0])
			mismatches++;
	}

	set_fs_base(0);
	args->mismatches = mismatches;
	return B_OK;
}


//!	Returns whether \a thread waits on \a sem, within 5 seconds.
static bool
wait_until_waiting(thread_id thread, sem_id sem)
{
	bigtime_t timeout = system_time() + 5000000;
	while (system_time() < timeout) {
		thread_info info;
		if (get_thread_info(thread, &info) != B_OK)
			return false;
		if (info.state == B_THREAD_WAITING && info.sem == sem)
			return true;
		snooze(20);
	}
	return false;
}


static void
test_null_fs(void)
{
	printf("context switches: the null selector 3 in FS\n");

	struct sigaction action, oldAction;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = null_fs_fault_handler;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &action, &oldAction);
	sNullFsFaults = 0;

	static struct null_fs_args args[THREAD_COUNT];
	for (int i = 0; i < THREAD_COUNT; i++) {
		args[i].page[0] = 0x2000 + i;
		args[i].sem = create_sem(0, "segments null fs");
		args[i].mismatches = -1;
		args[i].thread = spawn_thread(null_fs_worker, "segments null fs",
			B_NORMAL_PRIORITY, &args[i]);
		CHECK(args[i].sem >= 0 && args[i].thread >= 0, "create_sem,"
			" spawn_thread");
		resume_thread(args[i].thread);
	}

	// A thread seen waiting may still be on its way out of the CPU; one that
	// has waited for a millisecond is not.
	bool waiting = true;
	for (int round = 0; round < NULL_FS_ROUNDS && waiting; round++) {
		for (int i = 0; i < THREAD_COUNT && waiting; i++)
			waiting = wait_until_waiting(args[i].thread, args[i].sem);
		snooze(1000);
		for (int i = 0; i < THREAD_COUNT; i++)
			release_sem(args[i].sem);
	}
	CHECK(waiting, "a thread did not wait on its semaphore");

	// Deleting a semaphore lets a thread that waits on it go on.
	for (int i = 0; i < THREAD_COUNT; i++) {
		delete_sem(args[i].sem);
		status_t result;
		wait_for_thread(args[i].thread, &result);
		CHECK(args[i].mismatches == 0, "thread %d: %" B_PRId32 " of %d rounds"
			" came back with another FS or FS base", i, args[i].mismatches,
			NULL_FS_ROUNDS);
	}

	sigaction(SIGSEGV, &oldAction, NULL);
	CHECK(sNullFsFaults == 0, "%%fs:0 faulted with FS 0 in %" B_PRId32
		" rounds", sNullFsFaults);
}


//	#pragma mark - signals


static volatile uint16 sHandlerContextFS;
static volatile uint16 sHandlerFS;
static volatile thread_id sHandlerThread;
static thread_id sWorkerThread;
static volatile uint16 sNewFS;
static volatile int32 sHandled;


static void
fs_handler(int signal, siginfo_t* info, void* _context)
{
	ucontext_t* context = (ucontext_t*)_context;
	uint16* fs = &context->uc_mcontext.fpu.fp_fxsave.fs;

	sHandlerContextFS = *fs;
	sHandlerFS = read_fs();
	sHandlerThread = find_thread(NULL);
		// uses the TLS, at %fs:8
	if (sNewFS != 0xffff)
		*fs = sNewFS;
	sHandled++;
}


struct signal_args {
	uint16		fs;
	uint16		fsAfter;
	int32		rounds;
	status_t	sendStatus;
	bool		returned;
	bool		readAfter;
		// read %fs:0 after the handler, into valueAfter
	uint64		valueAfter;
};


/*!	Raises SIGUSR1 at itself, with FS loaded as args->fs unless that is 0, and
	records FS after each return from the handler. (Loading the null selector
	would clear the FS base on Intel CPUs, which is not what is tested here.)
*/
static status_t
signal_worker(void* data)
{
	struct signal_args* args = (struct signal_args*)data;
	thread_id self = find_thread(NULL);

	for (int32 i = 0; i < args->rounds; i++) {
		if (args->fs != 0)
			load_fs(args->fs);
		args->sendStatus = _kern_send_signal(self, SIGUSR1, NULL,
			SIGNAL_FLAG_SEND_TO_THREAD);
		args->fsAfter = read_fs();
		if (args->readAfter)
			args->valueAfter = read_fs_qword(0);
		set_fs_base(0);
	}
	args->returned = true;
	return B_OK;
}


static void
run_signal_worker(struct signal_args* args, uint16 newFS)
{
	sNewFS = newFS;
	sHandled = 0;
	sHandlerContextFS = 0xffff;
	sHandlerFS = 0xffff;
	sHandlerThread = -1;
	args->returned = false;

	sWorkerThread = spawn_thread(signal_worker, "segments signal",
		B_NORMAL_PRIORITY, args);
	resume_thread(sWorkerThread);
	status_t result;
	wait_for_thread(sWorkerThread, &result);
}


static void
test_signals(void)
{
	printf("signals: the handler gets the TLS; FS comes back after it\n");

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = fs_handler;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR1, &action, NULL);

	struct signal_args args = { USER_DATA_SELECTOR, 0, SIGNAL_ROUNDS, B_OK,
		false };
	run_signal_worker(&args, 0xffff);
	CHECK(args.sendStatus == B_OK, "sending the signal: %s",
		strerror(args.sendStatus));
	CHECK(args.returned && sHandled == SIGNAL_ROUNDS, "%" B_PRId32 " of %d"
		" signals handled", sHandled, SIGNAL_ROUNDS);
	CHECK(sHandlerContextFS == USER_DATA_SELECTOR, "the signal context holds"
		" FS %#x", sHandlerContextFS);
	CHECK(sHandlerFS == 0, "the handler ran with FS %#x", sHandlerFS);
	CHECK(sHandlerThread == sWorkerThread, "find_thread() in the handler gave"
		" %" B_PRId32, sHandlerThread);
	CHECK(args.fsAfter == USER_DATA_SELECTOR, "after the handler FS is %#x",
		args.fsAfter);

	printf("sigreturn: FS from the signal context\n");
	args = (struct signal_args){ 0, 0, 1, B_OK, false };
	run_signal_worker(&args, USER_DATA_SELECTOR);
	CHECK(args.returned && args.fsAfter == USER_DATA_SELECTOR, "FS set by the"
		" handler became %#x", args.fsAfter);

	// A null selector other than 0 comes back as 0, with the thread's own FS
	// base, which the kernel keeps only for 0.
	args = (struct signal_args){ 0, 0, 1, B_OK, false };
	run_signal_worker(&args, 3);
	CHECK(args.returned && args.fsAfter == 0, "the null selector 3 set by the"
		" handler became %#x", args.fsAfter);

	// A selector userland cannot load must not reach the kernel's own load:
	// the thread is killed, as for an invalid CS.
	args = (struct signal_args){ 0, 0, 1, B_OK, false };
	run_signal_worker(&args, KERNEL_DATA_SELECTOR);
	CHECK(sHandled == 1, "the handler did not run");
	CHECK(!args.returned, "a kernel FS selector from the signal context was"
		" accepted");

	signal(SIGUSR1, SIG_DFL);
}


//	#pragma mark - local descriptor table


/*!	32-bit code, copied below 4 GB to run (the image may be above). Called in
	64-bit mode as
		uint32 run(uint32 stackTop, uint16 fs, uint16 ds, uint32 rounds,
			uint32 expected, uint32 marker)
	it switches to the 32-bit user code segment and to the stack below
	stackTop, loads FS, DS and ES, and reads both %fs:0 and the marker through
	DS rounds times. It returns how many reads were not expected, with bit 31
	set if FS, DS or ES had changed at the end.
*/
__asm__(
	".pushsection .text\n"
	".globl compat_code_start, compat_code_end\n"
	".hidden compat_code_start, compat_code_end\n"
	".code64\n"
"compat_code_start:\n"
	"	pushq %rbx\n"
	"	pushq %rbp\n"
	"	pushq %r12\n"
	"	pushq %r13\n"
	"	pushq %r14\n"
	"	pushq %r15\n"
		// 64-bit registers do not survive 32-bit code: keep RSP on the
		// 32-bit stack
	"	movq %rsp, %rax\n"
	"	movl %edi, %esp\n"
	"	pushq %rax\n"
	"	movl %r8d, %ebp\n"
	"	movl %r9d, %edi\n"
	"	pushq $0x1b\n"
	"	leaq 1f(%rip), %rax\n"
	"	pushq %rax\n"
	"	lretq\n"
	".code32\n"
"1:\n"
	"	movw %si, %fs\n"
	"	movw %dx, %ds\n"
	"	movw %dx, %es\n"
	"	xorl %ebx, %ebx\n"
"2:\n"
	"	movl %fs:0, %eax\n"
	"	cmpl %ebp, %eax\n"
	"	jne 3f\n"
	"	cmpl %ebp, (%edi)\n"
	"	je 4f\n"
"3:\n"
	"	incl %ebx\n"
"4:\n"
	"	decl %ecx\n"
	"	jnz 2b\n"
	"	movw %fs, %ax\n"
	"	cmpw %si, %ax\n"
	"	jne 5f\n"
	"	movw %ds, %ax\n"
	"	cmpw %dx, %ax\n"
	"	jne 5f\n"
	"	movw %es, %ax\n"
	"	cmpw %dx, %ax\n"
	"	je 6f\n"
"5:\n"
	"	orl $0x80000000, %ebx\n"
"6:\n"
	"	call 7f\n"
"7:\n"
	"	popl %eax\n"
	"	addl $(8f - 7b), %eax\n"
	"	pushl $0x2b\n"
	"	pushl %eax\n"
	"	lret\n"
	".code64\n"
"8:\n"
	"	movl %ebx, %eax\n"
	"	movl %esp, %esp\n"
	"	popq %rsp\n"
	"	popq %r15\n"
	"	popq %r14\n"
	"	popq %r13\n"
	"	popq %r12\n"
	"	popq %rbp\n"
	"	popq %rbx\n"
	"	ret\n"
"compat_code_end:\n"
	".popsection\n"
);

extern const uint8 compat_code_start[];
extern const uint8 compat_code_end[];

typedef uint32 (*compat_function)(uint32 stackTop, uint32 fs, uint32 ds,
	uint32 rounds, uint32 expected, uint32 marker);


// Memory below 4 GB, as 32-bit code and segment bases need: page 0 holds the
// markers the segments point to, one per 64 bytes; then one stack page for
// each thread that runs 32-bit code.
static uint8* sLow;
static compat_function sCompat;

#define LOW_SIZE	((2 + MAX_COMPAT_THREADS) * B_PAGE_SIZE)


static uint32*
marker(int slot)
{
	return (uint32*)(sLow + slot * 64);
}


static uint64
make_descriptor(uint32 base, uint32 limit, int pages, int type, int s,
	int dpl, int present, int longMode, int big)
{
	return (limit & 0xffff) | ((uint64)(base & 0xffffff) << 16)
		| ((uint64)type << 40) | ((uint64)s << 44) | ((uint64)dpl << 45)
		| ((uint64)present << 47) | ((uint64)((limit >> 16) & 0xf) << 48)
		| ((uint64)longMode << 53) | ((uint64)big << 54)
		| ((uint64)pages << 55) | ((uint64)(base >> 24) << 56);
}


// a writable 32-bit data segment of DPL 3, as the kernel accepts
static uint64
data_descriptor(uint32 base, uint32 limit)
{
	return make_descriptor(base, limit, 0, 2, 1, 3, 1, 0, 1);
}


static status_t
set_ldt(uint32 index, uint64 descriptor)
{
	struct thread_ldt_entry entry = { index,
		{ (uint32)descriptor, (uint32)(descriptor >> 32) } };
	return _kern_generic_syscall(THREAD_SYSCALLS, THREAD_SET_LDT_ENTRY, &entry,
		sizeof(entry));
}


// Loading a selector the CPU refuses raises a signal; these catch it.
static __thread sigjmp_buf* tCatch;


static void
catch_handler(int number, siginfo_t* info, void* _context)
{
	if (tCatch != NULL)
		siglongjmp(*tCatch, 1);

	ucontext_t* context = (ucontext_t*)_context;
	printf("  FAIL: signal %d at rip %#lx, FS %#x\n", number,
		context->uc_mcontext.rip, context->uc_mcontext.fpu.fp_fxsave.fs);
	_exit(4);
}


static void
install_catch_handler(void)
{
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = catch_handler;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGSEGV, &action, NULL);
}


/*!	Loads \a selector into FS and, unless \a _value is NULL, reads %fs:0
	into it. Returns false if the CPU refused the selector. Nothing between
	loading FS and restoring the TLS may use the TLS (tCatch included).
*/
static bool
try_fs(uint16 selector, uint32* _value)
{
	sigjmp_buf catcher;
	if (sigsetjmp(catcher, 1) != 0) {
		tCatch = NULL;
		return false;
	}
	tCatch = &catcher;
	load_fs(selector);
	uint32 value = _value != NULL ? (uint32)read_fs_qword(0) : 0;
	uint16 loaded = read_fs();
	set_fs_base(0);
	tCatch = NULL;
	if (_value != NULL)
		*_value = value;
	return loaded == selector;
}


/*!	Creates an area below 4 GB. B_BASE_ADDRESS may place it anywhere, so
	free addresses are tried one after the other.
*/
static void*
create_low_area(const char* name, size_t size)
{
	for (addr_t base = 0x40000000; base + size <= 0x100000000ull;
			base += 0x10000000) {
		void* address = (void*)base;
		if (create_area(name, &address, B_EXACT_ADDRESS, size, B_NO_LOCK,
				B_READ_AREA | B_WRITE_AREA) >= 0) {
			return address;
		}
	}
	return NULL;
}


static void
setup_low_memory(void)
{
	sLow = (uint8*)create_low_area("segments test 32-bit data", LOW_SIZE);
	void* code = create_low_area("segments test 32-bit code", B_PAGE_SIZE);
	CHECK(sLow != NULL && code != NULL, "no free addresses below 4 GB");
	if (code == NULL)
		return;

	memcpy(code, compat_code_start, compat_code_end - compat_code_start);
	set_area_protection(area_for(code), B_READ_AREA | B_EXECUTE_AREA);
	sCompat = (compat_function)code;
}


struct first_entry_args {
	uint32	value;
	bool	loaded;
};

static int32 sSpinning;
static volatile int32 sGo;


static status_t
first_entry_worker(void* data)
{
	struct first_entry_args* args = (struct first_entry_args*)data;
	atomic_add(&sSpinning, 1);
	while (sGo == 0)
		;
	args->loaded = try_fs(LDT_SELECTOR(LDT_FIRST), &args->value);
	return B_OK;
}


/*!	The first entry makes the team's LDT. Threads already running on other
	CPUs, which have not entered the kernel since, must be able to load it.
	Runs before any other test sets an entry.
*/
static void
test_ldt_first_entry(int cpuCount)
{
	printf("LDT: the first entry works at once on the other CPUs\n");

	struct first_entry_args args[MAX_COMPAT_THREADS];
	thread_id threads[MAX_COMPAT_THREADS];
	int count = cpuCount - 1;
	if (count < 1)
		count = 1;
	if (count > MAX_COMPAT_THREADS)
		count = MAX_COMPAT_THREADS;

	sSpinning = 0;
	sGo = 0;
	for (int i = 0; i < count; i++) {
		args[i].loaded = false;
		threads[i] = spawn_thread(first_entry_worker, "segments spinner",
			B_NORMAL_PRIORITY, &args[i]);
		resume_thread(threads[i]);
	}
	while (atomic_get(&sSpinning) < count)
		snooze(1000);
	snooze(10000);

	*marker(0) = 0x7f000001;
	status_t status = set_ldt(LDT_FIRST, data_descriptor(
		(uint32)(addr_t)marker(0), 63));
	CHECK(status == B_OK, "THREAD_SET_LDT_ENTRY: %s", strerror(status));
	sGo = 1;

	for (int i = 0; i < count; i++) {
		status_t result;
		wait_for_thread(threads[i], &result);
		CHECK(args[i].loaded, "thread %d could not load the new entry", i);
		CHECK(!args[i].loaded || args[i].value == *marker(0), "thread %d read"
			" %#" B_PRIx32 " through it", i, args[i].value);
	}
}


static void
test_ldt_calls(void)
{
	printf("LDT: THREAD_SET_LDT_ENTRY accepts and refuses\n");

	uint32 base = (uint32)(addr_t)marker(0);
	CHECK(set_ldt(THREAD_LDT_ENTRY_COUNT, data_descriptor(base, 63))
		== B_BAD_INDEX, "an index past the table");

	static const struct {
		const char*	what;
		int			type, s, dpl, present, longMode, big;
	} kRefused[] = {
		{ "a code segment", 0xa, 1, 3, 1, 0, 1 },
		{ "a read-only data segment", 0, 1, 3, 1, 0, 1 },
		{ "a DPL 0 segment", 2, 1, 0, 1, 0, 1 },
		{ "a segment not present", 2, 1, 3, 0, 0, 1 },
		{ "a 16-bit segment", 2, 1, 3, 1, 0, 0 },
		{ "a system segment", 2, 0, 3, 1, 0, 1 },
		{ "a 64-bit segment", 2, 1, 3, 1, 1, 0 },
	};
	for (size_t i = 0; i < sizeof(kRefused) / sizeof(kRefused[0]); i++) {
		uint64 descriptor = make_descriptor(base, 63, 0, kRefused[i].type,
			kRefused[i].s, kRefused[i].dpl, kRefused[i].present,
			kRefused[i].longMode, kRefused[i].big);
		CHECK(set_ldt(LDT_UNSET, descriptor) == B_BAD_VALUE, "%s is accepted",
			kRefused[i].what);
	}

	struct thread_ldt_entry entry = { LDT_UNSET, { 0, 0 } };
	CHECK(_kern_generic_syscall(THREAD_SYSCALLS, THREAD_SET_LDT_ENTRY, &entry,
		sizeof(entry) - 1) == B_BAD_VALUE, "a short buffer");
	CHECK(_kern_generic_syscall(THREAD_SYSCALLS, THREAD_SET_LDT_ENTRY,
		(void*)0xffffffff80000000ull, sizeof(entry)) == B_BAD_ADDRESS,
		"a kernel buffer");

	CHECK(!try_fs(LDT_SELECTOR(LDT_UNSET), NULL), "an entry never set loads");

	*marker(1) = 0x7f000002;
	CHECK(set_ldt(LDT_LAST, data_descriptor((uint32)(addr_t)marker(1), 63))
		== B_OK, "the last entry");
	uint32 value = 0;
	CHECK(try_fs(LDT_SELECTOR(LDT_LAST), &value) && value == *marker(1),
		"the last entry read %#" B_PRIx32, value);

	// A cleared entry stays loadable, but no longer reaches the old memory.
	CHECK(set_ldt(LDT_CLEARED, data_descriptor((uint32)(addr_t)marker(1), 63))
		== B_OK && set_ldt(LDT_CLEARED, 0) == B_OK, "clearing an entry");
	CHECK(try_fs(LDT_SELECTOR(LDT_CLEARED), NULL), "a cleared entry cannot be"
		" loaded");
}


struct compat_args {
	int			index;
	uint32		mismatches;
	uint32		fsMismatches;
};


/*!	Runs the 32-bit code with an FS segment of its own, then checks the same
	segment from 64-bit code after yielding.
*/
static status_t
compat_worker(void* data)
{
	struct compat_args* args = (struct compat_args*)data;
	int index = args->index;
	uint16 fs = LDT_SELECTOR(LDT_THREAD(index));
	uint32 expected = *marker(1 + index);
	uint32 stackTop = (uint32)(addr_t)(sLow + (2 + index) * B_PAGE_SIZE);

	for (int i = 0; i < COMPAT_RUNS; i++) {
		args->mismatches += sCompat(stackTop, fs, LDT_SELECTOR(LDT_FLAT),
			COMPAT_ROUNDS, expected, (uint32)(addr_t)marker(1 + index));

		// back in 64-bit code, still with the segments of the 32-bit code
		_kern_thread_yield();
		if (read_fs() != fs || (uint32)read_fs_qword(0) != expected
			|| read_ds() != LDT_SELECTOR(LDT_FLAT)) {
			args->fsMismatches++;
		}
		load_ds_es(0);
		set_fs_base(0);
	}
	return B_OK;
}


static void
test_ldt_compat_code(int cpuCount)
{
	int count = 2 * cpuCount;
	if (count > MAX_COMPAT_THREADS)
		count = MAX_COMPAT_THREADS;
	printf("LDT: 32-bit code in %d threads, each its own FS\n", count);

	CHECK(set_ldt(LDT_FLAT, make_descriptor(0, 0xfffff, 1, 2, 1, 3, 1, 0, 1))
		== B_OK, "the flat data segment");

	static struct compat_args args[MAX_COMPAT_THREADS];
	thread_id threads[MAX_COMPAT_THREADS];
	for (int i = 0; i < count; i++) {
		*marker(1 + i) = 0x7e000000 + i;
		CHECK(set_ldt(LDT_THREAD(i), data_descriptor(
			(uint32)(addr_t)marker(1 + i), 63)) == B_OK, "entry %d", i);
		args[i] = (struct compat_args){ i, 0, 0 };
		threads[i] = spawn_thread(compat_worker, "segments 32-bit",
			B_NORMAL_PRIORITY, &args[i]);
	}
	for (int i = 0; i < count; i++)
		resume_thread(threads[i]);

	for (int i = 0; i < count; i++) {
		status_t result;
		wait_for_thread(threads[i], &result);
		CHECK(args[i].mismatches == 0, "thread %d: 32-bit code saw %#" B_PRIx32
			" wrong values (bit 31: a selector changed)", i,
			args[i].mismatches);
		CHECK(args[i].fsMismatches == 0, "thread %d: %" B_PRIu32 " of %d times"
			" 64-bit code saw another segment state", i, args[i].fsMismatches,
			COMPAT_RUNS);
	}
}


static void
test_ldt_signals(void)
{
	printf("LDT: signals and sigreturn with an FS of the LDT\n");

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = fs_handler;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR1, &action, NULL);

	uint16 fs = LDT_SELECTOR(LDT_THREAD(0));
	struct signal_args args = { fs, 0, SIGNAL_ROUNDS, B_OK, false, true, 0 };
	run_signal_worker(&args, 0xffff);
	CHECK(args.returned && sHandled == SIGNAL_ROUNDS, "%" B_PRId32 " of %d"
		" signals handled", sHandled, SIGNAL_ROUNDS);
	CHECK(sHandlerContextFS == fs, "the signal context holds FS %#x",
		sHandlerContextFS);
	CHECK(sHandlerFS == 0 && sHandlerThread == sWorkerThread, "the handler ran"
		" with FS %#x; find_thread() gave %" B_PRId32, sHandlerFS,
		sHandlerThread);
	CHECK(args.fsAfter == fs && args.valueAfter == *marker(1), "after the"
		" handler FS is %#x and %%fs:0 is %#" B_PRIx64, args.fsAfter,
		args.valueAfter);

	args = (struct signal_args){ 0, 0, 1, B_OK, false, true, 0 };
	run_signal_worker(&args, fs);
	CHECK(args.returned && args.fsAfter == fs && args.valueAfter == *marker(1),
		"FS set by the handler became %#x, %%fs:0 %#" B_PRIx64, args.fsAfter,
		args.valueAfter);

	// An entry never set: the thread is killed.
	args = (struct signal_args){ 0, 0, 1, B_OK, false, false, 0 };
	run_signal_worker(&args, LDT_SELECTOR(LDT_UNSET));
	CHECK(sHandled == 1, "the handler did not run");
	CHECK(!args.returned, "an FS of an unset LDT entry from the signal context"
		" was accepted");

	signal(SIGUSR1, SIG_DFL);
}


static void
test_ldt_fork(void)
{
	printf("LDT: a forked child starts without one\n");

	uint16 selector = LDT_SELECTOR(LDT_THREAD(0));
	load_ds_es(selector);
	pid_t child = fork();
	if (child == 0) {
		int result = 0;
		if (read_ds() != 0 || read_es() != 0)
			result |= 1;
		if (try_fs(selector, NULL))
			result |= 2;
		_exit(result);
	}
	load_ds_es(0);

	int status = 0;
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the child reported"
		" %d (1: DS or ES kept, 2: the LDT entry loads)", WEXITSTATUS(status));

	uint32 value = 0;
	CHECK(try_fs(selector, &value) && value == *marker(1), "the parent's entry"
		" read %#" B_PRIx32 " after fork()", value);
}


static status_t
exit_worker(void* data)
{
	uint16 selector = (uint16)(addr_t)data;
	load_ds_es(selector);
	load_fs(selector);
	_kern_exit_thread(B_OK);
		// with the selectors still loaded: exit_thread() would use the TLS
	return B_OK;
}


/*!	A thread that exits with selectors of the LDT in DS, ES and FS is moved to
	the kernel team, which has no LDT, and may be switched out and in again
	before it is gone. The kernel must not load its selectors then. Many
	threads exit at once to make that likely.
*/
static void
test_ldt_exit(int cpuCount)
{
	printf("LDT: threads exit with LDT selectors loaded\n");

	const int count = 4 * (cpuCount < 2 ? 2 : cpuCount);
	thread_id threads[4 * MAX_COMPAT_THREADS];
	int unfinished = 0;
	for (int round = 0; round < EXIT_ROUNDS; round++) {
		for (int i = 0; i < count && i < 4 * MAX_COMPAT_THREADS; i++) {
			threads[i] = spawn_thread(exit_worker, "segments exit",
				B_NORMAL_PRIORITY,
				(void*)(addr_t)LDT_SELECTOR(LDT_THREAD(0)));
			resume_thread(threads[i]);
		}
		for (int i = 0; i < count && i < 4 * MAX_COMPAT_THREADS; i++) {
			status_t result = B_ERROR;
			if (wait_for_thread(threads[i], &result) != B_OK || result != B_OK)
				unfinished++;
		}
	}
	CHECK(unfinished == 0, "%d threads did not exit with B_OK", unfinished);
}


//	#pragma mark - exec


static void
test_exec(const char* self)
{
	printf("exec: the new image starts with the default segment state and no"
		" LDT\n");

	pid_t child = fork();
	if (child == 0) {
		// The arguments are prepared while the TLS still works.
		uint16 selector = LDT_SELECTOR(LDT_THREAD(0));
		char selectorString[16];
		snprintf(selectorString, sizeof(selectorString), "%u", selector);
		const char* const args[] = { self, "--exec-child", selectorString };
		const char* const environment[] = { NULL };
		int32 environmentCount = 0;
		char** flatArgs;
		size_t flatSize;
		if (__flatten_process_args(args, 3, environment, &environmentCount,
				self, &flatArgs, &flatSize) != B_OK
			|| set_ldt(LDT_THREAD(0), data_descriptor(
				(uint32)(addr_t)marker(1), 63)) != B_OK) {
			_exit(3);
		}

		uint64 gsBase = 0x12340000;
		thread_call(THREAD_SET_GS_BASE, &gsBase);
		load_ds_es(selector);
		load_fs(selector);
		_kern_exec(self, (const char* const*)flatArgs, flatSize, 3,
			environmentCount, 022);
		_kern_exit_team(2);
	}

	int status = 0;
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the new image"
		" reported %d", WEXITSTATUS(status));
}


static int
exec_child(uint16 selector)
{
	// FS, DS and ES are 0, and the TLS works (printf() uses it).
	uint64 fsBase = 0, gsBase = 1;
	thread_call(THREAD_GET_FS_BASE, &fsBase);
	thread_call(THREAD_GET_GS_BASE, &gsBase);
	if (read_fs() != 0 || read_ds() != 0 || read_es() != 0 || gsBase != 0) {
		printf("  FAIL after exec(): FS %#x, DS %#x, ES %#x, GS base %#"
			B_PRIx64 "\n", read_fs(), read_ds(), read_es(), gsBase);
		return 1;
	}

	install_catch_handler();
	if (try_fs(selector, NULL)) {
		printf("  FAIL after exec(): the old image's LDT entry loads\n");
		return 1;
	}
	return 0;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc > 2 && strcmp(argv[1], "--exec-child") == 0)
		return exec_child((uint16)strtoul(argv[2], NULL, 0));

	uint64 base;
	if (thread_call(THREAD_GET_FS_BASE, &base) != B_OK) {
		printf("This kernel lacks THREAD_GET_FS_BASE; nothing to test.\n");
		return 1;
	}

	test_calls();
	test_context_switches(MODE_FS_BASE);
	test_context_switches(MODE_FS_SELECTOR);
	test_null_fs();
	test_signals();

	// An index past the table is refused before the LDT is made.
	if (set_ldt(THREAD_LDT_ENTRY_COUNT, 0) != B_BAD_INDEX) {
		printf("This kernel lacks THREAD_SET_LDT_ENTRY; %d check(s) failed\n",
			sFailures + 1);
		return 1;
	}

	system_info info;
	get_system_info(&info);
	install_catch_handler();
	setup_low_memory();
	if (sLow == NULL || sCompat == NULL) {
		printf("%d check(s) failed\n", sFailures);
		return 1;
	}
	test_ldt_first_entry(info.cpu_count);
	test_ldt_calls();
	test_ldt_compat_code(info.cpu_count);
	test_ldt_signals();
	test_ldt_fork();
	test_ldt_exit(info.cpu_count);
	test_exec(argv[0]);

	if (sFailures != 0) {
		printf("%d check(s) failed\n", sFailures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
