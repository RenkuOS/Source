/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	vfork() + exec while other threads use malloc().

	exec allocates memory in the child (it flattens the arguments and the
	environment). vfork() did not take the heap's locks around the fork, so a
	child forked while another thread held one of them waited for it forever,
	and so did a parent waiting for the child (posix_spawn() reads a pipe
	until the exec). That hung net_server's service launch at boot on the
	ROCK 5 ITX.

	Allocating threads run while the main thread vforks and execs /bin/true
	over and over; every child has to be gone within a few seconds. The rounds
	stop after kRounds or kTimeBudget, whichever comes first: under QEMU's
	arm64 emulation 400 rounds took 55 to 65 s, and the smoke test kills a
	guest test after 60 s. Without the fix the test hung in round 0.
*/

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>


static const int kAllocatorCount = 32;
	// enough threads for every heap pool: a thread's pool is its malloc ID
	// modulo the pool count, and the forking thread's must be busy too
static const int kRounds = 400;
static const bigtime_t kTimeBudget = 30000000;
static const bigtime_t kChildDeadline = 5000000;

static volatile bool sStop = false;


static void*
allocator(void*)
{
	void* blocks[64] = {};
	unsigned seed = (unsigned)find_thread(NULL);
	while (!sStop) {
		int index = rand_r(&seed) % 64;
		free(blocks[index]);
		blocks[index] = malloc(16 + rand_r(&seed) % 4096);
	}
	for (int i = 0; i < 64; i++)
		free(blocks[i]);
	return NULL;
}


int
main()
{
	pthread_t threads[kAllocatorCount];
	for (int i = 0; i < kAllocatorCount; i++)
		pthread_create(&threads[i], NULL, allocator, NULL);

	int failures = 0;
	int rounds = 0;
	bigtime_t start = system_time();
	for (int round = 0; round < kRounds && failures == 0
			&& system_time() - start < kTimeBudget; round++) {
		rounds++;
		pid_t child = vfork();
		if (child == 0) {
			char* const args[] = { (char*)"true", NULL };
			execv("/bin/true", args);
			_exit(127);
		}
		if (child < 0) {
			printf("FAIL: vfork() failed: %s\n", strerror(errno));
			failures++;
			break;
		}

		bigtime_t deadline = system_time() + kChildDeadline;
		while (true) {
			int status;
			pid_t result = waitpid(child, &status, WNOHANG);
			if (result == child) {
				if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
					printf("FAIL: round %d: the child did not exec /bin/true"
						" (status %#x)\n", round, status);
					failures++;
				}
				break;
			}
			if (system_time() > deadline) {
				printf("FAIL: round %d: the child is still there after %d s"
					" (it waits for a heap lock it never gets)\n", round,
					(int)(kChildDeadline / 1000000));
				kill(child, SIGKILL);
				waitpid(child, NULL, 0);
				failures++;
				break;
			}
			snooze(1000);
		}
	}

	sStop = true;
	for (int i = 0; i < kAllocatorCount; i++)
		pthread_join(threads[i], NULL);

	if (failures == 0)
		printf("PASS: %d vfork() + exec rounds in %.1f s with %d allocating"
			" threads\n", rounds, (system_time() - start) / 1e6,
			kAllocatorCount);
	return failures == 0 ? 0 : 1;
}
