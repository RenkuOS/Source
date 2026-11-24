/*
 * Copyright 2022, The Haiku Team. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _X86_THREAD_DEFS_H
#define _X86_THREAD_DEFS_H


#include <SupportDefs.h>


#define THREAD_SYSCALLS			"thread"
#define THREAD_SET_GS_BASE		1
#define THREAD_SET_LDT_ENTRY	2
	// buffer: struct thread_ldt_entry, for the calling thread's team
#define THREAD_SET_FS_BASE		3
	// buffer: uint64, the FS base for 64-bit code (0: the thread's TLS);
	// also sets the FS selector to 0
#define THREAD_GET_FS_BASE		4
	// buffer: uint64, receives the FS base used while the selector is 0
#define THREAD_GET_GS_BASE		5
	// buffer: uint64, receives the user GS base

// The local descriptor table of a team has this many entries.
#define THREAD_LDT_ENTRY_COUNT	8192


struct thread_ldt_entry {
	uint32	index;
	uint32	descriptor[2];
		// The raw segment descriptor, as the CPU reads it from the table. Only
		// present, writable 32-bit data segments of DPL 3 are accepted; all
		// zeros clears the entry.
};


#endif	/* _X86_THREAD_DEFS_H */
