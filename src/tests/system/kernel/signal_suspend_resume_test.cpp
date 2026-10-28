/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	signal_suspend_resume_test [seconds] [threads]: threads suspended and
	resumed by signals the way WebKit's WTF does it (JavaScriptCore's garbage
	collector), with ignored signals arriving meanwhile.

	Workers wait in pthread_cond_timedwait() with short timeouts, as GC helper
	threads do, and compute in between. A controller suspends one at a time:
	SIGUSR1, whose handler (all signals blocked, SA_RESTART) posts a semaphore
	and waits in sigsuspend() for SIGUSR1 again; then resumes it. Another
	thread keeps sending the workers SIGCHLD, which they ignore by default.

	summit#119: a GC helper thread died with an invalid opcode, its
	instruction pointer two bytes before the return address of the call in
	pthread_cond_timedwait(), as a syscall restart rewinds it. Prints
	the handshakes; a crash or a hang (no progress for 5 s) fails.
*/


#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <OS.h>


static const int kMaxWorkers = 16;

static sem_t sAcknowledged;
static volatile int sResuming;
static volatile bool sQuit;
static pthread_t sWorkers[kMaxWorkers];
static int sWorkerCount;
static volatile int64 sWaits;


static void
suspend_resume_handler(int, siginfo_t*, void*)
{
	if (sResuming)
		return;
	int saved = errno;
	sem_post(&sAcknowledged);
	sigset_t mask;
	sigfillset(&mask);
	sigdelset(&mask, SIGUSR1);
	sigsuspend(&mask);
	sem_post(&sAcknowledged);
	errno = saved;
}


static void*
worker(void*)
{
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
	volatile double sum = 0;
	while (!sQuit) {
		pthread_mutex_lock(&lock);
		struct timespec until;
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_nsec += 200000;
		if (until.tv_nsec >= 1000000000) {
			until.tv_sec++;
			until.tv_nsec -= 1000000000;
		}
		pthread_cond_timedwait(&condition, &lock, &until);
		pthread_mutex_unlock(&lock);
		for (int i = 0; i < 2000; i++)
			sum += i * 0.5;
		atomic_add64((int64*)&sWaits, 1);
	}
	return NULL;
}


static void*
noise(void*)
{
	while (!sQuit) {
		for (int i = 0; i < sWorkerCount; i++)
			pthread_kill(sWorkers[i], SIGCHLD);
		snooze(500);
	}
	return NULL;
}


static bool
wait_acknowledged()
{
	struct timespec until;
	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += 5;
	while (sem_timedwait(&sAcknowledged, &until) != 0) {
		if (errno != EINTR)
			return false;
	}
	return true;
}


int
main(int argc, char** argv)
{
	bigtime_t duration = (argc > 1 ? atoi(argv[1]) : 10) * 1000000LL;
	sWorkerCount = argc > 2 ? atoi(argv[2]) : 8;
	if (sWorkerCount < 1 || sWorkerCount > kMaxWorkers)
		sWorkerCount = 8;

	sem_init(&sAcknowledged, 0, 0);
	struct sigaction action = {};
	action.sa_sigaction = suspend_resume_handler;
	action.sa_flags = SA_RESTART | SA_SIGINFO;
	sigfillset(&action.sa_mask);
	sigaction(SIGUSR1, &action, NULL);

	for (int i = 0; i < sWorkerCount; i++)
		pthread_create(&sWorkers[i], NULL, worker, NULL);
	pthread_t noiseThread;
	pthread_create(&noiseThread, NULL, noise, NULL);

	int64 handshakes = 0;
	bool ok = true;
	bigtime_t end = system_time() + duration;
	for (int i = 0; system_time() < end; i++) {
		pthread_t target = sWorkers[i % sWorkerCount];
		sResuming = 0;
		pthread_kill(target, SIGUSR1);
		if (!wait_acknowledged()) {
			printf("worker %d did not suspend\n", i % sWorkerCount);
			ok = false;
			break;
		}
		sResuming = 1;
		pthread_kill(target, SIGUSR1);
		if (!wait_acknowledged()) {
			printf("worker %d did not resume\n", i % sWorkerCount);
			ok = false;
			break;
		}
		handshakes++;
	}
	sQuit = true;
	if (ok) {
		pthread_join(noiseThread, NULL);
		for (int i = 0; i < sWorkerCount; i++)
			pthread_join(sWorkers[i], NULL);
	}
	printf("%" B_PRId64 " suspend/resume handshakes, %" B_PRId64 " waits: %s\n",
		handshakes, sWaits, ok ? "PASSED" : "FAILED");
	return ok ? 0 : 1;
}
