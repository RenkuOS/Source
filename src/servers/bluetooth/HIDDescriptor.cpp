/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "HIDDescriptor.h"


// SDP attribute HIDDescriptorList and its report descriptor type
#define HID_DESCRIPTOR_LIST		"0x0206"
#define HID_DESCRIPTOR_REPORT	0x22


/*!	Reads the header of an SDP data element: \a type gets the type, \a length
	the length of its data, which starts at the returned position.
*/
static const uint8*
ReadElement(const uint8* data, const uint8* end, uint8& type, size_t& length)
{
	if (data >= end)
		return NULL;

	type = data[0] >> 3;
	uint8 sizeIndex = data[0] & 0x7;
	data++;

	if (type == 0) {
		// nil
		length = 0;
		return data;
	}

	switch (sizeIndex) {
		case 0: case 1: case 2: case 3: case 4:
			length = 1 << sizeIndex;
			break;
		case 5:
			if (end - data < 1)
				return NULL;
			length = data[0];
			data += 1;
			break;
		case 6:
			if (end - data < 2)
				return NULL;
			length = (data[0] << 8) | data[1];
			data += 2;
			break;
		default:
			if (end - data < 4)
				return NULL;
			length = ((uint32)data[0] << 24) | (data[1] << 16) | (data[2] << 8)
				| data[3];
			data += 4;
			break;
	}

	if ((size_t)(end - data) < length)
		return NULL;
	return data;
}


/*!	Finds the report descriptor in the HIDDescriptorList, a sequence of
	sequences of a descriptor type (uint8) and the descriptor (string). The
	attribute holds the content of the outer sequence.
*/
status_t
HIDDescriptor::FindReportDescriptor(const BMessage& record, const uint8*& descriptor,
	size_t& descriptorLength)
{
	const void* list;
	ssize_t listSize;
	if (record.FindData(HID_DESCRIPTOR_LIST, B_RAW_TYPE, &list, &listSize)
			!= B_OK)
		return B_NAME_NOT_FOUND;

	const uint8* data = (const uint8*)list;
	const uint8* end = data + listSize;
	while (data < end) {
		uint8 type;
		size_t length;
		const uint8* item = ReadElement(data, end, type, length);
		if (item == NULL || type != 6)
			return B_BAD_DATA;
		data = item + length;

		// the descriptor type
		const uint8* itemEnd = item + length;
		size_t valueLength;
		const uint8* value = ReadElement(item, itemEnd, type, valueLength);
		if (value == NULL || type != 1 || valueLength != 1)
			continue;
		uint8 descriptorType = value[0];

		// the descriptor
		value = ReadElement(value + valueLength, itemEnd, type, valueLength);
		if (value == NULL || type != 4)
			continue;

		if (descriptorType == HID_DESCRIPTOR_REPORT) {
			descriptor = value;
			descriptorLength = valueLength;
			return B_OK;
		}
	}

	return B_NAME_NOT_FOUND;
}


/*!	Looks for the battery strength (usage page Generic Device Controls,
	usage Battery Strength) among the input items of a report descriptor.
*/
HIDDescriptor::BatteryReport
HIDDescriptor::FindBatteryReport(const uint8* descriptor, size_t length)
{
	BatteryReport battery = { -1, 0, 0, 0 };

	uint32 usagePage = 0;
	int32 reportID = 0;
	uint32 reportSize = 0;
	uint32 reportCount = 0;
	int32 logicalMaximum = 0;
	uint32 usages[16];
	int32 usageCount = 0;
	// the bits used so far in every report, by report ID
	uint32 offsets[256] = {};

	size_t i = 0;
	while (i < length) {
		uint8 prefix = descriptor[i];
		if (prefix == 0xfe) {
			// long item
			if (i + 1 >= length)
				break;
			i += 3 + descriptor[i + 1];
			continue;
		}

		uint32 size = prefix & 0x3;
		if (size == 3)
			size = 4;
		if (i + 1 + size > length)
			break;

		uint32 data = 0;
		for (uint32 b = 0; b < size; b++)
			data |= (uint32)descriptor[i + 1 + b] << (8 * b);
		int32 signedData = data;
		if (size == 1)
			signedData = (int8)data;
		else if (size == 2)
			signedData = (int16)data;

		switch (prefix & 0xfc) {
			case 0x04:	// usage page
				usagePage = data;
				break;
			case 0x24:	// logical maximum
				logicalMaximum = size < 4 && signedData < 0
					? (int32)data : signedData;
				break;
			case 0x74:	// report size
				reportSize = data;
				break;
			case 0x84:	// report ID
				reportID = data & 0xff;
				break;
			case 0x94:	// report count
				reportCount = data;
				break;
			case 0x08:	// usage
				if (usageCount < 16)
					usages[usageCount++] = data;
				break;
			case 0x80:	// input
			{
				for (int32 u = 0; u < usageCount; u++) {
					if (usagePage == 0x06 && usages[u] == 0x20
						&& battery.id < 0) {
						battery.id = reportID;
						battery.bitOffset = offsets[reportID]
							+ u * reportSize;
						battery.bitSize = reportSize;
						battery.maximum = logicalMaximum;
					}
				}
				offsets[reportID] += reportSize * reportCount;
				usageCount = 0;
				break;
			}
			case 0x90:	// output
			case 0xb0:	// feature
			case 0xa0:	// collection
			case 0xc0:	// end collection
				usageCount = 0;
				break;
		}

		i += 1 + size;
	}

	return battery;
}
