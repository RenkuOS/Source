/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "thinkpad_charge_logic.h"

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
test_battery_number_comes_from_the_device_name()
{
	// The embedded controller counts BAT0 as 1 and BAT1 as 2, like the
	// thinkpad_acpi driver of Linux does.
	CHECK(thinkpad_battery_from_path("\\_SB_.PCI0.LPCB.BAT0")
		== THINKPAD_BATTERY_PRIMARY);
	CHECK(thinkpad_battery_from_path("\\_SB_.PCI0.LPCB.BAT1")
		== THINKPAD_BATTERY_SECONDARY);
	CHECK(thinkpad_battery_from_path("BAT0") == THINKPAD_BATTERY_PRIMARY);
}


static void
test_other_device_names_are_not_guessed()
{
	CHECK(thinkpad_battery_from_path("\\_SB_.PCI0.LPCB.BAT2")
		== THINKPAD_BATTERY_UNKNOWN);
	CHECK(thinkpad_battery_from_path("\\_SB_.BAT0X") == THINKPAD_BATTERY_UNKNOWN);
	CHECK(thinkpad_battery_from_path("\\_SB_.XBAT0") == THINKPAD_BATTERY_UNKNOWN);
	CHECK(thinkpad_battery_from_path("\\_SB_.PCI0.BAT0.") == THINKPAD_BATTERY_UNKNOWN);
	CHECK(thinkpad_battery_from_path("") == THINKPAD_BATTERY_UNKNOWN);
	CHECK(thinkpad_battery_from_path(NULL) == THINKPAD_BATTERY_UNKNOWN);
}


static void
test_bit_8_marks_support()
{
	// Start 75 and stop 80, with the support bits set.
	CHECK(thinkpad_threshold_supported(0x34b));
	CHECK(thinkpad_threshold_supported(0x350));
	CHECK(thinkpad_threshold_supported(0x100));
	CHECK(!thinkpad_threshold_supported(0x000));
	CHECK(!thinkpad_threshold_supported(0x04b));
}


static void
test_bit_31_is_an_error()
{
	CHECK(thinkpad_call_failed(0x80000000));
	CHECK(!thinkpad_threshold_supported(0x80000100));
	CHECK(!thinkpad_call_failed(0x34b));
}


static void
test_bit_9_marks_individual_addressing()
{
	CHECK(thinkpad_individual_addressing(0x34b));
	CHECK(!thinkpad_individual_addressing(0x14b));
}


static void
test_thresholds_are_in_the_low_byte()
{
	CHECK(thinkpad_start_from_result(0x34b) == 75);
	CHECK(thinkpad_stop_from_result(0x350) == 80);
	CHECK(thinkpad_start_from_result(0x300) == 0);
}


static void
test_stop_0_means_full_charge()
{
	CHECK(thinkpad_stop_from_result(0x300) == 100);
	CHECK(thinkpad_stop_from_result(0x364) == 100);
}


static void
test_arguments_carry_the_battery_in_bits_8_and_9()
{
	CHECK(thinkpad_start_argument(THINKPAD_BATTERY_PRIMARY, 75) == 0x14b);
	CHECK(thinkpad_start_argument(THINKPAD_BATTERY_SECONDARY, 55) == 0x237);
	CHECK(thinkpad_stop_argument(THINKPAD_BATTERY_PRIMARY, 80) == 0x150);
	CHECK(thinkpad_stop_argument(THINKPAD_BATTERY_SECONDARY, 60) == 0x23c);
	CHECK(thinkpad_start_argument(THINKPAD_BATTERY_PRIMARY, 0) == 0x100);
}


static void
test_full_charge_is_sent_as_0()
{
	// The embedded controller reads 0 as its default, a full charge.
	CHECK(thinkpad_stop_argument(THINKPAD_BATTERY_PRIMARY, 100) == 0x100);
	CHECK(thinkpad_stop_argument(THINKPAD_BATTERY_SECONDARY, 100) == 0x200);
}


static void
test_valid_thresholds()
{
	CHECK(charge_thresholds_valid(75, 80));
	CHECK(charge_thresholds_valid(55, 60));
	CHECK(charge_thresholds_valid(0, 100));
	CHECK(charge_thresholds_valid(0, 1));
	CHECK(charge_thresholds_valid(99, 100));
}


static void
test_invalid_thresholds()
{
	CHECK(!charge_thresholds_valid(0, 0));
	CHECK(!charge_thresholds_valid(0, 101));
	CHECK(!charge_thresholds_valid(80, 80));
	CHECK(!charge_thresholds_valid(85, 80));
	CHECK(!charge_thresholds_valid(100, 100));
	CHECK(!charge_thresholds_valid(0xffffffff, 80));
}


static void
test_lowering_the_limit_sets_the_start_first()
{
	// From 75/80 to 55/60: setting the stop first would leave 75/60.
	CHECK(charge_set_start_first(75, 60));
	CHECK(charge_set_start_first(75, 75));
}


static void
test_raising_the_limit_sets_the_stop_first()
{
	// From 55/60 to 75/80: setting the start first would leave 75/60.
	CHECK(!charge_set_start_first(55, 80));
	CHECK(!charge_set_start_first(0, 100));
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
	RUN(test_battery_number_comes_from_the_device_name);
	RUN(test_other_device_names_are_not_guessed);
	RUN(test_bit_8_marks_support);
	RUN(test_bit_31_is_an_error);
	RUN(test_bit_9_marks_individual_addressing);
	RUN(test_thresholds_are_in_the_low_byte);
	RUN(test_stop_0_means_full_charge);
	RUN(test_arguments_carry_the_battery_in_bits_8_and_9);
	RUN(test_full_charge_is_sent_as_0);
	RUN(test_valid_thresholds);
	RUN(test_invalid_thresholds);
	RUN(test_lowering_the_limit_sets_the_start_first);
	RUN(test_raising_the_limit_sets_the_stop_first);
	printf("%d tests, %d failures\n", sTests, sFailures);
	return sFailures == 0 ? 0 : 1;
}
