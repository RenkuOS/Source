/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include "ParallelRows.h"

#include <new>

#include <OS.h>


/*!	The app_server draws and copies on the thread of the window that asked
	for it. Large areas (video frames) are memory bound; on the StarFive
	JH7110 four cores move about 700 MB/s into the uncached frame buffer
	where one moves 450 MB/s, which is the difference between a 1080p video
	playing smoothly or not.
*/


static const int32 kMaxHelpers = 3;
static const size_t kMinimumBytes = 256 * 1024;


namespace {

struct Job {
	parallel_rows_function	function;
	void*					cookie;
	int32					first;
	int32					count;
};


struct HelperArgs {
	int32					index;
};


static int32 sBusy = 0;
static bool sInitDone = false;
static int32 sHelperCount = 0;
static sem_id sStart[kMaxHelpers];
static sem_id sDone = -1;
static Job sJobs[kMaxHelpers];


static status_t
helper_thread(void* data)
{
	int32 index = (int32)(addr_t)data;

	while (true) {
		status_t status;
		do {
			status = acquire_sem(sStart[index]);
		} while (status == B_INTERRUPTED);
		if (status != B_OK)
			return status;

		const Job& job = sJobs[index];
		job.function(job.cookie, job.first, job.count);
		release_sem_etc(sDone, 1, B_DO_NOT_RESCHEDULE);
	}
	return B_OK;
}


static void
init_helpers()
{
	sInitDone = true;

	system_info info;
	get_system_info(&info);
	int32 helpers = min_c((int32)info.cpu_count - 1, kMaxHelpers);
	if (helpers <= 0)
		return;

	sDone = create_sem(0, "parallel rows done");
	if (sDone < 0)
		return;

	for (int32 i = 0; i < helpers; i++) {
		sStart[i] = create_sem(0, "parallel rows start");
		if (sStart[i] < 0)
			break;
		thread_id thread = spawn_thread(helper_thread, "parallel rows",
			B_URGENT_DISPLAY_PRIORITY, (void*)(addr_t)i);
		if (thread < 0 || resume_thread(thread) != B_OK) {
			delete_sem(sStart[i]);
			break;
		}
		sHelperCount++;
	}
}

}	// namespace


bool
parallel_rows(int32 rows, size_t totalBytes, parallel_rows_function function,
	void* cookie)
{
	if (rows < 2 * (kMaxHelpers + 1) || totalBytes < kMinimumBytes)
		return false;

	// one caller at a time; a second one does its work alone rather than
	// waiting for the first
	if (atomic_test_and_set(&sBusy, 1, 0) != 0)
		return false;

	if (!sInitDone)
		init_helpers();
	if (sHelperCount == 0) {
		atomic_set(&sBusy, 0);
		return false;
	}

	int32 parts = sHelperCount + 1;
	int32 first = 0;
	for (int32 i = 0; i < parts; i++) {
		int32 next = (int32)((int64)rows * (i + 1) / parts);
		if (i < sHelperCount) {
			sJobs[i].function = function;
			sJobs[i].cookie = cookie;
			sJobs[i].first = first;
			sJobs[i].count = next - first;
			release_sem_etc(sStart[i], 1, B_DO_NOT_RESCHEDULE);
		} else
			function(cookie, first, next - first);
		first = next;
	}

	status_t status;
	do {
		status = acquire_sem_etc(sDone, sHelperCount, 0, 0);
	} while (status == B_INTERRUPTED);

	atomic_set(&sBusy, 0);
	return true;
}
