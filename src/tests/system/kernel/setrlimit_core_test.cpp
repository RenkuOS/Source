/*
 * Copyright 2026, RenkuOS contributors.
 * SPDX-License-Identifier: MIT
 */


/*!	Tests the RLIMIT_CORE case of getrlimit() and setrlimit(). Core files
	are controlled by debug_server, so the limit is always 0/0: asking for
	exactly that must succeed (as "ulimit -c 0" and ssh do), any other value
	must fail with EINVAL. Exits 0 when every case passes.
*/


#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>


static int sFailures = 0;


static void
check_set(const char* name, rlim_t current, rlim_t maximum, int expectedError)
{
	struct rlimit limit;
	limit.rlim_cur = current;
	limit.rlim_max = maximum;

	errno = 0;
	int result = setrlimit(RLIMIT_CORE, &limit);
	int error = result == 0 ? 0 : errno;

	if (error != expectedError) {
		printf("FAIL %s: setrlimit() returned %d, errno %s, expected %s\n",
			name, result, strerror(error), strerror(expectedError));
		sFailures++;
	} else
		printf("ok   %s\n", name);
}


int
main()
{
	struct rlimit limit;
	if (getrlimit(RLIMIT_CORE, &limit) != 0) {
		printf("FAIL getrlimit(RLIMIT_CORE): %s\n", strerror(errno));
		sFailures++;
	} else if (limit.rlim_cur != 0 || limit.rlim_max != 0) {
		printf("FAIL getrlimit(RLIMIT_CORE) reports %llu/%llu, expected 0/0\n",
			(unsigned long long)limit.rlim_cur,
			(unsigned long long)limit.rlim_max);
		sFailures++;
	} else
		printf("ok   getrlimit() reports 0/0\n");

	check_set("setrlimit(0/0) succeeds", 0, 0, 0);
	check_set("setrlimit(0/unlimited) fails", 0, RLIM_INFINITY, EINVAL);
	check_set("setrlimit(unlimited/unlimited) fails", RLIM_INFINITY,
		RLIM_INFINITY, EINVAL);
	check_set("setrlimit(1024/1024) fails", 1024, 1024, EINVAL);

	if (sFailures != 0) {
		printf("%d case(s) failed\n", sFailures);
		return 1;
	}

	printf("all cases passed\n");
	return 0;
}
