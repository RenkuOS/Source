/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	power: reports AC adapter, batteries, thermal zones and CPU idle time,
	once or as a series of samples (human readable or CSV), to measure the
	effect of power management changes.
*/


#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <new>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Directory.h>
#include <Entry.h>
#include <Path.h>
#include <StorageDefs.h>
#include <String.h>

#include <OS.h>
#include <scheduler.h>

#include <power_managment.h>


// Device directories published by the power drivers. Each one contains one
// node per instance, e.g. /dev/power/acpi_battery/0.
static const char* const kBatteryDirectory = "/dev/power/acpi_battery";
static const char* const kThermalDirectory = "/dev/power/acpi_thermal";
static const char* const kACDirectory = "/dev/power/acpi_ac";

static const int kMaxBatteries = 4;
static const int kMaxThermalZones = 4;
static const int kMaxACAdapters = 2;
static const int kMaxDevices = 8;

static int sBatteryFDs[kMaxBatteries];
static int sBatteryCount = 0;
static int sThermalFDs[kMaxThermalZones];
static int sThermalCount = 0;
static int sACFDs[kMaxACAdapters];
static int sACCount = 0;

static volatile sig_atomic_t sInterrupted = 0;


struct BatteryState {
	acpi_battery_info			info;
	acpi_extended_battery_info	extended;
	bool						hasExtended;
	status_t					status;
};


static void
handle_signal(int /*signal*/)
{
	sInterrupted = 1;
}


static void
usage()
{
	printf("Usage: power [options]\n"
		"Reports battery, AC adapter, thermal and CPU idle state.\n"
		"\n"
		"Options:\n"
		"  -c, --csv               write samples as CSV (implies -i)\n"
		"  -i, --interval SECONDS  sample interval in seconds (default 2)\n"
		"  -n, --count COUNT       number of samples (default: until "
			"interrupted)\n"
		"  -h, --help              show this help\n"
		"\n"
		"Without options, power prints the current state once.\n");
}


static void
open_devices_in(const char* directoryPath, int* fds, int maxCount, int& count)
{
	count = 0;

	BDirectory directory(directoryPath);
	if (directory.InitCheck() != B_OK)
		return;

	BString names[kMaxDevices];
	int nameCount = 0;

	BEntry entry;
	while (nameCount < maxCount && directory.GetNextEntry(&entry) == B_OK) {
		if (entry.IsDirectory())
			continue;

		char name[B_FILE_NAME_LENGTH];
		if (entry.GetName(name) != B_OK)
			continue;

		names[nameCount] = name;
		nameCount++;
	}

	// Sort the names so that instance numbers stay stable between runs; the
	// directory iteration order is not defined.
	for (int index = 1; index < nameCount; index++) {
		BString value = names[index];
		int position = index - 1;
		while (position >= 0 && names[position] > value) {
			names[position + 1] = names[position];
			position--;
		}
		names[position + 1] = value;
	}

	for (int index = 0; index < nameCount; index++) {
		BPath path(directoryPath, names[index].String());
		int fd = open(path.Path(), O_RDONLY);
		if (fd >= 0)
			fds[count++] = fd;
	}
}


static void
open_devices()
{
	open_devices_in(kBatteryDirectory, sBatteryFDs, kMaxBatteries,
		sBatteryCount);
	open_devices_in(kThermalDirectory, sThermalFDs, kMaxThermalZones,
		sThermalCount);
	open_devices_in(kACDirectory, sACFDs, kMaxACAdapters, sACCount);
}


static void
close_devices()
{
	for (int index = 0; index < sBatteryCount; index++)
		close(sBatteryFDs[index]);
	for (int index = 0; index < sThermalCount; index++)
		close(sThermalFDs[index]);
	for (int index = 0; index < sACCount; index++)
		close(sACFDs[index]);
}


static status_t
read_battery(int fd, BatteryState& battery)
{
	battery.hasExtended = false;
	battery.status = B_ERROR;

	if (ioctl(fd, GET_EXTENDED_BATTERY_INFO, &battery.extended,
			sizeof(acpi_extended_battery_info)) == 0)
		battery.hasExtended = true;

	if (ioctl(fd, GET_BATTERY_INFO, &battery.info,
			sizeof(acpi_battery_info)) != 0) {
		battery.status = errno;
		return battery.status;
	}

	battery.status = B_OK;
	return B_OK;
}


// Returns 1 when the adapter is online, 0 when it is offline and -1 when the
// state could not be read.
static int
read_ac(int fd)
{
	uint8 status = 0;
	ssize_t bytesRead = pread(fd, &status, sizeof(status), 0);
	if (bytesRead != (ssize_t)sizeof(status))
		return -1;

	return status != 0 ? 1 : 0;
}


static bool
parse_temperature(const char* text, const char* label, float& value)
{
	const char* position = strstr(text, label);
	if (position == NULL)
		return false;

	position += strlen(label);

	char* end = NULL;
	value = strtof(position, &end);
	return end != position;
}


static bool
read_thermal(int fd, float& current, float& critical)
{
	// The driver only returns its text when read from position 0.
	char buffer[512];
	ssize_t bytesRead = pread(fd, buffer, sizeof(buffer) - 1, 0);
	if (bytesRead <= 0)
		return false;

	buffer[bytesRead] = '\0';

	if (!parse_temperature(buffer, "Current Temperature:", current)
		|| !parse_temperature(buffer, "Critical Temperature:", critical))
		return false;

	// A zone without a working _TMP reports a wrapped negative value.
	return current > -50.0f && current < 200.0f;
}


static status_t
read_cpus(cpu_info*& infos, uint32& count, bigtime_t& wallTime)
{
	system_info systemInfo;
	status_t status = get_system_info(&systemInfo);
	if (status != B_OK)
		return status;

	infos = new(std::nothrow) cpu_info[systemInfo.cpu_count];
	if (infos == NULL)
		return B_NO_MEMORY;

	status = get_cpu_info(0, systemInfo.cpu_count, infos);
	if (status != B_OK) {
		delete[] infos;
		infos = NULL;
		return status;
	}

	count = systemInfo.cpu_count;
	wallTime = system_time();
	return B_OK;
}


static double
idle_percent(const cpu_info& previous, const cpu_info& current,
	bigtime_t previousTime, bigtime_t currentTime)
{
	bigtime_t active = current.active_time - previous.active_time;
	bigtime_t wall = currentTime - previousTime;
	if (wall <= 0 || active < 0)
		return 0.0;

	double percent = 100.0 - 100.0 * (double)active / (double)wall;
	if (percent < 0.0)
		return 0.0;
	if (percent > 100.0)
		return 100.0;

	return percent;
}


static double
average_frequency(const cpu_info* infos, uint32 count)
{
	if (count == 0)
		return 0.0;

	uint64 total = 0;
	for (uint32 index = 0; index < count; index++)
		total += infos[index].current_frequency;

	return (double)total / count / 1000000.0;
}


static const char*
battery_state_name(uint32 state)
{
	if ((state & BATTERY_CRITICAL_STATE) != 0)
		return "critical";
	if ((state & BATTERY_CHARGING) != 0)
		return "charging";
	if ((state & BATTERY_DISCHARGING) != 0)
		return "discharging";
	if ((state & BATTERY_NOT_CHARGING) != 0)
		return "not charging";

	return "idle";
}


static bool
battery_reports_ma(const BatteryState& battery)
{
	return battery.hasExtended
		&& battery.extended.power_unit == ACPI_BATTERY_UNIT_MA;
}


// Returns the battery rate in mW, converting from mA when the firmware
// reports in current units, or -1 when the rate is unknown.
static double
battery_rate_mw(const BatteryState& battery)
{
	if (battery.info.current_rate == UINT32_MAX)
		return -1;

	if (battery_reports_ma(battery)) {
		if (battery.info.voltage == 0 || battery.info.voltage == UINT32_MAX)
			return -1;
		return (double)battery.info.current_rate * battery.info.voltage
			/ 1000.0;
	}

	return battery.info.current_rate;
}


// ACPI reports an absent or unreadable battery with all values unknown.
static bool
battery_present(const BatteryState& battery)
{
	if (battery.status != B_OK)
		return false;
	return battery.info.capacity != UINT32_MAX
		|| (battery.hasExtended
			&& battery.extended.last_full_charge != UINT32_MAX);
}


static uint32
battery_full_charge(const BatteryState& battery)
{
	if (!battery.hasExtended)
		return 0;
	if (battery.extended.last_full_charge > 0
		&& battery.extended.last_full_charge != UINT32_MAX)
		return battery.extended.last_full_charge;

	return battery.extended.design_capacity;
}


static uint32
battery_percent(const BatteryState& battery)
{
	if (battery.info.capacity == UINT32_MAX)
		return 0;

	uint32 full = battery_full_charge(battery);
	if (full == 0)
		return 0;

	return (uint32)((uint64)battery.info.capacity * 100 / full);
}


// Returns the estimated minutes left while discharging, or -1 when unknown.
static int
battery_minutes_left(const BatteryState& battery)
{
	if ((battery.info.state & BATTERY_DISCHARGING) == 0)
		return -1;

	if (battery.info.capacity == UINT32_MAX
		|| battery.info.current_rate == 0
		|| battery.info.current_rate == UINT32_MAX)
		return -1;

	// Capacity and rate are in the same unit family: mWh and mW, or mAh
	// and mA.
	return (int)(battery.info.capacity * 60.0 / battery.info.current_rate);
}


static void
print_time_left(int minutes)
{
	if (minutes < 0) {
		printf("  -");
		return;
	}

	if (minutes >= 60) {
		printf("  ~%dh%02dm", minutes / 60, minutes % 60);
		return;
	}

	printf("  ~%dm", minutes);
}


static void
print_battery_line(int index, const BatteryState& battery)
{
	if (battery.status != B_OK || !battery_present(battery)) {
		printf("  Battery %d:      not present\n", index);
		return;
	}

	// The driver reports mWh or mAh; print Wh or Ah.
	const char* unit = battery_reports_ma(battery) ? "Ah" : "Wh";

	printf("  Battery %d:      %3" B_PRIu32 "%%  %.2f/%.2f %s  %s",
		index, battery_percent(battery),
		battery.info.capacity / 1000.0,
		battery_full_charge(battery) / 1000.0, unit,
		battery_state_name(battery.info.state));

	double rate = battery_rate_mw(battery);
	if (rate >= 0)
		printf("  %.2f W", rate / 1000.0);
	else
		printf("  ? W");

	print_time_left(battery_minutes_left(battery));

	if (battery.hasExtended && battery.extended.design_capacity > 0
		&& battery.extended.last_full_charge > 0) {
		printf("  health %" B_PRIu32 "%%",
			(uint32)((uint64)battery.extended.last_full_charge * 100
				/ battery.extended.design_capacity));
	}

	printf("\n");
}


static void
print_status()
{
	for (int index = 0; index < sACCount; index++) {
		int online = read_ac(sACFDs[index]);
		const char* text = "unknown";
		if (online == 1)
			text = "online";
		else if (online == 0)
			text = "offline";

		printf("  AC adapter %d:   %s\n", index, text);
	}

	if (sACCount == 0)
		printf("  AC adapter:     not found\n");

	// ACPI often describes more battery slots than the machine has; only
	// list the ones that hold a battery.
	int batteriesShown = 0;
	for (int index = 0; index < sBatteryCount; index++) {
		BatteryState battery;
		read_battery(sBatteryFDs[index], battery);
		if (battery.status != B_OK || !battery_present(battery))
			continue;
		print_battery_line(index, battery);
		batteriesShown++;
	}

	if (sBatteryCount == 0)
		printf("  Battery:        not found\n");
	else if (batteriesShown == 0)
		printf("  Battery:        not present\n");

	for (int index = 0; index < sThermalCount; index++) {
		float current = 0.0;
		float critical = 0.0;
		if (!read_thermal(sThermalFDs[index], current, critical)) {
			printf("  Thermal %d:      unavailable\n", index);
			continue;
		}

		printf("  Thermal %d:      %.1f C (critical %.1f C)\n", index,
			current, critical);
	}

	// Idle time over the next second, not since boot.
	cpu_info* before = NULL;
	cpu_info* infos = NULL;
	uint32 count = 0;
	uint32 countAfter = 0;
	bigtime_t beforeTime = 0;
	bigtime_t wallTime = 0;
	if (read_cpus(before, count, beforeTime) == B_OK) {
		snooze(1000000);
		if (read_cpus(infos, countAfter, wallTime) == B_OK) {
			if (countAfter < count)
				count = countAfter;

			double average = 0.0;
			printf("  CPU:            %" B_PRIu32 " cores  idle", count);
			for (uint32 index = 0; index < count; index++) {
				double percent = idle_percent(before[index], infos[index],
					beforeTime, wallTime);
				average += percent;
				printf(" %.0f%%", percent);
			}
			if (count > 0)
				average /= count;

			printf("  (avg %.0f%%)  %.0f MHz\n", average,
				average_frequency(infos, count));
			delete[] infos;
		}
		delete[] before;
	}

	int32 mode = get_scheduler_mode();
	const char* modeName = "unknown";
	if (mode == SCHEDULER_MODE_LOW_LATENCY)
		modeName = "low latency";
	else if (mode == SCHEDULER_MODE_POWER_SAVING)
		modeName = "power saving";

	printf("  Scheduler:      %s\n", modeName);
}


static void
print_monitor(int interval, int count, bool csv)
{
	cpu_info* previous = NULL;
	uint32 cpuCount = 0;
	bigtime_t previousTime = 0;
	if (read_cpus(previous, cpuCount, previousTime) != B_OK) {
		fprintf(stderr, "Cannot read CPU information.\n");
		return;
	}

	if (csv) {
		printf("time,ac,battery,percent,rate_mw,thermal_c,idle_percent,"
			"avg_mhz\n");
		fflush(stdout);
	}

	bigtime_t start = system_time();
	int sample = 0;
	while (!sInterrupted && (count == 0 || sample < count)) {
		snooze((bigtime_t)interval * 1000000);
		if (sInterrupted)
			break;

		cpu_info* current = NULL;
		uint32 currentCount = 0;
		bigtime_t currentTime = 0;
		if (read_cpus(current, currentCount, currentTime) != B_OK)
			break;

		double idleAverage = 0.0;
		uint32 idleCount = currentCount < cpuCount ? currentCount : cpuCount;
		for (uint32 index = 0; index < idleCount; index++) {
			idleAverage += idle_percent(previous[index], current[index],
				previousTime, currentTime);
		}
		if (idleCount > 0)
			idleAverage /= idleCount;

		int online = sACCount > 0 ? read_ac(sACFDs[0]) : -1;
		float temperature = 0.0;
		float critical = 0.0;
		bool hasTemperature = sThermalCount > 0
			&& read_thermal(sThermalFDs[0], temperature, critical);

		double elapsed = (currentTime - start) / 1000000.0;

		if (csv) {
			if (sBatteryCount == 0) {
				printf("%.1f,%d,,,,", elapsed, online);
				if (hasTemperature)
					printf("%.1f", temperature);
				printf(",%.1f,%.0f\n", idleAverage,
					average_frequency(current, currentCount));
			} else {
				for (int index = 0; index < sBatteryCount; index++) {
					BatteryState battery;
					read_battery(sBatteryFDs[index], battery);
					if (!battery_present(battery))
						continue;
					printf("%.1f,%d,%d,%" B_PRIu32 ",", elapsed, online,
						index, battery_percent(battery));
					double rate = battery_rate_mw(battery);
					if (rate >= 0)
						printf("%.0f", rate);
					printf(",");
					if (hasTemperature)
						printf("%.1f", temperature);
					printf(",%.1f,%.0f\n", idleAverage,
						average_frequency(current, currentCount));
				}
			}
			fflush(stdout);
		} else {
			printf("[%6.1fs] AC=%s", elapsed,
				online == 1 ? "on" : online == 0 ? "off" : "?");

			for (int index = 0; index < sBatteryCount; index++) {
				BatteryState battery;
				read_battery(sBatteryFDs[index], battery);
				if (!battery_present(battery))
					continue;
				double rate = battery_rate_mw(battery);
				printf("  bat%d=%" B_PRIu32 "%% ", index,
					battery_percent(battery));
				if (rate >= 0)
					printf("%.2fW", rate / 1000.0);
				else
					printf("?W");
				printf(" %s", battery_state_name(battery.info.state));
			}

			if (hasTemperature)
				printf("  %.1fC", temperature);

			printf("  idle=%.0f%%  %.0fMHz\n", idleAverage,
				average_frequency(current, currentCount));
			fflush(stdout);
		}

		delete[] previous;
		previous = current;
		previousTime = currentTime;
		sample++;
	}

	delete[] previous;
}


int
main(int argc, char** argv)
{
	int interval = 2;
	int count = 0;
	bool csv = false;
	bool monitor = false;

	int option;
	static const struct option kLongOptions[] = {
		{ "csv", no_argument, NULL, 'c' },
		{ "interval", required_argument, NULL, 'i' },
		{ "count", required_argument, NULL, 'n' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	while ((option = getopt_long(argc, argv, "chi:n:", kLongOptions, NULL))
			!= -1) {
		switch (option) {
			case 'c':
				csv = true;
				monitor = true;
				break;

			case 'i':
				interval = atoi(optarg);
				monitor = true;
				break;

			case 'n':
				count = atoi(optarg);
				monitor = true;
				break;

			case 'h':
				usage();
				return 0;

			default:
				usage();
				return 1;
		}
	}

	if (interval < 1)
		interval = 1;
	if (count < 0)
		count = 0;

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	open_devices();

	if (monitor)
		print_monitor(interval, count, csv);
	else
		print_status();

	close_devices();
	return 0;
}
