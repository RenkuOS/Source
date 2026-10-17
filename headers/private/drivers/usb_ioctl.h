/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef _USB_IOCTL_H
#define _USB_IOCTL_H


/*!	usb_module_info::usb_ioctl() opcode: the speed a device is actually
	CONNECTED at, which the USB driver API otherwise has no way to ask.

	In:  the buffer holds the device's usb_id (a usb_device), as for 'DNAM'.
	Out: the same uint32 is overwritten with the bus manager's usb_speed:
	     0 low, 1 full, 2 high, 3 SuperSpeed, 4 SuperSpeed+.
	bufferSize must be at least sizeof(uint32).

	A bus manager that predates it answers B_DEV_INVALID_IOCTL; callers must
	keep a fallback. Why it is needed: descriptors only say what a device is
	CAPABLE of, so a high-speed device connected at full speed (behind a USB
	1.1 hub, or on a port whose companion controller took it) looks high
	speed; and the descriptor-based check sends GET_DESCRIPTOR(
	DEVICE_QUALIFIER), which a full-speed-only device must STALL.

	The value is the one Media-OS uses for the same query, so drivers built
	against either bus manager understand each other.
*/
#define USB_IOCTL_GET_DEVICE_SPEED	'MSPD'


#endif	/* _USB_IOCTL_H */
