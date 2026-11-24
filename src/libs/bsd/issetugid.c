/*
 * Copyright 2006, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 */


#include <unistd.h>


int
issetugid(void)
{
	uid_t realUser, effectiveUser, savedUser;
	gid_t realGroup, effectiveGroup, savedGroup;

	if (getresuid(&realUser, &effectiveUser, &savedUser) != 0
		|| getresgid(&realGroup, &effectiveGroup, &savedGroup) != 0) {
		return 1;
	}

	// The kernel does not record whether an exec() changed our IDs, so any
	// difference between the real, effective, and saved IDs counts as tainted.
	// TODO: a process that has fully given up its privileges counts as
	//	untainted; that needs kernel support to track.
	return realUser != effectiveUser || realUser != savedUser
		|| realGroup != effectiveGroup || realGroup != savedGroup;
}
