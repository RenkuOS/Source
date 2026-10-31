/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef _BLUETOOTH_HID_DEVICE_H_
#define _BLUETOOTH_HID_DEVICE_H_


#include "HIDParser.h"


class ProtocolHandler;


/*!	A HID device behind a Bluetooth connection. The bluetooth_server owns the
	connection: it gives the report descriptor at creation and then passes
	each input report with InputReport().
*/
class HIDDevice {
public:
								HIDDevice(const uint8* descriptor,
									size_t descriptorLength);
								~HIDDevice();

			status_t			InitCheck() const { return fStatus; }

			bool				IsOpen() const { return fOpenCount > 0; }
			status_t			Open(ProtocolHandler* handler, uint32 flags);
			status_t			Close(ProtocolHandler* handler);
			int32				OpenCount() const { return fOpenCount; }

			void				Removed();
			bool				IsRemoved() const { return fRemoved; }

			status_t			MaybeScheduleTransfer(HIDReport* report);
			status_t			SendReport(HIDReport* report);

			HIDParser&			Parser() { return fParser; }
			ProtocolHandler*	ProtocolHandlerAt(uint32 index) const;

			void				InputReport(const uint8* report, size_t length);

private:
			status_t			fStatus;
			int32				fOpenCount;
			bool				fRemoved;

			HIDParser			fParser;
			uint32				fProtocolHandlerCount;
			ProtocolHandler*	fProtocolHandlerList;

			// the handlers read the last report from here after
			// InputReport() returns
			uint8*				fReport;
};


#endif	// _BLUETOOTH_HID_DEVICE_H_
