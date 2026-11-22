/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "ChargeLimit.h"


// Charging starts again 5 points below the limit, so that a battery on the
// adapter is not topped up all the time. A start of 0 leaves it to the
// firmware.
const charge_limit kChargeLimits[] = {
	{ 0, 100 },
	{ 75, 80 },
	{ 55, 60 }
};

const int32 kChargeLimitCount = sizeof(kChargeLimits) / sizeof(kChargeLimits[0]);


/*!	Returns the index of the choice whose stop all \a count batteries have,
	or -1 if they differ or no choice has it.
*/
int32
charge_limit_index(const uint32* stops, int32 count)
{
	if (count < 1)
		return -1;

	for (int32 i = 1; i < count; i++) {
		if (stops[i] != stops[0])
			return -1;
	}

	for (int32 i = 0; i < kChargeLimitCount; i++) {
		if (kChargeLimits[i].stop == stops[0])
			return i;
	}
	return -1;
}
