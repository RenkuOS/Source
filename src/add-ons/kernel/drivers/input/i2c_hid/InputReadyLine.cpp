/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


#include "InputReadyLine.h"

#include <KernelExport.h>

#include <stdlib.h>
#include <string.h>

extern "C" {
#	include "acpi.h"
}


// Shared by i2c_hid and i2c_elan.
#define TRACE_ALWAYS(x...)	dprintf("input ready: " x)


// Intel PCH GPIO pad configuration: PADCFG0 is the first of two 32-bit
// registers per pad (see Linux pinctrl-intel.c).
#define PADCFG0_GPIORXSTATE		(1U << 1)
#define PADCFG0_GPIORXDIS		(1U << 9)
#define PADCFG0_PMODE_MASK		(0xfU << 10)

// AMD FCH GPIO: one 32-bit register per pin (see Linux pinctrl-amd.h).
#define AMD_PIN_STS				(1U << 16)
#define AMD_OUTPUT_ENABLE		(1U << 23)

#define MAX_COMMUNITIES			4


struct pad_community {
	uint16	first;
	uint16	last;
};

// Sunrise Point-LP (also used by Kaby Lake-LP). Community n uses memory
// resource n of the controller. ACPI GPIO numbers are pad numbers.
static const pad_community kSunrisePointLPCommunities[] = {
	{ 0, 47 },
	{ 48, 119 },
	{ 120, 151 },
};

// AMD controllers have a single bank of pins, numbered from 0.
static const pad_community kAMDCommunities[] = {
	{ 0, 255 },
};

enum gpio_controller_type {
	GPIO_INTEL,
	GPIO_AMD,
};

static const struct {
	const char*				hid;
	gpio_controller_type	type;
	const pad_community*	communities;
	uint32					communityCount;
	uint32					padConfigOffset;
	uint32					padConfigStride;
} kGPIOControllers[] = {
	{ "INT344B", GPIO_INTEL, kSunrisePointLPCommunities,
		B_COUNT_OF(kSunrisePointLPCommunities), 0x400, 8 },
	{ "AMD0030", GPIO_AMD, kAMDCommunities,
		B_COUNT_OF(kAMDCommunities), 0, 4 },
	{ "AMDI0030", GPIO_AMD, kAMDCommunities,
		B_COUNT_OF(kAMDCommunities), 0, 4 },
};


struct gpio_interrupt {
	bool	found;
	char	controller[128];
	uint16	pin;
	uint8	triggering;
	uint8	polarity;
};

struct controller_memory {
	uint32	count;
	uint32	base[MAX_COMMUNITIES];
	uint32	length[MAX_COMMUNITIES];
};


static acpi_status
find_gpio_interrupt(ACPI_RESOURCE* resource, void* context)
{
	gpio_interrupt* interrupt = (gpio_interrupt*)context;
	if (interrupt->found || resource->Type != ACPI_RESOURCE_TYPE_GPIO)
		return AE_OK;

	ACPI_RESOURCE_GPIO& gpio = resource->Data.Gpio;
	if (gpio.ConnectionType != ACPI_RESOURCE_GPIO_TYPE_INT
		|| gpio.PinTableLength == 0 || gpio.ResourceSource.StringPtr == NULL)
		return AE_OK;

	strlcpy(interrupt->controller, gpio.ResourceSource.StringPtr,
		sizeof(interrupt->controller));
	interrupt->pin = gpio.PinTable[0];
	interrupt->triggering = gpio.Triggering;
	interrupt->polarity = gpio.Polarity;
	interrupt->found = true;
	return AE_OK;
}


static acpi_status
find_controller_memory(ACPI_RESOURCE* resource, void* context)
{
	controller_memory* memory = (controller_memory*)context;
	if (memory->count >= MAX_COMMUNITIES)
		return AE_OK;

	if (resource->Type == ACPI_RESOURCE_TYPE_FIXED_MEMORY32) {
		memory->base[memory->count] = resource->Data.FixedMemory32.Address;
		memory->length[memory->count]
			= resource->Data.FixedMemory32.AddressLength;
	} else if (resource->Type == ACPI_RESOURCE_TYPE_MEMORY32) {
		memory->base[memory->count] = resource->Data.Memory32.Minimum;
		memory->length[memory->count] = resource->Data.Memory32.AddressLength;
	} else
		return AE_OK;
	memory->count++;
	return AE_OK;
}


InputReadyLine::InputReadyLine()
	:
	fArea(-1),
	fPadConfig(NULL),
	fStateMask(PADCFG0_GPIORXSTATE),
	fActiveLow(true)
{
}


InputReadyLine::~InputReadyLine()
{
	if (fArea >= 0)
		delete_area(fArea);
}


status_t
InputReadyLine::Init(acpi_module_info* acpi, acpi_handle device)
{
	gpio_interrupt interrupt = {};
	if (acpi->walk_resources(device, (char*)"_CRS", find_gpio_interrupt,
			&interrupt) != B_OK || !interrupt.found) {
		return B_ENTRY_NOT_FOUND;
	}

	// The line must stay asserted while a report is ready.
	if (interrupt.triggering != ACPI_LEVEL_SENSITIVE
		|| (interrupt.polarity != ACPI_ACTIVE_LOW
			&& interrupt.polarity != ACPI_ACTIVE_HIGH)) {
		TRACE_ALWAYS("%s pin %u is not level triggered\n",
			interrupt.controller, interrupt.pin);
		return B_NOT_SUPPORTED;
	}

	char* hid = NULL;
	if (acpi->get_device_info(interrupt.controller, &hid, NULL, 0, NULL,
			NULL) != B_OK || hid == NULL) {
		return B_ENTRY_NOT_FOUND;
	}

	int32 type = -1;
	for (uint32 i = 0; i < B_COUNT_OF(kGPIOControllers); i++) {
		if (strcmp(hid, kGPIOControllers[i].hid) == 0) {
			type = i;
			break;
		}
	}
	if (type < 0) {
		TRACE_ALWAYS("GPIO controller %s (%s) not supported\n",
			interrupt.controller, hid);
		free(hid);
		return B_NOT_SUPPORTED;
	}
	free(hid);

	acpi_handle controller;
	if (acpi->get_handle(NULL, interrupt.controller, &controller) != B_OK)
		return B_ENTRY_NOT_FOUND;

	controller_memory memory = {};
	if (acpi->walk_resources(controller, (char*)"_CRS",
			find_controller_memory, &memory) != B_OK) {
		return B_ENTRY_NOT_FOUND;
	}

	const pad_community* communities = kGPIOControllers[type].communities;
	int32 community = -1;
	for (uint32 i = 0; i < kGPIOControllers[type].communityCount; i++) {
		if (interrupt.pin >= communities[i].first
			&& interrupt.pin <= communities[i].last) {
			community = i;
			break;
		}
	}
	if (community < 0 || (uint32)community >= memory.count)
		return B_BAD_VALUE;

	uint32 offset = kGPIOControllers[type].padConfigOffset
		+ (interrupt.pin - communities[community].first)
			* kGPIOControllers[type].padConfigStride;
	if (memory.base[community] == 0
		|| offset + sizeof(uint32) > memory.length[community]) {
		return B_BAD_VALUE;
	}

	phys_addr_t address = (phys_addr_t)memory.base[community] + offset;
	phys_addr_t pageAddress = address & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	void* registers;
	fArea = map_physical_memory("i2c_hid input ready", pageAddress,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, &registers);
	if (fArea < 0)
		return fArea;

	fPadConfig = (volatile uint32*)((addr_t)registers
		+ (address - pageAddress));

	// Use the pad only if it is a GPIO with its input enabled.
	uint32 padConfig = *fPadConfig;
	bool usable;
	if (kGPIOControllers[type].type == GPIO_AMD) {
		usable = (padConfig & AMD_OUTPUT_ENABLE) == 0;
		fStateMask = AMD_PIN_STS;
	} else {
		usable = (padConfig & PADCFG0_PMODE_MASK) == 0
			&& (padConfig & PADCFG0_GPIORXDIS) == 0;
		fStateMask = PADCFG0_GPIORXSTATE;
	}
	if (!usable) {
		TRACE_ALWAYS("pad %u is not a GPIO input (0x%08" B_PRIx32 ")\n",
			interrupt.pin, padConfig);
		delete_area(fArea);
		fArea = -1;
		fPadConfig = NULL;
		return B_NOT_SUPPORTED;
	}

	fActiveLow = interrupt.polarity == ACPI_ACTIVE_LOW;
	TRACE_ALWAYS("using %s pin %u\n", interrupt.controller,
		interrupt.pin);
	return B_OK;
}


bool
InputReadyLine::IsAsserted() const
{
	bool high = (*fPadConfig & fStateMask) != 0;
	return high != fActiveLow;
}
