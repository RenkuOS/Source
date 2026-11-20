/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * True random number generator of the StarFive JH7110 ("starfive,jh7110-trng").
 * Once a second it generates 256 random bits and feeds them to the kernel's
 * random pool, as the AMD CCP driver does.
 *
 * Register layout and the init sequence (auto reseed off, 256 bit mode, a
 * reseed, then one "generate" command per 256 bits, completion polled in
 * ISTAT) from the JH7110 TRM, checked against U-Boot's
 * drivers/rng/jh7110_rng.c.
 */


#include <new>
#include <stdio.h>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>
#include <dpc.h>
#include <fdt_clock_reset.h>

#include "random.h"


#define TRNG_DRIVER_MODULE_NAME "busses/random/jh7110_trng/driver_v1"

#define INFO(x...)	dprintf("jh7110_trng: " x)
#define ERROR(x...)	dprintf("jh7110_trng: " x)


// registers
#define TRNG_CTRL				0x00
#define TRNG_STAT				0x04
#define TRNG_MODE				0x08
#define TRNG_IE					0x10
#define TRNG_ISTAT				0x14
#define TRNG_RAND(i)			(0x20 + 4 * (i))
#define TRNG_AUTO_RQSTS			0x60
#define TRNG_AUTO_AGE			0x64
#define TRNG_REGS_SIZE			0x1000

#define TRNG_CTRL_GENERATE		0x1
#define TRNG_CTRL_RESEED		0x2
#define TRNG_STAT_SEEDED		(1u << 9)
#define TRNG_MODE_R256			(1u << 3)
#define TRNG_IE_RAND_RDY		(1u << 0)
#define TRNG_IE_SEED_DONE		(1u << 1)
#define TRNG_IE_LFSR_LOCKUP		(1u << 4)
#define TRNG_IE_GLOBAL			(1u << 31)
#define TRNG_ISTAT_RAND_RDY		(1u << 0)
#define TRNG_ISTAT_SEED_DONE	(1u << 1)
#define TRNG_ISTAT_LFSR_LOCKUP	(1u << 4)

#define TRNG_WORDS				8		// 256 bits per command


static const char* const kCompatible = "starfive,jh7110-trng";


struct trng_info {
	device_node*		node;
	area_id				registersArea;
	volatile uint32*	registers;

	timer				generateTimer;
	void*				dpcHandle;
	bool				timerAdded;

	uint32				failures;
	bool				disabled;
};


static device_manager_info* sDeviceManager;
static random_for_controller_interface* sRandom;
static dpc_module_info* sDPC;


static inline uint32
trng_read(trng_info* info, uint32 offset)
{
	return info->registers[offset / 4];
}


static inline void
trng_write(trng_info* info, uint32 offset, uint32 value)
{
	info->registers[offset / 4] = value;
}


/*!	Issues \a command and waits for its completion flag in ISTAT, at most
	\a timeout microseconds.
*/
static status_t
trng_command(trng_info* info, uint32 command, uint32 flag, bigtime_t timeout)
{
	trng_write(info, TRNG_CTRL, command);
	for (bigtime_t waited = 0; waited <= timeout; waited += 10) {
		uint32 status = trng_read(info, TRNG_ISTAT);
		if ((status & TRNG_ISTAT_LFSR_LOCKUP) != 0) {
			trng_write(info, TRNG_ISTAT, TRNG_ISTAT_LFSR_LOCKUP);
			return B_IO_ERROR;
		}
		if ((status & flag) != 0) {
			trng_write(info, TRNG_ISTAT, flag);
			return B_OK;
		}
		spin(10);
	}
	return B_TIMED_OUT;
}


static void
trng_dpc(void* arg)
{
	trng_info* info = (trng_info*)arg;
	if (info->disabled)
		return;

	status_t status = trng_command(info, TRNG_CTRL_GENERATE,
		TRNG_ISTAT_RAND_RDY, 10000);
	if (status != B_OK) {
		if (++info->failures == 3) {
			ERROR("generation keeps failing (%s), stopping\n",
				strerror(status));
			info->disabled = true;
		}
		return;
	}
	info->failures = 0;

	uint32 words[TRNG_WORDS];
	uint32 combined = 0;
	for (uint32 i = 0; i < TRNG_WORDS; i++) {
		words[i] = trng_read(info, TRNG_RAND(i));
		combined |= words[i];
	}
	if (combined == 0)
		return;

	for (uint32 i = 0; i < TRNG_WORDS; i += 2)
		sRandom->queue_randomness((uint64)words[i] | ((uint64)words[i + 1] << 32));
}


static int32
trng_timer_hook(timer* timer)
{
	trng_info* info = (trng_info*)timer->user_data;
	sDPC->queue_dpc(info->dpcHandle, trng_dpc, info);
	return B_HANDLED_INTERRUPT;
}


// #pragma mark - driver


static fdt_device_module_info*
get_fdt(device_node* node, fdt_device** _dev)
{
	fdt_device_module_info* module;
	if (sDeviceManager->get_driver(node, (driver_module_info**)&module,
			(void**)_dev) != B_OK) {
		return NULL;
	}
	return module;
}


static float
trng_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
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
	return 0.8f;
}


static status_t
trng_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "JH7110 random number generator" }},
		{}
	};
	return sDeviceManager->register_node(parent, TRNG_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


/*!	Clocks on and reset released through the clock/reset driver. Fails if
	that is not possible: an unclocked block can stall the bus.
*/
static status_t
trng_enable(device_node* fdtNode)
{
	fdt_clock_reset_module_info* clocks;
	if (get_module(JH7110_CLOCK_RESET_MODULE_NAME, (module_info**)&clocks)
			!= B_OK) {
		ERROR("no clock/reset driver\n");
		return B_NOT_SUPPORTED;
	}

	status_t status = B_OK;
	static const char* const kClocks[] = { "hclk", "ahb" };
	for (size_t i = 0; i < B_COUNT_OF(kClocks) && status == B_OK; i++) {
		fdt_clock clock;
		status = clocks->get_clock(fdtNode, kClocks[i], 0, &clock);
		if (status == B_OK)
			status = clocks->enable_clock(clock, true);
		if (status != B_OK)
			ERROR("clock %s: %s\n", kClocks[i], strerror(status));
	}

	if (status == B_OK) {
		fdt_reset reset;
		status = clocks->get_reset(fdtNode, NULL, 0, &reset);
		if (status == B_OK)
			status = clocks->set_reset(reset, false);
		if (status != B_OK)
			ERROR("reset: %s\n", strerror(status));
	}

	put_module(JH7110_CLOCK_RESET_MODULE_NAME);
	return status;
}


static void
trng_free(trng_info* info)
{
	if (info->timerAdded)
		cancel_timer(&info->generateTimer);
	if (info->dpcHandle != NULL)
		sDPC->delete_dpc_queue(info->dpcHandle);
	if (info->registersArea >= 0)
		delete_area(info->registersArea);
	delete info;
}


static status_t
trng_init_driver(device_node* node, void** _cookie)
{
	trng_info* info = new(std::nothrow) trng_info;
	if (info == NULL)
		return B_NO_MEMORY;
	memset(info, 0, sizeof(*info));
	info->node = node;
	info->registersArea = -1;

	device_node* fdtNode = sDeviceManager->get_parent_node(node);
	fdt_device* dev;
	fdt_device_module_info* module = get_fdt(fdtNode, &dev);
	uint64 regs, regsLength;
	if (module == NULL || !module->get_reg(dev, 0, &regs, &regsLength)) {
		sDeviceManager->put_node(fdtNode);
		trng_free(info);
		return B_BAD_DATA;
	}

	status_t status = trng_enable(fdtNode);
	sDeviceManager->put_node(fdtNode);
	if (status != B_OK) {
		trng_free(info);
		return status;
	}

	info->registersArea = map_physical_memory("jh7110 trng", regs,
		TRNG_REGS_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&info->registers);
	if (info->registersArea < 0) {
		status = info->registersArea;
		trng_free(info);
		return status;
	}

	// no automatic reseeding, 256 bit mode, interrupt flags reported in
	// ISTAT (the interrupt itself is not used)
	trng_write(info, TRNG_AUTO_AGE, 0);
	trng_write(info, TRNG_AUTO_RQSTS, 0);
	trng_write(info, TRNG_ISTAT, trng_read(info, TRNG_ISTAT));
	trng_write(info, TRNG_IE, TRNG_IE_GLOBAL | TRNG_IE_RAND_RDY
		| TRNG_IE_SEED_DONE | TRNG_IE_LFSR_LOCKUP);
	trng_write(info, TRNG_MODE, trng_read(info, TRNG_MODE) | TRNG_MODE_R256);

	status = trng_command(info, TRNG_CTRL_RESEED, TRNG_ISTAT_SEED_DONE,
		100000);
	if (status != B_OK) {
		ERROR("at %#" B_PRIx64 ": reseed failed (%s), status %#" B_PRIx32
			"\n", regs, strerror(status), trng_read(info, TRNG_STAT));
		trng_free(info);
		return status;
	}

	status = sDPC->new_dpc_queue(&info->dpcHandle, "jh7110 trng",
		B_LOW_PRIORITY);
	if (status != B_OK) {
		trng_free(info);
		return status;
	}

	info->generateTimer.user_data = info;
	status = add_timer(&info->generateTimer, &trng_timer_hook, 1000000,
		B_PERIODIC_TIMER);
	if (status != B_OK) {
		trng_free(info);
		return status;
	}
	info->timerAdded = true;

	// some entropy right away
	sDPC->queue_dpc(info->dpcHandle, trng_dpc, info);

	*_cookie = info;
	return B_OK;
}


static void
trng_uninit_driver(void* _cookie)
{
	trng_free((trng_info*)_cookie);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ RANDOM_FOR_CONTROLLER_MODULE_NAME, (module_info**)&sRandom },
	{ B_DPC_MODULE_NAME, (module_info**)&sDPC },
	{}
};


static driver_module_info sTrngDriver = {
	{
		TRNG_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	trng_supports_device,
	trng_register_device,
	trng_init_driver,
	trng_uninit_driver,
	NULL,	// register child devices
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sTrngDriver,
	NULL
};
