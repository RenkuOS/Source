/*
 * Copyright 2018-2021 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		B Krishnan Iyer, krishnaniyer97@gmail.com
 *		Adrien Destugues, pulkomandy@pulkomandy.tk
 */
#include "mmc_bus.h"


#define	MMC_BUS_DEVICE_NAME "bus_managers/mmc_bus/device/v1"


device_manager_info* gDeviceManager = NULL;


static status_t
mmc_bus_init(device_node* node, void** _device)
{
	CALLED();
	MMCBus* device = new(std::nothrow) MMCBus(node);
	if (device == NULL) {
		ERROR("Unable to allocate MMC bus\n");
		return B_NO_MEMORY;
	}

	status_t result = device->InitCheck();
	if (result != B_OK) {
		TRACE("failed to set up mmc bus device object\n");
		return result;
	}
	TRACE("MMC bus object created\n");

	*_device = device;
	return B_OK;
}


static void
mmc_bus_uninit(void* _device)
{
	CALLED();
	MMCBus* device = (MMCBus*)_device;
	delete device;
}


static status_t
mmc_bus_register_child(void* _device)
{
	// Publish what the first scan finds, so that a boot volume on the device
	// is there when the kernel looks for it. Devices found by later scans are
	// published by the scanning thread.
	MMCBus* bus = (MMCBus*)_device;
	bus->RegisterInitialDevices();
	return B_OK;
}


static void
mmc_bus_removed(void* _device)
{
	CALLED();
}


status_t
mmc_bus_added_device(device_node* parent)
{
	CALLED();

	device_attr attributes[] = {
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "mmc"}},
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "MMC bus root"}},
		{ NULL }
	};

	return gDeviceManager->register_node(parent, MMC_BUS_DEVICE_NAME,
		attributes, NULL, NULL);
}


static status_t
mmc_bus_execute_command(device_node* node, void* cookie, uint16_t rca,
	uint8_t command, uint32_t argument, uint32_t* result)
{
	TRACE("In mmc_bus_execute_command\n");

	MMCBus* bus = (MMCBus*)cookie;

	bus->AcquireBus();
	status_t error = bus->ExecuteCommand(rca, command, argument, result);
	bus->ReleaseBus();

	return error;
}


static status_t
mmc_bus_do_io(device_node* node, void* cookie, uint16_t rca, uint8_t command,
	IOOperation* operation, bool offsetAsSectors)
{
	MMCBus* bus = (MMCBus*)cookie;
	status_t result = B_OK;

	bus->AcquireBus();
	result = bus->DoIO(rca, command, operation, offsetAsSectors);
	bus->ReleaseBus();

	return result;
}


static void
mmc_bus_set_width(device_node* node, void* cookie, int width)
{
	MMCBus* bus = (MMCBus*)cookie;

	bus->AcquireBus();
	bus->SetBusWidth(width);
	bus->ReleaseBus();
}


static status_t
mmc_bus_set_timing(device_node* node, void* cookie, uint16_t rca, int timing,
	uint32_t flags)
{
	MMCBus* bus = (MMCBus*)cookie;

	bus->AcquireBus();
	status_t result = bus->SetTiming(rca, timing, flags);
	bus->ReleaseBus();

	return result;
}


static status_t
mmc_bus_read_data(device_node* node, void* cookie, uint16_t rca,
	uint8_t command, uint32_t argument, void* buffer, size_t length)
{
	MMCBus* bus = (MMCBus*)cookie;

	bus->AcquireBus();
	status_t result = bus->ReadData(rca, command, argument, buffer, length);
	bus->ReleaseBus();

	return result;
}


static status_t
mmc_bus_sdio_rw_direct(device_node* node, void* cookie, uint16_t rca,
	bool isWrite, uint8_t function, uint32_t address, uint8_t value,
	uint8_t* _result)
{
	MMCBus* bus = (MMCBus*)cookie;

	bus->AcquireBus();
	status_t result = bus->DirectIO(rca, isWrite, function, address, value,
		_result);
	bus->ReleaseBus();

	return result;
}


static status_t
mmc_bus_sdio_rw_extended(device_node* node, void* cookie, uint16_t rca,
	bool isWrite, uint8_t function, uint32_t address, bool incrementAddress,
	void* buffer, uint32_t blockCount, uint32_t blockSize)
{
	MMCBus* bus = (MMCBus*)cookie;

	bus->AcquireBus();
	status_t result = bus->ExtendedIO(rca, isWrite, function, address,
		incrementAddress, buffer, blockCount, blockSize);
	bus->ReleaseBus();

	return result;
}


static status_t
mmc_bus_sdio_set_interrupt_handler(device_node* node, void* cookie,
	void (*handler)(void*), void* data)
{
	MMCBus* bus = (MMCBus*)cookie;
	return bus->SetCardInterrupt(handler, data);
}


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			// Nothing to do
		case B_MODULE_UNINIT:
			return B_OK;

		default:
			break;
	}

	return B_ERROR;
}


driver_module_info mmc_bus_device_module = {
	{
		MMC_BUS_DEVICE_NAME,
		0,
		std_ops
	},
	NULL, // supported devices
	NULL, // register node
	mmc_bus_init,
	mmc_bus_uninit,
	mmc_bus_register_child,
	NULL, // rescan
	mmc_bus_removed,
	NULL, // suspend
	NULL // resume
};


mmc_device_interface mmc_bus_controller_module = {
	{
		{
			MMC_BUS_MODULE_NAME,
			0,
			&std_ops
		},

		NULL, // supported devices
		mmc_bus_added_device,
		NULL,
		NULL,
		NULL
	},
	mmc_bus_execute_command,
	mmc_bus_do_io,
	mmc_bus_set_width,
	mmc_bus_read_data,
	mmc_bus_set_timing,
	mmc_bus_sdio_rw_direct,
	mmc_bus_sdio_rw_extended,
	mmc_bus_sdio_set_interrupt_handler
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info **)&gDeviceManager },
	{}
};


module_info* modules[] = {
	(module_info*)&mmc_bus_controller_module,
	(module_info*)&mmc_bus_device_module,
	NULL
};
