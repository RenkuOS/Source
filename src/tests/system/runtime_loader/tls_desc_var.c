/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*	The thread-local variable for tls_desc_test.c, which says how to build
	and run it. Its names do not start with tls_, as libroot exports
	tls_address() and others, which a definition here would interpose.
*/


__thread int probe_counter = 7;


int
probe_bump(int n)
{
	probe_counter += n;
	return probe_counter;
}


int*
probe_address(void)
{
	return &probe_counter;
}
