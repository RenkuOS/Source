/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _DW_I2C_TIMING_H
#define _DW_I2C_TIMING_H


/*!	SCL counts of the DesignWare I2C controller, computed from the clock
	rate of the block. They follow the DesignWare databook equations, as
	U-Boot's dw_i2c_calc_timing() does:
	  SCL high = (HCNT + SPKLEN + 3) / clock + fall time
	  SCL low  = (LCNT + 1) / clock - fall time + rise time

	Kept apart from the driver, so that the unit test in
	src/tests/add-ons/kernel/busses/i2c can check the results, which are:

	clock        bus      rise/fall ns  hcnt/lcnt  SDA hold (300 ns)
	49.5 MHz     100 kHz  1000/510      200/240    15
	49.5 MHz     400 kHz   300/510       17/86     15

	49.5 MHz is the I2C clock of the StarFive JH7110 (Milk-V Mars), the times
	are those of its device tree: "i2c-scl-falling-time-ns" = 510 and
	"i2c-sda-hold-time-ns" = 300, no rise time (the driver's default). The
	100 kHz row is what the driver programs and logs on the Milk-V Mars; the
	400 kHz row is only computed, as the device tree asks for 100 kHz.
*/


#include <stdint.h>
	// no Haiku types, so that the unit test also builds on the host


// minimum SCL high and low times (I2C specification), in ns
#define DW_I2C_SS_MIN_HIGH_NS	4000
#define DW_I2C_SS_MIN_LOW_NS	4700
#define DW_I2C_FS_MIN_HIGH_NS	600
#define DW_I2C_FS_MIN_LOW_NS	1300
#define DW_I2C_HD_STA_OFFSET	3
#define DW_I2C_SPIKE_COUNT		1		// IC_*_SPKLEN after reset


/*!	The number of clock cycles that last at least \a ns. */
static inline uint32_t
dw_i2c_ns_to_counts(uint64_t clock, uint32_t ns)
{
	return (uint32_t)((clock * ns + 999999999) / 1000000000);
}


/*!	HCNT and LCNT for a bus speed, back-solved from the minimum SCL times and
	then stretched evenly to reach the requested period.
*/
static inline void
dw_i2c_compute_scl_counts(uint64_t clock, uint32_t speed, uint32_t minHighNs,
	uint32_t minLowNs, uint32_t riseNs, uint32_t fallNs, uint16_t* _hcnt,
	uint16_t* _lcnt)
{
	int32_t fall = dw_i2c_ns_to_counts(clock, fallNs);
	int32_t rise = dw_i2c_ns_to_counts(clock, riseNs);
	int32_t hcnt = (int32_t)dw_i2c_ns_to_counts(clock, minHighNs) - fall
		- DW_I2C_HD_STA_OFFSET - DW_I2C_SPIKE_COUNT;
	int32_t lcnt = (int32_t)dw_i2c_ns_to_counts(clock, minLowNs) - rise + fall
		- 1;
	if (hcnt < 6)
		hcnt = 6;
	if (lcnt < 8)
		lcnt = 8;

	// the period these counts give, against the one asked for
	int32_t period = (int32_t)(clock / speed);
	int32_t total = hcnt + DW_I2C_SPIKE_COUNT + DW_I2C_HD_STA_OFFSET + lcnt + 1
		+ rise;
	if (period > total) {
		int32_t extra = period - total;
		hcnt += extra / 2;
		lcnt += extra - extra / 2;
	}
	*_hcnt = (uint16_t)(hcnt < 0xffff ? hcnt : 0xffff);
	*_lcnt = (uint16_t)(lcnt < 0xffff ? lcnt : 0xffff);
}


#endif	// _DW_I2C_TIMING_H
