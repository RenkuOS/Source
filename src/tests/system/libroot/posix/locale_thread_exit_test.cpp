/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */


/*!	A C++ thread_local destructor runs after libroot has run the exiting
	thread's on_exit_thread() hooks (libstdc++ implements thread_local
	destructors with a pthread key, and the key destructors come last). One
	of those hooks frees the thread's locale state, so locale functions
	called from such a destructor must not find it again.

	Freed memory usually still holds its old contents, so reading it rarely
	shows. Every round therefore runs in a child process that uses the
	guarded heap of libroot_debug.so (MALLOC_DEBUG=g), where freed memory is
	unmapped and reading it crashes. libroot_debug.so comes with haiku_devel;
	a copy next to the test is used first. A round passes when the destructor sees
	valid locale state: the thread's own locale or the global one.

	Exits 0 when every round passes. Usage: locale_thread_exit_test
*/


#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __HAIKU__
#	include <OS.h>
#endif


extern char** environ;

static const int kRounds = 20;
static const char* kRoundArgument = "--round";
static const char* kDebugLibraryName = "libroot_debug.so";
static const char* kDebugLibraryPaths[] = {
	"/boot/system/lib/libroot_debug.so",
	"/boot/system/develop/lib/libroot_debug.so",
	NULL
};

static char sDebugLibrary[PATH_MAX];

static locale_t sThreadLocale;
static locale_t sSeenInDestructor;
static double sParsedInDestructor;


struct LocaleUser {
	~LocaleUser()
	{
		// Runs during thread exit, after the on_exit_thread() hooks.
		sSeenInDestructor = uselocale((locale_t)0);
		sParsedInDestructor = strtod("2.5", NULL);
	}

	void Touch()
	{
	}
};


static thread_local LocaleUser sUser;


static void*
thread_function(void*)
{
	// Give the thread its own locale state, and register the thread_local
	// object whose destructor runs at exit.
	uselocale(sThreadLocale);
	sUser.Touch();
	return NULL;
}


static int
run_round()
{
#ifdef __HAIKU__
	// A crash should end this child, not wait in the debugger.
	disable_debugger(1);
#endif

	sThreadLocale = newlocale(LC_ALL_MASK, "C", (locale_t)0);
	if (sThreadLocale == (locale_t)0) {
		printf("newlocale() failed\n");
		return 2;
	}

	pthread_t thread;
	if (pthread_create(&thread, NULL, thread_function, NULL) != 0) {
		printf("pthread_create() failed\n");
		return 2;
	}
	pthread_join(thread, NULL);

	bool valid = sSeenInDestructor == sThreadLocale
		|| sSeenInDestructor == LC_GLOBAL_LOCALE;
	if (!valid || sParsedInDestructor != 2.5) {
		printf("destructor saw invalid locale state, strtod() gave %g\n",
			sParsedInDestructor);
		return 1;
	}
	return 0;
}


/*!	Looks for libroot_debug.so next to this program first, then where
	haiku_devel installs it. Returns false when there is none.
*/
static bool
find_debug_library(const char* program)
{
	const char* slash = strrchr(program, '/');
	if (slash != NULL) {
		snprintf(sDebugLibrary, sizeof(sDebugLibrary), "%.*s/%s",
			(int)(slash - program), program, kDebugLibraryName);
		if (access(sDebugLibrary, R_OK) == 0)
			return true;
	}
	for (int i = 0; kDebugLibraryPaths[i] != NULL; i++) {
		if (access(kDebugLibraryPaths[i], R_OK) == 0) {
			strlcpy(sDebugLibrary, kDebugLibraryPaths[i],
				sizeof(sDebugLibrary));
			return true;
		}
	}
	return false;
}


static char**
round_environment(bool guarded)
{
	int count = 0;
	while (environ[count] != NULL)
		count++;

	char** environment = (char**)calloc(count + 3, sizeof(char*));
	int used = 0;
	for (int i = 0; i < count; i++) {
		if (strncmp(environ[i], "LD_PRELOAD=", 11) == 0
			|| strncmp(environ[i], "MALLOC_DEBUG=", 13) == 0) {
			continue;
		}
		environment[used++] = environ[i];
	}
	if (guarded) {
		static char preload[PATH_MAX + 16];
		snprintf(preload, sizeof(preload), "LD_PRELOAD=%s", sDebugLibrary);
		environment[used++] = preload;
		environment[used++] = (char*)"MALLOC_DEBUG=g";
	}
	environment[used] = NULL;
	return environment;
}


int
main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], kRoundArgument) == 0)
		return run_round();

	bool guarded = find_debug_library(argv[0]);
	if (guarded)
		printf("using the guarded heap of %s\n", sDebugLibrary);
	else {
		printf("%s not found: running without the guarded heap, so a read "
			"of freed memory may go unnoticed\n", kDebugLibraryName);
	}
	char** environment = round_environment(guarded);

	int failed = 0;
	for (int i = 0; i < kRounds; i++) {
		char* childArguments[] = { argv[0], (char*)kRoundArgument, NULL };
		pid_t child;
		if (posix_spawn(&child, argv[0], NULL, NULL, childArguments,
				environment) != 0) {
			printf("posix_spawn() failed\n");
			return 2;
		}

		int status;
		if (waitpid(child, &status, 0) != child) {
			printf("waitpid() failed\n");
			return 2;
		}
		if (WIFSIGNALED(status)) {
			printf("round %d: crashed with signal %d\n", i + 1,
				WTERMSIG(status));
			failed++;
		} else if (WEXITSTATUS(status) != 0)
			failed++;
	}

	printf("%d of %d rounds saw valid locale state%s: %s\n",
		kRounds - failed, kRounds, guarded ? " (guarded heap)" : "",
		failed == 0 ? "PASSED" : "FAILED");
	return failed == 0 ? 0 : 1;
}
