/*
 * Copyright 2026, Andrea Bernardi
 * Copyright 2008-2011, Michael Lotz <mmlr@mlotz.ch>
 * SPDX-License-Identifier: MIT
 */


//!	Driver for the HID devices (keyboards, mice, gamepads) behind Bluetooth.


#include "DeviceList.h"
#include "Driver.h"
#include "HIDDevice.h"
#include "ProtocolHandler.h"

#include <AutoDeleter.h>
#include <bluetooth_hid.h>
#include <fs/devfs.h>
#include <lock.h>
#include <util/AutoLock.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


struct device_cookie {
	ProtocolHandler*	handler;
	uint32				cookie;
};


// one for every open of the control device, that is one Bluetooth device
struct control_cookie {
	HIDDevice*			device;
};


int32 api_version = B_CUR_DRIVER_API_VERSION;
DeviceList* gDeviceList = NULL;
static mutex sDriverLock;

// stands for the control device in gDeviceList
static int32 sControlDevice;


static void
republish_devices()
{
	// not with sDriverLock held: the kernel calls publish_devices()
	devfs_rescan_driver(DRIVER_NAME);
}


//	#pragma mark - control device


static status_t
bluetooth_hid_control_open(const char* name, uint32 flags, void** _cookie)
{
	control_cookie* cookie = new(std::nothrow) control_cookie;
	if (cookie == NULL)
		return B_NO_MEMORY;

	cookie->device = NULL;
	*_cookie = cookie;
	return B_OK;
}


static status_t
bluetooth_hid_control_close(void* cookie)
{
	return B_OK;
}


static status_t
bluetooth_hid_control_free(void* _cookie)
{
	control_cookie* cookie = (control_cookie*)_cookie;

	if (cookie->device != NULL) {
		MutexLocker locker(sDriverLock);
		HIDDevice* device = cookie->device;

		for (uint32 i = 0;; i++) {
			ProtocolHandler* handler = device->ProtocolHandlerAt(i);
			if (handler == NULL)
				break;
			gDeviceList->RemoveDevice(NULL, handler);
		}

		if (device->IsOpen()) {
			// deleted when its last handler is closed
			device->Removed();
		} else
			delete device;

		locker.Unlock();
		republish_devices();
	}

	delete cookie;
	return B_OK;
}


static status_t
bluetooth_hid_control_control(void* _cookie, uint32 op, void* buffer,
	size_t length)
{
	control_cookie* cookie = (control_cookie*)_cookie;

	if (op != BLUETOOTH_HID_CREATE)
		return B_DEV_INVALID_IOCTL;
	if (cookie->device != NULL)
		return B_BUSY;
	if (buffer == NULL || length < sizeof(bluetooth_hid_create_info))
		return B_BAD_VALUE;

	bluetooth_hid_create_info* info
		= (bluetooth_hid_create_info*)malloc(sizeof(*info));
	if (info == NULL)
		return B_NO_MEMORY;
	MemoryDeleter infoDeleter(info);

	if (user_memcpy(info, buffer, sizeof(*info)) != B_OK)
		return B_BAD_ADDRESS;
	if (info->descriptor_length == 0
		|| info->descriptor_length > BLUETOOTH_HID_MAX_DESCRIPTOR)
		return B_BAD_VALUE;

	MutexLocker locker(sDriverLock);

	HIDDevice* device = new(std::nothrow) HIDDevice(info->descriptor,
		info->descriptor_length);
	if (device == NULL)
		return B_NO_MEMORY;
	status_t status = device->InitCheck();
	if (status != B_OK) {
		delete device;
		return status;
	}

	for (uint32 i = 0;; i++) {
		ProtocolHandler* handler = device->ProtocolHandlerAt(i);
		if (handler == NULL)
			break;

		// take the first free name
		char path[B_DEV_NAME_LENGTH];
		for (int32 index = 0;; index++) {
			snprintf(path, sizeof(path), "%s%" B_PRId32, handler->BasePath(),
				index);
			if (gDeviceList->FindDevice(path) == NULL)
				break;
		}

		handler->SetPublishPath(strdup(path));
		gDeviceList->AddDevice(handler->PublishPath(), handler);
		TRACE_ALWAYS("publishing %s\n", path);
	}

	cookie->device = device;

	locker.Unlock();
	republish_devices();
	return B_OK;
}


static status_t
bluetooth_hid_control_read(void* cookie, off_t position, void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_SUPPORTED;
}


static status_t
bluetooth_hid_control_write(void* _cookie, off_t position, const void* buffer,
	size_t* _length)
{
	control_cookie* cookie = (control_cookie*)_cookie;
	size_t length = *_length;

	if (cookie->device == NULL) {
		*_length = 0;
		return B_NO_INIT;
	}
	if (length == 0 || length > BLUETOOTH_HID_MAX_REPORT) {
		*_length = 0;
		return B_BAD_VALUE;
	}

	uint8 report[BLUETOOTH_HID_MAX_REPORT];
	if (user_memcpy(report, buffer, length) != B_OK) {
		*_length = 0;
		return B_BAD_ADDRESS;
	}

	cookie->device->InputReport(report, length);
	return B_OK;
}


//	#pragma mark - published HID devices


static status_t
bluetooth_hid_open(const char* name, uint32 flags, void** _cookie)
{
	TRACE("open(%s, %" B_PRIu32 ", %p)\n", name, flags, _cookie);

	device_cookie* cookie = new(std::nothrow) device_cookie();
	if (cookie == NULL)
		return B_NO_MEMORY;

	MutexLocker locker(sDriverLock);

	ProtocolHandler* handler = (ProtocolHandler*)gDeviceList->FindDevice(name);
	cookie->handler = handler;
	cookie->cookie = 0;

	status_t result = handler == NULL ? B_ENTRY_NOT_FOUND : B_OK;
	if (result == B_OK)
		result = handler->Open(flags, &cookie->cookie);

	if (result != B_OK) {
		delete cookie;
		return result;
	}

	*_cookie = cookie;
	return B_OK;
}


static status_t
bluetooth_hid_read(void* _cookie, off_t position, void* buffer,
	size_t* numBytes)
{
	device_cookie* cookie = (device_cookie*)_cookie;
	return cookie->handler->Read(&cookie->cookie, position, buffer, numBytes);
}


static status_t
bluetooth_hid_write(void* _cookie, off_t position, const void* buffer,
	size_t* numBytes)
{
	device_cookie* cookie = (device_cookie*)_cookie;
	return cookie->handler->Write(&cookie->cookie, position, buffer, numBytes);
}


static status_t
bluetooth_hid_control(void* _cookie, uint32 op, void* buffer, size_t length)
{
	device_cookie* cookie = (device_cookie*)_cookie;
	return cookie->handler->Control(&cookie->cookie, op, buffer, length);
}


static status_t
bluetooth_hid_close(void* _cookie)
{
	device_cookie* cookie = (device_cookie*)_cookie;
	return cookie->handler->Close(&cookie->cookie);
}


static status_t
bluetooth_hid_free(void* _cookie)
{
	device_cookie* cookie = (device_cookie*)_cookie;

	mutex_lock(&sDriverLock);

	HIDDevice* device = cookie->handler->Device();
	if (device->IsOpen()) {
		// another handler of this device is still open
	} else if (device->IsRemoved()) {
		// the connection is gone and none of its handlers is open anymore
		delete device;
	}

	mutex_unlock(&sDriverLock);

	delete cookie;
	return B_OK;
}


//	#pragma mark - driver API


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	gDeviceList = new(std::nothrow) DeviceList();
	if (gDeviceList == NULL)
		return B_NO_MEMORY;

	mutex_init(&sDriverLock, "bluetooth hid driver lock");

	if (gDeviceList->AddDevice(BLUETOOTH_HID_CONTROL_NAME, &sControlDevice)
			!= B_OK) {
		delete gDeviceList;
		gDeviceList = NULL;
		mutex_destroy(&sDriverLock);
		return B_NO_MEMORY;
	}

	return B_OK;
}


void
uninit_driver()
{
	delete gDeviceList;
	gDeviceList = NULL;
	mutex_destroy(&sDriverLock);
}


const char**
publish_devices()
{
	MutexLocker locker(sDriverLock);
	return gDeviceList->PublishDevices();
}


device_hooks*
find_device(const char* name)
{
	static device_hooks controlHooks = {
		bluetooth_hid_control_open,
		bluetooth_hid_control_close,
		bluetooth_hid_control_free,
		bluetooth_hid_control_control,
		bluetooth_hid_control_read,
		bluetooth_hid_control_write,
		NULL,	// select
		NULL	// deselect
	};

	static device_hooks hooks = {
		bluetooth_hid_open,
		bluetooth_hid_close,
		bluetooth_hid_free,
		bluetooth_hid_control,
		bluetooth_hid_read,
		bluetooth_hid_write,
		NULL,	// select
		NULL	// deselect
	};

	MutexLocker locker(sDriverLock);
	void* device = gDeviceList->FindDevice(name);
	if (device == &sControlDevice)
		return &controlHooks;
	if (device == NULL)
		return NULL;
	return &hooks;
}
