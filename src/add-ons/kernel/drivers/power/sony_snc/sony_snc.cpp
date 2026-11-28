/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * Sony Notebook Control (ACPI device SNY5001) of Sony VAIO laptops: the Fn
 * hotkeys and the VAIO/ASSIST buttons, published as a keyboard device
 * (/dev/input/keyboard/sony_snc/0) that the input_server keyboard add-on
 * picks up like any other keyboard.
 *
 * Only the handle based SNC interface (SN00/SN02/SN03/SN07, VAIO models from
 * about 2010 on) is supported. The firmware lists its functions as 16-bit
 * "handles"; hotkeys come from handle 0x0100, notified as 0x90 + its offset
 * in the list, and the key code is read back with SN07. The calling
 * conventions and the handle and key code meanings were checked against
 * Linux's drivers/platform/x86/sony-laptop.c and the DSDT of a VPCEB3J1E; no
 * code was taken from Linux.
 */


#include <ACPI.h>
#include <usb/USB_hid.h>
#include <usb/USB_hid_page_consumer.h>
#include <usb/USB_hid_page_generic_desktop.h>

#include <keyboard_mouse_driver.h>
#include <kernel.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define SONY_SNC_MODULE_NAME			"drivers/power/sony_snc/driver_v1"
#define SONY_SNC_KEYBOARD_MODULE_NAME	"drivers/power/sony_snc/keyboard_v1"

#define SONY_SNC_KEYBOARD_BASENAME		"input/keyboard/sony_snc/%d"
#define SONY_SNC_PATHID_GENERATOR		"sony_snc/path_id"

#define SONY_SNC_HID					"SNY5001"

#define SNC_MAX_HANDLES					16

// SN00 arguments
#define SNC_QUERY_EVENT_MASK			0x10
#define SNC_QUERY_HANDLE_BASE			0x20

// SN07 arguments, combined with the handle offset
#define SNC_HOTKEY_SETUP				0x0000
#define SNC_HOTKEY_READ					0x0200

#define SNC_NOTIFY_HANDLE_BASE			0x90

// hotkey codes reported by handle 0x0100; bit 7 is set on press
#define SNC_HOTKEY_PRESSED				0x80
#define SNC_HOTKEY_CODE_MASK			0x7f

#define KEY_QUEUE_SIZE					32

#define HID_KEY(page, usage)			(((uint32)(page) << 16) | (uint32)(usage))
#define CONSUMER_KEY(usage)				HID_KEY(B_HID_USAGE_PAGE_CONSUMER, usage)
#define DESKTOP_KEY(usage) \
	HID_KEY(B_HID_USAGE_PAGE_GENERIC_DESKTOP, usage)

//#define TRACE_SONY_SNC
#ifdef TRACE_SONY_SNC
#	define TRACE(x...) dprintf("sony_snc: " x)
#else
#	define TRACE(x...)
#endif
#define ERROR(x...) dprintf("sony_snc: " x)


static device_manager_info* sDeviceManager;


typedef struct sony_snc_device {
	device_node*			node;
	acpi_device_module_info* acpi;
	acpi_device				acpi_cookie;

	bool					has_handles;
	uint16					handles[SNC_MAX_HANDLES];
	uint32					event_mask;
	bool					notify_installed;

	sem_id					lock;
		// protects the key queue; only taken in thread context (the ACPI
		// notify handler runs in a DPC thread)
	sem_id					key_sem;
		// released once per queued key; readers re-check the queue
	raw_key_info			keys[KEY_QUEUE_SIZE];
	uint32					key_head;
	uint32					key_count;
} sony_snc_device;


typedef struct sony_snc_cookie {
	sony_snc_device*		device;
	volatile bool			closed;
} sony_snc_cookie;


/*!	Hotkeys reported as keys, using the same key codes that a USB keyboard
	produces for the HID usage closest to the symbol printed on the key. What
	the keys do is left to user space (the Shortcuts preferences, for example).
	The codes are those of handle 0x0100 with bit 7 cleared.
*/
static const struct {
	uint8	code;
	uint32	key;
} kHotkeyMap[] = {
	{ 0x05, CONSUMER_KEY(B_HID_UID_CON_DISPLAY_BRIGHTNESS_DECREMENT) },
		// Fn+F5
	{ 0x06, CONSUMER_KEY(B_HID_UID_CON_DISPLAY_BRIGHTNESS_INCREMENT) },
		// Fn+F6
	{ 0x07, DESKTOP_KEY(B_HID_UID_GD_SYSTEM_DISPLAY_TOGGLE) },
		// Fn+F7, switch display
	{ 0x09, CONSUMER_KEY(B_HID_UID_CON_AC_ZOOM_OUT) },
		// Fn+F9
	{ 0x0a, CONSUMER_KEY(B_HID_UID_CON_AC_ZOOM_IN) },
		// Fn+F10
	{ 0x0c, DESKTOP_KEY(B_HID_UID_GD_SYSTEM_SLEEP) },
		// Fn+F12, sleep
	{ 0x10, CONSUMER_KEY(B_HID_UID_CON_AL_PROGRAMMABLE_BUTTON_CONFIGURATION) },
		// S1 button (on some models it is a plain keyboard key instead)
	{ 0x25, CONSUMER_KEY(B_HID_UID_CON_AL_CONSUMER_CONTROL_CONFIGURATION) },
		// VAIO button
	{ 0x26, CONSUMER_KEY(B_HID_UID_CON_HELP) },
		// ASSIST button
};


static inline status_t
snc_lock(sony_snc_device* device)
{
	return acquire_sem(device->lock);
}


static inline void
snc_unlock(sony_snc_device* device)
{
	release_sem(device->lock);
}


//	#pragma mark - SNC method helpers


/*!	Evaluates an SNC method taking at most one integer argument and
	optionally returning an integer.
*/
static status_t
snc_call(sony_snc_device* device, const char* method, const uint32* argument,
	uint32* _result)
{
	acpi_object_type argObject;
	acpi_objects args;
	args.count = 0;
	args.pointer = NULL;
	if (argument != NULL) {
		argObject.object_type = ACPI_TYPE_INTEGER;
		argObject.integer.integer = *argument;
		args.count = 1;
		args.pointer = &argObject;
	}

	acpi_data buffer;
	buffer.pointer = NULL;
	buffer.length = ACPI_ALLOCATE_BUFFER;

	status_t status = device->acpi->evaluate_method(device->acpi_cookie,
		method, args.count > 0 ? &args : NULL,
		_result != NULL ? &buffer : NULL);
	if (status != B_OK)
		return status;

	if (_result != NULL) {
		// SN07 returns a buffer field, which ACPICA hands back either as an
		// integer or as a buffer depending on its width; accept both.
		acpi_object_type* object = (acpi_object_type*)buffer.pointer;
		if (object == NULL) {
			status = B_BAD_DATA;
		} else if (object->object_type == ACPI_TYPE_INTEGER) {
			*_result = (uint32)object->integer.integer;
		} else if (object->object_type == ACPI_TYPE_BUFFER) {
			uint32 value = 0;
			const uint8* bytes = (const uint8*)object->buffer.buffer;
			for (uint32 i = 0; i < object->buffer.length && i < 4; i++)
				value |= (uint32)bytes[i] << (8 * i);
			*_result = value;
		} else {
			status = B_BAD_DATA;
		}
		free(buffer.pointer);
	}

	return status;
}


static int32
snc_find_handle(sony_snc_device* device, uint16 handle)
{
	if (!device->has_handles || handle == 0)
		return -1;

	for (int32 i = 0; i < SNC_MAX_HANDLES; i++) {
		if (device->handles[i] == handle)
			return i;
	}
	return -1;
}


static status_t
snc_call_handle(sony_snc_device* device, uint16 handle, uint32 argument,
	uint32* _result)
{
	int32 offset = snc_find_handle(device, handle);
	if (offset < 0)
		return B_ENTRY_NOT_FOUND;

	uint32 value = (argument & ~(uint32)0xff) | (uint32)offset;
	uint32 result = 0;
	status_t status = snc_call(device, "SN07", &value, &result);
	if (status == B_OK && _result != NULL)
		*_result = result;
	return status;
}


static void
snc_setup_handles(sony_snc_device* device)
{
	device->has_handles = false;
	memset(device->handles, 0, sizeof(device->handles));

	for (uint32 i = 0; i < SNC_MAX_HANDLES; i++) {
		uint32 argument = SNC_QUERY_HANDLE_BASE + i;
		uint32 result;
		if (snc_call(device, "SN00", &argument, &result) != B_OK) {
			if (i == 0) {
				ERROR("SN00 not available, handle interface not supported\n");
				return;
			}
			continue;
		}
		device->handles[i] = (uint16)result;
		if (result != 0)
			TRACE("handle 0x%04" B_PRIx32 " at offset 0x%" B_PRIx32 "\n",
				result, i);
	}
	device->has_handles = true;
}


static void
snc_enable_events(sony_snc_device* device)
{
	if (!device->has_handles)
		return;

	// Hotkey handles need a setup call before they report key codes.
	static const uint16 kHotkeyHandles[] = { 0x0100, 0x0101, 0x0127 };
	for (size_t i = 0; i < B_COUNT_OF(kHotkeyHandles); i++) {
		if (snc_find_handle(device, kHotkeyHandles[i]) < 0)
			continue;
		uint32 result;
		if (snc_call_handle(device, kHotkeyHandles[i], SNC_HOTKEY_SETUP,
				&result) != B_OK) {
			ERROR("setup of hotkey handle 0x%04x failed\n",
				kHotkeyHandles[i]);
		}
	}

	uint32 argument = SNC_QUERY_EVENT_MASK;
	uint32 mask;
	if (snc_call(device, "SN00", &argument, &mask) != B_OK) {
		ERROR("could not read the event mask\n");
		return;
	}

	// SN02 and SN03 do not return a value
	if (snc_call(device, "SN02", &mask, NULL) != B_OK) {
		ERROR("could not enable events (mask 0x%" B_PRIx32 ")\n", mask);
		return;
	}

	device->event_mask = mask;
	TRACE("events enabled, mask 0x%" B_PRIx32 "\n", mask);
}


static void
snc_disable_events(sony_snc_device* device)
{
	if (device->event_mask == 0)
		return;

	if (snc_call(device, "SN03", &device->event_mask, NULL) != B_OK)
		ERROR("could not disable events\n");
	device->event_mask = 0;
}


//	#pragma mark - key queue


static void
snc_queue_key(sony_snc_device* device, uint32 hotkey, bigtime_t when)
{
	uint32 key = 0;
	for (size_t i = 0; i < B_COUNT_OF(kHotkeyMap); i++) {
		if (kHotkeyMap[i].code == (hotkey & SNC_HOTKEY_CODE_MASK)) {
			key = kHotkeyMap[i].key;
			break;
		}
	}
	if (key == 0) {
		TRACE("unmapped hotkey 0x%02" B_PRIx32 "\n", hotkey);
		return;
	}

	if (snc_lock(device) != B_OK)
		return;

	if (device->key_count == KEY_QUEUE_SIZE) {
		// drop the oldest key
		device->key_head = (device->key_head + 1) % KEY_QUEUE_SIZE;
		device->key_count--;
	}

	raw_key_info& info
		= device->keys[(device->key_head + device->key_count) % KEY_QUEUE_SIZE];
	info.timestamp = when;
	info.keycode = key;
	info.is_keydown = (hotkey & SNC_HOTKEY_PRESSED) != 0;
	device->key_count++;

	snc_unlock(device);

	release_sem_etc(device->key_sem, 1, B_DO_NOT_RESCHEDULE);
}


static void
snc_notify_handler(acpi_handle _device, uint32 value, void* context)
{
	sony_snc_device* device = (sony_snc_device*)context;
	bigtime_t when = system_time();

	if (value < SNC_NOTIFY_HANDLE_BASE
		|| value >= SNC_NOTIFY_HANDLE_BASE + SNC_MAX_HANDLES
		|| !device->has_handles) {
		TRACE("ignoring notification 0x%02" B_PRIx32 "\n", value);
		return;
	}

	uint16 handle = device->handles[value - SNC_NOTIFY_HANDLE_BASE];
	if (handle != 0x0100 && handle != 0x0127) {
		TRACE("ignoring notification 0x%02" B_PRIx32 " (handle 0x%04x)\n",
			value, handle);
		return;
	}

	uint32 result;
	if (snc_call_handle(device, handle, SNC_HOTKEY_READ, &result) != B_OK) {
		ERROR("could not read the hotkey code\n");
		return;
	}

	TRACE("hotkey 0x%02" B_PRIx32 "\n", result & 0xff);
	snc_queue_key(device, result & 0xff, when);
}


//	#pragma mark - keyboard device module API


static status_t
sony_snc_init_device(void* driverCookie, void** cookie)
{
	*cookie = driverCookie;
	return B_OK;
}


static void
sony_snc_uninit_device(void* cookie)
{
}


static status_t
sony_snc_open(void* _device, const char* path, int openMode, void** _cookie)
{
	sony_snc_cookie* cookie = (sony_snc_cookie*)calloc(1, sizeof(*cookie));
	if (cookie == NULL)
		return B_NO_MEMORY;

	cookie->device = (sony_snc_device*)_device;
	*_cookie = cookie;
	return B_OK;
}


static status_t
sony_snc_close(void* _cookie)
{
	sony_snc_cookie* cookie = (sony_snc_cookie*)_cookie;

	// Wake up a reader blocked in KB_READ. A plain release (rather than
	// B_RELEASE_ALL) also covers a reader that has not started waiting yet:
	// it gets the count, loops and sees the closed flag.
	cookie->closed = true;
	release_sem(cookie->device->key_sem);
	return B_OK;
}


static status_t
sony_snc_free(void* cookie)
{
	free(cookie);
	return B_OK;
}


static status_t
sony_snc_read_key(sony_snc_cookie* cookie, void* buffer, size_t length)
{
	sony_snc_device* device = cookie->device;

	if (buffer == NULL || length < sizeof(raw_key_info)
		|| !IS_USER_ADDRESS(buffer)) {
		return B_BAD_VALUE;
	}

	while (true) {
		if (cookie->closed)
			return B_FILE_ERROR;

		status_t status = snc_lock(device);
		if (status != B_OK)
			return status;

		if (device->key_count > 0) {
			raw_key_info info = device->keys[device->key_head];
			device->key_head = (device->key_head + 1) % KEY_QUEUE_SIZE;
			device->key_count--;
			snc_unlock(device);

			return user_memcpy(buffer, &info, sizeof(info));
		}
		snc_unlock(device);

		status = acquire_sem_etc(device->key_sem, 1, B_CAN_INTERRUPT, 0);
		if (status != B_OK)
			return status;
	}
}


static status_t
sony_snc_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
sony_snc_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
sony_snc_control(void* _cookie, uint32 op, void* arg, size_t length)
{
	sony_snc_cookie* cookie = (sony_snc_cookie*)_cookie;

	switch (op) {
		case KB_READ:
			return sony_snc_read_key(cookie, arg, length);

		case KB_SET_LEDS:
		case KB_SET_KEY_REPEATING:
		case KB_SET_KEY_NONREPEATING:
		case KB_SET_KEY_REPEAT_RATE:
		case KB_SET_KEY_REPEAT_DELAY:
			// no LEDs, and the firmware sends its own press/release events
			return B_OK;
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver module API


static float
sony_snc_support(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "acpi") != 0) {
		return 0.0;
	}

	uint32 deviceType;
	if (sDeviceManager->get_attr_uint32(parent, ACPI_DEVICE_TYPE_ITEM,
			&deviceType, false) != B_OK
		|| deviceType != ACPI_TYPE_DEVICE) {
		return 0.0;
	}

	const char* hid;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK || strcmp(hid, SONY_SNC_HID) != 0) {
		return 0.0;
	}

	return 0.6;
}


static status_t
sony_snc_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Sony Notebook Control" }},
		// keep the notify handler installed even when nobody has the device
		// open, so that firmware events are never left unhandled
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, SONY_SNC_MODULE_NAME, attrs,
		NULL, NULL);
}


static status_t
sony_snc_init_driver(device_node* node, void** _driverCookie)
{
	sony_snc_device* device
		= (sony_snc_device*)calloc(1, sizeof(sony_snc_device));
	if (device == NULL)
		return B_NO_MEMORY;

	device->node = node;
	device->lock = create_sem(1, "sony_snc lock");
	device->key_sem = create_sem(0, "sony_snc keys");
	if (device->lock < 0 || device->key_sem < 0) {
		delete_sem(device->lock);
		delete_sem(device->key_sem);
		free(device);
		return B_NO_MORE_SEMS;
	}

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&device->acpi,
		(void**)&device->acpi_cookie);
	sDeviceManager->put_node(parent);

	snc_setup_handles(device);

	status_t status = device->acpi->install_notify_handler(
		device->acpi_cookie, ACPI_DEVICE_NOTIFY, snc_notify_handler, device);
	if (status != B_OK)
		ERROR("could not install notify handler: %s\n", strerror(status));
	else
		device->notify_installed = true;

	snc_enable_events(device);

	*_driverCookie = device;
	return B_OK;
}


static void
sony_snc_uninit_driver(void* driverCookie)
{
	sony_snc_device* device = (sony_snc_device*)driverCookie;

	snc_disable_events(device);

	if (device->notify_installed) {
		device->acpi->remove_notify_handler(device->acpi_cookie,
			ACPI_DEVICE_NOTIFY, snc_notify_handler);
	}

	delete_sem(device->key_sem);
	delete_sem(device->lock);
	free(device);
}


static status_t
sony_snc_register_child_devices(void* driverCookie)
{
	sony_snc_device* device = (sony_snc_device*)driverCookie;

	if (!device->has_handles)
		return B_OK;

	int pathID = sDeviceManager->create_id(SONY_SNC_PATHID_GENERATOR);
	if (pathID < 0) {
		ERROR("could not create a path ID\n");
		return B_ERROR;
	}

	char name[B_DEV_NAME_LENGTH];
	snprintf(name, sizeof(name), SONY_SNC_KEYBOARD_BASENAME, pathID);

	return sDeviceManager->publish_device(device->node, name,
		SONY_SNC_KEYBOARD_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


driver_module_info sony_snc_driver_module = {
	{
		SONY_SNC_MODULE_NAME,
		0,
		NULL
	},

	sony_snc_support,
	sony_snc_register_device,
	sony_snc_init_driver,
	sony_snc_uninit_driver,
	sony_snc_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


struct device_module_info sony_snc_keyboard_module = {
	{
		SONY_SNC_KEYBOARD_MODULE_NAME,
		0,
		NULL
	},

	sony_snc_init_device,
	sony_snc_uninit_device,
	NULL,	// removed

	sony_snc_open,
	sony_snc_close,
	sony_snc_free,
	sony_snc_read,
	sony_snc_write,
	NULL,	// io
	sony_snc_control,
	NULL,	// select
	NULL	// deselect
};


module_info* modules[] = {
	(module_info*)&sony_snc_driver_module,
	(module_info*)&sony_snc_keyboard_module,
	NULL
};
