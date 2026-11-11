/*
 * Copyright 2008-2010, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors (in chronological order):
 *		Clemens Zeidler (haiku@Clemens-Zeidler.de)
 *		Axel Dörfler, axeld@pinc-software.de
 */


//!	PS/2 synaptics touchpad


#include "ps2_synaptics.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <keyboard_mouse_driver.h>

#include "ps2_service.h"


//#define TRACE_PS2_SYNAPTICS
#ifdef TRACE_PS2_SYNAPTICS
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...)
#endif

/*! The raw movement calculation is calibrated on ths synaptics touchpad. */
// increase the touchpad size a little bit
#define SYN_AREA_TOP_LEFT_OFFSET	40
#define SYN_AREA_BOTTOM_RIGHT_OFFSET	60
#define SYN_AREA_START_X		(1472 - SYN_AREA_TOP_LEFT_OFFSET)
#define SYN_AREA_END_X			(5472 + SYN_AREA_BOTTOM_RIGHT_OFFSET)
#define SYN_AREA_START_Y		(1408 - SYN_AREA_TOP_LEFT_OFFSET)
#define SYN_AREA_END_Y			(4448 + SYN_AREA_BOTTOM_RIGHT_OFFSET)

// synaptics touchpad proportions
#define SYN_EDGE_MOTION_WIDTH	50
#define SYN_AREA_OFFSET			40

#define MIN_PRESSURE			30
#define REAL_MAX_PRESSURE		100
#define MAX_PRESSURE			200

enum {
	kIdentify = 0x00,
	kReadModes = 0x01,
	kReadCapabilities = 0x02,
	kReadModelId = 0x03,
	kReadSerialNumberPrefix = 0x06,
	kReadSerialModelSuffix = 0x07,
	kReadResolutions = 0x08,
	kExtendedModelId = 0x09,
	kContinuedCapabilities = 0x0C,
	kMaximumCoordinates = 0x0D,
	kDeluxeLedInfo = 0x0E,
	kMinimumCoordinates = 0x0F,
	kTrackpointQuirk = 0x10
};

static touchpad_specs gHardwareSpecs;


typedef struct {
	uint8 majorVersion;
	uint8 minorVersion;

	uint8 nExtendedButtons;
	uint8 firstExtendedButton;
	uint8 extendedButtonsState;

	bool capExtended : 1;
	bool capMiddleButton : 1;
	bool capSleep : 1;
	bool capFourButtons : 1;
	bool capMultiFinger : 1;
	bool capMultiFingerReport : 1;
	bool capPalmDetection : 1;
	bool capPassThrough : 1;
	bool capAdvancedGestures : 1;
	bool capExtendedWMode : 1;
	bool capClickPadUniform : 1;
	int  capClickPadButtonCount : 2;
} touchpad_info;


const char* kSynapticsPath[4] = {
	"input/touchpad/ps2/synaptics_0",
	"input/touchpad/ps2/synaptics_1",
	"input/touchpad/ps2/synaptics_2",
	"input/touchpad/ps2/synaptics_3"
};


static touchpad_info sTouchpadInfo;
static ps2_dev *sPassthroughDevice = &ps2_device[PS2_DEVICE_SYN_PASSTHROUGH];

// When the guest last ran its self-test (sent AA), and how many self-test
// announces arrived in place of an ack or a reply (the "bounce": the pad
// synthesizes AA 00 ~84 ms after delivering a byte it could not handle).
static bigtime_t sGuestSelfTestTime;
static int32 sGuestBounces;
// A real ack is in within ~4 ms, the pad's synthetic AA 00 within ~90 ms.
static const bigtime_t kPassthroughReplyTimeout = 300000;

// Asynchronous pass-through: the guest's reply bytes arrive wrapped in
// pass-through packets through the interrupt path, one guest byte per
// packet (byte 1), while a command is in flight; only an enabled, idle
// guest streams 3-byte motion data (packet bytes 1, 4 and 5). The pad
// forwards the guest's bytes only while it is disabled itself, so commands
// are wrapped in pad disable/enable.
static mutex sGuestCommandLock = MUTEX_INITIALIZER("ps2 guest command");
static sem_id sGuestReplySem = -1;
static uint8 sGuestReplyBuffer[16];
static int32 sGuestReplyWrite;
static int32 sGuestReplyRead;
static int32 sGuestCommandInFlight;
static int32 sGuestReinits;


static bool
ext_buttons_use_guest()
{
	// ThinkPad quirk (firstExtendedButton == 0): the buttons above the pad
	// physically belong to the trackpoint. Report them through the
	// pass-through device when one is published, as Linux does, so that
	// holding a button and moving the stick looks like a drag coming from
	// a single device.
	return sTouchpadInfo.firstExtendedButton == 0
		&& sPassthroughDevice->active
		&& sPassthroughDevice->handle_int != NULL;
}


// interrupt context
static void
synaptics_ext_buttons_changed(const uint8 packet[])
{
	uint8 state = 0;
	for (int button = 0; button < sTouchpadInfo.nExtendedButtons; button++) {
		// Even buttons are in the X byte, odd buttons in the Y byte. For the
		// ThinkPad buttons this yields PS/2 button order: left, right,
		// middle.
		if ((packet[4 + button % 2] >> (button / 2) & 0x1) != 0)
			state |= 1 << button;
	}

	if (state == sTouchpadInfo.extendedButtonsState)
		return;
	sTouchpadInfo.extendedButtonsState = state;

	if (!ext_buttons_use_guest())
		return;

	TRACE("SYNAPTICS: stick buttons 0x%02x -> pass-through device\n", state);

	// The state is OR-ed into the buttons of every guest packet; a synthetic
	// motionless packet makes the change take effect while the stick is not
	// moving. The guest's packet index is 0 here: regular and wrapped guest
	// packets only complete whole.
	ps2_dev *guest = sPassthroughDevice;
	int32 (*handleInt)(ps2_dev*) = guest->handle_int;
	guest->extra_buttons = state & 7;
	for (size_t i = 0; i < guest->packet_size; i++) {
		guest->history[0].data = i == 0 ? 0x08 : 0;
		handleInt(guest);
	}
}


// interrupt context
static void
guest_reply_push(uint8 data)
{
	if (sGuestReplySem < 0)
		return;
	int32 write = atomic_get(&sGuestReplyWrite);
	int32 next = (write + 1) % (int32)sizeof(sGuestReplyBuffer);
	if (next == atomic_get(&sGuestReplyRead))
		return;
	sGuestReplyBuffer[write] = data;
	atomic_set(&sGuestReplyWrite, next);
	release_sem_etc(sGuestReplySem, 1, B_DO_NOT_RESCHEDULE);
}


static status_t
guest_reply_pop(uint8 *data, bigtime_t timeout)
{
	status_t status = acquire_sem_etc(sGuestReplySem, 1, B_RELATIVE_TIMEOUT,
		timeout);
	if (status != B_OK)
		return status;
	int32 read = atomic_get(&sGuestReplyRead);
	*data = sGuestReplyBuffer[read];
	atomic_set(&sGuestReplyRead,
		(read + 1) % (int32)sizeof(sGuestReplyBuffer));
	return B_OK;
}


// A stalled pad delivers its packet bytes ~10 ms apart (one packet every
// ~550 ms) and the guest goes dead with it; a guest reset-disable + enable
// recovers it (see synaptics_stall_probe()).
static bigtime_t sPacketStart;
static bigtime_t sStallQuietUntil;
static int32 sSlowPackets;
static int32 sStallRecoveries;
static bool sStallPending;


// interrupt context, on every completed pad packet
static void
check_stall(void)
{
	bigtime_t now = system_time();
	// normal packets take 7-11 ms, stalled ones ~56 ms
	bigtime_t span = now - sPacketStart;
	if (sPassthroughDevice->active && span > 30000 && now > sStallQuietUntil) {
		if (++sSlowPackets >= 3 && !sStallPending && sStallRecoveries < 3) {
			sStallPending = true;
			sStallRecoveries++;
			INFO("SYNAPTICS: pad stall detected (packet span %" B_PRIdBIGTIME
				" us), recovering\n", span);
			ps2_service_notify_stall_probe(sPassthroughDevice);
		}
	} else
		sSlowPackets = 0;
}


static status_t
send_touchpad_arg_timeout(ps2_dev *dev, uint8 arg, bigtime_t timeout)
{
	int8 i;
	uint8 val[8];

	for (i = 0; i < 4; i++) {
		val[2 * i] = (arg >> (6 - 2 * i)) & 3;
		val[2 * i + 1] = 0xE8;
	}
	return ps2_dev_command_timeout(dev, 0xE8, val, 7, NULL, 0, timeout);
}


static status_t
send_touchpad_arg(ps2_dev *dev, uint8 arg)
{
	return send_touchpad_arg_timeout(dev, arg, 4000000);
}


static status_t
set_touchpad_mode(ps2_dev *dev, uint8 mode)
{
	uint8 sample_rate = SYN_CHANGE_MODE;
	status_t status = send_touchpad_arg(dev, mode);
	if (status != B_OK)
		return status;
	return ps2_dev_command(dev, PS2_CMD_SET_SAMPLE_RATE, &sample_rate, 1,
		NULL, 0);
}


static status_t
get_information_query(ps2_dev *dev, uint8 extendedQueries, uint8 query,
	uint8 val[3])
{
	if (query == kTrackpointQuirk) {
		// Special case: this information query is not reported in the
		// "extended queries", but is still supported when the touchpad has
		// a pass-through port.
		if (!sTouchpadInfo.capPassThrough)
			return B_NOT_SUPPORTED;
	} else if (query > extendedQueries + 8)
		return B_NOT_SUPPORTED;

	status_t error = send_touchpad_arg(dev, query);
	if (error != B_OK)
		return error;
	return ps2_dev_command(dev, 0xE9, NULL, 0, val, 3);
}


static status_t
get_synaptics_movment(synaptics_cookie *cookie, touchpad_movement *_event, bigtime_t timeout)
{
	status_t status;
	touchpad_movement event = {};
	uint8 event_buffer[PS2_MAX_PACKET_SIZE];
	uint8 wValue0, wValue1, wValue2, wValue3, wValue, vValue;
	uint32 val32;
	uint32 xTwelfBit, yTwelfBit;

	status = acquire_sem_etc(cookie->synaptics_sem, 1, B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT,
		timeout);
	if (status < B_OK)
		return status;

	if (!cookie->dev->active) {
		TRACE("SYNAPTICS: read_event: Error device no longer active\n");
		return B_ERROR;
	}

	if (packet_buffer_read(cookie->synaptics_ring_buffer, event_buffer,
			cookie->dev->packet_size) != cookie->dev->packet_size) {
		TRACE("SYNAPTICS: error copying buffer\n");
		return B_ERROR;
	}

	event.buttons = event_buffer[0] & 3;
 	event.zPressure = event_buffer[2];
	bool isExtended = false;

 	if (sTouchpadInfo.capExtended) {
 		wValue0 = event_buffer[3] >> 2 & 1;
	 	wValue1 = event_buffer[0] >> 2 & 1;
 		wValue2 = event_buffer[0] >> 4 & 1;
	 	wValue3 = event_buffer[0] >> 5 & 1;

 		wValue = wValue0;
 		wValue = wValue | (wValue1 << 1);
 		wValue = wValue | (wValue2 << 2);
 		wValue = wValue | (wValue3 << 3);


		TRACE("SYNAPTICS: received packet %02x %02x %02x %02x %02x %02x -> W = %d\n",
			event_buffer[0], event_buffer[1], event_buffer[2], event_buffer[3], event_buffer[4],
			event_buffer[5], wValue);

		if (event.zPressure < MIN_PRESSURE)
			event.fingers = 0;
		else if (wValue <= 1) {
			// Multiple fingers detected, report the finger count.
			event.fingers = (1 << (2 + wValue)) - 1;

			// There is a separate "v" value indicating the finger width.
			// Keep it apart from W: W is tested below, and a V of 2 would
			// make this packet look like an extended W packet.
			vValue = (event_buffer[3] >> 1 & 1)
				| (event_buffer[4] >> 1 & 1) << 1
				| (event_buffer[2] & 1) << 2;

			event.fingerWidth = vValue * 2;
		} else if (wValue >= 4) {
			// wValue = 4 to 15 corresponds to a finger width report for a single finger
			event.fingerWidth = wValue;
			event.fingers = 1;
		}
		// wValue = 2 - Extended W mode
		// wValue = 3 - Packet from pass-through device
	 	event.gesture = false;

		if (sTouchpadInfo.capExtendedWMode && wValue == 2) {
			// The extended W can be encoded on 4 bits (values 0-7) or 8 bits (80-FF)
			uint8_t extendedWValue = event_buffer[5] >> 4;
			if (extendedWValue >= 8)
				extendedWValue = event_buffer[5];
			isExtended = true;

			// Extended W = 0 - Scroll wheels
			if (extendedWValue == 0) {
				event.wheel_ydelta = event_buffer[2];
				event.wheel_xdelta = event_buffer[3];
				event.wheel_zdelta = event_buffer[4];
				event.wheel_wdelta = (event_buffer[5] & 0x0f) | (event_buffer[3] & 0x30);
				// Sign extend wdelta if needed
				if (event.wheel_wdelta & 0x20)
					event.wheel_wdelta |= 0xffffffc0;
			}

			// Extended W = 1 - Secondary finger
			if (extendedWValue == 1) {
				event.xPosition = event_buffer[1];
				event.yPosition = event_buffer[2];

				val32 = event_buffer[4] & 0x0F;
				event.xPosition += val32 << 8;
				val32 = event_buffer[4] & 0xF0;
				event.yPosition += val32 << 4;

				event.xPosition *= 2;
				event.yPosition *= 2;

				event.zPressure = event_buffer[5] & 0x0F;
				event.zPressure += event_buffer[3] & 0x30;

				// There is a separate "v" value indicating the finger width
				vValue = (event_buffer[1] & 1)
					| (event_buffer[2] & 1) << 1
					| (event_buffer[5] & 1) << 2;

				event.fingerWidth = vValue * 2;
			}

			// Extended W = 2 - Finger state info
			if (extendedWValue == 2) {
				event.fingers = (1 << (event_buffer[1] & 0x0F)) - 1;
				// TODO this event also provides primary and secondary finger indexes, what do
				// these mean?
			}

			// Other values are reserved for other uses (usually enabled with special commands)

			if (extendedWValue != 0) {
				// The secondary finger and finger state packets describe
				// fingers besides the primary one. Reported as an event of
				// their own, the secondary finger position was taken for the
				// primary finger: the pointer jumped, for example to the
				// thumb resting on the clickpad button area while clicking.
				// The finger count also comes with the primary packets
				// (W 0 or 1), so these packets are not reported.
				return B_ENTRY_NOT_FOUND;
			}

			// Extended W packets do not report the buttons. Repeat the last
			// reported state, so that a held button (a clickpad click while
			// a second finger moves, for example) is not seen as released.
			event.buttons = cookie->buttons;

			*_event = event;

			return status;
		}

		// A one-button clickpad reports its click like a middle button
		// (byte 1 bit 0 XOR byte 4 bit 0). It is the primary button: report
		// it as the left button, and only as that, even when the pad also
		// claims a middle button (as on a ThinkPad X270); otherwise every
		// click on the pad arrives as left plus middle. Linux handles
		// clickpads the same way.
		uint8 clickBit = (event_buffer[0] ^ event_buffer[3]) & 0x01;
		if (sTouchpadInfo.capClickPadButtonCount == 1)
			event.buttons = (event.buttons & ~1) | clickBit;
		else if (sTouchpadInfo.capMiddleButton || sTouchpadInfo.capFourButtons)
			event.buttons |= clickBit << 2;

		if (sTouchpadInfo.nExtendedButtons > 0 && !ext_buttons_use_guest()) {
			// The state is tracked in synaptics_ext_buttons_changed(); when
			// the buttons belong to a published trackpoint, they are
			// reported through it instead of here.
			event.buttons |= sTouchpadInfo.extendedButtonsState
				<< sTouchpadInfo.firstExtendedButton;
		}
 	} else {
 		bool finger = event_buffer[0] >> 5 & 1;
 		if (finger) {
 			// finger with normal width
			event.fingerWidth = 4;
 		}
 		event.gesture = event_buffer[0] >> 2 & 1;
 	}

	cookie->buttons = event.buttons;

	if (!isExtended) {
		event.xPosition = event_buffer[4];
		event.yPosition = event_buffer[5];

		val32 = event_buffer[1] & 0x0F;
		event.xPosition += val32 << 8;
		val32 = event_buffer[1] >> 4 & 0x0F;
		event.yPosition += val32 << 8;

		xTwelfBit = event_buffer[3] >> 4 & 1;
		event.xPosition += xTwelfBit << 12;
		yTwelfBit = event_buffer[3] >> 5 & 1;
		event.yPosition += yTwelfBit << 12;
	}

	*_event = event;
	return B_OK;
}


static void
query_capability(ps2_dev *dev)
{
	uint8 val[3];
	uint8 nExtendedQueries = 0;

	get_information_query(dev, nExtendedQueries, kReadCapabilities, val);

	TRACE("SYNAPTICS: extended mode %2x\n", val[0] >> 7 & 1);
	sTouchpadInfo.capExtended = val[0] >> 7 & 1;
	TRACE("SYNAPTICS: extended queries %2x\n", val[0] >> 4 & 7);
	nExtendedQueries = val[0] >> 4 & 7;
	TRACE("SYNAPTICS: middle button %2x\n", val[0] >> 2 & 1);
	sTouchpadInfo.capMiddleButton = val[0] >> 2 & 1;

	TRACE("SYNAPTICS: pass through %2x\n", val[2] >> 7 & 1);
	sTouchpadInfo.capPassThrough = val[2] >> 7 & 1;

	// bit 6, low power, is only informative (touchpad has an automatic powersave mode)

	TRACE("SYNAPTICS: multi finger report %2x\n", val[2] >> 5 & 1);
	sTouchpadInfo.capMultiFingerReport = val[2] >> 5 & 1;

	TRACE("SYNAPTICS: sleep mode %2x\n", val[2] >> 4 & 1);
	sTouchpadInfo.capSleep = val[2] >> 4 & 1;

	TRACE("SYNAPTICS: four buttons %2x\n", val[2] >> 3 & 1);
	sTouchpadInfo.capFourButtons = val[2] >> 3 & 1;

	// bit 2, TouchStyk ballistics, not further documented in the documents I have

	TRACE("SYNAPTICS: multi finger %2x\n", val[2] >> 1 & 1);
	sTouchpadInfo.capMultiFinger = val[2] >> 1 & 1;

	TRACE("SYNAPTICS: palm detection %2x\n", val[2] & 1);
	sTouchpadInfo.capPalmDetection = val[2] & 1;

	if (get_information_query(dev, nExtendedQueries, kContinuedCapabilities,
			val) == B_OK) {
		sTouchpadInfo.capAdvancedGestures = val[0] >> 3 & 1;
		TRACE("SYNAPTICS: advanced gestures %x\n", sTouchpadInfo.capAdvancedGestures);

		sTouchpadInfo.capClickPadButtonCount = (val[0] >> 4 & 1) | ((val[1] >> 0 & 1) << 1);
		sTouchpadInfo.capClickPadUniform = val[1] >> 4 & 1;
		TRACE("SYNAPTICS: clickpad buttons: %x\n", sTouchpadInfo.capClickPadButtonCount);
		TRACE("SYNAPTICS: clickpad type: %s\n",
			sTouchpadInfo.capClickPadUniform ? "uniform" : "hinged");

	} else {
		sTouchpadInfo.capAdvancedGestures = 0;
		sTouchpadInfo.capClickPadButtonCount = 0;
		sTouchpadInfo.capClickPadUniform = 0;
		sTouchpadInfo.capAdvancedGestures = 0;
	}

	if (get_information_query(dev, nExtendedQueries, kExtendedModelId, val)
			!= B_OK) {
		// "Extended Model ID" is not supported, so there cannot be extra
		// buttons.
		sTouchpadInfo.nExtendedButtons = 0;
		sTouchpadInfo.firstExtendedButton = 0;
		sTouchpadInfo.capExtendedWMode = 0;
		return;
	}

	TRACE("SYNAPTICS: extended buttons %2x\n", val[1] >> 4 & 15);
	sTouchpadInfo.nExtendedButtons = val[1] >> 4 & 15;
	sTouchpadInfo.extendedButtonsState = 0;

	sTouchpadInfo.capExtendedWMode = val[0] >> 2 & 1;
	TRACE("SYNAPTICS: extended wmode %2x\n", sTouchpadInfo.capExtendedWMode);

	if (sTouchpadInfo.capFourButtons)
		sTouchpadInfo.firstExtendedButton = 4;
	else if (sTouchpadInfo.capMiddleButton)
		sTouchpadInfo.firstExtendedButton = 3;
	else
		sTouchpadInfo.firstExtendedButton = 2;

	// Capability 0x10 is not documented in the Synaptics Touchpad interfacing
	// guide (at least the versions I could find), but we got the information
	// from Linux patches: https://lkml.org/lkml/2015/2/6/621
	if (get_information_query(dev, nExtendedQueries, kTrackpointQuirk, val)
			!= B_OK)
		return;

	// Workaround for Thinkpad use of the extended buttons: they are
	// used as buttons for the trackpoint, so they should be reported
	// as buttons 0, 1, 2 rather than 3, 4, 5.
	TRACE("SYNAPTICS: alternate buttons %2x\n", val[0] >> 0 & 1);
	if (val[0] >> 0 & 1)
		sTouchpadInfo.firstExtendedButton = 0;
}


//	#pragma mark - exported functions


static int32 sGuestDetectRetries;


static int32
guest_reprobe_thread(void *data)
{
	// Delayed off the ps2 service thread, so queued hot-plug events keep
	// being processed during the wait.
	snooze(5000000);
	ps2_service_notify_device_republish((ps2_dev*)data);
	return 0;
}


void
synaptics_guest_detect_failed(ps2_dev *dev)
{
	// In some boots the pad answers every guest command with the synthetic
	// AA 00 for a while (the "bad window"). Re-probe the guest a few times;
	// the window often passes.
	int32 attempt = atomic_add(&sGuestDetectRetries, 1);
	if (attempt >= 3) {
		atomic_add(&sGuestDetectRetries, -1);
		return;
	}
	INFO("SYNAPTICS: guest detection failed, re-probe %" B_PRId32 " in 5 s\n",
		attempt + 1);
	thread_id thread = spawn_kernel_thread(guest_reprobe_thread,
		"ps2 guest re-probe", B_LOW_PRIORITY, dev);
	if (thread >= 0)
		resume_thread(thread);
}


void
synaptics_guest_standard_fallback(ps2_dev *dev)
{
	// The guest was detected as a plain mouse. With bounces in this boot
	// that usually means the TrackPoint's identification commands fell
	// into a bad window; re-probe later as for a failed detection.
	if (sGuestBounces > 0)
		synaptics_guest_detect_failed(dev);
}


bool
synaptics_hot_plug_is_packet_data(ps2_dev *dev)
{
	// An aa, 00 byte pair is a reset announce of the pad only if the aa
	// started a new packet. Bytes 2, 3, 5 and 6 of an absolute packet can
	// hold any value: aa, 00 there is ordinary data (for example the low X
	// and Y bytes), or the payload of a pass-through packet carrying the
	// guest's self-test announce. An aa that starts a packet fails the sync
	// check in synaptics_handle_int() (bit 3 set), which leaves the packet
	// index at 0, as does an aa at byte 4 (bits 7:6 not 11). A partial
	// packet followed by a pause is dropped there too, so an announce after
	// the pad reset in the middle of a packet still counts.
	if (dev->handle_int != &synaptics_handle_int || dev->cookie == NULL)
		return false;
	synaptics_cookie *cookie = (synaptics_cookie*)dev->cookie;
	return cookie->packet_index != 0;
}


status_t
synaptics_pass_through_set_packet_size(ps2_dev *dev, uint8 size)
{
	synaptics_cookie *synapticsCookie
		= (synaptics_cookie*)dev->parent_dev->cookie;

	status_t status = ps2_dev_command(dev->parent_dev, PS2_CMD_DISABLE, NULL,
		0, NULL, 0);
	if (status < B_OK) {
		INFO("SYNAPTICS: cannot disable touchpad %s\n", dev->parent_dev->name);
		return B_ERROR;
	}

	synapticsCookie->packet_index = 0;

	if (size == 4)
		synapticsCookie->mode |= SYN_FOUR_BYTE_CHILD;
	else
		synapticsCookie->mode &= ~SYN_FOUR_BYTE_CHILD;

	set_touchpad_mode(dev->parent_dev, synapticsCookie->mode);

	status = ps2_dev_command(dev->parent_dev, PS2_CMD_ENABLE, NULL, 0, NULL, 0);
	if (status < B_OK) {
		INFO("SYNAPTICS: cannot enable touchpad %s\n", dev->parent_dev->name);
		return B_ERROR;
	}
	return status;
}


static status_t
passthrough_send_byte(ps2_dev *padDev, uint8 val, bigtime_t timeout)
{
	// Deliver one byte to the guest: sliced E8 arguments carrying the byte,
	// then the magic sample rate that makes the pad forward it.
	uint8 passThroughCmd = SYN_PASSTHROUGH_CMD;
	status_t status = send_touchpad_arg_timeout(padDev, val, timeout);
	if (status != B_OK)
		return status;
	return ps2_dev_command_timeout(padDev, PS2_CMD_SET_SAMPLE_RATE,
		&passThroughCmd, 1, NULL, 0, timeout);
}


static status_t
guest_wait_ack(uint8 cmd)
{
	// The guest acks every byte it receives. Guest motion bytes can be
	// interleaved while it is streaming; skip them.
	bigtime_t deadline = system_time() + kPassthroughReplyTimeout;
	uint8 reply;

	while (true) {
		bigtime_t remaining = deadline - system_time();
		if (remaining <= 0)
			return B_TIMED_OUT;
		status_t status = guest_reply_pop(&reply, remaining);
		if (status != B_OK)
			return status;
		if (reply == PS2_REPLY_ACK)
			return B_OK;
		if (reply == PS2_CMD_TEST_PASSED) {
			// the pad answered with a self-test announce instead of the
			// guest's ack (the "bounce"); a 00 follows in its own packet
			sGuestBounces++;
			sGuestSelfTestTime = system_time();
			guest_reply_pop(&reply, 100000);
			TRACE("SYNAPTICS: guest bounced 0x%02x\n", cmd);
			return B_IO_ERROR;
		}
		if (reply == PS2_REPLY_RESEND || reply == PS2_REPLY_ERROR) {
			TRACE("SYNAPTICS: guest replied 0x%02x to 0x%02x\n", reply, cmd);
			return B_IO_ERROR;
		}
	}
}


static status_t
passthrough_command_once(ps2_dev *dev, uint8 cmd, const uint8 *out,
	int outCount, uint8 *in, int inCount, bigtime_t timeout)
{
	ps2_dev *padDev = dev->parent_dev;
	bigtime_t padTimeout = min_c(timeout, 500000);
	status_t status = B_OK;
	status_t enableStatus;
	uint8 stale;
	int i;

	// flush stale guest bytes
	while (guest_reply_pop(&stale, 0) == B_OK)
		;

	// The pad only forwards the guest's bytes while it is disabled itself
	// (with the pad enabled the guest's replies never arrive, verified by
	// a byte-level dump), so wrap the command in disable/enable.
	status = ps2_dev_command_timeout(padDev, PS2_CMD_DISABLE, NULL, 0, NULL, 0,
		padTimeout);
	if (status != B_OK)
		return status;

	atomic_set(&sGuestCommandInFlight, 1);

	for (i = -1; status == B_OK && i < outCount; i++) {
		status = passthrough_send_byte(padDev, i == -1 ? cmd : out[i],
			padTimeout);
		if (status != B_OK)
			break;
		status = guest_wait_ack(cmd);
	}

	for (i = 0; status == B_OK && i < inCount; i++) {
		status = guest_reply_pop(&in[i], cmd == PS2_CMD_RESET
			? timeout : kPassthroughReplyTimeout);
		if (status == B_OK && cmd != PS2_CMD_RESET
			&& in[i] == PS2_CMD_TEST_PASSED) {
			// The guest reset itself right after acking (seen as
			// "FA, then AA 00 ~80 ms later"): a self-test announce, not
			// reply data. None of the guest commands used here have AA in
			// a real reply. Drop the following 00 and retry the command.
			sGuestBounces++;
			sGuestSelfTestTime = system_time();
			guest_reply_pop(&in[i], 100000);
			TRACE("SYNAPTICS: self-test announce in 0x%02x reply\n", cmd);
			status = B_IO_ERROR;
		}
	}

	atomic_set(&sGuestCommandInFlight, 0);

	enableStatus = ps2_dev_command_timeout(padDev, PS2_CMD_ENABLE, NULL, 0,
		NULL, 0, padTimeout);
	return status != B_OK ? status : enableStatus;
}


status_t
passthrough_command(ps2_dev *dev, uint8 cmd, const uint8 *out, int outCount,
	uint8 *in, int in_count, bigtime_t timeout)
{
	status_t status = B_ERROR;
	int tries;

	if (sGuestReplySem < 0)
		return B_NO_INIT;

	mutex_lock(&sGuestCommandLock);

	for (tries = 0; tries < 3; tries++) {
		if (tries > 0)
			snooze(100000);
		status = passthrough_command_once(dev, cmd, out, outCount, in,
			in_count, timeout);
		if (status == B_OK)
			break;
	}

	TRACE("SYNAPTICS: passthrough 0x%02x: %s\n", cmd, strerror(status));
	if (status != B_OK) {
		INFO("SYNAPTICS: passthrough command 0x%02x failed: %s\n", cmd,
			strerror(status));
		if (cmd == PS2_CMD_ENABLE && dev->active) {
			// The published guest cannot be enabled (bad window); without
			// it the device stays dead for the session. Re-probe later.
			synaptics_guest_detect_failed(dev);
		}
	}

	mutex_unlock(&sGuestCommandLock);
	return status;
}


void
synaptics_stall_probe(ps2_dev *guest)
{
	// A guest reset-disable + enable reliably brings a stalled pad back,
	// while a pad status request or a guest get ID does not.
	ps2_dev *pad = guest->parent_dev;
	if ((atomic_get(&pad->flags) & PS2_FLAG_OPEN) == 0 || pad->cookie == NULL)
		return;
	synaptics_cookie *cookie = (synaptics_cookie*)pad->cookie;

	status_t status = ps2_dev_command(guest, PS2_CMD_MOUSE_RESET_DIS, NULL, 0,
		NULL, 0);
	status_t enable = ps2_dev_command(guest, PS2_CMD_ENABLE, NULL, 0, NULL, 0);
	cookie->packet_index = 0;
	INFO("SYNAPTICS: pad stall recovery (guest reset-disable): %s, "
		"enable %s\n", strerror(status), strerror(enable));

	// give the timing a few packets before another stall can be detected
	sStallQuietUntil = system_time() + 3000000;
	sSlowPackets = 0;
	sStallPending = false;
}


status_t
probe_synaptics(ps2_dev *dev)
{
	uint8 val[3];
	uint8 deviceId;
	status_t status;
	TRACE("SYNAPTICS: probe\n");

	// We reset the device here because it may have been left in a confused
	// state by a previous probing attempt. Some synaptics touchpads are known
	// to lockup when we attempt to detect them as IBM trackpoints.
	ps2_reset_mouse(dev);

	// Request "Identify touchpad"
	// The touchpad will delay this, until it's ready and calibrated.
	status = get_information_query(dev, 0, kIdentify, val);
	if (status != B_OK)
		return status;

	sTouchpadInfo.minorVersion = val[0];
	deviceId = val[1];
	if (deviceId != SYN_TOUCHPAD) {
		TRACE("SYNAPTICS: not found\n");
		return B_ERROR;
	}

	TRACE("SYNAPTICS: Touchpad found id:l %2x\n", deviceId);
	sTouchpadInfo.majorVersion = val[2] & 0x0F;
	TRACE("SYNAPTICS: version %d.%d\n", sTouchpadInfo.majorVersion,
		sTouchpadInfo.minorVersion);
	// version >= 4.0?
	if (sTouchpadInfo.minorVersion <= 2
		&& sTouchpadInfo.majorVersion <= 3) {
		TRACE("SYNAPTICS: too old touchpad not supported\n");
		return B_ERROR;
	}
	dev->name = kSynapticsPath[dev->idx];
	return B_OK;
}


//	#pragma mark - Device functions


// Bytes that do not fit the absolute packet format, without a complete
// packet in between, after which the pad is assumed to have lost its mode.
// A relative packet (3 bytes, bit 3 set in the first) is rejected as one or
// two bytes, so this is about ten such packets.
static const int32 kResyncBadBytes = 16;
// Do not restore the pad more often than this: if it does not help, the
// log should show it rather than the driver looping.
static const bigtime_t kResyncInterval = 3000000;
static bigtime_t sLastResync;
static int32 sResyncCount;


static void
synaptics_count_bad_byte(synaptics_cookie *cookie)
{
	// interrupt context
	if (++cookie->bad_bytes < kResyncBadBytes)
		return;
	if (atomic_test_and_set(&cookie->resync_pending, 1, 0) == 0)
		ps2_service_notify_synaptics_resync(cookie->dev);
}


/*!	Restores the touchpad after it stopped sending absolute packets.
	Both PS/2 Reset and Set Defaults clear the Absolute bit of the mode byte
	(Synaptics TouchPad Interfacing Guide, 2.5), after which the pad sends
	relative mouse packets; Linux psmouse likewise re-initializes a device
	that keeps sending data it cannot parse. Sets the mode byte again,
	re-enables the TrackPoint behind the pass-through port and the pad.
	Runs in the PS/2 service thread.
*/
void
synaptics_resync(ps2_dev *dev)
{
	synaptics_cookie *cookie = (synaptics_cookie*)dev->cookie;
	if ((atomic_get(&dev->flags) & PS2_FLAG_OPEN) == 0 || cookie == NULL)
		return;

	bigtime_t now = system_time();
	if (now - sLastResync < kResyncInterval) {
		atomic_set(&cookie->resync_pending, 0);
		return;
	}
	sLastResync = now;
	sResyncCount++;

	INFO("SYNAPTICS: pad lost its mode (%" B_PRId32 " bad bytes), restoring "
		"(%" B_PRId32 ")\n", cookie->bad_bytes, sResyncCount);

	// No pass-through command may run between the bytes of the mode
	// setting sequence.
	mutex_lock(&sGuestCommandLock);
	ps2_dev_command_timeout(dev, PS2_CMD_DISABLE, NULL, 0, NULL, 0, 150000);
	snooze(30000);
	cookie->packet_index = 0;

	status_t status = set_touchpad_mode(dev, cookie->mode);
	mutex_unlock(&sGuestCommandLock);
	if (status != B_OK)
		INFO("SYNAPTICS: resync: setting mode failed: %s\n", strerror(status));

	if (sTouchpadInfo.capPassThrough && sPassthroughDevice->active) {
		// This wraps the command in pad disable/enable.
		status = passthrough_command(sPassthroughDevice, PS2_CMD_ENABLE, NULL,
			0, NULL, 0, 500000);
		if (status != B_OK) {
			INFO("SYNAPTICS: resync: enabling the TrackPoint failed: %s\n",
				strerror(status));
		}
	}

	status = ps2_dev_command_timeout(dev, PS2_CMD_ENABLE, NULL, 0, NULL, 0,
		150000);
	INFO("SYNAPTICS: resync done: %s\n", strerror(status));

	cookie->bad_bytes = 0;
	atomic_set(&cookie->resync_pending, 0);
}


status_t
synaptics_open(const char *name, uint32 flags, void **_cookie)
{
	status_t status;
	synaptics_cookie *cookie;
	ps2_dev *dev;
	int i;

	for (dev = NULL, i = 0; i < PS2_DEVICE_COUNT; i++) {
		if (0 == strcmp(ps2_device[i].name, name)) {
			dev = &ps2_device[i];
			break;
		}
	}

	if (dev == NULL) {
		TRACE("ps2: dev = NULL\n");
		return B_ERROR;
	}

	if (atomic_or(&dev->flags, PS2_FLAG_OPEN) & PS2_FLAG_OPEN)
		return B_BUSY;

	cookie = (synaptics_cookie*)malloc(sizeof(synaptics_cookie));
	if (cookie == NULL)
		goto err1;
	memset(cookie, 0, sizeof(*cookie));

	*_cookie = cookie;

	cookie->dev = dev;
	dev->cookie = cookie;
	dev->disconnect = &synaptics_disconnect;
	dev->handle_int = &synaptics_handle_int;

	gHardwareSpecs.edgeMotionWidth = SYN_EDGE_MOTION_WIDTH;

	gHardwareSpecs.areaStartX = SYN_AREA_START_X;
	gHardwareSpecs.areaEndX = SYN_AREA_END_X;
	gHardwareSpecs.areaStartY = SYN_AREA_START_Y;
	gHardwareSpecs.areaEndY = SYN_AREA_END_Y;

	gHardwareSpecs.minPressure = MIN_PRESSURE;
	gHardwareSpecs.realMaxPressure = REAL_MAX_PRESSURE;
	gHardwareSpecs.maxPressure = MAX_PRESSURE;

	dev->packet_size = PS2_PACKET_SYNAPTICS;

	cookie->synaptics_ring_buffer
		= create_packet_buffer(SYNAPTICS_HISTORY_SIZE * dev->packet_size);
	if (cookie->synaptics_ring_buffer == NULL) {
		TRACE("ps2: can't allocate mouse actions buffer\n");
		goto err2;
	}

	// create the mouse semaphore, used for synchronization between
	// the interrupt handler and the read operation
	cookie->synaptics_sem = create_sem(0, "ps2_synaptics_sem");
	if (cookie->synaptics_sem < 0) {
		TRACE("SYNAPTICS: failed creating semaphore!\n");
		goto err3;
	}
	query_capability(dev);

	// create pass through dev
	if (sTouchpadInfo.capPassThrough) {
		TRACE("SYNAPTICS: pass through detected\n");
		if (sGuestReplySem < 0) {
			sGuestReplySem = create_sem(0, "ps2 guest reply");
			if (sGuestReplySem < 0)
				goto err4;
		}
		sPassthroughDevice->parent_dev = dev;
		sPassthroughDevice->idx = dev->idx;
		ps2_service_notify_device_added(sPassthroughDevice);
	}

	// Set Mode
	if (sTouchpadInfo.capExtendedWMode)
		cookie->mode = SYN_MODE_ABSOLUTE | SYN_MODE_W | SYN_MODE_EXTENDED_W;
	else if (sTouchpadInfo.capExtended)
		cookie->mode = SYN_MODE_ABSOLUTE | SYN_MODE_W;
	else
		cookie->mode = SYN_MODE_ABSOLUTE;
	status = set_touchpad_mode(dev, cookie->mode);
	if (status < B_OK) {
		INFO("SYNAPTICS: cannot set mode %s\n", name);
		goto err4;
	}

	status = ps2_dev_command(dev, PS2_CMD_ENABLE, NULL, 0, NULL, 0);
	if (status < B_OK) {
		INFO("SYNAPTICS: cannot enable touchpad %s\n", name);
		goto err4;
	}

	atomic_or(&dev->flags, PS2_FLAG_ENABLED);

	TRACE("SYNAPTICS: open %s success\n", name);
	return B_OK;

err4:
	delete_sem(cookie->synaptics_sem);
err3:
	delete_packet_buffer(cookie->synaptics_ring_buffer);
err2:
	free(cookie);
err1:
	atomic_and(&dev->flags, ~PS2_FLAG_OPEN);

	TRACE("SYNAPTICS: synaptics_open %s failed\n", name);
	return B_ERROR;
}


status_t
synaptics_close(void *_cookie)
{
	status_t status;
	synaptics_cookie *cookie = (synaptics_cookie*)_cookie;

	ps2_dev_command_timeout(cookie->dev, PS2_CMD_DISABLE, NULL, 0, NULL, 0,
		150000);

	delete_packet_buffer(cookie->synaptics_ring_buffer);
	delete_sem(cookie->synaptics_sem);

	atomic_and(&cookie->dev->flags, ~PS2_FLAG_OPEN);
	atomic_and(&cookie->dev->flags, ~PS2_FLAG_ENABLED);

	// Reset the touchpad so it generate standard ps2 packets instead of
	// extended ones. If not, BeOS is confused with such packets when rebooting
	// without a complete shutdown.
	INFO("SYNAPTICS: close, resetting the pad\n");
	status = ps2_reset_mouse(cookie->dev);
	if (status != B_OK) {
		INFO("ps2_synaptics: reset failed\n");
		return B_ERROR;
	}

	if (sTouchpadInfo.capPassThrough)
		ps2_service_notify_device_removed(sPassthroughDevice);

	TRACE("SYNAPTICS: close %s done\n", cookie->dev->name);
	return B_OK;
}


status_t
synaptics_freecookie(void *_cookie)
{
	free(_cookie);
	return B_OK;
}


static status_t
synaptics_read(void *cookie, off_t pos, void *buffer, size_t *_length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
synaptics_write(void *cookie, off_t pos, const void *buffer, size_t *_length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


status_t
synaptics_ioctl(void *_cookie, uint32 op, void *buffer, size_t length)
{
	synaptics_cookie *cookie = (synaptics_cookie*)_cookie;
	touchpad_read read;
	status_t status;

	switch (op) {
		case MS_IS_TOUCHPAD:
			TRACE("SYNAPTICS: MS_IS_TOUCHPAD\n");
			if (buffer == NULL)
				return B_OK;
			return user_memcpy(buffer, &gHardwareSpecs, sizeof(gHardwareSpecs));

		case MS_READ_TOUCHPAD:
		{
			TRACE("SYNAPTICS: MS_READ get event\n");
			if (user_memcpy(&read.timeout, &(((touchpad_read*)buffer)->timeout),
					sizeof(bigtime_t)) != B_OK)
				return B_BAD_ADDRESS;

			// Packets that are not reported (B_ENTRY_NOT_FOUND) do not end
			// the wait; the timeout still counts from the start.
			bigtime_t timeout = read.timeout;
			bigtime_t deadline = 0;
			if (timeout != B_INFINITE_TIMEOUT)
				deadline = system_time() + timeout;
			while (true) {
				status = get_synaptics_movment(cookie, &read.u.touchpad,
					timeout);
				if (status != B_ENTRY_NOT_FOUND)
					break;
				if (deadline != 0) {
					timeout = deadline - system_time();
					if (timeout <= 0)
						return B_TIMED_OUT;
				}
			}
			if (status != B_OK)
				return status;
			read.event = MS_READ_TOUCHPAD;
			return user_memcpy(buffer, &read, sizeof(read));
		}

		default:
			TRACE("SYNAPTICS: unknown opcode: %" B_PRIu32 "\n", op);
			return B_DEV_INVALID_IOCTL;
	}
}


int32
synaptics_handle_int(ps2_dev *dev)
{
	synaptics_cookie *cookie = (synaptics_cookie*)dev->cookie;
	uint8 val;

	val = cookie->dev->history[0].data;

	// The bytes of one packet follow each other within a few milliseconds.
	// After a long pause, a partial packet will not be completed (the pad
	// reset, or bytes were lost): start over.
	bigtime_t time = cookie->dev->history[0].time;
	if (cookie->packet_index != 0
		&& time - cookie->last_byte_time > SYN_PACKET_TIMEOUT) {
		INFO("SYNAPTICS: dropping %" B_PRIuSIZE " byte partial packet after %"
			B_PRIdBIGTIME " us\n", cookie->packet_index,
			time - cookie->last_byte_time);
		cookie->packet_index = 0;
	}
	cookie->last_byte_time = time;

	if ((cookie->packet_index == 0 || cookie->packet_index == 3) && (val & 8) != 0) {
		INFO("SYNAPTICS: bad mouse data %#02x, trying resync\n", val);
		cookie->packet_index = 0;
		synaptics_count_bad_byte(cookie);
		return B_UNHANDLED_INTERRUPT;
	}
	if (cookie->packet_index == 0 && val >> 6 != 0x02) {
		TRACE("SYNAPTICS: first package %#02x begins not with bit 1, 0\n", val);
		synaptics_count_bad_byte(cookie);
		return B_UNHANDLED_INTERRUPT;
	}
	if (cookie->packet_index == 3 && val >> 6 != 0x03) {
		TRACE("SYNAPTICS: third package %#02x begins not with bit 1, 1\n", val);
		cookie->packet_index = 0;
		synaptics_count_bad_byte(cookie);
		return B_UNHANDLED_INTERRUPT;
	}
	if (cookie->packet_index == 0)
		sPacketStart = system_time();
	cookie->buffer[cookie->packet_index] = val;

	cookie->packet_index++;
	if (cookie->packet_index >= 6) {
		cookie->packet_index = 0;
		cookie->bad_bytes = 0;
		check_stall();

		if (IS_SYN_PT_PACKAGE(cookie->buffer)) {
			// Pass-through packet, carrying bytes from the guest device.
			if (atomic_get(&sGuestCommandInFlight) != 0) {
				// A guest command is waiting for this byte. The pad wraps
				// one guest byte per packet here (byte 1), as on Linux.
				guest_reply_push(cookie->buffer[1]);
				return B_INVOKE_SCHEDULER;
			}

			if (cookie->buffer[1] == PS2_CMD_TEST_PASSED
				&& cookie->buffer[4] == 0) {
				// Unsolicited self-test announce: the guest reset itself
				// (a motion packet cannot look like this, AA has the
				// overflow bits set). Re-initialize it instead of letting
				// the bytes corrupt the guest's packet stream.
				sGuestSelfTestTime = system_time();
				INFO("SYNAPTICS: unsolicited guest self-test (guest %s)\n",
					sPassthroughDevice->active ? "active" : "inactive");
				if (sPassthroughDevice->active && sGuestReinits < 5) {
					sGuestReinits++;
					ps2_service_notify_device_republish(sPassthroughDevice);
				}
				return B_INVOKE_SCHEDULER;
			}

			if (sPassthroughDevice->active
				&& sPassthroughDevice->handle_int != NULL) {
				TRACE("SYNAPTICS: forward packet to passthrough device\n");
				status_t status;

				sPassthroughDevice->history[0].data = cookie->buffer[1];
				sPassthroughDevice->handle_int(sPassthroughDevice);
				sPassthroughDevice->history[0].data = cookie->buffer[4];
				sPassthroughDevice->handle_int(sPassthroughDevice);
				sPassthroughDevice->history[0].data = cookie->buffer[5];
				status = sPassthroughDevice->handle_int(sPassthroughDevice);

				if (cookie->dev->packet_size == 4) {
					sPassthroughDevice->history[0].data = cookie->buffer[2];
					status = sPassthroughDevice->handle_int(sPassthroughDevice);
				}
				return status;
			}

			// The guest device has not been published (yet), don't let its
			// packets turn into touchpad events.
			TRACE("SYNAPTICS: dropping packet for inactive passthrough\n");
			return B_HANDLED_INTERRUPT;
		}

		if (sTouchpadInfo.nExtendedButtons > 0
			&& ((cookie->buffer[0] ^ cookie->buffer[3]) & 0x02) != 0) {
			// This packet carries the extended buttons state. The pad only
			// sends it when the state changes (packets without this flag
			// mean "unchanged", not "released"), so track it here.
			synaptics_ext_buttons_changed(cookie->buffer);
		}

		if (packet_buffer_write(cookie->synaptics_ring_buffer,
				cookie->buffer, cookie->dev->packet_size)
			!= cookie->dev->packet_size) {
			// buffer is full, drop new data
			return B_HANDLED_INTERRUPT;
		}
		release_sem_etc(cookie->synaptics_sem, 1, B_DO_NOT_RESCHEDULE);

		return B_INVOKE_SCHEDULER;
	}

	return B_HANDLED_INTERRUPT;
}


void
synaptics_disconnect(ps2_dev *dev)
{
	synaptics_cookie *cookie = (synaptics_cookie*)dev->cookie;
	// the mouse device might not be opened at this point
	INFO("SYNAPTICS: synaptics_disconnect %s\n", dev->name);
	if ((dev->flags & PS2_FLAG_OPEN) != 0)
		release_sem(cookie->synaptics_sem);
}


device_hooks gSynapticsDeviceHooks = {
	synaptics_open,
	synaptics_close,
	synaptics_freecookie,
	synaptics_ioctl,
	synaptics_read,
	synaptics_write,
};
