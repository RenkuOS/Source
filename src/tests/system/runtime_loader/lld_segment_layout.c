/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*	Checks that runtime_loader maps an executable with lld's default segment
	layout: four PT_LOADs (R, RX, RW RELRO and RW), each aligned to lld's
	maximum page size, 64 KiB on arm64. map_image() used to refuse it with
	B_BAD_DATA ("Bad data") for the unused space between the segments.

	jam builds tests with gcc and GNU ld, which give two PT_LOADs, so this is
	built by hand with clang and lld (ld.lld in PATH), and run on the target:

	clang --target=aarch64-unknown-haiku --sysroot=<Haiku arm64 sysroot> \
		-fuse-ld=lld -O1 -Wall -o lld_segment_layout lld_segment_layout.c
	llvm-readelf -lW lld_segment_layout		# four LOAD lines
	./lld_segment_layout					# prints PASS, exit status 0
*/


#include <stdio.h>


static const char kReadOnly[] = "read-only";
volatile int gData = 7;
volatile int gBss;


int
main()
{
	gData++;
	gBss++;
	if (kReadOnly[0] != 'r' || gData != 8 || gBss != 1) {
		puts("FAIL");
		return 1;
	}

	puts("PASS");
	return 0;
}
