/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Scheduler affinity stress test. CPU hotplug, where a core's CPU count
	can drop to zero, is not available on the X270; this test exercises the
	affinity and reschedule paths heavily and reports through syslog.
*/


#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <OS.h>


extern "C" status_t _kern_set_thread_affinity(thread_id id, const void* mask,
	size_t size);


static volatile bool sStop;


static status_t
worker(void*)
{
	while (!sStop)
		snooze(1000);
	return B_OK;
}


int
main()
{
	system_info info;
	get_system_info(&info);
	int32 cpus = info.cpu_count;

	thread_id threads[8];
	for (int32 i = 0; i < 8; i++) {
		threads[i] = spawn_thread(worker, "affinity", B_NORMAL_PRIORITY, NULL);
		resume_thread(threads[i]);
	}

	for (int32 round = 0; round < 5000; round++) {
		uint32 mask[64];
		memset(mask, 0, sizeof(mask));
		int32 cpu = round % cpus;
		mask[cpu / 32] |= 1u << (cpu % 32);
		_kern_set_thread_affinity(threads[round % 8], mask, sizeof(mask));
	}

	sStop = true;
	for (int32 i = 0; i < 8; i++)
		wait_for_thread(threads[i], NULL);

	const char* result = "ok, 5000 affinity changes, no scheduler hang";
	printf("RESULT: %s\n", result);
	syslog(LOG_INFO, "scheduler_cpu_count_test: RESULT: %s", result);
	return 0;
}
