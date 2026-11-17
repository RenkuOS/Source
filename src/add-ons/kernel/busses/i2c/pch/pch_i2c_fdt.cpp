/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * Device tree front-end of the DesignWare I2C controller driver
 * ("snps,designware-i2c"), as on the StarFive JH7110. The controller is the
 * one of the Intel PCH I2C ports, so the bus side is shared with the ACPI and
 * PCI front-ends. The children of the device tree node are registered as I2C
 * devices, with their first "compatible" string as hardware ID.
 *
 * The SCL counts follow the DesignWare databook equations, as U-Boot's
 * dw_i2c_calc_timing() does:
 *   SCL high = (HCNT + SPKLEN + 3) / clock + fall time
 *   SCL low  = (LCNT + 1) / clock - fall time + rise time
 */


#include <new>
#include <stdio.h>
#include <string.h>

#include <ACPI.h>
#include <ByteOrder.h>

#include "pch_i2c.h"

#include <bus/FDT.h>
#include <fdt_clock_reset.h>


typedef struct {
	pch_i2c_sim_info info;
	device_node* fdtNode;
} pch_i2c_fdt_sim_info;


static const char* const kCompatible = "snps,designware-i2c";

// minimum SCL high and low times (I2C specification), in ns
#define SS_MIN_HIGH_NS		4000
#define SS_MIN_LOW_NS		4700
#define FS_MIN_HIGH_NS		600
#define FS_MIN_LOW_NS		1300
#define HD_STA_OFFSET		3
#define SPIKE_COUNT			1		// IC_*_SPKLEN after reset


static fdt_device_module_info*
get_fdt(device_node* node, fdt_device** _dev)
{
	fdt_device_module_info* module;
	if (gDeviceManager->get_driver(node, (driver_module_info**)&module,
			(void**)_dev) != B_OK) {
		return NULL;
	}
	return module;
}


static uint32
get_prop_u32(fdt_device_module_info* module, fdt_device* dev,
	const char* name, uint32 defaultValue)
{
	int length;
	const uint32* prop = (const uint32*)module->get_prop(dev, name, &length);
	if (prop == NULL || length < 4)
		return defaultValue;
	return B_BENDIAN_TO_HOST_INT32(*prop);
}


static uint32
ns_to_counts(uint64 clock, uint32 ns)
{
	return (uint32)((clock * ns + 999999999) / 1000000000);
}


/*!	HCNT and LCNT for a bus speed, back-solved from the minimum SCL times and
	then stretched evenly to reach the requested period.
*/
static void
compute_scl_counts(uint64 clock, uint32 speed, uint32 minHighNs,
	uint32 minLowNs, uint32 riseNs, uint32 fallNs, uint16* _hcnt,
	uint16* _lcnt)
{
	int32 fall = ns_to_counts(clock, fallNs);
	int32 rise = ns_to_counts(clock, riseNs);
	int32 hcnt = (int32)ns_to_counts(clock, minHighNs) - fall - HD_STA_OFFSET
		- SPIKE_COUNT;
	int32 lcnt = (int32)ns_to_counts(clock, minLowNs) - rise + fall - 1;
	if (hcnt < 6)
		hcnt = 6;
	if (lcnt < 8)
		lcnt = 8;

	// the period these counts give, against the one asked for
	int32 period = (int32)(clock / speed);
	int32 total = hcnt + SPIKE_COUNT + HD_STA_OFFSET + lcnt + 1 + rise;
	if (period > total) {
		int32 extra = period - total;
		hcnt += extra / 2;
		lcnt += extra - extra / 2;
	}
	*_hcnt = (uint16)min_c(hcnt, 0xffff);
	*_lcnt = (uint16)min_c(lcnt, 0xffff);
}


/*!	Clock on and reset released. Fails if the clock is unknown: the counts
	could not be computed and an unclocked block can stall the bus.
*/
static status_t
enable_controller(device_node* fdtNode, uint64* _clockRate)
{
	fdt_clock_reset_module_info* clocks;
	if (get_module(JH7110_CLOCK_RESET_MODULE_NAME, (module_info**)&clocks)
			!= B_OK) {
		ERROR("no clock/reset driver\n");
		return B_NOT_SUPPORTED;
	}

	fdt_clock clock;
	status_t status = clocks->get_clock(fdtNode, NULL, 0, &clock);
	if (status == B_OK)
		status = clocks->enable_clock(clock, true);
	int64 rate = status == B_OK ? clocks->get_clock_rate(clock) : -1;
	if (status == B_OK && rate <= 0)
		status = B_BAD_VALUE;

	if (status == B_OK) {
		fdt_reset reset;
		if (clocks->get_reset(fdtNode, NULL, 0, &reset) == B_OK)
			clocks->set_reset(reset, false);
		*_clockRate = rate;
	}

	put_module(JH7110_CLOCK_RESET_MODULE_NAME);
	return status;
}


/*!	Registers the children of the device tree node: "reg" is the slave
	address, the first "compatible" string the hardware ID.
*/
static status_t
fdt_scan_bus(i2c_bus_cookie cookie)
{
	CALLED();
	pch_i2c_fdt_sim_info* bus = (pch_i2c_fdt_sim_info*)cookie;

	device_attr attrs[] = {
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "fdt" }},
		{}
	};
	device_node* child = NULL;
	while (gDeviceManager->get_next_child_node(bus->fdtNode, attrs, &child)
			== B_OK) {
		fdt_device* dev;
		fdt_device_module_info* module = get_fdt(child, &dev);
		if (module == NULL)
			continue;

		int length;
		const char* status = (const char*)module->get_prop(dev, "status",
			&length);
		if (status != NULL && strcmp(status, "okay") != 0
			&& strcmp(status, "ok") != 0) {
			continue;
		}

		const char* compatible = (const char*)module->get_prop(dev,
			"compatible", &length);
		uint32 address = get_prop_u32(module, dev, "reg", UINT32_MAX);
		if (compatible == NULL || address > 0x3ff)
			continue;

		char* cidList[1] = { NULL };
		status_t result = gI2c->register_device(bus->info.sim,
			(i2c_addr)address, (char*)compatible, cidList, NULL);
		TRACE_ALWAYS("device %s at %#" B_PRIx32 ": %s\n", compatible,
			address, strerror(result));
	}
	return B_OK;
}


static status_t
register_child_devices(void* cookie)
{
	CALLED();
	pch_i2c_fdt_sim_info* bus = (pch_i2c_fdt_sim_info*)cookie;

	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "DesignWare I2C Controller" }},
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{ .string = I2C_FOR_CONTROLLER_MODULE_NAME }},
		{ NULL }
	};

	return gDeviceManager->register_node(bus->info.driver_node,
		PCH_I2C_SIM_MODULE_NAME, attrs, NULL, NULL);
}


static status_t
init_device(device_node* node, void** device_cookie)
{
	CALLED();

	pch_i2c_fdt_sim_info* bus = (pch_i2c_fdt_sim_info*)calloc(1,
		sizeof(pch_i2c_fdt_sim_info));
	if (bus == NULL)
		return B_NO_MEMORY;

	device_node* fdtNode = gDeviceManager->get_parent_node(node);
	fdt_device* dev;
	fdt_device_module_info* module = get_fdt(fdtNode, &dev);
	uint64 regs, regsLength, irq;
	if (module == NULL || !module->get_reg(dev, 0, &regs, &regsLength)
		|| !module->get_interrupt(dev, 0, NULL, &irq)) {
		gDeviceManager->put_node(fdtNode);
		free(bus);
		return B_BAD_DATA;
	}

	uint64 clock;
	status_t status = enable_controller(fdtNode, &clock);
	if (status != B_OK) {
		ERROR("bus at %#" B_PRIx64 ": no usable clock (%s), not used\n",
			regs, strerror(status));
		gDeviceManager->put_node(fdtNode);
		free(bus);
		return status;
	}

	uint32 speed = get_prop_u32(module, dev, "clock-frequency", 100000);
	uint32 fallNs = get_prop_u32(module, dev, "i2c-scl-falling-time-ns", 300);
	uint32 holdNs = get_prop_u32(module, dev, "i2c-sda-hold-time-ns", 300);

	// both counts are programmed; the bus manager runs the controller in
	// fast mode, so use the standard mode counts for it when the device
	// tree asks for 100 kHz
	uint16 ssHcnt, ssLcnt, fsHcnt, fsLcnt;
	compute_scl_counts(clock, 100000, SS_MIN_HIGH_NS, SS_MIN_LOW_NS,
		get_prop_u32(module, dev, "i2c-scl-rising-time-ns", 1000), fallNs,
		&ssHcnt, &ssLcnt);
	compute_scl_counts(clock, 400000, FS_MIN_HIGH_NS, FS_MIN_LOW_NS,
		get_prop_u32(module, dev, "i2c-scl-rising-time-ns", 300), fallNs,
		&fsHcnt, &fsLcnt);
	if (speed <= 100000) {
		fsHcnt = ssHcnt;
		fsLcnt = ssLcnt;
	}

	bus->fdtNode = fdtNode;
		// keeps the reference for the child scan, put in uninit_device()
	bus->info.base_addr = regs;
	bus->info.map_size = max_c(regsLength, (uint64)B_PAGE_SIZE);
	bus->info.irq = irq;
	bus->info.version = PCH_NONE;
	bus->info.ss_hcnt = ssHcnt;
	bus->info.ss_lcnt = ssLcnt;
	bus->info.fs_hcnt = fsHcnt;
	bus->info.fs_lcnt = fsLcnt;
	bus->info.sda_hold_time = max_c(ns_to_counts(clock, holdNs), 1);
	bus->info.driver_node = node;
	bus->info.scan_bus = fdt_scan_bus;

	TRACE_ALWAYS("bus at %#" B_PRIx64 ", irq %" B_PRIu64 ": clock %" B_PRIu64
		" Hz, %" B_PRIu32 " Hz bus, hcnt/lcnt %u/%u, sda hold %" B_PRIu32 "\n",
		regs, irq, clock, speed, fsHcnt, fsLcnt, bus->info.sda_hold_time);

	*device_cookie = bus;
	return B_OK;
}


static void
uninit_device(void* device_cookie)
{
	pch_i2c_fdt_sim_info* bus = (pch_i2c_fdt_sim_info*)device_cookie;
	gDeviceManager->put_node(bus->fdtNode);
	free(bus);
}


static status_t
register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "DesignWare I2C (device tree)" }},
		{}
	};

	return gDeviceManager->register_node(parent,
		PCH_I2C_FDT_DEVICE_MODULE_NAME, attrs, NULL, NULL);
}


static float
supports_device(device_node* parent)
{
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	const char* compatible;
	if (gDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK || strcmp(compatible, kCompatible) != 0) {
		return 0.0f;
	}

	fdt_device* dev;
	fdt_device_module_info* module = get_fdt(parent, &dev);
	if (module == NULL)
		return 0.0f;
	int length;
	const char* status = (const char*)module->get_prop(dev, "status", &length);
	if (status != NULL && strcmp(status, "okay") != 0
		&& strcmp(status, "ok") != 0) {
		return 0.0f;
	}
	return 0.6f;
}


//	#pragma mark -


driver_module_info gPchI2cFdtDevice = {
	{
		PCH_I2C_FDT_DEVICE_MODULE_NAME,
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
