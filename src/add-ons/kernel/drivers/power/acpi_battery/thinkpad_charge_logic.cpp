/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "thinkpad_charge_logic.h"

#include <string.h>


// Bits of the values the BCTG and BCSG methods return.
#define THINKPAD_RESULT_SUPPORTED	(1UL << 8)
#define THINKPAD_RESULT_INDIVIDUAL	(1UL << 9)
#define THINKPAD_RESULT_ERROR		(1UL << 31)
#define THINKPAD_THRESHOLD_MASK		0xff


/*!	Returns the number the embedded controller uses for the battery whose
	ACPI device is at \a acpiPath. Like Linux, it goes by the name of the
	device: BAT0 is the primary battery, BAT1 the secondary one. Any other
	name is not guessed.
*/
int32
thinkpad_battery_from_path(const char* acpiPath)
{
	if (acpiPath == NULL)
		return THINKPAD_BATTERY_UNKNOWN;

	const char* name = strrchr(acpiPath, '.');
	name = name != NULL ? name + 1 : acpiPath;

	if (strcmp(name, "BAT0") == 0)
		return THINKPAD_BATTERY_PRIMARY;
	if (strcmp(name, "BAT1") == 0)
		return THINKPAD_BATTERY_SECONDARY;
	return THINKPAD_BATTERY_UNKNOWN;
}


bool
thinkpad_call_failed(uint32 result)
{
	return (result & THINKPAD_RESULT_ERROR) != 0;
}


bool
thinkpad_threshold_supported(uint32 result)
{
	return !thinkpad_call_failed(result)
		&& (result & THINKPAD_RESULT_SUPPORTED) != 0;
}


bool
thinkpad_individual_addressing(uint32 result)
{
	return (result & THINKPAD_RESULT_INDIVIDUAL) != 0;
}


uint32
thinkpad_start_from_result(uint32 result)
{
	return result & THINKPAD_THRESHOLD_MASK;
}


/*!	A stop threshold of 0 is the default of the embedded controller: the
	battery is charged fully.
*/
uint32
thinkpad_stop_from_result(uint32 result)
{
	uint32 stop = result & THINKPAD_THRESHOLD_MASK;
	return stop == 0 ? 100 : stop;
}


uint32
thinkpad_start_argument(int32 battery, uint32 start)
{
	return ((uint32)battery << 8) | (start & THINKPAD_THRESHOLD_MASK);
}


uint32
thinkpad_stop_argument(int32 battery, uint32 stop)
{
	if (stop == 100)
		stop = 0;
	return ((uint32)battery << 8) | (stop & THINKPAD_THRESHOLD_MASK);
}


/*!	Charging stops at \a stop, 1 to 100 percent, and starts again below
	\a start, which must be lower. A start of 0 leaves it to the firmware.
*/
bool
charge_thresholds_valid(uint32 start, uint32 stop)
{
	return stop >= 1 && stop <= 100 && start < stop;
}


/*!	Returns whether the new start must be set before the new \a stop, so
	that the start never lies above the stop in between.
*/
bool
charge_set_start_first(uint32 currentStart, uint32 stop)
{
	return stop <= currentStart;
}
