/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Distributed under the terms of the MIT license.
 *
 */

#include "Driver.h"

#include <AutoLock.h>
#include <KernelExport.h>	// dprintf, for the unconditional Media-OS banner
#include <usb/USB_audio.h>

#include "Device.h"
#include "Settings.h"


static const char* sDeviceBaseName = "audio/hmulti/usb/";

usb_module_info* gUSBModule = NULL;

Device* gDevices[MAX_DEVICES];
char* gDeviceNames[MAX_DEVICES + 1];

mutex gDriverLock;
int32 api_version = B_CUR_DRIVER_API_VERSION;


status_t
usb_audio_device_added(usb_device device, void** cookie)
{
	*cookie = NULL;

	MutexLocker _(gDriverLock);

	// check if this is a replug of an existing device first
	for (int32 i = 0; i < MAX_DEVICES; i++) {
		if (gDevices[i] == NULL)
			continue;

		if (gDevices[i]->CompareAndReattach(device) != B_OK)
			continue;

		TRACE(INF, "The device is plugged back. Use entry at %ld.\n", i);
		*cookie = gDevices[i];
		return B_OK;
	}

	// no such device yet, create a new one
	Device* audioDevice = new(std::nothrow) Device(device);
	if (audioDevice == 0)
		return ENODEV;

	status_t status = audioDevice->InitCheck();
	if (status < B_OK) {
		delete audioDevice;
		return status;
	}

	status = audioDevice->SetupDevice(false);
	if (status < B_OK) {
		delete audioDevice;
		return status;
	}

	// Assign a per-model instance index so two identical devices are told
	// apart in Media preferences: the first keeps the plain product name,
	// later ones render " #2", " #3"... Pick the lowest index not already
	// used by a live device with the same product name, so unplugging one
	// frees its number. (audioDevice is not in gDevices yet: no self-match.)
	int32 instance = 0;
	bool collision = true;
	while (collision) {
		collision = false;
		for (int32 i = 0; i < MAX_DEVICES; i++) {
			if (gDevices[i] == NULL)
				continue;
			if (strcmp(gDevices[i]->ProductName(),
					audioDevice->ProductName()) == 0
				&& gDevices[i]->InstanceIndex() == instance) {
				instance++;
				collision = true;
				break;
			}
		}
	}
	audioDevice->SetInstanceIndex(instance);

	for (int32 i = 0; i < MAX_DEVICES; i++) {
		if (gDevices[i] != NULL)
			continue;

		gDevices[i] = audioDevice;
		*cookie = audioDevice;

		TRACE(INF, "New device is added at %ld.\n", i);
		return B_OK;
	}

	// no space for the device
	TRACE(ERR, "Error: no more device entries availble.\n");

	delete audioDevice;
	return B_ERROR;
}


status_t
usb_audio_device_removed(void* cookie)
{
	MutexLocker _(gDriverLock);

	Device* device = (Device*)cookie;
	for (int32 i = 0; i < MAX_DEVICES; i++) {
		if (gDevices[i] == device) {
			if (device->IsOpen()) {
				// Keep the entry: a replug rebinds THIS object - and with it
				// the sample buffers already published to the media node -
				// through CompareAndReattach(). It is retired in the free
				// hook instead, once devfs releases the last cookie. The
				// comment that used to sit here claimed as much, but nothing
				// implemented it: Device::Free() returned B_OK without
				// deleting anything, so a device unplugged while open leaked,
				// and one unplugged while closed was deleted straight out from
				// under any fd that still had it as a cookie.
				device->Removed();
			} else {
				gDevices[i] = NULL;
				delete device;
				TRACE(INF, "Device at %ld deleted.\n", i);
			}
			break;
		}
	}

	return B_OK;
}


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	status_t status = get_module(B_USB_MODULE_NAME,
		(module_info**)&gUSBModule);
	if (status < B_OK)
		return status;

	load_settings();

	TRACE(INF, "%s\n", kVersion);

	for (int32 i = 0; i < MAX_DEVICES; i++)
		gDevices[i] = NULL;

	gDeviceNames[0] = NULL;
	mutex_init(&gDriverLock, DRIVER_NAME "_devices");

	static usb_notify_hooks notifyHooks = {
		&usb_audio_device_added,
		&usb_audio_device_removed
	};

	static usb_support_descriptor supportedDevices[] = {
		{ USB_AUDIO_INTERFACE_AUDIO_CLASS, 0, 0, 0, 0 }
	};

	gUSBModule->register_driver(DRIVER_NAME, supportedDevices, 0, NULL);
	gUSBModule->install_notify(DRIVER_NAME, &notifyHooks);
	return B_OK;
}


void
uninit_driver()
{
	gUSBModule->uninstall_notify(DRIVER_NAME);
	mutex_lock(&gDriverLock);

	for (int32 i = 0; i < MAX_DEVICES; i++) {
		if (gDevices[i]) {
			delete gDevices[i];
			gDevices[i] = NULL;
		}
	}

	for (int32 i = 0; gDeviceNames[i]; i++) {
		free(gDeviceNames[i]);
		gDeviceNames[i] = NULL;
	}

	mutex_destroy(&gDriverLock);
	put_module(B_USB_MODULE_NAME);

	release_settings();
}


static status_t
usb_audio_open(const char* name, uint32 flags, void** cookie)
{
	MutexLocker _(gDriverLock);

	*cookie = NULL;

	// Match the name against the slot it encodes, rather than trusting
	// gDeviceNames[i] to describe gDevices[i]. publish_devices() names an
	// entry after its gDevices slot but packs the name array, so any hole -
	// one device removed while another stays - makes the two arrays disagree.
	// The old loop also used "gDevices[i] != NULL" as its condition, so it
	// stopped at the first hole entirely and the surviving device could not be
	// opened at all.
	for (size_t i = 0; i < MAX_DEVICES; i++) {
		if (gDevices[i] == NULL)
			continue;

		char deviceName[32];
		snprintf(deviceName, sizeof(deviceName), "%s%ld", sDeviceBaseName,
			i + 1);
		if (strcmp(deviceName, name) != 0)
			continue;

		status_t status = gDevices[i]->Open(flags);
		if (status == B_OK)
			*cookie = gDevices[i];
		return status;
	}

	return ENODEV;
}


static status_t
usb_audio_read(void* cookie, off_t position, void* buffer, size_t* numBytes)
{
	Device* device = (Device*)cookie;
	return device->Read((uint8*)buffer, numBytes);
}


static status_t
usb_audio_write(void* cookie, off_t position, const void* buffer,
	size_t* numBytes)
{
	Device* device = (Device*)cookie;
	return device->Write((const uint8*)buffer, numBytes);
}


static status_t
usb_audio_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	Device* device = (Device*)cookie;
	return device->Control(op, buffer, length);
}


static status_t
usb_audio_close(void* cookie)
{
	Device* device = (Device*)cookie;
	return device->Close();
}


static status_t
usb_audio_free(void* cookie)
{
	MutexLocker _(gDriverLock);

	Device* device = (Device*)cookie;

	// This is the last reference devfs holds. A device that was unplugged
	// while open was kept alive by usb_audio_device_removed() so a replug
	// could rebind it; with the fd gone, that can no longer happen, so retire
	// it here. (A device that came back before the fd closed has cleared
	// fRemoved in CompareAndReattach and is left alone - it is merely closed,
	// not gone.)
	if (device->IsRemoved()) {
		for (size_t i = 0; i < MAX_DEVICES; i++) {
			if (gDevices[i] == device) {
				gDevices[i] = NULL;
				TRACE(INF, "Removed device at %ld retired on free.\n", i);
				break;
			}
		}

		delete device;
		return B_OK;
	}

	return device->Free();
}


const char**
publish_devices()
{
	MutexLocker _(gDriverLock);

	for (int32 i = 0; gDeviceNames[i]; i++) {
		free(gDeviceNames[i]);
		gDeviceNames[i] = NULL;
	}

	int32 deviceCount = 0;
	for (size_t i = 0; i < MAX_DEVICES; i++) {
		// A removed device keeps its slot while it is still open, so that a
		// replug can rebind this same object through CompareAndReattach().
		// CompareAndReattach() clears fRemoved, so a replug publishes it again.
		//
		// KEEP PUBLISHING A REMOVED DEVICE UNTIL IT IS CLOSED. Dropping it
		// while an fd is still open makes republish_driver() unpublish the
		// node, and devfs's LegacyDevice::Removed() does `delete this` - while
		// devfs_free_cookie() will still make a virtual call through that same
		// pointer when the fd finally closes. That is a use-after-free: it
		// KDLs with a page fault whose address moves between runs, because it
		// jumps through a recycled vtable.
		//
		// Publishing it while open inverts the order safely: multi_audio sees
		// B_CANCELED from the exchange, closes, usb_audio_free() retires the
		// slot, and only THEN does the entry leave this list - so devfs
		// unpublishes a device nothing holds open.
		//
		// This deliberately relaxes 1a6c661, which stopped republishing removed
		// devices to break a circular wait: the add-on decided a device was
		// gone purely by its path vanishing, so republishing meant it never
		// noticed and never closed. That is no longer true. MultiAudioNode's
		// output thread now checks the exchange result and stops on B_CANCELED
		// (it previously discarded the return value entirely), so it has an
		// error-driven exit that does not depend on the path. THE TWO CHANGES
		// ARE A PAIR - reverting either one alone brings back a bug: without
		// the add-on's check, this deadlocks; without this, that use-after-free
		// returns. See docs/usb-audio-removal-hang.md.
		if (gDevices[i] == NULL)
			continue;
		if (gDevices[i]->IsRemoved() && !gDevices[i]->IsOpen())
			continue;

		gDeviceNames[deviceCount] = (char*)malloc(strlen(sDeviceBaseName) + 4);
		if (gDeviceNames[deviceCount]) {
			sprintf(gDeviceNames[deviceCount], "%s%ld", sDeviceBaseName, i + 1);
			TRACE(INF, "publishing %s\n", gDeviceNames[deviceCount]);
			deviceCount++;
		} else
			TRACE(ERR, "Error: out of memory during allocating device name.\n");
	}

	gDeviceNames[deviceCount] = NULL;
	return (const char**)&gDeviceNames[0];
}


device_hooks*
find_device(const char* name)
{
	static device_hooks deviceHooks = {
		usb_audio_open,
		usb_audio_close,
		usb_audio_free,
		usb_audio_control,
		usb_audio_read,
		usb_audio_write,
		NULL,				// select
		NULL				// deselect
	};

	return &deviceHooks;
}

