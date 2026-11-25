/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */

/*!	posix_spawn() of a missing file while a SIGALRM handler without
	SA_RESTART runs every kInterval microseconds. The parent reads the child's
	exec*() error from a pipe; when a signal interrupted that read,
	posix_spawn() returned 0 for a child that then exited with 127. Every call
	must return ENOENT. Exits with 0 on success.
*/


#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>


enum {
	kSpawnCount = 2000,
	kInterval = 200
		// microseconds
};

static volatile int sAlarms;


static void
alarm_handler(int signal)
{
	sAlarms++;
}


int
main(void)
{
	// older libroot flushed stdio in _exit(), so a child would repeat
	// buffered output
	setvbuf(stdout, NULL, _IONBF, 0);

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = alarm_handler;
		// no SA_RESTART, so blocking calls fail with EINTR
	sigaction(SIGALRM, &action, NULL);

	struct itimerval timer;
	timer.it_interval.tv_sec = 0;
	timer.it_interval.tv_usec = kInterval;
	timer.it_value = timer.it_interval;
	setitimer(ITIMER_REAL, &timer, NULL);

	// with an attribute set, posix_spawn() uses vfork() instead of load_image()
	posix_spawnattr_t attr;
	sigset_t noSignals;
	sigemptyset(&noSignals);
	posix_spawnattr_init(&attr);
	posix_spawnattr_setsigmask(&attr, &noSignals);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);

	char* args[] = { "/nonexistent/posix_spawn_eintr_test", NULL };
	int missed = 0;
	int failed = 0;
	for (int i = 0; i < kSpawnCount; i++) {
		pid_t child = -1;
		int error = posix_spawn(&child, args[0], NULL, &attr, args, NULL);
		if (error == 0)
			missed++;
		else if (error != ENOENT && failed++ == 0)
			printf("posix_spawn() %d: %s\n", i + 1, strerror(error));

		// reap the child, unless posix_spawn() already did
		if (child > 0) {
			while (waitpid(child, NULL, 0) == -1 && errno == EINTR)
				;
		}
	}

	memset(&timer, 0, sizeof(timer));
	setitimer(ITIMER_REAL, &timer, NULL);
	posix_spawnattr_destroy(&attr);

	if (missed != 0 || failed != 0 || sAlarms == 0) {
		printf("FAIL: %d of %d spawns returned 0, %d other errors, %d alarms\n",
			missed, kSpawnCount, failed, sAlarms);
		return 1;
	}

	printf("PASS: %d spawns returned ENOENT, %d alarms\n", kSpawnCount,
		sAlarms);
	return 0;
}
