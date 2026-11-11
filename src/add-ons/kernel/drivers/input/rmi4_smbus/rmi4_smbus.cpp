/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 *
 * Synaptics RMI4 touchpad on SMBus (InterTouch): driver and device modules.
 *
 * The i801 SMBus driver adds a node for the touchpad address. This driver
 * only uses it when "intertouch true" is set in
 * ~/config/settings/kernel/drivers/rmi4_smbus; the PS/2 driver then hands
 * the pad over, and the touchpad (and the TrackPoint behind it) are
 * published as input/touchpad/rmi4/synaptics_0 and
 * input/mouse/rmi4/trackpoint_0.
 */


#include "rmi4.h"

#include <ACPI.h>
#include <kernel.h>

#include <new>
#include <string.h>


struct rmi4_cookie {
	rmi4_device*	device;
	bool			touchpad;
};


device_manager_info* gDeviceManager;


//	#pragma mark - device module


static status_t
rmi4_init_device(void* driverCookie, void** _cookie)
{
	*_cookie = driverCookie;
	return B_OK;
}


static void
rmi4_uninit_device(void* cookie)
{
}


static status_t
rmi4_open(void* initCookie, const char* path, int flags, void** _cookie)
{
	rmi4_cookie* cookie = new(std::nothrow) rmi4_cookie;
	if (cookie == NULL)
		return B_NO_MEMORY;

	cookie->device = (rmi4_device*)initCookie;
	cookie->touchpad = strcmp(path, RMI4_TOUCHPAD_PATH) == 0;
	*_cookie = cookie;
	return B_OK;
}


static status_t
rmi4_close(void* cookie)
{
	return B_OK;
}


static status_t
rmi4_free(void* cookie)
{
	delete (rmi4_cookie*)cookie;
	return B_OK;
}


static status_t
rmi4_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_SUPPORTED;
}


static status_t
rmi4_write(void* cookie, off_t position, const void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_SUPPORTED;
}


static status_t
rmi4_control(void* _cookie, uint32 op, void* buffer, size_t length)
{
	rmi4_cookie* cookie = (rmi4_cookie*)_cookie;
	rmi4_device* device = cookie->device;

	switch (op) {
		case MS_IS_TOUCHPAD:
			if (!cookie->touchpad)
				return B_BAD_VALUE;
			if (buffer == NULL)
				return B_OK;
			if (!IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return user_memcpy(buffer, &device->specs, sizeof(touchpad_specs));

		case MS_READ_TOUCHPAD:
		{
			if (!cookie->touchpad)
				return B_BAD_VALUE;
			if (length < sizeof(touchpad_read))
				return B_BUFFER_OVERFLOW;

			touchpad_read read;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&read.timeout,
					&((touchpad_read*)buffer)->timeout,
					sizeof(bigtime_t)) != B_OK) {
				return B_BAD_ADDRESS;
			}

			status_t status = rmi4_read_touchpad(device, &read.u.touchpad,
				read.timeout);
			if (status != B_OK)
				return status;

			read.event = MS_READ_TOUCHPAD;
			if (user_memcpy(buffer, &read, sizeof(read)) != B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case MS_READ:
		{
			if (cookie->touchpad)
				return B_BAD_VALUE;
			if (!IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;

			mouse_movement movement;
			status_t status = rmi4_read_mouse(device, &movement);
			if (status != B_OK)
				return status;
			return user_memcpy(buffer, &movement, sizeof(movement));
		}

		case MS_NUM_EVENTS:
		{
			int32 count;
			get_sem_count(cookie->touchpad ? device->touchpadQueue.sem
				: device->mouseQueue.sem, &count);
			return count;
		}

		case MS_SET_CLICKSPEED:
			if (!IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return user_memcpy(&device->clickSpeed, buffer, sizeof(bigtime_t));
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver module


static float
rmi4_supports_device(device_node* parent)
{
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "i2c") != 0) {
		return 0.0f;
	}

	const char* hid;
	if (gDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK || strcmp(hid, RMI4_SMBUS_HID) != 0) {
		return 0.0f;
	}
	return 0.6f;
}


static status_t
rmi4_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Synaptics RMI4 touchpad (SMBus)" }},
		{ NULL }
	};

	return gDeviceManager->register_node(node, RMI4_SMBUS_DRIVER_NAME, attrs,
		NULL, NULL);
}


static status_t
rmi4_init_driver(device_node* node, void** _cookie)
{
	rmi4_device* device = new(std::nothrow) rmi4_device;
	if (device == NULL)
		return B_NO_MEMORY;
	memset((void*)device, 0, sizeof(*device));
	device->node = node;

	device_node* parent = gDeviceManager->get_parent_node(node);
	gDeviceManager->get_driver(parent, (driver_module_info**)&device->i2c,
		(void**)&device->i2cDevice);
	gDeviceManager->put_node(parent);

	status_t status = rmi4_device_init(device);
	if (status != B_OK) {
		delete device;
		return status;
	}

	*_cookie = device;
	return B_OK;
}


static void
rmi4_uninit_driver(void* cookie)
{
	rmi4_device* device = (rmi4_device*)cookie;
	rmi4_device_uninit(device);
	delete device;
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{}
};


static driver_module_info sRmi4SmbusDriver = {
	{
		RMI4_SMBUS_DRIVER_NAME,
		0,
		NULL
	},

	rmi4_supports_device,
	rmi4_register_device,
	rmi4_init_driver,
	rmi4_uninit_driver,
	NULL,	// register child devices: published once the pad is running
	NULL,	// rescan
	NULL,	// removed
};


static struct device_module_info sRmi4SmbusDevice = {
	{
		RMI4_SMBUS_DEVICE_NAME,
		0,
		NULL
	},

	rmi4_init_device,
	rmi4_uninit_device,
	NULL,	// removed

	rmi4_open,
	rmi4_close,
	rmi4_free,
	rmi4_read,
	rmi4_write,
	NULL,	// io
	rmi4_control,

	NULL,	// select
	NULL	// deselect
};


module_info* modules[] = {
	(module_info*)&sRmi4SmbusDriver,
	(module_info*)&sRmi4SmbusDevice,
	NULL
};
