/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */


/*!	Tests that the runtime loader writes its error messages at the current
	position of stderr: appended to a regular file shared with the parent,
	instead of over its start, and into a pipe, instead of failing with
	ESPIPE. The message is caused by exec'ing a program that does not exist.

	Exits 0 when both cases pass.
*/


#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>

#include <syscalls.h>


static const char kFirstLine[] = "first line\n";
static const char kLastLine[] = "last line\n";
static const char kLoaderPrefix[] = "runtime_loader: ";
static const bigtime_t kTimeout = 10000000;
static const int kExecReturned = 10;

// The runtime loader of the exec'ed image fails to open the missing program
// and exits the team with this error, of which waitpid() reports the low byte.
static const int kNotFoundExitCode = B_ENTRY_NOT_FOUND & 0xff;


/*!	Runs in the forked child: makes \a fd its stderr, closes \a closeFD if it
	is not -1, and execs a missing program.
*/
static void
exec_missing_program(int fd, int closeFD)
{
	if (dup2(fd, STDERR_FILENO) < 0)
		_exit(kExecReturned + 1);
	if (closeFD >= 0)
		close(closeFD);

	// Bypass libroot's checks, so that the kernel starts the exec. It only
	// fails in the new image's runtime loader, which reports it on stderr.
	static const char kPath[] = "/nonexistent/runtime_loader_stderr_test";
	struct {
		const char*	args[3];
		char		path[sizeof(kPath)];
	} flatArgs;
	flatArgs.args[0] = flatArgs.path;
	flatArgs.args[1] = NULL;
	flatArgs.args[2] = NULL;
	memcpy(flatArgs.path, kPath, sizeof(kPath));

	_kern_exec(kPath, flatArgs.args, sizeof(flatArgs), 1, 0, 022);
	_exit(kExecReturned);
}


/*!	Waits up to kTimeout for \a pid to exit with kNotFoundExitCode. Returns
	whether it did, after printing why not otherwise.
*/
static bool
wait_for_child(const char* name, pid_t pid)
{
	int status;
	bigtime_t end = system_time() + kTimeout;
	pid_t result;
	while ((result = waitpid(pid, &status, WNOHANG)) == 0
		&& system_time() < end) {
		snooze(1000);
	}

	if (result == 0) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		printf("FAIL %s: timed out\n", name);
		return false;
	}

	if (result != pid || !WIFEXITED(status)
		|| WEXITSTATUS(status) != kNotFoundExitCode) {
		printf("FAIL %s: waitpid() = %d, status %#x\n", name, (int)result,
			status);
		return false;
	}
	return true;
}


/*!	Prints a FAIL line for \a name with \a detail and the \a length bytes read,
	escaped so that newlines show. Returns false.
*/
static bool
fail_with_output(const char* name, const char* detail, const char* buffer,
	ssize_t length)
{
	printf("FAIL %s: %s, read \"", name, detail);
	for (ssize_t i = 0; i < length; i++) {
		unsigned char c = buffer[i];
		if (c == '\n')
			printf("\\n");
		else if (c < 0x20 || c >= 0x7f || c == '"' || c == '\\')
			printf("\\x%02x", c);
		else
			putchar(c);
	}
	printf("\"\n");
	return false;
}


static bool
test_regular_file()
{
	const char* name = "into a regular file";
	char path[] = "/tmp/runtime_loader_stderr_test.XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) {
		printf("FAIL %s: mkstemp: %s\n", name, strerror(errno));
		return false;
	}
	// the file stays open until we close it, and is gone in every case
	unlink(path);

	const size_t firstLength = strlen(kFirstLine);
	const size_t lastLength = strlen(kLastLine);
	if (write(fd, kFirstLine, firstLength) != (ssize_t)firstLength) {
		printf("FAIL %s: write: %s\n", name, strerror(errno));
		close(fd);
		return false;
	}

	pid_t pid = fork();
	if (pid == 0)
		exec_missing_program(fd, -1);
	if (pid < 0)
		printf("FAIL %s: fork: %s\n", name, strerror(errno));
	bool ok = pid > 0 && wait_for_child(name, pid);

	// shares the file position with the child
	if (write(fd, kLastLine, lastLength) != (ssize_t)lastLength) {
		printf("FAIL %s: write: %s\n", name, strerror(errno));
		ok = false;
	}

	char buffer[1024];
	ssize_t length = pread(fd, buffer, sizeof(buffer) - 1, 0);
	close(fd);
	if (length < 0) {
		printf("FAIL %s: pread: %s\n", name, strerror(errno));
		return false;
	}
	buffer[length] = '\0';

	// first line, exactly one runtime_loader line, last line
	const char* middle = buffer + firstLength;
	ssize_t middleLength = length - (ssize_t)(firstLength + lastLength);
	if (middleLength <= 0 || memcmp(buffer, kFirstLine, firstLength) != 0
		|| memcmp(middle + middleLength, kLastLine, lastLength) != 0
		|| memchr(middle, '\n', middleLength) != middle + middleLength - 1
		|| strstr(middle, kLoaderPrefix) == NULL) {
		return fail_with_output(name, "unexpected file contents", buffer,
			length);
	}

	if (ok)
		printf("ok   %s\n", name);
	return ok;
}


static bool
test_pipe()
{
	const char* name = "through a pipe";
	int fds[2];
	if (pipe(fds) != 0) {
		printf("FAIL %s: pipe: %s\n", name, strerror(errno));
		return false;
	}

	pid_t pid = fork();
	if (pid == 0)
		exec_missing_program(fds[1], fds[0]);
	close(fds[1]);
	if (pid < 0) {
		printf("FAIL %s: fork: %s\n", name, strerror(errno));
		close(fds[0]);
		return false;
	}

	// The message fits into the pipe, so wait first: the timeout then also
	// covers a child that never closes its end.
	bool ok = wait_for_child(name, pid);

	char buffer[1024];
	ssize_t length = 0;
	ssize_t bytesRead;
	while ((bytesRead = read(fds[0], buffer + length,
			sizeof(buffer) - 1 - length)) > 0) {
		length += bytesRead;
	}
	close(fds[0]);
	buffer[length] = '\0';

	if (strstr(buffer, kLoaderPrefix) == NULL)
		return fail_with_output(name, "no runtime_loader message", buffer,
			length);

	if (ok)
		printf("ok   %s\n", name);
	return ok;
}


int
main()
{
	// don't let the children inherit buffered output
	setvbuf(stdout, NULL, _IONBF, 0);

	bool ok = test_regular_file();
	ok &= test_pipe();

	printf("%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
