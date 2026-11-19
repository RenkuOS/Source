/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * Clocks and resets of the StarFive JH7110 (SYS, AON and STG clock/reset
 * generators), for device tree consumers. See <fdt_clock_reset.h>.
 */


#include <fdt_clock_reset.h>

#include <string.h>
#include <new>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <bus/FDT.h>
#include <util/AutoLock.h>

#include "Jh7110ClockTree.h"
#include "dt-bindings/clock/starfive,jh7110-crg.h"
#include "dt-bindings/reset/starfive,jh7110-crg.h"


//#define TRACE_JH7110_CLOCK
#ifdef TRACE_JH7110_CLOCK
#	define TRACE(x...) dprintf("jh7110_clk: " x)
#else
#	define TRACE(x...)
#endif
#define ERROR(x...) dprintf("jh7110_clk: " x)


struct ControllerDef {
	const char*	compatible;
	phys_addr_t	base;
	size_t		size;
	uint32		resetAssert;	// 0: no resets
	uint32		resetStatus;
	uint32		resetCount;
};

// Indexed by jh7110_controller; the PLLs are in the SYS syscon. The
// addresses are checked against the device tree of each provider.
static const ControllerDef kControllers[JH7110_CONTROLLER_COUNT] = {
	{ NULL, 0, 0, 0, 0, 0 },
	{ "starfive,jh7110-pll", 0x13030000, 0x1000, 0, 0, 0 },
	{ "starfive,jh7110-syscrg", 0x13020000, 0x10000, 0x2f8, 0x308,
		JH7110_SYSRST_END },
	{ "starfive,jh7110-aoncrg", 0x17000000, 0x10000, 0x38, 0x3c,
		JH7110_AONRST_END },
	{ "starfive,jh7110-stgcrg", 0x10230000, 0x10000, 0x74, 0x78,
		JH7110_STGRST_END },
};


class KernelRegisters : public Jh7110RegisterAccess {
public:
	status_t Init()
	{
		for (uint32 i = JH7110_PLL; i < JH7110_CONTROLLER_COUNT; i++) {
			void* address;
			fAreas[i] = map_physical_memory("jh7110 clock/reset",
				kControllers[i].base, kControllers[i].size,
				B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
				&address);
			if (fAreas[i] < 0) {
				status_t status = fAreas[i];
				Uninit();
				return status;
			}
			fBase[i] = (addr_t)address;
		}
		return B_OK;
	}

	void Uninit()
	{
		for (uint32 i = 0; i < JH7110_CONTROLLER_COUNT; i++) {
			if (fAreas[i] > 0)
				delete_area(fAreas[i]);
			fAreas[i] = -1;
			fBase[i] = 0;
		}
	}

	uint32 Read(uint32 controller, uint32 offset) override
	{
		if (controller >= JH7110_CONTROLLER_COUNT || fBase[controller] == 0
			|| offset + 4 > kControllers[controller].size)
			return 0;
		return *(vuint32*)(fBase[controller] + offset);
	}

	void Write(uint32 controller, uint32 offset, uint32 value) override
	{
		if (controller >= JH7110_CONTROLLER_COUNT || fBase[controller] == 0
			|| offset + 4 > kControllers[controller].size)
			return;
		*(vuint32*)(fBase[controller] + offset) = value;
	}

private:
	area_id	fAreas[JH7110_CONTROLLER_COUNT] = { -1, -1, -1, -1, -1 };
	addr_t	fBase[JH7110_CONTROLLER_COUNT] = {};
};


static device_manager_info* sDeviceManager;
// constructed in B_MODULE_INIT, as kernel add-ons do not run static
// constructors
static KernelRegisters* sRegistersPointer;
static Jh7110ClockTree* sTreePointer;
static char sRegistersStorage[sizeof(KernelRegisters)]
	__attribute__((aligned(8)));
static char sTreeStorage[sizeof(Jh7110ClockTree)] __attribute__((aligned(8)));
#define sRegisters (*sRegistersPointer)
#define sTree (*sTreePointer)
static spinlock sLock = B_SPINLOCK_INITIALIZER;


static inline fdt_clock
make_handle(uint32 controller, uint32 id)
{
	return (controller << 16) | id;
}


static inline jh7110_clock_ref
clock_ref(fdt_clock clock)
{
	return { (uint8)(clock >> 16), (uint8)(clock & 0xffff) };
}


static fdt_device_module_info*
get_fdt(device_node* node, fdt_device** _device)
{
	fdt_device_module_info* module;
	if (node == NULL || sDeviceManager->get_driver(node,
			(driver_module_info**)&module, (void**)_device) != B_OK)
		return NULL;
	return module;
}


static bool
has_compatible(fdt_device_module_info* module, fdt_device* device,
	const char* compatible)
{
	int length;
	const char* list = (const char*)module->get_prop(device, "compatible",
		&length);
	if (list == NULL)
		return false;

	for (const char* entry = list; entry < list + length;
			entry += strlen(entry) + 1) {
		if (strcmp(entry, compatible) == 0)
			return true;
	}
	return false;
}


static uint32
get_cells(fdt_device_module_info* module, fdt_device* device,
	const char* name)
{
	int length;
	const uint32* prop = (const uint32*)module->get_prop(device, name,
		&length);
	if (prop == NULL || length != 4)
		return 0;
	return B_BENDIAN_TO_HOST_INT32(*prop);
}


/*!	Looks the entry up in "<kind>s"/"<kind>-names" of the node and returns
	the controller and the id of the provider.
*/
static status_t
lookup(device_node* fdtNode, const char* listName, const char* namesName,
	const char* cellsName, const char* name, uint32 index,
	uint32& _controller, uint32& _id)
{
	fdt_device* device;
	fdt_device_module_info* module = get_fdt(fdtNode, &device);
	if (module == NULL)
		return B_BAD_VALUE;

	int listLength;
	const uint32* list = (const uint32*)module->get_prop(device, listName,
		&listLength);
	if (list == NULL)
		return B_NAME_NOT_FOUND;

	int namesLength = 0;
	const char* names = name != NULL
		? (const char*)module->get_prop(device, namesName, &namesLength)
		: NULL;
	if (name != NULL && names == NULL)
		return B_NAME_NOT_FOUND;

	fdt_bus_module_info* busModule;
	fdt_bus* bus;
	if (sDeviceManager->get_driver(module->get_bus(device),
			(driver_module_info**)&busModule, (void**)&bus) != B_OK)
		return B_ERROR;

	const char* entryName = names;
	int cellCount = listLength / 4;
	for (uint32 entry = 0, cell = 0; (int)cell < cellCount; entry++) {
		uint32 phandle = B_BENDIAN_TO_HOST_INT32(list[cell]);
		fdt_device* provider = NULL;
		fdt_device_module_info* providerModule = get_fdt(
			busModule->node_by_phandle(bus, phandle), &provider);
		if (providerModule == NULL)
			return B_ERROR;

		uint32 cells = get_cells(providerModule, provider, cellsName);
		if ((int)(cell + 1 + cells) > cellCount)
			return B_BAD_DATA;

		bool match;
		if (name != NULL) {
			if (entryName == NULL || entryName >= names + namesLength)
				return B_NAME_NOT_FOUND;
			match = strcmp(entryName, name) == 0;
			entryName += strlen(entryName) + 1;
		} else
			match = entry == index;

		if (match) {
			if (cells != 1)
				return B_UNSUPPORTED;
			for (uint32 i = JH7110_PLL; i < JH7110_CONTROLLER_COUNT; i++) {
				if (!has_compatible(providerModule, provider,
						kControllers[i].compatible))
					continue;

				uint64 regBase, regSize;
				if (i != JH7110_PLL && (!providerModule->get_reg(provider, 0,
						&regBase, &regSize)
					|| regBase != kControllers[i].base)) {
					ERROR("%s is not at the expected address\n",
						kControllers[i].compatible);
					return B_UNSUPPORTED;
				}

				_controller = i;
				_id = B_BENDIAN_TO_HOST_INT32(list[cell + 1]);
				return B_OK;
			}
			return B_UNSUPPORTED;
		}

		cell += 1 + cells;
	}

	return B_NAME_NOT_FOUND;
}


//	#pragma mark - clocks


static status_t
get_clock(device_node* fdtNode, const char* name, uint32 index,
	fdt_clock* _clock)
{
	uint32 controller, id;
	status_t status = lookup(fdtNode, "clocks", "clock-names", "#clock-cells",
		name, index, controller, id);
	if (status != B_OK)
		return status;

	if (!Jh7110ClockTree::IsKnown({ (uint8)controller, (uint8)id })
		|| id > 0xff) {
		ERROR("clock %" B_PRIu32 ":%" B_PRIu32 " (%s) is not supported\n",
			controller, id, name != NULL ? name : "?");
		return B_UNSUPPORTED;
	}

	// The device tree has a JH7110 clock generator: the registers exist.
	static bool sReported;
	if (!sReported) {
		sReported = true;
		InterruptsSpinLocker locker(sLock);
		int64 pll0 = sTree.PllRate(0);
		int64 pll1 = sTree.PllRate(1);
		int64 pll2 = sTree.PllRate(2);
		locker.Unlock();
		dprintf("jh7110_clk: pll0 %" B_PRId64 " Hz, pll1 %" B_PRId64
			" Hz, pll2 %" B_PRId64 " Hz\n", pll0, pll1, pll2);
	}

	*_clock = make_handle(controller, id);
	TRACE("clock %s -> %s\n", name != NULL ? name : "?",
		Jh7110ClockTree::Name(clock_ref(*_clock)));
	return B_OK;
}


static status_t
enable_clock(fdt_clock clock, bool enable)
{
	InterruptsSpinLocker locker(sLock);
	return sTree.SetEnabled(clock_ref(clock), enable);
}


static bool
is_clock_enabled(fdt_clock clock)
{
	InterruptsSpinLocker locker(sLock);
	return sTree.IsEnabled(clock_ref(clock));
}


static int64
get_clock_rate(fdt_clock clock)
{
	InterruptsSpinLocker locker(sLock);
	return sTree.GetRate(clock_ref(clock));
}


static int64
set_clock_rate(fdt_clock clock, int64 rate)
{
	InterruptsSpinLocker locker(sLock);
	int64 result = sTree.SetRate(clock_ref(clock), rate);
	TRACE("set %s to %" B_PRId64 " Hz -> %" B_PRId64 "\n",
		Jh7110ClockTree::Name(clock_ref(clock)), rate, result);
	return result;
}


//	#pragma mark - resets


static status_t
get_reset(device_node* fdtNode, const char* name, uint32 index,
	fdt_reset* _reset)
{
	uint32 controller, id;
	status_t status = lookup(fdtNode, "resets", "reset-names", "#reset-cells",
		name, index, controller, id);
	if (status != B_OK)
		return status;

	if (kControllers[controller].resetCount == 0
		|| id >= kControllers[controller].resetCount)
		return B_UNSUPPORTED;

	*_reset = make_handle(controller, id);
	return B_OK;
}


static bool
is_reset_asserted(fdt_reset reset)
{
	uint32 controller = reset >> 16;
	uint32 id = reset & 0xffff;
	if (controller >= JH7110_CONTROLLER_COUNT
		|| id >= kControllers[controller].resetCount)
		return true;

	InterruptsSpinLocker locker(sLock);
	uint32 value = sRegisters.Read(controller,
		kControllers[controller].resetAssert + id / 32 * 4);
	return (value & (1u << (id % 32))) != 0;
}


static status_t
set_reset(fdt_reset reset, bool assert)
{
	uint32 controller = reset >> 16;
	uint32 id = reset & 0xffff;
	if (controller >= JH7110_CONTROLLER_COUNT
		|| id >= kControllers[controller].resetCount)
		return B_BAD_VALUE;

	const ControllerDef& def = kControllers[controller];
	uint32 mask = 1u << (id % 32);
	uint32 assertOffset = def.resetAssert + id / 32 * 4;
	uint32 statusOffset = def.resetStatus + id / 32 * 4;

	InterruptsSpinLocker locker(sLock);
	uint32 value = sRegisters.Read(controller, assertOffset);
	if (assert)
		value |= mask;
	else
		value &= ~mask;
	sRegisters.Write(controller, assertOffset, value);

	// the status bit is set once the block is out of reset
	uint32 expected = assert ? 0 : mask;
	for (int32 i = 0; i < 1000; i++) {
		if ((sRegisters.Read(controller, statusOffset) & mask) == expected)
			return B_OK;
		spin(1);
	}

	ERROR("reset %" B_PRIu32 ":%" B_PRIu32 " did not %s\n", controller, id,
		assert ? "assert" : "deassert");
	return B_TIMED_OUT;
}


//	#pragma mark - module


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		{
			sRegistersPointer = new(sRegistersStorage) KernelRegisters;
			status_t status = sRegisters.Init();
			if (status != B_OK)
				return status;
			sTreePointer = new(sTreeStorage) Jh7110ClockTree(sRegisters);
				// no register access here: the module may be loaded on a
				// machine without these generators
			return B_OK;
		}

		case B_MODULE_UNINIT:
			sRegisters.Uninit();
			return B_OK;
	}

	return B_ERROR;
}


static fdt_clock_reset_module_info sModuleInfo = {
	{
		JH7110_CLOCK_RESET_MODULE_NAME,
		B_KEEP_LOADED,
			// mapped once, initialized once: drivers get and put it often
		std_ops
	},
	get_clock,
	enable_clock,
	is_clock_enabled,
	get_clock_rate,
	set_clock_rate,
	get_reset,
	set_reset,
	is_reset_asserted,
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


module_info* modules[] = {
	(module_info*)&sModuleInfo,
	NULL
};
