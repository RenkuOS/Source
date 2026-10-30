/*
 * Copyright 2020, Jérôme Duval, jerome.duval@gmail.com.
 * Distributed under the terms of the MIT License.
 */


#include <new>
#include <stdio.h>
#include <string.h>

#include <ACPI.h>
#include <ByteOrder.h>
#include <condition_variable.h>

#include "pch_i2c.h"

extern "C" {
#	include "amlcode.h"
}


struct {
	const char* name;
	pch_version version;
} pch_acpi_devices [] = {
	{"INT33C2", PCH_ATOM},
	{"INT33C3", PCH_ATOM},
	{"INT3332", PCH_ATOM},
	{"INT3433", PCH_ATOM},
	{"INT3442", PCH_ATOM},
	{"INT3443", PCH_ATOM},
	{"INT3444", PCH_ATOM},
	{"INT3445", PCH_ATOM},
	{"INT3446", PCH_ATOM},
	{"INT3447", PCH_ATOM},
	{"80860AAC", PCH_ATOM},
	{"80865AAC", PCH_ATOM},
	{"80860F41", PCH_ATOM},
	{"808662C1", PCH_ATOM},
	{"AMD0010", PCH_ATOM},
	{"AMDI0010", PCH_ATOM},
	{"AMDI0510", PCH_ATOM},
	{"APMC0D0F", PCH_EMAG},
	{NULL, PCH_NONE}
};

struct pch_i2c_region_context {
	// Must be the first member: ACPICA passes the handler context to the
	// region handler and fills in the connection of the accessed field there.
	ACPI_CONNECTION_INFO connection;
	pch_i2c_sim_info* bus;
};

typedef struct {
	pch_i2c_sim_info info;
	acpi_device_module_info* acpi;
	acpi_device device;

	pch_i2c_region_context region;
	bool region_handler_installed;
} pch_i2c_acpi_sim_info;


//	#pragma mark - ACPI GenericSerialBus operation region


// Buffer of a GenericSerialBus field access: status, length, then the data
// (ACPI 6.x, section 5.5.2.4.6.2).
#define GSBUS_STATUS_OK			0x00
#define GSBUS_STATUS_ERROR		0x01
#define GSBUS_HEADER_SIZE		2


/*!	Returns the 7-bit slave address of an I2cSerialBus descriptor, as found in
	the AML byte stream of a Connection() (ACPI 6.x, section 6.4.3.8.2.1).
*/
static bool
parse_i2c_connection(const uint8* descriptor, uint32 length,
	i2c_addr* _address)
{
	// large resource, serial bus type 1 (I2C), 7-bit addressing
	if (descriptor == NULL || length < 18 || descriptor[0] != 0x8e
		|| descriptor[5] != 1 || (descriptor[7] & 1) != 0) {
		return false;
	}

	uint16 address;
	memcpy(&address, descriptor + 16, sizeof(uint16));
	*_address = B_LENDIAN_TO_HOST_INT16(address);
	return true;
}


static acpi_status
gsbus_region_handler(uint32 function, acpi_physical_address address,
	uint32 bitWidth, int* _value, void* handlerContext, void* regionContext)
{
	pch_i2c_region_context* context = (pch_i2c_region_context*)handlerContext;
	uint8* buffer = (uint8*)_value;

	i2c_addr slave;
	if (!parse_i2c_connection(context->connection.Connection,
			context->connection.Length, &slave)) {
		ERROR("GenericSerialBus: unsupported connection\n");
		return AE_BAD_PARAMETER;
	}

	size_t length;
	switch (function >> 16) {
		case AML_FIELD_ATTRIB_BYTE:
			length = 1;
			break;
		case AML_FIELD_ATTRIB_WORD:
			length = 2;
			break;
		case AML_FIELD_ATTRIB_BYTES:
			length = context->connection.AccessLength;
			break;
		default:
			ERROR("GenericSerialBus: access attribute %#" B_PRIx32
				" not supported\n", function >> 16);
			return AE_BAD_PARAMETER;
	}
	if (length == 0 || length > 32 || address > 0xff)
		return AE_BAD_PARAMETER;

	uint8* data = buffer + GSBUS_HEADER_SIZE;
	uint8 command = (uint8)address;
	status_t status;
	if ((function & ACPI_IO_MASK) == ACPI_WRITE) {
		uint8 out[33];
		out[0] = command;
		memcpy(out + 1, data, length);
		status = pch_i2c_transfer(context->bus, I2C_OP_WRITE_STOP, slave,
			out, length + 1, NULL, 0);
	} else {
		status = pch_i2c_transfer(context->bus, I2C_OP_READ_STOP, slave,
			&command, 1, data, length);
	}

	TRACE("GenericSerialBus: %s %#x register %#x, %" B_PRIuSIZE
		" bytes: %s\n", (function & ACPI_IO_MASK) == ACPI_WRITE ? "write"
			: "read", slave, command, length, strerror(status));

	buffer[0] = status == B_OK ? GSBUS_STATUS_OK : GSBUS_STATUS_ERROR;
	buffer[1] = (uint8)length;
	return status == B_OK ? AE_OK : AE_ERROR;
}


static acpi_status
gsbus_region_setup(acpi_handle region, uint32 function, void* handlerContext,
	void** regionContext)
{
	if (function == ACPI_REGION_DEACTIVATE)
		*regionContext = NULL;
	else
		*regionContext = handlerContext;

	return AE_OK;
}


static void
install_region_handler(void* cookie)
{
	pch_i2c_acpi_sim_info* bus = (pch_i2c_acpi_sim_info*)cookie;

	// Installing the handler runs the _REG methods of the regions: from now
	// on AML code may access devices on this bus (for example a PMIC).
	bus->region.bus = &bus->info;
	if (bus->acpi->install_address_space_handler(bus->device,
			ACPI_ADR_SPACE_GSBUS, gsbus_region_handler, gsbus_region_setup,
			&bus->region) == B_OK) {
		bus->region_handler_installed = true;
	} else
		ERROR("installing the GenericSerialBus region handler failed\n");
}


static void
remove_region_handler(void* cookie)
{
	pch_i2c_acpi_sim_info* bus = (pch_i2c_acpi_sim_info*)cookie;
	if (!bus->region_handler_installed)
		return;

	bus->acpi->remove_address_space_handler(bus->device, ACPI_ADR_SPACE_GSBUS,
		gsbus_region_handler);
	bus->region_handler_installed = false;
}


static status_t
pch_i2c_acpi_set_powerstate(pch_i2c_acpi_sim_info* info, uint8 power)
{
	status_t status = info->acpi->evaluate_method(info->device,
		power == 1 ? "_PS0" : "_PS3", NULL, NULL);
	return status;
}



static acpi_status
pch_i2c_scan_parse_callback(ACPI_RESOURCE *res, void *context)
{
	struct pch_i2c_crs* crs = (struct pch_i2c_crs*)context;

	if (res->Type == ACPI_RESOURCE_TYPE_IRQ) {
		crs->irq = res->Data.Irq.Interrupts[0];
		crs->irq_triggering = res->Data.Irq.Triggering;
		crs->irq_polarity = res->Data.Irq.Polarity;
		crs->irq_shareable = res->Data.Irq.Shareable;
	} else if (res->Type == ACPI_RESOURCE_TYPE_EXTENDED_IRQ) {
		crs->irq = res->Data.ExtendedIrq.Interrupts[0];
		crs->irq_triggering = res->Data.ExtendedIrq.Triggering;
		crs->irq_polarity = res->Data.ExtendedIrq.Polarity;
		crs->irq_shareable = res->Data.ExtendedIrq.Shareable;
	} else if (res->Type == ACPI_RESOURCE_TYPE_FIXED_MEMORY32) {
		crs->addr_bas = res->Data.FixedMemory32.Address;
		crs->addr_len = res->Data.FixedMemory32.AddressLength;
	}

	return B_OK;
}


//	#pragma mark -


static status_t
acpi_scan_bus(i2c_bus_cookie cookie)
{
	CALLED();
	pch_i2c_acpi_sim_info* bus = (pch_i2c_acpi_sim_info*)cookie;

	bus->acpi->walk_namespace(bus->device, ACPI_TYPE_DEVICE, 1,
		pch_i2c_scan_bus_callback, NULL, bus, NULL);

	return B_OK;
}


static status_t
register_child_devices(void* cookie)
{
	CALLED();

	pch_i2c_acpi_sim_info* bus = (pch_i2c_acpi_sim_info*)cookie;
	device_node* node = bus->info.driver_node;

	char prettyName[25];
	sprintf(prettyName, "PCH I2C Controller");

	device_attr attrs[] = {
		// properties of this controller for i2c bus manager
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = prettyName }},
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{ .string = I2C_FOR_CONTROLLER_MODULE_NAME }},

		// private data to identify the device
		{ NULL }
	};

	return gDeviceManager->register_node(node, PCH_I2C_SIM_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
init_device(device_node* node, void** device_cookie)
{
	CALLED();
	status_t status = B_OK;

	pch_i2c_acpi_sim_info* bus = (pch_i2c_acpi_sim_info*)calloc(1,
		sizeof(pch_i2c_acpi_sim_info));
	if (bus == NULL)
		return B_NO_MEMORY;

	acpi_device_module_info* acpi;
	acpi_device device;
	{
		device_node* acpiParent = gDeviceManager->get_parent_node(node);
		gDeviceManager->get_driver(acpiParent, (driver_module_info**)&acpi,
			(void**)&device);

		const char* name;
		if (gDeviceManager->get_attr_string(acpiParent, ACPI_DEVICE_HID_ITEM, &name, false)
			== B_OK) {

			size_t device = 0;

			while (pch_acpi_devices[device].name) {
				if (strcmp(name, pch_acpi_devices[device].name) == 0) {
					bus->info.version = pch_acpi_devices[device].version;
					break;
				}
				device++;
			}
		}

		gDeviceManager->put_node(acpiParent);
	}

	bus->acpi = acpi;
	bus->device = device;
	bus->info.driver_node = node;
	bus->info.scan_bus = acpi_scan_bus;
	bus->info.bus_started = install_region_handler;
	bus->info.bus_stopping = remove_region_handler;
	bus->info.bus_hook_cookie = bus;

	// Attach devices for I2C resources
	struct pch_i2c_crs crs;
	status = acpi->walk_resources(device, (ACPI_STRING)"_CRS",
		pch_i2c_scan_parse_callback, &crs);
	if (status != B_OK) {
		ERROR("Error while getting I2C devices\n");
		free(bus);
		return status;
	}
	if (crs.addr_bas == 0 || crs.addr_len == 0) {
		TRACE("skipping non configured I2C devices\n");
		free(bus);
		return B_BAD_VALUE;
	}

	// Bay Trail and Cherry Trail: the bus to the PMIC is shared with the
	// P-Unit, which ACPI marks with a _SEM method returning 1.
	{
		acpi_object_type object;
		acpi_data buffer;
		buffer.pointer = &object;
		buffer.length = sizeof(object);
		if (acpi->evaluate_method(device, "_SEM", NULL, &buffer) == B_OK
			&& object.object_type == ACPI_TYPE_INTEGER
			&& object.integer.integer == 1) {
			dprintf("pch_i2c: bus at %#" B_PRIx32 " is shared with the "
				"P-Unit (PMIC bus), using its semaphore\n", crs.addr_bas);
			bus->info.punit_semaphore = true;
		}
	}

	bus->info.base_addr = crs.addr_bas;
	bus->info.map_size = crs.addr_len;
	bus->info.irq = crs.irq;

	pch_i2c_acpi_set_powerstate(bus, 1);

	*device_cookie = bus;
	return B_OK;
}


static void
uninit_device(void* device_cookie)
{
	pch_i2c_acpi_sim_info* bus = (pch_i2c_acpi_sim_info*)device_cookie;
	free(bus);
}


static status_t
register_device(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "PCH I2C ACPI"}},
		{}
	};

	return gDeviceManager->register_node(parent,
		PCH_I2C_ACPI_DEVICE_MODULE_NAME, attrs, NULL, NULL);
}


static float
supports_device(device_node* parent)
{
	CALLED();
	const char* bus;

	// make sure parent is a PCH I2C ACPI device node
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
		< B_OK) {
		return -1;
	}

	if (strcmp(bus, "acpi") != 0)
		return 0.0f;

	TRACE("found an acpi node\n");

	// check whether it's really a device
	uint32 device_type;
	if (gDeviceManager->get_attr_uint32(parent, ACPI_DEVICE_TYPE_ITEM,
			&device_type, false) != B_OK
		|| device_type != ACPI_TYPE_DEVICE) {
		return 0.0;
	}
	TRACE("found an acpi device\n");

	// check whether it's a PCH I2C device
	const char *name;
	if (gDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &name,
		false) != B_OK) {
		return 0.0;
	}
	TRACE("found an acpi device hid %s\n", name);

	size_t device = 0;

	while (pch_acpi_devices[device].name) {
		if (strcmp(name, pch_acpi_devices[device].name) == 0) {
			TRACE("PCH I2C device found! name %s\n", name);
			return 0.6f;
		}
		device++;
	}

	return 0.0f;
}


//	#pragma mark -


driver_module_info gPchI2cAcpiDevice = {
	{
		PCH_I2C_ACPI_DEVICE_MODULE_NAME,
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


