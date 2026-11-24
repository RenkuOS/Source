/*
 * Copyright 2026, rations.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	x86_64_segments_test: the user segment state a thread keeps on x86_64.

	A thread's FS base set with THREAD_SET_FS_BASE, and FS, DS and ES selectors
	it loaded itself, must stay its own across context switches, signals,
	sigreturn and exec(). 32-bit code needs that: Wine, for one, runs 32-bit
	Windows programs with their TEB in FS.

	While a thread runs with another FS than its TLS, it uses no libroot
	function that could reach the TLS: only raw syscalls.

	Run without arguments. "x86_64_segments_test --exec-child" is the half of
	the exec() test that runs in the new image.
*/


#include <OS.h>

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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


//	#pragma mark - exec


static void
test_exec(const char* self)
{
	printf("exec: the new image starts with the default segment state\n");

	pid_t child = fork();
	if (child == 0) {
		// The arguments are prepared while the TLS still works.
		const char* const args[] = { self, "--exec-child" };
		const char* const environment[] = { NULL };
		int32 environmentCount = 0;
		char** flatArgs;
		size_t flatSize;
		if (__flatten_process_args(args, 2, environment, &environmentCount,
				self, &flatArgs, &flatSize) != B_OK) {
			_exit(3);
		}

		uint64 gsBase = 0x12340000;
		thread_call(THREAD_SET_GS_BASE, &gsBase);
		load_ds_es(USER_DATA_SELECTOR);
		load_fs(USER_DATA_SELECTOR);
		_kern_exec(self, (const char* const*)flatArgs, flatSize, 2,
			environmentCount, 022);
		_kern_exit_team(2);
	}

	int status = 0;
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the new image"
		" reported %d", WEXITSTATUS(status));
}


static int
exec_child(void)
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
	return 0;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc > 1 && strcmp(argv[1], "--exec-child") == 0)
		return exec_child();

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
	test_exec(argv[0]);

	if (sFailures != 0) {
		printf("%d check(s) failed\n", sFailures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
