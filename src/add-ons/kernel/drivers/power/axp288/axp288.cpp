/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	Driver for the X-Powers AXP288 PMIC (ACPI INT33F4, "Dollar Cove"), as
	found on tablets with Intel Bay Trail and Cherry Trail SoCs.

	The PMIC sits on the I2C bus that the SoC P-Unit shares; pch_i2c takes the
	P-Unit semaphore for every transfer on that bus. The driver
	- handles the ACPI PMIC power operation region (0x8D), with which the
	  firmware switches the LDO regulators of devices such as the SD slot;
	- publishes the battery with the interface of acpi_battery
	  (power/acpi_battery/N), so that PowerStatus shows it;
	- publishes the power button as power/button/power_pmic, which the power
	  daemon watches. The button is polled, the PMIC interrupt is not used yet.

	Register map: X-Powers AXP288C datasheet V1.0, chapter 10.
*/


#include <ACPI.h>
#include <Drivers.h>
#include <Errors.h>
#include <KernelExport.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <condition_variable.h>
#include <device_manager.h>
#include <fs/select_sync_pool.h>
#include <i2c.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>

#include "device/power_managment.h"

extern "C" {
#	include "acpi.h"
}


#define AXP288_DRIVER_NAME		"drivers/power/axp288/driver_v1"
#define AXP288_DEVICE_NAME		"drivers/power/axp288/device_v1"

// Published like acpi_battery, so PowerStatus finds the battery.
#define BATTERY_PATH			"power/acpi_battery/%d"
#define BATTERY_PATH_ID			"acpi_battery/path_id"
#define BUTTON_PATH				"power/button/power_pmic"

#define TRACE_ALWAYS(x...)		dprintf("axp288 PMIC: " x)
//#define TRACE_AXP288
#ifdef TRACE_AXP288
#	define TRACE(x...)			dprintf("axp288 PMIC: " x)
#else
#	define TRACE(x...)
#endif

// Registers
#define AXP288_POWER_STATUS		0x00	// bit 5 VBUS present, 2 charging
#define AXP288_CHARGER_STATUS	0x01	// bit 6 charging, 5 battery, 4 valid
#define AXP288_POWER_REASON		0x02
#define AXP288_IC_TYPE			0x03
#define AXP288_POWER_CONTROL_1	0x10	// bucks
#define AXP288_POWER_CONTROL_2	0x12	// DLDO, ELDO
#define AXP288_POWER_CONTROL_3	0x13	// ALDO, FLDO
#define AXP288_IRQ_STATUS_5		0x4c	// bit 4 power key short press
#define AXP288_BATTERY_VOLTAGE	0x78	// 12 bit, 1.1 mV per step
#define AXP288_CHARGE_CURRENT	0x7a	// 12 bit, 1 mA per step
#define AXP288_DISCHARGE_CURRENT 0x7c	// 12 bit, 1 mA per step
#define AXP288_GPIO1_CONTROL	0x92	// bits 2:0 function
#define AXP288_CAPACITY			0xb9	// bit 7 valid, bits 6:0 percent
#define AXP288_MAX_CAPACITY		0xe0	// 15 bit, 1.456 mAh per step

#define AXP288_IC_TYPE_AXP288C	0x51

#define POWER_KEY_SHORT_PRESS	(1 << 4)
#define GPIO1_LDO_ON			0x3
#define GPIO1_LDO_OFF			0x4
#define GPIO1_FUNCTION_MASK		0x7

#define POLL_INTERVAL			250000
#define BATTERY_POLL_INTERVAL	40		// in POLL_INTERVALs
#define CRITICAL_CAPACITY		5		// percent

// ACPI PMIC power operation region of Intel SoCs
#define ACPI_ADR_SPACE_PMIC_POWER	0x8d


struct axp288_device {
	device_node*			node;
	i2c_device_interface*	i2c;
	i2c_device				i2cCookie;
	acpi_handle				acpiHandle;
	bool					regionHandlerInstalled;

	mutex					lock;
	thread_id				pollThread;
	volatile bool			stopPolling;

	// power button
	uint8					buttonPressed;
	select_sync_pool*		selectPool;

	// battery
	uint8					lastCapacity;
	uint8					lastChargerStatus;
	ConditionVariable		batteryCondition;
};

struct axp288_handle {
	axp288_device*			device;
	bool					button;
	int32					stopWatching;
};


static device_manager_info* sDeviceManager;
static acpi_module_info* sACPI;


//	#pragma mark - register access


static status_t
read_registers(axp288_device* device, uint8 reg, uint8* buffer, size_t length)
{
	status_t status = device->i2c->acquire_bus(device->i2cCookie);
	if (status != B_OK)
		return status;
	status = device->i2c->exec_command(device->i2cCookie, I2C_OP_READ_STOP,
		&reg, 1, buffer, length);
	device->i2c->release_bus(device->i2cCookie);
	return status;
}


static status_t
write_register(axp288_device* device, uint8 reg, uint8 value)
{
	uint8 command[2] = { reg, value };
	status_t status = device->i2c->acquire_bus(device->i2cCookie);
	if (status != B_OK)
		return status;
	status = device->i2c->exec_command(device->i2cCookie, I2C_OP_WRITE_STOP,
		command, sizeof(command), NULL, 0);
	device->i2c->release_bus(device->i2cCookie);
	return status;
}


static status_t
read_12bit(axp288_device* device, uint8 reg, uint32* _value)
{
	uint8 data[2];
	status_t status = read_registers(device, reg, data, sizeof(data));
	if (status == B_OK)
		*_value = ((uint32)data[0] << 4) | (data[1] & 0x0f);
	return status;
}


//	#pragma mark - ACPI PMIC power operation region


/*!	The fields of the region are 32 bits each, in this order (see the
	PMOP region of the PMIC device in the DSDT). Each switches one regulator.
*/
struct power_field {
	const char*	name;
	uint8		reg;
	uint8		bit;
	bool		writable;
};

static const power_field kPowerFields[] = {
	{ "ALDO1", AXP288_POWER_CONTROL_3, 5, true },
	{ "ALDO2", AXP288_POWER_CONTROL_3, 6, true },
	{ "ALDO3", AXP288_POWER_CONTROL_3, 7, true },
	{ "DLDO1", AXP288_POWER_CONTROL_2, 3, true },
	{ "DLDO2", AXP288_POWER_CONTROL_2, 4, true },
	{ "DLDO3", AXP288_POWER_CONTROL_2, 5, true },
	{ "DLDO4", AXP288_POWER_CONTROL_2, 6, true },
	{ "ELDO1", AXP288_POWER_CONTROL_2, 0, true },
	{ "ELDO2", AXP288_POWER_CONTROL_2, 1, true },
	{ "ELDO3", AXP288_POWER_CONTROL_2, 2, true },
	{ "FLDO1", AXP288_POWER_CONTROL_3, 2, true },
	{ "FLDO2", AXP288_POWER_CONTROL_3, 3, true },
	{ "FLDO3", AXP288_POWER_CONTROL_3, 4, true },
	// The bucks feed the SoC and the memory: only reading is allowed.
	{ "BUCK1", AXP288_POWER_CONTROL_1, 3, false },
	{ "BUCK2", AXP288_POWER_CONTROL_1, 6, false },
	{ "BUCK3", AXP288_POWER_CONTROL_1, 5, false },
	{ "BUCK4", AXP288_POWER_CONTROL_1, 4, false },
	{ "BUCK5", AXP288_POWER_CONTROL_1, 1, false },
	{ "BUCK6", AXP288_POWER_CONTROL_1, 0, false },
	// GPIO1 as low noise LDO, handled separately
	{ "GPIO1", AXP288_GPIO1_CONTROL, 0, true },
};


static acpi_status
power_region_handler(uint32 function, acpi_physical_address address,
	uint32 bitWidth, int* _value, void* handlerContext, void* regionContext)
{
	axp288_device* device = (axp288_device*)handlerContext;
	uint64* value = (uint64*)_value;
		// ACPICA passes a pointer to a 64-bit value

	if ((address % 4) != 0 || address / 4 >= B_COUNT_OF(kPowerFields))
		return AE_BAD_PARAMETER;

	const power_field& field = kPowerFields[address / 4];
	bool write = (function & ACPI_IO_MASK) == ACPI_WRITE;
	bool gpio1 = field.reg == AXP288_GPIO1_CONTROL;

	MutexLocker locker(device->lock);

	uint8 data;
	if (read_registers(device, field.reg, &data, 1) != B_OK)
		return AE_ERROR;

	if (!write) {
		if (gpio1)
			*value = (data & GPIO1_FUNCTION_MASK) == GPIO1_LDO_ON ? 1 : 0;
		else
			*value = (data >> field.bit) & 1;
		TRACE("read %s: %" B_PRIu64 "\n", field.name, *value);
		return AE_OK;
	}

	if (!field.writable) {
		TRACE_ALWAYS("refusing to switch %s from ACPI\n", field.name);
		return AE_BAD_PARAMETER;
	}

	if (gpio1) {
		data = (data & ~GPIO1_FUNCTION_MASK)
			| (*value != 0 ? GPIO1_LDO_ON : GPIO1_LDO_OFF);
	} else if (*value != 0)
		data |= 1 << field.bit;
	else
		data &= ~(1 << field.bit);

	TRACE_ALWAYS("%s %s\n", field.name, *value != 0 ? "on" : "off");
	return write_register(device, field.reg, data) == B_OK ? AE_OK : AE_ERROR;
}


static acpi_status
power_region_setup(acpi_handle region, uint32 function, void* handlerContext,
	void** regionContext)
{
	if (function == ACPI_REGION_DEACTIVATE)
		*regionContext = NULL;
	else
		*regionContext = handlerContext;

	return AE_OK;
}


//	#pragma mark - battery


static status_t
read_battery_status(axp288_device* device, acpi_battery_info* info)
{
	uint8 status[2];
	uint8 capacity;
	uint32 voltage;
	status_t result = read_registers(device, AXP288_POWER_STATUS, status,
		sizeof(status));
	if (result == B_OK)
		result = read_registers(device, AXP288_CAPACITY, &capacity, 1);
	if (result == B_OK)
		result = read_12bit(device, AXP288_BATTERY_VOLTAGE, &voltage);
	if (result != B_OK)
		return result;

	bool charging = (status[1] & (1 << 6)) != 0;
	bool external = (status[0] & (1 << 5)) != 0;
	uint32 percent = (capacity & 0x80) != 0 ? capacity & 0x7f : 0;

	uint32 current = 0;
	read_12bit(device, charging ? AXP288_CHARGE_CURRENT
		: AXP288_DISCHARGE_CURRENT, &current);

	info->state = charging ? BATTERY_CHARGING
		: external ? BATTERY_NOT_CHARGING : BATTERY_DISCHARGING;
	if (!charging && percent <= CRITICAL_CAPACITY)
		info->state |= BATTERY_CRITICAL_STATE;
	info->current_rate = current;
	info->voltage = voltage * 11 / 10;

	// The capacity is reported in the unit of the full charge, see below.
	uint8 maximum[2];
	uint32 fullCharge = 100;
	if (read_registers(device, AXP288_MAX_CAPACITY, maximum, sizeof(maximum))
			== B_OK && (maximum[0] & 0x80) != 0) {
		fullCharge = ((((uint32)maximum[0] & 0x7f) << 8) | maximum[1])
			* 1456 / 1000;
	}
	info->capacity = fullCharge * percent / 100;
	return B_OK;
}


static status_t
read_battery_info(axp288_device* device, acpi_extended_battery_info* info)
{
	memset(info, 0, sizeof(acpi_extended_battery_info));

	// Without a valid fuel gauge maximum, the capacity is in percent.
	uint32 fullCharge = 100;
	uint8 maximum[2];
	if (read_registers(device, AXP288_MAX_CAPACITY, maximum, sizeof(maximum))
			== B_OK && (maximum[0] & 0x80) != 0) {
		fullCharge = ((((uint32)maximum[0] & 0x7f) << 8) | maximum[1])
			* 1456 / 1000;
	}

	info->power_unit = ACPI_BATTERY_UNIT_MA;
	info->design_capacity = fullCharge;
	info->last_full_charge = fullCharge;
	info->technology = 1;	// rechargeable
	info->design_capacity_warning = fullCharge * 10 / 100;
	info->design_capacity_low = fullCharge * CRITICAL_CAPACITY / 100;
	info->capacity_granularity_1 = 1;
	info->capacity_granularity_2 = 1;
	strlcpy(info->model_number, "AXP288 fuel gauge", BATTERY_MAX_STRING_LENGTH);
	strlcpy(info->type, "LION", BATTERY_MAX_STRING_LENGTH);
	strlcpy(info->oem_info, "X-Powers", BATTERY_MAX_STRING_LENGTH);
	info->revision = ACPI_BATTERY_REVISION_BIF;
	return B_OK;
}


//	#pragma mark - polling


static status_t
poll_thread(void* data)
{
	axp288_device* device = (axp288_device*)data;
	int32 round = 0;

	while (!device->stopPolling) {
		snooze(POLL_INTERVAL);

		uint8 irqStatus;
		if (read_registers(device, AXP288_IRQ_STATUS_5, &irqStatus, 1) == B_OK
			&& (irqStatus & POWER_KEY_SHORT_PRESS) != 0) {
			write_register(device, AXP288_IRQ_STATUS_5,
				POWER_KEY_SHORT_PRESS);
			TRACE_ALWAYS("power key pressed\n");

			MutexLocker locker(device->lock);
			device->buttonPressed = 1;
			if (device->selectPool != NULL)
				notify_select_event_pool(device->selectPool, B_SELECT_READ);
		}

		if (++round < BATTERY_POLL_INTERVAL)
			continue;
		round = 0;

		uint8 capacity;
		uint8 chargerStatus;
		if (read_registers(device, AXP288_CAPACITY, &capacity, 1) != B_OK
			|| read_registers(device, AXP288_CHARGER_STATUS, &chargerStatus,
				1) != B_OK) {
			continue;
		}
		if (capacity != device->lastCapacity
			|| ((chargerStatus ^ device->lastChargerStatus) & (1 << 6)) != 0) {
			device->lastCapacity = capacity;
			device->lastChargerStatus = chargerStatus;
			device->batteryCondition.NotifyAll();
		}
	}

	return B_OK;
}


static void
log_state(axp288_device* device)
{
	uint8 status[4];
	status_t result = read_registers(device, AXP288_POWER_STATUS, status,
		sizeof(status));
	if (result != B_OK) {
		TRACE_ALWAYS("cannot read the status registers: %s\n",
			strerror(result));
		return;
	}

	TRACE_ALWAYS("IC type %#x%s, power %#x, charger %#x, power up/down "
		"reason %#x\n", status[3],
		status[3] == AXP288_IC_TYPE_AXP288C ? " (AXP288C)" : "",
		status[0], status[1], status[2]);

	device->lastChargerStatus = status[1];
	read_registers(device, AXP288_CAPACITY, &device->lastCapacity, 1);
}


//	#pragma mark - device module


static status_t
axp288_init_device(void* driverCookie, void** _cookie)
{
	*_cookie = driverCookie;
	return B_OK;
}


static void
axp288_uninit_device(void* cookie)
{
}


static status_t
axp288_open(void* cookie, const char* path, int openMode, void** _cookie)
{
	axp288_handle* handle = (axp288_handle*)calloc(1, sizeof(axp288_handle));
	if (handle == NULL)
		return B_NO_MEMORY;

	handle->device = (axp288_device*)cookie;
	handle->button = strstr(path, "button") != NULL;
	*_cookie = handle;
	return B_OK;
}


static status_t
axp288_close(void* cookie)
{
	return B_OK;
}


static status_t
axp288_free(void* cookie)
{
	free(cookie);
	return B_OK;
}


static status_t
axp288_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	axp288_handle* handle = (axp288_handle*)cookie;
	axp288_device* device = handle->device;

	if (handle->button) {
		if (*_length < 1)
			return B_BAD_VALUE;

		MutexLocker locker(device->lock);
		uint8 pressed = device->buttonPressed;
		device->buttonPressed = 0;
		locker.Unlock();

		if (user_memcpy(buffer, &pressed, 1) != B_OK)
			return B_BAD_ADDRESS;
		*_length = 1;
		return B_OK;
	}

	if (position != 0) {
		*_length = 0;
		return B_OK;
	}

	acpi_battery_info info;
	status_t status = read_battery_status(device, &info);
	if (status != B_OK)
		return status;

	char text[160];
	int length = snprintf(text, sizeof(text), "Battery Status:\n"
		" State %" B_PRIu32 ", Current Rate %" B_PRIu32 ", Capacity %"
		B_PRIu32 ", Voltage %" B_PRIu32 "\n", info.state, info.current_rate,
		info.capacity, info.voltage);
	if ((size_t)length > *_length)
		length = *_length;
	if (user_memcpy(buffer, text, length) != B_OK)
		return B_BAD_ADDRESS;
	*_length = length;
	return B_OK;
}


static status_t
axp288_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	return B_NOT_ALLOWED;
}


static status_t
axp288_control(void* cookie, uint32 op, void* arg, size_t length)
{
	axp288_handle* handle = (axp288_handle*)cookie;
	axp288_device* device = handle->device;

	if (handle->button)
		return B_DEV_INVALID_IOCTL;

	switch (op) {
		case IDENTIFY_DEVICE:
		{
			if (length < sizeof(uint32))
				return B_BAD_VALUE;
			uint32 magic = kMagicACPIBatteryID;
			if (!IS_USER_ADDRESS(arg)
				|| user_memcpy(arg, &magic, sizeof(magic)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			return B_OK;
		}

		case GET_BATTERY_INFO:
		{
			if (length < sizeof(acpi_battery_info))
				return B_BAD_VALUE;
			acpi_battery_info info;
			status_t status = read_battery_status(device, &info);
			if (status != B_OK)
				return status;
			if (!IS_USER_ADDRESS(arg)
				|| user_memcpy(arg, &info, sizeof(info)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			return B_OK;
		}

		case GET_EXTENDED_BATTERY_INFO:
		{
			if (length < sizeof(acpi_extended_battery_info))
				return B_BAD_VALUE;
			acpi_extended_battery_info info;
			status_t status = read_battery_info(device, &info);
			if (status != B_OK)
				return status;
			if (!IS_USER_ADDRESS(arg)
				|| user_memcpy(arg, &info, sizeof(info)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			return B_OK;
		}

		case WATCH_BATTERY:
			device->batteryCondition.Wait();
			if (atomic_get(&handle->stopWatching) != 0) {
				atomic_set(&handle->stopWatching, 0);
				return B_ERROR;
			}
			return B_OK;

		case STOP_WATCHING_BATTERY:
			atomic_set(&handle->stopWatching, 1);
			device->batteryCondition.NotifyAll();
			return B_OK;
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
axp288_select(void* cookie, uint8 event, selectsync* sync)
{
	axp288_handle* handle = (axp288_handle*)cookie;
	axp288_device* device = handle->device;
	if (!handle->button || event != B_SELECT_READ)
		return B_BAD_VALUE;

	MutexLocker locker(device->lock);
	status_t status = add_select_sync_pool_entry(&device->selectPool, sync,
		event);
	if (status != B_OK)
		return status;
	if (device->buttonPressed != 0)
		notify_select_event(sync, event);
	return B_OK;
}


static status_t
axp288_deselect(void* cookie, uint8 event, selectsync* sync)
{
	axp288_handle* handle = (axp288_handle*)cookie;
	axp288_device* device = handle->device;
	if (!handle->button || event != B_SELECT_READ)
		return B_BAD_VALUE;

	MutexLocker locker(device->lock);
	return remove_select_sync_pool_entry(&device->selectPool, sync, event);
}


//	#pragma mark - driver module


static float
axp288_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "i2c") != 0) {
		return 0.0;
	}

	const char* name;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &name,
			false) == B_OK && strcmp(name, "INT33F4") == 0) {
		return 0.8;
	}

	return 0.0;
}


static status_t
axp288_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "X-Powers AXP288 PMIC" }},
		// The ACPI interpreter may use the power region at any time.
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, AXP288_DRIVER_NAME, attrs,
		NULL, NULL);
}


static status_t
axp288_init_driver(device_node* node, void** _driverCookie)
{
	axp288_device* device = (axp288_device*)calloc(1, sizeof(axp288_device));
	if (device == NULL)
		return B_NO_MEMORY;

	device->node = node;
	device->pollThread = -1;
	mutex_init(&device->lock, "axp288");
	device->batteryCondition.Init(device, "axp288 battery");

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&device->i2c,
		(void**)&device->i2cCookie);
	uint64 handle = 0;
	if (sDeviceManager->get_attr_uint64(parent, ACPI_DEVICE_HANDLE_ITEM,
			&handle, false) == B_OK) {
		device->acpiHandle = (acpi_handle)(addr_t)handle;
	}
	sDeviceManager->put_node(parent);

	log_state(device);

	// Installing the handler runs _REG, which tells the AML that the PMIC
	// regulators can be switched.
	if (device->acpiHandle != NULL
		&& sACPI->install_address_space_handler(device->acpiHandle,
			ACPI_ADR_SPACE_PMIC_POWER, power_region_handler,
			power_region_setup, device) == B_OK) {
		device->regionHandlerInstalled = true;
	} else
		TRACE_ALWAYS("installing the PMIC power region handler failed\n");

	device->pollThread = spawn_kernel_thread(poll_thread, "axp288 poll",
		B_LOW_PRIORITY, device);
	if (device->pollThread >= 0)
		resume_thread(device->pollThread);

	*_driverCookie = device;
	return B_OK;
}


static void
axp288_uninit_driver(void* driverCookie)
{
	axp288_device* device = (axp288_device*)driverCookie;

	if (device->pollThread >= 0) {
		device->stopPolling = true;
		status_t result;
		wait_for_thread(device->pollThread, &result);
	}
	if (device->regionHandlerInstalled) {
		sACPI->remove_address_space_handler(device->acpiHandle,
			ACPI_ADR_SPACE_PMIC_POWER, power_region_handler);
	}

	mutex_destroy(&device->lock);
	free(device);
}


static status_t
axp288_register_child_devices(void* driverCookie)
{
	axp288_device* device = (axp288_device*)driverCookie;

	int32 pathID = sDeviceManager->create_id(BATTERY_PATH_ID);
	if (pathID >= 0) {
		char path[B_DEV_NAME_LENGTH];
		snprintf(path, sizeof(path), BATTERY_PATH, (int)pathID);
		sDeviceManager->publish_device(device->node, path,
			AXP288_DEVICE_NAME);
	}

	return sDeviceManager->publish_device(device->node, BUTTON_PATH,
		AXP288_DEVICE_NAME);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ B_ACPI_MODULE_NAME, (module_info**)&sACPI },
	{}
};


static driver_module_info sAxp288DriverModule = {
	{
		AXP288_DRIVER_NAME,
		0,
		NULL
	},

	axp288_supports_device,
	axp288_register_device,
	axp288_init_driver,
	axp288_uninit_driver,
	axp288_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


static device_module_info sAxp288DeviceModule = {
	{
		AXP288_DEVICE_NAME,
		0,
		NULL
	},

	axp288_init_device,
	axp288_uninit_device,
	NULL,	// removed

	axp288_open,
	axp288_close,
	axp288_free,
	axp288_read,
	axp288_write,
	NULL,	// io
	axp288_control,

	axp288_select,
	axp288_deselect,
};


module_info* modules[] = {
	(module_info*)&sAxp288DriverModule,
	(module_info*)&sAxp288DeviceModule,
	NULL
};
