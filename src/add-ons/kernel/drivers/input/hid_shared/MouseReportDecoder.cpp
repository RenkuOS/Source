/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


#include "MouseReportDecoder.h"

#include "HIDCollection.h"
#include "HIDReport.h"
#include "HIDReportItem.h"

#include <string.h>
#include <usb/USB_hid.h>


static const bigtime_t kDefaultClickSpeed = 250000;


static bool
is_mouse_button(HIDReportItem &item)
{
	return item.UsagePage() == B_HID_USAGE_PAGE_BUTTON
		&& item.UsageID() >= 1 && item.UsageID() <= B_MAX_MOUSE_BUTTONS;
}


MouseReportDecoder::MouseReportDecoder(HIDCollection &collection,
	HIDReport &motionReport)
	:
	fMotionReport(motionReport),

	fXAxis(motionReport.FindItem(B_HID_USAGE_PAGE_GENERIC_DESKTOP,
		B_HID_UID_GD_X)),
	fYAxis(motionReport.FindItem(B_HID_USAGE_PAGE_GENERIC_DESKTOP,
		B_HID_UID_GD_Y)),
	fWheel(NULL),
	fHorizontalPan(NULL),

	fReportCount(0),

	fButtonState(0),
	fClickCount(0),
	fLastClickTime(0),
	fClickSpeed(kDefaultClickSpeed)
{
	memset(fButtons, 0, sizeof(fButtons));

	_RememberReport(motionReport);
	_AddControlsOf(motionReport);
	_AddControlsOfSiblingReports(collection);
}


bool
MouseReportDecoder::HasNewData()
{
	for (uint32 i = 0; i < fReportCount; i++) {
		if (fReports[i]->CurrentReport() != NULL
				&& fReports[i]->ReportStatus() == B_OK) {
			return true;
		}
	}

	return false;
}


mouse_movement
MouseReportDecoder::Decode(bigtime_t timestamp)
{
	uint32 previousButtons = fButtonState;
	_UpdateButtonState();

	mouse_movement movement;
	memset(&movement, 0, sizeof(movement));
	movement.timestamp = timestamp;
	movement.buttons = fButtonState;
	movement.clicks = _UpdateClickCount(previousButtons, timestamp);
	movement.xdelta = _ValueOf(fXAxis);
	movement.ydelta = -_ValueOf(fYAxis);
	movement.wheel_ydelta = -_ValueOf(fWheel);
	movement.wheel_xdelta = _ValueOf(fHorizontalPan);
	return movement;
}


void
MouseReportDecoder::SetClickSpeed(bigtime_t clickSpeed)
{
	fClickSpeed = clickSpeed;
}


/*!	Some mice report their buttons and the wheel in a report of their own,
	next to the report with the axes. Controls that the motion report already
	provides are not taken from these reports.
*/
void
MouseReportDecoder::_AddControlsOfSiblingReports(HIDCollection &collection)
{
	uint32 maxReportCount
		= fMotionReport.Parser()->CountReports(HID_REPORT_TYPE_INPUT);
	HIDReport *reports[maxReportCount];
	uint32 reportCount = 0;
	collection.BuildReportList(HID_REPORT_TYPE_INPUT, reports, reportCount);

	for (uint32 i = 0; i < reportCount; i++) {
		if (reports[i] != &fMotionReport && !_IsMotionReport(*reports[i]))
			_AddControlsOf(*reports[i]);
	}
}


/*!	Takes the buttons, the wheel and the pan of \a report that are still
	missing. The report is remembered if it provides at least one of them.
*/
void
MouseReportDecoder::_AddControlsOf(HIDReport &report)
{
	bool providesControl = false;

	for (uint32 i = 0; i < report.CountItems(); i++) {
		HIDReportItem *item = report.ItemAt(i);
		if (!item->HasData() || !is_mouse_button(*item))
			continue;

		HIDReportItem *&button = fButtons[item->UsageID() - 1];
		if (button == NULL) {
			button = item;
			providesControl = true;
		}
	}

	if (fWheel == NULL) {
		fWheel = report.FindItem(B_HID_USAGE_PAGE_GENERIC_DESKTOP,
			B_HID_UID_GD_WHEEL);
		providesControl |= fWheel != NULL;
	}

	if (fHorizontalPan == NULL) {
		fHorizontalPan = report.FindItem(B_HID_USAGE_PAGE_CONSUMER,
			B_HID_UID_CON_AC_PAN);
		providesControl |= fHorizontalPan != NULL;
	}

	if (providesControl)
		_RememberReport(report);
}


void
MouseReportDecoder::_RememberReport(HIDReport &report)
{
	for (uint32 i = 0; i < fReportCount; i++) {
		if (fReports[i] == &report)
			return;
	}

	if (fReportCount < kMaxReports)
		fReports[fReportCount++] = &report;
}


/*!	Takes over the state of every button whose report has just arrived. Buttons
	in reports that did not arrive keep their state.
*/
void
MouseReportDecoder::_UpdateButtonState()
{
	for (uint32 i = 0; i < B_MAX_MOUSE_BUTTONS; i++) {
		HIDReportItem *button = fButtons[i];
		if (button == NULL || button->Extract() != B_OK)
			continue;

		// A value outside of the logical range counts as released.
		uint32 mask = (uint32)1 << i;
		if (button->Valid() && (button->Data() & 1) != 0)
			fButtonState |= mask;
		else
			fButtonState &= ~mask;
	}
}


/*!	Counts consecutive presses (double click and so on) and returns the click
	count to report. While no button is pressed, the count to report is zero.
*/
uint32
MouseReportDecoder::_UpdateClickCount(uint32 previousButtons,
	bigtime_t timestamp)
{
	if (fButtonState == 0)
		return 0;

	if (previousButtons == 0) {
		bool withinClickSpeed = fLastClickTime + fClickSpeed > timestamp;
		fClickCount = withinClickSpeed ? fClickCount + 1 : 1;
	}

	fLastClickTime = timestamp;
	return fClickCount;
}


bool
MouseReportDecoder::_IsMotionReport(HIDReport &report)
{
	return report.FindItem(B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_X)
			!= NULL
		&& report.FindItem(B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_Y)
			!= NULL;
}


int32
MouseReportDecoder::_ValueOf(HIDReportItem *item)
{
	if (item == NULL || item->Extract() != B_OK || !item->Valid())
		return 0;

	return (int32)item->Data();
}
