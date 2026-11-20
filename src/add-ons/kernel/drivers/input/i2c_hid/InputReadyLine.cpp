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


// Intel PCH GPIO pad configuration: PADCFG0 is the first of the registers
// of each pad (see Linux pinctrl-intel.c).
#define PADCFG0_GPIORXSTATE		(1U << 1)
#define PADCFG0_GPIORXDIS		(1U << 9)
#define PADCFG0_PMODE_MASK		(0xfU << 10)

// AMD FCH GPIO: one 32-bit register per pin (see Linux pinctrl-amd.h).
#define AMD_PIN_STS				(1U << 16)
#define AMD_OUTPUT_ENABLE		(1U << 23)

// Intel PCH GPIO community register whose bits 15:0 hold the offset of the
// first pad's PADCFG0 (600 and 700 Series PCH datasheet vol 2, 710279, p973).
// The datasheet gives its default as 600h but lists the first pad at 700h
// (p967-968), so it is read rather than assumed.
#define INTEL_PADBAR			0x0c
#define INTEL_PADBAR_MASK		0xffff

#define MAX_COMMUNITIES			4


// A range of ACPI GPIO numbers whose pads sit back to back in one community.
// "memory" is the index of the controller's memory resource that holds the
// community, and "firstPad" is the index of the group's first pad within
// that community.
struct pad_group {
	uint16	firstPin;
	uint16	count;
	uint8	memory;
	uint16	firstPad;
};

// Sunrise Point-LP (also used by Kaby Lake-LP). Community n uses memory
// resource n of the controller. ACPI GPIO numbers are pad numbers.
static const pad_group kSunrisePointLPGroups[] = {
	{ 0, 48, 0, 0 },
	{ 48, 72, 1, 0 },
	{ 120, 32, 2, 0 },
};

// INTC1055: the 600 Series on-package PCH (Alder Lake-P). The firmware
// numbers ACPI GPIOs in blocks of 32 per group, so a group's pins can end
// before the next block starts. The memory resources are communities 0, 1,
// 4, and 5, in that order. Groups whose pads have no ACPI number, and the
// two community 1 groups the datasheet does not describe, are left out, but
// their pads still count in firstPad. Taken from the GPCL table and
// the GPI0 _CRS in the DSDT of a Dell XPS 15 9520. The pad offsets match the
// datasheet (710279 vol 2: p968, p1025-1030, p1125-1131; PortIDs in 691222
// vol 1, p225).
static const pad_group kAlderLakePGroups[] = {
	{ 0x000, 26, 0, 0 },	// GPP_B
	{ 0x020, 16, 0, 26 },	// GPP_T
	{ 0x040, 25, 0, 42 },	// GPP_A
	{ 0x060, 8, 1, 0 },		// GPP_S
	{ 0x080, 24, 1, 8 },	// GPP_H
	{ 0x0a0, 21, 1, 32 },	// GPP_D
	{ 0x100, 24, 2, 0 },	// GPP_C
	{ 0x120, 25, 2, 24 },	// GPP_F
	{ 0x140, 25, 2, 55 },	// GPP_E
	{ 0x160, 8, 3, 0 },		// GPP_R
};

// AMD controllers have a single bank of pins, numbered from 0.
static const pad_group kAMDGroups[] = {
	{ 0, 256, 0, 0 },
};

// The pad configuration of a community starts at the offset in its PADBAR
// register rather than at a fixed offset.
#define PAD_CONFIG_FROM_PADBAR	0xffffffff

enum gpio_controller_type {
	GPIO_INTEL,
	GPIO_AMD,
};

static const struct {
	const char*				hid;
	gpio_controller_type	type;
	const pad_group*		groups;
	uint32					groupCount;
	uint32					padConfigOffset;
	uint32					padConfigStride;
} kGPIOControllers[] = {
	{ "INT344B", GPIO_INTEL, kSunrisePointLPGroups,
		B_COUNT_OF(kSunrisePointLPGroups), 0x400, 8 },
	{ "INTC1055", GPIO_INTEL, kAlderLakePGroups,
		B_COUNT_OF(kAlderLakePGroups), PAD_CONFIG_FROM_PADBAR, 16 },
	{ "AMD0030", GPIO_AMD, kAMDGroups, B_COUNT_OF(kAMDGroups), 0, 4 },
	{ "AMDI0030", GPIO_AMD, kAMDGroups, B_COUNT_OF(kAMDGroups), 0, 4 },
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


// Reads one 32-bit register at a physical address through a temporary
// read-only mapping.
static status_t
read_physical_register(phys_addr_t address, uint32* value)
{
	phys_addr_t pageAddress = address & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	void* registers;
	area_id area = map_physical_memory("i2c_hid input ready probe",
		pageAddress, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA,
		&registers);
	if (area < 0)
		return area;

	*value = *(volatile uint32*)((addr_t)registers + (address - pageAddress));
	delete_area(area);
	return B_OK;
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

	const pad_group* group = NULL;
	for (uint32 i = 0; i < kGPIOControllers[type].groupCount; i++) {
		const pad_group& candidate = kGPIOControllers[type].groups[i];
		if (interrupt.pin >= candidate.firstPin
			&& interrupt.pin - candidate.firstPin < candidate.count) {
			group = &candidate;
			break;
		}
	}
	if (group == NULL || group->memory >= memory.count
		|| memory.base[group->memory] == 0) {
		TRACE_ALWAYS("%s pin %u has no known pad\n", interrupt.controller,
			interrupt.pin);
		return B_BAD_VALUE;
	}

	phys_addr_t communityBase = memory.base[group->memory];
	uint32 communityLength = memory.length[group->memory];

	uint32 padConfigOffset = kGPIOControllers[type].padConfigOffset;
	if (padConfigOffset == PAD_CONFIG_FROM_PADBAR) {
		if (communityLength < INTEL_PADBAR + sizeof(uint32))
			return B_BAD_VALUE;
		status_t status = read_physical_register(communityBase + INTEL_PADBAR,
			&padConfigOffset);
		if (status != B_OK)
			return status;
		padConfigOffset &= INTEL_PADBAR_MASK;
	}

	uint32 offset = padConfigOffset
		+ (group->firstPad + interrupt.pin - group->firstPin)
			* kGPIOControllers[type].padConfigStride;
	if (padConfigOffset >= communityLength || (padConfigOffset & 3) != 0
		|| offset + sizeof(uint32) > communityLength) {
		TRACE_ALWAYS("%s pin %u: pad offset 0x%" B_PRIx32 " is outside the "
			"community\n", interrupt.controller, interrupt.pin, offset);
		return B_BAD_VALUE;
	}

	phys_addr_t address = communityBase + offset;
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
	TRACE_ALWAYS("using %s pin %u (pad at 0x%" B_PRIxPHYSADDR ", 0x%08"
		B_PRIx32 ")\n", interrupt.controller, interrupt.pin, address,
		padConfig);
	return B_OK;
}


bool
InputReadyLine::IsAsserted() const
{
	bool high = (*fPadConfig & fStateMask) != 0;
	return high != fActiveLow;
}
