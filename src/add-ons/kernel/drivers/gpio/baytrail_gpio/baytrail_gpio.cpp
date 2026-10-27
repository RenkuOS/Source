/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	Driver for the GPIO controllers of the Intel Atom Z3700 (Bay Trail) SoC,
	ACPI "INT33FC".

	The SoC has three GPIO communities, each described by its own ACPI device:
	SCORE (_UID 1, 102 pins), NCORE (_UID 2, 28 pins) and SUS (_UID 3, 44
	pins). Every pad has 16 bytes of registers: CONF0, CONF1, VAL and DFT. An
	ACPI pin number is the GPIO number within its community; it is not the pad
	index, a fixed table per community maps one to the other.

	This driver gives the ACPI interpreter access to the pins: it installs the
	handler of the GeneralPurposeIo operation region of each controller, so
	that AML code which reads or writes GPIO fields (for example the reset line
	of a touch screen in its _PS0 method) works. Interrupts are not handled
	yet.
*/


#include <ACPI.h>
#include <ByteOrder.h>
#include <Drivers.h>
#include <Errors.h>
#include <KernelExport.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lock.h>

extern "C" {
#	include "acpi.h"
}


#define BAYTRAIL_GPIO_DRIVER_NAME	"drivers/gpio/baytrail_gpio/driver_v1"

#define ACPI_NAME_BAYTRAIL_GPIO		"INT33FC"

//#define TRACE_BAYTRAIL_GPIO
#ifdef TRACE_BAYTRAIL_GPIO
#	define TRACE(x...)	dprintf("baytrail_gpio: " x)
#else
#	define TRACE(x...)
#endif
#define ERROR(x...)	dprintf("baytrail_gpio: " x)


// Pad registers, at pad * 16 from the start of the community
#define PAD_CONF0				0x00
#define PAD_CONF1				0x04
#define PAD_VAL					0x08
#define PAD_REGISTER_STRIDE		16

// VAL register
#define PAD_VAL_LEVEL			(1U << 0)
#define PAD_VAL_OUTPUT_DISABLE	(1U << 1)
#define PAD_VAL_INPUT_DISABLE	(1U << 2)

// CONF0 register
#define PAD_CONF0_MUX_MASK		0x7


/*	GPIO number to pad index, per community. Taken from the pad description
	of the Intel Atom Z3600/Z3700 (Bay Trail) SoC; the same values are in
	Linux, drivers/pinctrl/intel/pinctrl-baytrail.c.
*/
static const uint8 kScorePadMap[] = {
	85, 89, 93, 96, 99, 102, 98, 101, 34, 37,
	36, 38, 39, 35, 40, 84, 62, 61, 64, 59,
	54, 56, 60, 55, 63, 57, 51, 50, 53, 47,
	52, 49, 48, 43, 46, 41, 45, 42, 58, 44,
	95, 105, 70, 68, 67, 66, 69, 71, 65, 72,
	86, 90, 88, 92, 103, 77, 79, 83, 78, 81,
	80, 82, 13, 12, 15, 14, 17, 18, 19, 16,
	2, 1, 0, 4, 6, 7, 9, 8, 33, 32,
	31, 30, 29, 27, 25, 28, 26, 23, 21, 20,
	24, 22, 5, 3, 10, 11, 106, 87, 91, 104,
	97, 100,
};

static const uint8 kNcorePadMap[] = {
	19, 18, 17, 20, 21, 22, 24, 25, 23, 16,
	14, 15, 12, 26, 27, 1, 4, 8, 11, 0,
	3, 6, 10, 13, 2, 5, 9, 7,
};

static const uint8 kSusPadMap[] = {
	29, 33, 30, 31, 32, 34, 36, 35, 38, 37,
	18, 7, 11, 20, 17, 1, 8, 10, 19, 12,
	0, 2, 23, 39, 28, 27, 22, 21, 24, 25,
	26, 51, 56, 54, 49, 55, 48, 57, 50, 58,
	52, 53, 59, 40,
};

struct gpio_community {
	const char*		uid;
	const char*		name;
	const uint8*	padMap;
	uint32			pinCount;
};

static const gpio_community kCommunities[] = {
	{ "1", "SCORE", kScorePadMap, B_COUNT_OF(kScorePadMap) },
	{ "2", "NCORE", kNcorePadMap, B_COUNT_OF(kNcorePadMap) },
	{ "3", "SUS", kSusPadMap, B_COUNT_OF(kSusPadMap) },
};


struct gpio_controller {
	// Must be the first member: ACPICA passes the address of the handler
	// context to the region handler and fills in the connection of the
	// accessed field there.
	ACPI_CONNECTION_INFO		connection;

	device_node*				node;
	acpi_device_module_info*	acpi;
	acpi_device					acpiCookie;

	const gpio_community*		community;
	phys_addr_t					physicalBase;
	uint32						length;
	uint32						irq;
	area_id						area;
	addr_t						registers;
	spinlock					lock;
	bool						regionHandlerInstalled;
};


static device_manager_info* sDeviceManager;


//	#pragma mark - pad access


static inline volatile uint32*
pad_register(gpio_controller* controller, uint32 pin, uint32 offset)
{
	uint32 pad = controller->community->padMap[pin];
	return (volatile uint32*)(controller->registers
		+ pad * PAD_REGISTER_STRIDE + offset);
}


static bool
pad_in_range(gpio_controller* controller, uint32 pin)
{
	if (pin >= controller->community->pinCount)
		return false;

	uint32 pad = controller->community->padMap[pin];
	return (pad + 1) * PAD_REGISTER_STRIDE <= controller->length;
}


static uint32
read_level(gpio_controller* controller, uint32 pin)
{
	return *pad_register(controller, pin, PAD_VAL) & PAD_VAL_LEVEL;
}


static void
write_level(gpio_controller* controller, uint32 pin, uint32 level)
{
	cpu_status state = disable_interrupts();
	acquire_spinlock(&controller->lock);

	volatile uint32* val = pad_register(controller, pin, PAD_VAL);
	uint32 value = *val;
	bool enabledOutput = (value & PAD_VAL_OUTPUT_DISABLE) != 0;
	value &= ~(PAD_VAL_LEVEL | PAD_VAL_OUTPUT_DISABLE);
	if (level != 0)
		value |= PAD_VAL_LEVEL;
	*val = value;

	release_spinlock(&controller->lock);
	restore_interrupts(state);

	if (enabledOutput) {
		dprintf("baytrail_gpio: %s pin %" B_PRIu32 ": output enabled\n",
			controller->community->name, pin);
	}
}


static void
dump_pins(gpio_controller* controller)
{
	const gpio_community* community = controller->community;
	dprintf("baytrail_gpio: %s pins (pin: conf0 val)\n", community->name);

	char line[160];
	size_t length = 0;
	for (uint32 pin = 0; pin < community->pinCount; pin++) {
		if (!pad_in_range(controller, pin))
			continue;

		length += snprintf(line + length, sizeof(line) - length,
			" %3" B_PRIu32 ": %08" B_PRIx32 " %" B_PRIx32, pin,
			*pad_register(controller, pin, PAD_CONF0),
			*pad_register(controller, pin, PAD_VAL) & 0x7);
		if ((pin % 4) == 3 || pin + 1 == community->pinCount) {
			dprintf("baytrail_gpio:%s\n", line);
			length = 0;
		}
	}
}


//	#pragma mark - ACPI GeneralPurposeIo operation region


/*!	Returns the pin numbers of a GpioIo or GpioInt descriptor, as found in the
	AML byte stream of a Connection() (ACPI 6.x, section 6.4.3.8.1).
*/
static bool
parse_gpio_connection(const uint8* descriptor, uint32 length,
	const uint8** _pins, uint32* _pinCount)
{
	// large resource: tag, 16-bit length, then the fields
	if (descriptor == NULL || length < 23 || descriptor[0] != 0x8c)
		return false;

	uint16 pinTableOffset;
	uint16 resourceSourceOffset;
	memcpy(&pinTableOffset, descriptor + 14, sizeof(uint16));
	memcpy(&resourceSourceOffset, descriptor + 17, sizeof(uint16));
	pinTableOffset = B_LENDIAN_TO_HOST_INT16(pinTableOffset);
	resourceSourceOffset = B_LENDIAN_TO_HOST_INT16(resourceSourceOffset);

	if (pinTableOffset >= resourceSourceOffset || resourceSourceOffset > length)
		return false;

	*_pins = descriptor + pinTableOffset;
	*_pinCount = (resourceSourceOffset - pinTableOffset) / 2;
	return true;
}


static acpi_status
gpio_region_handler(uint32 function, acpi_physical_address address,
	uint32 bitWidth, int* _value, void* handlerContext, void* regionContext)
{
	gpio_controller* controller = (gpio_controller*)handlerContext;
	uint64* value = (uint64*)_value;
		// ACPICA passes a pointer to a 64-bit value

	const uint8* pins;
	uint32 pinCount;
	if (!parse_gpio_connection(controller->connection.Connection,
			controller->connection.Length, &pins, &pinCount)) {
		ERROR("%s: unsupported connection descriptor\n",
			controller->community->name);
		return AE_BAD_PARAMETER;
	}

	// For GeneralPurposeIo, the address is the index into the pin table of
	// the connection, and the width is the number of pins of the field.
	if (address + bitWidth > pinCount || bitWidth > 64)
		return AE_BAD_PARAMETER;

	bool write = (function & ACPI_IO_MASK) == ACPI_WRITE;
	if (!write)
		*value = 0;

	for (uint32 i = 0; i < bitWidth; i++) {
		uint16 pin;
		memcpy(&pin, pins + (address + i) * 2, sizeof(uint16));
		pin = B_LENDIAN_TO_HOST_INT16(pin);
		if (!pad_in_range(controller, pin)) {
			ERROR("%s: pin %" B_PRIu16 " out of range\n",
				controller->community->name, pin);
			return AE_BAD_PARAMETER;
		}

		if (write) {
			uint32 level = (*value >> i) & 1;
			TRACE("%s: write pin %" B_PRIu16 " = %" B_PRIu32 "\n",
				controller->community->name, pin, level);
			write_level(controller, pin, level);
		} else {
			if (read_level(controller, pin) != 0)
				*value |= 1ULL << i;
			TRACE("%s: read pin %" B_PRIu16 " = %d\n",
				controller->community->name, pin, (int)((*value >> i) & 1));
		}
	}

	return AE_OK;
}


static acpi_status
gpio_region_setup(acpi_handle region, uint32 function, void* handlerContext,
	void** regionContext)
{
	if (function == ACPI_REGION_DEACTIVATE)
		*regionContext = NULL;
	else
		*regionContext = handlerContext;

	return AE_OK;
}


//	#pragma mark - driver module API


static acpi_status
parse_controller_resources(ACPI_RESOURCE* resource, void* context)
{
	gpio_controller* controller = (gpio_controller*)context;

	switch (resource->Type) {
		case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
			controller->physicalBase = resource->Data.FixedMemory32.Address;
			controller->length = resource->Data.FixedMemory32.AddressLength;
			break;
		case ACPI_RESOURCE_TYPE_MEMORY32:
			controller->physicalBase = resource->Data.Memory32.Minimum;
			controller->length = resource->Data.Memory32.AddressLength;
			break;
		case ACPI_RESOURCE_TYPE_IRQ:
			controller->irq = resource->Data.Irq.Interrupts[0];
			break;
		case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
			controller->irq = resource->Data.ExtendedIrq.Interrupts[0];
			break;
	}

	return AE_OK;
}


static float
baytrail_gpio_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "acpi") != 0) {
		return 0.0;
	}

	uint32 deviceType;
	if (sDeviceManager->get_attr_uint32(parent, ACPI_DEVICE_TYPE_ITEM,
			&deviceType, false) != B_OK || deviceType != ACPI_TYPE_DEVICE) {
		return 0.0;
	}

	const char* hid;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK || strcmp(hid, ACPI_NAME_BAYTRAIL_GPIO) != 0) {
		return 0.0;
	}

	return 0.6;
}


static status_t
baytrail_gpio_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Intel Bay Trail GPIO controller" }},
		// The ACPI interpreter may access the pins at any time.
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, BAYTRAIL_GPIO_DRIVER_NAME,
		attrs, NULL, NULL);
}


static status_t
baytrail_gpio_init_driver(device_node* node, void** _driverCookie)
{
	gpio_controller* controller
		= (gpio_controller*)calloc(1, sizeof(gpio_controller));
	if (controller == NULL)
		return B_NO_MEMORY;

	controller->node = node;
	controller->area = -1;
	B_INITIALIZE_SPINLOCK(&controller->lock);

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&controller->acpi,
		(void**)&controller->acpiCookie);

	const char* uid = NULL;
	sDeviceManager->get_attr_string(parent, ACPI_DEVICE_UID_ITEM, &uid, false);
	for (uint32 i = 0; uid != NULL && i < B_COUNT_OF(kCommunities); i++) {
		if (strcmp(uid, kCommunities[i].uid) == 0)
			controller->community = &kCommunities[i];
	}
	sDeviceManager->put_node(parent);

	if (controller->community == NULL) {
		ERROR("unknown _UID %s\n", uid != NULL ? uid : "(none)");
		free(controller);
		return B_ERROR;
	}

	status_t status = controller->acpi->walk_resources(controller->acpiCookie,
		(char*)"_CRS", parse_controller_resources, controller);
	if (status != B_OK || controller->physicalBase == 0
		|| controller->length == 0) {
		ERROR("%s: no memory resource\n", controller->community->name);
		free(controller);
		return B_ERROR;
	}

	controller->area = map_physical_memory("Bay Trail GPIO registers",
		controller->physicalBase, controller->length, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&controller->registers);
	if (controller->area < 0) {
		ERROR("%s: mapping the registers failed\n",
			controller->community->name);
		status = controller->area;
		free(controller);
		return status;
	}

	dprintf("baytrail_gpio: %s at %#" B_PRIxPHYSADDR ", %" B_PRIu32
		" pins, irq %" B_PRIu32 "\n", controller->community->name,
		controller->physicalBase, controller->community->pinCount,
		controller->irq);
#ifdef TRACE_BAYTRAIL_GPIO
	dump_pins(controller);
#else
	(void)dump_pins;
#endif

	// Installing the handler runs the _REG methods of the regions, which
	// tells the AML code that the GPIO fields are available.
	status = controller->acpi->install_address_space_handler(
		controller->acpiCookie, ACPI_ADR_SPACE_GPIO, gpio_region_handler,
		gpio_region_setup, controller);
	if (status == B_OK)
		controller->regionHandlerInstalled = true;
	else {
		ERROR("%s: installing the GPIO region handler failed\n",
			controller->community->name);
	}

	*_driverCookie = controller;
	return B_OK;
}


static void
baytrail_gpio_uninit_driver(void* driverCookie)
{
	gpio_controller* controller = (gpio_controller*)driverCookie;

	if (controller->regionHandlerInstalled) {
		controller->acpi->remove_address_space_handler(controller->acpiCookie,
			ACPI_ADR_SPACE_GPIO, gpio_region_handler);
	}
	if (controller->area >= 0)
		delete_area(controller->area);

	free(controller);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


static driver_module_info sBaytrailGPIODriverModule = {
	{
		BAYTRAIL_GPIO_DRIVER_NAME,
		0,
		NULL
	},

	baytrail_gpio_supports_device,
	baytrail_gpio_register_device,
	baytrail_gpio_init_driver,
	baytrail_gpio_uninit_driver,
	NULL,	// register child devices
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sBaytrailGPIODriverModule,
	NULL
};
