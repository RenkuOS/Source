/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * Sony Notebook Control (ACPI device SNY5001) of Sony VAIO laptops: the Fn
 * hotkeys and the VAIO/ASSIST buttons, published as a keyboard device
 * (/dev/input/keyboard/sony_snc/0) that the input_server keyboard add-on
 * picks up like any other keyboard, and a control device
 * (/dev/power/sony_snc/0, see headers/private/drivers/sony_snc.h) for battery
 * care, the thermal profile, the touchpad and the wireless switch state.
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
#include <sony_snc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define SONY_SNC_MODULE_NAME			"drivers/power/sony_snc/driver_v1"
#define SONY_SNC_KEYBOARD_MODULE_NAME	"drivers/power/sony_snc/keyboard_v1"
#define SONY_SNC_CONTROL_MODULE_NAME	"drivers/power/sony_snc/control_v1"

#define SONY_SNC_KEYBOARD_BASENAME		"input/keyboard/sony_snc/%d"
#define SONY_SNC_CONTROL_BASENAME		"power/sony_snc/%d"
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

// battery care (handles 0x0115, 0x0136, 0x013f)
#define SNC_BATTERY_CARE_GET			0x0000
#define SNC_BATTERY_CARE_SET			0x0100
#define SNC_BATTERY_HEALTH_GET			0x0200

// thermal profile (handle 0x0122)
#define SNC_THERMAL_HANDLE				0x0122
#define SNC_THERMAL_GET_PROFILES		0x0000
#define SNC_THERMAL_GET					0x0100
#define SNC_THERMAL_SET					0x0200

// touchpad (handles 0x0105, 0x0148)
#define SNC_TOUCHPAD_GET				0x0000
#define SNC_TOUCHPAD_SET				0x0100

// wireless (handles 0x0124, 0x0135)
#define SNC_WIRELESS_SWITCH_GET			0x0200
#define SNC_RADIO_LIST_SIZE				32

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

	// handles of the functions offered through the control device, 0 if
	// the firmware does not list them
	uint16					battery_care_handle;
	uint16					touchpad_handle;
	uint16					wireless_handle;
	bool					has_thermal;
	uint32					radios;

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


/*!	Evaluates an SNC method taking an integer argument and returning a buffer
	(SN06), copying at most \a size bytes of it.
*/
static status_t
snc_call_buffer(sony_snc_device* device, const char* method, uint32 argument,
	uint8* data, size_t size)
{
	acpi_object_type argObject;
	argObject.object_type = ACPI_TYPE_INTEGER;
	argObject.integer.integer = argument;
	acpi_objects args;
	args.count = 1;
	args.pointer = &argObject;

	acpi_data buffer;
	buffer.pointer = NULL;
	buffer.length = ACPI_ALLOCATE_BUFFER;

	status_t status = device->acpi->evaluate_method(device->acpi_cookie,
		method, &args, &buffer);
	if (status != B_OK)
		return status;

	acpi_object_type* object = (acpi_object_type*)buffer.pointer;
	if (object == NULL || object->object_type != ACPI_TYPE_BUFFER) {
		free(buffer.pointer);
		return B_BAD_DATA;
	}

	memset(data, 0xff, size);
	memcpy(data, object->buffer.buffer, min_c(size, object->buffer.length));
	free(buffer.pointer);
	return B_OK;
}


static uint16
snc_first_handle(sony_snc_device* device, const uint16* handles, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (snc_find_handle(device, handles[i]) >= 0)
			return handles[i];
	}
	return 0;
}


static void
snc_setup_features(sony_snc_device* device)
{
	static const uint16 kBatteryCareHandles[] = { 0x0115, 0x0136, 0x013f };
	static const uint16 kTouchpadHandles[] = { 0x0105, 0x0148 };
	static const uint16 kWirelessHandles[] = { 0x0124, 0x0135 };

	device->battery_care_handle = snc_first_handle(device,
		kBatteryCareHandles, B_COUNT_OF(kBatteryCareHandles));
	device->touchpad_handle = snc_first_handle(device, kTouchpadHandles,
		B_COUNT_OF(kTouchpadHandles));
	device->wireless_handle = snc_first_handle(device, kWirelessHandles,
		B_COUNT_OF(kWirelessHandles));
	device->has_thermal = snc_find_handle(device, SNC_THERMAL_HANDLE) >= 0;

	// The radios are listed by codes in the SN06 buffer of the wireless
	// handle, terminated by 0xff.
	device->radios = 0;
	if (device->wireless_handle != 0) {
		uint8 list[SNC_RADIO_LIST_SIZE];
		int32 offset = snc_find_handle(device, device->wireless_handle);
		if (snc_call_buffer(device, "SN06", (uint32)offset, list, sizeof(list))
				== B_OK) {
			for (size_t i = 0; i < sizeof(list) && list[i] != 0xff; i++) {
				if (list[i] == 0x00)
					device->radios |= SONY_SNC_RADIO_WLAN;
				else if (list[i] == 0x10)
					device->radios |= SONY_SNC_RADIO_BLUETOOTH;
				else if ((list[i] & 0xf0) == 0x20 || (list[i] & 0xf0) == 0x50)
					device->radios |= SONY_SNC_RADIO_WWAN;
				else if (list[i] == 0x30)
					device->radios |= SONY_SNC_RADIO_WIMAX;
			}
		}
	}
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


//	#pragma mark - functions of the control device


static uint32
snc_battery_care_limit(uint32 result)
{
	if ((result & 0x01) == 0)
		return 0;

	switch ((result & 0x30) >> 4) {
		case 1:
			return 80;
		case 2:
			return 50;
		case 3:
			return 100;
	}
	return 0;
}


static void
snc_get_status(sony_snc_device* device, sony_snc_status& status)
{
	memset(&status, 0, sizeof(status));
	status.api_version = SONY_SNC_API_VERSION;

	uint32 result;
	if (device->battery_care_handle != 0
		&& snc_call_handle(device, device->battery_care_handle,
			SNC_BATTERY_CARE_GET, &result) == B_OK) {
		status.features |= SONY_SNC_FEATURE_BATTERY_CARE;
		status.battery_care_limit = snc_battery_care_limit(result);

		// handle 0x0115 has no health status
		if (device->battery_care_handle != 0x0115
			&& snc_call_handle(device, device->battery_care_handle,
				SNC_BATTERY_HEALTH_GET, &result) == B_OK) {
			status.features |= SONY_SNC_FEATURE_BATTERY_HEALTH;
			status.battery_health = result & 0xff;
		}
	}

	if (device->has_thermal
		&& snc_call_handle(device, SNC_THERMAL_HANDLE, SNC_THERMAL_GET_PROFILES,
			&result) == B_OK) {
		// The firmware reports the optional profiles as a bit mask whose
		// bits match the profile numbers (bit 0 silent, bit 1 performance).
		status.thermal_profiles = (1 << SONY_SNC_THERMAL_BALANCED)
			| ((result & 0x1) != 0 ? 1 << SONY_SNC_THERMAL_SILENT : 0)
			| ((result & 0x2) != 0 ? 1 << SONY_SNC_THERMAL_PERFORMANCE : 0);
		if (snc_call_handle(device, SNC_THERMAL_HANDLE, SNC_THERMAL_GET,
				&result) == B_OK) {
			status.features |= SONY_SNC_FEATURE_THERMAL;
			status.thermal_profile = result & 0xff;
		}
	}

	if (device->touchpad_handle != 0
		&& snc_call_handle(device, device->touchpad_handle, SNC_TOUCHPAD_GET,
			&result) == B_OK) {
		// the firmware reports whether the touchpad is disabled
		status.features |= SONY_SNC_FEATURE_TOUCHPAD;
		status.touchpad_enabled = (result & 0x01) == 0;
	}

	if (device->wireless_handle != 0
		&& snc_call_handle(device, device->wireless_handle,
			SNC_WIRELESS_SWITCH_GET, &result) == B_OK) {
		status.features |= SONY_SNC_FEATURE_WIRELESS;
		status.wireless_switch_on = (result & 0x01) != 0;
		status.radios = device->radios;

		// each radio has its own address; bit 1 is set when it is not
		// blocked in software
		static const struct {
			uint32	radio;
			uint32	address;
		} kRadios[] = {
			{ SONY_SNC_RADIO_WLAN, 0x0300 },
			{ SONY_SNC_RADIO_BLUETOOTH, 0x0500 },
			{ SONY_SNC_RADIO_WWAN, 0x0700 },
			{ SONY_SNC_RADIO_WIMAX, 0x0900 },
		};
		for (size_t i = 0; i < B_COUNT_OF(kRadios); i++) {
			if ((device->radios & kRadios[i].radio) != 0
				&& snc_call_handle(device, device->wireless_handle,
					kRadios[i].address, &result) == B_OK
				&& (result & 0x02) != 0) {
				status.radios_enabled |= kRadios[i].radio;
			}
		}
	}
}


static status_t
snc_set_battery_care(sony_snc_device* device, uint32 limit)
{
	if (device->battery_care_handle == 0)
		return B_NOT_SUPPORTED;

	// Limit in bits 5:4 (01 80%, 10 50%, 11 100%), also stored in the
	// battery (bits 7:6) except with handle 0x013f; bit 0 enables the limit.
	uint32 command = 0;
	switch (limit) {
		case 0:
			break;
		case 50:
			command = 0x20;
			break;
		case 80:
			command = 0x10;
			break;
		case 100:
			command = 0x30;
			break;
		default:
			return B_BAD_VALUE;
	}
	if (command != 0) {
		if (device->battery_care_handle != 0x013f)
			command |= command << 2;
		command = (command | 0x01) << 16;
	}

	uint32 result;
	return snc_call_handle(device, device->battery_care_handle,
		command | SNC_BATTERY_CARE_SET, &result);
}


static status_t
snc_set_thermal_profile(sony_snc_device* device, uint32 profile)
{
	if (!device->has_thermal)
		return B_NOT_SUPPORTED;

	uint32 profiles;
	status_t status = snc_call_handle(device, SNC_THERMAL_HANDLE,
		SNC_THERMAL_GET_PROFILES, &profiles);
	if (status != B_OK)
		return status;

	// balanced is always available, the others only if listed
	if (profile > SONY_SNC_THERMAL_PERFORMANCE
		|| (profile != SONY_SNC_THERMAL_BALANCED
			&& (profiles & profile) == 0)) {
		return B_BAD_VALUE;
	}

	uint32 result;
	return snc_call_handle(device, SNC_THERMAL_HANDLE,
		(profile << 16) | SNC_THERMAL_SET, &result);
}


static status_t
snc_set_touchpad(sony_snc_device* device, uint32 enabled)
{
	if (device->touchpad_handle == 0)
		return B_NOT_SUPPORTED;
	if (enabled > 1)
		return B_BAD_VALUE;

	// the firmware expects 1 to disable the touchpad
	uint32 result;
	return snc_call_handle(device, device->touchpad_handle,
		((enabled == 0 ? 1 : 0) << 16) | SNC_TOUCHPAD_SET, &result);
}


//	#pragma mark - control device module API


static status_t
sony_snc_control_close(void* cookie)
{
	return B_OK;
}


static status_t
sony_snc_control_ioctl(void* _cookie, uint32 op, void* arg, size_t length)
{
	sony_snc_cookie* cookie = (sony_snc_cookie*)_cookie;
	sony_snc_device* device = cookie->device;

	if (arg == NULL || !IS_USER_ADDRESS(arg))
		return B_BAD_ADDRESS;

	switch (op) {
		case SONY_SNC_GET_STATUS:
		{
			sony_snc_status status;
			snc_get_status(device, status);
			return user_memcpy(arg, &status,
				min_c(length, sizeof(sony_snc_status)));
		}

		case SONY_SNC_SET_BATTERY_CARE:
		case SONY_SNC_SET_THERMAL_PROFILE:
		case SONY_SNC_SET_TOUCHPAD:
		{
			// The charge limit, the thermal profile and the touchpad are
			// for the system to change, not for any program: the status
			// stays open to everyone.
			if (geteuid() != 0)
				return B_NOT_ALLOWED;

			uint32 value;
			if (length < sizeof(value)
				|| user_memcpy(&value, arg, sizeof(value)) != B_OK) {
				return B_BAD_ADDRESS;
			}

			if (op == SONY_SNC_SET_BATTERY_CARE)
				return snc_set_battery_care(device, value);
			if (op == SONY_SNC_SET_THERMAL_PROFILE)
				return snc_set_thermal_profile(device, value);
			return snc_set_touchpad(device, value);
		}
	}

	return B_DEV_INVALID_IOCTL;
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
	snc_setup_features(device);

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
	status_t status = sDeviceManager->publish_device(device->node, name,
		SONY_SNC_KEYBOARD_MODULE_NAME);
	if (status != B_OK)
		return status;

	snprintf(name, sizeof(name), SONY_SNC_CONTROL_BASENAME, pathID);
	return sDeviceManager->publish_device(device->node, name,
		SONY_SNC_CONTROL_MODULE_NAME);
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


struct device_module_info sony_snc_control_module = {
	{
		SONY_SNC_CONTROL_MODULE_NAME,
		0,
		NULL
	},

	sony_snc_init_device,
	sony_snc_uninit_device,
	NULL,	// removed

	sony_snc_open,
	sony_snc_control_close,
	sony_snc_free,
	sony_snc_read,
	sony_snc_write,
	NULL,	// io
	sony_snc_control_ioctl,
	NULL,	// select
	NULL	// deselect
};


module_info* modules[] = {
	(module_info*)&sony_snc_driver_module,
	(module_info*)&sony_snc_keyboard_module,
	(module_info*)&sony_snc_control_module,
	NULL
};
