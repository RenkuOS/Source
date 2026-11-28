/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _SONY_SNC_H
#define _SONY_SNC_H


#include <Drivers.h>


/*!	Control interface of the sony_snc driver, published as
	/dev/power/sony_snc/<n>. Only the functions the firmware lists are
	available; SONY_SNC_GET_STATUS tells which ones.
*/


#define SONY_SNC_CONTROL_PATH			"/dev/power/sony_snc/0"
#define SONY_SNC_API_VERSION			1

enum {
	SONY_SNC_GET_STATUS = B_DEVICE_OP_CODES_END + 0x5300,
		// arg: sony_snc_status*
	SONY_SNC_SET_BATTERY_CARE,
		// arg: uint32*, charge limit in percent: 0 (off), 50, 80 or 100
	SONY_SNC_SET_THERMAL_PROFILE,
		// arg: uint32*, one of sony_snc_thermal_profile
	SONY_SNC_SET_TOUCHPAD,
		// arg: uint32*, 0 to disable the touchpad, 1 to enable it
};

// sony_snc_status::features
enum {
	SONY_SNC_FEATURE_BATTERY_CARE	= 0x01,
	SONY_SNC_FEATURE_BATTERY_HEALTH	= 0x02,
	SONY_SNC_FEATURE_THERMAL		= 0x04,
	SONY_SNC_FEATURE_TOUCHPAD		= 0x08,
	SONY_SNC_FEATURE_WIRELESS		= 0x10,
};

typedef enum {
	SONY_SNC_THERMAL_BALANCED		= 0,
	SONY_SNC_THERMAL_SILENT			= 1,
	SONY_SNC_THERMAL_PERFORMANCE	= 2,
} sony_snc_thermal_profile;

// sony_snc_status::radios and radios_enabled
enum {
	SONY_SNC_RADIO_WLAN				= 0x01,
	SONY_SNC_RADIO_BLUETOOTH		= 0x02,
	SONY_SNC_RADIO_WWAN				= 0x04,
	SONY_SNC_RADIO_WIMAX			= 0x08,
};

typedef struct sony_snc_status {
	uint32	api_version;
	uint32	features;

	uint32	battery_care_limit;
		// percent, 0 if battery care is off
	uint32	battery_health;
		// percent, as reported by the firmware

	uint32	thermal_profile;
	uint32	thermal_profiles;
		// bit (1 << profile) set for every selectable profile

	uint32	touchpad_enabled;

	uint32	wireless_switch_on;
		// position of the hardware wireless switch
	uint32	radios;
		// SONY_SNC_RADIO_* present
	uint32	radios_enabled;
		// SONY_SNC_RADIO_* not blocked in software
} sony_snc_status;


#endif	// _SONY_SNC_H
