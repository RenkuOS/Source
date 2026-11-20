/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Concurrent positional writes to one growing file, then a check of every
	block. BFS used to zero the gap a write left past the old end of the file
	after it had released the inode lock, wiping out what other writers put
	into that gap meanwhile (QEMU's I/O threads on a growing disk image lost
	data that way).

	bfs_concurrent_grow_test FILE [THREADS [SECONDS [SIZE_MB [PREALLOCATE]]]]
	Exits with 0 when every block holds what was last written to it.
*/
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#include <OS.h>
#include <time.h>

#define BLK 4096
static int fd; static uint64_t nblocks; static uint32_t *gen;
static pthread_mutex_t *locks; static volatile int stop; static long writes;

static void fill(uint8_t *p, uint64_t blk, uint32_t g) {
	for (int i = 0; i < BLK; i += 16) {
		uint64_t a = blk, b = ((uint64_t)g << 32) | (uint32_t)i;
		memcpy(p + i, &a, 8); memcpy(p + i + 8, &b, 8);
	}
}

static void *worker(void *arg) {
	unsigned seed = (unsigned)(uintptr_t)arg * 7919u + 1;
	uint8_t *bufs[16];
	for (int i = 0; i < 16; i++) bufs[i] = aligned_alloc(BLK, BLK);
	while (!stop) {
		int n = 1 + rand_r(&seed) % 16;
		uint64_t first = (uint64_t)rand_r(&seed) * 65521u % (nblocks - n);
		// lock the range in order (no two writers on one block at once)
		for (int i = 0; i < n; i++) pthread_mutex_lock(&locks[(first + i) % 4096]);
		struct iovec iov[16];
		for (int i = 0; i < n; i++) {
			uint32_t g = gen[first + i] + 1;
			fill(bufs[i], first + i, g);
			iov[i].iov_base = bufs[i]; iov[i].iov_len = BLK;
		}
		ssize_t w = writev_pos(fd, (off_t)(first * BLK), iov, n);
		if (w != (ssize_t)n * BLK) { fprintf(stderr, "short write %zd\n", w); exit(2); }
		for (int i = 0; i < n; i++) gen[first + i]++;
		for (int i = n - 1; i >= 0; i--) pthread_mutex_unlock(&locks[(first + i) % 4096]);
		__sync_fetch_and_add(&writes, 1);
		if (rand_r(&seed) % 200 == 0) fdatasync(fd);
	}
	return NULL;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s FILE [THREADS [SECONDS [SIZE_MB [PREALLOCATE]]]]\n", argv[0]);
		return 1;
	}
	int threads = argc > 2 ? atoi(argv[2]) : 8, secs = argc > 3 ? atoi(argv[3]) : 10;
	if (threads < 1 || threads > 64) threads = 8;
	nblocks = (uint64_t)(argc > 4 ? atoi(argv[4]) : 64) * 1024 * 1024 / BLK;
	unlink(argv[1]);
	fd = open(argv[1], O_RDWR | O_CREAT, 0644);
	if (fd < 0) { perror("open"); return 1; }
	gen = calloc(nblocks, sizeof *gen);
	if (argc > 5 && atoi(argv[5])) {
		// preallocate: the whole file written with zeros first
		uint8_t *z = calloc(1, 1 << 20);
		for (uint64_t o = 0; o < nblocks * BLK; o += 1 << 20) pwrite(fd, z, 1 << 20, (off_t)o);
		fsync(fd);
	}
	locks = malloc(4096 * sizeof *locks);
	for (int i = 0; i < 4096; i++) pthread_mutex_init(&locks[i], NULL);
	pthread_t t[64];
	for (int i = 0; i < threads; i++) pthread_create(&t[i], NULL, worker, (void *)(uintptr_t)i);
	sleep(secs); stop = 1;
	for (int i = 0; i < threads; i++) pthread_join(t[i], NULL);
	fsync(fd);
	uint8_t *buf = aligned_alloc(BLK, BLK), *want = aligned_alloc(BLK, BLK);
	uint64_t bad = 0, written = 0;
	for (uint64_t b = 0; b < nblocks; b++) {
		ssize_t r = pread(fd, buf, BLK, (off_t)(b * BLK));
		if (r < 0) { memset(buf, 0xee, BLK); }
		else if (r < BLK) memset(buf + r, 0, BLK - r);	// past the end: zeros
		if (gen[b] == 0) { memset(want, 0, BLK); } else { fill(want, b, gen[b]); written++; }
		if (memcmp(buf, want, BLK) != 0) {
			if (bad < 10) {
				uint64_t a, c; memcpy(&a, buf, 8); memcpy(&c, buf + 8, 8);
				fprintf(stderr, "block %llu: want gen %u, has blk %llu gen %u\n",
					(unsigned long long)b, gen[b], (unsigned long long)a, (unsigned)(c >> 32));
			}
			bad++;
		}
	}
	printf("%ld writes by %d threads in %d s, %llu blocks written, %llu bad\n", writes, threads, secs,
		(unsigned long long)written, (unsigned long long)bad);
	close(fd); unlink(argv[1]);
	return bad ? 3 : 0;
}
