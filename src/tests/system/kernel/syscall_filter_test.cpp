/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */


/*!	Tests restrict_syscalls(). Every case runs in a forked child, because a
	restriction cannot be undone. Exits 0 when every case passes.

	Run as root (as every Haiku application is) from a writable directory;
	it creates and removes a file named syscall_filter_test.tmp there.
*/


#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>
#include <debugger.h>
#include <image.h>

#include <generic_syscall_defs.h>
#include <random_defs.h>
#include <syscalls.h>


static const char* kTestFile = "syscall_filter_test.tmp";
static int sFailures;


#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			printf("    FAILED: %s (line %d, errno %s)\n", #condition, \
				__LINE__, strerror(errno)); \
			fflush(stdout); \
			_exit(1); \
		} \
	} while (false)


static void
run_case(const char* name, void (*function)())
{
	fflush(stdout);
	pid_t child = fork();
	if (child < 0) {
		printf("%s: cannot fork: %s\n", name, strerror(errno));
		sFailures++;
		return;
	}
	if (child == 0) {
		function();
		fflush(stdout);
		_exit(0);
	}

	int status;
	waitpid(child, &status, 0);
	bool passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	printf("%s: %s\n", name, passed ? "passed" : "FAILED");
	if (!passed)
		sFailures++;
}


static bool
denied(status_t status)
{
	return status == B_NOT_ALLOWED;
}


static bool
denied_errno(int result)
{
	return result < 0 && errno == EPERM;
}


// #pragma mark - cases


static void
test_bad_value()
{
	CHECK(restrict_syscalls(0x80000000) == B_BAD_VALUE);
	CHECK(restrict_syscalls(B_SYSCALL_GROUP_ALL) == B_OK);
}


static void*
thread_function(void* argument)
{
	return (void*)((addr_t)argument + 1);
}


static void
test_basic()
{
	sem_id parentSem = (sem_id)atoi(getenv("SYSCALL_FILTER_TEST_SEM"));
	CHECK(restrict_syscalls(B_SYSCALL_GROUP_BASIC) == B_OK);

	// memory and threads keep working
	void* memory = malloc(1 << 20);
	CHECK(memory != NULL);
	memset(memory, 1, 1 << 20);
	free(memory);

	pthread_t thread;
	CHECK(pthread_create(&thread, NULL, thread_function, (void*)41) == 0);
	void* result;
	CHECK(pthread_join(thread, &result) == 0);
	CHECK((addr_t)result == 42);

	// own semaphores, yes; the parent's, no
	sem_id sem = create_sem(0, "own");
	CHECK(sem >= 0);
	CHECK(release_sem(sem) == B_OK);
	CHECK(acquire_sem(sem) == B_OK);
	CHECK(delete_sem(sem) == B_OK);
	CHECK(denied(release_sem(parentSem)));
	CHECK(denied(delete_sem(parentSem)));

	// signals to itself, yes; to the parent, no
	CHECK(kill(getpid(), 0) == 0);
	CHECK(denied_errno(kill(getppid(), 0)));
	CHECK(denied(kill_thread(getppid())));
	thread_info info;
	CHECK(denied(get_thread_info(getppid(), &info)));
	team_info teamInfo;
	CHECK(get_team_info(B_CURRENT_TEAM, &teamInfo) == B_OK);
	CHECK(denied(get_team_info(getppid(), &teamInfo)));

	// descriptors it already has
	int pipeFDs[2];
	CHECK(pipe(pipeFDs) == 0);
	CHECK(write(pipeFDs[1], "x", 1) == 1);
	char buffer;
	CHECK(read(pipeFDs[0], &buffer, 1) == 1);
	struct stat st;
	CHECK(fstat(pipeFDs[0], &st) == 0);
	int pair[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);

	// everything outside BASIC
	CHECK(denied_errno(open("/boot/system/lib/libroot.so", O_RDONLY)));
	CHECK(denied_errno(stat("/boot/system/lib/libroot.so", &st)));
	CHECK(denied_errno(socket(AF_INET, SOCK_STREAM, 0)));
	CHECK(denied_errno(fork()));
	CHECK(denied(create_port(1, "denied")));
	CHECK(denied(find_port("system:roster")));
	area_info areaInfo;
	ssize_t cookie = 0;
	CHECK(get_next_area_info(B_CURRENT_TEAM, &cookie, &areaInfo) == B_OK);
	CHECK(denied(get_next_area_info(getppid(), &cookie, &areaInfo)));
	void* address;
	CHECK(denied(clone_area("denied", &address, B_ANY_ADDRESS, B_READ_AREA,
		areaInfo.area)));
	int32 teamCookie = 0;
	CHECK(denied(get_next_team_info(&teamCookie, &teamInfo)));
	CHECK(denied(install_team_debugger(getppid(), -1)));
}


static void
test_entropy()
{
	// getentropy() is a generic syscall, and the only one in BASIC: random
	// number generators need it.
	CHECK(restrict_syscalls(B_SYSCALL_GROUP_BASIC) == B_OK);
	char buffer[64];
	CHECK(getentropy(buffer, sizeof(buffer)) == 0);

	// any other function of the same subsystem needs SYSTEM
	uint32 version = 0;
	CHECK(denied(_kern_generic_syscall(RANDOM_SYSCALLS, B_SYSCALL_INFO,
		&version, sizeof(version))));
}


static void
test_file_read()
{
	CHECK(restrict_syscalls(B_SYSCALL_GROUP_BASIC
		| B_SYSCALL_GROUP_FILE_READ) == B_OK);

	int fd = open(kTestFile, O_RDONLY);
	CHECK(fd >= 0);
	close(fd);
	struct stat st;
	CHECK(stat(kTestFile, &st) == 0);

	CHECK(denied_errno(open(kTestFile, O_WRONLY)));
	CHECK(denied_errno(open(kTestFile, O_RDWR)));
	CHECK(denied_errno(open(kTestFile, O_RDONLY | O_TRUNC)));
	CHECK(denied_errno(open("syscall_filter_test.new", O_RDONLY | O_CREAT,
		0644)));
	CHECK(denied_errno(unlink(kTestFile)));
	CHECK(denied_errno(mkdir("syscall_filter_test.dir", 0755)));
	CHECK(denied_errno(chmod(kTestFile, 0600)));
}


static void
test_cannot_widen()
{
	CHECK(restrict_syscalls(B_SYSCALL_GROUP_BASIC) == B_OK);
	CHECK(restrict_syscalls(B_SYSCALL_GROUP_ALL) == B_OK);
	CHECK(denied_errno(open(kTestFile, O_RDONLY)));
	CHECK(denied_errno(socket(AF_INET, SOCK_STREAM, 0)));
}


static volatile bool sRestricted;
static volatile int sThreadResult = -1;


static void*
existing_thread_function(void*)
{
	while (!sRestricted)
		snooze(1000);
	int fd = open(kTestFile, O_RDONLY);
	sThreadResult = (fd < 0 && errno == EPERM) ? 0 : 1;
	return NULL;
}


static void
test_existing_threads()
{
	// a thread that exists before the call is filtered too
	pthread_t thread;
	CHECK(pthread_create(&thread, NULL, existing_thread_function, NULL) == 0);
	snooze(10000);
	CHECK(restrict_syscalls(B_SYSCALL_GROUP_BASIC) == B_OK);
	sRestricted = true;
	CHECK(pthread_join(thread, NULL) == 0);
	CHECK(sThreadResult == 0);
}


static void
test_fork_inherits()
{
	CHECK(restrict_syscalls(B_SYSCALL_GROUP_BASIC
		| B_SYSCALL_GROUP_PROCESS) == B_OK);
	pid_t child = fork();
	CHECK(child >= 0);
	if (child == 0) {
		bool ok = denied_errno(socket(AF_INET, SOCK_STREAM, 0))
			&& denied_errno(open(kTestFile, O_RDONLY));
		_exit(ok ? 0 : 1);
	}
	int status;
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}


static const char* sSelfPath;


static void
test_load_image_inherits()
{
	// The loaded team must be able to read its own files to start at all.
	CHECK(restrict_syscalls(B_SYSCALL_GROUP_BASIC | B_SYSCALL_GROUP_PROCESS
		| B_SYSCALL_GROUP_FILE_READ) == B_OK);
	const char* args[] = { sSelfPath, "--loaded-child", NULL };
	extern char** environ;
	thread_id child = load_image(2, args, (const char**)environ);
	CHECK(child >= 0);
	CHECK(resume_thread(child) == B_OK);
	status_t result;
	CHECK(wait_for_thread(child, &result) == B_OK);
	CHECK(result == 0);
}


static status_t
exiting_thread(void*)
{
	return 7;
}


static void
test_wait_for_exited()
{
	// wait_for_thread() on threads that are already gone: the caller's own
	// death records, not someone else's thread
	CHECK(restrict_syscalls(B_SYSCALL_GROUP_BASIC | B_SYSCALL_GROUP_PROCESS
		| B_SYSCALL_GROUP_FILE_READ) == B_OK);

	thread_id thread = spawn_thread(exiting_thread, "exiting",
		B_NORMAL_PRIORITY, NULL);
	CHECK(thread >= 0);
	CHECK(resume_thread(thread) == B_OK);
	snooze(200000);
	status_t result;
	CHECK(wait_for_thread(thread, &result) == B_OK);
	CHECK(result == 7);

	const char* args[] = { sSelfPath, "--exit-at-once", NULL };
	extern char** environ;
	thread_id child = load_image(2, args, (const char**)environ);
	CHECK(child >= 0);
	CHECK(resume_thread(child) == B_OK);
	snooze(500000);
	CHECK(wait_for_thread(child, &result) == B_OK);
	CHECK(result == 3);

	// no record of it: neither ours nor a child's
	CHECK(denied(wait_for_thread(0x7ffffff0, &result)));
}


static int
loaded_child()
{
	// started by test_load_image_inherits(): FILE_READ but no SOCKET
	return denied_errno(socket(AF_INET, SOCK_STREAM, 0))
		&& denied_errno(open(kTestFile, O_WRONLY)) ? 0 : 1;
}


// #pragma mark -


int
main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "--loaded-child") == 0)
		return loaded_child();
	if (argc > 1 && strcmp(argv[1], "--exit-at-once") == 0)
		return 3;

	sSelfPath = argv[0];

	int fd = open(kTestFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printf("cannot create %s: %s\n", kTestFile, strerror(errno));
		return 1;
	}
	write(fd, "test\n", 5);
	close(fd);

	sem_id sem = create_sem(0, "parent");
	char semString[16];
	snprintf(semString, sizeof(semString), "%" B_PRId32, sem);
	setenv("SYSCALL_FILTER_TEST_SEM", semString, 1);

	run_case("bad value", test_bad_value);
	run_case("basic", test_basic);
	run_case("entropy", test_entropy);
	run_case("file read", test_file_read);
	run_case("cannot widen", test_cannot_widen);
	run_case("existing threads", test_existing_threads);
	run_case("fork inherits", test_fork_inherits);
	run_case("load_image inherits", test_load_image_inherits);
	run_case("wait for exited", test_wait_for_exited);

	delete_sem(sem);
	unlink(kTestFile);
	rmdir("syscall_filter_test.dir");
	unlink("syscall_filter_test.new");

	printf("%d failure(s)\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
