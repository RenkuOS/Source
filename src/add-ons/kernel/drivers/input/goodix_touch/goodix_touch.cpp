/*
 * Copyright 2026, Ian Dumych. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */

/*	Goodix GT9xx touchscreen controllers on an Allwinner A64 I2C (TWI)
	controller, as the GT917S of the PINE64 PinePhone.

	A device manager driver: it attaches to the controller's node in the
	device tree (a child of its I2C controller's node), drives that TWI
	controller itself as a polled master (Haiku's I2C bus manager only knows
	ACPI), resets the chip through its reset and interrupt lines to select
	its address as Linux' goodix.c does, and polls its coordinate register
	at 100 Hz while the device is open. The panel's supplies (AXP803 DLDO1
	and GPIO0 LDO) are switched on by the boot loader.

	The first contact is published as an absolute pointing device in the
	tablet protocol (input/tablet/goodix/0), which input_server's tablet
	add-on turns into mouse messages: touching presses the primary button,
	lifting releases it. */


#include <Drivers.h>
#include <KernelExport.h>

#include <new>
#include <stdio.h>
#include <string.h>

#include <ByteOrder.h>
#include <bus/FDT.h>
#include <condition_variable.h>
#include <device_manager.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>

#include <keyboard_mouse_driver.h>


//#define TRACE_GOODIX
#ifdef TRACE_GOODIX
#	define TRACE(x...) dprintf("goodix_touch: " x)
#else
#	define TRACE(x...) ;
#endif
#define INFO(x...)	dprintf("goodix_touch: " x)
#define ERROR(x...)	dprintf("goodix_touch: " x)


#define DRIVER_MODULE_NAME	"drivers/input/goodix_touch/driver_v1"
#define DEVICE_MODULE_NAME	"drivers/input/goodix_touch/device_v1"
#define DEVICE_PATH			"input/tablet/goodix/0"


// A64: clock controller and PIO share a page
static const phys_addr_t kCcuBase = 0x01c20000;
static const phys_addr_t kPioOffset = 0x800;
static const uint32 kCcuBusGate3 = 0x06c;
static const uint32 kCcuBusReset4 = 0x2d8;
static const phys_addr_t kTwi0Base = 0x01c2ac00;
static const uint32 kPortH = 7;

// TWI registers (Allwinner's layout of the Marvell mv64xxx controller)
#define TWI_DATA			0x08
#define TWI_CNTR			0x0c
#define TWI_STAT			0x10
#define TWI_CCR				0x14
#define TWI_SRST			0x18
#define TWI_EFR				0x1c
#define TWI_XADDR			0x04
#define TWI_ADDR			0x00

#define CNTR_ACK			(1u << 2)
#define CNTR_IFLG			(1u << 3)
#define CNTR_STOP			(1u << 4)
#define CNTR_START			(1u << 5)
#define CNTR_BUS_EN			(1u << 6)

#define STAT_START			0x08
#define STAT_REPEAT_START	0x10
#define STAT_WRITE_ADDR_ACK	0x18
#define STAT_WRITE_DATA_ACK	0x28
#define STAT_READ_ADDR_ACK	0x40
#define STAT_READ_DATA_ACK	0x50
#define STAT_READ_DATA_NACK	0x58

// Goodix GT9xx registers
#define GOODIX_REG_CONFIG	0x8047
#define GOODIX_REG_ID		0x8140
#define GOODIX_REG_STATUS	0x814e
#define GOODIX_STATUS_READY	0x80
#define GOODIX_CONTACT_SIZE	8
#define GOODIX_MAX_CONTACTS	10

static const bigtime_t kPollInterval = 10000;


static device_manager_info* sDeviceManager;


struct goodix_device {
	device_node*		node;

	area_id				twiArea;
	volatile uint8*		twi;
	area_id				ccuArea;
	volatile uint8*		ccu;
	uint32				twiIndex;
	uint32				clockDivider;

	uint8				address;
	int32				interruptPin;	// on port H, -1 if unknown
	int32				resetPin;

	uint32				sizeX;
	uint32				sizeY;
	bool				swapXY;
	bool				invertX;
	bool				invertY;

	mutex				lock;
	ConditionVariable	condition;
	int32				openCount;
	thread_id			thread;
	bool				running;

	// the latest report and how many there were
	tablet_movement		movement;
	uint32				generation;
	bool				touching;
	bigtime_t			clickSpeed;
	bigtime_t			lastClickTime;
	int32				clickCount;
	uint32				errors;
};


struct goodix_cookie {
	goodix_device*		device;
	uint32				generation;
	bool				closed;
};


// #pragma mark - GPIO and clocks


static inline uint32
ccu_read(goodix_device* device, uint32 offset)
{
	return *(volatile uint32*)(device->ccu + offset);
}


static inline void
ccu_write(goodix_device* device, uint32 offset, uint32 value)
{
	*(volatile uint32*)(device->ccu + offset) = value;
}


static void
pio_configure(goodix_device* device, uint32 port, uint32 pin,
	uint32 function)
{
	uint32 offset = kPioOffset + port * 0x24 + (pin / 8) * 4;
	uint32 shift = (pin % 8) * 4;
	ccu_write(device, offset,
		(ccu_read(device, offset) & ~(0x7u << shift)) | (function << shift));
}


static void
pio_set(goodix_device* device, uint32 port, uint32 pin, bool high)
{
	uint32 offset = kPioOffset + port * 0x24 + 0x10;
	uint32 value = ccu_read(device, offset);
	ccu_write(device, offset,
		high ? value | (1u << pin) : value & ~(1u << pin));
}


// #pragma mark - TWI


static inline uint32
twi_read(goodix_device* device, uint32 reg)
{
	return *(volatile uint32*)(device->twi + reg);
}


static inline void
twi_write(goodix_device* device, uint32 reg, uint32 value)
{
	*(volatile uint32*)(device->twi + reg) = value;
}


static void
twi_reset(goodix_device* device)
{
	twi_write(device, TWI_SRST, 1);
	for (int i = 0; i < 1000 && (twi_read(device, TWI_SRST) & 1) != 0; i++)
		spin(10);
	twi_write(device, TWI_EFR, 0);
	twi_write(device, TWI_XADDR, 0);
	twi_write(device, TWI_ADDR, 0);
	twi_write(device, TWI_CCR, device->clockDivider);
	twi_write(device, TWI_CNTR, CNTR_BUS_EN);
}


static status_t
twi_wait(goodix_device* device, uint32 expected)
{
	bigtime_t timeout = system_time() + 20000;
	while ((twi_read(device, TWI_CNTR) & CNTR_IFLG) == 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		spin(5);
	}
	return (twi_read(device, TWI_STAT) & 0xff) == expected ? B_OK : B_IO_ERROR;
}


static status_t
twi_start(goodix_device* device, bool repeated)
{
	twi_write(device, TWI_CNTR,
		CNTR_BUS_EN | CNTR_START | (repeated ? CNTR_IFLG : 0));
	return twi_wait(device, repeated ? STAT_REPEAT_START : STAT_START);
}


static status_t
twi_send(goodix_device* device, uint8 byte, uint32 expected)
{
	twi_write(device, TWI_DATA, byte);
	twi_write(device, TWI_CNTR, CNTR_BUS_EN | CNTR_IFLG);
	return twi_wait(device, expected);
}


static void
twi_stop(goodix_device* device)
{
	twi_write(device, TWI_CNTR, CNTR_BUS_EN | CNTR_STOP | CNTR_IFLG);
	bigtime_t timeout = system_time() + 10000;
	while ((twi_read(device, TWI_CNTR) & CNTR_STOP) != 0
		&& system_time() < timeout) {
		spin(5);
	}
}


/*!	Writes the 16 bit big endian register address, then \a writeLength
	bytes, or reads \a readLength bytes after a repeated start.
*/
static status_t
twi_transfer(goodix_device* device, uint16 reg, const uint8* writeData,
	size_t writeLength, uint8* readData, size_t readLength)
{
	uint8 address = device->address;
	status_t status = twi_start(device, false);
	if (status == B_OK)
		status = twi_send(device, address << 1, STAT_WRITE_ADDR_ACK);
	if (status == B_OK)
		status = twi_send(device, reg >> 8, STAT_WRITE_DATA_ACK);
	if (status == B_OK)
		status = twi_send(device, reg & 0xff, STAT_WRITE_DATA_ACK);
	for (size_t i = 0; status == B_OK && i < writeLength; i++)
		status = twi_send(device, writeData[i], STAT_WRITE_DATA_ACK);

	if (status == B_OK && readLength > 0) {
		status = twi_start(device, true);
		if (status == B_OK) {
			status = twi_send(device, (address << 1) | 1,
				STAT_READ_ADDR_ACK);
		}
		for (size_t i = 0; status == B_OK && i < readLength; i++) {
			bool last = i == readLength - 1;
			twi_write(device, TWI_CNTR,
				CNTR_BUS_EN | CNTR_IFLG | (last ? 0 : CNTR_ACK));
			status = twi_wait(device,
				last ? STAT_READ_DATA_NACK : STAT_READ_DATA_ACK);
			if (status == B_OK)
				readData[i] = twi_read(device, TWI_DATA);
		}
	}

	twi_stop(device);
	if (status != B_OK)
		twi_reset(device);
	return status;
}


static status_t
goodix_read(goodix_device* device, uint16 reg, void* data, size_t length)
{
	return twi_transfer(device, reg, NULL, 0, (uint8*)data, length);
}


static status_t
goodix_write8(goodix_device* device, uint16 reg, uint8 value)
{
	return twi_transfer(device, reg, &value, 1, NULL, 0);
}


// #pragma mark - controller


/*!	Linux' goodix_reset(): the level of the interrupt line while the chip
	comes out of reset selects its address (low: 0x5d, high: 0x14).
*/
static void
goodix_reset(goodix_device* device)
{
	if (device->resetPin < 0 || device->interruptPin < 0)
		return;

	pio_configure(device, kPortH, device->resetPin, 1);
	pio_set(device, kPortH, device->resetPin, false);
	pio_configure(device, kPortH, device->interruptPin, 1);
	pio_set(device, kPortH, device->interruptPin, false);
	snooze(20000);

	pio_set(device, kPortH, device->interruptPin, device->address == 0x14);
	spin(150);

	pio_set(device, kPortH, device->resetPin, true);
	snooze(10000);

	// "int sync": hold the line low, then let the chip drive it
	pio_set(device, kPortH, device->interruptPin, false);
	snooze(50000);
	pio_configure(device, kPortH, device->interruptPin, 0);
}


static status_t
goodix_probe(goodix_device* device)
{
	char id[7] = {};
	status_t status = goodix_read(device, GOODIX_REG_ID, id, 6);
	if (status != B_OK && device->address == 0x5d) {
		// without a reset line the firmware may have picked the other one
		device->address = 0x14;
		status = goodix_read(device, GOODIX_REG_ID, id, 6);
		if (status != B_OK)
			device->address = 0x5d;
	}
	if (status != B_OK)
		return status;

	uint8 config[5];
	status = goodix_read(device, GOODIX_REG_CONFIG, config, sizeof(config));
	if (status != B_OK)
		return status;
	uint32 configX = config[1] | (config[2] << 8);
	uint32 configY = config[3] | (config[4] << 8);
	if (device->sizeX == 0 || device->sizeY == 0) {
		device->sizeX = configX;
		device->sizeY = configY;
	}

	INFO("GT%.4s firmware %#x at %#x, config %" B_PRIu32 "x%" B_PRIu32
		", using %" B_PRIu32 "x%" B_PRIu32 "\n", id, id[4] | (id[5] << 8),
		device->address, configX, configY, device->sizeX, device->sizeY);
	return device->sizeX != 0 && device->sizeY != 0 ? B_OK : B_BAD_DATA;
}


static void
goodix_report(goodix_device* device, bool touching, uint32 x, uint32 y)
{
	MutexLocker locker(device->lock);

	bigtime_t now = system_time();
	tablet_movement& movement = device->movement;

	if (touching) {
		if (device->swapXY) {
			uint32 swap = x;
			x = y;
			y = swap;
		}
		float xpos = (float)x / device->sizeX;
		float ypos = (float)y / device->sizeY;
		xpos = xpos < 0 ? 0 : xpos > 1 ? 1 : xpos;
		ypos = ypos < 0 ? 0 : ypos > 1 ? 1 : ypos;
		if (device->invertX)
			xpos = 1 - xpos;
		if (device->invertY)
			ypos = 1 - ypos;

		if (!device->touching) {
			if (device->lastClickTime + device->clickSpeed > now)
				device->clickCount++;
			else
				device->clickCount = 1;
			device->lastClickTime = now;
		}
		movement.xpos = xpos;
		movement.ypos = ypos;
		movement.buttons = 0x1;
			// B_PRIMARY_MOUSE_BUTTON
		movement.clicks = device->clickCount;
	} else {
		if (!device->touching)
			return;
		// still in contact at the last position, without the button: the
		// tablet add-on only reports changes while in contact
		movement.buttons = 0;
		movement.clicks = 0;
	}

	device->touching = touching;
	movement.has_contact = true;
	movement.pressure = touching ? 1.0f : 0.0f;
	movement.timestamp = now;
	device->generation++;
	device->condition.NotifyAll();
}


static status_t
goodix_poll_thread(void* data)
{
	goodix_device* device = (goodix_device*)data;
	uint8 buffer[1 + GOODIX_CONTACT_SIZE];

	while (device->running) {
		snooze(kPollInterval);

		if (goodix_read(device, GOODIX_REG_STATUS, buffer, sizeof(buffer))
				!= B_OK) {
			if (device->errors++ < 5)
				ERROR("cannot read the touch status\n");
			snooze(100000);
			continue;
		}
		if ((buffer[0] & GOODIX_STATUS_READY) == 0)
			continue;

		uint32 contacts = buffer[0] & 0x0f;
		goodix_write8(device, GOODIX_REG_STATUS, 0);
		if (contacts > GOODIX_MAX_CONTACTS)
			continue;

		uint32 x = buffer[2] | (buffer[3] << 8);
		uint32 y = buffer[4] | (buffer[5] << 8);
		TRACE("%" B_PRIu32 " contacts, first at %" B_PRIu32 ",%" B_PRIu32
			"\n", contacts, x, y);
		goodix_report(device, contacts > 0, x, y);
	}
	return B_OK;
}


// #pragma mark - device


static status_t
goodix_init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static status_t
goodix_open(void* deviceCookie, const char* path, int openMode,
	void** _cookie)
{
	goodix_device* device = (goodix_device*)deviceCookie;
	goodix_cookie* cookie = new(std::nothrow) goodix_cookie;
	if (cookie == NULL)
		return B_NO_MEMORY;

	MutexLocker locker(device->lock);
	cookie->device = device;
	cookie->generation = device->generation;
	cookie->closed = false;

	if (device->openCount++ == 0) {
		device->running = true;
		device->thread = spawn_kernel_thread(goodix_poll_thread,
			"goodix touch poller", B_URGENT_DISPLAY_PRIORITY, device);
		if (device->thread < 0) {
			status_t status = device->thread;
			device->running = false;
			device->openCount--;
			delete cookie;
			return status;
		}
		resume_thread(device->thread);
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
	device->condition.NotifyAll();

	thread_id thread = -1;
	if (--device->openCount == 0) {
		device->running = false;
		thread = device->thread;
		device->thread = -1;
	}
	locker.Unlock();

	if (thread >= 0)
		wait_for_thread(thread, NULL);
	return B_OK;
}


static status_t
goodix_free(void* cookie)
{
	delete (goodix_cookie*)cookie;
	return B_OK;
}


static status_t
goodix_read_device(void* cookie, off_t position, void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_SUPPORTED;
}


static status_t
goodix_write_device(void* cookie, off_t position, const void* buffer,
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
			static const char kName[] = "Goodix Touchscreen";
			if (!IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return user_strlcpy((char*)buffer, kName, length) >= 0
				? B_OK : B_BAD_ADDRESS;
		}

		case MS_READ:
		{
			if (length < sizeof(tablet_movement))
				return B_BUFFER_OVERFLOW;

			MutexLocker locker(device->lock);
			while (cookie->generation == device->generation) {
				if (cookie->closed)
					return B_FILE_ERROR;
				ConditionVariableEntry entry;
				device->condition.Add(&entry);
				locker.Unlock();
				status_t status = entry.Wait(B_CAN_INTERRUPT);
				locker.Lock();
				if (status == B_INTERRUPTED)
					return status;
			}
			tablet_movement movement = device->movement;
			cookie->generation = device->generation;
			locker.Unlock();

			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(buffer, &movement, sizeof(movement)) != B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case MS_NUM_EVENTS:
			return 0;

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


// #pragma mark - driver


static const char* kCompatible[] = {
	"goodix,gt911", "goodix,gt9110", "goodix,gt912", "goodix,gt9147",
	"goodix,gt917s", "goodix,gt927", "goodix,gt9271", "goodix,gt928",
	"goodix,gt967"
};


static bool
is_compatible(device_node* node, const char* const* list, size_t count)
{
	device_attr* attr = NULL;
	while (sDeviceManager->get_next_attr(node, &attr) == B_OK) {
		if (strcmp(attr->name, "fdt/compatible") != 0
			|| attr->type != B_STRING_TYPE) {
			continue;
		}
		for (size_t i = 0; i < count; i++) {
			if (strcmp(attr->value.string, list[i]) == 0)
				return true;
		}
	}
	return false;
}


/*!	Whether an ancestor of \a node in the device tree (the root) is an
	Allwinner A64.
*/
static bool
is_on_a64(device_node* node)
{
	static const char* kSoc[] = { "allwinner,sun50i-a64" };
	device_node* current = sDeviceManager->get_parent_node(node);
	for (int depth = 0; current != NULL && depth < 8; depth++) {
		const char* bus;
		bool found = sDeviceManager->get_attr_string(current, B_DEVICE_BUS,
				&bus, false) == B_OK && strcmp(bus, "fdt") == 0
			&& is_compatible(current, kSoc, B_COUNT_OF(kSoc));
		device_node* parent = found
			? NULL : sDeviceManager->get_parent_node(current);
		sDeviceManager->put_node(current);
		if (found)
			return true;
		current = parent;
	}
	if (current != NULL)
		sDeviceManager->put_node(current);
	return false;
}


static float
goodix_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0
		|| !is_compatible(parent, kCompatible, B_COUNT_OF(kCompatible))) {
		return 0.0f;
	}

	// only on the A64's TWI controllers, which this driver drives itself;
	// U-Boot's device tree (Tow-Boot) only names the A31 compatible, which
	// other SoCs share, so the SoC is checked as well
	static const char* kTwiCompatible[] = {
		"allwinner,sun50i-a64-i2c", "allwinner,sun6i-a31-i2c"
	};
	device_node* twi = sDeviceManager->get_parent_node(parent);
	bool supported = twi != NULL
		&& is_compatible(twi, kTwiCompatible, B_COUNT_OF(kTwiCompatible))
		&& is_on_a64(twi);
	if (twi != NULL)
		sDeviceManager->put_node(twi);
	if (!supported)
		INFO("touch controller not on an A64 TWI controller, ignored\n");
	return supported ? 0.8f : 0.0f;
}


static status_t
goodix_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Goodix touchscreen" } },
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED } },
		{}
	};
	return sDeviceManager->register_node(parent, DRIVER_MODULE_NAME, attrs,
		NULL, NULL);
}


static uint32
get_cell(fdt_device_module_info* fdt, fdt_device* device, const char* name,
	uint32 index, uint32 defaultValue)
{
	int length;
	const uint32* property = (const uint32*)fdt->get_prop(device, name,
		&length);
	if (property == NULL || length < (int)((index + 1) * 4))
		return defaultValue;
	return B_BENDIAN_TO_HOST_INT32(property[index]);
}


/*!	A "<&pio bank pin flags>" GPIO on port H, or -1. */
static int32
get_port_h_pin(fdt_device_module_info* fdt, fdt_device* device,
	const char* name, int32 defaultPin)
{
	int length;
	if (fdt->get_prop(device, name, &length) == NULL)
		return defaultPin;
	if (length < 16 || get_cell(fdt, device, name, 1, 0) != kPortH)
		return -1;
	return get_cell(fdt, device, name, 2, 0);
}


static status_t
goodix_init_driver(device_node* node, void** _cookie)
{
	goodix_device* device = new(std::nothrow) goodix_device;
	if (device == NULL)
		return B_NO_MEMORY;
	device->node = node;
	device->twiArea = device->ccuArea = -1;
	device->twi = device->ccu = NULL;
	device->twiIndex = 0;
	device->clockDivider = 0;
	device->address = 0x5d;
	device->interruptPin = device->resetPin = -1;
	device->sizeX = device->sizeY = 0;
	device->swapXY = device->invertX = device->invertY = false;
	device->openCount = 0;
	device->thread = -1;
	device->running = false;
	memset(&device->movement, 0, sizeof(device->movement));
	device->generation = 0;
	device->touching = false;
	device->clickSpeed = 500000;
	device->lastClickTime = 0;
	device->clickCount = 0;
	device->errors = 0;
	mutex_init(&device->lock, "goodix touch");
	device->condition.Init(device, "goodix touch");

	status_t status = B_OK;

	// the touch controller's node: address, lines, size
	device_node* touchNode = sDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* fdtDevice;
	if (sDeviceManager->get_driver(touchNode, (driver_module_info**)&fdt,
			(void**)&fdtDevice) == B_OK) {
		device->address = get_cell(fdt, fdtDevice, "reg", 0, 0x5d);
		device->interruptPin = get_port_h_pin(fdt, fdtDevice, "irq-gpios", 4);
		device->resetPin = get_port_h_pin(fdt, fdtDevice, "reset-gpios", 11);
		device->sizeX = get_cell(fdt, fdtDevice, "touchscreen-size-x", 0, 0);
		device->sizeY = get_cell(fdt, fdtDevice, "touchscreen-size-y", 0, 0);
		device->swapXY = fdt->get_prop(fdtDevice, "touchscreen-swapped-x-y",
			NULL) != NULL;
		device->invertX = fdt->get_prop(fdtDevice, "touchscreen-inverted-x",
			NULL) != NULL;
		device->invertY = fdt->get_prop(fdtDevice, "touchscreen-inverted-y",
			NULL) != NULL;
	} else
		status = B_ERROR;

	// its TWI controller
	device_node* twiNode = sDeviceManager->get_parent_node(touchNode);
	uint64 twiBase = 0;
	uint64 twiSize = 0;
	uint32 frequency = 100000;
	if (status == B_OK && sDeviceManager->get_driver(twiNode,
			(driver_module_info**)&fdt, (void**)&fdtDevice) == B_OK
		&& fdt->get_reg(fdtDevice, 0, &twiBase, &twiSize)) {
		frequency = get_cell(fdt, fdtDevice, "clock-frequency", 0, 100000);
	} else
		status = B_ERROR;
	sDeviceManager->put_node(twiNode);
	sDeviceManager->put_node(touchNode);

	if (status != B_OK || twiBase < kTwi0Base
		|| (twiBase - kTwi0Base) / 0x400 > 1
		|| (twiBase - kTwi0Base) % 0x400 != 0) {
		// TWI0 and TWI1 have their pins on port H
		ERROR("unexpected device tree: TWI at %#" B_PRIx64 "\n", twiBase);
		delete device;
		return B_BAD_DATA;
	}
	device->twiIndex = (twiBase - kTwi0Base) / 0x400;

	// SCL = 24 MHz / (10 * (M + 1) * 2^N): 400 kHz with M = 5, N = 0;
	// 100 kHz with M = 11, N = 1
	device->clockDivider = frequency >= 400000 ? (5 << 3) : (11 << 3 | 1);

	device->twiArea = map_physical_memory("goodix twi",
		twiBase & ~(phys_addr_t)(B_PAGE_SIZE - 1), B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&device->twi);
	device->ccuArea = map_physical_memory("goodix ccu/pio", kCcuBase,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&device->ccu);
	if (device->twiArea < 0 || device->ccuArea < 0) {
		status = device->twiArea < 0 ? device->twiArea : device->ccuArea;
		if (device->twiArea >= 0)
			delete_area(device->twiArea);
		if (device->ccuArea >= 0)
			delete_area(device->ccuArea);
		delete device;
		return status;
	}
	device->twi += twiBase & (B_PAGE_SIZE - 1);

	// bus clock on, out of reset; SCK and SDA on PH(2n), PH(2n + 1)
	uint32 bit = 1u << device->twiIndex;
	ccu_write(device, kCcuBusGate3, ccu_read(device, kCcuBusGate3) | bit);
	ccu_write(device, kCcuBusReset4, ccu_read(device, kCcuBusReset4) | bit);
	pio_configure(device, kPortH, 2 * device->twiIndex, 2);
	pio_configure(device, kPortH, 2 * device->twiIndex + 1, 2);
	spin(10);
	twi_reset(device);

	goodix_reset(device);
	status = goodix_probe(device);
	if (status != B_OK) {
		ERROR("no touch controller answers at %#x: %s\n", device->address,
			strerror(status));
		delete_area(device->twiArea);
		delete_area(device->ccuArea);
		delete device;
		return status;
	}

	*_cookie = device;
	return B_OK;
}


static void
goodix_uninit_driver(void* cookie)
{
	goodix_device* device = (goodix_device*)cookie;
	delete_area(device->twiArea);
	delete_area(device->ccuArea);
	mutex_destroy(&device->lock);
	delete device;
}


static status_t
goodix_register_child_devices(void* cookie)
{
	goodix_device* device = (goodix_device*)cookie;
	return sDeviceManager->publish_device(device->node, DEVICE_PATH,
		DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


static device_module_info sGoodixDevice = {
	{
		DEVICE_MODULE_NAME,
		0,
		NULL
	},
	goodix_init_device,
	NULL,	// uninit
	NULL,	// removed
	goodix_open,
	goodix_close,
	goodix_free,
	goodix_read_device,
	goodix_write_device,
	NULL,	// io
	goodix_control,
	NULL,	// select
	NULL	// deselect
};


static driver_module_info sGoodixDriver = {
	{
		DRIVER_MODULE_NAME,
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


module_info* modules[] = {
	(module_info*)&sGoodixDriver,
	(module_info*)&sGoodixDevice,
	NULL
};
