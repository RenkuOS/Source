/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef _HID_DESCRIPTOR_H_
#define _HID_DESCRIPTOR_H_


#include <Message.h>


/*!	What the Bluetooth HID profile reads in the descriptions of a device:
	its report descriptor in the SDP records, and where that descriptor puts
	the battery level.
*/
namespace HIDDescriptor {

struct BatteryReport {
	int32		id;				// -1: none
	uint32		bitOffset;		// after the report ID
	uint32		bitSize;
	int32		maximum;
};


status_t		FindReportDescriptor(const BMessage& serviceRecord,
					const uint8*& descriptor, size_t& length);
BatteryReport	FindBatteryReport(const uint8* descriptor, size_t length);

}


#endif	// _HID_DESCRIPTOR_H_
