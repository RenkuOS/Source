/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Concurrent use of distinct locales from several threads. Every thread
	sets its own C or POSIX locale with newlocale()/uselocale(), then converts
	a long string with mbstowcs()/wcstombs(). The shared mbstate_t holds the
	ICU converter; without the lock the converters get mixed up and the
	converted text comes back wrong or libroot crashes.
*/


#include <locale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <wchar.h>


extern "C" size_t mbstowcs(wchar_t* pwc, const char* s, size_t n);
extern "C" size_t wcstombs(char* s, const wchar_t* pwcs, size_t n);


static const char* kText = "the quick brown fox jumps over the lazy dog 0123456789";
static int sFailures = 0;
static pthread_mutex_t sFailureLock = PTHREAD_MUTEX_INITIALIZER;


static void*
convert(void* data)
{
	long id = (long)data;
	locale_t locale = newlocale(LC_ALL_MASK, id % 2 == 0 ? "C" : "POSIX", NULL);
	if (locale == NULL) {
		pthread_mutex_lock(&sFailureLock);
		sFailures++;
		pthread_mutex_unlock(&sFailureLock);
		return NULL;
	}
	uselocale(locale);

	for (int round = 0; round < 200; round++) {
		wchar_t wide[256];
		char narrow[256];
		memset(wide, 0, sizeof(wide));
		memset(narrow, 0, sizeof(narrow));
		size_t w = mbstowcs(wide, kText, 256);
		size_t n = wcstombs(narrow, wide, 256);
		if (w != strlen(kText) || n != w || memcmp(narrow, kText, n) != 0) {
			pthread_mutex_lock(&sFailureLock);
			if (sFailures < 5)
				printf("thread %ld round %d: w=%zu n=%zu\n", id, round, w, n);
			sFailures++;
			pthread_mutex_unlock(&sFailureLock);
		}
	}
	uselocale(LC_GLOBAL_LOCALE);
	freelocale(locale);
	return NULL;
}


int
main()
{
	pthread_t threads[8];
	for (long i = 0; i < 8; i++)
		pthread_create(&threads[i], NULL, convert, (void*)i);
	for (int i = 0; i < 8; i++)
		pthread_join(threads[i], NULL);

	const char* result = sFailures == 0
		? "ok, 8 threads x 200 locale conversions" : "FAILED";
	printf("RESULT: %s (%d bad conversions)\n", result, sFailures);
	syslog(LOG_INFO, "wchar_thread_test: RESULT: %s (%d bad conversions)",
		result, sFailures);
	return sFailures == 0 ? 0 : 1;
}
