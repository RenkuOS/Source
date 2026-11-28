/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */

/*!	Checks that munmap() and mprotect() invalidate the TLBs of other CPUs.

	Reader threads (one per other CPU) read and write every page, then
	spin without touching it, so their TLBs keep the old translations. The
	main thread then remaps the region with new contents, or write-protects
	it, and the readers check they see the new contents, or fault on write.
	Small and large regions cover the per-page and the whole-ASID
	invalidation paths of the arm64 translation map.
*/


#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <OS.h>


static const size_t kPageSize = B_PAGE_SIZE;
static const int kIterations = 300;

static uint8* sRegion;
static size_t sPages;
static volatile int32 sGeneration;
static int32 sDone;
static volatile int32 sMode;	// 0: check contents, 1: write must fault
static int32 sFailures;
static volatile bool sQuit;
static int32 sReaders;

static __thread sigjmp_buf tFaultJump;
static __thread volatile bool tExpectFault;


static void
fault_handler(int signal)
{
	if (!tExpectFault)
		abort();
	siglongjmp(tFaultJump, 1);
}


static void*
reader(void*)
{
	int32 seen = 0;
	while (true) {
		while (sGeneration == seen && !sQuit)
			;
		if (sQuit)
			return NULL;
		seen = sGeneration;
		__atomic_thread_fence(__ATOMIC_ACQUIRE);

		for (size_t i = 0; i < sPages; i++) {
			uint8* page = sRegion + i * kPageSize;
			if (sMode == 0) {
				if (page[0] != (uint8)seen || page[kPageSize - 1] != (uint8)seen) {
					fprintf(stderr, "gen %" B_PRId32 " page %zu: stale value %u\n",
						seen, i, page[0]);
					atomic_add(&sFailures, 1);
				}
				// a write makes the cached translation writable
				page[0] = (uint8)seen;
			} else {
				tExpectFault = true;
				if (sigsetjmp(tFaultJump, 1) == 0) {
					page[0] = (uint8)seen;
					fprintf(stderr, "gen %" B_PRId32 " page %zu: write to read-only "
						"page did not fault\n", seen, i);
					atomic_add(&sFailures, 1);
				}
				tExpectFault = false;
			}
		}
		atomic_add(&sDone, 1);
	}
}


static void
wait_readers()
{
	while (atomic_get(&sDone) != sReaders)
		;
	sDone = 0;
}


static uint8*
map_region(void* at, int32 value)
{
	uint8* region = (uint8*)mmap(at, sPages * kPageSize, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | (at != NULL ? MAP_FIXED : 0), -1, 0);
	if (region == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		exit(1);
	}
	memset(region, value, sPages * kPageSize);
	return region;
}


static void
run(size_t pages)
{
	sPages = pages;
	sRegion = map_region(NULL, 1);
	sGeneration = 0;

	pthread_t threads[sReaders];
	sQuit = false;
	for (int32 i = 0; i < sReaders; i++)
		pthread_create(&threads[i], NULL, reader, NULL);

	for (int32 generation = 1; generation <= kIterations; generation++) {
		// Readers check the current contents and fill their TLBs.
		sMode = 0;
		__atomic_store_n(&sGeneration, generation * 2 - 1, __ATOMIC_RELEASE);
		wait_readers();

		if (generation % 2 == 0) {
			// munmap() + remap with new contents
			munmap(sRegion, pages * kPageSize);
			map_region(sRegion, (uint8)(generation * 2));
			sMode = 0;
		} else {
			// write-protect: writes must fault on every CPU
			memset(sRegion, (uint8)(generation * 2), pages * kPageSize);
			if (mprotect(sRegion, pages * kPageSize, PROT_READ) != 0) {
				fprintf(stderr, "mprotect failed: %s\n", strerror(errno));
				exit(1);
			}
			sMode = 1;
		}
		__atomic_store_n(&sGeneration, generation * 2, __ATOMIC_RELEASE);
		wait_readers();

		if (sMode == 1)
			mprotect(sRegion, pages * kPageSize, PROT_READ | PROT_WRITE);
		memset(sRegion, (uint8)(generation * 2 + 1), pages * kPageSize);
	}

	sQuit = true;
	for (int32 i = 0; i < sReaders; i++)
		pthread_join(threads[i], NULL);
	munmap(sRegion, pages * kPageSize);
}


int
main()
{
	system_info info;
	get_system_info(&info);
	sReaders = info.cpu_count > 1 ? info.cpu_count - 1 : 1;

	struct sigaction action = {};
	action.sa_handler = fault_handler;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);

	// below and above the translation map's batching threshold (64 pages)
	const size_t sizes[] = { 1, 8, 63, 65, 512 };
	for (size_t i = 0; i < B_COUNT_OF(sizes); i++) {
		run(sizes[i]);
		printf("%zu pages: %s\n", sizes[i], sFailures == 0 ? "ok" : "FAILED");
		if (sFailures != 0)
			return 1;
	}
	return 0;
}
