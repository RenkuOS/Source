/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "ChargeLimit.h"

#include <stdio.h>


static int sFailures = 0;
static int sTests = 0;


#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			sFailures++; \
		} \
	} while (0)


static void
test_choices_start_5_points_below_the_stop()
{
	CHECK(kChargeLimitCount == 3);
	CHECK(kChargeLimits[0].stop == 100);
	CHECK(kChargeLimits[0].start == 0);
	CHECK(kChargeLimits[1].stop == 80);
	CHECK(kChargeLimits[1].start == 75);
	CHECK(kChargeLimits[2].stop == 60);
	CHECK(kChargeLimits[2].start == 55);
}


static void
test_choice_is_found_by_its_stop()
{
	uint32 stop = 80;
	CHECK(charge_limit_index(&stop, 1) == 1);
	stop = 100;
	CHECK(charge_limit_index(&stop, 1) == 0);
	stop = 60;
	CHECK(charge_limit_index(&stop, 1) == 2);
}


static void
test_other_stop_marks_no_choice()
{
	uint32 stop = 90;
	CHECK(charge_limit_index(&stop, 1) == -1);
	stop = 0;
	CHECK(charge_limit_index(&stop, 1) == -1);
}


static void
test_batteries_with_the_same_stop_mark_it()
{
	uint32 stops[] = { 80, 80 };
	CHECK(charge_limit_index(stops, 2) == 1);
}


static void
test_batteries_with_different_stops_mark_nothing()
{
	// A tick would claim that the choice applies to every battery.
	uint32 stops[] = { 80, 100 };
	CHECK(charge_limit_index(stops, 2) == -1);
}


static void
test_no_battery_marks_nothing()
{
	CHECK(charge_limit_index(NULL, 0) == -1);
}


#define RUN(test) \
	do { \
		sTests++; \
		int before = sFailures; \
		test(); \
		printf("%s %s\n", sFailures == before ? "ok    " : "FAILED", #test); \
	} while (0)


int
main()
{
	RUN(test_choices_start_5_points_below_the_stop);
	RUN(test_choice_is_found_by_its_stop);
	RUN(test_other_stop_marks_no_choice);
	RUN(test_batteries_with_the_same_stop_mark_it);
	RUN(test_batteries_with_different_stops_mark_nothing);
	RUN(test_no_battery_marks_nothing);
	printf("%d tests, %d failures\n", sTests, sFailures);
	return sFailures == 0 ? 0 : 1;
}
