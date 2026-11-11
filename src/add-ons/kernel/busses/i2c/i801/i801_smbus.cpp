/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 *
 * Intel PCH SMBus host controller (the "i801" family, PCI D31:F4).
 *
 * This driver gives the i2c bus manager access to the PCH SMBus host
 * controller, including SMBus Host Notify. Host Notify is how a Synaptics
 * touchpad in SMBus (InterTouch/RMI4) mode tells the host it has data; it
 * has no interrupt line of its own.
 *
 * Written from the Intel datasheets (334659-005 volume 2, chapter 7;
 * 332995-004 volume 1, chapter 30) and the SMBus 3.3.1 specification.
 *
 * Design points that follow from the datasheet:
 * - While SSTS.HOST_NOTIFY is set the PCH NACKs new Host Notify messages,
 *   so the interrupt handler copies NDA/NDLB/NDHB and clears it first.
 * - SSTS and SCMD live in a 64 kHz clock domain: a write must be read
 *   back before it is known to have taken effect.
 * - HCFG.SMB_SMI_EN routes all SMBus interrupts to SMI#. When the firmware
 *   set it, this driver does not touch it, polls host transactions and
 *   reports Host Notify as not supported.
 * - HSTS.IUS is a semaphore shared with firmware (SMM/ACPI); it is taken
 *   around every host transaction.
 * - The interrupt is legacy INTx (no MSI capability) and shared.
 */


#include <new>
#include <stdio.h>
#include <string.h>

#include <KernelExport.h>
#include <util/AutoLock.h>

#include "i801_smbus.h"


device_manager_info* gDeviceManager;
i2c_for_controller_interface* gI2c;
acpi_module_info* gACPI;


static const uint16 kSupportedDevices[] = {
	0x9d23,		// Sunrise Point-LP (6th/7th gen Core U/Y), Vol 1 table 2-1
	0
};


// ACPI PnP IDs of PS/2 touchpads known to have a working SMBus
// (InterTouch) side at address 0x2c. Windows binds the Synaptics SMBus
// driver to the PCH SMBus on these systems.
static const char* kInterTouchPnpIds[] = {
	"LEN2050",	// ThinkPad X270
	NULL
};


// Longest host transaction: a 32 byte block at 10 kHz is about 40 ms; the
// SMBus clock low timeout is 25 to 35 ms on top.
static const bigtime_t kTransactionTimeout = 100000;
// how long to wait for firmware to release the IUS semaphore
static const bigtime_t kSemaphoreTimeout = 50000;


//	#pragma mark - register access


static inline uint8
smb_read(i801_smbus* bus, uint8 offset)
{
	return bus->pci->read_io_8(bus->device, bus->ioBase + offset);
}


static inline void
smb_write(i801_smbus* bus, uint8 offset, uint8 value)
{
	bus->pci->write_io_8(bus->device, bus->ioBase + offset, value);
}


// SSTS and SCMD are in the 64 kHz domain; read back until the register
// shows the expected bits (one 64 kHz clock is 15.6 us).
static status_t
smb_write_slow(i801_smbus* bus, uint8 offset, uint8 value, uint8 mask,
	uint8 expected)
{
	smb_write(bus, offset, value);
	for (int i = 0; i < 100; i++) {
		if ((smb_read(bus, offset) & mask) == (expected & mask))
			return B_OK;
		spin(20);
	}
	return B_TIMED_OUT;
}


static inline status_t
smb_write_scmd(i801_smbus* bus, uint8 value)
{
	const uint8 mask = SMB_SCMD_HNI | SMB_SCMD_HNW | SMB_SCMD_SMBALERT_DIS;
	return smb_write_slow(bus, SMB_SCMD, value, mask, value);
}


static inline status_t
smb_clear_host_notify(i801_smbus* bus)
{
	// RW1C: the bit must read back as 0
	return smb_write_slow(bus, SMB_SSTS, SMB_SSTS_HOST_NOTIFY,
		SMB_SSTS_HOST_NOTIFY, 0);
}


//	#pragma mark - interrupt


static int32
i801_interrupt_handler(void* data)
{
	i801_smbus* bus = (i801_smbus*)data;
	int32 result = B_UNHANDLED_INTERRUPT;

	// The line is shared. When the interrupt is not ours, HSTS may hold the
	// result of a polled transaction by the firmware (it owns IUS then),
	// which must not be cleared under it.
	if ((bus->pci->read_pci_config(bus->device, PCI_status, 2)
			& SMB_PCI_STATUS_INTS) == 0)
		return B_UNHANDLED_INTERRUPT;

	// Host Notify first: the PCH NACKs further notifications until the
	// status bit is cleared.
	if ((smb_read(bus, SMB_SSTS) & SMB_SSTS_HOST_NOTIFY) != 0) {
		i2c_addr address = smb_read(bus, SMB_NDA) >> 1;
		uint16 value = smb_read(bus, SMB_NDLB)
			| ((uint16)smb_read(bus, SMB_NDHB) << 8);
		smb_write(bus, SMB_SSTS, SMB_SSTS_HOST_NOTIFY);

		bus->notifyCount++;
		SpinLocker locker(bus->notifyLock);
		if (bus->notifyHook != NULL)
			bus->notifyHook(bus->notifyCookie, address, value);
		else
			bus->notifyUnclaimed++;
		result = B_HANDLED_INTERRUPT;
	}

	// Host transaction completion. Reading HSTS takes the IUS semaphore
	// (332995 volume 2, HSTS bit 6: a read returns the old value and sets
	// it), so HSTS is only read while this driver holds the semaphore.
	// Otherwise a Host Notify interrupt took it, nobody released it, and
	// every later transaction found the bus "in use by firmware".
	if (atomic_get(&bus->ownsHardware) == 0)
		return result;

	uint8 status = smb_read(bus, SMB_HSTS);
	if ((status & SMB_HSTS_DONE) != 0) {
		smb_write(bus, SMB_HSTS, status & SMB_HSTS_CLEAR);
		if (atomic_get(&bus->transactionPending) != 0) {
			// store the status before giving up the pending flag: a waiter
			// that just timed out reads it once it loses the flag
			bus->transactionStatus = status;
			if (atomic_test_and_set(&bus->transactionPending, 0, 1) == 1)
				bus->transactionDone.NotifyAll();
		}
		// a late completion after a timeout is acknowledged as well, so
		// that the level triggered line goes quiet
		result = B_HANDLED_INTERRUPT;
	}

	return result;
}


//	#pragma mark - host transactions


static void release_hardware(i801_smbus* bus);


static status_t
acquire_hardware(i801_smbus* bus)
{
	// The first read after a release returns IUS = 0 and sets it.
	bigtime_t end = system_time() + kSemaphoreTimeout;
	while ((smb_read(bus, SMB_HSTS) & SMB_HSTS_IUS) != 0) {
		if (system_time() > end) {
			ERROR("SMBus in use by firmware, giving up\n");
			return B_BUSY;
		}
		snooze(100);
	}
	atomic_set(&bus->ownsHardware, 1);

	// wait for a transaction started by someone else to finish
	end = system_time() + kTransactionTimeout;
	while ((smb_read(bus, SMB_HSTS) & SMB_HSTS_HBSY) != 0) {
		if (system_time() > end) {
			release_hardware(bus);
			return B_BUSY;
		}
		snooze(50);
	}
	return B_OK;
}


static void
release_hardware(i801_smbus* bus)
{
	atomic_set(&bus->ownsHardware, 0);
	// writing 1 to IUS makes the next read return 0
	smb_write(bus, SMB_HSTS, SMB_HSTS_IUS);
}


static void
kill_transaction(i801_smbus* bus)
{
	smb_write(bus, SMB_HCTL, SMB_HCTL_KILL);
	for (int i = 0; i < 100; i++) {
		if ((smb_read(bus, SMB_HSTS) & SMB_HSTS_HBSY) == 0)
			break;
		snooze(10);
	}
	// KILL must be cleared again before the controller accepts commands
	smb_write(bus, SMB_HCTL, 0);
	smb_write(bus, SMB_HSTS, SMB_HSTS_CLEAR);
}


static status_t
status_to_error(i801_smbus* bus, uint8 status)
{
	if ((status & SMB_HSTS_FAIL) != 0)
		return B_INTERRUPTED;
	if ((status & SMB_HSTS_BERR) != 0)
		return B_IO_ERROR;			// collision, worth a retry
	if ((status & SMB_HSTS_DERR) != 0) {
		if ((smb_read(bus, SMB_AUXS) & SMB_AUXS_CRCE) != 0) {
			smb_write(bus, SMB_AUXS, SMB_AUXS_CRCE);
			return B_BAD_DATA;
		}
		return B_DEV_NOT_READY;		// NACK or device timeout
	}
	if ((status & SMB_HSTS_INTR) == 0)
		return B_ERROR;
	return B_OK;
}


// Runs a prepared command (TSA, HCMD, HD0/HD1 and the block buffer are
// set up) and waits for it. Returns the final HSTS in *_status.
static status_t
run_transaction(i801_smbus* bus, uint8 command, uint8* _status)
{
	smb_write(bus, SMB_HSTS, SMB_HSTS_CLEAR);

	uint8 status = 0;
	if (bus->interruptsUsable) {
		ConditionVariableEntry entry;
		bus->transactionDone.Add(&entry);
		atomic_set(&bus->transactionPending, 1);

		smb_write(bus, SMB_HCTL, command | SMB_HCTL_INTREN | SMB_HCTL_START);

		status_t waitStatus = entry.Wait(B_RELATIVE_TIMEOUT,
			kTransactionTimeout);
		if (waitStatus == B_OK) {
			status = bus->transactionStatus;
		} else if (atomic_test_and_set(&bus->transactionPending, 0, 1)
				== 1) {
			// no interrupt: look at the hardware once more
			status = smb_read(bus, SMB_HSTS);
			if ((status & SMB_HSTS_DONE) == 0) {
				ERROR("transaction timed out (HSTS 0x%02x)\n", status);
				kill_transaction(bus);
				return B_TIMED_OUT;
			}
			TRACE("completion without interrupt\n");
			smb_write(bus, SMB_HSTS, status & SMB_HSTS_CLEAR);
		} else {
			// the interrupt arrived just after the timeout
			status = bus->transactionStatus;
		}
	} else {
		smb_write(bus, SMB_HCTL, command | SMB_HCTL_START);

		bigtime_t end = system_time() + kTransactionTimeout;
		while (true) {
			status = smb_read(bus, SMB_HSTS);
			if ((status & SMB_HSTS_HBSY) == 0
				&& (status & SMB_HSTS_DONE) != 0)
				break;
			if (system_time() > end) {
				ERROR("transaction timed out (HSTS 0x%02x)\n", status);
				kill_transaction(bus);
				return B_TIMED_OUT;
			}
			snooze(25);
		}
		smb_write(bus, SMB_HSTS, status & SMB_HSTS_CLEAR);
	}

	*_status = status;
	return status_to_error(bus, status);
}


static void
set_block_buffer(i801_smbus* bus, bool enable)
{
	// AAC stays off: this driver never uses PEC, and a block write needs
	// PEC_EN and AAC both clear (332995 volume 1, "Block Read/Write").
	smb_write(bus, SMB_AUXC, enable ? SMB_AUXC_E32B : 0);
}


static status_t
exec_locked(i801_smbus* bus, i2c_op op, i2c_addr address,
	const uint8* cmd, size_t cmdLength, uint8* data, size_t dataLength)
{
	const bool read = IS_READ_OP(op);
	uint8 command;

	smb_write(bus, SMB_TSA, (uint8)(address << 1)
		| (read ? SMB_TSA_READ : SMB_TSA_WRITE));

	if (IS_BLOCK_OP(op)) {
		if (cmdLength != 1)
			return B_BAD_VALUE;
		smb_write(bus, SMB_HCMD, cmd[0]);
		command = SMB_CMD_BLOCK;
		set_block_buffer(bus, true);

		if (!read) {
			if (dataLength == 0 || dataLength > I2C_SMBUS_BLOCK_MAX)
				return B_BAD_VALUE;
			smb_write(bus, SMB_HD0, (uint8)dataLength);
			smb_read(bus, SMB_HCTL);	// resets the block buffer index
			for (size_t i = 0; i < dataLength; i++)
				smb_write(bus, SMB_HBD, data[i]);
		} else if (dataLength < 1)
			return B_BAD_VALUE;
	} else if (cmdLength == 0 && dataLength == 0) {
		command = SMB_CMD_QUICK;
	} else if (cmdLength == 0 && dataLength == 1) {
		// receive byte, or send byte with the data byte as the code
		command = SMB_CMD_BYTE;
		if (!read)
			smb_write(bus, SMB_HCMD, data[0]);
	} else if (cmdLength == 1 && dataLength == 0 && !read) {
		command = SMB_CMD_BYTE;
		smb_write(bus, SMB_HCMD, cmd[0]);
	} else if (cmdLength == 1 && dataLength == 1) {
		command = SMB_CMD_BYTE_DATA;
		smb_write(bus, SMB_HCMD, cmd[0]);
		if (!read)
			smb_write(bus, SMB_HD0, data[0]);
	} else if (cmdLength == 1 && dataLength == 2) {
		command = SMB_CMD_WORD_DATA;
		smb_write(bus, SMB_HCMD, cmd[0]);
		if (!read) {
			smb_write(bus, SMB_HD0, data[0]);
			smb_write(bus, SMB_HD1, data[1]);
		}
	} else {
		// plain I2C transfers are not what this controller is for
		return B_NOT_SUPPORTED;
	}

	uint8 status;
	status_t result = run_transaction(bus, command, &status);

	if (result == B_OK && read) {
		if (command == SMB_CMD_BLOCK) {
			uint8 count = smb_read(bus, SMB_HD0);
			if (count == 0 || count > I2C_SMBUS_BLOCK_MAX)
				result = B_BAD_DATA;
			else if ((size_t)count + 1 > dataLength)
				result = B_BUFFER_OVERFLOW;
			else {
				data[0] = count;
				smb_read(bus, SMB_HCTL);	// reset the buffer index
				for (uint8 i = 0; i < count; i++)
					data[1 + i] = smb_read(bus, SMB_HBD);
			}
		} else if (command == SMB_CMD_BYTE
				|| command == SMB_CMD_BYTE_DATA) {
			data[0] = smb_read(bus, SMB_HD0);
		} else if (command == SMB_CMD_WORD_DATA) {
			data[0] = smb_read(bus, SMB_HD0);
			data[1] = smb_read(bus, SMB_HD1);
		}
	}

	if (command == SMB_CMD_BLOCK)
		set_block_buffer(bus, false);

	if (result != B_OK) {
		TRACE("addr 0x%02x op %d failed: %s (HSTS 0x%02x)\n", address,
			op, strerror(result), status);
	}
	return result;
}


//	#pragma mark - i2c_sim_interface


static void
set_sim(i2c_bus_cookie cookie, i2c_bus sim)
{
	CALLED();
	((i801_smbus*)cookie)->sim = sim;
}


static status_t
exec_command(i2c_bus_cookie cookie, i2c_op op, i2c_addr slaveAddress,
	const void* cmdBuffer, size_t cmdLength, void* dataBuffer,
	size_t dataLength)
{
	CALLED();
	i801_smbus* bus = (i801_smbus*)cookie;

	if (slaveAddress > 0x7f)
		return B_BAD_VALUE;
	if ((cmdLength > 0 && cmdBuffer == NULL)
		|| (dataLength > 0 && dataBuffer == NULL))
		return B_BAD_VALUE;

	MutexLocker locker(bus->hwLock);

	status_t status = acquire_hardware(bus);
	if (status != B_OK)
		return status;

	status = exec_locked(bus, op, slaveAddress, (const uint8*)cmdBuffer,
		cmdLength, (uint8*)dataBuffer, dataLength);

	release_hardware(bus);
	return status;
}


static status_t
scan_bus(i2c_bus_cookie cookie)
{
	CALLED();
	i801_smbus* bus = (i801_smbus*)cookie;

	// A Synaptics InterTouch pad enumerates as a PS/2 device in ACPI; its
	// SMBus side has no ACPI node. Register a device at 0x2c when ACPI
	// lists one of the known touchpads. Whether the pad actually answers
	// on SMBus is up to the peripheral driver to find out.
	for (int i = 0; kInterTouchPnpIds[i] != NULL; i++) {
		char path[128];
		if (gACPI->get_device(kInterTouchPnpIds[i], 0, path, sizeof(path))
				!= B_OK)
			continue;

		TRACE_ALWAYS("%s at %s, registering SMBus device 0x%02x\n",
			kInterTouchPnpIds[i], path, I801_SYNAPTICS_RMI4_ADDRESS);

		static char hid[] = I801_SYNAPTICS_RMI4_HID;
		static char* cid[] = { hid, NULL };
		return gI2c->register_device(bus->sim, I801_SYNAPTICS_RMI4_ADDRESS,
			hid, cid, NULL);
	}
	return B_OK;
}


static status_t
acquire_bus(i2c_bus_cookie cookie)
{
	return mutex_lock(&((i801_smbus*)cookie)->lock);
}


static void
release_bus(i2c_bus_cookie cookie)
{
	mutex_unlock(&((i801_smbus*)cookie)->lock);
}


static status_t
set_host_notify_hook(i2c_bus_cookie cookie, i2c_host_notify_hook hook,
	void* hookCookie)
{
	CALLED();
	i801_smbus* bus = (i801_smbus*)cookie;

	if (hook != NULL && !bus->interruptsUsable)
		return B_NOT_SUPPORTED;

	{
		InterruptsSpinLocker locker(bus->notifyLock);
		bus->notifyHook = hook;
		bus->notifyCookie = hookCookie;
	}

	uint8 scmd = bus->savedScmd & ~SMB_SCMD_HNW;
	if (hook != NULL) {
		// drop a notification that waited from before; the slave
		// repeats it
		smb_clear_host_notify(bus);
		scmd |= SMB_SCMD_HNI;
	} else
		scmd &= ~SMB_SCMD_HNI;

	status_t status = smb_write_scmd(bus, scmd);
	if (status != B_OK)
		ERROR("SCMD write did not take effect\n");
	else {
		TRACE_ALWAYS("Host Notify %s\n",
			hook != NULL ? "enabled" : "disabled");
	}
	return status;
}


//	#pragma mark - sim module


static status_t
init_bus(device_node* node, void** _cookie)
{
	CALLED();

	driver_module_info* driver;
	i801_smbus* bus;
	device_node* parent = gDeviceManager->get_parent_node(node);
	gDeviceManager->get_driver(parent, &driver, (void**)&bus);
	gDeviceManager->put_node(parent);

	mutex_init(&bus->lock, "i801 smbus");
	mutex_init(&bus->hwLock, "i801 smbus hardware");
	bus->transactionDone.Init(bus, "i801 smbus transaction");
	B_INITIALIZE_SPINLOCK(&bus->notifyLock);

	// no Host Notify until someone asks for it, no PEC
	smb_write_scmd(bus, bus->savedScmd & ~SMB_SCMD_HNI);
	smb_write(bus, SMB_AUXC, 0);
	smb_clear_host_notify(bus);
	smb_write(bus, SMB_HSTS, SMB_HSTS_CLEAR);

	if (bus->interruptsUsable) {
		status_t status = install_io_interrupt_handler(bus->irq,
			&i801_interrupt_handler, bus, 0);
		if (status != B_OK) {
			ERROR("cannot install interrupt handler on %" B_PRIu32
				", polling\n", bus->irq);
			bus->interruptsUsable = false;
		}
	}

	TRACE_ALWAYS("SMBus at I/O 0x%04" B_PRIxADDR ", %s\n", bus->ioBase,
		bus->interruptsUsable ? "interrupt driven, Host Notify available"
			: "polling, no Host Notify");

	*_cookie = bus;
	return B_OK;
}


static void
uninit_bus(void* cookie)
{
	CALLED();
	i801_smbus* bus = (i801_smbus*)cookie;

	set_host_notify_hook(bus, NULL, NULL);
	if (bus->interruptsUsable) {
		remove_io_interrupt_handler(bus->irq, &i801_interrupt_handler, bus);
	}

	// leave the controller as the firmware configured it
	smb_write_scmd(bus, bus->savedScmd);
	smb_write(bus, SMB_AUXC, bus->savedAuxc);
	mutex_destroy(&bus->hwLock);
	mutex_destroy(&bus->lock);
}


//	#pragma mark - PCI driver module


static float
supports_device(device_node* parent)
{
	const char* busName;
	uint16 vendorID, deviceID;

	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &busName,
			false) != B_OK
		|| gDeviceManager->get_attr_uint16(parent, B_DEVICE_VENDOR_ID,
			&vendorID, false) != B_OK
		|| gDeviceManager->get_attr_uint16(parent, B_DEVICE_ID, &deviceID,
			false) != B_OK) {
		return -1;
	}

	if (strcmp(busName, "pci") != 0 || vendorID != 0x8086)
		return 0.0f;

	for (int i = 0; kSupportedDevices[i] != 0; i++) {
		if (kSupportedDevices[i] == deviceID)
			return 0.8f;
	}
	return 0.0f;
}


static status_t
register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Intel PCH SMBus" }},
		{}
	};

	return gDeviceManager->register_node(parent,
		I801_SMBUS_PCI_DEVICE_MODULE_NAME, attrs, NULL, NULL);
}


static status_t
init_device(device_node* node, void** _cookie)
{
	CALLED();

	i801_smbus* bus = new(std::nothrow) i801_smbus;
	if (bus == NULL)
		return B_NO_MEMORY;
	memset((void*)bus, 0, sizeof(*bus));

	{
		device_node* pciParent = gDeviceManager->get_parent_node(node);
		gDeviceManager->get_driver(pciParent,
			(driver_module_info**)&bus->pci, (void**)&bus->device);
		gDeviceManager->put_node(pciParent);
	}
	bus->driverNode = node;
	bus->pci->get_pci_info(bus->device, &bus->pciInfo);

	bus->ioBase = bus->pci->read_pci_config(bus->device, SMB_PCI_SBA, 4)
		& SMB_SBA_MASK;
	if (bus->ioBase == 0) {
		ERROR("SMBus I/O BAR not assigned\n");
		delete bus;
		return B_DEV_CONFIGURATION_ERROR;
	}

	// I/O decode on, INTx not masked
	uint16 command = bus->pci->read_pci_config(bus->device, PCI_command, 2);
	command |= PCI_command_io;
	command &= ~PCI_command_int_disable;
	bus->pci->write_pci_config(bus->device, PCI_command, 2, command);

	bus->savedHcfg = bus->pci->read_pci_config(bus->device, SMB_PCI_HCFG, 1);
	if ((bus->savedHcfg & SMB_HCFG_I2C_EN) != 0) {
		ERROR("controller in I2C mode, not touching it\n");
		delete bus;
		return B_DEV_CONFIGURATION_ERROR;
	}
	if ((bus->savedHcfg & SMB_HCFG_HST_EN) == 0) {
		TRACE_ALWAYS("enabling host controller\n");
		bus->pci->write_pci_config(bus->device, SMB_PCI_HCFG, 1,
			bus->savedHcfg | SMB_HCFG_HST_EN);
	}

	bus->savedScmd = smb_read(bus, SMB_SCMD);
	bus->savedAuxc = smb_read(bus, SMB_AUXC);

	bus->irq = bus->pciInfo.u.h0.interrupt_line;
	if (bus->irq == 0xff)
		bus->irq = 0;
	bus->interruptsUsable = bus->irq != 0
		&& (bus->savedHcfg & SMB_HCFG_SMB_SMI_EN) == 0;
	if ((bus->savedHcfg & SMB_HCFG_SMB_SMI_EN) != 0)
		TRACE_ALWAYS("firmware routes SMBus interrupts to SMI#\n");

	TRACE_ALWAYS("8086:%04x at %u:%u:%u, HCFG 0x%02x SCMD 0x%02x, "
		"IRQ %" B_PRIu32 "\n", bus->pciInfo.device_id, bus->pciInfo.bus,
		bus->pciInfo.device, bus->pciInfo.function, bus->savedHcfg,
		bus->savedScmd, bus->irq);

	*_cookie = bus;
	return B_OK;
}


static void
uninit_device(void* cookie)
{
	i801_smbus* bus = (i801_smbus*)cookie;
	// HST_EN may have been set by us; firmware expects to find it as it
	// left it
	bus->pci->write_pci_config(bus->device, SMB_PCI_HCFG, 1, bus->savedHcfg);
	delete bus;
}


static status_t
register_child_devices(void* cookie)
{
	CALLED();
	i801_smbus* bus = (i801_smbus*)cookie;

	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Intel PCH SMBus host" }},
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{ .string = I2C_FOR_CONTROLLER_MODULE_NAME }},
		{ NULL }
	};

	return gDeviceManager->register_node(bus->driverNode,
		I801_SMBUS_SIM_MODULE_NAME, attrs, NULL, NULL);
}


//	#pragma mark -


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{ B_ACPI_MODULE_NAME, (module_info**)&gACPI },
	{ I2C_FOR_CONTROLLER_MODULE_NAME, (module_info**)&gI2c },
	{}
};


static driver_module_info sI801PciDevice = {
	{
		I801_SMBUS_PCI_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	supports_device,
	register_device,
	init_device,
	uninit_device,
	register_child_devices,
	NULL,	// rescan
	NULL,	// device removed
};


static i2c_sim_interface sI801SimModule = {
	{
		{
			I801_SMBUS_SIM_MODULE_NAME,
			0,
			NULL
		},

		NULL,	// supports device
		NULL,	// register device
		init_bus,
		uninit_bus,
		NULL,	// register child devices
		NULL,	// rescan
		NULL,	// device removed
	},

	set_sim,
	exec_command,
	scan_bus,
	acquire_bus,
	release_bus,
	NULL,	// install_interrupt_handler
	NULL,	// uninstall_interrupt_handler
	set_host_notify_hook,
};


module_info* modules[] = {
	(module_info*)&sI801PciDevice,
	(module_info*)&sI801SimModule,
	NULL
};
