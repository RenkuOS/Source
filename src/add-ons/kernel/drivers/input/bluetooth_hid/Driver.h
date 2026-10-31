/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef _BLUETOOTH_HID_DRIVER_H_
#define _BLUETOOTH_HID_DRIVER_H_


#include <Drivers.h>
#include <KernelExport.h>
#include <OS.h>
#include <util/kernel_cpp.h>

#include "DeviceList.h"


#define DRIVER_NAME			"bluetooth_hid"
#define DEVICE_PATH_SUFFIX	"bluetooth"
#define DEVICE_NAME			"Bluetooth"

extern DeviceList* gDeviceList;


//#define TRACE_BLUETOOTH_HID
#ifdef TRACE_BLUETOOTH_HID
#	define TRACE(x...)		dprintf(DRIVER_NAME ": " x)
#else
#	define TRACE(x...)
#endif
#define ERROR(x...)			dprintf(DRIVER_NAME ": " x)
#define TRACE_ALWAYS(x...)	dprintf(DRIVER_NAME ": " x)
#define CALLED()			TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


#endif	// _BLUETOOTH_HID_DRIVER_H_
