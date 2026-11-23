/*
 * Copyright 2026, Dru Satori, dru@druware.com.
 * All rights reserved. Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


//! Hyper-V synthetic keyboard driver (VMBus keyboard protocol 1.0)


#include <new>
#include <stdlib.h>
#include <string.h>

#include <ATKeymap.h>
#include <condition_variable.h>
#include <device_manager.h>
#include <hyperv.h>
#include <kernel.h>
#include <keyboard_mouse_driver.h>
#include <lock.h>
#include <util/AutoLock.h>

#include "Driver.h"
#include "HyperVKeyboardProtocol.h"


#define KEY_BUFFER_SIZE		64
	// key strokes buffered before new ones are dropped

#define PAUSE_KEY			(0x80 | 0x46)
	// same pseudo scan code the PS/2 keyboard driver uses for Pause


typedef struct {
	device_node*				node;
	hyperv_device_interface*	hyperv;
	hyperv_device				hyperv_cookie;
	bool						channel_open;
	uint8*						packet;

	ConditionVariable			protocol_event;
	uint32						protocol_status;

	bool						pause_pending;
	int32						open_count;

	mutex						lock;
	sem_id						key_sem;
	raw_key_info				keys[KEY_BUFFER_SIZE];
	uint32						key_head;
	uint32						key_count;
} kbd_driver_cookie;


static device_manager_info* sDeviceManager;


static void
hyperv_kbd_queue_key(kbd_driver_cookie* kbdCookie, uint32 scancode, bool isKeyDown)
{
	if (scancode == 0 || scancode > B_COUNT_OF(kATKeycodeMap))
		return;

	uint32 keycode = kATKeycodeMap[scancode - 1];
	if (keycode == 0)
		return;

	// Like PS/2, drop key strokes while nobody has the device open
	if (atomic_get(&kbdCookie->open_count) == 0)
		return;

	MutexLocker locker(kbdCookie->lock);
	if (kbdCookie->key_count == KEY_BUFFER_SIZE) {
		// Drop new key strokes rather than old ones, to not destroy what was already typed
		return;
	}

	raw_key_info& info = kbdCookie->keys[(kbdCookie->key_head + kbdCookie->key_count)
		% KEY_BUFFER_SIZE];
	info.timestamp = system_time();
	info.keycode = keycode;
	info.is_keydown = isKeyDown;
	kbdCookie->key_count++;
	locker.Unlock();

	release_sem(kbdCookie->key_sem);
}


static void
hyperv_kbd_handle_keystroke(kbd_driver_cookie* kbdCookie, hv_kbd_msg_keystroke* keystroke)
{
	uint32 info = keystroke->info;
	uint32 scancode = keystroke->make_code & 0x7f;

	if ((info & HV_KBD_INFO_UNICODE) != 0)
		return;

	// Pause arrives as E1 1D followed by 45, like on PS/2
	bool pausePending = kbdCookie->pause_pending;
	kbdCookie->pause_pending = false;
	if (pausePending && scancode == 0x45)
		scancode = PAUSE_KEY;
	else if ((info & HV_KBD_INFO_E1) != 0) {
		kbdCookie->pause_pending = scancode == 0x1d;
		return;
	} else if ((info & HV_KBD_INFO_E0) != 0)
		scancode |= 0x80;

	hyperv_kbd_queue_key(kbdCookie, scancode, (info & HV_KBD_INFO_BREAK) == 0);
}


static void
hyperv_kbd_callback(void* data)
{
	kbd_driver_cookie* kbdCookie = reinterpret_cast<kbd_driver_cookie*>(data);

	while (true) {
		uint32 length = HV_KBD_RX_PKT_BUFFER_SIZE;
		uint32 headerLength;
		uint32 messageLength;

		status_t status = kbdCookie->hyperv->read_packet(kbdCookie->hyperv_cookie,
			kbdCookie->packet, &length, &headerLength, &messageLength);
		if (status == B_DEV_NOT_READY) {
			break;
		} else if (status != B_OK) {
			ERROR("Failed to read packet (%s)\n", strerror(status));
			break;
		}

		if (messageLength < sizeof(hv_kbd_msg_header))
			continue;

		hv_kbd_in_msg* message = reinterpret_cast<hv_kbd_in_msg*>(kbdCookie->packet
			+ headerLength);
		switch (message->header.type) {
			case HV_KBD_MSGTYPE_PROTOCOL_RESPONSE:
				if (messageLength < sizeof(message->protocol_resp))
					break;
				kbdCookie->protocol_status = message->protocol_resp.status;
				kbdCookie->protocol_event.NotifyAll();
				break;

			case HV_KBD_MSGTYPE_EVENT:
				if (messageLength < sizeof(message->keystroke))
					break;
				hyperv_kbd_handle_keystroke(kbdCookie, &message->keystroke);
				break;

			default:
				TRACE("Unexpected keyboard message type %u received\n", message->header.type);
				break;
		}
	}
}


static status_t
hyperv_kbd_connect(kbd_driver_cookie* kbdCookie)
{
	status_t status = kbdCookie->hyperv->open(kbdCookie->hyperv_cookie, HV_KBD_RING_SIZE,
		HV_KBD_RING_SIZE, hyperv_kbd_callback, kbdCookie);
	if (status != B_OK) {
		ERROR("Failed to open channel\n");
		return status;
	}
	kbdCookie->channel_open = true;

	hv_kbd_msg_protocol_request request;
	request.header.type = HV_KBD_MSGTYPE_PROTOCOL_REQUEST;
	request.version = HV_KBD_VERSION_V1_0;

	ConditionVariableEntry protocolEntry;
	kbdCookie->protocol_event.Add(&protocolEntry);

	status = kbdCookie->hyperv->write_packet(kbdCookie->hyperv_cookie,
		VMBUS_PKTTYPE_DATA_INBAND, &request, sizeof(request), true, HV_KBD_REQUEST_TRANS_ID);
	if (status != B_OK) {
		ERROR("Failed to send keyboard protocol request\n");
		return status;
	}

	status = protocolEntry.Wait(B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT, HV_KBD_TIMEOUT_US);
	if (status != B_OK) {
		ERROR("No keyboard protocol response (%s)\n", strerror(status));
		return status;
	}

	if ((kbdCookie->protocol_status & HV_KBD_PROTOCOL_ACCEPTED) == 0) {
		ERROR("Keyboard protocol 1.0 rejected (status 0x%x)\n", kbdCookie->protocol_status);
		return B_NOT_SUPPORTED;
	}

	return B_OK;
}


static void
hyperv_kbd_free_cookie(kbd_driver_cookie* kbdCookie)
{
	if (kbdCookie->channel_open)
		kbdCookie->hyperv->close(kbdCookie->hyperv_cookie);
	if (kbdCookie->key_sem >= 0)
		delete_sem(kbdCookie->key_sem);
	mutex_destroy(&kbdCookie->lock);
	free(kbdCookie->packet);
	delete kbdCookie;
}


//	#pragma mark - device module


static status_t
hyperv_kbd_init_device(void* driverCookie, void** _deviceCookie)
{
	CALLED();
	*_deviceCookie = driverCookie;
	return B_OK;
}


static void
hyperv_kbd_uninit_device(void* deviceCookie)
{
	CALLED();
}


static status_t
hyperv_kbd_open(void* deviceCookie, const char* path, int openMode, void** _cookie)
{
	CALLED();
	kbd_driver_cookie* kbdCookie = reinterpret_cast<kbd_driver_cookie*>(deviceCookie);
	atomic_add(&kbdCookie->open_count, 1);
	*_cookie = deviceCookie;
	return B_OK;
}


static status_t
hyperv_kbd_close(void* cookie)
{
	CALLED();
	kbd_driver_cookie* kbdCookie = reinterpret_cast<kbd_driver_cookie*>(cookie);
	atomic_add(&kbdCookie->open_count, -1);
	return B_OK;
}


static status_t
hyperv_kbd_free(void* cookie)
{
	CALLED();
	return B_OK;
}


static status_t
hyperv_kbd_read(void* cookie, off_t pos, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
hyperv_kbd_write(void* cookie, off_t pos, const void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
hyperv_kbd_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	kbd_driver_cookie* kbdCookie = reinterpret_cast<kbd_driver_cookie*>(cookie);

	switch (op) {
		case KB_READ:
		{
			if (!IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;

			status_t status = acquire_sem_etc(kbdCookie->key_sem, 1, B_CAN_INTERRUPT, 0);
			if (status != B_OK)
				return status;

			MutexLocker locker(kbdCookie->lock);
			raw_key_info info = kbdCookie->keys[kbdCookie->key_head];
			kbdCookie->key_head = (kbdCookie->key_head + 1) % KEY_BUFFER_SIZE;
			kbdCookie->key_count--;
			locker.Unlock();

			return user_memcpy(buffer, &info, sizeof(info));
		}

		// LEDs and key repeat settings are not implemented: accept and ignore them
		case KB_SET_LEDS:
		case KB_SET_KEY_REPEAT_RATE:
		case KB_SET_KEY_REPEAT_DELAY:
			return B_OK;

		case KB_SET_DEBUG_READER:
			return B_NOT_SUPPORTED;

		default:
			return B_DEV_INVALID_IOCTL;
	}
}


//	#pragma mark - driver module


static float
hyperv_kbd_supports_device(device_node* parent)
{
	CALLED();

	// Check if parent is the Hyper-V bus manager
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK)
		return -1;

	if (strcmp(bus, HYPERV_BUS_NAME) != 0)
		return 0.0f;

	// Check if parent is a Hyper-V keyboard device
	const char* type;
	if (sDeviceManager->get_attr_string(parent, HYPERV_DEVICE_TYPE_STRING_ITEM, &type, false)
			!= B_OK)
		return 0.0f;

	if (strcmp(type, VMBUS_TYPE_KEYBOARD) != 0)
		return 0.0f;

	TRACE("Hyper-V keyboard device found!\n");
	return 0.8f;
}


static status_t
hyperv_kbd_register_device(device_node* parent)
{
	CALLED();

	device_attr attributes[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = HYPERV_PRETTYNAME_KEYBOARD }},
		{ NULL }
	};

	return sDeviceManager->register_node(parent, HYPERV_KBD_DRIVER_MODULE_NAME, attributes, NULL,
		NULL);
}


static status_t
hyperv_kbd_init_driver(device_node* node, void** _driverCookie)
{
	CALLED();

	kbd_driver_cookie* kbdCookie = new(std::nothrow) kbd_driver_cookie;
	if (kbdCookie == NULL)
		return B_NO_MEMORY;

	kbdCookie->node = node;
	kbdCookie->channel_open = false;
	kbdCookie->protocol_status = 0;
	kbdCookie->pause_pending = false;
	kbdCookie->open_count = 0;
	memset(kbdCookie->keys, 0, sizeof(kbdCookie->keys));
	kbdCookie->key_head = 0;
	kbdCookie->key_count = 0;
	kbdCookie->protocol_event.Init(kbdCookie, "hyper-v kbd protoresp");
	mutex_init(&kbdCookie->lock, "hyper-v kbd lock");
	kbdCookie->key_sem = create_sem(0, "hyper-v kbd keys");
	kbdCookie->packet = static_cast<uint8*>(malloc(HV_KBD_RX_PKT_BUFFER_SIZE));
	if (kbdCookie->key_sem < 0 || kbdCookie->packet == NULL) {
		hyperv_kbd_free_cookie(kbdCookie);
		return B_NO_MEMORY;
	}

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&kbdCookie->hyperv,
		(void**)&kbdCookie->hyperv_cookie);
	sDeviceManager->put_node(parent);

	status_t status = hyperv_kbd_connect(kbdCookie);
	if (status != B_OK) {
		hyperv_kbd_free_cookie(kbdCookie);
		return status;
	}

	*_driverCookie = kbdCookie;
	return B_OK;
}


static void
hyperv_kbd_uninit_driver(void* driverCookie)
{
	CALLED();
	hyperv_kbd_free_cookie(reinterpret_cast<kbd_driver_cookie*>(driverCookie));
}


static status_t
hyperv_kbd_register_child_devices(void* driverCookie)
{
	CALLED();
	kbd_driver_cookie* kbdCookie = reinterpret_cast<kbd_driver_cookie*>(driverCookie);

	status_t status = sDeviceManager->publish_device(kbdCookie->node, HYPERV_KBD_PUBLISH_PATH,
		HYPERV_KBD_DEVICE_MODULE_NAME);
	return status;
}


static device_module_info sHyperVKeyboardDeviceModule = {
	{
		HYPERV_KBD_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	hyperv_kbd_init_device,
	hyperv_kbd_uninit_device,
	NULL,	// remove

	hyperv_kbd_open,
	hyperv_kbd_close,
	hyperv_kbd_free,
	hyperv_kbd_read,
	hyperv_kbd_write,
	NULL,	// io
	hyperv_kbd_control,

	NULL,	// select
	NULL	// deselect
};


static driver_module_info sHyperVKeyboardDriverModule = {
	{
		HYPERV_KBD_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	hyperv_kbd_supports_device,
	hyperv_kbd_register_device,
	hyperv_kbd_init_driver,
	hyperv_kbd_uninit_driver,
	hyperv_kbd_register_child_devices,
	NULL,	// rescan
	NULL	// removed
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ NULL }
};


module_info* modules[] = {
	(module_info*)&sHyperVKeyboardDriverModule,
	(module_info*)&sHyperVKeyboardDeviceModule,
	NULL
};
