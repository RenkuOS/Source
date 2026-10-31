/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef _HID_PROFILE_H_
#define _HID_PROFILE_H_


#include <Message.h>

#include <bluetooth/bluetooth.h>


/*!	The host side of the Bluetooth HID profile: connects the control and the
	interrupt channels of a keyboard, mouse or gamepad and passes its input
	reports to the bluetooth_hid driver, which publishes the device like a USB
	one.
*/
namespace HIDProfile {

status_t	Connect(const bdaddr_t& address, const BMessage& serviceRecord);
status_t	ConnectKnown(const bdaddr_t& address);
status_t	Listen();

int32		BatteryLevel(const bdaddr_t& address);
				// percent, or -1 when the device does not tell

}


#endif	// _HID_PROFILE_H_
