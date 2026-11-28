/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */


/*!	Tests that fork() does not fail while other threads of the team exit: one
	thread forks many times while another keeps creating threads that exit at
	once, so the areas fork() copies keep losing thread stacks. Every fork()
	must succeed and every child must exit normally with the expected code.

	"fork_thread_exit_test [<forks>]" exits 0 when every fork passes.
*/


#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


static const int kChildExitCode = 7;
static const int kDefaultForks = 20000;
static const int kMaxReported = 5;
static const unsigned kTimeout = 300;
	// seconds

static volatile bool sChurning;
static volatile bool sDone;


static void
timed_out(int)
{
	static const char kMessage[] = "FAIL: timed out\n";
	write(STDOUT_FILENO, kMessage, sizeof(kMessage) - 1);
	_exit(1);
}


static void*
exit_at_once(void*)
{
	return NULL;
}


static void*
churn_threads(void*)
{
	sChurning = true;
	while (!sDone) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, exit_at_once, NULL) == 0)
			pthread_join(thread, NULL);
	}
	return NULL;
}


int
main(int argc, char** argv)
{
	int forks = argc > 1 ? atoi(argv[1]) : kDefaultForks;
	int failed = 0;

	// _exit() flushes stdio, so the children would repeat buffered output
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGALRM, timed_out);
	alarm(kTimeout);

	pthread_t churner;
	int error = pthread_create(&churner, NULL, churn_threads, NULL);
	if (error != 0) {
		printf("FAIL: pthread_create: %s\n", strerror(error));
		return 1;
	}

	// libroot's heap goes multi-threaded when the first thread starts, which
	// races with fork(); don't test that here
	while (!sChurning)
		usleep(1000);

	for (int i = 0; i < forks; i++) {
		pid_t child = fork();
		if (child == 0)
			_exit(kChildExitCode);
		if (child < 0) {
			if (failed++ < kMaxReported)
				printf("fork %d: %s (%#x)\n", i, strerror(errno), errno);
			continue;
		}

		int status = 0;
		pid_t result = waitpid(child, &status, 0);
		if (result != child || !WIFEXITED(status)
			|| WEXITSTATUS(status) != kChildExitCode) {
			if (failed++ < kMaxReported) {
				printf("fork %d: waitpid() = %d, status %#x\n", i, (int)result,
					status);
			}
		}
	}

	sDone = true;
	pthread_join(churner, NULL);

	if (failed != 0) {
		printf("FAIL: %d of %d forks failed\n", failed, forks);
		return 1;
	}

	printf("PASS: %d forks\n", forks);
	return 0;
}
