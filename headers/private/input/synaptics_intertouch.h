/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef _SYNAPTICS_INTERTOUCH_H
#define _SYNAPTICS_INTERTOUCH_H


#include <SupportDefs.h>


/*!	Hand-over of a Synaptics InterTouch touchpad from the PS/2 driver to the
	RMI4 SMBus driver.

	An InterTouch touchpad talks both PS/2 and RMI4 over SMBus, but only one
	of them at a time: SMBus traffic while the PS/2 side is active disturbs
	it. The PS/2 driver identifies the pad, resets and disables it, and posts
	kSynapticsInterTouchHandOff to the hand-over port. Only then does the
	RMI4 driver talk to the pad. It answers on the reply port with
	kSynapticsInterTouchTaken, or with kSynapticsInterTouchFailed, after which
	the PS/2 driver takes the pad back. It also takes it back when no answer
	comes in time.

	Both drivers only do this when "intertouch true" is set in the driver
	settings file of the RMI4 driver,
	~/config/settings/kernel/drivers/rmi4_smbus.
*/


#define SYNAPTICS_INTERTOUCH_SETTINGS		"rmi4_smbus"
#define SYNAPTICS_INTERTOUCH_PARAMETER		"intertouch"

#define SYNAPTICS_INTERTOUCH_HAND_OFF_PORT	"synaptics intertouch hand-off"
#define SYNAPTICS_INTERTOUCH_REPLY_PORT		"synaptics intertouch reply"

enum {
	kSynapticsInterTouchHandOff	= 'SIho',
	kSynapticsInterTouchTaken	= 'SItk',
	kSynapticsInterTouchFailed	= 'SIfl'
};

// flags in synaptics_intertouch_info
#define SYNAPTICS_INTERTOUCH_CLICKPAD			0x01
	// the pad is one big button (extended capability 0x0C bit 4)
#define SYNAPTICS_INTERTOUCH_TRACKSTICK_BUTTONS	0x02
	// the buttons above the pad belong to the TrackPoint (query 0x10 bit 0)

typedef struct {
	uint32	flags;
	uint8	continued_capabilities[3];	// query 0x0C, for the log
} synaptics_intertouch_info;


#endif	// _SYNAPTICS_INTERTOUCH_H
