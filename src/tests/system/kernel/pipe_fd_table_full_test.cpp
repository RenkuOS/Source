/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


/*!	pipe() and FIFO opens that fail (airos-works/haiku#59).

	With a full FD table, pipe() could not open its read end and then opened
	the write end blocking, which waited forever for a reader: the caller hung
	in _kern_create_pipe instead of getting EMFILE. A failed
	open(fifo, O_WRONLY | O_NONBLOCK) (ENXIO) also left its writer count
	behind, so readers of that FIFO never saw the end of the data.

	Each case runs in a child process with a deadline, so a regression fails
	the test instead of hanging it. Prints one PASS or FAIL line per case;
	the exit status is 0 when all of them passed.
*/


#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>


static const bigtime_t kDeadline = 5000000;

static int sFailures = 0;


static void
report(bool passed, const char* name, const char* format, ...)
{
	char details[256] = "";
	va_list args;
	va_start(args, format);
	vsnprintf(details, sizeof(details), format, args);
	va_end(args);

	printf("%s: %s%s%s\n", passed ? "PASS" : "FAIL", name,
		details[0] != '\0' ? ": " : "", details);
	fflush(stdout);
	if (!passed)
		sFailures++;
}


/*!	Runs \a function in a child process. The child's exit status is the
	error number it reports (0 for success); a child that is still running
	at the deadline is killed and counts as hung.
*/
static void
run_case(const char* name, int (*function)(char* details, size_t size))
{
	int fds[2];
	if (pipe(fds) != 0) {
		report(false, name, "pipe for the result: %s", strerror(errno));
		return;
	}

	pid_t child = fork();
	if (child < 0) {
		report(false, name, "fork: %s", strerror(errno));
		close(fds[0]);
		close(fds[1]);
		return;
	}
	if (child == 0) {
		close(fds[0]);
		char details[200] = "";
		int result = function(details, sizeof(details));
		write(fds[1], details, strlen(details) + 1);
		_exit(result == 0 ? 0 : 1);
	}
	close(fds[1]);

	bigtime_t deadline = system_time() + kDeadline;
	int status = 0;
	bool hung = true;
	while (system_time() < deadline) {
		pid_t done = waitpid(child, &status, WNOHANG);
		if (done == child) {
			hung = false;
			break;
		}
		snooze(10000);
	}
	if (hung) {
		kill(child, SIGKILL);
		waitpid(child, &status, 0);
		report(false, name, "hung for %d s", (int)(kDeadline / 1000000));
		close(fds[0]);
		return;
	}

	char details[200] = "";
	ssize_t bytes = read(fds[0], details, sizeof(details) - 1);
	details[bytes > 0 ? bytes : 0] = '\0';
	close(fds[0]);

	report(WIFEXITED(status) && WEXITSTATUS(status) == 0, name, "%s", details);
}


static int
pipe_with_full_table(char* details, size_t size)
{
	// fill the FD table
	while (dup(0) >= 0)
		;
	if (errno != EMFILE) {
		snprintf(details, size, "dup() ended with %s", strerror(errno));
		syslog(LOG_INFO, "pipe_fd_table_full_test: return 1");
		return 1;
	}

	int fds[2];
	if (pipe(fds) == 0) {
		snprintf(details, size, "pipe() succeeded with a full table");
		syslog(LOG_INFO, "pipe_fd_table_full_test: return 1");
		return 1;
	}
	int error = errno;
	snprintf(details, size, "pipe() failed with %s", strerror(error));
	return error == EMFILE ? 0 : 1;
}


static int
pipe_with_one_free_fd(char* details, size_t size)
{
	int last = -1;
	int fd;
	while ((fd = dup(0)) >= 0)
		last = fd;
	if (last < 0) {
		snprintf(details, size, "could not fill the table");
		syslog(LOG_INFO, "pipe_fd_table_full_test: return 1");
		return 1;
	}
	close(last);

	int fds[2];
	if (pipe(fds) == 0) {
		snprintf(details, size, "pipe() succeeded with one free FD");
		syslog(LOG_INFO, "pipe_fd_table_full_test: return 1");
		return 1;
	}
	int error = errno;
	snprintf(details, size, "pipe() failed with %s", strerror(error));
	return error == EMFILE ? 0 : 1;
}


static int
pipe_repeated_failures(char* details, size_t size)
{
	// Failed pipe() calls must not leak anything: they used to keep their
	// FIFO node. Afterwards a pipe must still work.
	int fd;
	int filled[4096];
	int count = 0;
	while ((fd = dup(0)) >= 0 && count < 4096)
		filled[count++] = fd;

	for (int i = 0; i < 2000; i++) {
		int fds[2];
		if (pipe(fds) == 0) {
			snprintf(details, size, "pipe() succeeded with a full table");
			syslog(LOG_INFO, "pipe_fd_table_full_test: return 1");
			return 1;
		}
	}

	for (int i = 0; i < count; i++)
		close(filled[i]);

	int fds[2];
	if (pipe(fds) != 0) {
		snprintf(details, size, "pipe() after 2000 failures: %s",
			strerror(errno));
		syslog(LOG_INFO, "pipe_fd_table_full_test: return 1");
		return 1;
	}
	snprintf(details, size, "2000 failed pipe() calls, then one worked");
	syslog(LOG_INFO, "pipe_fd_table_full_test: return 0");
	return 0;
}


static int
pipe_blocking_flags(char* details, size_t size)
{
	int fds[2];
	if (pipe(fds) != 0) {
		snprintf(details, size, "pipe(): %s", strerror(errno));
		syslog(LOG_INFO, "pipe_fd_table_full_test: return 1");
		return 1;
	}
	int readFlags = fcntl(fds[0], F_GETFL);
	int writeFlags = fcntl(fds[1], F_GETFL);
	if ((readFlags & O_NONBLOCK) != 0 || (writeFlags & O_NONBLOCK) != 0) {
		snprintf(details, size, "O_NONBLOCK set: read end %d, write end %d",
			(readFlags & O_NONBLOCK) != 0, (writeFlags & O_NONBLOCK) != 0);
		syslog(LOG_INFO, "pipe_fd_table_full_test: return 1");
		return 1;
	}
	if ((readFlags & O_ACCMODE) != O_RDONLY
		|| (writeFlags & O_ACCMODE) != O_WRONLY) {
		snprintf(details, size, "access modes %#x, %#x", readFlags,
			writeFlags);
		syslog(LOG_INFO, "pipe_fd_table_full_test: return 1");
		return 1;
	}

	if (write(fds[1], "hello", 6) != 6) {
		snprintf(details, size, "write: %s", strerror(errno));
		syslog(LOG_INFO, "pipe_fd_table_full_test: return 1");
		return 1;
	}
	char buffer[16];
	if (read(fds[0], buffer, sizeof(buffer)) != 6
		|| strcmp(buffer, "hello") != 0) {
		snprintf(details, size, "read back failed");
		syslog(LOG_INFO, "pipe_fd_table_full_test: return 1");
		return 1;
	}
	close(fds[1]);
	ssize_t bytes = read(fds[0], buffer, sizeof(buffer));
	if (bytes != 0) {
		snprintf(details, size, "read after the writer closed: %zd", bytes);
		syslog(LOG_INFO, "pipe_fd_table_full_test: return 1");
		return 1;
	}
	snprintf(details, size, "blocking ends, data and EOF");
	syslog(LOG_INFO, "pipe_fd_table_full_test: return 0");
	return 0;
}


static int
pipe2_nonblocking_flags(char* details, size_t size)
{
	int fds[2];
	if (pipe2(fds, O_NONBLOCK) != 0) {
		snprintf(details, size, "pipe2(): %s", strerror(errno));
		syslog(LOG_INFO, "pipe_fd_table_full_test: return 1");
		return 1;
	}
	int readFlags = fcntl(fds[0], F_GETFL);
	int writeFlags = fcntl(fds[1], F_GETFL);
	if ((readFlags & O_NONBLOCK) == 0 || (writeFlags & O_NONBLOCK) == 0) {
		snprintf(details, size, "O_NONBLOCK missing: read end %d, write end %d",
			(readFlags & O_NONBLOCK) != 0, (writeFlags & O_NONBLOCK) != 0);
		syslog(LOG_INFO, "pipe_fd_table_full_test: return 1");
		return 1;
	}
	char buffer[4];
	if (read(fds[0], buffer, sizeof(buffer)) != -1 || errno != EAGAIN) {
		snprintf(details, size, "empty non-blocking read did not say EAGAIN");
		syslog(LOG_INFO, "pipe_fd_table_full_test: return 1");
		return 1;
	}
	snprintf(details, size, "both ends non-blocking");
	syslog(LOG_INFO, "pipe_fd_table_full_test: return 0");
	return 0;
}


static int
fifo_failed_write_open(char* details, size_t size)
{
	// somewhere a FIFO can be made: a CD boot's overlay takes no special
	// nodes, but the root file system (rootfs, in memory) does
	const char* const kDirectories[] = { getenv("TMPDIR"), "/tmp",
		"/var/tmp", "/boot/home", "" };
	char path[PATH_MAX];
	bool created = false;
	for (size_t i = 0; !created
			&& i < sizeof(kDirectories) / sizeof(kDirectories[0]); i++) {
		if (kDirectories[i] == NULL)
			continue;
		snprintf(path, sizeof(path), "%s/pipe_fd_table_full_test.%d",
			kDirectories[i], (int)getpid());
		unlink(path);
		created = mkfifo(path, 0600) == 0;
	}
	if (!created) {
		// nowhere to put a FIFO: nothing to check
		snprintf(details, size, "SKIP, no writable directory for a FIFO (%s)",
			strerror(errno));
		syslog(LOG_INFO, "pipe_fd_table_full_test: return 0");
		return 0;
	}

	int result = 1;
	int reader = -1;
	int writer = -1;
	char buffer[4];
	ssize_t bytes;

	// no reader: must fail with ENXIO and leave no writer behind
	int fd = open(path, O_WRONLY | O_NONBLOCK);
	if (fd >= 0 || errno != ENXIO) {
		snprintf(details, size, "write-only open without a reader: %s",
			fd >= 0 ? "succeeded" : strerror(errno));
		goto out;
	}

	reader = open(path, O_RDONLY | O_NONBLOCK);
	if (reader < 0) {
		snprintf(details, size, "reader open: %s", strerror(errno));
		goto out;
	}
	writer = open(path, O_WRONLY | O_NONBLOCK);
	if (writer < 0) {
		snprintf(details, size, "writer open: %s", strerror(errno));
		goto out;
	}
	close(writer);
	writer = -1;

	// the only writer is gone: end of data, not EAGAIN
	bytes = read(reader, buffer, sizeof(buffer));
	if (bytes != 0) {
		snprintf(details, size, "read after the last writer closed: %zd (%s)",
			bytes, bytes < 0 ? strerror(errno) : "data");
		goto out;
	}
	snprintf(details, size, "ENXIO left no writer behind");
	result = 0;

out:
	if (reader >= 0)
		close(reader);
	if (writer >= 0)
		close(writer);
	unlink(path);
	return result;
}


int
main()
{
	run_case("pipe() with a full FD table", pipe_with_full_table);
	run_case("pipe() with one free FD", pipe_with_one_free_fd);
	run_case("repeated pipe() failures", pipe_repeated_failures);
	run_case("pipe() is blocking", pipe_blocking_flags);
	run_case("pipe2(O_NONBLOCK)", pipe2_nonblocking_flags);
	run_case("FIFO write-only open without a reader",
		fifo_failed_write_open);

	printf("%s\n", sFailures == 0 ? "all passed" : "FAILURES");
	return sFailures == 0 ? 0 : 1;
}
