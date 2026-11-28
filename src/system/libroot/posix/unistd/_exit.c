/* 
 * Copyright 2004, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <stdlib.h>
#include <unistd.h>
#include <syscalls.h>

#include <libroot_private.h>


extern void _IO_cleanup(void);

void
_exit(int status)
{
#ifdef __HAIKU_BEOS_COMPATIBLE
	// POSIX says _exit() must not flush streams; BeOS binaries expect it to
	if (__gABIVersion < B_HAIKU_ABI_GCC_2_HAIKU)
		_IO_cleanup();
#endif

	// exit with status code
	_kern_exit_team(status);
}


void
_Exit(int status)
{
	_exit(status);
}