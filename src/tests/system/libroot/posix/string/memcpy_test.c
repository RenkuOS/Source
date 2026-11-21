/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */

/*!	Checks memcpy(), memmove() and memset() against byte-wise reference
	copies for every size up to kSmallMax, some large sizes, every source and
	destination alignment modulo 16, overlapping memmove() in both directions,
	and that no byte outside the destination range is touched.
*/


#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define kSmallMax	320
#define kGuard		64
#define kAlign		16

static const size_t kLargeSizes[] = { 511, 1000, 4095, 4096, 4097, 8191,
	8192, 8193, 65539,
	1024 * 1024 + 7 };

// Called through volatile pointers so the compiler cannot inline them.
static void* (*volatile sMemcpy)(void*, const void*, size_t) = memcpy;
static void* (*volatile sMemmove)(void*, const void*, size_t) = memmove;
static void* (*volatile sMemset)(void*, int, size_t) = memset;

static uint8_t* sSource;
static uint8_t* sDest;
static uint8_t* sExpected;
static size_t sBufferSize;
static unsigned sFailures;


static uint8_t
pattern(size_t index, uint32_t seed)
{
	uint32_t x = (uint32_t)index * 2654435761u + seed;
	x ^= x >> 15;
	return (uint8_t)(x * 2246822519u >> 24);
}


static void
fill(uint8_t* buffer, size_t size, uint32_t seed)
{
	for (size_t i = 0; i < size; i++)
		buffer[i] = pattern(i, seed);
}


/*!	Byte-wise reference copy. The volatile stops the compiler from turning
	it into a call to the memcpy() under test.
*/
static void
copy(uint8_t* dest, const uint8_t* source, size_t size)
{
	volatile uint8_t* d = dest;
	for (size_t i = 0; i < size; i++)
		d[i] = source[i];
}


static void
fail(const char* function, size_t size, size_t a, size_t b, size_t at)
{
	if (sFailures++ < 20) {
		printf("FAIL %s size %zu align/offset %zu/%zu: first bad byte %zu\n",
			function, size, a, b, at);
	}
}


/*!	Compares the first \a total bytes of sDest and sExpected; \a size, \a a
	and \a b only describe the test case.
*/
static void
check(const char* function, size_t total, size_t size, size_t a, size_t b)
{
	if (memcmp(sDest, sExpected, total) == 0)
		return;
	for (size_t i = 0; i < total; i++) {
		if (sDest[i] != sExpected[i]) {
			fail(function, size, a, b, i);
			return;
		}
	}
}


static void
test_memcpy(size_t size)
{
	size_t total = kGuard + kAlign + size + kGuard;
	for (size_t srcAlign = 0; srcAlign < kAlign; srcAlign++) {
		for (size_t dstAlign = 0; dstAlign < kAlign; dstAlign++) {
			fill(sDest, total, 0x1234);
			copy(sExpected, sDest, total);
			uint8_t* src = sSource + kGuard + srcAlign;
			copy(sExpected + kGuard + dstAlign, src, size);

			uint8_t* dst = sDest + kGuard + dstAlign;
			if (sMemcpy(dst, src, size) != dst)
				fail("memcpy return", size, srcAlign, dstAlign, 0);
			check("memcpy", total, size, srcAlign, dstAlign);
		}
	}
}


static void
test_memset(size_t size)
{
	static const int kValues[] = { 0, 0x5a, 0xff, 0x100 + 0x81 };
	size_t total = kGuard + kAlign + size + kGuard;
	for (size_t v = 0; v < sizeof(kValues) / sizeof(kValues[0]); v++) {
		for (size_t dstAlign = 0; dstAlign < kAlign; dstAlign++) {
			fill(sDest, total, 0x5678);
			copy(sExpected, sDest, total);
			volatile uint8_t* expected = sExpected + kGuard + dstAlign;
			for (size_t i = 0; i < size; i++)
				expected[i] = (uint8_t)kValues[v];

			uint8_t* dst = sDest + kGuard + dstAlign;
			if (sMemset(dst, kValues[v], size) != dst)
				fail("memset return", size, v, dstAlign, 0);
			check("memset", total, size, v, dstAlign);
		}
	}
}


/*!	memmove() within one buffer, from offset \a from to offset \a to (both
	relative to kGuard), so the ranges overlap whenever they are closer than
	\a size.
*/
static void
test_memmove_one(size_t size, size_t from, size_t to)
{
	size_t total = kGuard + (from > to ? from : to) + size + kGuard;
	fill(sDest, total, 0x9abc + (uint32_t)size);
	copy(sExpected, sDest, total);
	copy(sExpected + kGuard + to, sDest + kGuard + from, size);

	uint8_t* dst = sDest + kGuard + to;
	if (sMemmove(dst, sDest + kGuard + from, size) != dst)
		fail("memmove return", size, from, to, 0);
	check("memmove", total, size, from, to);
}


static void
test_memmove(size_t size)
{
	static const size_t kDistances[] = { 0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 17,
		31, 32, 33, 63, 64, 65, 127, 128, 129 };
	for (size_t base = 0; base < kAlign; base++) {
		for (size_t d = 0; d < sizeof(kDistances) / sizeof(kDistances[0]);
				d++) {
			size_t distance = kDistances[d];
			test_memmove_one(size, base, base + distance);
			test_memmove_one(size, base + distance, base);
		}
		// Distances relative to the size, including no overlap at all.
		size_t relative[] = { size / 2, size - 1, size, size + 1 };
		for (size_t r = 0; r < 4; r++) {
			if (size == 0)
				break;
			test_memmove_one(size, base, base + relative[r]);
			test_memmove_one(size, base + relative[r], base);
		}
	}
}


static void
test_size(size_t size)
{
	test_memcpy(size);
	test_memset(size);
	test_memmove(size);
}


int
main()
{
	size_t largest = kLargeSizes[sizeof(kLargeSizes) / sizeof(kLargeSizes[0])
		- 1];
	sBufferSize = 2 * kGuard + 2 * largest + 2 * kAlign + 256;
	sSource = (uint8_t*)malloc(sBufferSize);
	sDest = (uint8_t*)malloc(sBufferSize);
	sExpected = (uint8_t*)malloc(sBufferSize);
	if (sSource == NULL || sDest == NULL || sExpected == NULL) {
		printf("out of memory\n");
		return 1;
	}
	fill(sSource, sBufferSize, 0xdef0);

	for (size_t size = 0; size <= kSmallMax; size++)
		test_size(size);
	for (size_t i = 0; i < sizeof(kLargeSizes) / sizeof(kLargeSizes[0]); i++)
		test_size(kLargeSizes[i]);

	if (sFailures != 0) {
		printf("%u failures\n", sFailures);
		return 1;
	}
	printf("memcpy/memmove/memset: all tests passed\n");
	return 0;
}
