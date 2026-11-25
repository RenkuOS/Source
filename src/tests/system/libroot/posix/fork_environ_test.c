/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */

/*!	fork() while other threads keep calling setenv() and unsetenv() on many
	variables, so environ grows, shrinks and its strings are replaced. setenv()
	frees the old string before it allocates the new one; a child forked in
	between got an environ that pointed at freed memory. The child walks
	environ, calls getenv() and clearenv(), which frees every string: a
	dangling one is a double free and kills the child. A child that has not
	exited after kTimeout seconds is killed and fails the test as well.
	Exits with 0 on success.
*/


#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>


enum {
	kThreadCount = 4,
	kVariableCount = 64,
	kForkCount = 2000,
	kTimeout = 5
		// seconds
};

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
	int thread = (int)(intptr_t)data;
	char name[32];

	while (!sStop) {
		// add (growing environ), then replace, then remove every variable
		for (int pass = 0; pass < 2; pass++) {
			for (int i = 0; i < kVariableCount; i++) {
				snprintf(name, sizeof(name), "FORK_ENVIRON_%d_%d", thread, i);
				setenv(name, pass == 0 ? "first" : "second value", 1);
			}
		}
		for (int i = 0; i < kVariableCount; i++) {
			snprintf(name, sizeof(name), "FORK_ENVIRON_%d_%d", thread, i);
			unsetenv(name);
		}
	}
	return NULL;
}


static int
check_environ(void)
{
	// a heap error ends the child instead of stopping it in the debugger
	disable_debugger(1);

	for (char** variable = environ; *variable != NULL; variable++) {
		if (strchr(*variable, '=') == NULL)
			return 2;
	}
	if (getenv("PATH") == NULL)
		return 3;
	if (clearenv() != 0)
		return 4;
	return 0;
}


int
main(void)
{
	// older libroot flushed stdio in _exit(), so a child would repeat
	// buffered output
	setvbuf(stdout, NULL, _IONBF, 0);

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
		pthread_create(&threads[i], NULL, churn, (void*)(intptr_t)i);
	pthread_sigmask(SIG_UNBLOCK, &alarmSet, NULL);

	int failed = 0;
	for (int i = 0; i < kForkCount; i++) {
		sTimedOut = 0;
		alarm(kTimeout);
		pid_t child = fork();
		if (child == 0)
			_exit(check_environ());
		if (child < 0) {
			alarm(0);
			printf("FAIL: fork() %d: %s\n", i + 1, strerror(errno));
			failed++;
			break;
		}

		sChild = child;
		int status = 0;
		while (waitpid(child, &status, 0) == -1 && errno == EINTR)
			;
		sChild = 0;
		alarm(0);

		if (!sTimedOut && WIFEXITED(status) && WEXITSTATUS(status) == 0)
			continue;
		if (failed++ >= 5)
			continue;
		if (sTimedOut)
			printf("fork %d: child hung\n", i + 1);
		else if (WIFSIGNALED(status))
			printf("fork %d: child signal %d\n", i + 1, WTERMSIG(status));
		else
			printf("fork %d: child exit %d\n", i + 1, WEXITSTATUS(status));
	}

	sStop = 1;
	for (int i = 0; i < kThreadCount; i++)
		pthread_join(threads[i], NULL);

	if (failed != 0) {
		printf("FAIL: %d of %d children\n", failed, kForkCount);
		return 1;
	}

	printf("PASS: %d forks\n", kForkCount);
	return 0;
}
