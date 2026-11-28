/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	The buttons of tablets with an Intel Bay Trail SoC, described in ACPI as a
	"5 Button Array" (PNP0C40, here INTCFD9). Its _CRS lists one GpioInt per
	button, in the order Microsoft defines: power, home, volume up, volume
	down, rotation lock. The power button already reaches the system through
	ACPI; this driver publishes the volume buttons as a keyboard that sends
	the consumer page keys of USB HID, the key codes the USB keyboard driver
	uses for volume keys, so the default Shortcuts settings act on them.
*/


#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ACPI.h>
#include <device_manager.h>
#include <kernel.h>
#include <keyboard_mouse_driver.h>
#include <usb/USB_hid.h>
#include <usb/USB_hid_page_consumer.h>
#include <lock.h>
#include <util/AutoLock.h>

#include <baytrail_gpio.h>

extern "C" {
#	include "acpi.h"
}


#define TRACE_ALWAYS(x...)	dprintf("soc_buttons: " x)

#define SOC_BUTTONS_DRIVER_MODULE_NAME	"drivers/input/soc_buttons/driver_v1"
#define SOC_BUTTONS_DEVICE_MODULE_NAME	"drivers/input/soc_buttons/device_v1"
#define SOC_BUTTONS_PUBLISH_PATH		"input/keyboard/soc_buttons/0"

// shortcut: polls the pins; switch to GPIO interrupts once baytrail_gpio
// offers them to drivers
#define POLL_INTERVAL				50000
#define KEY_BUFFER_SIZE				16

// the key codes hid_shared reports for these USB HID usages; the default
// Shortcuts settings bind them to setvolume
#define CONSUMER_KEY(usage)			(((uint32)B_HID_USAGE_PAGE_CONSUMER << 16) \
										| (usage))
#define KEY_VOLUME_UP		CONSUMER_KEY(B_HID_UID_CON_VOLUME_INCREMENT)
#define KEY_VOLUME_DOWN		CONSUMER_KEY(B_HID_UID_CON_VOLUME_DECREMENT)

#define BUTTON_VOLUME_UP			2
#define BUTTON_VOLUME_DOWN			3
#define MAX_BUTTONS					2


struct button {
	char		community[8];	// _UID of the GPIO controller
	uint32		pin;
	uint32		keycode;
	bool		pressed;
};

struct soc_buttons {
	device_node*	node;
	button			buttons[MAX_BUTTONS];
	uint32			buttonCount;
	uint32			resourceIndex;

	baytrail_gpio_pins_module_info* gpio;
	acpi_module_info* acpi;

	thread_id		pollThread;
	volatile bool	stopPolling;
	int32			openCount;

	mutex			lock;
	sem_id			keySem;
	raw_key_info	keys[KEY_BUFFER_SIZE];
	uint32			keyHead;
	uint32			keyCount;
};


static device_manager_info* sDeviceManager;


static void
queue_key(soc_buttons* device, uint32 keycode, bool down)
{
	// like the other keyboards, drop keys while nobody listens
	if (atomic_get(&device->openCount) == 0)
		return;

	MutexLocker locker(device->lock);
	if (device->keyCount == KEY_BUFFER_SIZE)
		return;

	raw_key_info& info
		= device->keys[(device->keyHead + device->keyCount) % KEY_BUFFER_SIZE];
	info.timestamp = system_time();
	info.keycode = keycode;
	info.is_keydown = down;
	device->keyCount++;
	locker.Unlock();

	release_sem(device->keySem);
}


static status_t
poll_thread(void* data)
{
	soc_buttons* device = (soc_buttons*)data;

	while (!device->stopPolling) {
		for (uint32 i = 0; i < device->buttonCount; i++) {
			button& button = device->buttons[i];
			bool high;
			if (device->gpio->get_level(button.community, button.pin, &high)
					!= B_OK) {
				continue;
			}

			// the buttons pull their line low while pressed
			bool pressed = !high;
			if (pressed != button.pressed) {
				button.pressed = pressed;
				queue_key(device, button.keycode, pressed);
			}
		}
		snooze(POLL_INTERVAL);
	}
	return B_OK;
}


/*!	Collects the GpioInt resources of the volume buttons whose controller is
	a Bay Trail GPIO community; others (a PMIC pin for the power button, for
	example) are left to their own drivers.
*/
static acpi_status
collect_buttons(acpi_resource* resource, void* context)
{
	soc_buttons* device = (soc_buttons*)context;
	if (resource->Type != ACPI_RESOURCE_TYPE_GPIO
		|| resource->Data.Gpio.ConnectionType != ACPI_RESOURCE_GPIO_TYPE_INT) {
		return AE_OK;
	}

	uint32 index = device->resourceIndex++;
	uint32 keycode;
	if (index == BUTTON_VOLUME_UP)
		keycode = KEY_VOLUME_UP;
	else if (index == BUTTON_VOLUME_DOWN)
		keycode = KEY_VOLUME_DOWN;
	else
		return AE_OK;

	const ACPI_RESOURCE_GPIO& gpio = resource->Data.Gpio;
	if (gpio.PinTableLength < 1 || gpio.ResourceSource.StringPtr == NULL
		|| device->buttonCount >= MAX_BUTTONS) {
		return AE_OK;
	}

	acpi_handle controller;
	acpi_object_type uid;
	if (device->acpi->get_handle(NULL, gpio.ResourceSource.StringPtr,
			&controller) != B_OK
		|| device->acpi->evaluate_object(controller, "_UID", NULL, &uid,
			sizeof(uid)) != B_OK
		|| uid.object_type != ACPI_TYPE_INTEGER) {
		TRACE_ALWAYS("button %" B_PRIu32 ": no GPIO controller %s\n", index,
			gpio.ResourceSource.StringPtr);
		return AE_OK;
	}

	button& button = device->buttons[device->buttonCount];
	snprintf(button.community, sizeof(button.community), "%" B_PRIu64,
		uid.integer.integer);
	button.pin = gpio.PinTable[0];
	button.keycode = keycode;

	bool high;
	if (device->gpio->get_level(button.community, button.pin, &high)
			!= B_OK) {
		TRACE_ALWAYS("button %" B_PRIu32 ": %s pin %" B_PRIu32
			" not available\n", index, gpio.ResourceSource.StringPtr,
			button.pin);
		return AE_OK;
	}
	button.pressed = !high;
	device->buttonCount++;

	TRACE_ALWAYS("%s on %s pin %" B_PRIu32 " (now %s)\n",
		index == BUTTON_VOLUME_UP ? "volume up" : "volume down",
		gpio.ResourceSource.StringPtr, button.pin, high ? "high" : "low");
	return AE_OK;
}


//	#pragma mark - device module


static status_t
soc_buttons_init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static void
soc_buttons_uninit_device(void* deviceCookie)
{
}


static status_t
soc_buttons_open(void* deviceCookie, const char* path, int openMode,
	void** _cookie)
{
	soc_buttons* device = (soc_buttons*)deviceCookie;
	atomic_add(&device->openCount, 1);
	*_cookie = device;
	return B_OK;
}


static status_t
soc_buttons_close(void* cookie)
{
	soc_buttons* device = (soc_buttons*)cookie;
	atomic_add(&device->openCount, -1);
	return B_OK;
}


static status_t
soc_buttons_free(void* cookie)
{
	return B_OK;
}


static status_t
soc_buttons_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
soc_buttons_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
soc_buttons_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	soc_buttons* device = (soc_buttons*)cookie;

	switch (op) {
		case KB_READ:
		{
			if (!IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;

			status_t status = acquire_sem_etc(device->keySem, 1,
				B_CAN_INTERRUPT, 0);
			if (status != B_OK)
				return status;

			MutexLocker locker(device->lock);
			raw_key_info info = device->keys[device->keyHead];
			device->keyHead = (device->keyHead + 1) % KEY_BUFFER_SIZE;
			device->keyCount--;
			locker.Unlock();

			return user_memcpy(buffer, &info, sizeof(info));
		}

		// no LEDs and no key repeat
		case KB_SET_LEDS:
		case KB_SET_KEY_REPEAT_RATE:
		case KB_SET_KEY_REPEAT_DELAY:
			return B_OK;
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver module


static float
soc_buttons_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK
		|| strcmp(bus, "acpi") != 0) {
		return 0.0f;
	}

	const char* hid;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK
		|| strcmp(hid, "INTCFD9") != 0) {
		return 0.0f;
	}

	return 0.6f;
}


static status_t
soc_buttons_register_device(device_node* node)
{
	device_attr attributes[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Tablet buttons" }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, SOC_BUTTONS_DRIVER_MODULE_NAME,
		attributes, NULL, NULL);
}


static void
free_device(soc_buttons* device)
{
	if (device->pollThread >= 0) {
		device->stopPolling = true;
		status_t result;
		wait_for_thread(device->pollThread, &result);
	}
	if (device->keySem >= 0)
		delete_sem(device->keySem);
	if (device->gpio != NULL)
		put_module(BAYTRAIL_GPIO_PINS_MODULE_NAME);
	if (device->acpi != NULL)
		put_module(B_ACPI_MODULE_NAME);
	mutex_destroy(&device->lock);
	delete device;
}


static status_t
soc_buttons_init_driver(device_node* node, void** _driverCookie)
{
	soc_buttons* device = new(std::nothrow) soc_buttons();
	if (device == NULL)
		return B_NO_MEMORY;

	device->node = node;
	device->pollThread = -1;
	mutex_init(&device->lock, "soc_buttons");
	device->keySem = create_sem(0, "soc_buttons keys");

	status_t status = device->keySem;
	if (status >= B_OK) {
		status = get_module(BAYTRAIL_GPIO_PINS_MODULE_NAME,
			(module_info**)&device->gpio);
		if (status != B_OK)
			device->gpio = NULL;
	}
	if (status >= B_OK) {
		status = get_module(B_ACPI_MODULE_NAME, (module_info**)&device->acpi);
		if (status != B_OK)
			device->acpi = NULL;
	}
	if (status < B_OK) {
		free_device(device);
		return status;
	}

	device_node* parent = sDeviceManager->get_parent_node(node);
	acpi_device_module_info* acpi;
	acpi_device acpiCookie;
	status = sDeviceManager->get_driver(parent, (driver_module_info**)&acpi,
		(void**)&acpiCookie);
	if (status == B_OK) {
		status = acpi->walk_resources(acpiCookie, (char*)"_CRS",
			collect_buttons, device);
	}
	sDeviceManager->put_node(parent);

	if (status == B_OK && device->buttonCount == 0)
		status = B_ENTRY_NOT_FOUND;
	if (status == B_OK) {
		device->pollThread = spawn_kernel_thread(poll_thread,
			"soc_buttons poll", B_LOW_PRIORITY, device);
		status = device->pollThread >= 0 ? B_OK : device->pollThread;
	}
	if (status != B_OK) {
		TRACE_ALWAYS("no usable buttons: %s\n", strerror(status));
		free_device(device);
		return status;
	}
	resume_thread(device->pollThread);

	*_driverCookie = device;
	return B_OK;
}


static void
soc_buttons_uninit_driver(void* driverCookie)
{
	free_device((soc_buttons*)driverCookie);
}


static status_t
soc_buttons_register_child_devices(void* driverCookie)
{
	soc_buttons* device = (soc_buttons*)driverCookie;
	return sDeviceManager->publish_device(device->node,
		SOC_BUTTONS_PUBLISH_PATH, SOC_BUTTONS_DEVICE_MODULE_NAME);
}


static device_module_info sSocButtonsDeviceModule = {
	{
		SOC_BUTTONS_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	soc_buttons_init_device,
	soc_buttons_uninit_device,
	NULL,	// remove

	soc_buttons_open,
	soc_buttons_close,
	soc_buttons_free,
	soc_buttons_read,
	soc_buttons_write,
	NULL,	// io
	soc_buttons_control,

	NULL,	// select
	NULL	// deselect
};


static driver_module_info sSocButtonsDriverModule = {
	{
		SOC_BUTTONS_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	soc_buttons_supports_device,
	soc_buttons_register_device,
	soc_buttons_init_driver,
	soc_buttons_uninit_driver,
	soc_buttons_register_child_devices,
	NULL,	// rescan
	NULL	// removed
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ NULL }
};


module_info* modules[] = {
	(module_info*)&sSocButtonsDriverModule,
	(module_info*)&sSocButtonsDeviceModule,
	NULL
};
