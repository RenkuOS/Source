/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "thinkpad_charge.h"

#include <ACPI.h>
#include <KernelExport.h>

#include <util/AutoLock.h>

#include "thinkpad_charge_logic.h"


#define ERROR(x...) dprintf("acpi_battery: " x)


// Hardware ids of the ThinkPad hotkey device, which has the charge methods.
static const char* const kHotkeyIds[] = {
	"LEN0068",
	"LEN0268",
	"IBM0068"
};

// Serializes reading and setting the thresholds.
static mutex sThresholdsLock = MUTEX_INITIALIZER("thinkpad charge thresholds");


static status_t
find_hotkey(acpi_module_info* acpi, acpi_handle* hotkey)
{
	char path[255];
	for (size_t i = 0; i < B_COUNT_OF(kHotkeyIds); i++) {
		if (acpi->get_device(kHotkeyIds[i], 0, path, sizeof(path)) == B_OK
			&& acpi->get_handle(NULL, path, hotkey) == B_OK) {
			return B_OK;
		}
	}
	return B_NOT_SUPPORTED;
}


static status_t
call_hotkey(acpi_module_info* acpi, acpi_handle hotkey, const char* method,
	uint32 argument, uint32* result)
{
	acpi_object_type argumentObject;
	argumentObject.object_type = ACPI_TYPE_INTEGER;
	argumentObject.integer.integer = argument;

	acpi_objects arguments;
	arguments.count = 1;
	arguments.pointer = &argumentObject;

	acpi_object_type resultObject;
	acpi_data buffer;
	buffer.pointer = &resultObject;
	buffer.length = sizeof(resultObject);

	status_t status = acpi->evaluate_method(hotkey, method, &arguments,
		&buffer);
	if (status != B_OK)
		return status;
	if (resultObject.object_type != ACPI_TYPE_INTEGER)
		return B_BAD_DATA;

	*result = (uint32)resultObject.integer.integer;
	return B_OK;
}


/*!	Reads the thresholds of the battery at \a batteryPath and checks that
	the embedded controller supports them, as the thinkpad_acpi driver of
	Linux does.
*/
static status_t
read_thresholds(acpi_module_info* acpi, const char* batteryPath,
	acpi_handle* _hotkey, int32* _battery,
	acpi_battery_charge_thresholds* thresholds)
{
	int32 battery = thinkpad_battery_from_path(batteryPath);
	if (battery == THINKPAD_BATTERY_UNKNOWN)
		return B_NOT_SUPPORTED;

	acpi_handle hotkey;
	status_t status = find_hotkey(acpi, &hotkey);
	if (status != B_OK)
		return status;

	uint32 start;
	if (call_hotkey(acpi, hotkey, "BCTG", battery, &start) != B_OK
		|| !thinkpad_threshold_supported(start)) {
		return B_NOT_SUPPORTED;
	}

	// Without individual addressing, only the primary battery is known.
	if (battery != THINKPAD_BATTERY_PRIMARY
		&& !thinkpad_individual_addressing(start)) {
		return B_NOT_SUPPORTED;
	}

	uint32 stop;
	if (call_hotkey(acpi, hotkey, "BCSG", battery, &stop) != B_OK
		|| !thinkpad_threshold_supported(stop)) {
		return B_NOT_SUPPORTED;
	}

	thresholds->start = thinkpad_start_from_result(start);
	thresholds->stop = thinkpad_stop_from_result(stop);
	*_hotkey = hotkey;
	*_battery = battery;
	return B_OK;
}


static status_t
set_threshold(acpi_module_info* acpi, acpi_handle hotkey, const char* method,
	uint32 argument)
{
	uint32 result;
	status_t status = call_hotkey(acpi, hotkey, method, argument, &result);
	if (status == B_OK && thinkpad_call_failed(result))
		status = B_IO_ERROR;
	if (status != B_OK)
		ERROR("%s(0x%" B_PRIx32 ") failed\n", method, argument);
	return status;
}


status_t
thinkpad_get_charge_thresholds(const char* batteryPath,
	acpi_battery_charge_thresholds* thresholds)
{
	acpi_module_info* acpi;
	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi) != B_OK)
		return B_NOT_SUPPORTED;

	MutexLocker locker(sThresholdsLock);

	acpi_handle hotkey;
	int32 battery;
	status_t status = read_thresholds(acpi, batteryPath, &hotkey, &battery,
		thresholds);

	locker.Unlock();
	put_module(B_ACPI_MODULE_NAME);
	return status;
}


status_t
thinkpad_set_charge_thresholds(const char* batteryPath,
	const acpi_battery_charge_thresholds* thresholds)
{
	if (!charge_thresholds_valid(thresholds->start, thresholds->stop))
		return B_BAD_VALUE;

	acpi_module_info* acpi;
	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi) != B_OK)
		return B_NOT_SUPPORTED;

	MutexLocker locker(sThresholdsLock);

	acpi_handle hotkey;
	int32 battery;
	acpi_battery_charge_thresholds current;
	status_t status = read_thresholds(acpi, batteryPath, &hotkey, &battery,
		&current);
	if (status == B_OK) {
		const char* methods[2] = { "BCSS", "BCCS" };
		uint32 arguments[2] = {
			thinkpad_stop_argument(battery, thresholds->stop),
			thinkpad_start_argument(battery, thresholds->start)
		};
		int first = charge_set_start_first(current.start, thresholds->stop)
			? 1 : 0;
		status = set_threshold(acpi, hotkey, methods[first], arguments[first]);
		if (status == B_OK) {
			status = set_threshold(acpi, hotkey, methods[1 - first],
				arguments[1 - first]);
		}
	}

	locker.Unlock();
	put_module(B_ACPI_MODULE_NAME);
	return status;
}
