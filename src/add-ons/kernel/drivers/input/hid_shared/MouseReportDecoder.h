/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MOUSE_REPORT_DECODER_H
#define MOUSE_REPORT_DECODER_H


#include <InterfaceDefs.h>
#include <SupportDefs.h>

#include <keyboard_mouse_driver.h>


class HIDCollection;
class HIDReport;
class HIDReportItem;


/*!	Turns the input reports of one HID mouse into mouse movements.

	A mouse is described by a motion report, which holds the relative X and Y
	axes. Buttons, wheel and horizontal pan are usually part of the same report,
	but a device may place them in other input reports of the same collection.
	The decoder keeps the state that spans reports (the pressed buttons and the
	click count), so that it does not matter which report carried a change.

	The decoder only reads what the HID parser has stored. It does not wait for
	reports and does not touch the hardware, which keeps it testable outside of
	the kernel.
*/
class MouseReportDecoder {
public:
								MouseReportDecoder(HIDCollection &collection,
									HIDReport &motionReport);

			//! Did the last packet carry a report of this mouse? Stays true
			//! after Decode(), so ask only after waiting for a packet.
			bool				HasNewData();

			mouse_movement		Decode(bigtime_t timestamp);

			void				SetClickSpeed(bigtime_t clickSpeed);

private:
	static	const uint32		kMaxReports = 4;

			void				_AddControlsOfSiblingReports(
									HIDCollection &collection);
			void				_AddControlsOf(HIDReport &report);
			void				_RememberReport(HIDReport &report);
			void				_UpdateButtonState();
			uint32				_UpdateClickCount(uint32 previousButtons,
									bigtime_t timestamp);

	static	bool				_IsMotionReport(HIDReport &report);
	static	int32				_ValueOf(HIDReportItem *item);

			HIDReport &			fMotionReport;

			HIDReportItem *		fXAxis;
			HIDReportItem *		fYAxis;
			HIDReportItem *		fWheel;
			HIDReportItem *		fHorizontalPan;
			HIDReportItem *		fButtons[B_MAX_MOUSE_BUTTONS];
									// indexed by button usage id - 1

			HIDReport *			fReports[kMaxReports];
									// the motion report and every other report
									// that holds one of the controls above
			uint32				fReportCount;

			uint32				fButtonState;
			uint32				fClickCount;
			bigtime_t			fLastClickTime;
			bigtime_t			fClickSpeed;
};


#endif // MOUSE_REPORT_DECODER_H
