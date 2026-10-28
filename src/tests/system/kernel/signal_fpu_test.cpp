/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	signal_fpu_test [seconds]: a thread interrupted by a signal must get its
	SSE registers back as they were when the handler returns.

	A worker keeps a pattern in xmm8-xmm15 and checks them in a loop that
	makes no syscall, so the signals another thread sends it arrive on
	interrupts, with live register state. Its handlers return through
	_kern_restore_signal_frame() in three ways:
	- plain: nothing else to do, the syscall's fast exit;
	- with an ignored signal pending: the handler, which blocks SIGCHLD,
	  sends itself SIGCHLD, which becomes deliverable when the frame is
	  restored, so the exit takes the slow path and finds nothing to run;
	- with another handled signal pending (SIGUSR2, blocked in the handler):
	  a new frame is set up on the way out.
	Any lost register is reported; the test fails then.
*/


#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>


static volatile int sMode;
static volatile int64 sHandled;
static volatile int64 sChecks;
static volatile int64 sLost;
static volatile bool sQuit;
static pthread_t sWorker;


static void
usr1_handler(int signal)
{
	// clobber the registers the way any code may
	asm volatile("pxor %%xmm8, %%xmm8\n\tpxor %%xmm15, %%xmm15" ::: "xmm8",
		"xmm15");
	if (sMode == 1)
		pthread_kill(pthread_self(), SIGCHLD);
	else if (sMode == 2)
		pthread_kill(pthread_self(), SIGUSR2);
	atomic_add64((int64*)&sHandled, 1);
}


static void
usr2_handler(int signal)
{
	asm volatile("pxor %%xmm9, %%xmm9" ::: "xmm9");
}


static void*
worker(void*)
{
	while (!sQuit) {
		uint64 lost = 0;
		// a pattern in xmm8-15, then many checks of it without a syscall
		asm volatile(
			"movq $0x1111111111111111, %%rax\n\t"
			"movq %%rax, %%xmm8\n\t"
			"movq $0x2222222222222222, %%rax\n\t"
			"movq %%rax, %%xmm9\n\t"
			"movq $0x3333333333333333, %%rax\n\t"
			"movq %%rax, %%xmm10\n\t"
			"movq $0x4444444444444444, %%rax\n\t"
			"movq %%rax, %%xmm11\n\t"
			"movq $0x5555555555555555, %%rax\n\t"
			"movq %%rax, %%xmm12\n\t"
			"movq $0x6666666666666666, %%rax\n\t"
			"movq %%rax, %%xmm13\n\t"
			"movq $0x7777777777777777, %%rax\n\t"
			"movq %%rax, %%xmm14\n\t"
			"movq $0x0123456789abcdef, %%rax\n\t"
			"movq %%rax, %%xmm15\n\t"
			"movl $2000000, %%ecx\n"
			"1:\n\t"
			"movq %%xmm8, %%rax\n\t"
			"movq $0x1111111111111111, %%rdx\n\t"
			"cmpq %%rdx, %%rax\n\t"
			"jne 2f\n\t"
			"movq %%xmm9, %%rax\n\t"
			"movq $0x2222222222222222, %%rdx\n\t"
			"cmpq %%rdx, %%rax\n\t"
			"jne 2f\n\t"
			"movq %%xmm12, %%rax\n\t"
			"movq $0x5555555555555555, %%rdx\n\t"
			"cmpq %%rdx, %%rax\n\t"
			"jne 2f\n\t"
			"movq %%xmm15, %%rax\n\t"
			"movq $0x0123456789abcdef, %%rdx\n\t"
			"cmpq %%rdx, %%rax\n\t"
			"jne 2f\n\t"
			"decl %%ecx\n\t"
			"jnz 1b\n\t"
			"jmp 3f\n"
			"2:\n\t"
			"movq $1, %0\n"
			"3:\n\t"
			: "+r"(lost)
			:
			: "rax", "rcx", "rdx", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
				"xmm13", "xmm14", "xmm15", "cc");
		atomic_add64((int64*)&sChecks, 1);
		if (lost != 0)
			atomic_add64((int64*)&sLost, 1);
	}
	return NULL;
}


static bool
run(int mode, const char* name, bigtime_t duration)
{
	sMode = mode;
	sHandled = sChecks = sLost = 0;
	sQuit = false;
	pthread_create(&sWorker, NULL, worker, NULL);
	bigtime_t end = system_time() + duration;
	while (system_time() < end) {
		pthread_kill(sWorker, SIGUSR1);
		snooze(200);
	}
	sQuit = true;
	pthread_join(sWorker, NULL);
	printf("%-36s %" B_PRId64 " signals, %" B_PRId64 " rounds, %" B_PRId64
		" with registers lost: %s\n", name, sHandled, sChecks, sLost,
		sLost == 0 ? "ok" : "FAILED");
	return sLost == 0;
}


int
main(int argc, char** argv)
{
	bigtime_t duration = (argc > 1 ? atoi(argv[1]) : 3) * 1000000LL;

	struct sigaction action = {};
	action.sa_handler = usr1_handler;
	action.sa_flags = SA_RESTART;
	sigemptyset(&action.sa_mask);
	sigaddset(&action.sa_mask, SIGCHLD);
	sigaddset(&action.sa_mask, SIGUSR2);
	sigaction(SIGUSR1, &action, NULL);
	action.sa_handler = usr2_handler;
	sigemptyset(&action.sa_mask);
	sigaction(SIGUSR2, &action, NULL);

	bool ok = run(0, "plain return", duration);
	ok &= run(1, "return with SIGCHLD pending", duration);
	ok &= run(2, "return with SIGUSR2 pending", duration);
	printf("%s\n", ok ? "PASSED" : "FAILED");
	return ok ? 0 : 1;
}
