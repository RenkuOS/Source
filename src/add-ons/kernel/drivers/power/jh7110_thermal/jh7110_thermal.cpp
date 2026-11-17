/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * Temperature sensor of the StarFive JH7110 ("starfive,jh7110-temp"),
 * published as /dev/power/jh7110_thermal/0 with the text and ioctl
 * interface of the pch_thermal driver.
 *
 * The sensor has a single register: reset (bit 0), power down (bit 1), run
 * (bit 2) and the 12 bit conversion result in bits 27-16. The power-up
 * sequence and the conversion formula (from the sensor's datasheet values)
 * were checked against Linux's drivers/hwmon/sfctemp.c.
 */


#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>
#include <fdt_clock_reset.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>


#define THERMAL_DRIVER_MODULE_NAME	"drivers/power/jh7110_thermal/driver_v1"
#define THERMAL_DEVICE_MODULE_NAME	"drivers/power/jh7110_thermal/device_v1"
#define THERMAL_PATHID_GENERATOR	"jh7110_thermal/path_id"
#define THERMAL_BASENAME			"power/jh7110_thermal/%d"

#define INFO(x...)	dprintf("jh7110_thermal: " x)
#define ERROR(x...)	dprintf("jh7110_thermal: " x)


// the register
#define SFCTEMP_RSTN			(1u << 0)
#define SFCTEMP_PD				(1u << 1)
#define SFCTEMP_RUN				(1u << 2)
#define SFCTEMP_DOUT_SHIFT		16
#define SFCTEMP_DOUT_MASK		0xfffu

// millidegrees Celsius = dout * Y / Z - K
#define SFCTEMP_Y1000			237500
#define SFCTEMP_Z				4094
#define SFCTEMP_K1000			81100

// the junction temperature limit of the JH7110 (TRM: 125 C)
#define CRITICAL_TEMP_DECI		1250


// same layout and opcode as pch_thermal and acpi_thermal
enum {
	drvOpGetThermalType = B_DEVICE_OP_CODES_END + 10001,
};

struct jh7110_thermal_type {
	uint32 critical_temp;	// in 1/10 C
	uint32 current_temp;
	uint32 hot_temp;
};


static const char* const kCompatible = "starfive,jh7110-temp";


struct thermal_info {
	device_node*		node;
	area_id				area;
	volatile uint32*	reg;
	mutex				lock;
};


static device_manager_info* sDeviceManager;


static fdt_device_module_info*
get_fdt(device_node* node, fdt_device** _dev)
{
	fdt_device_module_info* module;
	if (sDeviceManager->get_driver(node, (driver_module_info**)&module,
			(void**)_dev) != B_OK) {
		return NULL;
	}
	return module;
}


/*!	Temperature in tenths of a degree Celsius, may be negative.
*/
static int32
thermal_read_deci(thermal_info* info)
{
	MutexLocker locker(info->lock);
	uint32 dout = (*info->reg >> SFCTEMP_DOUT_SHIFT) & SFCTEMP_DOUT_MASK;
	int64 milli = (int64)dout * SFCTEMP_Y1000 / SFCTEMP_Z - SFCTEMP_K1000;
	return (int32)(milli / 100);
}


// #pragma mark - device


static status_t
thermal_open(void* _info, const char* path, int openMode, void** _cookie)
{
	*_cookie = _info;
	return B_OK;
}


static status_t
thermal_close(void* cookie)
{
	return B_OK;
}


static status_t
thermal_free(void* cookie)
{
	return B_OK;
}


static status_t
thermal_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	thermal_info* info = (thermal_info*)cookie;

	switch (op) {
		case drvOpGetThermalType:
		{
			int32 current = thermal_read_deci(info);
			jh7110_thermal_type type;
			type.critical_temp = CRITICAL_TEMP_DECI;
			type.current_temp = current > 0 ? current : 0;
			type.hot_temp = 0;
			if (!IS_USER_ADDRESS(buffer))
				memcpy(buffer, &type, sizeof(type));
			else if (user_memcpy(buffer, &type, sizeof(type)) != B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case B_GET_DEVICE_NAME:
		{
			static const char kName[] = "JH7110 SoC temperature";
			if (!IS_USER_ADDRESS(buffer))
				return strlcpy((char*)buffer, kName, length) < length
					? B_OK : B_NAME_TOO_LONG;
			return user_strlcpy((char*)buffer, kName, length) >= 0
				? B_OK : B_BAD_ADDRESS;
		}
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
thermal_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	thermal_info* info = (thermal_info*)cookie;
	if (position != 0) {
		*_length = 0;
		return B_OK;
	}

	int32 current = thermal_read_deci(info);
	int32 whole = current / 10;
	int32 tenths = current % 10;
	if (tenths < 0)
		tenths = -tenths;

	char text[256];
	int length = snprintf(text, sizeof(text),
		"  Critical Temperature: %d.%d C\n"
		"  Current Temperature: %s%" B_PRId32 ".%" B_PRId32 " C\n",
		CRITICAL_TEMP_DECI / 10, CRITICAL_TEMP_DECI % 10,
		current < 0 && whole == 0 ? "-" : "", whole, tenths);

	size_t toCopy = min_c((size_t)length, *_length);
	if (user_memcpy(buffer, text, toCopy) != B_OK)
		return B_BAD_ADDRESS;
	*_length = toCopy;
	return B_OK;
}


static status_t
thermal_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
thermal_init_device(void* _cookie, void** _deviceCookie)
{
	*_deviceCookie = _cookie;
	return B_OK;
}


static void
thermal_uninit_device(void* cookie)
{
}


// #pragma mark - driver


static float
thermal_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK || strcmp(compatible, kCompatible) != 0) {
		return 0.0f;
	}

	fdt_device* dev;
	fdt_device_module_info* module = get_fdt(parent, &dev);
	if (module == NULL)
		return 0.0f;
	int length;
	const char* status = (const char*)module->get_prop(dev, "status", &length);
	if (status != NULL && strcmp(status, "okay") != 0
		&& strcmp(status, "ok") != 0) {
		return 0.0f;
	}
	return 0.8f;
}


static status_t
thermal_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "JH7110 temperature sensor" }},
		{}
	};
	return sDeviceManager->register_node(parent, THERMAL_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


/*!	Clocks on and resets released through the clock/reset driver, as Linux
	does: resets asserted, clocks enabled, resets released.
*/
static status_t
thermal_enable(device_node* fdtNode)
{
	fdt_clock_reset_module_info* clocks;
	if (get_module(JH7110_CLOCK_RESET_MODULE_NAME, (module_info**)&clocks)
			!= B_OK) {
		ERROR("no clock/reset driver\n");
		return B_NOT_SUPPORTED;
	}

	static const char* const kNames[] = { "sense", "bus" };
	fdt_clock clockHandles[2];
	fdt_reset resetHandles[2];
	status_t status = B_OK;
	for (int i = 0; i < 2 && status == B_OK; i++) {
		status = clocks->get_clock(fdtNode, kNames[i], 0, &clockHandles[i]);
		if (status == B_OK)
			status = clocks->get_reset(fdtNode, kNames[i], 0, &resetHandles[i]);
		if (status != B_OK)
			ERROR("%s clock or reset not found\n", kNames[i]);
	}

	if (status == B_OK) {
		for (int i = 0; i < 2; i++)
			clocks->set_reset(resetHandles[i], true);
		for (int i = 0; i < 2 && status == B_OK; i++)
			status = clocks->enable_clock(clockHandles[i], true);
		if (status == B_OK) {
			for (int i = 0; i < 2; i++)
				clocks->set_reset(resetHandles[i], false);
			INFO("sense clock %" B_PRId64 " Hz\n",
				clocks->get_clock_rate(clockHandles[0]));
		} else
			ERROR("could not enable the clocks\n");
	}

	put_module(JH7110_CLOCK_RESET_MODULE_NAME);
	return status;
}


static status_t
thermal_init_driver(device_node* node, void** _cookie)
{
	thermal_info* info = new(std::nothrow) thermal_info;
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;
	info->area = -1;
	mutex_init(&info->lock, "jh7110 thermal");

	device_node* fdtNode = sDeviceManager->get_parent_node(node);
	fdt_device* dev;
	fdt_device_module_info* module = get_fdt(fdtNode, &dev);
	uint64 regs, regsLength;
	status_t status = B_BAD_DATA;
	if (module != NULL && module->get_reg(dev, 0, &regs, &regsLength))
		status = thermal_enable(fdtNode);
	sDeviceManager->put_node(fdtNode);
	if (status != B_OK) {
		mutex_destroy(&info->lock);
		delete info;
		return status;
	}

	info->area = map_physical_memory("jh7110 thermal", regs, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&info->reg);
	if (info->area < 0) {
		status = info->area;
		mutex_destroy(&info->lock);
		delete info;
		return status;
	}

	// power up, release the sensor reset, then convert continuously
	*info->reg = SFCTEMP_PD;
	spin(1);
	*info->reg = 0;
	spin(100);
	*info->reg = SFCTEMP_RSTN;
	spin(1);
	*info->reg = SFCTEMP_RSTN | SFCTEMP_RUN;
	snooze(10000);

	INFO("at %#" B_PRIx64 ", %" B_PRId32 " deci-C\n", regs,
		thermal_read_deci(info));

	*_cookie = info;
	return B_OK;
}


static void
thermal_uninit_driver(void* _cookie)
{
	thermal_info* info = (thermal_info*)_cookie;
	*info->reg = SFCTEMP_PD;
	delete_area(info->area);
	mutex_destroy(&info->lock);
	delete info;
}


static status_t
thermal_register_child_devices(void* _cookie)
{
	thermal_info* info = (thermal_info*)_cookie;

	int pathId = sDeviceManager->create_id(THERMAL_PATHID_GENERATOR);
	if (pathId < 0)
		return pathId;

	char name[B_DEV_NAME_LENGTH];
	snprintf(name, sizeof(name), THERMAL_BASENAME, pathId);
	status_t status = sDeviceManager->publish_device(info->node, name,
		THERMAL_DEVICE_MODULE_NAME);
	if (status != B_OK)
		sDeviceManager->free_id(THERMAL_PATHID_GENERATOR, pathId);
	return status;
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


static struct device_module_info sThermalDevice = {
	{
		THERMAL_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	thermal_init_device,
	thermal_uninit_device,
	NULL,	// remove

	thermal_open,
	thermal_close,
	thermal_free,
	thermal_read,
	thermal_write,
	NULL,	// io
	thermal_control,

	NULL,	// select
	NULL,	// deselect
};


static struct driver_module_info sThermalDriver = {
	{
		THERMAL_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	thermal_supports_device,
	thermal_register_device,
	thermal_init_driver,
	thermal_uninit_driver,
	thermal_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sThermalDriver,
	(module_info*)&sThermalDevice,
	NULL
};
