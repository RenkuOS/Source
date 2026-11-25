/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*	Checks thread-local storage accessed through TLS descriptors on arm64:
	the variable, in tls_desc_var.c, is bumped from several threads, and each
	must see its own copy, starting at 7. clang has only TLS descriptors for
	dynamic TLS on arm64, so both the library and the executable that
	contain the variable have R_AARCH64_TLSDESC relocations, which
	runtime_loader used to refuse with B_BAD_DATA ("Troubles relocating: Bad
	data").

	jam builds tests with gcc, so this is built by hand with clang and lld
	(ld.lld in PATH). --no-rosegment and -z norelro give GNU ld's two
	PT_LOADs; they only keep this test independent of the loader accepting
	lld's default layout (lld_segment_layout.c).

	CC="clang --target=aarch64-unknown-haiku --sysroot=<Haiku arm64 sysroot> \
		-fuse-ld=lld -fno-emulated-tls -O1 -Wall \
		-Wl,--no-rosegment,-z,norelro"
	$CC -fPIC -shared -o libtls_desc_var.so tls_desc_var.c
	$CC -o tls_desc_lib tls_desc_test.c -L. -ltls_desc_var
	$CC -o tls_desc_exe tls_desc_test.c tls_desc_var.c
	llvm-readelf -r libtls_desc_var.so tls_desc_exe	# R_AARCH64_TLSDESC

	On the target, each prints PASS and exits with status 0:

	LIBRARY_PATH=.:$LIBRARY_PATH ./tls_desc_lib
	./tls_desc_exe
*/


#include <pthread.h>
#include <stdio.h>


int probe_bump(int n);
int* probe_address(void);


enum {
	kThreads = 4,
	kRounds = 1000,
	kInitial = 7
};


static void*
worker(void* arg)
{
	long id = (long)arg;
	int value = 0;
	for (int i = 0; i < kRounds; i++)
		value = probe_bump(1);

	int ok = value == kInitial + kRounds;
	printf("thread %ld: value %d at %p: %s\n", id, value,
		(void*)probe_address(), ok ? "ok" : "WRONG");
	return (void*)(long)ok;
}


int
main()
{
	pthread_t threads[kThreads];
	int failures = 0;

	printf("main: value %d at %p\n", probe_bump(0), (void*)probe_address());
	for (long i = 0; i < kThreads; i++) {
		if (pthread_create(&threads[i], NULL, worker, (void*)i) != 0) {
			puts("pthread_create failed");
			return 2;
		}
	}
	for (int i = 0; i < kThreads; i++) {
		void* ok;
		pthread_join(threads[i], &ok);
		failures += !(long)ok;
	}

	int value = probe_bump(0);
	printf("main: value after threads %d: %s\n", value,
		value == kInitial ? "ok" : "WRONG");
	failures += value != kInitial;

	puts(failures != 0 ? "FAIL" : "PASS");
	return failures != 0;
}
