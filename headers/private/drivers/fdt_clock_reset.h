/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _FDT_CLOCK_RESET_H
#define _FDT_CLOCK_RESET_H


#include <device_manager.h>


/*!	Clocks and resets of device tree nodes ("clocks"/"clock-names",
	"resets"/"reset-names"), provided by a SoC specific module.

	A consumer passes its own FDT device node and the name of the entry (or
	NULL and an index) and gets an opaque handle back. Enabling a clock also
	enables its parents; disabling only touches the clock itself, so a
	consumer can never stop a clock another device depends on.
*/

typedef uint32 fdt_clock;
typedef uint32 fdt_reset;


typedef struct fdt_clock_reset_module_info {
	module_info info;

	status_t	(*get_clock)(device_node* fdtNode, const char* name,
					uint32 index, fdt_clock* _clock);
	status_t	(*enable_clock)(fdt_clock clock, bool enable);
	bool		(*is_clock_enabled)(fdt_clock clock);
	int64		(*get_clock_rate)(fdt_clock clock);
				// in Hz, or a negative error code
	int64		(*set_clock_rate)(fdt_clock clock, int64 rate);
				// returns the rate actually set, or a negative error code

	status_t	(*get_reset)(device_node* fdtNode, const char* name,
					uint32 index, fdt_reset* _reset);
	status_t	(*set_reset)(fdt_reset reset, bool assert);
	bool		(*is_reset_asserted)(fdt_reset reset);
} fdt_clock_reset_module_info;


// StarFive JH7110 clock and reset generators
#define JH7110_CLOCK_RESET_MODULE_NAME "generic/jh7110_clock_reset/v1"


#endif	// _FDT_CLOCK_RESET_H
