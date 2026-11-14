/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 *
 * Synaptics RMI4 touchpad and TrackPoint on SMBus (InterTouch).
 *
 * Sources:
 * - Synaptics RMI4 Interfacing Guide 511-000136-01 rev. E: Page Description
 *   Table (2.3.5), F01 device control and interrupt registers (3.1), F11
 *   2D sensor, F30 GPIO/LED, RMI on SMBus (12.2).
 * - Synaptics TouchPad Interfacing Guide: the PS/2 packets of the guest
 *   device behind the pass-through port (the TrackPoint).
 * - Register descriptors (F12), F03 PS/2 pass-through, F3A GPIO and the
 *   mapping table of RMI-on-SMBus protocol versions 2 and 3 are not in the
 *   guide; they follow the behaviour of the Linux rmi4 drivers, which were
 *   used as a description of the protocol only. No code was taken from
 *   them.
 *
 * The pad only answers on SMBus after the PS/2 driver has handed it over
 * (see <synaptics_intertouch.h>).
 */


#include "rmi4.h"

#include <driver_settings.h>
#include <synaptics_intertouch.h>
#include <util/AutoLock.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const uint8 kVersionCommand = 0xfd;
static const uint8 kMapCommandBase = 0x80;
static const uint8 kMapFlagWrite = 0x01;

static const uint16 kPdtStart = 0x00e9;
static const uint16 kPdtEnd = 0x0005;
static const int kPdtEntrySize = 6;
static const int kMaxPages = 8;

// F01
static const uint8 kF01ControlSleepMask = 0x03;
static const uint8 kF01ControlConfigured = 0x80;
static const uint8 kF01StatusUnconfigured = 0x80;
static const uint8 kF01StatusCodeMask = 0x0f;

// F03
static const uint8 kF03DataValid = 0x01;
static const uint8 kF03Timeout = 0x40;
static const uint8 kF03Parity = 0x80;

// How long the driver waits for the PS/2 driver to hand the pad over.
static const bigtime_t kHandOffWait = 120000000;
// Without Host Notify, or when a notification was lost, the interrupt
// status is read at this interval.
static const bigtime_t kIdlePoll = 200000;
static const bigtime_t kFastPoll = 12000;

static const uint8 kButtonLeft = 0x01;
static const uint8 kButtonRight = 0x02;
static const uint8 kButtonMiddle = 0x04;


//	#pragma mark - SMBus and RMI register access


static status_t
smbus_read_byte(rmi4_device* device, uint8 command, uint8* _value)
{
	return device->i2c->exec_command(device->i2cDevice, I2C_OP_READ_STOP,
		&command, 1, _value, 1);
}


static status_t
smbus_block_write(rmi4_device* device, uint8 command, const void* data,
	size_t length)
{
	return device->i2c->exec_command(device->i2cDevice, I2C_OP_WRITE_BLOCK,
		&command, 1, (void*)data, length);
}


static status_t
smbus_block_read(rmi4_device* device, uint8 command,
	uint8 buffer[I2C_SMBUS_BLOCK_MAX + 1])
{
	return device->i2c->exec_command(device->i2cDevice, I2C_OP_READ_BLOCK,
		&command, 1, buffer, I2C_SMBUS_BLOCK_MAX + 1);
}


/*!	Returns the SMBus command for an RMI register range. Protocol versions 2
	and 3 have no page select register: an 8 entry mapping table on the pad
	binds a command n to a 16 bit RMI address and a byte count (for reads)
	or to an address for writes. An entry is written with a block write of
	4 bytes (address low, address high, count, flags) to command 0x80 + n.
*/
static status_t
map_registers(rmi4_device* device, uint16 address, uint8 count, bool write,
	uint8* _command)
{
	for (int i = 0; i < kRmi4MapEntries; i++) {
		const rmi4_map_entry& entry = device->map[i];
		if (!entry.valid || entry.address != address)
			continue;
		if ((write && (entry.flags & kMapFlagWrite) != 0)
			|| (!write && (entry.flags & kMapFlagWrite) == 0
				&& entry.count == count)) {
			*_command = i;
			return B_OK;
		}
	}

	int index = device->nextMapEntry;
	device->nextMapEntry = (index + 1) % kRmi4MapEntries;

	const uint8 raw[4] = { (uint8)(address & 0xff), (uint8)(address >> 8),
		count, (uint8)(write ? kMapFlagWrite : 0) };
	rmi4_map_entry& entry = device->map[index];
	entry.valid = false;

	status_t status = smbus_block_write(device, kMapCommandBase + index, raw,
		sizeof(raw));
	if (status != B_OK)
		return status;

	entry.address = address;
	entry.count = count;
	entry.flags = raw[3];
	entry.valid = true;
	*_command = index;
	return B_OK;
}


static void
forget_mapping(rmi4_device* device)
{
	memset(device->map, 0, sizeof(device->map));
	device->nextMapEntry = 0;
}


static status_t
rmi_read(rmi4_device* device, uint16 address, void* _data, size_t length)
{
	uint8* data = (uint8*)_data;
	while (length > 0) {
		uint8 count = length > I2C_SMBUS_BLOCK_MAX
			? I2C_SMBUS_BLOCK_MAX : (uint8)length;
		uint8 command;
		status_t status = map_registers(device, address, count, false,
			&command);
		if (status != B_OK)
			return status;

		uint8 buffer[I2C_SMBUS_BLOCK_MAX + 1];
		status = smbus_block_read(device, command, buffer);
		if (status != B_OK)
			return status;
		if (buffer[0] < count)
			return B_BAD_DATA;
		memcpy(data, buffer + 1, count);

		address += count;
		data += count;
		length -= count;
	}
	return B_OK;
}


static status_t
rmi_write(rmi4_device* device, uint16 address, const void* _data,
	size_t length)
{
	const uint8* data = (const uint8*)_data;
	while (length > 0) {
		uint8 count = length > I2C_SMBUS_BLOCK_MAX
			? I2C_SMBUS_BLOCK_MAX : (uint8)length;
		uint8 command;
		status_t status = map_registers(device, address, count, true,
			&command);
		if (status != B_OK)
			return status;

		status = smbus_block_write(device, command, data, count);
		if (status != B_OK)
			return status;

		address += count;
		data += count;
		length -= count;
	}
	return B_OK;
}


static status_t
rmi_write_byte(rmi4_device* device, uint16 address, uint8 value)
{
	return rmi_write(device, address, &value, 1);
}


//	#pragma mark - event queues


template<typename Type>
static status_t
queue_init(rmi4_queue<Type>& queue, const char* name)
{
	queue.head = 0;
	queue.count = 0;
	mutex_init(&queue.lock, name);
	queue.sem = create_sem(0, name);
	return queue.sem >= 0 ? B_OK : queue.sem;
}


template<typename Type>
static void
queue_uninit(rmi4_queue<Type>& queue)
{
	if (queue.sem >= 0)
		delete_sem(queue.sem);
	queue.sem = -1;
	mutex_destroy(&queue.lock);
}


/*!	Adds an event. When the reader falls behind, the newest event replaces
	the last one in the queue: positions are absolute and a button change
	is kept in the replacing event.
*/
template<typename Type>
static void
queue_put(rmi4_queue<Type>& queue, const Type& event)
{
	MutexLocker locker(queue.lock);
	if (queue.count == kRmi4QueueSize) {
		int32 last = (queue.head + queue.count - 1) % kRmi4QueueSize;
		queue.events[last] = event;
		return;
	}
	queue.events[(queue.head + queue.count) % kRmi4QueueSize] = event;
	queue.count++;
	locker.Unlock();
	release_sem_etc(queue.sem, 1, B_DO_NOT_RESCHEDULE);
}


template<typename Type>
static status_t
queue_get(rmi4_queue<Type>& queue, Type* _event, uint32 flags,
	bigtime_t timeout)
{
	status_t status = acquire_sem_etc(queue.sem, 1, flags, timeout);
	if (status != B_OK)
		return status;

	MutexLocker locker(queue.lock);
	if (queue.count == 0)
		return B_INTERRUPTED;
	*_event = queue.events[queue.head];
	queue.head = (queue.head + 1) % kRmi4QueueSize;
	queue.count--;
	return B_OK;
}


//	#pragma mark - Page Description Table


static const rmi4_function*
find_function(rmi4_device* device, uint8 number)
{
	for (int i = 0; i < device->functionCount; i++) {
		if (device->functions[i].number == number)
			return &device->functions[i];
	}
	return NULL;
}


static status_t
scan_pdt(rmi4_device* device)
{
	device->functionCount = 0;
	device->irqCount = 0;

	int emptyPages = 0;
	for (int page = 0; page < kMaxPages && emptyPages < 2; page++) {
		uint16 pageStart = page << 8;
		bool empty = true;
		// int, not uint16: the loop counts down past zero on a full page
		for (int offset = kPdtStart; offset >= kPdtEnd;
				offset -= kPdtEntrySize) {
			uint8 raw[kPdtEntrySize];
			status_t status = rmi_read(device, pageStart + offset, raw,
				sizeof(raw));
			if (status != B_OK) {
				TRACE_ALWAYS("PDT read at 0x%04x failed: %s\n",
					pageStart + offset, strerror(status));
				return status;
			}
			// query, command, control and data base, then version
			// (bits 6:5) and interrupt count (bits 2:0), function number
			if (raw[5] == 0x00 || raw[5] == 0xff)
				break;
			empty = false;

			if (device->functionCount == kRmi4MaxFunctions)
				continue;
			rmi4_function& function
				= device->functions[device->functionCount++];
			function.number = raw[5];
			function.version = (raw[4] >> 5) & 0x3;
			function.interruptCount = raw[4] & 0x7;
			// interrupt bits are given out in the order of the table
			function.irqPosition = device->irqCount;
			device->irqCount += function.interruptCount;
			function.queryBase = pageStart + raw[0];
			function.commandBase = pageStart + raw[1];
			function.controlBase = pageStart + raw[2];
			function.dataBase = pageStart + raw[3];

			TRACE_ALWAYS("F%02x v%u: interrupt(s) %u+%u, query 0x%04x "
				"command 0x%04x control 0x%04x data 0x%04x\n", function.number,
				function.version, function.irqPosition,
				function.interruptCount, function.queryBase,
				function.commandBase, function.controlBase, function.dataBase);
		}
		emptyPages = empty ? emptyPages + 1 : 0;
	}

	device->irqRegisterCount = (device->irqCount + 7) / 8;
	if (device->irqRegisterCount > kRmi4MaxIrqRegisters) {
		TRACE_ALWAYS("%d interrupt sources, too many\n", device->irqCount);
		return B_BAD_DATA;
	}
	return B_OK;
}


static void
enable_function_irqs(rmi4_device* device, const rmi4_function* function,
	int first, int count)
{
	if (function == NULL)
		return;
	for (int i = first; i < first + count && i < function->interruptCount;
			i++) {
		int bit = function->irqPosition + i;
		device->irqEnable[bit / 8] |= 1 << (bit % 8);
	}
}


static bool
function_irq_pending(const rmi4_function* function, const uint8* status)
{
	if (function == NULL)
		return false;
	for (int i = 0; i < function->interruptCount; i++) {
		int bit = function->irqPosition + i;
		if ((status[bit / 8] & (1 << (bit % 8))) != 0)
			return true;
	}
	return false;
}


//	#pragma mark - register descriptors (F12)


static status_t
read_register_descriptor(rmi4_device* device, uint16 address,
	rmi4_register_descriptor* descriptor)
{
	// A register descriptor takes three registers: the size of the
	// presence register, the presence register (structure size and a
	// bitmap of the registers present), and the structure, which gives
	// the size and subpacket bitmap of each register present.
	descriptor->count = 0;

	uint8 presenceSize;
	status_t status = rmi_read(device, address, &presenceSize, 1);
	if (status != B_OK)
		return status;
	if (presenceSize < 1 || presenceSize > 35)
		return B_BAD_DATA;

	uint8 presence[35];
	status = rmi_read(device, address + 1, presence, presenceSize);
	if (status != B_OK)
		return status;

	uint32 structureSize;
	int bitmapStart;
	if (presence[0] == 0) {
		if (presenceSize < 3)
			return B_BAD_DATA;
		structureSize = presence[1] | (presence[2] << 8);
		bitmapStart = 3;
	} else {
		structureSize = presence[0];
		bitmapStart = 1;
	}
	if (structureSize == 0 || structureSize > 1024)
		return B_BAD_DATA;

	uint8* structure = (uint8*)malloc(structureSize);
	if (structure == NULL)
		return B_NO_MEMORY;
	status = rmi_read(device, address + 2, structure, structureSize);
	if (status != B_OK) {
		free(structure);
		return status;
	}

	uint32 offset = 0;
	for (int byte = bitmapStart; byte < presenceSize; byte++) {
		for (int bit = 0; bit < 8; bit++) {
			if ((presence[byte] & (1 << bit)) == 0)
				continue;
			if (offset >= structureSize
				|| descriptor->count == kRmi4MaxDescriptorItems) {
				goto done;
			}

			rmi4_register_item item;
			memset(&item, 0, sizeof(item));
			item.reg = (byte - bitmapStart) * 8 + bit;

			// size: one byte, or 0 and then 16 bit, or 0, 0 and 32 bit
			item.size = structure[offset++];
			if (item.size == 0 && offset + 2 <= structureSize) {
				item.size = structure[offset] | (structure[offset + 1] << 8);
				offset += 2;
				if (item.size == 0 && offset + 4 <= structureSize) {
					item.size = structure[offset]
						| (structure[offset + 1] << 8)
						| (structure[offset + 2] << 16)
						| ((uint32)structure[offset + 3] << 24);
					offset += 4;
				}
			}

			// subpacket bitmap: 7 bits per byte, bit 7 means another byte
			int subpacket = 0;
			while (offset < structureSize) {
				uint8 value = structure[offset++];
				for (int i = 0; i < 7; i++, subpacket++) {
					if ((value & (1 << i)) == 0)
						continue;
					item.subpacketCount++;
					if (subpacket < 64)
						item.subpackets |= (uint64)1 << subpacket;
				}
				if ((value & 0x80) == 0)
					break;
			}

			if (item.size != 0)
				descriptor->items[descriptor->count++] = item;
		}
	}

done:
	free(structure);
	return B_OK;
}


static const rmi4_register_item*
descriptor_item(const rmi4_register_descriptor& descriptor, uint16 reg,
	int* _index = NULL)
{
	for (int i = 0; i < descriptor.count; i++) {
		if (descriptor.items[i].reg == reg) {
			// each packet register takes one RMI address
			if (_index != NULL)
				*_index = i;
			return &descriptor.items[i];
		}
	}
	return NULL;
}


//	#pragma mark - F01 device control


static status_t
f01_configure(rmi4_device* device)
{
	const rmi4_function* f01 = device->f01;

	uint8 control;
	status_t status = rmi_read(device, f01->controlBase, &control, 1);
	if (status != B_OK)
		return status;

	// A sleep mode can be left over from before a warm reboot.
	control &= ~kF01ControlSleepMask;
	// Cleared by the pad when it resets: shows the driver a reset.
	control |= kF01ControlConfigured;
	status = rmi_write_byte(device, f01->controlBase, control);
	if (status != B_OK)
		return status;

	status = rmi_write(device, f01->controlBase + 1, device->irqEnable,
		device->irqRegisterCount);
	if (status != B_OK)
		return status;

	// reading the interrupt status clears it
	uint8 irqStatus[kRmi4MaxIrqRegisters];
	return rmi_read(device, f01->dataBase + 1, irqStatus,
		device->irqRegisterCount);
}


static void
f01_log_properties(rmi4_device* device)
{
	uint8 query[21];
	if (rmi_read(device, device->f01->queryBase, query, sizeof(query))
			!= B_OK) {
		return;
	}
	char product[11];
	for (int i = 0; i < 10; i++)
		product[i] = query[11 + i] & 0x7f;
	product[10] = '\0';
	TRACE_ALWAYS("F01: manufacturer %u, product \"%s\", firmware date "
		"%u-%02u-%02u, properties 0x%02x\n", query[0], product,
		2000 + (query[4] & 0x1f), query[5] & 0x0f, query[6] & 0x1f, query[1]);
}


//	#pragma mark - 2D sensor (F11, F12)


static status_t
f12_init(rmi4_device* device)
{
	const rmi4_function* f12 = device->sensor;

	uint8 general;
	status_t status = rmi_read(device, f12->queryBase, &general, 1);
	if (status != B_OK)
		return status;
	if ((general & 0x01) == 0) {
		TRACE_ALWAYS("F12 without register descriptors\n");
		return B_NOT_SUPPORTED;
	}

	rmi4_register_descriptor* query = new(std::nothrow)
		rmi4_register_descriptor;
	rmi4_register_descriptor* control = new(std::nothrow)
		rmi4_register_descriptor;
	rmi4_register_descriptor* data = new(std::nothrow)
		rmi4_register_descriptor;
	if (query == NULL || control == NULL || data == NULL) {
		delete query;
		delete control;
		delete data;
		return B_NO_MEMORY;
	}

	status = read_register_descriptor(device, f12->queryBase + 1, query);
	if (status == B_OK)
		status = read_register_descriptor(device, f12->queryBase + 4, control);
	if (status == B_OK)
		status = read_register_descriptor(device, f12->queryBase + 7, data);

	// Control 8, subpacket 0: maximum X and Y
	int index;
	const rmi4_register_item* item = status == B_OK
		? descriptor_item(*control, 8, &index) : NULL;
	if (status == B_OK && item != NULL && (item->subpackets & 1) != 0) {
		uint8 sensor[4];
		status = rmi_read(device, f12->controlBase + index, sensor,
			sizeof(sensor));
		if (status == B_OK) {
			device->maxX = sensor[0] | (sensor[1] << 8);
			device->maxY = sensor[2] | (sensor[3] << 8);
		}
	} else if (status == B_OK) {
		TRACE_ALWAYS("F12 has no sensor size (control 8)\n");
		status = B_NOT_SUPPORTED;
	}

	// Data 1: one 8 byte object per finger. The data registers are read as
	// one block from the data base, up to the end of Data 1.
	if (status == B_OK) {
		uint32 offset = 0;
		item = NULL;
		for (int i = 0; i < data->count; i++) {
			if (data->items[i].reg == 1) {
				item = &data->items[i];
				break;
			}
			offset += data->items[i].size;
		}
		if (item == NULL || item->subpacketCount == 0) {
			TRACE_ALWAYS("F12 reports no objects (data 1)\n");
			status = B_NOT_SUPPORTED;
		} else {
			// One SMBus block holds 32 bytes; that is enough for the first
			// fingers, which is all the pointer needs.
			int fingers = min_c(item->subpacketCount, kRmi4MaxFingers);
			if (offset + 8 > I2C_SMBUS_BLOCK_MAX) {
				TRACE_ALWAYS("F12 objects at offset %" B_PRIu32 "\n", offset);
				status = B_NOT_SUPPORTED;
			} else {
				fingers = min_c(fingers,
					(int)(I2C_SMBUS_BLOCK_MAX - offset) / 8);
				device->fingerCount = fingers;
				device->sensorReadOffset = offset;
				device->sensorReadLength = offset + fingers * 8;
			}
		}
	}

	if (status == B_OK) {
		TRACE_ALWAYS("F12: %" B_PRIu32 " x %" B_PRIu32 ", %d object(s), "
			"%d data register(s), objects at %u\n", device->maxX, device->maxY,
			device->fingerCount, data->count, device->sensorReadOffset);
	}

	delete query;
	delete control;
	delete data;
	return status;
}


static status_t
f11_init(rmi4_device* device)
{
	const rmi4_function* f11 = device->sensor;

	// query 0: general, query 1 to 4: first sensor
	uint8 query[5];
	status_t status = rmi_read(device, f11->queryBase, query, sizeof(query));
	if (status != B_OK)
		return status;

	if ((query[1] & 0x10) == 0) {
		TRACE_ALWAYS("F11 without absolute positions\n");
		return B_NOT_SUPPORTED;
	}
	int fingers = query[1] & 0x07;
	device->fingerCount = fingers == 5 ? 10 : fingers + 1;

	// control 6 to 9: maximum X and Y (12 bit)
	uint8 control[4];
	status = rmi_read(device, f11->controlBase + 6, control, sizeof(control));
	if (status != B_OK)
		return status;
	device->maxX = control[0] | (control[1] << 8);
	device->maxY = control[2] | (control[3] << 8);

	// finger states (2 bits each), then 5 bytes per finger
	int stateBytes = (device->fingerCount + 3) / 4;
	int length = stateBytes + device->fingerCount * 5;
	if (length > I2C_SMBUS_BLOCK_MAX) {
		device->fingerCount = (I2C_SMBUS_BLOCK_MAX - stateBytes) / 5;
		length = stateBytes + device->fingerCount * 5;
	}
	device->sensorReadOffset = stateBytes;
	device->sensorReadLength = length;

	TRACE_ALWAYS("F11: %" B_PRIu32 " x %" B_PRIu32 ", %d finger(s)\n",
		device->maxX, device->maxY, device->fingerCount);
	return B_OK;
}


static void
sensor_report(rmi4_device* device)
{
	// Follow the finger that moved most, so that a thumb resting on the
	// pad does not hold the pointer back; start at the average of the
	// fingers when the touch begins.
	uint32 common = device->fingerMask & device->lastFingerMask;
	uint32 sumX = 0, sumY = 0, sumZ = 0, sumWidth = 0;
	int32 deltaX = 0, deltaY = 0, moved = -1;
	int count = 0;
	for (int i = 0; i < device->fingerCount; i++) {
		if ((device->fingerMask & (1 << i)) == 0)
			continue;
		const rmi4_finger& finger = device->fingers[i];
		count++;
		sumX += finger.x;
		sumY += finger.y;
		sumZ += finger.z;
		sumWidth += finger.width;

		if ((common & (1 << i)) == 0)
			continue;
		int32 dx = (int32)finger.x - (int32)device->lastFingers[i].x;
		int32 dy = (int32)finger.y - (int32)device->lastFingers[i].y;
		if (abs(dx) + abs(dy) > moved) {
			moved = abs(dx) + abs(dy);
			deltaX = dx;
			deltaY = dy;
		}
	}

	if (count > 0 && device->lastFingerMask == 0) {
		device->positionX = sumX / count;
		device->positionY = sumY / count;
	} else {
		device->positionX = min_c(max_c(device->positionX + deltaX, 0),
			(int32)device->maxX);
		device->positionY = min_c(max_c(device->positionY + deltaY, 0),
			(int32)device->maxY);
	}

	memcpy(device->lastFingers, device->fingers, sizeof(device->fingers));
	device->lastFingerMask = device->fingerMask;

	touchpad_movement event;
	memset(&event, 0, sizeof(event));
	event.buttons = device->padButtons;
	event.fingers = device->fingerMask;
	if (count == 0) {
		// x and y 0 is no contact; a width of 0 or 1 would mean 2 or 3
		// fingers
		event.fingerWidth = 4;
	} else {
		event.xPosition = max_c(device->positionX, 1);
		event.yPosition = device->positionY;
		event.zPressure = min_c(max_c(sumZ / count, 1), 254);
		// the Synaptics width scale of movement_maker: 4 to 7 is a finger,
		// 0 and 1 mean two and three or more fingers
		if (count == 1)
			event.fingerWidth = 4 + min_c(sumWidth / 4, 3);
		else
			event.fingerWidth = count == 2 ? 0 : 1;
	}

	queue_put(device->touchpadQueue, event);
}


static status_t
sensor_read(rmi4_device* device)
{
	uint8 data[I2C_SMBUS_BLOCK_MAX];
	status_t status = rmi_read(device, device->sensor->dataBase, data,
		device->sensorReadLength);
	if (status != B_OK)
		return status;

	if (device->loggedFrames < 6) {
		device->loggedFrames++;
		char line[3 * I2C_SMBUS_BLOCK_MAX + 1];
		size_t used = 0;
		for (int i = 0; i < device->sensorReadLength; i++)
			used += snprintf(line + used, sizeof(line) - used, " %02x", data[i]);
		TRACE_ALWAYS("sensor data:%s\n", line);
	}

	device->fingerMask = 0;
	for (int i = 0; i < device->fingerCount; i++) {
		rmi4_finger& finger = device->fingers[i];
		finger.present = false;

		if (device->sensor->number == 0x12) {
			const uint8* object = data + device->sensorReadOffset + i * 8;
			// object type: 1 finger, 4 unclassified, 6 gloved finger;
			// a palm (3) and everything else is left out
			uint8 type = object[0];
			if (type != 0x01 && type != 0x04 && type != 0x06)
				continue;
			finger.x = object[1] | (object[2] << 8);
			finger.y = object[3] | (object[4] << 8);
			finger.z = object[5];
			finger.width = max_c(object[6], object[7]);
		} else {
			// F11: finger state 1 is present (2 is inaccurate)
			uint8 state = (data[i / 4] >> (2 * (i % 4))) & 0x3;
			if (state != 1)
				continue;
			const uint8* position = data + device->sensorReadOffset + i * 5;
			finger.x = (position[0] << 4) | (position[2] & 0x0f);
			finger.y = (position[1] << 4) | (position[2] >> 4);
			finger.z = position[4];
			finger.width = max_c(position[3] & 0x0f, position[3] >> 4);
		}

		// The raw Y axis grows upwards, as movement_maker expects.
		if (finger.x > device->maxX || finger.y > device->maxY)
			continue;
		finger.present = true;
		device->fingerMask |= 1 << i;
	}

	sensor_report(device);
	return B_OK;
}


//	#pragma mark - TrackPoint (F03 PS/2 pass-through) and buttons


static void
report_mouse(rmi4_device* device, int32 deltaX, int32 deltaY)
{
	uint32 buttons = device->guestButtons | device->stickButtons;
	bigtime_t now = system_time();

	if (buttons != 0 && device->mouseButtons == 0) {
		if (device->lastClickTime + device->clickSpeed > now)
			device->clickCount++;
		else
			device->clickCount = 1;
		device->lastClickTime = now;
	}
	if (buttons == device->mouseButtons && deltaX == 0 && deltaY == 0)
		return;
	device->mouseButtons = buttons;

	mouse_movement event;
	memset(&event, 0, sizeof(event));
	event.buttons = buttons;
	event.xdelta = deltaX;
	event.ydelta = deltaY;
	event.clicks = device->clickCount;
	event.timestamp = now;
	queue_put(device->mouseQueue, event);
}


static void
guest_byte(rmi4_device* device, uint8 value)
{
	if (!device->guestReady)
		return;

	// standard 3 byte PS/2 mouse packet; bit 3 of the first byte is set
	if (device->guestIndex == 0 && (value & 0x08) == 0)
		return;
	device->guestPacket[device->guestIndex++] = value;
	if (device->guestIndex < 3)
		return;
	device->guestIndex = 0;

	const uint8* packet = device->guestPacket;
	if ((packet[0] & 0xc0) != 0) {
		// overflow
		return;
	}
	int32 deltaX = packet[1] - ((packet[0] & 0x10) != 0 ? 256 : 0);
	int32 deltaY = packet[2] - ((packet[0] & 0x20) != 0 ? 256 : 0);
	device->guestButtons = packet[0] & 0x07;
	report_mouse(device, deltaX, deltaY);
}


/*!	Reads the output buffers of the PS/2 guest. Returns the number of
	bytes, and passes them on to the TrackPoint or into \a reply.
*/
static int
f03_read(rmi4_device* device, uint8* reply, int replySize)
{
	const rmi4_function* f03 = device->f03;
	uint8 buffers[2 * 15];
	int length = device->f03QueueLength * 2;
	if (rmi_read(device, f03->dataBase + 2, buffers, length) != B_OK)
		return 0;

	int count = 0;
	for (int i = 0; i < length; i += 2) {
		uint8 status = buffers[i];
		if ((status & kF03DataValid) == 0)
			continue;
		if ((status & (kF03Timeout | kF03Parity)) != 0)
			continue;
		if (reply != NULL) {
			if (count < replySize)
				reply[count] = buffers[i + 1];
		} else
			guest_byte(device, buffers[i + 1]);
		count++;
	}
	return count;
}


static status_t
guest_command(rmi4_device* device, uint8 command, uint8* reply,
	int replyCount, bigtime_t timeout)
{
	status_t status = rmi_write_byte(device, device->f03->dataBase, command);
	if (status != B_OK)
		return status;

	uint8 bytes[8];
	int received = 0;
	bigtime_t deadline = system_time() + timeout;
	while (received < replyCount + 1 && system_time() < deadline) {
		snooze(5000);
		uint8 chunk[8];
		int count = f03_read(device, chunk, sizeof(chunk));
		for (int i = 0; i < count && received < (int)sizeof(bytes); i++)
			bytes[received++] = chunk[i];
	}

	if (received == 0)
		return B_TIMED_OUT;
	if (bytes[0] != 0xfa) {
		TRACE_ALWAYS("TrackPoint: command 0x%02x answered 0x%02x\n", command,
			bytes[0]);
		return B_ERROR;
	}
	for (int i = 0; i < replyCount; i++)
		reply[i] = i + 1 < received ? bytes[i + 1] : 0;
	return received >= replyCount + 1 ? B_OK : B_TIMED_OUT;
}


static status_t
f03_init(rmi4_device* device)
{
	const rmi4_function* f03 = device->f03;

	uint8 query;
	status_t status = rmi_read(device, f03->queryBase, &query, 1);
	if (status != B_OK)
		return status;
	int guests = query & 0x07;
	int bytesPerGuest = (query >> 4) & 0x07;
	if (guests * bytesPerGuest > 0) {
		uint8 query2;
		status = rmi_read(device, f03->queryBase + 1, &query2, 1);
		if (status != B_OK)
			return status;
		device->f03QueueLength = query2 & 0x0f;
	} else
		device->f03QueueLength = 7;
	if (device->f03QueueLength == 0)
		device->f03QueueLength = 7;

	TRACE_ALWAYS("F03: %d guest(s), output queue of %u\n", guests,
		device->f03QueueLength);
	return B_OK;
}


/*!	Resets and enables the TrackPoint behind the pass-through port. The
	guest replies arrive in the F03 output buffers; they are polled here,
	before the F03 interrupt is processed by the event loop.
*/
static void
trackpoint_start(rmi4_device* device)
{
	device->guestReady = false;
	device->guestIndex = 0;

	// drop announcements (AA 00) that are still queued
	f03_read(device, NULL, 0);

	uint8 reply[2];
	status_t status = guest_command(device, 0xff, reply, 2, 1500000);
	TRACE_ALWAYS("TrackPoint reset: %s (%02x %02x)\n", strerror(status),
		reply[0], reply[1]);

	status = guest_command(device, 0xf4, NULL, 0, 300000);
	TRACE_ALWAYS("TrackPoint enable: %s\n", strerror(status));

	device->guestReady = true;
}


static bool
gpio_valid(int gpio, const uint8* exists, const uint8* direction)
{
	// a button is a GPIO that exists and is an input (direction bit 0)
	return (exists[gpio / 8] & (1 << (gpio % 8))) != 0
		&& (direction[gpio / 8] & (1 << (gpio % 8))) == 0;
}


static status_t
buttons_init(rmi4_device* device)
{
	const rmi4_function* function = device->buttons;
	uint8 exists[2] = { 0, 0 };
	uint8 direction[2] = { 0, 0 };
	status_t status;

	if (function->number == 0x3a) {
		uint8 query;
		status = rmi_read(device, function->queryBase, &query, 1);
		if (status != B_OK)
			return status;
		device->gpioCount = min_c(query & 0x7f, 16);
		device->gpioRegisterCount = (device->gpioCount + 7) / 8;
		if (device->gpioRegisterCount == 0)
			return B_NOT_SUPPORTED;
		// query 1: GPIO present, control 1: direction
		status = rmi_read(device, function->queryBase + 1, exists,
			device->gpioRegisterCount);
		if (status == B_OK) {
			status = rmi_read(device, function->controlBase + 1, direction,
				device->gpioRegisterCount);
		}
	} else {
		uint8 query[2];
		status = rmi_read(device, function->queryBase, query, 2);
		if (status != B_OK)
			return status;
		bool hasLed = (query[0] & 0x04) != 0;
		bool hasGpio = (query[0] & 0x08) != 0;
		device->gpioCount = min_c(query[1] & 0x1f, 16);
		device->gpioRegisterCount = (device->gpioCount + 7) / 8;
		if (!hasGpio || device->gpioRegisterCount == 0)
			return B_NOT_SUPPORTED;
		// control 0 (LED and GPIO only), 1, then 2: direction and 3: the
		// GPIOs used as buttons
		uint16 address = function->controlBase + 1
			+ (hasLed ? device->gpioRegisterCount : 0);
		status = rmi_read(device, address, direction,
			device->gpioRegisterCount);
		if (status == B_OK) {
			status = rmi_read(device, address + device->gpioRegisterCount,
				exists, device->gpioRegisterCount);
		}
	}
	if (status != B_OK)
		return status;

	// GPIO 0 to 2 are the pad buttons, 3 to 5 the TrackPoint buttons when
	// the PS/2 side said so. A clickpad has one button: the first one.
	static const uint8 kButtons[3] = { kButtonLeft, kButtonRight,
		kButtonMiddle };
	int padButton = 0;
	int stickButton = 0;
	for (int i = 0; i < 8; i++) {
		device->gpioPadButton[i] = -1;
		device->gpioStickButton[i] = -1;
	}
	for (int i = 0; i < min_c(device->gpioCount, 6); i++) {
		if (!gpio_valid(i, exists, direction))
			continue;
		if (device->trackstickButtons && i >= 3) {
			if (stickButton < 3)
				device->gpioStickButton[i] = kButtons[stickButton++];
		} else if (padButton < 3 && (!device->clickpad || padButton == 0))
			device->gpioPadButton[i] = kButtons[padButton++];
	}

	TRACE_ALWAYS("F%02x: %u GPIO(s), %d pad button(s), %d TrackPoint "
		"button(s)\n", function->number, device->gpioCount, padButton,
		stickButton);
	return B_OK;
}


static status_t
buttons_read(rmi4_device* device)
{
	uint8 data[2] = { 0xff, 0xff };
	status_t status = rmi_read(device, device->buttons->dataBase, data,
		device->gpioRegisterCount);
	if (status != B_OK)
		return status;

	uint8 pad = 0;
	uint8 stick = 0;
	for (int i = 0; i < 8; i++) {
		// the GPIOs are low while a button is down
		if ((data[i / 8] & (1 << (i % 8))) != 0)
			continue;
		if (device->gpioPadButton[i] > 0)
			pad |= device->gpioPadButton[i];
		if (device->gpioStickButton[i] > 0)
			stick |= device->gpioStickButton[i];
	}

	if (pad != device->padButtons) {
		device->padButtons = pad;
		sensor_report(device);
	}
	if (stick != device->stickButtons) {
		device->stickButtons = stick;
		report_mouse(device, 0, 0);
	}
	return B_OK;
}


//	#pragma mark - start and event loop


static status_t
configure(rmi4_device* device)
{
	memset(device->irqEnable, 0, sizeof(device->irqEnable));
	enable_function_irqs(device, device->f01, 0, 8);
	// F11 and F12: the first interrupt is absolute data, the second
	// relative data, which is not used
	enable_function_irqs(device, device->sensor, 0, 1);
	enable_function_irqs(device, device->f03, 0, 8);
	enable_function_irqs(device, device->buttons, 0, 8);

	return f01_configure(device);
}


static status_t
start_pad(rmi4_device* device)
{
	// Reading the protocol version switches the pad to SMBus. The PS/2
	// driver reset it before the hand-over; give the firmware time to
	// come up again first, as Linux does.
	snooze(30000);
	forget_mapping(device);

	uint8 version;
	status_t status = smbus_read_byte(device, kVersionCommand, &version);
	if (status != B_OK) {
		TRACE_ALWAYS("reading the SMBus protocol version failed: %s\n",
			strerror(status));
		return status;
	}
	// the register holds the version minus one
	TRACE_ALWAYS("RMI-on-SMBus protocol version %u\n", version + 1);
	if (version + 1 != 2 && version + 1 != 3)
		return B_NOT_SUPPORTED;

	status = scan_pdt(device);
	if (status != B_OK)
		return status;

	device->f01 = find_function(device, 0x01);
	device->sensor = find_function(device, 0x12);
	if (device->sensor == NULL)
		device->sensor = find_function(device, 0x11);
	device->f03 = find_function(device, 0x03);
	device->buttons = find_function(device, 0x3a);
	if (device->buttons == NULL)
		device->buttons = find_function(device, 0x30);

	if (device->f01 == NULL || device->sensor == NULL) {
		TRACE_ALWAYS("no %s function\n", device->f01 == NULL ? "F01" : "2D");
		return B_NOT_SUPPORTED;
	}
	f01_log_properties(device);

	status = device->sensor->number == 0x12
		? f12_init(device) : f11_init(device);
	if (status != B_OK)
		return status;
	if (device->maxX == 0 || device->maxY == 0)
		return B_BAD_DATA;

	if (device->f03 != NULL && f03_init(device) != B_OK)
		device->f03 = NULL;
	// without the TrackPoint, its buttons are ordinary pad buttons
	if (device->f03 == NULL)
		device->trackstickButtons = false;
	if (device->buttons != NULL && buttons_init(device) != B_OK)
		device->buttons = NULL;

	status = configure(device);
	if (status != B_OK) {
		TRACE_ALWAYS("configuring the pad failed: %s\n", strerror(status));
		return status;
	}

	device->specs.areaStartX = 0;
	device->specs.areaEndX = device->maxX;
	device->specs.areaStartY = 0;
	device->specs.areaEndY = device->maxY;
	// The Synaptics PS/2 driver uses 50 for a pad about 4000 units wide.
	device->specs.edgeMotionWidth = max_c(device->maxX * 50 / 4000, 1);
	device->specs.minPressure = 0;
	device->specs.realMaxPressure = 100;
	device->specs.maxPressure = 255;
	return B_OK;
}


static void
host_notify(void* cookie, i2c_addr address, uint16 data)
{
	// interrupt context
	rmi4_device* device = (rmi4_device*)cookie;
	atomic_add(&device->notifications, 1);
	release_sem_etc(device->notifySem, 1, B_DO_NOT_RESCHEDULE);
}


static bool
process_interrupts(rmi4_device* device)
{
	uint8 status[kRmi4MaxIrqRegisters];
	if (rmi_read(device, device->f01->dataBase + 1, status,
			device->irqRegisterCount) != B_OK) {
		return false;
	}
	bool any = false;
	for (int i = 0; i < device->irqRegisterCount; i++) {
		status[i] &= device->irqEnable[i];
		any |= status[i] != 0;
	}
	if (!any)
		return false;

	if (function_irq_pending(device->f01, status)) {
		uint8 deviceStatus;
		if (rmi_read(device, device->f01->dataBase, &deviceStatus, 1) == B_OK
			&& (deviceStatus & kF01StatusUnconfigured) != 0) {
			TRACE_ALWAYS("the pad was reset (status 0x%02x), configuring it "
				"again\n", deviceStatus & kF01StatusCodeMask);
			forget_mapping(device);
			if (configure(device) == B_OK && device->f03 != NULL)
				trackpoint_start(device);
			return true;
		}
	}
	if (function_irq_pending(device->buttons, status))
		buttons_read(device);
	if (function_irq_pending(device->sensor, status))
		sensor_read(device);
	if (function_irq_pending(device->f03, status))
		f03_read(device, NULL, 0);
	return true;
}


static void
event_loop(rmi4_device* device)
{
	bigtime_t interval = kIdlePoll;
	bigtime_t logTime = system_time() + 10000000;
	int32 polledEvents = 0;

	while (!device->quit) {
		status_t status = acquire_sem_etc(device->notifySem, 1,
			B_RELATIVE_TIMEOUT, interval);
		if (device->quit)
			break;

		bool events = process_interrupts(device);
		if (status != B_OK && events && polledEvents++ == 0) {
			TRACE_ALWAYS("pad events without Host Notify\n");
		}

		// Without Host Notify, poll quickly while the pad is in use.
		if (!device->hostNotify || atomic_get(&device->notifications) == 0)
			interval = events ? kFastPoll : min_c(interval * 2, kIdlePoll);

		if (logTime != 0 && system_time() > logTime) {
			logTime = 0;
			TRACE_ALWAYS("%" B_PRId32 " Host Notify message(s), %" B_PRId32
				" event(s) found by polling in the first seconds\n",
				atomic_get(&device->notifications), polledEvents);
		}
	}
}


static status_t
reply_to_ps2(int32 code)
{
	port_id port = find_port(SYNAPTICS_INTERTOUCH_REPLY_PORT);
	if (port < 0)
		return port;
	return write_port_etc(port, code, NULL, 0, B_RELATIVE_TIMEOUT, 1000000);
}


static bool
wait_for_hand_off(rmi4_device* device)
{
	bigtime_t end = system_time() + kHandOffWait;
	while (!device->quit && system_time() < end) {
		port_id port = find_port(SYNAPTICS_INTERTOUCH_HAND_OFF_PORT);
		if (port >= 0) {
			synaptics_intertouch_info info;
			int32 code;
			ssize_t size = read_port_etc(port, &code, &info, sizeof(info),
				B_RELATIVE_TIMEOUT, 0);
			if (size == (ssize_t)sizeof(info)
				&& code == kSynapticsInterTouchHandOff) {
				device->clickpad
					= (info.flags & SYNAPTICS_INTERTOUCH_CLICKPAD) != 0;
				device->trackstickButtons = (info.flags
					& SYNAPTICS_INTERTOUCH_TRACKSTICK_BUTTONS) != 0;
				TRACE_ALWAYS("the PS/2 driver handed the pad over (%s%s)\n",
					device->clickpad ? "clickpad" : "buttons",
					device->trackstickButtons
						? ", TrackPoint buttons" : "");
				return true;
			}
		}
		snooze(200000);
	}
	if (!device->quit)
		TRACE_ALWAYS("the PS/2 driver did not hand over a pad\n");
	return false;
}


static status_t
device_thread(void* data)
{
	rmi4_device* device = (rmi4_device*)data;

	if (!wait_for_hand_off(device))
		return B_OK;

	status_t status = start_pad(device);
	if (status == B_OK) {
		status = device->i2c->install_host_notify(device->i2cDevice,
			&host_notify, device);
		device->hostNotify = status == B_OK;
		if (status != B_OK) {
			TRACE_ALWAYS("no Host Notify (%s), polling\n", strerror(status));
			status = B_OK;
		}
	}

	if (status != B_OK) {
		TRACE_ALWAYS("cannot drive the pad (%s), giving it back to PS/2\n",
			strerror(status));
		reply_to_ps2(kSynapticsInterTouchFailed);
		return status;
	}
	if (reply_to_ps2(kSynapticsInterTouchTaken) != B_OK) {
		// too late: the PS/2 driver took the pad back
		TRACE_ALWAYS("the PS/2 driver no longer waits, leaving the pad\n");
		if (device->hostNotify)
			device->i2c->uninstall_host_notify(device->i2cDevice);
		device->hostNotify = false;
		return B_TIMED_OUT;
	}

	if (device->f03 != NULL)
		trackpoint_start(device);

	status = gDeviceManager->publish_device(device->node, RMI4_TOUCHPAD_PATH,
		RMI4_SMBUS_DEVICE_NAME);
	if (status == B_OK && device->f03 != NULL) {
		status = gDeviceManager->publish_device(device->node,
			RMI4_TRACKPOINT_PATH, RMI4_SMBUS_DEVICE_NAME);
	}
	device->published = true;
	TRACE_ALWAYS("touchpad%s published: %s\n",
		device->f03 != NULL ? " and TrackPoint" : "", strerror(status));

	event_loop(device);

	if (device->hostNotify)
		device->i2c->uninstall_host_notify(device->i2cDevice);
	return B_OK;
}


//	#pragma mark - interface


status_t
rmi4_device_init(rmi4_device* device)
{
	device->thread = -1;
	device->clickSpeed = 500000;
	device->notifySem = create_sem(0, "rmi4 host notify");
	status_t touchpadStatus = queue_init(device->touchpadQueue,
		"rmi4 touchpad");
	status_t mouseStatus = queue_init(device->mouseQueue, "rmi4 trackpoint");
	if (device->notifySem < 0 || touchpadStatus != B_OK
		|| mouseStatus != B_OK) {
		rmi4_device_uninit(device);
		return B_NO_MEMORY;
	}

	bool wanted = false;
	void* settings = load_driver_settings(SYNAPTICS_INTERTOUCH_SETTINGS);
	if (settings != NULL) {
		wanted = get_driver_boolean_parameter(settings,
			SYNAPTICS_INTERTOUCH_PARAMETER, false, true);
		unload_driver_settings(settings);
	}
	if (!wanted) {
		// Any SMBus access while the PS/2 driver drives the pad disturbs
		// it: stay off the bus.
		TRACE_ALWAYS("InterTouch is off (\"" SYNAPTICS_INTERTOUCH_PARAMETER
			" true\" in the " SYNAPTICS_INTERTOUCH_SETTINGS " driver settings "
			"turns it on)\n");
		return B_OK;
	}

	device->thread = spawn_kernel_thread(&device_thread, "rmi4 smbus",
		B_REAL_TIME_DISPLAY_PRIORITY, device);
	if (device->thread < 0) {
		status_t status = device->thread;
		rmi4_device_uninit(device);
		return status;
	}
	resume_thread(device->thread);
	return B_OK;
}


void
rmi4_device_uninit(rmi4_device* device)
{
	device->quit = true;
	if (device->notifySem >= 0)
		release_sem(device->notifySem);
	if (device->thread >= 0) {
		status_t result;
		wait_for_thread(device->thread, &result);
		device->thread = -1;
	}

	if (device->published) {
		gDeviceManager->unpublish_device(device->node, RMI4_TOUCHPAD_PATH);
		if (device->f03 != NULL) {
			gDeviceManager->unpublish_device(device->node,
				RMI4_TRACKPOINT_PATH);
		}
		device->published = false;
	}

	if (device->notifySem >= 0)
		delete_sem(device->notifySem);
	device->notifySem = -1;
	queue_uninit(device->touchpadQueue);
	queue_uninit(device->mouseQueue);
}


status_t
rmi4_read_touchpad(rmi4_device* device, touchpad_movement* event,
	bigtime_t timeout)
{
	uint32 flags = B_CAN_INTERRUPT;
	if (timeout >= 0 && timeout != B_INFINITE_TIMEOUT)
		flags |= B_RELATIVE_TIMEOUT;
	return queue_get(device->touchpadQueue, event, flags, timeout);
}


status_t
rmi4_read_mouse(rmi4_device* device, mouse_movement* event)
{
	return queue_get(device->mouseQueue, event, B_CAN_INTERRUPT, 0);
}
