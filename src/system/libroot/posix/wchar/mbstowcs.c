/*
** Copyright 2011, Oliver Tappe, zooey@hirschkaefer.de. All rights reserved.
** Distributed under the terms of the MIT License.
*/

#include <pthread.h>

#include <wchar_private.h>


size_t
__mbstowcs(wchar_t* pwc, const char* s, size_t n)
{
	static mbstate_t internalMbState;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	size_t result;

	pthread_mutex_lock(&lock);
	result = __mbsrtowcs(pwc, &s, n, &internalMbState);
	pthread_mutex_unlock(&lock);
	return result;
}


B_DEFINE_WEAK_ALIAS(__mbstowcs, mbstowcs);
