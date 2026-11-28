/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */

/*!	A forked child inherits the parent's unflushed stdio buffer. POSIX says
	_exit() and _Exit() must not flush streams, so a child calling them must
	not write that buffer a second time. The exit() case is the control: it
	flushes, so the marker must appear twice. Exits with 0 on success.
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


static const char* const kMarker = "exit_stdio_test marker\n";


/*!	Writes kMarker unflushed to a temporary file, forks a child that ends with
	\a how, and returns how often the marker is in the file afterwards, or -1
	on error.
*/
static int
count_markers(const char* how)
{
	FILE* file = tmpfile();
	if (file == NULL) {
		perror("tmpfile");
		return -1;
	}
	static char buffer[BUFSIZ];
	setvbuf(file, buffer, _IOFBF, sizeof(buffer));
	fputs(kMarker, file);

	pid_t child = fork();
	if (child < 0) {
		perror("fork");
		fclose(file);
		return -1;
	}
	if (child == 0) {
		if (strcmp(how, "_exit") == 0)
			_exit(0);
		if (strcmp(how, "_Exit") == 0)
			_Exit(0);
		exit(0);
	}

	int status;
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status)
		|| WEXITSTATUS(status) != 0) {
		fprintf(stderr, "child for %s did not exit cleanly\n", how);
		fclose(file);
		return -1;
	}

	// flushes the parent's copy, then read everything back
	rewind(file);
	char line[64];
	int count = 0;
	while (fgets(line, sizeof(line), file) != NULL) {
		if (strcmp(line, kMarker) == 0)
			count++;
	}
	fclose(file);
	return count;
}


int
main(void)
{
	static const struct {
		const char* how;
		int expected;
	} kCases[] = {
		{ "_exit", 1 },
		{ "_Exit", 1 },
		{ "exit", 2 },
	};

	// keeps a flushing child from repeating our own output
	setvbuf(stdout, NULL, _IONBF, 0);

	int failures = 0;
	for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
		int count = count_markers(kCases[i].how);
		if (count != kCases[i].expected) {
			printf("FAIL: child %s(): marker found %d times, expected %d\n",
				kCases[i].how, count, kCases[i].expected);
			failures++;
		}
	}

	if (failures != 0)
		return 1;

	printf("PASS\n");
	return 0;
}
