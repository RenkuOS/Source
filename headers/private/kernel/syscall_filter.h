/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */
#ifndef _KERNEL_SYSCALL_FILTER_H
#define _KERNEL_SYSCALL_FILTER_H


#include <OS.h>


#ifdef __cplusplus
extern "C" {
#endif

status_t syscall_filter_init(void);

/*!	Called on every syscall of a thread that has THREAD_FLAGS_SYSCALL_FILTERED
	set, before the syscall runs. \a args are the syscall's arguments, one
	64 bit slot each. Returns \c B_OK to let the syscall run, or the error
	(\c B_NOT_ALLOWED) to return instead.
*/
status_t syscall_filter_check(uint32 syscall, const uint64* args);

/*!	Whether the current team may call the generic syscall \a function of
	\a subsystem, which must be a kernel copy of the name. Generic syscalls
	are dispatched by name, so syscall_filter_check() cannot tell them apart.
*/
bool syscall_filter_allows_generic_syscall(const char* subsystem,
	uint32 function);

status_t _user_restrict_syscalls(uint32 allowedGroups);

#ifdef __cplusplus
}
#endif


#endif	/* _KERNEL_SYSCALL_FILTER_H */
