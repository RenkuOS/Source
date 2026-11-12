/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	A thread waiting for a libroot rw_lock gets a signal, then the lock is
	released. The waiter has to wake up, and it must not burn CPU time while
	it waits. Both an ignored signal (SIGCHLD: the kernel restarts the wait)
	and a caught one without SA_RESTART (libroot calls the wait again) are
	tried. Before the fix, the waiter spun at full CPU forever in both cases.
*/


#include <OS.h>
#include <signal.h>
#include <stdio.h>

#include <locks.h>


static rw_lock sLock;
static int32 sReaderDone;


static void
signal_handler(int)
{
}


static status_t
reader(void*)
{
	rw_lock_read_lock(&sLock);
	atomic_set(&sReaderDone, 1);
	rw_lock_read_unlock(&sLock);
	return B_OK;
}


static bigtime_t
cpu_time(thread_id thread)
{
	thread_info info;
	if (get_thread_info(thread, &info) != B_OK)
		return -1;
	return info.user_time + info.kernel_time;
}


static bool
test(int signalNumber, bool catchSignal)
{
	struct sigaction action = {};
	action.sa_handler = catchSignal ? signal_handler : SIG_DFL;
	sigaction(signalNumber, &action, NULL);

	rw_lock_init(&sLock, "rw_lock signal test");
	atomic_set(&sReaderDone, 0);
	rw_lock_write_lock(&sLock);

	thread_id thread = spawn_thread(reader, "reader", B_NORMAL_PRIORITY, NULL);
	resume_thread(thread);
	snooze(200000);

	bigtime_t before = cpu_time(thread);
	send_signal(thread, signalNumber);
	snooze(1000000);
	bigtime_t spent = cpu_time(thread) - before;

	rw_lock_write_unlock(&sLock);
	for (int i = 0; i < 40 && atomic_get(&sReaderDone) == 0; i++)
		snooze(50000);

	bool woke = atomic_get(&sReaderDone) != 0;
	if (woke) {
		status_t result;
		wait_for_thread(thread, &result);
	} else
		kill_thread(thread);
	rw_lock_destroy(&sLock);

	bool passed = woke && spent < 100000;
	printf("%s: signal %d %s: %lld us of CPU while waiting, %s after the "
		"unlock\n", passed ? "PASS" : "FAIL", signalNumber,
		catchSignal ? "caught" : "ignored", (long long)spent,
		woke ? "woke" : "did not wake");
	return passed;
}


int
main()
{
	bool passed = test(SIGCHLD, false);
	passed &= test(SIGUSR1, true);
	return passed ? 0 : 1;
}
