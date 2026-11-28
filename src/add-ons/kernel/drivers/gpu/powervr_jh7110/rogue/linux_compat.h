/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 *
 * Minimal shim so that the FWIF headers ported from the Linux
 * drm/imagination driver compile as Haiku kernel C++.
 */
#ifndef PVR_FWIF_LINUX_COMPAT_H
#define PVR_FWIF_LINUX_COMPAT_H


#include <stddef.h>

#include <SupportDefs.h>


typedef uint8 u8;
typedef uint16 u16;
typedef uint32 u32;
typedef uint64 u64;
typedef int32 s32;
typedef int64 s64;

// Linux' aligned_u64: a 64 bit value that is 8 byte aligned even on ABIs
// where uint64 is only 4 byte aligned (the firmware is 32 bit MIPS).
typedef uint64 aligned_u64 __attribute__((aligned(8)));

#ifndef __aligned
#	define __aligned(x)	__attribute__((__aligned__(x)))
#endif

#ifndef BIT
#	define BIT(n)		(1UL << (n))
#endif

#ifndef BIT_ULL
#	define BIT_ULL(n)	(1ULL << (n))
#endif

#ifndef ARRAY_SIZE
#	define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))
#endif

#ifndef SZ_4K
#	define SZ_4K		0x00001000
#	define SZ_64K		0x00010000
#	define SZ_1M		0x00100000
#endif

#ifndef SZ_2M
#	define SZ_2M		0x00200000
#	define SZ_256M		0x10000000
#	define SZ_4G		0x0000000100000000ULL
#	define SZ_16G		0x0000000400000000ULL
#	define SZ_128G		0x0000002000000000ULL
#	define SZ_1T		0x0000010000000000ULL
#endif

// pvr_check.h
#define OFFSET_CHECK(type, member, offset) \
	static_assert(offsetof(type, member) == (offset), \
		"offsetof(" #type ", " #member ") incorrect")

#define SIZE_CHECK(type, size) \
	static_assert(sizeof(type) == (size), #type " is incorrect size")

// The last member is a flexible array: it starts in the tail padding of the
// structure (Linux: pvr_check.h, the C++ form of the same intent).
#define FLEX_ARRAY_CHECK(type, member) \
	static_assert(offsetof(type, member) <= sizeof(type) \
		&& sizeof(type) - offsetof(type, member) < alignof(type), \
		#type "->" #member " is not a trailing flexible array")


#endif	// PVR_FWIF_LINUX_COMPAT_H
