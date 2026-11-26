/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */


/*!	Tests that fork() works while the heap switches to multi-threaded mode,
	which libroot does when a team's first thread starts. fork() must lock the
	same heap mutexes it later unlocks; if it does not, the parent stops in
	the debugger with "mutex was not actually locked!".

	The test runs rounds. Each round forks a child that disables the debugger
	(so the bug makes the child exit with 1 instead), starts its first thread
	and forks in a loop until that thread runs. The loop also calls malloc(),
	which holds the heap mutex the switch waits for and widens the window.

	"heap_fork_threaded_init_test [<rounds>]" exits 0 when every round passes.
*/


#include <OS.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


static const int kDefaultRounds = 1000;
static const int kMaxReported = 5;
static const unsigned kTimeout = 300;
	// seconds

static volatile bool sThreadRan;
static volatile bool sRoundDone;


static void
timed_out(int)
{
	static const char kMessage[] = "FAIL: timed out\n";
	write(STDOUT_FILENO, kMessage, sizeof(kMessage) - 1);
	_exit(1);
}


static void*
wait_for_round(void*)
{
	sThreadRan = true;

	// don't exit during a fork(): that trips a separate kernel race
	while (!sRoundDone)
		snooze(1000);
	return NULL;
}


/*!	Body of a round child: the heap is still single-threaded here, because
	the main process never starts a thread.
*/
static int
run_round()
{
	disable_debugger(1);

	pthread_t thread;
	if (pthread_create(&thread, NULL, wait_for_round, NULL) != 0)
		return 2;

	while (!sThreadRan) {
		free(malloc(64));

		pid_t child = fork();
		if (child == 0)
			_exit(0);
		if (child < 0)
			return 3;
		waitpid(child, NULL, 0);
	}

	sRoundDone = true;
	pthread_join(thread, NULL);
	return 0;
}


int
main(int argc, char** argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : kDefaultRounds;
	int failed = 0;

	// _exit() flushes stdio, so the children would repeat buffered output
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGALRM, timed_out);
	alarm(kTimeout);

	for (int i = 0; i < rounds; i++) {
		pid_t child = fork();
		if (child == 0)
			_exit(run_round());
		if (child < 0) {
			printf("FAIL: fork: %s\n", strerror(errno));
			return 1;
		}

		int status = 0;
		pid_t result = waitpid(child, &status, 0);
		if (result != child || !WIFEXITED(status)
			|| WEXITSTATUS(status) != 0) {
			if (failed++ < kMaxReported) {
				printf("round %d: waitpid() = %d, status %#x\n", i,
					(int)result, status);
			}
		}
	}

	if (failed != 0) {
		printf("FAIL: %d of %d rounds failed\n", failed, rounds);
		return 1;
	}

	printf("PASS: %d rounds\n", rounds);
	return 0;
}
