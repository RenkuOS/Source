/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */

/*!	posix_spawn() and posix_spawnp() while other threads keep calling malloc(),
	free() and getenv(), as swift-driver does with -j 8: the child runs exec*()
	code that takes the heap and env locks, so it hangs if vfork() copied one
	of them while another thread held it. A child that has not exited after
	kTimeout seconds is killed and fails the test instead of hanging it.
	Exits with 0 on success.
*/


#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


enum {
	kThreadCount = 8,
	kSlotCount = 256,
	kSpawnCount = 2000,
	kTimeout = 5
		// seconds
};

static void* sSlots[kSlotCount];
static volatile int sStop;
static volatile pid_t sChild;
static volatile int sTimedOut;


static void
timeout_handler(int signal)
{
	pid_t child = sChild;
	if (child > 0) {
		kill(child, SIGKILL);
		sTimedOut = 1;
	}
}


static void*
churn(void* data)
{
	unsigned seed = (unsigned)(intptr_t)data;

	while (!sStop) {
		// blocks are freed by any thread, so every heap lock gets taken by all
		void* block = malloc((size_t)16 << (rand_r(&seed) % 15));
		free(__atomic_exchange_n(&sSlots[rand_r(&seed) % kSlotCount], block,
			__ATOMIC_ACQ_REL));
		getenv("PATH");
	}
	return NULL;
}


int
main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "child") == 0)
		return 0;

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = timeout_handler;
	sigaction(SIGALRM, &action, NULL);

	// only the main thread takes the SIGALRM
	sigset_t alarmSet;
	sigemptyset(&alarmSet);
	sigaddset(&alarmSet, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &alarmSet, NULL);
	pthread_t threads[kThreadCount];
	for (int i = 0; i < kThreadCount; i++)
		pthread_create(&threads[i], NULL, churn, (void*)(intptr_t)(i + 1));
	pthread_sigmask(SIG_UNBLOCK, &alarmSet, NULL);

	// with an attribute set, posix_spawn() uses vfork() instead of load_image()
	posix_spawnattr_t attr;
	sigset_t noSignals;
	sigemptyset(&noSignals);
	posix_spawnattr_init(&attr);
	posix_spawnattr_setsigmask(&attr, &noSignals);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);

	char* selfArgs[] = { argv[0], "child", NULL };
	char* trueArgs[] = { "true", NULL };
	int result = 0;
	for (int i = 0; i < kSpawnCount && result == 0; i++) {
		// posix_spawnp() looks "true" up in PATH, in the child
		const char* call = (i % 2) != 0 ? "posix_spawnp" : "posix_spawn";
		int error;

		sChild = 0;
		alarm(kTimeout);
		if ((i % 2) != 0) {
			error = posix_spawnp((pid_t*)&sChild, "true", NULL, &attr,
				trueArgs, NULL);
		} else {
			error = posix_spawn((pid_t*)&sChild, argv[0], NULL, &attr,
				selfArgs, NULL);
		}
		pid_t child = sChild;
		int status = 0;
		if (error == 0) {
			while (waitpid(child, &status, 0) == -1 && errno == EINTR)
				;
		}
		sChild = 0;
		alarm(0);

		if (error != 0) {
			printf("FAIL: %s() %d: %s\n", call, i + 1, strerror(error));
			result = 1;
		} else if (sTimedOut) {
			printf("FAIL: %s() child %d hung before exiting (spawn %d of %d)\n",
				call, (int)child, i + 1, kSpawnCount);
			result = 1;
		} else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			printf("FAIL: %s() child status %#x\n", call, status);
			result = 1;
		}
	}

	sStop = 1;
	for (int i = 0; i < kThreadCount; i++)
		pthread_join(threads[i], NULL);
	posix_spawnattr_destroy(&attr);

	if (result == 0)
		printf("PASS: %d spawns\n", kSpawnCount);
	return result;
}
