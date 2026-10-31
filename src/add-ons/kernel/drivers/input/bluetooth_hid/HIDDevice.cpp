/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */


#include "HIDDevice.h"

#include <stdlib.h>
#include <string.h>

#include <bluetooth_hid.h>

#include "Driver.h"
#include "HIDReport.h"
#include "ProtocolHandler.h"


HIDDevice::HIDDevice(const uint8* descriptor, size_t descriptorLength)
	:
	fStatus(B_NO_INIT),
	fOpenCount(0),
	fRemoved(false),
	fParser(this),
	fProtocolHandlerCount(0),
	fProtocolHandlerList(NULL),
	fReport(NULL)
{
	fReport = (uint8*)malloc(BLUETOOTH_HID_MAX_REPORT);
	if (fReport == NULL) {
		fStatus = B_NO_MEMORY;
		return;
	}

	fStatus = fParser.ParseReportDescriptor(descriptor, descriptorLength);
	if (fStatus != B_OK) {
		ERROR("cannot parse the report descriptor: %s\n", strerror(fStatus));
		return;
	}

	ProtocolHandler::AddHandlers(*this, fProtocolHandlerList,
		fProtocolHandlerCount);
	if (fProtocolHandlerCount == 0) {
		ERROR("no keyboard, mouse or joystick in the report descriptor\n");
		fStatus = B_NOT_SUPPORTED;
	}
}


HIDDevice::~HIDDevice()
{
	// wake up the handler threads still waiting for a report: they find the
	// device removed and end
	fRemoved = true;
	fParser.SetReport(B_DEV_NOT_READY, NULL, 0);

	ProtocolHandler* handler = fProtocolHandlerList;
	while (handler != NULL) {
		ProtocolHandler* next = handler->NextHandler();
		delete handler;
		handler = next;
	}

	free(fReport);
}


status_t
HIDDevice::Open(ProtocolHandler* handler, uint32 flags)
{
	atomic_add(&fOpenCount, 1);
	return B_OK;
}


status_t
HIDDevice::Close(ProtocolHandler* handler)
{
	atomic_add(&fOpenCount, -1);
	return B_OK;
}


void
HIDDevice::Removed()
{
	fRemoved = true;

	// wake up the readers, they find the device removed
	fParser.SetReport(B_DEV_NOT_READY, NULL, 0);
}


status_t
HIDDevice::MaybeScheduleTransfer(HIDReport* report)
{
	if (fRemoved)
		return ENODEV;

	// the reports come when the device sends them
	return B_OK;
}


status_t
HIDDevice::SendReport(HIDReport* report)
{
	// TODO: output reports (keyboard LEDs, rumble) would go back to the
	// bluetooth_server through read() on the control device
	return B_NOT_SUPPORTED;
}


ProtocolHandler*
HIDDevice::ProtocolHandlerAt(uint32 index) const
{
	ProtocolHandler* handler = fProtocolHandlerList;
	while (handler != NULL) {
		if (index == 0)
			return handler;

		handler = handler->NextHandler();
		index--;
	}

	return NULL;
}


void
HIDDevice::InputReport(const uint8* report, size_t length)
{
	if (fRemoved || length == 0 || length > BLUETOOTH_HID_MAX_REPORT)
		return;

	memcpy(fReport, report, length);
	fParser.SetReport(B_OK, fReport, length);
}
