/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */

/*!	exit() must not run the calling thread's pthread cleanup handlers or key
	destructors: POSIX runs those only on pthread_exit() and thread return.
	atexit() handlers and on_exit_thread() callbacks still run. A child sets
	up all four and calls exit(); each one writes a letter to a pipe, which
	the parent checks after the child has ended.
	Exits with 0 on success.
*/


#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>


static int sPipe;


static void
record(char event)
{
	write(sPipe, &event, 1);
}


static void
atexit_handler(void)
{
	record('A');
}


static void
exit_thread_callback(void* data)
{
	record('T');
}


static void
cleanup_handler(void* data)
{
	record('C');
}


static void
key_destructor(void* value)
{
	record('D');
}


static void
child(void)
{
	pthread_key_t key;
	if (pthread_key_create(&key, key_destructor) != 0
		|| pthread_setspecific(key, (void*)1) != 0
		|| atexit(atexit_handler) != 0
		|| on_exit_thread(exit_thread_callback, NULL) != B_OK) {
		record('E');
		_exit(1);
	}

	pthread_cleanup_push(cleanup_handler, NULL);
	exit(0);
	pthread_cleanup_pop(0);
}


int
main(void)
{
	int fds[2];
	if (pipe(fds) != 0) {
		printf("FAIL: pipe(): %s\n", strerror(errno));
		return 1;
	}

	pid_t pid = fork();
	if (pid == 0) {
		close(fds[0]);
		sPipe = fds[1];
		child();
	}
	close(fds[1]);
	if (pid < 0) {
		printf("FAIL: fork(): %s\n", strerror(errno));
		return 1;
	}

	char events[16];
	size_t length = 0;
	ssize_t bytes;
	while (length < sizeof(events) - 1
		&& (bytes = read(fds[0], events + length,
			sizeof(events) - 1 - length)) > 0) {
		length += bytes;
	}
	events[length] = '\0';

	int status;
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("FAIL: child did not exit with 0 (events \"%s\")\n", events);
		return 1;
	}

	int failed = 0;
	if (strchr(events, 'A') == NULL) {
		printf("FAIL: the atexit() handler did not run\n");
		failed = 1;
	}
	if (strchr(events, 'T') == NULL) {
		printf("FAIL: the on_exit_thread() callback did not run\n");
		failed = 1;
	}
	if (strchr(events, 'C') != NULL) {
		printf("FAIL: exit() ran a pthread cleanup handler\n");
		failed = 1;
	}
	if (strchr(events, 'D') != NULL) {
		printf("FAIL: exit() ran a key destructor\n");
		failed = 1;
	}
	if (failed)
		return 1;

	printf("PASS: events \"%s\"\n", events);
	return 0;
}
