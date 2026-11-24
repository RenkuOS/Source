/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */


/*!	Tests for MouseReportDecoder, run in userland on top of the real HID parser.

	Build and run (on Haiku, from the top of the source tree):
		S=src/add-ons/kernel/drivers/input/hid_shared
		T=src/tests/add-ons/kernel/drivers/input/hid_shared
		g++ -std=gnu++17 -DUSERLAND_HID -I $S \
			-I src/add-ons/kernel/drivers/input/usb_hid \
			-I src/bin/hid_decode -I headers/private/input \
			-I headers/private/drivers -I headers/private/kernel \
			-I headers/private $S/MouseReportDecoder.cpp \
			$S/HIDCollection.cpp $S/HIDParser.cpp $S/HIDReport.cpp \
			$S/HIDReportItem.cpp $S/HIDWriter.cpp \
			$T/MouseReportDecoderTest.cpp \
			-o MouseReportDecoderTest -lbe
		./MouseReportDecoderTest
*/


#include "MouseReportDecoder.h"

#include "HIDCollection.h"
#include "HIDParser.h"
#include "HIDReport.h"
#include "HIDReportItem.h"

#include <stdio.h>
#include <string.h>
#include <usb/USB_hid.h>

#include <memory>
#include <vector>


typedef std::vector<uint8> Packet;


// --- Mouse descriptors ------------------------------------------------------

// Boot protocol mouse from the HID specification (appendix B.2): three buttons,
// X and Y, everything in a single report without report ID.
static const uint8 kBootMouseDescriptor[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09,
	0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
	0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30,
	0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
	0xc0, 0xc0
};

// A mouse whose three buttons are two bits wide, with the logical range 0 to 1.
// The values 2 and 3 are outside of that range. No report ID.
static const uint8 kTwoBitButtonsDescriptor[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09,
	0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x02, 0x95, 0x03,
	0x81, 0x02, 0x75, 0x02, 0x95, 0x01, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30,
	0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
	0xc0, 0xc0
};

// A mouse with 16 buttons, X and Y, all in one report without report ID.
static const uint8 kSixteenButtonsDescriptor[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09,
	0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x95, 0x10, 0x75, 0x01,
	0x81, 0x02, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
	0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0
};

// A mouse with X and Y in report 2 and three buttons in report 1, where the
// motion report comes first.
static const uint8 kMotionFirstDescriptor[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x85, 0x02,
	0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02,
	0x81, 0x06, 0x85, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,
	0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x0d,
	0x81, 0x03, 0xc0, 0xc0
};

// Mouse interface of the CHERRY DW 9100 Slim (USB 046a:c114, interface 1),
// read from the device. Buttons and wheel are in report 1, the 12 bit X and Y
// axes are in report 2, report 9 is vendor specific.
static const uint8 kCherryMouseDescriptor[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xa1, 0x00,
	0x95, 0x05, 0x75, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00,
	0x25, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01, 0x75, 0x08,
	0x95, 0x01, 0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x81, 0x06,
	0x05, 0x0c, 0x0a, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, 0xc0, 0x85, 0x02,
	0x09, 0x01, 0xa1, 0x00, 0x75, 0x0c, 0x95, 0x02, 0x05, 0x01, 0x09, 0x30,
	0x09, 0x31, 0x16, 0x01, 0xf8, 0x26, 0xff, 0x07, 0x81, 0x06, 0xc0, 0xc0,
	0x06, 0xa0, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x09, 0x09, 0x01, 0x15,
	0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x07, 0x91, 0x82, 0x09, 0x02,
	0x75, 0x08, 0x95, 0x07, 0x81, 0x82, 0xc0
};


// --- Packets of the CHERRY mouse --------------------------------------------

static const uint8 kCherryButtonsReportID = 1;
static const uint8 kCherryMotionReportID = 2;
static const uint8 kCherryVendorReportID = 9;

static const uint8 kLeftButton = 0x01;
static const uint8 kRightButton = 0x02;


static Packet
cherry_buttons_packet(uint8 buttons, int8 wheel = 0, int8 pan = 0)
{
	return Packet{kCherryButtonsReportID, buttons, (uint8)wheel, (uint8)pan};
}


static Packet
cherry_motion_packet(int x, int y)
{
	// Two signed 12 bit values, X in the low and Y in the high half of 3 bytes.
	uint32 packed = ((uint32)x & 0xfff) | (((uint32)y & 0xfff) << 12);
	return Packet{kCherryMotionReportID, (uint8)(packed & 0xff),
		(uint8)((packed >> 8) & 0xff), (uint8)((packed >> 16) & 0xff)};
}


static Packet
cherry_vendor_packet()
{
	return Packet{kCherryVendorReportID, 0, 0, 0, 0, 0, 0, 0};
}


// --- Test fixture -----------------------------------------------------------

// Far enough from zero that the first press does not count as a quick
// second press.
static const bigtime_t kFirstTimestamp = 10000000;


struct Reception {
	bool			producedMovement;
	mouse_movement	movement;
};


/*!	Wires a parsed descriptor to a MouseReportDecoder the same way the driver
	does and lets the test feed it packets as the hardware would.
*/
class MouseFixture {
public:
	MouseFixture(const uint8 *descriptor, size_t length)
		:
		fParser(NULL)
	{
		if (fParser.ParseReportDescriptor(descriptor, length) != B_OK) {
			fprintf(stderr, "descriptor does not parse\n");
			abort();
		}

		HIDCollection *collection = fParser.RootCollection()->ChildAtFlat(
			COLLECTION_APPLICATION, 0);

		std::vector<HIDReport *> reports(
			fParser.CountReports(HID_REPORT_TYPE_INPUT));
		uint32 reportCount = 0;
		collection->BuildReportList(HID_REPORT_TYPE_INPUT, reports.data(),
			reportCount);

		for (uint32 i = 0; i < reportCount; i++) {
			HIDReportItem *x = reports[i]->FindItem(
				B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_X);
			HIDReportItem *y = reports[i]->FindItem(
				B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_Y);
			if (x != NULL && y != NULL) {
				fDecoder.reset(new MouseReportDecoder(*collection,
					*reports[i]));
				return;
			}
		}

		fprintf(stderr, "descriptor has no mouse\n");
		abort();
	}

	Reception Receive(const Packet &packet,
		bigtime_t timestamp = kFirstTimestamp)
	{
		// The driver's transfer buffer is three bytes larger than the report,
		// and HIDReportItem::Extract() always copies four bytes.
		fLastPacket = packet;
		fLastPacket.resize(packet.size() + 3, 0);
		fParser.SetReport(B_OK, fLastPacket.data(), packet.size());

		Reception reception;
		reception.producedMovement = fDecoder->HasNewData();
		memset(&reception.movement, 0, sizeof(reception.movement));
		if (reception.producedMovement)
			reception.movement = fDecoder->Decode(timestamp);
		return reception;
	}

	MouseReportDecoder &Decoder() { return *fDecoder; }

private:
	HIDParser							fParser;
	std::unique_ptr<MouseReportDecoder>	fDecoder;
	Packet								fLastPacket;
};


// --- Minimal test runner ----------------------------------------------------

static int sFailedChecks = 0;
static int sFailedTests = 0;
static int sPassedTests = 0;


#define CHECK_EQUAL(expected, actual)										\
	do {																	\
		long long expectedValue = (expected);								\
		long long actualValue = (actual);									\
		if (expectedValue != actualValue) {									\
			printf("    %s:%d: expected %s == %lld, got %lld\n", __FILE__,	\
				__LINE__, #actual, expectedValue, actualValue);				\
			sFailedChecks++;												\
		}																	\
	} while (false)

#define CHECK_TRUE(condition)												\
	do {																	\
		if (!(condition)) {													\
			printf("    %s:%d: expected %s\n", __FILE__, __LINE__,			\
				#condition);												\
			sFailedChecks++;												\
		}																	\
	} while (false)

#define RUN_TEST(test)														\
	do {																	\
		int failedBefore = sFailedChecks;									\
		test();																\
		if (sFailedChecks == failedBefore) {								\
			printf("PASS  %s\n", #test);									\
			sPassedTests++;													\
		} else {															\
			printf("FAIL  %s\n", #test);									\
			sFailedTests++;													\
		}																	\
	} while (false)


// --- Tests: mouse with everything in one report (behaviour must not change) --

static void
boot_mouse_reports_motion_and_buttons_from_one_packet()
{
	MouseFixture mouse(kBootMouseDescriptor, sizeof(kBootMouseDescriptor));

	Reception reception = mouse.Receive(Packet{kLeftButton, 5, (uint8)-2});

	CHECK_TRUE(reception.producedMovement);
	CHECK_EQUAL(kLeftButton, reception.movement.buttons);
	CHECK_EQUAL(5, reception.movement.xdelta);
	CHECK_EQUAL(2, reception.movement.ydelta);
		// the Y axis points down in the HID spec and up for the app server
	CHECK_EQUAL(1, reception.movement.clicks);
}


static void
boot_mouse_reports_release()
{
	MouseFixture mouse(kBootMouseDescriptor, sizeof(kBootMouseDescriptor));
	mouse.Receive(Packet{kLeftButton, 0, 0});

	Reception reception = mouse.Receive(Packet{0, 0, 0});

	CHECK_EQUAL(0, reception.movement.buttons);
	CHECK_EQUAL(0, reception.movement.clicks);
}


static void
boot_mouse_counts_quick_second_press_as_double_click()
{
	MouseFixture mouse(kBootMouseDescriptor, sizeof(kBootMouseDescriptor));
	mouse.Receive(Packet{kLeftButton, 0, 0}, 1000000);
	mouse.Receive(Packet{0, 0, 0}, 1050000);

	Reception reception = mouse.Receive(Packet{kLeftButton, 0, 0}, 1100000);

	CHECK_EQUAL(2, reception.movement.clicks);
}


static void
boot_mouse_restarts_click_count_after_the_click_speed()
{
	MouseFixture mouse(kBootMouseDescriptor, sizeof(kBootMouseDescriptor));
	mouse.Decoder().SetClickSpeed(200000);
	mouse.Receive(Packet{kLeftButton, 0, 0}, 1000000);
	mouse.Receive(Packet{0, 0, 0}, 1050000);

	Reception reception = mouse.Receive(Packet{kLeftButton, 0, 0}, 1300000);

	CHECK_EQUAL(1, reception.movement.clicks);
}


// --- Tests: mouse with motion and buttons in different reports --------------

static void
cherry_motion_packet_reports_the_movement()
{
	MouseFixture mouse(kCherryMouseDescriptor, sizeof(kCherryMouseDescriptor));

	Reception reception = mouse.Receive(cherry_motion_packet(10, -3));

	CHECK_TRUE(reception.producedMovement);
	CHECK_EQUAL(10, reception.movement.xdelta);
	CHECK_EQUAL(3, reception.movement.ydelta);
	CHECK_EQUAL(0, reception.movement.buttons);
}


static void
cherry_motion_packet_reports_full_twelve_bit_range()
{
	MouseFixture mouse(kCherryMouseDescriptor, sizeof(kCherryMouseDescriptor));

	Reception reception = mouse.Receive(cherry_motion_packet(-2047, 2047));

	CHECK_EQUAL(-2047, reception.movement.xdelta);
	CHECK_EQUAL(-2047, reception.movement.ydelta);
}


static void
cherry_button_packet_reports_a_pressed_button()
{
	MouseFixture mouse(kCherryMouseDescriptor, sizeof(kCherryMouseDescriptor));

	Reception reception = mouse.Receive(cherry_buttons_packet(kLeftButton));

	CHECK_TRUE(reception.producedMovement);
	CHECK_EQUAL(kLeftButton, reception.movement.buttons);
	CHECK_EQUAL(1, reception.movement.clicks);
	CHECK_EQUAL(0, reception.movement.xdelta);
	CHECK_EQUAL(0, reception.movement.ydelta);
}


static void
cherry_right_button_is_reported_as_the_second_button()
{
	MouseFixture mouse(kCherryMouseDescriptor, sizeof(kCherryMouseDescriptor));

	Reception reception = mouse.Receive(cherry_buttons_packet(kRightButton));

	CHECK_EQUAL(kRightButton, reception.movement.buttons);
}


static void
cherry_button_stays_pressed_while_the_mouse_moves()
{
	// This is what dragging a window looks like.
	MouseFixture mouse(kCherryMouseDescriptor, sizeof(kCherryMouseDescriptor));
	mouse.Receive(cherry_buttons_packet(kLeftButton));

	Reception reception = mouse.Receive(cherry_motion_packet(4, 0));

	CHECK_EQUAL(kLeftButton, reception.movement.buttons);
	CHECK_EQUAL(4, reception.movement.xdelta);
}


static void
cherry_button_release_is_reported()
{
	MouseFixture mouse(kCherryMouseDescriptor, sizeof(kCherryMouseDescriptor));
	mouse.Receive(cherry_buttons_packet(kLeftButton));

	Reception reception = mouse.Receive(cherry_buttons_packet(0));

	CHECK_TRUE(reception.producedMovement);
	CHECK_EQUAL(0, reception.movement.buttons);
	CHECK_EQUAL(0, reception.movement.clicks);
}


static void
cherry_wheel_packet_reports_the_wheel()
{
	MouseFixture mouse(kCherryMouseDescriptor, sizeof(kCherryMouseDescriptor));

	Reception reception = mouse.Receive(cherry_buttons_packet(0, 1, 0));

	CHECK_TRUE(reception.producedMovement);
	CHECK_EQUAL(-1, reception.movement.wheel_ydelta);
	CHECK_EQUAL(0, reception.movement.wheel_xdelta);
}


static void
cherry_horizontal_pan_is_reported()
{
	MouseFixture mouse(kCherryMouseDescriptor, sizeof(kCherryMouseDescriptor));

	Reception reception = mouse.Receive(cherry_buttons_packet(0, 0, -1));

	CHECK_EQUAL(-1, reception.movement.wheel_xdelta);
}


static void
cherry_wheel_is_not_repeated_by_the_next_motion_packet()
{
	MouseFixture mouse(kCherryMouseDescriptor, sizeof(kCherryMouseDescriptor));
	mouse.Receive(cherry_buttons_packet(0, 1, 0));

	Reception reception = mouse.Receive(cherry_motion_packet(1, 1));

	CHECK_EQUAL(0, reception.movement.wheel_ydelta);
}


static void
cherry_two_quick_presses_are_a_double_click()
{
	MouseFixture mouse(kCherryMouseDescriptor, sizeof(kCherryMouseDescriptor));
	mouse.Receive(cherry_buttons_packet(kLeftButton), 1000000);
	mouse.Receive(cherry_buttons_packet(0), 1050000);

	Reception reception = mouse.Receive(cherry_buttons_packet(kLeftButton),
		1100000);

	CHECK_EQUAL(2, reception.movement.clicks);
}


static void
cherry_vendor_packet_produces_no_movement()
{
	MouseFixture mouse(kCherryMouseDescriptor, sizeof(kCherryMouseDescriptor));
	mouse.Receive(cherry_buttons_packet(kLeftButton));

	Reception reception = mouse.Receive(cherry_vendor_packet());

	CHECK_TRUE(!reception.producedMovement);
}


static void
cherry_too_short_buttons_packet_produces_no_movement()
{
	MouseFixture mouse(kCherryMouseDescriptor, sizeof(kCherryMouseDescriptor));

	// The buttons report holds three bytes of data, this packet only one.
	Reception reception = mouse.Receive(Packet{kCherryButtonsReportID, 0x01});

	CHECK_TRUE(!reception.producedMovement);
}


static void
button_value_outside_its_range_counts_as_released()
{
	MouseFixture mouse(kTwoBitButtonsDescriptor,
		sizeof(kTwoBitButtonsDescriptor));
	Reception pressed = mouse.Receive(Packet{0x01, 0, 0});
	CHECK_EQUAL(kLeftButton, pressed.movement.buttons);

	// The logical maximum of a button is 1, so the value 3 is not valid.
	Reception invalid = mouse.Receive(Packet{0x03, 0, 0});

	CHECK_EQUAL(0, invalid.movement.buttons);
}


static void
mouse_reports_buttons_nine_to_sixteen()
{
	MouseFixture mouse(kSixteenButtonsDescriptor,
		sizeof(kSixteenButtonsDescriptor));

	Reception reception = mouse.Receive(Packet{0x00, 0x81, 0, 0});

	CHECK_EQUAL(0x8100, reception.movement.buttons);
}


// --- Tests: what a woken listener sees --------------------------------------

static MouseReportDecoder *sListener = NULL;
static std::vector<mouse_movement> sSeenByListener;


static void
decode_like_a_woken_listener(HIDReport *report)
{
	// The mouse handler waits for the motion report only.
	bool isMotionReport = report->FindItem(B_HID_USAGE_PAGE_GENERIC_DESKTOP,
		B_HID_UID_GD_X) != NULL;
	status_t status = report->ReportStatus();
	bool readsReport = status == B_OK
		|| (status == B_BUSY && sListener->HasNewData());
	if (isMotionReport && readsReport)
		sSeenByListener.push_back(sListener->Decode(kFirstTimestamp));
}


/*!	The buttons report still points at the transfer buffer from the last packet.
	When the motion packet overwrites it, a listener that wakes up must not read
	the buttons from the X byte.
*/
static void
check_listener_sees_no_buttons(MouseFixture &mouse,
	const Packet &buttonsPacket, const Packet &motionPacket)
{
	mouse.Receive(buttonsPacket);
	sListener = &mouse.Decoder();
	sSeenByListener.clear();
	HIDReport::sNotifyHook = decode_like_a_woken_listener;

	mouse.Receive(motionPacket);

	HIDReport::sNotifyHook = NULL;
	CHECK_TRUE(!sSeenByListener.empty());
	for (size_t i = 0; i < sSeenByListener.size(); i++)
		CHECK_EQUAL(0, sSeenByListener[i].buttons);
}


static void
cherry_listener_woken_by_motion_sees_no_buttons_in_old_report()
{
	MouseFixture mouse(kCherryMouseDescriptor, sizeof(kCherryMouseDescriptor));
	check_listener_sees_no_buttons(mouse, cherry_buttons_packet(0),
		cherry_motion_packet(1, 0));
}


static void
listener_sees_no_buttons_in_old_report_when_motion_report_comes_first()
{
	MouseFixture mouse(kMotionFirstDescriptor, sizeof(kMotionFirstDescriptor));
	check_listener_sees_no_buttons(mouse, Packet{1, 0, 0}, Packet{2, 1, 0});
}


static void
cherry_listener_of_the_motion_report_is_woken_by_a_buttons_packet()
{
	MouseFixture mouse(kCherryMouseDescriptor, sizeof(kCherryMouseDescriptor));
	sListener = &mouse.Decoder();
	sSeenByListener.clear();
	HIDReport::sNotifyHook = decode_like_a_woken_listener;

	mouse.Receive(cherry_buttons_packet(kLeftButton));

	HIDReport::sNotifyHook = NULL;
	CHECK_EQUAL(1, sSeenByListener.size());
	if (!sSeenByListener.empty())
		CHECK_EQUAL(kLeftButton, sSeenByListener[0].buttons);
}


int
main()
{
	printf("mouse with everything in one report\n");
	RUN_TEST(boot_mouse_reports_motion_and_buttons_from_one_packet);
	RUN_TEST(boot_mouse_reports_release);
	RUN_TEST(boot_mouse_counts_quick_second_press_as_double_click);
	RUN_TEST(boot_mouse_restarts_click_count_after_the_click_speed);

	printf("mouse with motion and buttons in different reports\n");
	RUN_TEST(cherry_motion_packet_reports_the_movement);
	RUN_TEST(cherry_motion_packet_reports_full_twelve_bit_range);
	RUN_TEST(cherry_button_packet_reports_a_pressed_button);
	RUN_TEST(cherry_right_button_is_reported_as_the_second_button);
	RUN_TEST(cherry_button_stays_pressed_while_the_mouse_moves);
	RUN_TEST(cherry_button_release_is_reported);
	RUN_TEST(cherry_wheel_packet_reports_the_wheel);
	RUN_TEST(cherry_horizontal_pan_is_reported);
	RUN_TEST(cherry_wheel_is_not_repeated_by_the_next_motion_packet);
	RUN_TEST(cherry_two_quick_presses_are_a_double_click);
	RUN_TEST(cherry_vendor_packet_produces_no_movement);
	RUN_TEST(cherry_too_short_buttons_packet_produces_no_movement);

	printf("mouse buttons with values outside their range\n");
	RUN_TEST(button_value_outside_its_range_counts_as_released);
	RUN_TEST(mouse_reports_buttons_nine_to_sixteen);

	printf("what a woken listener sees\n");
	RUN_TEST(cherry_listener_woken_by_motion_sees_no_buttons_in_old_report);
	RUN_TEST(
		listener_sees_no_buttons_in_old_report_when_motion_report_comes_first);
	RUN_TEST(cherry_listener_of_the_motion_report_is_woken_by_a_buttons_packet);

	printf("\n%d passed, %d failed\n", sPassedTests, sFailedTests);
	return sFailedTests == 0 ? 0 : 1;
}
