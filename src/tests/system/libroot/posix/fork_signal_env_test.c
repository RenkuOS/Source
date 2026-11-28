/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */

/*!	fork() from a signal handler that interrupted setenv() on the same thread.
	POSIX lists fork() as async-signal-safe, but fork() takes the env lock,
	and a handler that ran while setenv() held it deadlocked on it. A worker
	process loops on setenv() while an interval timer's SIGALRM handler
	forks; the test fails if the worker has not done kForkCount forks after
	kTimeout seconds. A signal lands inside setenv() most of the time, so a
	broken libroot hangs within the first few forks.
	Exits with 0 on success.
*/


#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>


enum {
	kForkCount = 500,
	kTimeout = 30
		// seconds
};

static volatile sig_atomic_t sForks;
static volatile sig_atomic_t sForkFailed;


static void
alarm_handler(int signal)
{
	pid_t child = fork();
	if (child == 0)
		_exit(0);
	if (child < 0) {
		sForkFailed = 1;
		return;
	}

	while (waitpid(child, NULL, 0) == -1 && errno == EINTR)
		;
	sForks++;
}


static int
worker(void)
{
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = alarm_handler;
	sigaction(SIGALRM, &action, NULL);

	struct itimerval timer;
	timer.it_interval.tv_sec = 0;
	timer.it_interval.tv_usec = 1000;
	timer.it_value = timer.it_interval;
	setitimer(ITIMER_REAL, &timer, NULL);

	// every setenv() frees the old value and allocates the new one
	while (sForks < kForkCount && !sForkFailed) {
		setenv("FORK_SIGNAL_ENV", "first", 1);
		setenv("FORK_SIGNAL_ENV", "second value", 1);
	}

	return sForkFailed ? 2 : 0;
}


int
main(void)
{
	pid_t child = fork();
	if (child == 0)
		_exit(worker());
	if (child < 0) {
		printf("FAIL: fork(): %s\n", strerror(errno));
		return 1;
	}

	int status = 0;
	for (int i = 0; i < kTimeout * 100; i++) {
		pid_t result = waitpid(child, &status, WNOHANG);
		if (result == child) {
			if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
				printf("PASS: %d forks from a signal handler\n", kForkCount);
				return 0;
			}
			if (WIFSIGNALED(status))
				printf("FAIL: worker signal %d\n", WTERMSIG(status));
			else
				printf("FAIL: worker exit %d\n", WEXITSTATUS(status));
			return 1;
		}
		if (result == -1 && errno != EINTR) {
			printf("FAIL: waitpid(): %s\n", strerror(errno));
			return 1;
		}
		usleep(10000);
	}

	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	printf("FAIL: worker hung (fork() in a signal handler deadlocked)\n");
	return 1;
}
