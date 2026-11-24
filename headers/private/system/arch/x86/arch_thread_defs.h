/*
 * Copyright 2022, The Haiku Team. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _X86_THREAD_DEFS_H
#define _X86_THREAD_DEFS_H


#define THREAD_SYSCALLS			"thread"
#define THREAD_SET_GS_BASE		1
	// 2 is reserved for THREAD_SET_LDT_ENTRY
#define THREAD_SET_FS_BASE		3
	// buffer: uint64, the FS base for 64-bit code (0: the thread's TLS);
	// also sets the FS selector to 0
#define THREAD_GET_FS_BASE		4
	// buffer: uint64, receives the FS base used while the selector is 0
#define THREAD_GET_GS_BASE		5
	// buffer: uint64, receives the user GS base


#endif	/* _X86_THREAD_DEFS_H */
