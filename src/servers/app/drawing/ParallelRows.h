/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 */
#ifndef PARALLEL_ROWS_H
#define PARALLEL_ROWS_H


#include <SupportDefs.h>


typedef void (*parallel_rows_function)(void* cookie, int32 first,
	int32 count);


/*!	Runs function over the rows [0, rows) split into one range per CPU (up
	to four), on helper threads kept for that and on the calling thread.
	Returns false without running anything when the work, totalBytes, is too
	small to be worth it, when the helpers are busy with another caller, or
	when there are none; the caller then does the work itself.
*/
bool parallel_rows(int32 rows, size_t totalBytes,
	parallel_rows_function function, void* cookie);


#endif	// PARALLEL_ROWS_H
