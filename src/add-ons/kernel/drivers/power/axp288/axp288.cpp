/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	Driver for the X-Powers AXP288 PMIC (ACPI INT33F4, "Dollar Cove"), as
	found on tablets with Intel Bay Trail and Cherry Trail SoCs.

	First step: identify the chip and log the power and battery state, read
	only. The PMIC sits on the I2C bus that the SoC P-Unit shares; pch_i2c
	takes the P-Unit semaphore for every transfer on that bus.

	Register map: X-Powers AXP288C datasheet V1.0, chapter 10.
*/


#include <ACPI.h>
#include <Drivers.h>
#include <Errors.h>
#include <KernelExport.h>

#include <stdlib.h>
#include <string.h>

#include <device_manager.h>
#include <i2c.h>


#define AXP288_DRIVER_NAME		"drivers/power/axp288/driver_v1"

#define TRACE_ALWAYS(x...)		dprintf("axp288 PMIC: " x)

// Registers
#define AXP288_POWER_STATUS		0x00
#define AXP288_CHARGER_STATUS	0x01
#define AXP288_POWER_REASON		0x02
#define AXP288_IC_TYPE			0x03
#define AXP288_BATTERY_VOLTAGE	0x78	// 12 bit, 78H bits 11:4, 79H bits 3:0
#define AXP288_CAPACITY			0xb9	// bit 7 valid, bits 6:0 percent

#define AXP288_IC_TYPE_AXP288C	0x51


struct axp288_device {
	device_node*			node;
	i2c_device_interface*	i2c;
	i2c_device				i2cCookie;
};


static device_manager_info* sDeviceManager;


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
	TRACE_ALWAYS("VBUS %s, %s, battery %s\n",
		(status[0] & (1 << 5)) != 0 ? "present" : "absent",
		(status[0] & (1 << 2)) != 0 ? "charging" : "discharging",
		(status[1] & (1 << 4)) == 0 ? "not detected yet"
			: (status[1] & (1 << 5)) != 0 ? "present" : "absent");

	uint8 voltage[2];
	if (read_registers(device, AXP288_BATTERY_VOLTAGE, voltage,
			sizeof(voltage)) == B_OK) {
		// 1.1 mV per step
		uint32 raw = ((uint32)voltage[0] << 4) | (voltage[1] & 0x0f);
		TRACE_ALWAYS("battery voltage %" B_PRIu32 " mV\n", raw * 11 / 10);
	}

	uint8 capacity;
	if (read_registers(device, AXP288_CAPACITY, &capacity, 1) == B_OK) {
		if ((capacity & 0x80) != 0)
			TRACE_ALWAYS("battery capacity %d %%\n", capacity & 0x7f);
		else
			TRACE_ALWAYS("battery capacity not valid yet\n");
	}
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

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&device->i2c,
		(void**)&device->i2cCookie);
	sDeviceManager->put_node(parent);

	log_state(device);

	*_driverCookie = device;
	return B_OK;
}


static void
axp288_uninit_driver(void* driverCookie)
{
	free(driverCookie);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
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
	NULL,	// register child devices
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sAxp288DriverModule,
	NULL
};
