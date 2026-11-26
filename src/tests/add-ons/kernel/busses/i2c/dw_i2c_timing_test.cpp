/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


/*!	Checks the SCL counts of the DesignWare I2C controller driver against the
	values listed in dw_i2c_timing.h. It has no dependency on the
	kernel and can also be built on the host:
	  g++ -I src/add-ons/kernel/busses/i2c/pch \
		src/tests/add-ons/kernel/busses/i2c/dw_i2c_timing_test.cpp
*/


#include <inttypes.h>
#include <stdio.h>

#include "dw_i2c_timing.h"


struct scl_case {
	const char*	name;
	uint64_t	clock;
	uint32_t	speed;
	uint32_t	riseNs;
	uint32_t	fallNs;
	uint16_t	hcnt;
	uint16_t	lcnt;
};


// StarFive JH7110 (Milk-V Mars): 49.5 MHz, i2c-scl-falling-time-ns = 510
static const scl_case kCases[] = {
	{ "JH7110 100 kHz", 49500000, 100000, 1000, 510, 200, 240 },
	{ "JH7110 400 kHz", 49500000, 400000, 300, 510, 17, 86 },
};


int
main()
{
	int failures = 0;

	for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
		const scl_case& test = kCases[i];
		bool standard = test.speed <= 100000;
		uint16_t hcnt, lcnt;
		dw_i2c_compute_scl_counts(test.clock, test.speed,
			standard ? DW_I2C_SS_MIN_HIGH_NS : DW_I2C_FS_MIN_HIGH_NS,
			standard ? DW_I2C_SS_MIN_LOW_NS : DW_I2C_FS_MIN_LOW_NS,
			test.riseNs, test.fallNs, &hcnt, &lcnt);

		bool ok = hcnt == test.hcnt && lcnt == test.lcnt;
		printf("%s: %s: hcnt/lcnt %u/%u, expected %u/%u\n",
			ok ? "PASS" : "FAIL", test.name, hcnt, lcnt, test.hcnt,
			test.lcnt);
		if (!ok)
			failures++;
	}

	// SDA hold time of 300 ns at 49.5 MHz
	uint32_t hold = dw_i2c_ns_to_counts(49500000, 300);
	printf("%s: JH7110 SDA hold: %" PRIu32 ", expected 15\n",
		hold == 15 ? "PASS" : "FAIL", hold);
	if (hold != 15)
		failures++;

	return failures == 0 ? 0 : 1;
}
