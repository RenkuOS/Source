/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


/*!	A debugger that goes away while it profiles running threads.

	The profiling timer is one per CPU and belongs to the thread that armed
	it. The scheduler cancelled it only for threads of a team with a debugger
	installed, so once the debugger had gone (profile's remove_team_debugger(),
	or the debugger dying), a thread that left its CPU left its timer armed
	there. It fired for the next thread on that CPU, often the idle thread,
	and profiling_event() dereferenced that thread's NULL installed_timer:
	"PANIC: page fault, but interrupts were disabled" in
	profiling_event() from timer_interrupt() (Framework 13, 2026-11-10, while
	`profile` followed Summit's child teams). The threads also kept their
	sample buffers, so B_DEBUG_START_PROFILER failed for them afterwards.

	A child team runs more busy threads than there are CPUs, so they keep
	being switched. Each round installs a debugger, starts a profiler on every
	thread with a short interval, removes the debugger while they run and
	gives the stray timers time to fire. A kernel with the bug panics; the
	test then fails by not finishing. It also checks that every thread can be
	profiled again in the next round. Prints PASS or FAIL, exits 0 on PASS.
*/


#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>
#include <debugger.h>


static const int kMaxThreads = 64;
static const int32 kRounds = 30;
static const bigtime_t kTimeBudget = 20000000;
static const bigtime_t kReplyTimeout = 2000000;
static const bigtime_t kInterval = 20;
static const size_t kSampleAreaSize = 64 * 1024;


static status_t
spin_thread(void*)
{
	volatile uint32 counter = 0;
	while (true)
		counter++;
	return B_OK;
}


static void
run_child(int threadCount)
{
	for (int i = 0; i < threadCount; i++) {
		thread_id thread = spawn_thread(spin_thread, "spin", B_LOW_PRIORITY,
			NULL);
		if (thread < 0)
			_exit(1);
		resume_thread(thread);
	}
	while (true)
		snooze(1000000);
}


static void
fail(pid_t child, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	printf("FAIL: profiler_detach: ");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	fflush(stdout);

	if (child > 0) {
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
	}
	exit(1);
}


int
main()
{
	system_info info;
	get_system_info(&info);
	int threadCount = (int)info.cpu_count * 2 + 2;
	if (threadCount > kMaxThreads - 1)
		threadCount = kMaxThreads - 1;

	pid_t child = fork();
	if (child < 0)
		fail(-1, "fork: %s", strerror(errno));
	if (child == 0)
		run_child(threadCount);

	// wait for the child's threads
	thread_id threads[kMaxThreads];
	int count = 0;
	bigtime_t deadline = system_time() + 5000000;
	while (count < threadCount + 1) {
		if (system_time() > deadline)
			fail(child, "child started %d of %d threads", count,
				threadCount + 1);
		snooze(10000);
		count = 0;
		int32 cookie = 0;
		thread_info threadInfo;
		while (count < kMaxThreads
			&& get_next_thread_info(child, &cookie, &threadInfo) == B_OK) {
			threads[count++] = threadInfo.thread;
		}
	}

	printf("profiler_detach: %" B_PRId32 " CPUs, %d threads profiled\n",
		info.cpu_count, count);
	fflush(stdout);

	bigtime_t start = system_time();
	int32 rounds = 0;
	for (; rounds < kRounds && system_time() - start < kTimeBudget; rounds++) {
		port_id debuggerPort = create_port(100, "profiler_detach debugger");
		port_id replyPort = create_port(10, "profiler_detach reply");
		if (debuggerPort < 0 || replyPort < 0)
			fail(child, "create_port failed");

		port_id nubPort = install_team_debugger(child, debuggerPort);
		if (nubPort < 0) {
			fail(child, "round %" B_PRId32 ": install_team_debugger: %s",
				rounds, strerror(nubPort));
		}

		area_id areas[kMaxThreads];
		for (int i = 0; i < count; i++) {
			void* address;
			areas[i] = create_area("profiler_detach samples", &address,
				B_ANY_ADDRESS, kSampleAreaSize, B_NO_LOCK,
				B_READ_AREA | B_WRITE_AREA);
			if (areas[i] < 0)
				fail(child, "create_area failed");

			debug_nub_start_profiler message;
			memset(&message, 0, sizeof(message));
			message.reply_port = replyPort;
			message.thread = threads[i];
			message.interval = kInterval;
			message.sample_area = areas[i];
			message.stack_depth = 1;
			message.variable_stack_depth = false;
			message.profile_kernel = false;
			status_t error = write_port(nubPort,
				B_DEBUG_MESSAGE_START_PROFILER, &message, sizeof(message));
			if (error != B_OK)
				fail(child, "write to the nub port: %s", strerror(error));

			debug_nub_start_profiler_reply reply;
			int32 code;
			ssize_t size = read_port_etc(replyPort, &code, &reply,
				sizeof(reply), B_RELATIVE_TIMEOUT, kReplyTimeout);
			if (size < 0)
				fail(child, "no B_DEBUG_START_PROFILER reply: %s",
					strerror(size));
			if (reply.error != B_OK) {
				fail(child, "round %" B_PRId32 ": thread %" B_PRId32
					" could not be profiled: %s", rounds, threads[i],
					strerror(reply.error));
			}
		}

		// let the threads sample and switch, then leave while they run
		snooze(20000);
		status_t error = remove_team_debugger(child);
		if (error != B_OK)
			fail(child, "remove_team_debugger: %s", strerror(error));

		// the stray timers fire here
		snooze(20000);

		for (int i = 0; i < count; i++)
			delete_area(areas[i]);
		delete_port(replyPort);
		delete_port(debuggerPort);
	}

	kill(child, SIGKILL);
	waitpid(child, NULL, 0);

	printf("PASS: profiler_detach: %" B_PRId32 " rounds, debugger removed "
		"while %d threads were profiled\n", rounds, count);
	syslog(LOG_INFO, "profiler_detach_test: return 0");
	return 0;
}
