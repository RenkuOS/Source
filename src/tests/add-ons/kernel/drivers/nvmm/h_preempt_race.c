/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Threads that enter the NVMM driver's preemption-disabled sections as
	often as they can (the state ioctls of the VMX backend, and vcpu_run of
	both), among threads that are busy and asleep in short turns: those wake
	up all the time, preempt the first wherever they happen to be, and keep
	the load of the CPUs uneven, so that a preempted thread often continues
	on another CPU.

	os_preempt_disable() used to look up the thread's CPU and count on it
	with interrupts on. A thread moved in between counted on the CPU it had
	left, which never rescheduled again (and a thread there that wanted to
	block carried on instead). The test ends by starting a thread on every
	CPU: one that does not get to run, or whose snooze() returns at once,
	shows such a CPU. With the fault the machine may as well stop or panic
	before that.

	usage: libnvmm_preempt_race [seconds [threads]]
*/


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <err.h>
#include <errno.h>
#include <syslog.h>

#include <OS.h>

#include <nvmm.h>

#include "h_os.h"


extern status_t _kern_set_thread_affinity(thread_id id, const void* mask,
	size_t size);

#define MAX_THREADS	256

static volatile bool sStop;
static volatile int64 sCalls[MAX_THREADS];
static volatile int32 sRuns[MAX_THREADS];
static volatile bool sDone[MAX_THREADS];
static thread_id sThreads[MAX_THREADS];
static thread_id sBusyThreads[MAX_THREADS];
static thread_id sProbeThreads[MAX_THREADS];
static int32 sThreadCount;
static int32 sCPUCount;
static volatile int64 sMoves;

static volatile bigtime_t sProbeTime[MAX_THREADS];
static volatile bool sProbeDone[MAX_THREADS];


/*!	Only for the calling thread: _kern_set_thread_affinity() for another
	thread looks at that thread's CPU, which it has none of while it does
	not run (a NULL dereference in the kernel).
*/
static void
only_cpu(int32 cpu)
{
	// more than any CPUSet: the kernel takes its size from the front
	uint32 mask[64];
	memset(mask, 0, sizeof(mask));
	mask[cpu / 32] |= 1u << (cpu % 32);
	_kern_set_thread_affinity(find_thread(NULL), mask, sizeof(mask));
}


static status_t
enter_thread(void* data)
{
	int32 index = (int32)(addr_t)data;
	struct nvmm_machine mach;
	struct nvmm_vcpu vcpu;

	if (nvmm_machine_create(&mach) == -1)
		err(1, "nvmm_machine_create");
	if (nvmm_vcpu_create(&mach, 0, &vcpu) == -1)
		err(1, "nvmm_vcpu_create");

	while (!sStop) {
		// nothing cached: the call has to ask the driver
		mach.pages[0]->state_cached = 0;
		if (nvmm_vcpu_getstate(&mach, &vcpu, NVMM_X64_STATE_GPRS) == -1)
			err(1, "nvmm_vcpu_getstate");
		sCalls[index]++;

		// no memory behind the reset vector: back at once, whatever with
		if ((sCalls[index] & 7) == 0 && nvmm_vcpu_run(&mach, &vcpu) == 0)
			sRuns[index]++;
	}

	nvmm_vcpu_destroy(&mach, &vcpu);
	nvmm_machine_destroy(&mach);
	sDone[index] = true;
	return B_OK;
}


static status_t
busy_thread(void* data)
{
	uint32 seed = 12345 + (uint32)(addr_t)data * 7919;
	volatile uint64 count = 0;
	while (!sStop) {
		seed = seed * 1103515245 + 12345;
		bigtime_t end = system_time() + 200 + (seed >> 16) % 2800;
		while (system_time() < end)
			count++;
		seed = seed * 1103515245 + 12345;
		snooze(200 + (seed >> 16) % 2800);
		sMoves++;
	}
	return B_OK;
}


static status_t
probe_thread(void* data)
{
	int32 cpu = (int32)(addr_t)data;
	only_cpu(cpu);
	bigtime_t start = system_time();
	snooze(20000);
	sProbeTime[cpu] = system_time() - start;
	sProbeDone[cpu] = true;
	return B_OK;
}


//!	Waits without relying on a blocking call that works.
static void
wait_until(volatile bool* flags, int32 count, bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	while (system_time() < end) {
		int32 done = 0;
		for (int32 i = 0; i < count; i++)
			done += flags[i] ? 1 : 0;
		if (done == count)
			return;
		snooze(1000);
	}
}


int
main(int argc, char** argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 60;
	system_info info;
	get_system_info(&info);
	sCPUCount = info.cpu_count;
	sThreadCount = argc > 2 ? atoi(argv[2]) : sCPUCount;
	if (sThreadCount > MAX_THREADS)
		sThreadCount = MAX_THREADS;
	if (sCPUCount > MAX_THREADS)
		sCPUCount = MAX_THREADS;

	setvbuf(stdout, NULL, _IOLBF, 0);

	if (nvmm_init() == -1)
		err(1, "nvmm_init");

	printf("%d threads entering the driver, %d busy threads, %d CPUs, %d s\n",
		(int)sThreadCount, (int)sCPUCount * 2, (int)sCPUCount, seconds);

	for (int32 i = 0; i < sThreadCount; i++) {
		sThreads[i] = spawn_thread(enter_thread, "enter", B_NORMAL_PRIORITY,
			(void*)(addr_t)i);
		resume_thread(sThreads[i]);
	}
	for (int32 i = 0; i < sCPUCount * 2; i++) {
		sBusyThreads[i] = spawn_thread(busy_thread, "busy", B_NORMAL_PRIORITY,
			(void*)(addr_t)i);
		resume_thread(sBusyThreads[i]);
	}

	int64 lastCalls = 0;
	for (int second = 1; second <= seconds; second++) {
		bigtime_t end = system_time() + 1000000;
		while (system_time() < end)
			snooze(10000);
		int64 calls = 0, runs = 0;
		for (int32 i = 0; i < sThreadCount; i++) {
			calls += sCalls[i];
			runs += sRuns[i];
		}
		if (second % 5 == 0 || calls == lastCalls) {
			printf("%3d s: %" B_PRId64 " state calls, %" B_PRId64
				" runs, %" B_PRId64 " wake-ups%s\n", second, calls, runs,
				sMoves, calls == lastCalls ? " (no call in this second)" : "");
		}
		lastCalls = calls;
	}

	sStop = true;
	wait_until(sDone, sThreadCount, 10000000);
	for (int32 i = 0; i < sThreadCount; i++)
		wait_for_thread(sThreads[i], NULL);
	for (int32 i = 0; i < sCPUCount * 2; i++)
		wait_for_thread(sBusyThreads[i], NULL);
	int32 left = 0;
	for (int32 i = 0; i < sThreadCount; i++)
		left += sDone[i] ? 0 : 1;

	// a thread on every CPU: does it get to run, and does it sleep?
	for (int32 cpu = 0; cpu < sCPUCount; cpu++) {
		sProbeThreads[cpu] = spawn_thread(probe_thread, "probe",
			B_NORMAL_PRIORITY, (void*)(addr_t)cpu);
		resume_thread(sProbeThreads[cpu]);
	}
	wait_until(sProbeDone, sCPUCount, 5000000);
	for (int32 cpu = 0; cpu < sCPUCount; cpu++)
		wait_for_thread(sProbeThreads[cpu], NULL);

	int32 bad = 0;
	for (int32 cpu = 0; cpu < sCPUCount; cpu++) {
		if (!sProbeDone[cpu]) {
			printf("CPU %d: a thread for it did not run within 5 s\n",
				(int)cpu);
			bad++;
		} else if (sProbeTime[cpu] < 15000) {
			printf("CPU %d: snooze(20 ms) came back after %" B_PRIdBIGTIME
				" us\n", (int)cpu, sProbeTime[cpu]);
			bad++;
		}
	}

	if (left > 0)
		printf("%d of the threads did not end\n", (int)left);
	printf("RESULT: %s\n", bad == 0 && left == 0
		? "ok, every CPU reschedules" : "FAILED");
	fflush(stdout);
	syslog(LOG_INFO, "libnvmm_preempt_race: RESULT: %s",
		bad == 0 && left == 0 ? "ok, every CPU reschedules" : "FAILED");
	// not through exit(): threads may be stuck
	_exit(bad == 0 && left == 0 ? 0 : 1);
}
