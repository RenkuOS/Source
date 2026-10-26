/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	The first localtime_r() of a process loads the ICU locale backend. Before
	the backend was published only once initialized, threads scanning numbers
	at that moment saw an empty ctype table (sscanf("3.1", "%i.%i") returned
	0), and threads making their own first time conversion could use the
	backend before its initialization. Every child process here starts cold.
*/


#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>


#define kProcesses		20
#define kScanners		3
#define kConverters		4


static volatile bool sStart = false;
static volatile bool sDone = false;
static int32_t sReady = 0;
static int32_t sFailures = 0;
static int32_t sScans = 0;


static void*
scanner(void* unused)
{
	__atomic_add_fetch(&sReady, 1, __ATOMIC_SEQ_CST);
	while (!__atomic_load_n(&sStart, __ATOMIC_ACQUIRE))
		;

	do {
		int major = -1, minor = -1;
		if (sscanf("3.1 Mesa 25.3.6", "%i.%i", &major, &minor) != 2
				|| major != 3 || minor != 1) {
			__atomic_add_fetch(&sFailures, 1, __ATOMIC_SEQ_CST);
		}
		__atomic_add_fetch(&sScans, 1, __ATOMIC_SEQ_CST);
	} while (!__atomic_load_n(&sDone, __ATOMIC_ACQUIRE));

	return NULL;
}


static void*
converter(void* unused)
{
	__atomic_add_fetch(&sReady, 1, __ATOMIC_SEQ_CST);
	while (!__atomic_load_n(&sStart, __ATOMIC_ACQUIRE))
		;

	time_t now = time(NULL);
	struct tm converted;
	if (localtime_r(&now, &converted) == NULL || converted.tm_year < 120)
		__atomic_add_fetch(&sFailures, 1, __ATOMIC_SEQ_CST);

	return NULL;
}


static int
run_cold()
{
	pthread_t threads[kScanners + kConverters];
	for (int i = 0; i < kScanners + kConverters; i++) {
		if (pthread_create(&threads[i], NULL,
				i < kScanners ? scanner : converter, NULL) != 0) {
			perror("pthread_create");
			return 2;
		}
	}

	while (__atomic_load_n(&sReady, __ATOMIC_SEQ_CST)
			!= kScanners + kConverters)
		;
	__atomic_store_n(&sStart, true, __ATOMIC_RELEASE);

	while (__atomic_load_n(&sScans, __ATOMIC_SEQ_CST) < 1000)
		;

	time_t now = time(NULL);
	struct tm converted;
	if (localtime_r(&now, &converted) == NULL)
		__atomic_add_fetch(&sFailures, 1, __ATOMIC_SEQ_CST);

	__atomic_store_n(&sDone, true, __ATOMIC_RELEASE);
	for (int i = 0; i < kScanners + kConverters; i++)
		pthread_join(threads[i], NULL);

	return sFailures != 0 ? 1 : 0;
}


int
main()
{
	int failed = 0;
	for (int i = 0; i < kProcesses; i++) {
		pid_t child = fork();
		if (child < 0) {
			perror("fork");
			return 2;
		}
		if (child == 0)
			exit(run_cold());

		int status;
		if (waitpid(child, &status, 0) != child || !WIFEXITED(status)
				|| WEXITSTATUS(status) != 0) {
			failed++;
		}
	}

	printf("%d of %d cold processes saw a bad scan or time conversion\n",
		failed, kProcesses);
	return failed != 0 ? 1 : 0;
}
