/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef RMI4_H
#define RMI4_H


#include <device_manager.h>
#include <i2c.h>
#include <keyboard_mouse_driver.h>
#include <KernelExport.h>
#include <lock.h>
#include <OS.h>


#define TRACE_ALWAYS(x...)	dprintf("rmi4_smbus: " x)

#define RMI4_SMBUS_DRIVER_NAME	"drivers/input/rmi4_smbus/driver_v1"
#define RMI4_SMBUS_DEVICE_NAME	"drivers/input/rmi4_smbus/device_v1"
// must match I801_SYNAPTICS_RMI4_HID in the i801 SMBus driver
#define RMI4_SMBUS_HID			"SYNA_RMI4_SMBUS"

#define RMI4_TOUCHPAD_PATH		"input/touchpad/rmi4/synaptics_0"
#define RMI4_TRACKPOINT_PATH	"input/mouse/rmi4/trackpoint_0"


static const int kRmi4MapEntries = 8;
static const int kRmi4MaxFunctions = 16;
static const int kRmi4MaxIrqRegisters = 4;
static const int kRmi4MaxFingers = 10;
static const int kRmi4MaxDescriptorItems = 48;
static const int kRmi4QueueSize = 64;


struct rmi4_map_entry {
	uint16	address;
	uint8	count;
	uint8	flags;
	bool	valid;
};


struct rmi4_function {
	uint8	number;
	uint8	version;
	uint8	interruptCount;
	uint8	irqPosition;		// first bit in the interrupt status
	uint16	queryBase;
	uint16	commandBase;
	uint16	controlBase;
	uint16	dataBase;
};


// A packet register as described by a register descriptor (F12)
struct rmi4_register_item {
	uint16	reg;
	uint32	size;
	uint64	subpackets;			// bitmap of the first 64 subpackets
	uint16	subpacketCount;
};

struct rmi4_register_descriptor {
	int					count;
	rmi4_register_item	items[kRmi4MaxDescriptorItems];
};


struct rmi4_finger {
	bool	present;
	uint32	x;
	uint32	y;
	uint8	z;
	uint8	width;
};


template<typename Type>
struct rmi4_queue {
	Type		events[kRmi4QueueSize];
	int32		head;
	int32		count;
	sem_id		sem;
	mutex		lock;
};


struct rmi4_device {
	device_node*			node;
	i2c_device_interface*	i2c;
	i2c_device				i2cDevice;

	thread_id				thread;
	sem_id					notifySem;
	volatile bool			quit;
	int32					notifications;
	bool					hostNotify;
	bool					published;

	// from the PS/2 driver at the hand-over
	bool					clickpad;
	bool					trackstickButtons;

	// transport
	rmi4_map_entry			map[kRmi4MapEntries];
	int						nextMapEntry;

	// Page Description Table
	rmi4_function			functions[kRmi4MaxFunctions];
	int						functionCount;
	int						irqCount;
	int						irqRegisterCount;
	uint8					irqEnable[kRmi4MaxIrqRegisters];

	const rmi4_function*	f01;
	const rmi4_function*	sensor;		// F11 or F12
	const rmi4_function*	f03;
	const rmi4_function*	buttons;	// F30 or F3A

	// 2D sensor
	uint32					maxX;
	uint32					maxY;
	int						fingerCount;
	uint16					sensorReadOffset;	// F12: Data1 within the read
	uint16					sensorReadLength;
	rmi4_finger				fingers[kRmi4MaxFingers];
	rmi4_finger				lastFingers[kRmi4MaxFingers];
	uint32					fingerMask;
	uint32					lastFingerMask;
	int32					positionX;
	int32					positionY;
	int						loggedFrames;

	// F03 PS/2 guest (TrackPoint)
	uint8					f03QueueLength;
	uint8					guestPacket[3];
	int						guestIndex;
	uint8					guestButtons;
	bool					guestReady;

	// F30/F3A GPIO buttons
	uint8					gpioCount;
	uint8					gpioRegisterCount;
	int8					gpioPadButton[8];		// gpio -> button bit, or -1
	int8					gpioStickButton[8];
	uint8					padButtons;
	uint8					stickButtons;

	// TrackPoint clicks
	uint32					mouseButtons;
	bigtime_t				clickSpeed;
	bigtime_t				lastClickTime;
	int32					clickCount;

	touchpad_specs			specs;
	rmi4_queue<touchpad_movement>	touchpadQueue;
	rmi4_queue<mouse_movement>		mouseQueue;
};


extern device_manager_info* gDeviceManager;


status_t	rmi4_device_init(rmi4_device* device);
void		rmi4_device_uninit(rmi4_device* device);

status_t	rmi4_read_touchpad(rmi4_device* device, touchpad_movement* event,
				bigtime_t timeout);
status_t	rmi4_read_mouse(rmi4_device* device, mouse_movement* event);


#endif	// RMI4_H
