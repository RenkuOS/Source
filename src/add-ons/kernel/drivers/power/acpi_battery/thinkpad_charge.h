/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef THINKPAD_CHARGE_H
#define THINKPAD_CHARGE_H


#include "device/power_managment.h"


status_t	thinkpad_get_charge_thresholds(const char* batteryPath,
				acpi_battery_charge_thresholds* thresholds);
status_t	thinkpad_set_charge_thresholds(const char* batteryPath,
				const acpi_battery_charge_thresholds* thresholds);


#endif	// THINKPAD_CHARGE_H
