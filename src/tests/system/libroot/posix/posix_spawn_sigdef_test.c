/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */

/*!	posix_spawn() with POSIX_SPAWN_SETSIGDEF and a sigfillset() set, as Swift's
	TSC and llbuild use it: the set includes the signals that cannot be caught,
	which must not make the spawn fail, and the child must still get the
	default action for the ones that can.
	Exits with 0 on success.
*/


#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>


int
main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "child") == 0) {
		struct sigaction action;
		if (sigaction(SIGUSR1, NULL, &action) != 0)
			return 3;
		return action.sa_handler == SIG_DFL ? 0 : 2;
	}

	// the child must see SIGUSR1 reset from the inherited SIG_IGN
	signal(SIGUSR1, SIG_IGN);

	posix_spawnattr_t attr;
	sigset_t all;
	sigfillset(&all);
	posix_spawnattr_init(&attr);
	posix_spawnattr_setsigdefault(&attr, &all);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);

	char* childArgs[] = { argv[0], "child", NULL };
	pid_t pid;
	int error = posix_spawn(&pid, argv[0], NULL, &attr, childArgs, NULL);
	posix_spawnattr_destroy(&attr);
	if (error != 0) {
		printf("FAIL: posix_spawn: %s\n", strerror(error));
		return 1;
	}

	int status;
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)
		|| WEXITSTATUS(status) != 0) {
		printf("FAIL: child status %#x (2: SIGUSR1 not reset to SIG_DFL)\n",
			status);
		return 1;
	}

	printf("PASS\n");
	return 0;
}
