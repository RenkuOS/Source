/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Sets the CPU affinity of other threads of the team, sleeping and
	running ones. _user_set_thread_affinity() read the CPU of the thread it
	was called for, which a thread that does not run has none of: a NULL
	dereference in the kernel (#149).
*/


#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <OS.h>


extern "C" status_t _kern_set_thread_affinity(thread_id id, const void* mask,
	size_t size);
extern "C" status_t _kern_get_thread_affinity(thread_id id, void* mask,
	size_t size);

static volatile bool sStop;


static status_t
sleeper(void* data)
{
	while (!sStop)
		snooze(50000);
	// leave one by one: simultaneous exits stress thread teardown
	snooze(((intptr_t)data) * 100000);
	return B_OK;
}


static status_t
runner(void* data)
{
	volatile uint64 count = 0;
	while (!sStop)
		count++;
	snooze(((intptr_t)data) * 100000);
	return B_OK;
}


int
main()
{
	system_info info;
	get_system_info(&info);
	int32 cpus = info.cpu_count;

	thread_id threads[4];
	for (int i = 0; i < 4; i++) {
		threads[i] = spawn_thread(i < 2 ? sleeper : runner, "target",
			B_NORMAL_PRIORITY, (void*)(intptr_t)i);
		resume_thread(threads[i]);
	}
	snooze(100000);

	int failures = 0;
	for (int round = 0; round < 20000; round++) {
		// more than any CPUSet: the kernel takes its size from the front
		uint32 mask[64];
		memset(mask, 0, sizeof(mask));
		int32 cpu = round % cpus;
		mask[cpu / 32] |= 1u << (cpu % 32);
		thread_id thread = threads[round % 4];
		status_t status = _kern_set_thread_affinity(thread, mask,
			sizeof(mask));
		if (status != B_OK) {
			printf("set_thread_affinity(%" B_PRId32 ", CPU %" B_PRId32
				"): %s\n", thread, cpu, strerror(status));
			failures++;
			break;
		}
		uint32 got[64];
		memset(got, 0, sizeof(got));
		if (_kern_get_thread_affinity(thread, got, sizeof(got)) == B_OK
			&& cpus > 1 && got[cpu / 32] != mask[cpu / 32]) {
			printf("thread %" B_PRId32 ": mask %#" B_PRIx32 " instead of %#"
				B_PRIx32 "\n", thread, got[cpu / 32], mask[cpu / 32]);
			failures++;
			break;
		}
	}

	sStop = true;
	for (int i = 0; i < 4; i++) {
		status_t result;
		wait_for_thread(threads[i], &result);
	}

	const char* result = failures == 0
		? "ok, 20000 affinity changes of 4 other threads" : "FAILED";
	printf("RESULT: %s\n", result);
	syslog(LOG_INFO, "thread_affinity_other: RESULT: %s", result);
	return failures == 0 ? 0 : 1;
}
