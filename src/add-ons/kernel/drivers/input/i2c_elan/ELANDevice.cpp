/*
 * Copyright 2020, Jérôme Duval, jerome.duval@gmail.com.
 * Copyright 2008-2011, Michael Lotz <mmlr@mlotz.ch>
 * Copyright 2020, 2022 Vladimir Kondratyev <wulf@FreeBSD.org>
 * Copyright 2023 Vladimir Serbinenko <phcoder@gmail.com>
 * Distributed under the terms of the MIT license.
 */


//!	Driver for I2C Elan touchpads.
// Speaks the native Elan I2C protocol, as the Linux elan_i2c driver does.
// Chromebook touchpads (ELAN0000) only support this protocol, not HID.


#include "Driver.h"
#include "ELANDevice.h"
#include "InputReadyLine.h"

#include <keyboard_mouse_driver.h>
#include <kernel.h>
#include <util/AutoLock.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <new>


#define ETP_I2C_RESET			0x0100
#define ETP_I2C_WAKE_UP			0x0800
#define ETP_I2C_SLEEP			0x0801

#define ETP_I2C_DESC_CMD		0x0001
#define ETP_I2C_REPORT_DESC_CMD	0x0002
#define ETP_I2C_STAND_CMD		0x0005
#define ETP_I2C_PATTERN_CMD		0x0100
#define ETP_I2C_FW_VERSION_CMD	0x0102
#define ETP_I2C_MAX_X_AXIS_CMD	0x0106
#define ETP_I2C_MAX_Y_AXIS_CMD	0x0107
#define ETP_I2C_PRESSURE_CMD	0x010A
#define ETP_I2C_SET_CMD			0x0300

#define ETP_I2C_DESC_LENGTH		30
#define ETP_I2C_REPORT_DESC_LENGTH	158
#define ETP_I2C_INF_LENGTH		2
#define ETP_I2C_REPORT_LEN		34
#define ETP_I2C_REPORT_LEN_ID2	39

#define ETP_ENABLE_ABS			0x0001
#define ETP_PRESSURE_OFFSET		25

#define ETP_REPORT_ID			0x5d
#define ETP_TP_REPORT_ID		0x5e
#define ETP_TP_REPORT_ID2		0x5f
#define ETP_REPORT_ID2			0x60	// high precision

#define ETP_REPORT_ID_OFFSET	2
#define ETP_TOUCH_INFO_OFFSET	3
#define ETP_FINGER_DATA_OFFSET	4
#define ETP_MK_DATA_OFFSET		33		// high precision only
#define ETP_MAX_FINGERS			ELAN_MAX_FINGERS
#define ETP_FINGER_DATA_LEN		5

#define ETP_RETRY_COUNT			3

// How often the input ready line is checked, and how often the touchpad is
// read when there is no such line.
static const bigtime_t kInputReadyPollInterval = 1000;
static const bigtime_t kBlindPollInterval = 8000;

// A touch of two or more fingers that ends within this time, without moving
// or pressing the pad, is a tap for the right (or middle) button.
static const bigtime_t kMultiFingerTapTimeout = 250000;

#define BUTTON_LEFT		0x01
#define BUTTON_RIGHT	0x02
#define BUTTON_MIDDLE	0x04


ELANDevice::ELANDevice(device_node* parent, i2c_device_interface* i2c,
	i2c_device i2cCookie, InputReadyLine* inputReady)
	:	fStatus(B_NO_INIT),
		fOpenCount(0),
		fRemoved(false),
		fPublishPath(NULL),
		fParent(parent),
		fI2C(i2c),
		fI2CCookie(i2cCookie),
		fInputReady(inputReady),
		fPattern(0),
		fReportLength(ETP_I2C_REPORT_LEN),
		fMaxX(0),
		fMaxY(0),
		fPressureAdjustment(0),
		fBadReports(0),
		fFingerMask(0),
		fLastFingerMask(0),
		fPositionX(0),
		fPositionY(0)
{
	_ResetContact();
	mutex_init(&fReadLock, "elan report");
	memset(fLastReport, 0, sizeof(fLastReport));
	memset(&fHardwareSpecs, 0, sizeof(fHardwareSpecs));

	status_t status = B_ERROR;
	for (int i = 0; i < ETP_RETRY_COUNT; i++) {
		status = _Initialize();
		if (status == B_OK)
			break;
		snooze(30000);
	}
	if (status != B_OK) {
		ERROR("initialization failed: %s\n", strerror(status));
		fStatus = status;
		return;
	}

	fHardwareSpecs.areaStartX = 0;
	fHardwareSpecs.areaEndX = fMaxX;
	fHardwareSpecs.areaStartY = 0;
	fHardwareSpecs.areaEndY = fMaxY;
	// The Synaptics driver uses 55 for a pad about 4000 units wide.
	fHardwareSpecs.edgeMotionWidth = max_c(fMaxX * 55 / 4000, 1);
	fHardwareSpecs.minPressure = 0;
	fHardwareSpecs.realMaxPressure = 50;
	fHardwareSpecs.maxPressure = 255;

	TRACE_ALWAYS("touchpad %ux%u, pattern %u, report length %" B_PRIuSIZE
		", %s\n", fMaxX, fMaxY, fPattern, fReportLength,
		fInputReady != NULL ? "input ready line" : "polling");

	// Stay quiet until someone opens the device.
	_Sleep();
	fStatus = B_OK;
}


ELANDevice::~ELANDevice()
{
	delete fInputReady;
	free(fPublishPath);
	mutex_destroy(&fReadLock);
}


status_t
ELANDevice::Open(uint32 flags)
{
	if (atomic_add(&fOpenCount, 1) == 0) {
		status_t status = _Wake();
		if (status != B_OK) {
			ERROR("failed to wake touchpad: %s\n", strerror(status));
			atomic_add(&fOpenCount, -1);
			return status;
		}
	}

	return B_OK;
}


status_t
ELANDevice::Close()
{
	if (atomic_add(&fOpenCount, -1) == 1)
		_Sleep();

	return B_OK;
}


void
ELANDevice::Removed()
{
	fRemoved = true;
}


void
ELANDevice::SetPublishPath(char *publishPath)
{
	free(fPublishPath);
	fPublishPath = publishPath;
}


status_t
ELANDevice::Control(uint32 op, void *buffer, size_t length)
{
	switch (op) {
		case B_GET_DEVICE_NAME:
		{
			if (!IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;

			if (user_strlcpy((char *)buffer, "Elan I2C touchpad", length) > 0)
				return B_OK;

			return B_ERROR;
		}

		case MS_IS_TOUCHPAD:
			if (buffer == NULL)
				return B_OK;
			if (!IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return user_memcpy(buffer, &fHardwareSpecs,
				sizeof(fHardwareSpecs));

		case MS_READ_TOUCHPAD:
		{
			touchpad_read read;
			if (length < sizeof(touchpad_read))
				return B_BUFFER_OVERFLOW;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&read.timeout,
					&((touchpad_read*)buffer)->timeout,
					sizeof(bigtime_t)) != B_OK) {
				return B_BAD_ADDRESS;
			}

			read.event = MS_READ_TOUCHPAD;

			MutexLocker locker(fReadLock);
			status_t status = _ReadReport(&read.u.touchpad, read.timeout);
			locker.Unlock();
			if (status != B_OK)
				return status;

			if (user_memcpy(buffer, &read, sizeof(read)) != B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - protocol


status_t
ELANDevice::_Initialize()
{
	uint8 buffer[ETP_I2C_REPORT_DESC_LENGTH];

	status_t status = _WriteRegister(ETP_I2C_STAND_CMD, ETP_I2C_RESET);
	if (status != B_OK) {
		ERROR("reset failed: %s\n", strerror(status));
		return status;
	}

	snooze(100000);

	// The touchpad acknowledges the reset with a 0000 report.
	status = _Receive(buffer, ETP_I2C_INF_LENGTH);
	if (status != B_OK) {
		ERROR("no reset acknowledgement: %s\n", strerror(status));
		return status;
	}

	status = _ReadRegister(ETP_I2C_DESC_CMD, buffer, ETP_I2C_DESC_LENGTH);
	if (status != B_OK) {
		ERROR("failed to read device descriptor: %s\n", strerror(status));
		return status;
	}

	status = _ReadRegister(ETP_I2C_REPORT_DESC_CMD, buffer,
		ETP_I2C_REPORT_DESC_LENGTH);
	if (status != B_OK) {
		ERROR("failed to read report descriptor: %s\n", strerror(status));
		return status;
	}

	return _QueryDevice();
}


status_t
ELANDevice::_QueryDevice()
{
	uint8 value[ETP_I2C_INF_LENGTH];

	status_t status = _ReadRegister(ETP_I2C_PATTERN_CMD, value, sizeof(value));
	if (status != B_OK)
		return status;
	// Firmware without this command answers ffff: pattern 0.
	fPattern = value[0] == 0xff && value[1] == 0xff ? 0 : value[1];
	fReportLength = fPattern <= 1 ? ETP_I2C_REPORT_LEN : ETP_I2C_REPORT_LEN_ID2;

	if (_ReadRegister(ETP_I2C_FW_VERSION_CMD, value, sizeof(value)) == B_OK)
		TRACE_ALWAYS("firmware version 0x%02x\n", value[0]);

	status = _ReadRegister(ETP_I2C_MAX_X_AXIS_CMD, value, sizeof(value));
	if (status != B_OK)
		return status;
	fMaxX = value[0] | (value[1] << 8);

	status = _ReadRegister(ETP_I2C_MAX_Y_AXIS_CMD, value, sizeof(value));
	if (status != B_OK)
		return status;
	fMaxY = value[0] | (value[1] << 8);

	if (fMaxX == 0 || fMaxY == 0 || fMaxX == 0xffff || fMaxY == 0xffff) {
		ERROR("bad dimensions %ux%u\n", fMaxX, fMaxY);
		return B_BAD_DATA;
	}

	status = _ReadRegister(ETP_I2C_PRESSURE_CMD, value, sizeof(value));
	if (status != B_OK)
		return status;
	fPressureAdjustment = (value[0] & 0x10) != 0 ? 0 : ETP_PRESSURE_OFFSET;

	return B_OK;
}


status_t
ELANDevice::_Wake()
{
	status_t status = B_ERROR;
	for (int i = 0; i < ETP_RETRY_COUNT; i++) {
		status = _WriteRegister(ETP_I2C_SET_CMD, ETP_ENABLE_ABS);
		if (status == B_OK)
			status = _WriteRegister(ETP_I2C_STAND_CMD, ETP_I2C_WAKE_UP);
		if (status == B_OK)
			break;
		snooze(30000);
	}
	if (status != B_OK)
		return status;

	// Drop any report left over from before the touchpad slept.
	memset(fLastReport, 0, sizeof(fLastReport));
	fLastFingerMask = 0;
	_ResetContact();
	if (fInputReady != NULL && fInputReady->IsAsserted())
		_Receive(fReport, fReportLength);

	return B_OK;
}


status_t
ELANDevice::_Sleep()
{
	return _WriteRegister(ETP_I2C_STAND_CMD, ETP_I2C_SLEEP);
}


status_t
ELANDevice::_ReadReport(touchpad_movement* movement, bigtime_t timeout)
{
	bigtime_t deadline = timeout < 0
		? B_INFINITE_TIMEOUT : system_time() + timeout;

	// Without an input ready line the touchpad is read at a fixed rate.
	bool mustWait = fInputReady == NULL;

	while (true) {
		if (fRemoved)
			return B_DEV_NOT_READY;

		if (fPendingRelease) {
			// Second half of an emulated tap click.
			fPendingRelease = false;
			memset(movement, 0, sizeof(*movement));
			movement->fingerWidth = 4;
			return B_OK;
		}

		bool ready = !mustWait
			&& (fInputReady == NULL || fInputReady->IsAsserted());
		if (!ready) {
			bigtime_t now = system_time();
			if (now >= deadline)
				return B_TIMED_OUT;

			bigtime_t interval = fInputReady != NULL
				? kInputReadyPollInterval : kBlindPollInterval;
			if (deadline - now < interval)
				interval = deadline - now;

			status_t status = snooze_etc(interval, B_SYSTEM_TIMEBASE,
				B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT);
			if (status == B_INTERRUPTED)
				return status;

			mustWait = false;
			continue;
		}

		mustWait = fInputReady == NULL;

		status_t status = _Receive(fReport, fReportLength);
		if (status != B_OK) {
			if (fBadReports++ < 10)
				ERROR("failed to read report: %s\n", strerror(status));
			mustWait = true;
			continue;
		}

		// A zero length means no report: the touchpad was reset, or it
		// had nothing to send.
		uint16 length = fReport[0] | (fReport[1] << 8);
		if (length == 0 || length == 0xffff)
			continue;

		uint8 id = fReport[ETP_REPORT_ID_OFFSET];
		if (id != ETP_REPORT_ID && id != ETP_REPORT_ID2) {
			if (id != ETP_TP_REPORT_ID && id != ETP_TP_REPORT_ID2
				&& fBadReports++ < 10) {
				ERROR("unknown report id 0x%02x, length %u\n", id, length);
			}
			continue;
		}

		// Some touchpads repeat their last report when read without one
		// pending; only pass on changes then.
		if (fInputReady == NULL
			&& memcmp(fReport, fLastReport, fReportLength) == 0) {
			continue;
		}
		memcpy(fLastReport, fReport, fReportLength);

		int count = _ParseReport(fReport, movement);
		_TrackPosition(movement, count);
		if (!_EmulateButtons(movement, count))
			continue;
		return B_OK;
	}
}


int
ELANDevice::_ParseReport(const uint8* report, touchpad_movement* movement)
{
	bool highPrecision = report[ETP_REPORT_ID_OFFSET] == ETP_REPORT_ID2;
	uint8 info = report[ETP_TOUCH_INFO_OFFSET];

	memset(movement, 0, sizeof(*movement));
	movement->buttons = (info & 0x01 ? 0x01 : 0)	// left
		| (info & 0x02 ? 0x02 : 0)					// right
		| (info & 0x04 ? 0x04 : 0);					// middle

	// Records are only present for fingers in contact, in order.
	const uint8* finger = report + ETP_FINGER_DATA_OFFSET;
	fFingerMask = 0;
	int count = 0;
	for (int i = 0; i < ETP_MAX_FINGERS; i++) {
		if ((info & (1 << (3 + i))) == 0)
			continue;

		uint32 x, y;
		uint8 traces;
		if (highPrecision) {
			x = (finger[0] << 8) | finger[1];
			y = (finger[2] << 8) | finger[3];
			traces = report[ETP_MK_DATA_OFFSET + i];
		} else {
			x = ((finger[0] & 0xf0) << 4) | finger[1];
			y = ((finger[0] & 0x0f) << 8) | finger[2];
			traces = finger[3];
		}
		uint32 pressure = finger[4] + fPressureAdjustment;
		finger += ETP_FINGER_DATA_LEN;

		if (x > fMaxX || y > fMaxY)
			continue;

		fFingerMask |= 1 << i;
		count++;
		fFingers[i].x = x;
		fFingers[i].y = y;
		fFingers[i].pressure = pressure;
		// The width is given in traces covered along x and y.
		fFingers[i].width = max_c(traces & 0x0f, traces >> 4);
	}

	return count;
}


/*!	Reports one position for all fingers. The slots the touchpad gives each
	finger stay the same while it is down, so the position follows the
	fingers that were down in the last report too. Averaging them instead
	would make the position jump whenever a finger is added or lifted, as at
	the end of every two finger scroll, and movement_maker would move the
	cursor by that much.
*/
void
ELANDevice::_TrackPosition(touchpad_movement* movement, int count)
{
	uint32 common = fFingerMask & fLastFingerMask;
	uint32 sumZ = 0, sumWidth = 0;
	uint32 sumX = 0, sumY = 0;
	int32 deltaX = 0, deltaY = 0, moved = -1;
	for (int i = 0; i < ETP_MAX_FINGERS; i++) {
		if ((fFingerMask & (1 << i)) == 0)
			continue;
		sumX += fFingers[i].x;
		sumY += fFingers[i].y;
		sumZ += fFingers[i].pressure;
		sumWidth += fFingers[i].width;

		if ((common & (1 << i)) == 0)
			continue;
		// Follow the finger that moved most, so that a thumb resting on
		// the pad does not slow down the finger dragging next to it.
		int32 dx = (int32)fFingers[i].x - (int32)fLastFingers[i].x;
		int32 dy = (int32)fFingers[i].y - (int32)fLastFingers[i].y;
		if (abs(dx) + abs(dy) > moved) {
			moved = abs(dx) + abs(dy);
			deltaX = dx;
			deltaY = dy;
		}
	}

	if (count > 0 && fLastFingerMask == 0) {
		fPositionX = sumX / count;
		fPositionY = sumY / count;
	} else {
		// When every finger was swapped for another at once, there is
		// nothing to follow; hold the position.
		fPositionX = min_c(max_c(fPositionX + deltaX, 0), (int32)fMaxX);
		fPositionY = min_c(max_c(fPositionY + deltaY, 0), (int32)fMaxY);
	}

	memcpy(fLastFingers, fFingers, sizeof(fLastFingers));
	fLastFingerMask = fFingerMask;

	movement->fingers = fFingerMask;
	if (count == 0) {
		// A width of 0 or 1 would read as two or three fingers.
		movement->fingerWidth = 4;
		return;
	}

	// movement_maker reads 0, 0 as no touch.
	movement->xPosition = max_c(fPositionX, 1);
	movement->yPosition = fPositionY;
	// movement_maker ignores contacts at maxPressure and up.
	movement->zPressure = min_c(sumZ / count, 254);
	// movement_maker follows the Synaptics width scale: 4 to 7 is a
	// finger, 8 and up a palm, 0 and 1 mean two and three fingers.
	if (count == 1)
		movement->fingerWidth = min_c(4 + sumWidth / 3, 15);
	else
		movement->fingerWidth = count == 2 ? 0 : 1;
}


/*!	Counts the fingers of a click. A thumb pressing the bottom of the pad
	while another finger rests higher up is a one finger click, so that a
	thumb can click and the other finger drag. Returns the finger that
	presses the pad: the lowest.
*/
int
ELANDevice::_ClickFingers(int count, const elan_finger** presser) const
{
	const elan_finger* lowest = NULL;
	int low = 0;
	for (int i = 0; i < ETP_MAX_FINGERS; i++) {
		if ((fFingerMask & (1 << i)) == 0)
			continue;
		// Elan Y grows upwards, as movement_maker expects.
		if (lowest == NULL || fFingers[i].y < lowest->y)
			lowest = &fFingers[i];
		if (fFingers[i].y < fMaxY / 5)
			low++;
	}

	*presser = lowest;
	if (count == 2 && low == 1)
		return 1;
	return count;
}


//!	Turns the single button under the pad into right and middle buttons too.
bool
ELANDevice::_EmulateButtons(touchpad_movement* movement, int count)
{
	bigtime_t now = system_time();

	if (fIgnoreContact) {
		// The rest of a touch that was already reported as a tap.
		if (count == 0)
			_ResetContact();
		return false;
	}

	if (count > 0 && fLastCount == 0) {
		fContactStart = now;
		fContactMaxFingers = 0;
		fContactMoved = false;
		fContactClicked = false;
	}
	fLastCount = count;

	if (count >= 2) {
		uint32 slop = max_c(fMaxX / 30, 1);
		if (fContactMaxFingers < 2) {
			fTapX = movement->xPosition;
			fTapY = movement->yPosition;
		} else if (abs((int32)movement->xPosition - (int32)fTapX) > (int32)slop
			|| abs((int32)movement->yPosition - (int32)fTapY) > (int32)slop) {
			fContactMoved = true;
		}
	}
	fContactMaxFingers = max_c(fContactMaxFingers, count);

	// The pad clicks as one button. Like Chrome OS, a click with two
	// fingers, or with one finger in the bottom right corner, is a right
	// click, and one with three fingers a middle click. The button is
	// chosen when the pad goes down and kept until it comes up again.
	if ((movement->buttons & BUTTON_LEFT) != 0) {
		fContactClicked = true;
		if (fClickButton == 0) {
			const elan_finger* presser;
			int fingers = _ClickFingers(count, &presser);
			if (fingers >= 3)
				fClickButton = BUTTON_MIDDLE;
			else if (fingers == 2)
				fClickButton = BUTTON_RIGHT;
			else if (fingers == 1 && presser->y < fMaxY / 5
				&& presser->x > fMaxX / 2)
				fClickButton = BUTTON_RIGHT;
			else
				fClickButton = BUTTON_LEFT;
		}
		movement->buttons = (movement->buttons & ~BUTTON_LEFT) | fClickButton;
	} else
		fClickButton = 0;

	// After a click, the rest of the touch is one finger to movement_maker,
	// even with more on the pad: it would scroll instead of drag, and turn
	// a held left button into the right one when a second finger comes.
	if (fContactClicked && count >= 2) {
		movement->fingers &= -movement->fingers;
		movement->fingerWidth = 5;
	}

	// Tapping with two fingers is a right click, with three a middle click.
	// It is reported as soon as the fingers start to lift; the rest of the
	// touch is dropped so movement_maker does not see a one finger tap.
	if (fContactMaxFingers >= 2 && count < 2 && !fContactMoved
		&& !fContactClicked && now - fContactStart < kMultiFingerTapTimeout) {
		uint32 button = fContactMaxFingers == 2 ? BUTTON_RIGHT : BUTTON_MIDDLE;
		_ResetContact();
		fIgnoreContact = count > 0;
		fLastCount = count;
		memset(movement, 0, sizeof(*movement));
		movement->buttons = button;
		movement->fingerWidth = 4;
		fPendingRelease = true;
	}

	return true;
}


void
ELANDevice::_ResetContact()
{
	fClickButton = 0;
	fLastCount = 0;
	fContactMaxFingers = 0;
	fContactStart = 0;
	fTapX = fTapY = 0;
	fContactMoved = false;
	fContactClicked = false;
	fIgnoreContact = false;
	fPendingRelease = false;
}


//	#pragma mark - I2C


status_t
ELANDevice::_ReadRegister(uint16 reg, void* value, size_t length)
{
	uint8 cmd[2] = { (uint8)(reg & 0xff), (uint8)(reg >> 8) };
	return _Exec(cmd, sizeof(cmd), value, length);
}


status_t
ELANDevice::_WriteRegister(uint16 reg, uint16 value)
{
	uint8 cmd[4] = { (uint8)(reg & 0xff), (uint8)(reg >> 8),
		(uint8)(value & 0xff), (uint8)(value >> 8) };
	return _Exec(cmd, sizeof(cmd), NULL, 0);
}


status_t
ELANDevice::_Receive(void* buffer, size_t length)
{
	return _Exec(NULL, 0, buffer, length);
}


status_t
ELANDevice::_Exec(const uint8* cmd, size_t cmdLength, void* buffer,
	size_t length)
{
	status_t status = fI2C->acquire_bus(fI2CCookie);
	if (status != B_OK)
		return status;
	status = fI2C->exec_command(fI2CCookie,
		length > 0 ? I2C_OP_READ_STOP : I2C_OP_WRITE_STOP, cmd, cmdLength,
		buffer, length);
	fI2C->release_bus(fI2CCookie);
	return status;
}
