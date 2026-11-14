/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef _BLUETOOTH_HID_H_
#define _BLUETOOTH_HID_H_


#include <Drivers.h>


/*!	Interface between the bluetooth_server and the bluetooth_hid driver.

	The server opens the control device once for every connected Bluetooth
	HID device and gives it the report descriptor of the device with
	BLUETOOTH_HID_CREATE. The driver then publishes the keyboards, mice and
	joysticks it finds in the descriptor, like for USB devices. Every write()
	to the control device is one input report, as it comes from the interrupt
	channel without the 0xa1 header byte. Closing the control device removes
	the device.
*/


#define BLUETOOTH_HID_CONTROL_NAME		"input/bluetooth_hid/control"
#define BLUETOOTH_HID_CONTROL_PATH		"/dev/" BLUETOOTH_HID_CONTROL_NAME

#define BLUETOOTH_HID_MAX_DESCRIPTOR	4096
#define BLUETOOTH_HID_MAX_REPORT		1024


enum {
	BLUETOOTH_HID_CREATE = B_DEVICE_OP_CODES_END + 0x4248
};


typedef struct bluetooth_hid_create_info {
	uint32	descriptor_length;
	uint8	descriptor[BLUETOOTH_HID_MAX_DESCRIPTOR];
} bluetooth_hid_create_info;


#endif	// _BLUETOOTH_HID_H_
