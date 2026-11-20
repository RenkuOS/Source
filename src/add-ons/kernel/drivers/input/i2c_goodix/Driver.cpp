/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	Driver for Goodix GT9xx touch screen controllers on an I2C bus (ACPI
	GDIX1001, GDIX1002, GODX0911), as found in tablets with Intel Atom SoCs.

	These controllers do not speak HID over I2C; they have registers with
	16-bit big endian addresses. The driver polls the status register, reads
	the first contact and publishes the screen as a tablet device, which the
	tablet add-on of the input_server turns into absolute pointer events.

	The ACPI _PS0 method of the device powers it up and pulses its reset line.
	On Bay Trail that line is a GPIO, so the baytrail_gpio driver has to be
	loaded first; the driver retries until the controller answers.

	Gestures follow the usual convention of touch screens on desktop systems:
	a tap is a click of the primary button and two taps are a double click; a
	finger that moves drags with the primary button; a finger that is held
	still for a while is a click of the secondary button (context menu).
	Because a contact can only be classified once the finger moves, is lifted
	or has been held long enough, the button is pressed at that moment, at
	the place where the finger first touched.

	Settings (in the "i2c_goodix" driver settings file):
		swap_xy, invert_x, invert_y		to match the screen orientation
*/


#include <ACPI.h>
#include <Drivers.h>
#include <Errors.h>
#include <KernelExport.h>
#include <driver_settings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <baytrail_gpio.h>
#include <condition_variable.h>
#include <i2c.h>
#include <kernel.h>
#include <keyboard_mouse_driver.h>
#include <lock.h>
#include <util/AutoLock.h>


#define GOODIX_DRIVER_NAME	"drivers/input/i2c_goodix/driver_v1"
#define GOODIX_DEVICE_NAME	"drivers/input/i2c_goodix/device_v1"
#define GOODIX_PUBLISH_PATH	"input/tablet/goodix/%" B_PRId32
#define GOODIX_PATH_ID		"i2c_goodix/path_id"

//#define TRACE_GOODIX
#ifdef TRACE_GOODIX
#	define TRACE(x...)	dprintf("i2c_goodix: " x)
#else
#	define TRACE(x...)
#endif
#define ERROR(x...)	dprintf("i2c_goodix: " x)


// Registers
#define GOODIX_REG_CONFIG_VERSION	0x8047
#define GOODIX_REG_PRODUCT_ID		0x8140
#define GOODIX_REG_STATUS			0x814e
#define GOODIX_REG_POINTS			0x814f

#define GOODIX_STATUS_READY			0x80
#define GOODIX_STATUS_TOUCHES		0x0f
#define GOODIX_MAX_TOUCHES			10
#define GOODIX_POINT_SIZE			8

// mouse buttons, as in InterfaceDefs.h
#define BUTTON_PRIMARY				0x01
#define BUTTON_SECONDARY			0x02

static const bigtime_t kPollInterval = 10000;
static const bigtime_t kIdlePollInterval = 100000;
static const bigtime_t kRetryInterval = 1000000;
static const uint32 kMaxErrors = 20;
static const int32 kQueueSize = 32;
static const float kTapDistance = 0.02f;
	// how far a finger may move and still be a tap, and how far apart the
	// taps of a double tap may be, relative to the screen size
static const bigtime_t kLongPressTime = 800000;
static const bigtime_t kClickHoldTime = 60000;
	// how long a tap holds the button down: applications that poll the
	// mouse, like menus while they track, miss a press and release that
	// arrive at the same time


enum gesture_state {
	GESTURE_NONE,		// no contact
	GESTURE_PENDING,	// contact, not classified yet
	GESTURE_DRAG,		// moving with the primary button pressed
	GESTURE_DONE,		// long press reported, waiting for the release
};

struct goodix_device {
	device_node*			node;
	i2c_device_interface*	i2c;
	i2c_device				i2cCookie;
	acpi_handle				handle;

	// settings
	bool					swapXY;
	bool					invertX;
	bool					invertY;

	// controller state, only touched by the poll thread
	bool					ready;
	uint32					maxX;
	uint32					maxY;
	uint32					errors;
	gesture_state			state;
	bigtime_t				contactTime;
	float					startX;
	float					startY;
	float					lastX;
	float					lastY;
	float					lastClickX;
	float					lastClickY;
	bigtime_t				lastClickTime;
	int32					clickCount;

	thread_id				thread;
	volatile bool			stopThread;

	// protected by lock
	mutex					lock;
	ConditionVariable		condition;
	tablet_movement			queue[kQueueSize];
	int32					queueStart;
	int32					queueCount;
	int32					openCount;
	bigtime_t				clickSpeed;
};

struct goodix_cookie {
	goodix_device*			device;
	bool					closed;
};


static device_manager_info* sDeviceManager;


static inline float
distance(float delta)
{
	return delta < 0 ? -delta : delta;
}


static bool
is_goodix_name(const char* name)
{
	return name != NULL && (strcmp(name, "GDIX1001") == 0
		|| strcmp(name, "GDIX1002") == 0 || strcmp(name, "GODX0911") == 0);
}


/*!	Configuration of the GT9271 in the Teclast X98 Air III (1536x2048).

	The tablet can boot a 32-bit UEFI firmware (for Windows) or a 64-bit one
	(for Android). With the 32-bit firmware the controller runs and reports
	this configuration. With the 64-bit firmware it answers on the bus, but
	its firmware does not run and all configuration registers read as zero:
	the Android driver resets the controller itself and sends a configuration
	on every boot. The driver then does the same, see reset_with_int_sync().

	These are the registers 0x8047 to 0x80fe as read back under the 32-bit
	firmware; the checksum (0x80ff) and the "config fresh" flag (0x8100) are
	added when it is sent.
*/
static const uint8 kGT9271Config[] = {
	0x41, 0x00, 0x06, 0x00, 0x08, 0x0a, 0x35, 0x00, 0x01, 0x0a, 0x28, 0x0f,
	0x5a, 0x3c, 0x03, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x18,
	0x1a, 0x1e, 0x14, 0x90, 0x30, 0xaa, 0x3a, 0x38, 0xee, 0x06, 0x00, 0x00,
	0x00, 0x03, 0x02, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x1e, 0x50, 0x94, 0xc5, 0x02, 0x07, 0x00, 0x00, 0x04,
	0xc7, 0x21, 0x00, 0xa9, 0x28, 0x00, 0x8e, 0x31, 0x00, 0x7b, 0x3b, 0x00,
	0x6a, 0x48, 0x00, 0x6a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x22,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
	0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x1b,
	0x1c, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0f, 0x0e, 0x0d, 0x0c, 0x0a,
	0x08, 0x07, 0x06, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
};


//	#pragma mark - controller access


static status_t
read_register(goodix_device* device, uint16 reg, void* buffer, size_t length)
{
	uint8 cmd[2] = { (uint8)(reg >> 8), (uint8)reg };

	status_t status = device->i2c->acquire_bus(device->i2cCookie);
	if (status != B_OK)
		return status;
	status = device->i2c->exec_command(device->i2cCookie, I2C_OP_READ_STOP,
		cmd, sizeof(cmd), buffer, length);
	device->i2c->release_bus(device->i2cCookie);
	return status;
}


static status_t
write_register(goodix_device* device, uint16 reg, uint8 value)
{
	uint8 cmd[3] = { (uint8)(reg >> 8), (uint8)reg, value };

	status_t status = device->i2c->acquire_bus(device->i2cCookie);
	if (status != B_OK)
		return status;
	status = device->i2c->exec_command(device->i2cCookie, I2C_OP_WRITE_STOP,
		cmd, sizeof(cmd), NULL, 0);
	device->i2c->release_bus(device->i2cCookie);
	return status;
}


/*!	Writes \a length bytes to consecutive registers from \a reg on. The bus
	driver does not wait for room in the transmit FIFO of the controller, so
	this writes small pieces that fit in it, each as its own transfer.
*/
static status_t
write_registers(goodix_device* device, uint16 reg, const void* data,
	size_t length)
{
	const size_t kChunkSize = 16;
	const uint8* bytes = (const uint8*)data;

	while (length > 0) {
		size_t chunk = min_c(length, kChunkSize);
		uint8 cmd[2 + kChunkSize];
		cmd[0] = (uint8)(reg >> 8);
		cmd[1] = (uint8)reg;
		memcpy(cmd + 2, bytes, chunk);

		status_t status = device->i2c->acquire_bus(device->i2cCookie);
		if (status != B_OK)
			return status;
		status = device->i2c->exec_command(device->i2cCookie,
			I2C_OP_WRITE_STOP, cmd, 2 + chunk, NULL, 0);
		device->i2c->release_bus(device->i2cCookie);
		if (status != B_OK)
			return status;

		reg += chunk;
		bytes += chunk;
		length -= chunk;
	}
	return B_OK;
}


/*!	Sends \a config (the registers from GOODIX_REG_CONFIG_VERSION up to the
	checksum) to the controller, followed by its checksum and the flag that
	makes the controller apply it. The flag is written last, so it does not
	matter that the configuration is written in pieces.
*/
static status_t
send_config(goodix_device* device, const uint8* config, size_t length)
{
	uint8 data[sizeof(kGT9271Config) + 2];
	if (length > sizeof(data) - 2)
		return B_BAD_VALUE;

	uint8 sum = 0;
	for (size_t i = 0; i < length; i++)
		sum += config[i];

	memcpy(data, config, length);
	data[length] = (uint8)(~sum + 1);
	data[length + 1] = 1;

	status_t status = write_registers(device, GOODIX_REG_CONFIG_VERSION, data,
		length + 2);
	if (status != B_OK)
		return status;

	// the controller clears the flag once it has taken the configuration
	snooze(100000);
	uint8 fresh;
	status = read_register(device, GOODIX_REG_CONFIG_VERSION + length + 1,
		&fresh, 1);
	if (status == B_OK && fresh != 0) {
		ERROR("the controller did not take the configuration\n");
		status = B_ERROR;
	}
	return status;
}


/*!	Resets the controller the way the Goodix drivers of Linux and Android do
	for address 0x14: reset low, INT high, reset high, then INT low for 50 ms
	("INT sync") before INT becomes an input again. Without the INT sync the
	firmware of the controller does not start.

	The ACPI tables of the Teclast X98 Air III name a wrong INT pin, so this
	uses the pins that Android uses: reset NCORE 26, INT SUS 3.
*/
static status_t
reset_with_int_sync()
{
	baytrail_gpio_pins_module_info* gpio;
	status_t status = get_module(BAYTRAIL_GPIO_PINS_MODULE_NAME,
		(module_info**)&gpio);
	if (status != B_OK)
		return status;

	status = gpio->set_output("NCORE", 26, false);
	if (status == B_OK) {
		snooze(20000);
		gpio->set_output("SUS", 3, true);
		snooze(2000);
		gpio->set_output("NCORE", 26, true);
		snooze(6000);
		gpio->set_output("SUS", 3, false);
		snooze(50000);
		gpio->set_input("SUS", 3);
		snooze(100000);
	}

	put_module(BAYTRAIL_GPIO_PINS_MODULE_NAME);
	return status;
}


/*!	Powers the controller up through ACPI and reads its identification and
	resolution.
*/
static status_t
start_controller(goodix_device* device)
{
	acpi_module_info* acpi;
	if (device->handle != NULL
		&& get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi) == B_OK) {
		// _PS0 is optional, a missing method is fine
		acpi->evaluate_method(device->handle, "_PS0", NULL, NULL);
		put_module(B_ACPI_MODULE_NAME);
		snooze(50000);
	}

	uint8 id[6];
	status_t status = read_register(device, GOODIX_REG_PRODUCT_ID, id,
		sizeof(id));
	if (status != B_OK)
		return status;

	uint8 config[5];
	status = read_register(device, GOODIX_REG_CONFIG_VERSION, config,
		sizeof(config));
	if (status != B_OK)
		return status;

	device->maxX = config[1] | (config[2] << 8);
	device->maxY = config[3] | (config[4] << 8);

	if (config[0] == 0 && device->maxX == 0 && device->maxY == 0
		&& memcmp(id, "9271", 4) == 0) {
		// the firmware of the controller does not run, see kGT9271Config
		dprintf("i2c_goodix: controller has no configuration, resetting it "
			"and sending one\n");
		status = reset_with_int_sync();
		if (status == B_OK)
			status = send_config(device, kGT9271Config, sizeof(kGT9271Config));
		if (status == B_OK) {
			status = read_register(device, GOODIX_REG_CONFIG_VERSION, config,
				sizeof(config));
		}
		if (status != B_OK) {
			ERROR("sending the configuration failed: %s\n", strerror(status));
			return status;
		}
		device->maxX = config[1] | (config[2] << 8);
		device->maxY = config[3] | (config[4] << 8);
	}

	if (device->maxX == 0 || device->maxY == 0) {
		ERROR("controller reports no resolution\n");
		return B_BAD_DATA;
	}

	dprintf("i2c_goodix: GT%c%c%c%c, firmware %02x%02x, config %#x, "
		"%" B_PRIu32 "x%" B_PRIu32 "\n", id[0], id[1], id[2], id[3], id[5],
		id[4], config[0], device->maxX, device->maxY);

	// discard a report that may be pending
	write_register(device, GOODIX_REG_STATUS, 0);
	return B_OK;
}


//	#pragma mark - event queue


/*!	Queues the state of the pointer: \a buttons is a mask of BUTTON_PRIMARY
	and BUTTON_SECONDARY, \a clicks the click count of a button press.
*/
static void
queue_movement(goodix_device* device, uint32 buttons, int32 clicks, float x,
	float y)
{
	tablet_movement movement;
	memset(&movement, 0, sizeof(movement));
	movement.xpos = x;
	movement.ypos = y;
	// The tablet add-on only sends events while there is contact, so a
	// release is reported as a contact without a button.
	movement.has_contact = true;
	// the tablet add-on maps the tip switch to the primary button
	movement.switches = (buttons & BUTTON_PRIMARY) != 0 ? B_TIP_SWITCH : 0;
	movement.buttons = buttons & BUTTON_SECONDARY;
	movement.pressure = buttons != 0 ? 1.0f : 0.0f;
	movement.clicks = clicks;
	movement.timestamp = system_time();

	MutexLocker locker(device->lock);

	if (device->queueCount == kQueueSize) {
		// drop the oldest event
		device->queueStart = (device->queueStart + 1) % kQueueSize;
		device->queueCount--;
	}
	int32 index = (device->queueStart + device->queueCount) % kQueueSize;
	device->queue[index] = movement;
	device->queueCount++;

	device->condition.NotifyAll();
}


static bigtime_t
click_speed(goodix_device* device)
{
	MutexLocker locker(device->lock);
	return device->clickSpeed;
}


/*!	Presses the primary button where the finger first touched, counting
	clicks for double clicks.
*/
static void
press_primary(goodix_device* device)
{
	bigtime_t now = system_time();
	float x = device->startX;
	float y = device->startY;

	if (device->lastClickTime + click_speed(device) > now
		&& distance(x - device->lastClickX) < kTapDistance
		&& distance(y - device->lastClickY) < kTapDistance) {
		// put the second tap exactly on the first one
		device->clickCount++;
		x = device->lastClickX;
		y = device->lastClickY;
		device->startX = x;
		device->startY = y;
	} else
		device->clickCount = 1;

	device->lastClickTime = now;
	device->lastClickX = x;
	device->lastClickY = y;

	queue_movement(device, BUTTON_PRIMARY, device->clickCount, x, y);
}


/*!	Reports a long press as a click of the secondary button. */
static void
check_long_press(goodix_device* device)
{
	if (device->state != GESTURE_PENDING
		|| system_time() - device->contactTime < kLongPressTime) {
		return;
	}

	queue_movement(device, BUTTON_SECONDARY, 1, device->startX,
		device->startY);
	snooze(kClickHoldTime);
	queue_movement(device, 0, 0, device->startX, device->startY);
	device->lastClickTime = 0;
	device->state = GESTURE_DONE;
}


/*!	Reads one report and turns it into tablet movements. */
static status_t
poll_controller(goodix_device* device)
{
	uint8 status;
	status_t result = read_register(device, GOODIX_REG_STATUS, &status, 1);
	if (result != B_OK)
		return result;

	if ((status & GOODIX_STATUS_READY) == 0)
		return B_OK;

	uint32 touches = status & GOODIX_STATUS_TOUCHES;
	if (touches > GOODIX_MAX_TOUCHES)
		touches = 0;

	uint8 point[GOODIX_POINT_SIZE];
	if (touches > 0) {
		result = read_register(device, GOODIX_REG_POINTS, point,
			sizeof(point));
	}

	// tell the controller that the report has been read
	write_register(device, GOODIX_REG_STATUS, 0);

	if (result != B_OK)
		return result;

	if (touches == 0) {
		// the finger has been lifted
		switch (device->state) {
			case GESTURE_PENDING:
				// a tap
				press_primary(device);
				snooze(kClickHoldTime);
				queue_movement(device, 0, 0, device->startX, device->startY);
				break;
			case GESTURE_DRAG:
				queue_movement(device, 0, 0, device->lastX, device->lastY);
				break;
			default:
				break;
		}
		device->state = GESTURE_NONE;
		return B_OK;
	}

	uint32 rawX = point[1] | (point[2] << 8);
	uint32 rawY = point[3] | (point[4] << 8);
	uint32 maxX = device->maxX;
	uint32 maxY = device->maxY;
	if (device->swapXY) {
		uint32 temp = rawX;
		rawX = rawY;
		rawY = temp;
		maxX = device->maxY;
		maxY = device->maxX;
	}

	float x = (float)rawX / (maxX > 1 ? maxX - 1 : 1);
	float y = (float)rawY / (maxY > 1 ? maxY - 1 : 1);
	if (x > 1.0f)
		x = 1.0f;
	if (y > 1.0f)
		y = 1.0f;
	if (device->invertX)
		x = 1.0f - x;
	if (device->invertY)
		y = 1.0f - y;

	switch (device->state) {
		case GESTURE_NONE:
			// a new contact: move the pointer there, without a button
			device->state = GESTURE_PENDING;
			device->contactTime = system_time();
			device->startX = x;
			device->startY = y;
			queue_movement(device, 0, 0, x, y);
			break;

		case GESTURE_PENDING:
			if (distance(x - device->startX) >= kTapDistance
				|| distance(y - device->startY) >= kTapDistance) {
				// the finger moves: drag from where it started
				press_primary(device);
				device->state = GESTURE_DRAG;
				queue_movement(device, BUTTON_PRIMARY, 0, x, y);
			}
			break;

		case GESTURE_DRAG:
			if (x != device->lastX || y != device->lastY)
				queue_movement(device, BUTTON_PRIMARY, 0, x, y);
			break;

		case GESTURE_DONE:
			break;
	}

	device->lastX = x;
	device->lastY = y;
	return B_OK;
}


static int32
poll_thread(void* data)
{
	goodix_device* device = (goodix_device*)data;

	while (!device->stopThread) {
		if (!device->ready) {
			if (start_controller(device) == B_OK) {
				device->ready = true;
				device->errors = 0;
			} else {
				snooze(kRetryInterval);
				continue;
			}
		}

		mutex_lock(&device->lock);
		bool open = device->openCount > 0;
		mutex_unlock(&device->lock);
		if (!open) {
			snooze(kIdlePollInterval);
			continue;
		}

		if (poll_controller(device) != B_OK) {
			if (++device->errors >= kMaxErrors) {
				ERROR("controller does not answer, restarting it\n");
				device->ready = false;
			}
		} else
			device->errors = 0;

		check_long_press(device);
		snooze(kPollInterval);
	}

	return 0;
}


//	#pragma mark - device module API


static status_t
goodix_init_device(void* driverCookie, void** _cookie)
{
	*_cookie = driverCookie;
	return B_OK;
}


static void
goodix_uninit_device(void* cookie)
{
}


static status_t
goodix_open(void* initCookie, const char* path, int flags, void** _cookie)
{
	goodix_device* device = (goodix_device*)initCookie;

	goodix_cookie* cookie = (goodix_cookie*)calloc(1, sizeof(goodix_cookie));
	if (cookie == NULL)
		return B_NO_MEMORY;
	cookie->device = device;

	MutexLocker locker(device->lock);
	if (device->openCount++ == 0) {
		// start with an empty queue
		device->queueStart = 0;
		device->queueCount = 0;
	}

	*_cookie = cookie;
	return B_OK;
}


static status_t
goodix_close(void* _cookie)
{
	goodix_cookie* cookie = (goodix_cookie*)_cookie;
	goodix_device* device = cookie->device;

	MutexLocker locker(device->lock);
	cookie->closed = true;
	device->openCount--;
	device->condition.NotifyAll(B_FILE_ERROR);
	return B_OK;
}


static status_t
goodix_free(void* cookie)
{
	free(cookie);
	return B_OK;
}


static status_t
goodix_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_SUPPORTED;
}


static status_t
goodix_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_SUPPORTED;
}


static status_t
goodix_control(void* _cookie, uint32 op, void* buffer, size_t length)
{
	goodix_cookie* cookie = (goodix_cookie*)_cookie;
	goodix_device* device = cookie->device;

	switch (op) {
		case B_GET_DEVICE_NAME:
		{
			static const char kName[] = "Goodix Touch Screen";
			if (!IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return user_strlcpy((char*)buffer, kName, length) >= 0
				? B_OK : B_BAD_ADDRESS;
		}

		case MS_READ:
		{
			if (length < sizeof(tablet_movement))
				return B_BUFFER_OVERFLOW;
			if (!IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;

			MutexLocker locker(device->lock);
			while (device->queueCount == 0) {
				if (cookie->closed)
					return B_FILE_ERROR;
				status_t status = device->condition.Wait(&device->lock,
					B_CAN_INTERRUPT);
				if (status == B_INTERRUPTED)
					return status;
			}

			tablet_movement movement = device->queue[device->queueStart];
			device->queueStart = (device->queueStart + 1) % kQueueSize;
			device->queueCount--;
			locker.Unlock();

			if (user_memcpy(buffer, &movement, sizeof(movement)) != B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case MS_NUM_EVENTS:
		{
			MutexLocker locker(device->lock);
			return device->queueCount;
		}

		case MS_SET_CLICKSPEED:
		{
			bigtime_t clickSpeed;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&clickSpeed, buffer, sizeof(clickSpeed))
					!= B_OK) {
				return B_BAD_ADDRESS;
			}
			MutexLocker locker(device->lock);
			device->clickSpeed = clickSpeed;
			return B_OK;
		}
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver module API


static float
goodix_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "i2c") != 0) {
		return 0.0;
	}

	const char* name;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &name,
			false) == B_OK && is_goodix_name(name)) {
		return 0.8;
	}

	return 0.0;
}


static status_t
goodix_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Goodix touch screen" }},
		// Keep the controller and the I2C bus initialized: unloading and
		// reloading the stack under a powered touch controller made the
		// boot hang on the Teclast X98 Air III.
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, GOODIX_DRIVER_NAME, attrs,
		NULL, NULL);
}


static status_t
goodix_init_driver(device_node* node, void** _driverCookie)
{
	goodix_device* device = (goodix_device*)calloc(1, sizeof(goodix_device));
	if (device == NULL)
		return B_NO_MEMORY;

	device->node = node;
	device->clickSpeed = 500000;
	mutex_init(&device->lock, "i2c_goodix");
	device->condition.Init(device, "i2c_goodix report");

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&device->i2c,
		(void**)&device->i2cCookie);
	uint64 handle = 0;
	if (sDeviceManager->get_attr_uint64(parent, ACPI_DEVICE_HANDLE_ITEM,
			&handle, false) == B_OK) {
		device->handle = (acpi_handle)(addr_t)handle;
	}
	sDeviceManager->put_node(parent);

	void* settings = load_driver_settings("i2c_goodix");
	if (settings != NULL) {
		device->swapXY = get_driver_boolean_parameter(settings, "swap_xy",
			false, true);
		device->invertX = get_driver_boolean_parameter(settings, "invert_x",
			false, true);
		device->invertY = get_driver_boolean_parameter(settings, "invert_y",
			false, true);
		unload_driver_settings(settings);
	}

	device->thread = spawn_kernel_thread(poll_thread, "i2c_goodix poll",
		B_DISPLAY_PRIORITY, device);
	if (device->thread < 0) {
		status_t status = device->thread;
		mutex_destroy(&device->lock);
		free(device);
		return status;
	}
	resume_thread(device->thread);

	*_driverCookie = device;
	return B_OK;
}


static void
goodix_uninit_driver(void* driverCookie)
{
	goodix_device* device = (goodix_device*)driverCookie;

	device->stopThread = true;
	status_t result;
	wait_for_thread(device->thread, &result);

	mutex_destroy(&device->lock);
	free(device);
}


static status_t
goodix_register_child_devices(void* driverCookie)
{
	goodix_device* device = (goodix_device*)driverCookie;

	int32 pathID = sDeviceManager->create_id(GOODIX_PATH_ID);
	if (pathID < 0)
		return pathID;

	char path[B_DEV_NAME_LENGTH];
	snprintf(path, sizeof(path), GOODIX_PUBLISH_PATH, pathID);

	return sDeviceManager->publish_device(device->node, path,
		GOODIX_DEVICE_NAME);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


static driver_module_info sGoodixDriverModule = {
	{
		GOODIX_DRIVER_NAME,
		0,
		NULL
	},

	goodix_supports_device,
	goodix_register_device,
	goodix_init_driver,
	goodix_uninit_driver,
	goodix_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


static struct device_module_info sGoodixDeviceModule = {
	{
		GOODIX_DEVICE_NAME,
		0,
		NULL
	},

	goodix_init_device,
	goodix_uninit_device,
	NULL,	// removed

	goodix_open,
	goodix_close,
	goodix_free,
	goodix_read,
	goodix_write,
	NULL,	// io
	goodix_control,

	NULL,	// select
	NULL,	// deselect
};


module_info* modules[] = {
	(module_info*)&sGoodixDriverModule,
	(module_info*)&sGoodixDeviceModule,
	NULL
};
