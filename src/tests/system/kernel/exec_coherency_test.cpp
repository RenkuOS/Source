/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


/*!	Checks that code freshly written to a file is what the CPU runs once the
	file is mapped executable.

	Each round, a writer thread writes pages of tiny functions to a new file
	(through the file cache, so the CPU writes them), then the main thread maps
	the file executable and calls some functions of every page, each of which
	must return a value that is new in this round. Data left in a data cache,
	or old instructions left in an instruction cache, show up as a value of an
	earlier round, a crash or an illegal instruction.

	exec_coherency_test [rounds] [pages] [directory]
*/


#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <OS.h>


static const int kPageSize = B_PAGE_SIZE;

#if defined(__arm__) || defined(__i386__) || defined(__x86_64__)
#	define SUPPORTED_ARCHITECTURE
#endif


#ifdef SUPPORTED_ARCHITECTURE


// ARM: movw r0, #value; bx lr - x86: mov eax, value; ret; padding
static const int kFunctionSize = 8;
static const int kFunctionsPerPage = kPageSize / kFunctionSize;
static const int kCalledEvery = 17;

static sigjmp_buf sJump;
static volatile int sSignal;
static volatile bool sStop;


static void
on_signal(int signal)
{
	sSignal = signal;
	siglongjmp(sJump, 1);
}


static uint32_t
value_for(int round, int page, int function)
{
	return (uint32_t)((round * 131 + page * 7 + function) & 0xffff);
}


static void
write_function(uint8_t* code, uint32_t value)
{
#if defined(__arm__)
	uint32_t instructions[2] = {
		0xe3000000 | ((value & 0xf000) << 4) | (value & 0x0fff),
		0xe12fff1e
	};
	memcpy(code, instructions, sizeof(instructions));
#else
	code[0] = 0xb8;
	memcpy(code + 1, &value, 4);
	code[5] = 0xc3;
	code[6] = 0xcc;
	code[7] = 0xcc;
#endif
}


struct WriteJob {
	const char*	path;
	int			round;
	int			pages;
	int			result;
};


static void*
write_file(void* data)
{
	WriteJob* job = (WriteJob*)data;
	job->result = -1;

	int fd = open(job->path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0)
		return NULL;

	uint8_t* page = (uint8_t*)malloc(kPageSize);
	for (int p = 0; p < job->pages; p++) {
		for (int f = 0; f < kFunctionsPerPage; f++)
			write_function(page + f * kFunctionSize, value_for(job->round, p, f));
		if (write(fd, page, kPageSize) != kPageSize) {
			free(page);
			close(fd);
			return NULL;
		}
	}
	free(page);
	close(fd);
	job->result = 0;
	return NULL;
}


/*!	Keeps another CPU busy, so that the writer and the caller do not always
	run on the same one.
*/
static void*
spin(void*)
{
	volatile uint32_t sum = 0;
	while (!sStop) {
		for (int i = 0; i < 100000; i++)
			sum += i;
		snooze(500);
	}
	return NULL;
}


int
main(int argc, char** argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 1000;
	int pages = argc > 2 ? atoi(argv[2]) : 16;
	const char* directory = argc > 3 ? argv[3] : "/tmp";

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = on_signal;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGBUS, &action, NULL);

	pthread_t spinner;
	pthread_create(&spinner, NULL, spin, NULL);

	char path[B_PATH_NAME_LENGTH];
	snprintf(path, sizeof(path), "%s/exec_coherency_test-%d.code", directory,
		(int)getpid());

	long wrong = 0;
	long crashes = 0;
	long calls = 0;
	for (int round = 0; round < rounds; round++) {
		unlink(path);

		WriteJob job = { path, round, pages, -1 };
		pthread_t writer;
		pthread_create(&writer, NULL, write_file, &job);
		pthread_join(writer, NULL);
		if (job.result != 0) {
			fprintf(stderr, "could not write %s: %s\n", path, strerror(errno));
			return 1;
		}

		int fd = open(path, O_RDONLY);
		void* code = mmap(NULL, pages * kPageSize, PROT_READ | PROT_EXEC,
			MAP_PRIVATE, fd, 0);
		close(fd);
		if (code == MAP_FAILED) {
			fprintf(stderr, "could not map %s: %s\n", path, strerror(errno));
			return 1;
		}

		for (int p = 0; p < pages; p++) {
			for (int f = 0; f < kFunctionsPerPage; f += kCalledEvery) {
				uint32_t (*function)() = (uint32_t (*)())((uint8_t*)code
					+ p * kPageSize + f * kFunctionSize);
				uint32_t expected = value_for(round, p, f);
				calls++;
				if (sigsetjmp(sJump, 1) != 0) {
					if (++crashes <= 10) {
						printf("round %d, page %d, function %d: signal %d\n",
							round, p, f, sSignal);
					}
					continue;
				}
				uint32_t value = function();
				if (value != expected && ++wrong <= 10) {
					printf("round %d, page %d, function %d: %#" B_PRIx32
						" instead of %#" B_PRIx32 "\n", round, p, f, value,
						expected);
				}
			}
		}
		munmap(code, pages * kPageSize);
	}

	sStop = true;
	pthread_join(spinner, NULL);
	unlink(path);

	printf("%d rounds of %d pages, %ld calls: %ld wrong values, %ld crashes\n",
		rounds, pages, calls, wrong, crashes);
	return wrong + crashes > 0 ? 1 : 0;
}


#else	// !SUPPORTED_ARCHITECTURE


int
main()
{
	printf("exec_coherency_test: no code to write for this architecture\n");
	return 0;
}


#endif
