/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef THINKPAD_CHARGE_LOGIC_H
#define THINKPAD_CHARGE_LOGIC_H


#include <SupportDefs.h>


// Battery numbers of the ThinkPad embedded controller.
enum {
	THINKPAD_BATTERY_UNKNOWN	= 0,
	THINKPAD_BATTERY_PRIMARY	= 1,
	THINKPAD_BATTERY_SECONDARY	= 2
};


int32	thinkpad_battery_from_path(const char* acpiPath);

bool	thinkpad_threshold_supported(uint32 result);
bool	thinkpad_individual_addressing(uint32 result);
bool	thinkpad_call_failed(uint32 result);

uint32	thinkpad_start_from_result(uint32 result);
uint32	thinkpad_stop_from_result(uint32 result);
uint32	thinkpad_start_argument(int32 battery, uint32 start);
uint32	thinkpad_stop_argument(int32 battery, uint32 stop);

bool	charge_thresholds_valid(uint32 start, uint32 stop);
bool	charge_set_start_first(uint32 currentStart, uint32 stop);


#endif	// THINKPAD_CHARGE_LOGIC_H
