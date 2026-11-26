/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */

/*!	malloc_usable_size() must report at least the requested size, and all of
	it must be writable. The guarded heap of libroot_debug once lacked the
	hook behind it, so every call jumped to address 0. Run it under the
	default heap and with
	LD_PRELOAD=libroot_debug.so MALLOC_DEBUG=g (guarded heap).
*/


#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static int sFailures = 0;


static void
check(const char* what, void* block, size_t requested)
{
	if (block == NULL) {
		printf("FAIL: %s(%zu) returned NULL\n", what, requested);
		sFailures++;
		return;
	}

	size_t usable = malloc_usable_size(block);
	if (usable < requested) {
		printf("FAIL: %s(%zu): usable size %zu\n", what, requested, usable);
		sFailures++;
	} else {
		// Every reported byte must be writable. The guarded heap reports the
		// requested size, past which only alignment padding is left before
		// its guard page.
		memset(block, 0xa5, usable);
	}

	free(block);
}


int
main()
{
	static const size_t kSizes[] = {
		1, 7, 16, 100, 1000, 4095, 4096, 5000, 65536, 2 * 1024 * 1024
	};

	if (malloc_usable_size(NULL) != 0) {
		printf("FAIL: malloc_usable_size(NULL) != 0\n");
		sFailures++;
	}

	// malloc(0) may return NULL, or a block of which no byte need be usable
	void* empty = malloc(0);
	if (empty != NULL)
		check("malloc", empty, 0);

	for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++) {
		size_t size = kSizes[i];
		check("malloc", malloc(size), size);
		check("calloc", calloc(1, size), size);
		check("memalign(64)", memalign(64, size), size);
		check("memalign(16384)", memalign(16384, size), size);

		void* block = malloc(1);
		check("realloc", block != NULL ? realloc(block, size) : NULL, size);
	}

	if (sFailures != 0) {
		printf("%d failure(s)\n", sFailures);
		return 1;
	}

	printf("malloc_usable_size_test: pass\n");
	return 0;
}
