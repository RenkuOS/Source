/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */

/*!	issetugid() returns 0 for an ordinary process, whose real, effective and
	saved IDs are all equal. Exits with 0 on success.
*/


#include <stdio.h>
#include <unistd.h>


int
main(void)
{
	int result = issetugid();
	if (result != 0) {
		printf("FAIL: issetugid() returned %d for an ordinary process\n",
			result);
		return 1;
	}

	printf("PASS\n");
	return 0;
}
