/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef _I801_SMBUS_H
#define _I801_SMBUS_H


#include <ACPI.h>
#include <bus/PCI.h>
#include <condition_variable.h>
#include <i2c.h>
#include <lock.h>

#include "i801_smbus_hardware.h"


//#define TRACE_I801_SMBUS
#ifdef TRACE_I801_SMBUS
#	define TRACE(x...) dprintf("i801_smbus: " x)
#else
#	define TRACE(x...) ;
#endif
#define TRACE_ALWAYS(x...)	dprintf("i801_smbus: " x)
#define ERROR(x...)			dprintf("i801_smbus: " x)
#define CALLED()			TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


#define I801_SMBUS_PCI_DEVICE_MODULE_NAME "busses/i2c/i801_smbus/pci/driver_v1"
#define I801_SMBUS_SIM_MODULE_NAME "busses/i2c/i801_smbus/device/v1"


// Slave address of a Synaptics touchpad in SMBus (InterTouch, RMI4) mode.
#define I801_SYNAPTICS_RMI4_ADDRESS	0x2c
// HID given to the device node this driver registers for it. The RMI4
// driver matches on it; there is no ACPI node for the SMBus side.
#define I801_SYNAPTICS_RMI4_HID		"SYNA_RMI4_SMBUS"


extern device_manager_info* gDeviceManager;
extern i2c_for_controller_interface* gI2c;
extern acpi_module_info* gACPI;


typedef struct {
	// PCI
	pci_device_module_info*	pci;
	pci_device*				device;
	pci_info				pciInfo;
	device_node*			driverNode;

	// I/O BAR (SBA), accessed through the PCI module
	addr_t					ioBase;
	uint32					irq;

	i2c_bus					sim;

	// configuration found at init, restored at uninit
	uint8					savedHcfg;
	uint8					savedScmd;
	uint8					savedAuxc;
	bool					interruptsUsable;
		// false when the firmware routes SMBus interrupts to SMI#

	// lock: held by a client across several commands (acquire_bus)
	// hwLock: one host transaction at a time
	mutex					lock;
	mutex					hwLock;
	ConditionVariable		transactionDone;
	int32					transactionPending;
	int32					ownsHardware;	// this driver holds HSTS.IUS
	uint8					transactionStatus;

	// Host Notify
	spinlock				notifyLock;
	i2c_host_notify_hook	notifyHook;
	void*					notifyCookie;
	uint32					notifyCount;
	uint32					notifyUnclaimed;
} i801_smbus;


#endif	// _I801_SMBUS_H
