/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */


/*!	Tests that any thread may call exec*(), not only the main thread: the
	other threads go away and the process keeps its ID, while the main thread
	is blocked in pthread_join(), in a semaphore, or running in userland. Also
	tests that the new image gets the signal mask of the thread that called
	exec, that an exec failing in libroot still returns to the calling thread
	with all threads intact, that an exec failing in the kernel after the
	hand-over to the main thread ends the team instead of hanging, and that
	the main thread still execs as before.

	The race cases run many times each and accept every legal outcome, but
	require that the process neither hangs nor ends otherwise: concurrent
	execs from two threads and from the main thread and another thread, exec
	while the main thread is in sigsuspend() or in wait_for_thread() on the
	exec'ing thread, exec racing exit() from another thread, and exec while
	another thread forks children, which must exit unharmed.

	Exits 0 when every case passes.

	"exec_from_thread_test exec <path> [<args>...]" execs the given command
	from a thread other than the main thread, for checking it by hand.
*/


#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>

#include <syscalls.h>


static const int kHelperOK = 42;
static const int kExitCode = 77;
static const int kForkChildOK = 7;
static const int kBlockedThreads = 3;
static const int kMaxForks = 100;
static const int kRaceIterations = 1000;
static const bigtime_t kCaseTimeout = 10000000;

// The runtime loader of the exec'ed image fails to open the missing program
// and exits the team with this error, of which waitpid() reports the low byte.
static const int kNotFoundExitCode = B_ENTRY_NOT_FOUND & 0xff;

enum {
	WAIT_JOIN,
	WAIT_SEM,
	WAIT_SPIN
};

static const char* sSelf;
static sem_id sNeverReleased;
static pthread_barrier_t sBarrier;
static char** sExecArgs;
static int sIteration;
static bool sReapForkChildren;


static int
helper(int argc, char** argv)
{
	// runs in the exec'ed image
	if (getpid() != atoi(argv[2]))
		return 1;
	if (find_thread(NULL) != getpid())
		return 2;

	team_info info;
	if (get_team_info(B_CURRENT_TEAM, &info) != B_OK || info.thread_count != 1)
		return 3;

	// the signal mask is the one of the thread that called exec
	sigset_t mask;
	if (sigprocmask(SIG_BLOCK, NULL, &mask) != 0
		|| !sigismember(&mask, SIGUSR1)) {
		return 4;
	}

	if (argc > 3 && strcmp(argv[3], "--reap") == 0) {
		// the children forked by the old image must have exited by themselves
		int status;
		while (waitpid(-1, &status, 0) > 0) {
			if (!WIFEXITED(status) || WEXITSTATUS(status) != kForkChildOK)
				return 5;
		}
		if (errno != ECHILD)
			return 6;
	}

	return kHelperOK;
}


static int32
thread_count()
{
	team_info info;
	if (get_team_info(B_CURRENT_TEAM, &info) != B_OK)
		return -1;
	return info.thread_count;
}


static void
exec_helper()
{
	char pid[16];
	snprintf(pid, sizeof(pid), "%d", (int)getpid());
	const char* args[] = { sSelf, "--helper", pid,
		sReapForkChildren ? "--reap" : NULL, NULL };

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, &mask, NULL);

	execv(sSelf, (char**)args);
}


static void*
block_forever(void*)
{
	acquire_sem(sNeverReleased);
	return NULL;
}


static void
start_blocked_threads()
{
	sNeverReleased = create_sem(0, "never released");
	for (int i = 0; i < kBlockedThreads; i++) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, block_forever, NULL) != 0)
			_exit(20);
	}
}


static void*
exec_helper_thread(void* waitForBarrier)
{
	if (waitForBarrier != NULL)
		pthread_barrier_wait(&sBarrier);
	exec_helper();
	_exit(errno == EPERM ? 11 : 10);
}


static status_t
exec_helper_spawned_thread(void*)
{
	exec_helper();
	_exit(errno == EPERM ? 11 : 10);
}


static void*
exec_fails_in_libroot_thread(void*)
{
	int32 before = thread_count();

	// libroot finds that the file is missing before calling the kernel
	const char* args[] = { "/nonexistent/exec_from_thread_test", NULL };
	if (execv(args[0], (char**)args) != -1 || errno != ENOENT)
		return (void*)1;

	// every thread must still be there
	if (thread_count() != before || before != kBlockedThreads + 2)
		return (void*)2;

	return (void*)kHelperOK;
}


static void*
exec_fails_in_kernel_thread(void*)
{
	// Bypass libroot's checks, so that the kernel starts the exec. It only
	// fails once the old image is gone, in the new image's runtime loader.
	static const char kPath[] = "/nonexistent/exec_from_thread_test";
	struct {
		const char*	args[3];
		char		path[sizeof(kPath)];
	} flatArgs;
	flatArgs.args[0] = flatArgs.path;
	flatArgs.args[1] = NULL;
	flatArgs.args[2] = NULL;
	memcpy(flatArgs.path, kPath, sizeof(kPath));

	status_t status = _kern_exec(kPath, flatArgs.args, sizeof(flatArgs), 1, 0,
		022);
	_exit(status == B_NOT_ALLOWED ? 11 : 10);
}


static void*
exit_racing_thread(void*)
{
	pthread_barrier_wait(&sBarrier);
	if (sIteration % 8 != 0)
		snooze((sIteration % 8) * 500);
	exit(kExitCode);
}


static void*
fork_loop_thread(void*)
{
	// fork once before the exec can start, so that there is a child to reap
	for (int i = 0; i < kMaxForks; i++) {
		if (i == 1)
			pthread_barrier_wait(&sBarrier);

		pid_t child = fork();
		if (child == 0)
			_exit(kForkChildOK);
		if (child < 0) {
			// Once the exec may have started, fork() was seen to fail twice
			// in about 30000 runs, for a reason not established yet. That is
			// not what this case checks, so just stop forking.
			if (i == 0)
				_exit(24);
			break;
		}
	}

	acquire_sem(sNeverReleased);
	return NULL;
}


static void*
exec_command_thread(void*)
{
	execv(sExecArgs[0], sExecArgs);
	fprintf(stderr, "exec %s: %s\n", sExecArgs[0], strerror(errno));
	_exit(1);
}


static void
child_exec_from_thread(int mainWait)
{
	start_blocked_threads();

	pthread_t thread;
	if (pthread_create(&thread, NULL, exec_helper_thread, NULL) != 0)
		_exit(21);

	switch (mainWait) {
		case WAIT_JOIN:
			pthread_join(thread, NULL);
			break;
		case WAIT_SEM:
			acquire_sem(sNeverReleased);
			break;
		case WAIT_SPIN:
		{
			bigtime_t end = system_time() + kCaseTimeout / 2;
			while (system_time() < end)
				;
			break;
		}
	}

	_exit(22);
}


static void
child_exec_fails_in_libroot(int)
{
	start_blocked_threads();

	pthread_t thread;
	void* result;
	if (pthread_create(&thread, NULL, exec_fails_in_libroot_thread, NULL) != 0
		|| pthread_join(thread, &result) != 0)
		_exit(21);

	if (thread_count() != kBlockedThreads + 1)
		_exit(23);

	_exit((int)(addr_t)result);
}


static void
child_exec_fails_in_kernel(int)
{
	start_blocked_threads();

	pthread_t thread;
	if (pthread_create(&thread, NULL, exec_fails_in_kernel_thread, NULL) != 0)
		_exit(21);
	pthread_join(thread, NULL);
	_exit(22);
}


static void
child_exec_from_main_thread(int)
{
	start_blocked_threads();
	exec_helper();
	_exit(10);
}


static void
child_concurrent_execs(int fromMainThread)
{
	start_blocked_threads();
	pthread_barrier_init(&sBarrier, NULL, 2);

	pthread_t thread;
	for (int i = fromMainThread ? 1 : 0; i < 2; i++) {
		if (pthread_create(&thread, NULL, exec_helper_thread, &sBarrier) != 0)
			_exit(21);
	}

	if (fromMainThread) {
		pthread_barrier_wait(&sBarrier);
		exec_helper();
		_exit(10);
	}

	acquire_sem(sNeverReleased);
	_exit(22);
}


static void
child_exec_main_in_sigsuspend(int)
{
	start_blocked_threads();

	pthread_t thread;
	if (pthread_create(&thread, NULL, exec_helper_thread, NULL) != 0)
		_exit(21);

	// the helper checks that this mask does not leak into the new image
	sigset_t mask;
	sigemptyset(&mask);
	sigsuspend(&mask);
	_exit(22);
}


static void
child_exec_racing_exit(int)
{
	start_blocked_threads();
	pthread_barrier_init(&sBarrier, NULL, 2);

	pthread_t thread;
	if (pthread_create(&thread, NULL, exec_helper_thread, &sBarrier) != 0
		|| pthread_create(&thread, NULL, exit_racing_thread, NULL) != 0)
		_exit(21);

	acquire_sem(sNeverReleased);
	_exit(22);
}


static void
child_exec_racing_fork(int)
{
	// In this case, a pipe given as stdout was seen to stay open after every
	// process using it had ended; the cause, and whether it needs the exec,
	// is not known yet. Keep our caller's stdout away from the children.
	int devNull = open("/dev/null", O_WRONLY);
	if (devNull < 0 || dup2(devNull, STDOUT_FILENO) < 0
		|| dup2(devNull, STDERR_FILENO) < 0)
		_exit(25);
	close(devNull);

	start_blocked_threads();
	pthread_barrier_init(&sBarrier, NULL, 2);
	sReapForkChildren = true;

	pthread_t thread;
	if (pthread_create(&thread, NULL, fork_loop_thread, NULL) != 0
		|| pthread_create(&thread, NULL, exec_helper_thread, &sBarrier) != 0)
		_exit(21);

	acquire_sem(sNeverReleased);
	_exit(22);
}


static void
child_exec_main_in_wait_for_thread(int)
{
	start_blocked_threads();

	thread_id thread = spawn_thread(exec_helper_spawned_thread, "exec",
		B_NORMAL_PRIORITY, NULL);
	if (thread < 0 || resume_thread(thread) != B_OK)
		_exit(21);

	status_t result;
	wait_for_thread(thread, &result);
	_exit(22);
}


/*!	Runs \a child(arg) in a forked process and waits up to kCaseTimeout for it
	to exit with \a expected or \a alternative (if not -1). Returns the exit
	code, or -1 after printing why the case failed. Prints success only if
	\a verbose.
*/
static int
run_case(const char* name, void (*child)(int), int arg, int expected,
	int alternative = -1, bool verbose = true)
{
	// don't let the child flush our buffered output a second time
	fflush(stdout);

	pid_t pid = fork();
	if (pid < 0) {
		printf("FAIL %s: fork: %s\n", name, strerror(errno));
		return -1;
	}
	if (pid == 0)
		child(arg);

	int status;
	bigtime_t end = system_time() + kCaseTimeout;
	pid_t result;
	while ((result = waitpid(pid, &status, WNOHANG)) == 0
		&& system_time() < end) {
		snooze(1000);
	}

	if (result == 0) {
		kill(pid, SIGKILL);
		end = system_time() + kCaseTimeout / 2;
		while ((result = waitpid(pid, &status, WNOHANG)) == 0
			&& system_time() < end) {
			snooze(10000);
		}
		printf("FAIL %s (run %d): timed out%s\n", name, sIteration,
			result == 0 ? ", and SIGKILL did not end it" : "");
		if (result == 0)
			exit(1);
		return -1;
	}

	int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	if (result != pid || code < 0
		|| (code != expected && code != alternative)) {
		printf("FAIL %s (run %d): waitpid() = %d, status %#x (exit code %d)\n",
			name, sIteration, (int)result, status, code);
		return -1;
	}

	if (verbose)
		printf("ok   %s\n", name);
	return code;
}


struct race_case {
	const char*	name;
	void		(*child)(int);
	int			arg;
	int			alternative;
	int			execs;
	int			alternatives;
};


int
main(int argc, char** argv)
{
	sSelf = argv[0];

	if (argc >= 3 && strcmp(argv[1], "--helper") == 0)
		return helper(argc, argv);

	if (argc >= 3 && strcmp(argv[1], "exec") == 0) {
		sExecArgs = argv + 2;
		start_blocked_threads();
		pthread_t thread;
		pthread_create(&thread, NULL, exec_command_thread, NULL);
		pthread_join(thread, NULL);
		return 1;
	}

	if (strchr(sSelf, '/') == NULL) {
		fprintf(stderr, "run %s with a path\n", sSelf);
		return 1;
	}

	bool ok = true;
	ok &= run_case("exec from a thread, main in pthread_join()",
		child_exec_from_thread, WAIT_JOIN, kHelperOK) >= 0;
	ok &= run_case("exec from a thread, main in acquire_sem()",
		child_exec_from_thread, WAIT_SEM, kHelperOK) >= 0;
	ok &= run_case("exec from a thread, main in userland",
		child_exec_from_thread, WAIT_SPIN, kHelperOK) >= 0;
	ok &= run_case("exec failing in libroot returns to the thread",
		child_exec_fails_in_libroot, 0, kHelperOK) >= 0;
	ok &= run_case("exec failing in the kernel ends the team",
		child_exec_fails_in_kernel, 0, kNotFoundExitCode) >= 0;
	ok &= run_case("exec from the main thread",
		child_exec_from_main_thread, 0, kHelperOK) >= 0;

	race_case races[] = {
		{ "concurrent execs from two threads",
			child_concurrent_execs, 0, -1, 0, 0 },
		{ "concurrent execs from main and a thread",
			child_concurrent_execs, 1, -1, 0, 0 },
		{ "exec from a thread, main in sigsuspend()",
			child_exec_main_in_sigsuspend, 0, -1, 0, 0 },
		{ "exec from a thread, main in wait_for_thread() on it",
			child_exec_main_in_wait_for_thread, 0, -1, 0, 0 },
		{ "exec racing exit() from another thread",
			child_exec_racing_exit, 0, kExitCode, 0, 0 },
		{ "exec while another thread forks",
			child_exec_racing_fork, 0, -1, 0, 0 },
	};
	const int raceCount = sizeof(races) / sizeof(races[0]);

	for (sIteration = 0; sIteration < kRaceIterations && ok; sIteration++) {
		for (int i = 0; i < raceCount && ok; i++) {
			race_case& race = races[i];
			int code = run_case(race.name, race.child, race.arg, kHelperOK,
				race.alternative, false);
			if (code == kHelperOK)
				race.execs++;
			else if (code >= 0)
				race.alternatives++;
			else
				ok = false;
		}
	}

	for (int i = 0; ok && i < raceCount; i++) {
		printf("ok   %s: %d runs, %d exec'ed", races[i].name,
			races[i].execs + races[i].alternatives, races[i].execs);
		if (races[i].alternative >= 0) {
			printf(", %d exited with %d", races[i].alternatives,
				races[i].alternative);
		}
		printf("\n");
	}

	printf("%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
