/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * Shows and changes the Sony VAIO functions offered by the sony_snc driver.
 */


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sony_snc.h>


static const char* kThermalProfileNames[] = {
	"balanced",
	"silent",
	"performance"
};


static void
usage(int exitCode)
{
	fprintf(exitCode == 0 ? stdout : stderr,
		"Usage: sonyctl [status]\n"
		"       sonyctl battery-care off|50|80|100\n"
		"       sonyctl thermal balanced|silent|performance\n"
		"       sonyctl touchpad on|off\n"
		"\n"
		"Shows or changes the Sony VAIO functions of the sony_snc driver.\n");
	exit(exitCode);
}


static void
print_status(const sony_snc_status& status)
{
	if ((status.features & SONY_SNC_FEATURE_BATTERY_CARE) != 0) {
		if (status.battery_care_limit == 0) {
			printf("Battery care:      off\n");
		} else {
			printf("Battery care:      charge up to %" B_PRIu32 "%%\n",
				status.battery_care_limit);
		}
	}
	if ((status.features & SONY_SNC_FEATURE_BATTERY_HEALTH) != 0) {
		// a missing or dead battery reports values above 100
		if (status.battery_health > 100) {
			printf("Battery health:    unknown\n");
		} else {
			printf("Battery health:    %" B_PRIu32 "%%\n",
				status.battery_health);
		}
	}

	if ((status.features & SONY_SNC_FEATURE_THERMAL) != 0) {
		printf("Thermal profile:   %s (available:",
			status.thermal_profile < B_COUNT_OF(kThermalProfileNames)
				? kThermalProfileNames[status.thermal_profile] : "unknown");
		for (uint32 i = 0; i < B_COUNT_OF(kThermalProfileNames); i++) {
			if ((status.thermal_profiles & (1 << i)) != 0)
				printf(" %s", kThermalProfileNames[i]);
		}
		printf(")\n");
	}

	if ((status.features & SONY_SNC_FEATURE_TOUCHPAD) != 0) {
		printf("Touchpad:          %s\n",
			status.touchpad_enabled ? "on" : "off");
	}

	if ((status.features & SONY_SNC_FEATURE_WIRELESS) != 0) {
		printf("Wireless switch:   %s\n",
			status.wireless_switch_on ? "on" : "off");

		static const struct {
			uint32		radio;
			const char*	name;
		} kRadios[] = {
			{ SONY_SNC_RADIO_WLAN, "Wi-Fi" },
			{ SONY_SNC_RADIO_BLUETOOTH, "Bluetooth" },
			{ SONY_SNC_RADIO_WWAN, "WWAN" },
			{ SONY_SNC_RADIO_WIMAX, "WiMAX" },
		};
		for (size_t i = 0; i < B_COUNT_OF(kRadios); i++) {
			if ((status.radios & kRadios[i].radio) == 0)
				continue;
			printf("  %-16s %s\n", kRadios[i].name,
				(status.radios_enabled & kRadios[i].radio) != 0
					? "enabled" : "blocked");
		}
	}

	if (status.features == 0)
		printf("No supported functions found.\n");
}


static bool
parse_value(const char* command, const char* argument, uint32& op,
	uint32& value)
{
	if (strcmp(command, "battery-care") == 0) {
		op = SONY_SNC_SET_BATTERY_CARE;
		if (strcmp(argument, "off") == 0) {
			value = 0;
			return true;
		}
		char* end;
		value = strtoul(argument, &end, 10);
		return *end == '\0'
			&& (value == 50 || value == 80 || value == 100);
	}

	if (strcmp(command, "thermal") == 0) {
		op = SONY_SNC_SET_THERMAL_PROFILE;
		for (uint32 i = 0; i < B_COUNT_OF(kThermalProfileNames); i++) {
			if (strcmp(argument, kThermalProfileNames[i]) == 0) {
				value = i;
				return true;
			}
		}
		return false;
	}

	if (strcmp(command, "touchpad") == 0) {
		op = SONY_SNC_SET_TOUCHPAD;
		if (strcmp(argument, "on") == 0 || strcmp(argument, "off") == 0) {
			value = strcmp(argument, "on") == 0 ? 1 : 0;
			return true;
		}
		return false;
	}

	usage(1);
	return false;
}


int
main(int argc, char** argv)
{
	if (argc > 1 && (strcmp(argv[1], "-h") == 0
			|| strcmp(argv[1], "--help") == 0)) {
		usage(0);
	}
	if (argc > 3 || (argc == 3 && strcmp(argv[1], "status") == 0))
		usage(1);

	int fd = open(SONY_SNC_CONTROL_PATH, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "sonyctl: cannot open %s: %s\n", SONY_SNC_CONTROL_PATH,
			strerror(errno));
		return 1;
	}

	int result = 0;
	if (argc == 3) {
		uint32 op;
		uint32 value;
		if (!parse_value(argv[1], argv[2], op, value)) {
			fprintf(stderr, "sonyctl: invalid value \"%s\" for %s\n", argv[2],
				argv[1]);
			close(fd);
			return 1;
		}
		if (ioctl(fd, op, &value, sizeof(value)) != 0) {
			fprintf(stderr, "sonyctl: %s: %s\n", argv[1], strerror(errno));
			result = 1;
		}
	} else if (argc == 2 && strcmp(argv[1], "status") != 0) {
		usage(1);
	}

	sony_snc_status status;
	if (ioctl(fd, SONY_SNC_GET_STATUS, &status, sizeof(status)) != 0) {
		fprintf(stderr, "sonyctl: cannot get the status: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}
	print_status(status);

	close(fd);
	return result;
}
