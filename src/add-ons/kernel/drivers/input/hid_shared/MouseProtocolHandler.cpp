/*
 * Copyright 2008-2011 Michael Lotz <mmlr@mlotz.ch>
 * Distributed under the terms of the MIT license.
 */


//!	Driver for USB Human Interface Devices.


#include "Driver.h"
#include "MouseProtocolHandler.h"

#include "HIDCollection.h"
#include "HIDDevice.h"
#include "HIDReport.h"
#include "HIDReportItem.h"

#include <new>
#include <string.h>
#include <usb/USB_hid.h>

#include <kernel.h>
#include <keyboard_mouse_driver.h>


MouseProtocolHandler::MouseProtocolHandler(HIDCollection &collection,
	HIDReport &report)
	:
	ProtocolHandler(report.Device(), "input/mouse/" DEVICE_PATH_SUFFIX "/", 0),
	fReport(report),
	fDecoder(collection, report)
{
	TRACE("mouse device, report id: %u\n", report.ID());
}


void
MouseProtocolHandler::AddHandlers(HIDDevice &device, HIDCollection &collection,
	ProtocolHandler *&handlerList)
{
	bool supported = false;
	switch (collection.UsagePage()) {
		case B_HID_USAGE_PAGE_GENERIC_DESKTOP:
		{
			switch (collection.UsageID()) {
				case B_HID_UID_GD_MOUSE:
				case B_HID_UID_GD_POINTER:
					supported = true;
					break;
			}

			break;
		}
	}

	if (!supported) {
		TRACE("collection not a mouse\n");
		return;
	}

	HIDParser &parser = device.Parser();
	uint32 maxReportCount = parser.CountReports(HID_REPORT_TYPE_INPUT);
	if (maxReportCount == 0)
		return;

	uint32 inputReportCount = 0;
	HIDReport *inputReports[maxReportCount];
	collection.BuildReportList(HID_REPORT_TYPE_INPUT, inputReports,
		inputReportCount);

	for (uint32 i = 0; i < inputReportCount; i++) {
		HIDReport *inputReport = inputReports[i];

		// try to find at least an absolute x and y axis
		HIDReportItem *xAxis = inputReport->FindItem(
			B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_X);
		if (xAxis == NULL || !xAxis->Relative())
			continue;

		HIDReportItem *yAxis = inputReport->FindItem(
			B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_Y);
		if (yAxis == NULL || !yAxis->Relative())
			continue;

		ProtocolHandler *newHandler = new(std::nothrow) MouseProtocolHandler(
			collection, *inputReport);
		if (newHandler == NULL) {
			TRACE("failed to allocated mouse protocol handler\n");
			continue;
		}

		newHandler->SetNextHandler(handlerList);
		handlerList = newHandler;
	}
}


status_t
MouseProtocolHandler::Control(uint32 *cookie, uint32 op, void *buffer,
	size_t length)
{
	switch (op) {

		case B_GET_DEVICE_NAME:
		{
			const char name[] = DEVICE_NAME" Mouse";
			return IOGetDeviceName(name,buffer,length);
		}

		case MS_READ:
		{
			if (length < sizeof(mouse_movement))
				return B_BUFFER_OVERFLOW;

			while (true) {
				mouse_movement movement;
				status_t result = _ReadReport(&movement, cookie);
				if (result == B_INTERRUPTED)
					return result;
				if (result == B_BUSY)
					continue;

				if (!IS_USER_ADDRESS(buffer)
					|| user_memcpy(buffer, &movement, sizeof(movement))
						!= B_OK) {
					return B_BAD_ADDRESS;
				}

				return result;
			}
		}

		case MS_NUM_EVENTS:
		{
			if (fReport.Device()->IsRemoved())
				return B_DEV_NOT_READY;

			// we are always on demand, so 0 queued events
			return 0;
		}

		case MS_SET_CLICKSPEED:
		{
			bigtime_t clickSpeed;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&clickSpeed, buffer, sizeof(bigtime_t))
					!= B_OK) {
				return B_BAD_ADDRESS;
			}

			fDecoder.SetClickSpeed(clickSpeed);
			return B_OK;
		}
	}

	return B_ERROR;
}


status_t
MouseProtocolHandler::_ReadReport(void *buffer, uint32 *cookie)
{
	status_t result = fReport.WaitForReport(B_INFINITE_TIMEOUT);

	// "busy" means that another report came in on the same input. For a mouse
	// whose buttons are in a report of their own, that is the report to read.
	bool ownReportArrived = result == B_OK;
	bool otherReportArrived = result == B_BUSY && fDecoder.HasNewData();

	if (!ownReportArrived && !otherReportArrived) {
		if (fReport.Device()->IsRemoved()) {
			TRACE("device has been removed\n");
			return B_DEV_NOT_READY;
		}

		if (result == B_INTERRUPTED)
			return result;
		if ((*cookie & PROTOCOL_HANDLER_COOKIE_FLAG_CLOSED) != 0)
			return B_FILE_ERROR;

		if (result != B_BUSY) {
			// "busy" happens when other reports come in on the same input as
			// ours
			TRACE_ALWAYS("error waiting for report: %s\n", strerror(result));
		}

		// signal that we simply want to try again
		return B_BUSY;
	}

	*(mouse_movement *)buffer = fDecoder.Decode(system_time());

	if (ownReportArrived)
		fReport.DoneProcessing();

	TRACE("got mouse report\n");
	return B_OK;
}
