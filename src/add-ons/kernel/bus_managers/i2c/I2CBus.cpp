/*
 * Copyright 2020, Jérôme Duval, jerome.duval@gmail.com.
 * Distributed under the terms of the MIT License.
 */


#include "I2CPrivate.h"


I2CBus::I2CBus(device_node *node, uint8 id)
	:
	fNode(node),
	fID(id),
	fController(NULL),
	fCookie(NULL),
	fHostNotifyEnabled(false)
{
	CALLED();
	B_INITIALIZE_SPINLOCK(&fHostNotifyLock);
	memset(fHostNotify, 0, sizeof(fHostNotify));

	device_node *parent = gDeviceManager->get_parent_node(node);
	status_t status = gDeviceManager->get_driver(parent,
		(driver_module_info **)&fController, &fCookie);
	gDeviceManager->put_node(parent);

	if (status != B_OK)
		return;

	fController->set_i2c_bus(fCookie, this);

}


I2CBus::~I2CBus()
{
	if (fHostNotifyEnabled && fController->set_host_notify_hook != NULL)
		fController->set_host_notify_hook(fCookie, NULL, NULL);
}


status_t
I2CBus::InitCheck()
{
	return B_OK;
}


status_t
I2CBus::ExecCommand(i2c_op op, i2c_addr slaveAddress, const void *cmdBuffer,
	size_t cmdLength, void* dataBuffer, size_t dataLength)
{
	CALLED();
	return fController->exec_command(fCookie, op, slaveAddress, cmdBuffer, cmdLength,
		dataBuffer, dataLength);
}


status_t
I2CBus::RegisterDevice(i2c_addr slaveAddress, char* hid, char** cid,
	acpi_handle acpiHandle)
{
	CALLED();

	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "I2C device" }},

		// connection
		{ I2C_DEVICE_SLAVE_ADDR_ITEM, B_UINT16_TYPE, { .ui16 = slaveAddress }},

		// description of peripheral drivers
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "i2c" }},

		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_FIND_MULTIPLE_CHILDREN }},

		{ ACPI_DEVICE_HID_ITEM, B_STRING_TYPE, { .string = hid }},

		{ ACPI_DEVICE_CID_ITEM, B_STRING_TYPE, { .string = cid[0] }},

		{ ACPI_DEVICE_HANDLE_ITEM, B_UINT64_TYPE, { .ui64 = (addr_t)acpiHandle }},

		{ NULL }
	};

	return gDeviceManager->register_node(fNode, I2C_DEVICE_MODULE_NAME, attrs,
		NULL, NULL);
}


status_t
I2CBus::Scan()
{
	CALLED();
	if (fController->scan_bus != NULL)
		fController->scan_bus(fCookie);
	return B_OK;
}


status_t
I2CBus::AcquireBus()
{
	CALLED();
	if (fController->acquire_bus != NULL)
		return fController->acquire_bus(fCookie);
	return B_OK;
}


void
I2CBus::ReleaseBus()
{
	CALLED();
	if (fController->release_bus != NULL)
		fController->release_bus(fCookie);
}


// interrupt context
/*static*/ void
I2CBus::_HostNotify(void* cookie, i2c_addr address, uint16 data)
{
	I2CBus* bus = (I2CBus*)cookie;
	i2c_host_notify_hook hook = NULL;
	void* hookCookie = NULL;

	SpinLocker locker(bus->fHostNotifyLock);
	for (int i = 0; i < kMaxHostNotify; i++) {
		if (bus->fHostNotify[i].hook != NULL
			&& bus->fHostNotify[i].address == address) {
			hook = bus->fHostNotify[i].hook;
			hookCookie = bus->fHostNotify[i].cookie;
			break;
		}
	}
	// Call the hook with the lock held, so that UninstallHostNotify()
	// cannot return while the hook runs.
	if (hook != NULL)
		hook(hookCookie, address, data);
	else
		TRACE("host notify from unclaimed address 0x%x\n", address);
}


status_t
I2CBus::InstallHostNotify(i2c_addr slaveAddress, i2c_host_notify_hook hook,
	void* cookie)
{
	CALLED();
	if (hook == NULL)
		return B_BAD_VALUE;
	if (fController->set_host_notify_hook == NULL)
		return B_NOT_SUPPORTED;

	status_t status = B_NO_MEMORY;
	{
		InterruptsSpinLocker locker(fHostNotifyLock);
		for (int i = 0; i < kMaxHostNotify; i++) {
			if (fHostNotify[i].hook != NULL
				&& fHostNotify[i].address == slaveAddress)
				return B_BUSY;
		}
		for (int i = 0; i < kMaxHostNotify; i++) {
			if (fHostNotify[i].hook == NULL) {
				fHostNotify[i].address = slaveAddress;
				fHostNotify[i].cookie = cookie;
				fHostNotify[i].hook = hook;
				status = B_OK;
				break;
			}
		}
	}
	if (status != B_OK)
		return status;

	if (!fHostNotifyEnabled) {
		status = fController->set_host_notify_hook(fCookie, &_HostNotify,
			this);
		if (status != B_OK) {
			UninstallHostNotify(slaveAddress);
			return status;
		}
		fHostNotifyEnabled = true;
	}
	return B_OK;
}


void
I2CBus::UninstallHostNotify(i2c_addr slaveAddress)
{
	CALLED();
	InterruptsSpinLocker locker(fHostNotifyLock);
	for (int i = 0; i < kMaxHostNotify; i++) {
		if (fHostNotify[i].hook != NULL
			&& fHostNotify[i].address == slaveAddress) {
			fHostNotify[i].hook = NULL;
			fHostNotify[i].cookie = NULL;
		}
	}
}


static status_t
i2c_init_bus(device_node *node, void **_bus)
{
	CALLED();
	uint8 pathID;
	if (gDeviceManager->get_attr_uint8(node, I2C_BUS_PATH_ID_ITEM, &pathID,
		false) != B_OK) {
		return B_ERROR;
	}

	I2CBus *bus = new(std::nothrow) I2CBus(node, pathID);
	if (bus == NULL)
		return B_NO_MEMORY;

	status_t result = bus->InitCheck();
	if (result != B_OK) {
		ERROR("failed to set up i2c bus object\n");
		return result;
	}

	*_bus = bus;

	char name[B_DEV_NAME_LENGTH];
	snprintf(name, sizeof(name), "bus/i2c/%d/bus_raw", pathID);

	return gDeviceManager->publish_device(node, name, I2C_BUS_RAW_MODULE_NAME);
}


static void
i2c_uninit_bus(void *_bus)
{
	CALLED();
	I2CBus *bus = (I2CBus *)_bus;
	delete bus;
}


static status_t
i2c_scan_bus(void *_bus)
{
	I2CBus *bus = (I2CBus *)_bus;
	return bus->Scan();
}


status_t
i2c_bus_exec_command(void* _bus, i2c_op op, i2c_addr slaveAddress,
		const void *cmdBuffer, size_t cmdLength, void* dataBuffer,
		size_t dataLength)
{
	CALLED();
	I2CBus* bus = (I2CBus*)_bus;
	return bus->ExecCommand(op, slaveAddress, cmdBuffer, cmdLength,
		dataBuffer, dataLength);
}


static status_t
i2c_bus_acquire_bus(void* _bus)
{
	CALLED();
	I2CBus* bus = (I2CBus*)_bus;
	return bus->AcquireBus();
}


static void
i2c_bus_release_bus(void* _bus)
{
	CALLED();
	I2CBus* bus = (I2CBus*)_bus;
	return bus->ReleaseBus();
}


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		case B_MODULE_UNINIT:
			return B_OK;

		default:
			break;
	}

	return B_ERROR;
}


i2c_bus_interface gI2CBusModule = {
	{
		{
			I2C_BUS_MODULE_NAME,
			0,
			std_ops
		},

		NULL, // supported devices
		NULL, // register node
		i2c_init_bus,
		i2c_uninit_bus,
		i2c_scan_bus, // register child devices
		NULL, // rescan
	},

	i2c_bus_exec_command,
	i2c_bus_acquire_bus,
	i2c_bus_release_bus,
};

