/*
 * Copyright 2023, Haiku, Inc.
 * Copyright 2026, The RenkuOS Authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * PLDA XpressRich PCIe host bridge as found in the StarFive JH7110.
 * Based on the PLDA driver of X547's device_manager2 branch; register usage
 * cross-checked with the U-Boot and Linux StarFive/PLDA drivers.
 *
 * If the firmware left the port off (the factory U-Boot of the Milk-V Mars
 * does so for PCIe0, the VL805 USB 3 controller), the driver brings it up as
 * the U-Boot driver does: STG syscon, PHY tuning, clocks, resets, PERST#,
 * root port setup and bridge windows. A port the firmware enabled is left as
 * it is, apart from the translation tables.
 */


#include "PldaPCIController.h"
#include "Jh7110Pcie.h"

#include <bus/FDT.h>
#include <fdt_clock_reset.h>

#include <AutoDeleterDrivers.h>
#include <util/AutoLock.h>

#include <string.h>
#include <new>


// clock/reset generators, for the direct check if the clock/reset driver is
// not available
#define JH7110_SYSCRG_BASE				0x13020000
#define JH7110_SYSCLK_NOC_BUS_STG_AXI	96
#define JH7110_STGCRG_BASE				0x10230000
#define JH7110_STGCRG_RESET_ASSERT		0x74
#define JH7110_CLK_ENABLE				(1u << 31)

#define PLDA_FUNCTION_DIS				(1u << 15)
#define PLDA_RP_ENABLE					1
#define PLDA_PREF_MEM_WIN_64_SUPPORT	(1u << 3)
#define PLDA_PMSG_LTR_SUPPORT			(1u << 2)
#define PCI_CLASS_BRIDGE_PCI_NORMAL		0x060400


static vuint32*
map_physical_reg(phys_addr_t address, AreaDeleter& area)
{
	phys_addr_t pageBase = address & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	void* virtualBase;
	area.SetTo(map_physical_memory("jh7110 pcie setup", pageBase,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &virtualBase));
	if (area.Get() < 0)
		return NULL;

	return (vuint32*)((addr_t)virtualBase + (address - pageBase));
}


static uint32
read_physical_reg(phys_addr_t address)
{
	AreaDeleter area;
	vuint32* reg = map_physical_reg(address, area);
	return reg != NULL ? *reg : 0;
}


//!	Read-modify-write; returns the value read back.
static uint32
update_physical_reg(phys_addr_t address, uint32 mask, uint32 bits)
{
	AreaDeleter area;
	vuint32* reg = map_physical_reg(address, area);
	if (reg == NULL)
		return 0;

	*reg = jh7110_update_bits(*reg, mask, bits);
	return *reg;
}


//#pragma mark - driver


float
PldaPCIController::SupportsDevice(device_node* parent)
{
	const char* bus;
	status_t status = gDeviceManager->get_attr_string(parent, B_DEVICE_BUS,
		&bus, false);
	if (status < B_OK)
		return -1.0f;

	if (strcmp(bus, "fdt") != 0)
		return 0.0f;

	const char* compatible;
	status = gDeviceManager->get_attr_string(parent, "fdt/compatible",
		&compatible, false);
	if (status < B_OK)
		return -1.0f;

	if (strcmp(compatible, "starfive,jh7110-pcie") != 0)
		return 0.0f;

	return 1.0f;
}


status_t
PldaPCIController::RegisterDevice(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{.string = "PLDA PCIe Host Controller"} },
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{.string = "bus_managers/pci/root/driver_v1"} },
		{}
	};

	return gDeviceManager->register_node(parent, PLDA_PCI_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


status_t
PldaPCIController::InitDriver(device_node* node, PldaPCIController*& outDriver)
{
	ObjectDeleter<PldaPCIController> driver(
		new(std::nothrow) PldaPCIController());
	if (!driver.IsSet())
		return B_NO_MEMORY;

	CHECK_RET(driver->InitDriverInt(node));
	outDriver = driver.Detach();
	return B_OK;
}


void
PldaPCIController::UninitDriver()
{
	delete this;
}


status_t
PldaPCIController::ReadResourceInfo()
{
	DeviceNodePutter<&gDeviceManager> fdtNode(
		gDeviceManager->get_parent_node(fNode));

	fdt_device_module_info* fdtModule;
	fdt_device* fdtDev;
	CHECK_RET(gDeviceManager->get_driver(fdtNode.Get(),
		(driver_module_info**)&fdtModule, (void**)&fdtDev));

	const void* prop;
	int propLen;

	prop = fdtModule->get_prop(fdtDev, "linux,pci-domain", &propLen);
	if (prop != NULL && propLen == 4)
		fDomain = B_BENDIAN_TO_HOST_INT32(*(uint32*)prop);

	prop = fdtModule->get_prop(fdtDev, "ranges", &propLen);
	if (prop == NULL) {
		dprintf("plda: \"ranges\" property not found\n");
		return B_ERROR;
	}

	for (uint32* it = (uint32*)prop;
			(uint8*)it - (uint8*)prop + 7 * 4 <= propLen; it += 7) {
		uint32 type = B_BENDIAN_TO_HOST_INT32(*(it + 0));
		uint64 childAdr = B_BENDIAN_TO_HOST_INT64(*(uint64*)(it + 1));
		uint64 parentAdr = B_BENDIAN_TO_HOST_INT64(*(uint64*)(it + 3));
		uint64 len = B_BENDIAN_TO_HOST_INT64(*(uint64*)(it + 5));

		pci_resource_range range = {};
		range.host_address = parentAdr;
		range.pci_address = childAdr;
		range.size = len;

		if ((type & fdtPciRangePrefechable) != 0)
			range.address_type |= PCI_address_prefetchable;

		switch (type & fdtPciRangeTypeMask) {
			case fdtPciRangeIoPort:
				range.type = B_IO_PORT;
				fResourceRanges.Add(range);
				break;
			case fdtPciRangeMmio32Bit:
				range.type = B_IO_MEMORY;
				range.address_type |= PCI_address_type_32;
				fResourceRanges.Add(range);
				break;
			case fdtPciRangeMmio64Bit:
				range.type = B_IO_MEMORY;
				range.address_type |= PCI_address_type_64;
				fResourceRanges.Add(range);
				break;
		}

		TRACE("range type %#" B_PRIx32 ": pci %#" B_PRIx64 ", host %#"
			B_PRIx64 ", size %#" B_PRIx64 "\n", type, childAdr, parentAdr, len);
	}

	return B_OK;
}


// ports this driver enabled, for a second probe after a failed one
static bool sEnabledByDriver[2];

static const char* const kJh7110Clocks[] = { "noc", "tl", "axi_mst0", "apb" };
static const char* const kJh7110Resets[] = { "mst0", "slv0", "slv", "brg",
	"core", "apb" };


/*!	Touching the bridge registers while its clocks are gated or its resets
	asserted would hang the bus, so look at the clock/reset generator first,
	which is always accessible.
	Returns B_OK if the port is enabled, B_DEV_NOT_READY if it is off.
*/
status_t
PldaPCIController::CheckJH7110Port()
{
	if (fDomain > 1)
		return B_BAD_VALUE;

	// through the clock/reset driver, by the names of the device tree
	fdt_clock_reset_module_info* clockReset;
	if (get_module(JH7110_CLOCK_RESET_MODULE_NAME,
			(module_info**)&clockReset) == B_OK) {
		DeviceNodePutter<&gDeviceManager> fdtNode(
			gDeviceManager->get_parent_node(fNode));

		status_t status = B_OK;
		for (size_t i = 0; i < B_COUNT_OF(kJh7110Clocks); i++) {
			fdt_clock clock;
			if (clockReset->get_clock(fdtNode.Get(), kJh7110Clocks[i], 0,
					&clock) != B_OK) {
				status = B_ERROR;
				break;
			}
			if (!clockReset->is_clock_enabled(clock)) {
				TRACE("PCIe%" B_PRIu32 " clock %s is off\n", fDomain,
					kJh7110Clocks[i]);
				status = B_DEV_NOT_READY;
			}
		}
		for (size_t i = 0; status != B_ERROR && i < B_COUNT_OF(kJh7110Resets);
				i++) {
			fdt_reset reset;
			if (clockReset->get_reset(fdtNode.Get(), kJh7110Resets[i], 0,
					&reset) != B_OK) {
				status = B_ERROR;
				break;
			}
			if (clockReset->is_reset_asserted(reset)) {
				TRACE("PCIe%" B_PRIu32 " reset %s is asserted\n",
					fDomain, kJh7110Resets[i]);
				status = B_DEV_NOT_READY;
			}
		}
		put_module(JH7110_CLOCK_RESET_MODULE_NAME);

		if (status == B_DEV_NOT_READY) {
			dprintf("plda: PCIe%" B_PRIu32 " not enabled by the firmware\n",
				fDomain);
			return status;
		}
		if (status == B_OK)
			TRACE("PCIe%" B_PRIu32 " clocks and resets ok\n", fDomain);
		else {
			dprintf("plda: clock/reset lookup failed, checking the registers "
				"directly\n");
		}
	}

	// The direct register check also runs when the module answered: the two
	// must agree, which also checks the clock and reset IDs used to enable
	// the port.

	uint32 firstClock = fDomain == 0 ? 8 : 11;
	uint32 firstReset = fDomain == 0 ? 11 : 17;

	bool clocksOn = (read_physical_reg(JH7110_SYSCRG_BASE
		+ JH7110_SYSCLK_NOC_BUS_STG_AXI * 4) & JH7110_CLK_ENABLE) != 0;
	for (uint32 i = 0; i < 3; i++) {
		if ((read_physical_reg(JH7110_STGCRG_BASE + (firstClock + i) * 4)
				& JH7110_CLK_ENABLE) == 0)
			clocksOn = false;
	}

	uint32 resetMask = ((1u << 6) - 1) << firstReset;
	uint32 resets = read_physical_reg(JH7110_STGCRG_BASE
		+ JH7110_STGCRG_RESET_ASSERT) & resetMask;

	if (!clocksOn || resets != 0) {
		dprintf("plda: PCIe%" B_PRIu32 " not enabled by the firmware (clocks "
			"%s, resets %#" B_PRIx32 ")\n", fDomain, clocksOn ? "on" : "off",
			resets);
		return B_DEV_NOT_READY;
	}

	TRACE("PCIe%" B_PRIu32 " clock and reset registers ok\n", fDomain);
	return B_OK;
}


uint32
PldaPCIController::StgSysconReg(uint32 offset)
{
	return JH7110_STG_SYSCON_BASE + jh7110_stg_port_base(fDomain) + offset;
}


void
PldaPCIController::SetPerst(bool assert)
{
	// PERST# is active low; set the level before enabling the output
	phys_addr_t base = JH7110_SYS_GPIO_BASE + jh7110_gpio_offset(fPerstGpio);
	uint32 shift = jh7110_gpio_shift(fPerstGpio);
	uint32 dout = update_physical_reg(base + JH7110_SYS_GPIO_DOUT,
		JH7110_GPIO_DOUT_MASK << shift, (assert ? 0u : 1u) << shift);
	uint32 doen = update_physical_reg(base + JH7110_SYS_GPIO_DOEN,
		JH7110_GPIO_DOEN_MASK << shift, JH7110_GPOEN_ENABLE << shift);

	TRACE("PCIe%" B_PRIu32 " PERST# (GPIO %" B_PRIu32 ") %s: dout %#"
		B_PRIx32 ", doen %#" B_PRIx32 "\n", fDomain, fPerstGpio,
		assert ? "asserted" : "released", (dout >> shift) & JH7110_GPIO_DOUT_MASK,
		(doen >> shift) & JH7110_GPIO_DOEN_MASK);
}


/*!	First half of the U-Boot starfive_pcie_probe(): system controller, PHY,
	clocks and resets, PERST# asserted. The bridge registers are accessible
	afterwards.
*/
status_t
PldaPCIController::EnableJH7110Port(fdt_device_module_info* fdtModule,
	fdt_device* fdtDev)
{
	fdt_clock_reset_module_info* clockReset;
	if (get_module(JH7110_CLOCK_RESET_MODULE_NAME,
			(module_info**)&clockReset) != B_OK) {
		dprintf("plda: no clock/reset driver, cannot enable PCIe%" B_PRIu32
			"\n", fDomain);
		return B_DEV_NOT_READY;
	}

	dprintf("plda: PCIe%" B_PRIu32 ": enabling the port\n", fDomain);

	// PERST# line: "perst-gpios = <&sysgpio pin flags>"
	fPerstGpio = fDomain == 0 ? JH7110_PCIE0_PERST_GPIO
		: JH7110_PCIE1_PERST_GPIO;
	int propLen;
	const uint32* perst = (const uint32*)fdtModule->get_prop(fdtDev,
		"perst-gpios", &propLen);
	if (perst != NULL && propLen == 12)
		fPerstGpio = B_BENDIAN_TO_HOST_INT32(perst[1]);
	TRACE("PCIe%" B_PRIu32 " PERST# on GPIO %" B_PRIu32 "%s\n",
		fDomain, fPerstGpio, perst != NULL ? "" : " (default)");
	if (fPerstGpio >= 64) {
		put_module(JH7110_CLOCK_RESET_MODULE_NAME);
		return B_BAD_VALUE;
	}

	// system controller
	TRACE("PCIe%" B_PRIu32 " stg syscon before: ar %#" B_PRIx32
		", aw %#" B_PRIx32 ", rp_nep %#" B_PRIx32 ", lnksta %#" B_PRIx32 "\n",
		fDomain, read_physical_reg(StgSysconReg(JH7110_STG_AR)),
		read_physical_reg(StgSysconReg(JH7110_STG_AW)),
		read_physical_reg(StgSysconReg(JH7110_STG_RP_NEP)),
		read_physical_reg(StgSysconReg(JH7110_STG_LNKSTA)));

	update_physical_reg(StgSysconReg(JH7110_STG_RP_NEP), JH7110_STG_K_RP_NEP,
		JH7110_STG_K_RP_NEP);
	update_physical_reg(StgSysconReg(JH7110_STG_AW),
		JH7110_STG_AW_CKREF_SRC_MASK, 2 << JH7110_STG_AW_CKREF_SRC_SHIFT);
	update_physical_reg(StgSysconReg(JH7110_STG_AW), JH7110_STG_AW_CLKREQ,
		JH7110_STG_AW_CLKREQ);

	TRACE("PCIe%" B_PRIu32 " stg syscon after: aw %#" B_PRIx32
		", rp_nep %#" B_PRIx32 "\n", fDomain,
		read_physical_reg(StgSysconReg(JH7110_STG_AW)),
		read_physical_reg(StgSysconReg(JH7110_STG_RP_NEP)));

	// clocks and resets, in the order of the device tree
	DeviceNodePutter<&gDeviceManager> fdtNode(
		gDeviceManager->get_parent_node(fNode));
	status_t status = B_OK;
	for (size_t i = 0; status == B_OK && i < B_COUNT_OF(kJh7110Clocks); i++) {
		fdt_clock clock;
		status = clockReset->get_clock(fdtNode.Get(), kJh7110Clocks[i], 0,
			&clock);
		if (status == B_OK)
			status = clockReset->enable_clock(clock, true);
		if (status == B_OK && !clockReset->is_clock_enabled(clock))
			status = B_ERROR;
		TRACE("PCIe%" B_PRIu32 " clock %s: %s, %" B_PRId64 " Hz\n",
			fDomain, kJh7110Clocks[i], strerror(status),
			status == B_OK ? clockReset->get_clock_rate(clock) : 0);
	}
	for (size_t i = 0; status == B_OK && i < B_COUNT_OF(kJh7110Resets); i++) {
		fdt_reset reset;
		status = clockReset->get_reset(fdtNode.Get(), kJh7110Resets[i], 0,
			&reset);
		if (status == B_OK)
			status = clockReset->set_reset(reset, false);
		if (status == B_OK && clockReset->is_reset_asserted(reset))
			status = B_ERROR;
		TRACE("PCIe%" B_PRIu32 " reset %s released: %s\n", fDomain,
			kJh7110Resets[i], strerror(status));
	}
	put_module(JH7110_CLOCK_RESET_MODULE_NAME);
	if (status != B_OK)
		return status;
	sEnabledByDriver[fDomain] = true;

	// the direct check must agree before the bridge registers are touched
	status = CheckJH7110Port();
	if (status != B_OK)
		return status;

	// PHY: PLL KVCO fine tuning, as the U-Boot and Linux PHY drivers do
	phys_addr_t phy = fDomain == 0 ? JH7110_PCIE_PHY0_BASE
		: JH7110_PCIE_PHY1_BASE;
	TRACE("PCIe%" B_PRIu32 " PHY before: kvco level %#" B_PRIx32
		", tune %#" B_PRIx32 ", pll ctl %#" B_PRIx32 "\n", fDomain,
		read_physical_reg(phy + JH7110_PHY_KVCO_LEVEL),
		read_physical_reg(phy + JH7110_PHY_KVCO_TUNE_SIGNAL),
		read_physical_reg(phy + JH7110_PHY_PLL_CTL));
	update_physical_reg(phy + JH7110_PHY_KVCO_LEVEL, 0xffffffff,
		JH7110_PHY_KVCO_LEVEL_VALUE);
	update_physical_reg(phy + JH7110_PHY_KVCO_TUNE_SIGNAL, 0xffffffff,
		JH7110_PHY_KVCO_TUNE_VALUE);

	SetPerst(true);
	return B_OK;
}


//!	Second half: root port setup, as in U-Boot starfive_pcie_init_port().
void
PldaPCIController::SetupJH7110RootPort()
{
	// disable the physical functions except #0
	for (uint32 i = 1; i < JH7110_PLDA_FUNC_COUNT; i++) {
		update_physical_reg(StgSysconReg(JH7110_STG_AR),
			JH7110_STG_AR_FUNC_MASK,
			(i << JH7110_PLDA_PHY_FUNC_SHIFT) << JH7110_STG_AR_FUNC_SHIFT);
		update_physical_reg(StgSysconReg(JH7110_STG_AW),
			JH7110_STG_AW_FUNC_MASK, i << JH7110_PLDA_PHY_FUNC_SHIFT);
		WriteReg(kPldaPciMisc, ReadReg(kPldaPciMisc) | PLDA_FUNCTION_DIS);
	}
	update_physical_reg(StgSysconReg(JH7110_STG_AR), JH7110_STG_AR_FUNC_MASK,
		0);
	update_physical_reg(StgSysconReg(JH7110_STG_AW), JH7110_STG_AW_FUNC_MASK,
		0);

	WriteReg(kPldaGenSettings, ReadReg(kPldaGenSettings) | PLDA_RP_ENABLE);
	WriteReg(kPldaPciIds, (ReadReg(kPldaPciIds) & 0xff)
		| (PCI_CLASS_BRIDGE_PCI_NORMAL << 8));
	WriteReg(kPldaPmsgSupportRx,
		ReadReg(kPldaPmsgSupportRx) & ~PLDA_PMSG_LTR_SUPPORT);
	WriteReg(kPldaWinRom, ReadReg(kPldaWinRom) | PLDA_PREF_MEM_WIN_64_SUPPORT);

	TRACE("PCIe%" B_PRIu32 " root port: gen %#" B_PRIx32 ", ids %#"
		B_PRIx32 ", misc %#" B_PRIx32 ", winrom %#" B_PRIx32 ", ar %#" B_PRIx32
		", aw %#" B_PRIx32 "\n", fDomain, ReadReg(kPldaGenSettings),
		ReadReg(kPldaPciIds), ReadReg(kPldaPciMisc), ReadReg(kPldaWinRom),
		read_physical_reg(StgSysconReg(JH7110_STG_AR)),
		read_physical_reg(StgSysconReg(JH7110_STG_AW)));
}


status_t
PldaPCIController::WaitForJH7110Link(bigtime_t timeout)
{
	bigtime_t start = system_time();
	uint32 linkStatus;
	while (true) {
		linkStatus = read_physical_reg(StgSysconReg(JH7110_STG_LNKSTA));
		if ((linkStatus & JH7110_STG_DATA_LINK_ACTIVE) != 0)
			break;
		if (system_time() - start >= timeout) {
			dprintf("plda: PCIe%" B_PRIu32 " link down (status %#" B_PRIx32
				")\n", fDomain, linkStatus);
			return B_DEVICE_NOT_FOUND;
		}
		snooze(10000);
	}

	dprintf("plda: PCIe%" B_PRIu32 " link up after %" B_PRId64 " ms (status %#"
		B_PRIx32 ")\n", fDomain, (system_time() - start) / 1000, linkStatus);
	return B_OK;
}


/*!	Nobody assigned bus numbers and windows to the root port and the PCI bus
	manager does not do it on this architecture: one bus behind the port, the
	whole 32 bit memory range forwarded.
*/
void
PldaPCIController::ConfigureJH7110Bridge()
{
	const pci_resource_range* memory = NULL;
	for (int32 i = 0; i < fResourceRanges.Count(); i++) {
		const pci_resource_range& range = fResourceRanges[i];
		if (range.type == B_IO_MEMORY
			&& (range.address_type & PCI_address_prefetchable) == 0
			&& range.pci_address + range.size <= ((uint64)1 << 32)) {
			memory = &range;
			break;
		}
	}

	vuint32* config = (vuint32*)fConfigBase;
	config[PCI_primary_bus / 4] = (config[PCI_primary_bus / 4] & 0xff000000)
		| (1 << 16) | (1 << 8) | 0;
	fSecondaryBus = 1;

	// I/O and prefetchable windows closed (base above limit)
	config[PCI_io_base / 4] = 0x000000f0;
	config[PCI_prefetchable_memory_base / 4] = 0x0000fff0;
	config[PCI_prefetchable_memory_base_upper32 / 4] = 0;
	config[PCI_prefetchable_memory_limit_upper32 / 4] = 0;

	if (memory != NULL) {
		config[PCI_memory_base / 4]
			= jh7110_bridge_memory_reg(memory->pci_address)
			| (uint32)jh7110_bridge_memory_reg(
				memory->pci_address + memory->size - 1) << 16;
	}

	config[PCI_command / 4] = (config[PCI_command / 4] & 0xffff)
		| PCI_command_memory | PCI_command_master;

	TRACE("PCIe%" B_PRIu32 " bridge: buses %#" B_PRIx32 ", memory %#"
		B_PRIx32 ", command %#" B_PRIx32 "\n", fDomain,
		config[PCI_primary_bus / 4], config[PCI_memory_base / 4],
		config[PCI_command / 4]);
}


#ifdef TRACE_PLDA
void
PldaPCIController::DumpJH7110Bus()
{
	uint32 value;
	ReadConfig(0, 0, 0, PCI_vendor_id, 4, value);
	dprintf("plda: PCIe%" B_PRIu32 " 00:00.0 id %#" B_PRIx32, fDomain, value);
	ReadConfig(0, 0, 0, PCI_primary_bus, 4, value);
	dprintf(", buses %#" B_PRIx32 "\n", value);

	ReadConfig(fSecondaryBus, 0, 0, PCI_vendor_id, 4, value);
	dprintf("plda: PCIe%" B_PRIu32 " %02x:00.0 id %#" B_PRIx32, fDomain,
		fSecondaryBus, value);
	if (value != 0xffffffff) {
		uint32 classCode;
		ReadConfig(fSecondaryBus, 0, 0, PCI_revision, 4, classCode);
		dprintf(", class %#" B_PRIx32 ", BARs", classCode >> 8);
		for (uint16 bar = 0; bar < 6; bar++) {
			ReadConfig(fSecondaryBus, 0, 0, PCI_base_registers + bar * 4, 4,
				value);
			dprintf(" %#" B_PRIx32, value);
		}
	}
	dprintf("\n");
}
#endif


status_t
PldaPCIController::InitDriverInt(device_node* node)
{
	fNode = node;

	DeviceNodePutter<&gDeviceManager> fdtNode(
		gDeviceManager->get_parent_node(node));

	fdt_device_module_info* fdtModule;
	fdt_device* fdtDev;
	CHECK_RET(gDeviceManager->get_driver(fdtNode.Get(),
		(driver_module_info**)&fdtModule, (void**)&fdtDev));

	CHECK_RET(ReadResourceInfo());

	// reg-names = "cfg", "apb"
	if (!fdtModule->get_reg(fdtDev, 0, &fConfigPhysBase, &fConfigSize)
		|| !fdtModule->get_reg(fdtDev, 1, &fRegsPhysBase, &fRegsSize))
		return B_ERROR;

	uint64 irq;
	if (!fdtModule->get_interrupt(fdtDev, 0, NULL, &irq))
		return B_ERROR;

	dprintf("plda: PCIe%" B_PRIu32 ": regs %#" B_PRIx64 ", config %#" B_PRIx64
		" (%#" B_PRIxSIZE "), IRQ %" B_PRIu64 "\n", fDomain, fRegsPhysBase,
		fConfigPhysBase, fConfigSize, irq);

	status_t status = CheckJH7110Port();
	bool coldStart = status == B_DEV_NOT_READY;
	if (coldStart) {
		CHECK_RET(EnableJH7110Port(fdtModule, fdtDev));
	} else if (status != B_OK) {
		return status;
	} else {
		// the port may have been enabled by an earlier probe that did not
		// see the link
		CHECK_RET(WaitForJH7110Link(sEnabledByDriver[fDomain] ? 1000000 : 0));
	}

	fRegsArea.SetTo(map_physical_memory("PCI PLDA regs", fRegsPhysBase,
		fRegsSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fRegsBase));
	CHECK_RET(fRegsArea.Get());

	fConfigArea.SetTo(map_physical_memory("PCI PLDA config", fConfigPhysBase,
		fConfigSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fConfigBase));
	CHECK_RET(fConfigArea.Get());

	if (coldStart)
		SetupJH7110RootPort();
#ifdef TRACE_PLDA
	else
		DumpAtr();
#endif

	// The firmware programmed the translation tables from its own device
	// tree: program them again from ours (identity mappings).
	uint32 atrIndex = 0;
	SetAtrEntry(atrIndex++, fConfigPhysBase, 0, fConfigSize,
		kPldaAtrTrslIdConfig);
	for (int32 i = 0; i < fResourceRanges.Count(); i++) {
		const pci_resource_range& range = fResourceRanges[i];
		if (range.type != B_IO_MEMORY)
			continue;
		if (atrIndex >= kPldaAtrTableCount)
			break;
		SetAtrEntry(atrIndex++, range.host_address, range.pci_address,
			range.size, kPldaAtrTrslIdMemory);
	}

	if (coldStart) {
		// PERST# held for 100 ms, then released; at least 100 ms before the
		// link may be used (PCIe CEM), U-Boot waits 300 ms
		snooze(100000);
		SetPerst(false);
		snooze(300000);
		CHECK_RET(WaitForJH7110Link(1000000));
		snooze(100000);
	}

	// keep the secondary bus number set by the firmware, if any
	uint32 busNumbers = *(vuint32*)(fConfigBase + PCI_primary_bus);
	if (((busNumbers >> 8) & 0xff) != 0)
		fSecondaryBus = (busNumbers >> 8) & 0xff;
	else
		ConfigureJH7110Bridge();

#ifdef TRACE_PLDA
	DumpJH7110Bus();
#endif

	if (msi_supported()) {
		// There is a single MSI interface; the first controller with a
		// device behind it keeps it.
		dprintf("plda: PCIe%" B_PRIu32 ": MSI already provided by another "
			"controller\n", fDomain);
	} else
		CHECK_RET(fIrqCtrl.Init(fRegsBase, irq));

	return B_OK;
}


void
PldaPCIController::SetAtrEntry(uint32 index, phys_addr_t srcAddr,
	phys_addr_t trslAddr, uint64 windowSize, uint32 trslParam)
{
	uint32 sizeBits = 0;
	while (((uint64)1 << (sizeBits + 1)) <= windowSize && sizeBits < 63)
		sizeBits++;
		// window size is 2^(field + 1); field = fls(size) - 1 as in U-Boot

	uint32 base = kPldaAtrAxi4Slv0 + index * kPldaAtrTableSize;
	WriteReg(base + kPldaAtrSrcAddrLow, ((uint32)srcAddr & 0xfffff000)
		| (sizeBits << 1) | 1);
	WriteReg(base + kPldaAtrSrcAddrHigh, (uint32)(srcAddr >> 32));
	WriteReg(base + kPldaAtrTrslAddrLow, (uint32)trslAddr & 0xfffff000);
	WriteReg(base + kPldaAtrTrslAddrHigh, (uint32)(trslAddr >> 32));
	WriteReg(base + kPldaAtrTrslParam, trslParam);

	TRACE("ATR %" B_PRIu32 ": %#" B_PRIx64 " -> %#" B_PRIx64
		" [%#" B_PRIx64 "] param %#" B_PRIx32 "\n", index, srcAddr, trslAddr,
		windowSize, trslParam);
}


void
PldaPCIController::DumpAtr()
{
	for (uint32 i = 0; i < kPldaAtrTableCount; i++) {
		uint32 base = kPldaAtrAxi4Slv0 + i * kPldaAtrTableSize;
		uint32 srcLow = ReadReg(base + kPldaAtrSrcAddrLow);
		if ((srcLow & 1) == 0)
			continue;
		TRACE("firmware ATR %" B_PRIu32 ": src %#" B_PRIx32 "%08"
			B_PRIx32 " trsl %#" B_PRIx32 "%08" B_PRIx32 " param %#" B_PRIx32
			"\n", i, ReadReg(base + kPldaAtrSrcAddrHigh), srcLow,
			ReadReg(base + kPldaAtrTrslAddrHigh),
			ReadReg(base + kPldaAtrTrslAddrLow),
			ReadReg(base + kPldaAtrTrslParam));
	}
}


addr_t
PldaPCIController::ConfigAddress(uint8 bus, uint8 device, uint8 function,
	uint16 offset)
{
	// The root port is the only device on bus 0. On the secondary bus the
	// bridge answers for every device number with the one device behind it
	// (U-Boot: "secondary bus of host bridge enumerates duplicate devices").
	if ((bus == 0 || bus == fSecondaryBus) && device != 0)
		return 0;

	addr_t ecamOffset = ((addr_t)bus << 20) | ((addr_t)device << 15)
		| ((addr_t)function << 12) | offset;
	if (ecamOffset + 4 > fConfigSize)
		return 0;

	return fConfigBase + ecamOffset;
}


bool
PldaPCIController::IsHiddenRootPortBar(uint8 bus, uint8 device,
	uint8 function, uint16 offset)
{
	// As in Linux: the root port BARs must not be assigned.
	return bus == 0 && device == 0 && function == 0
		&& offset >= PCI_base_registers && offset < PCI_base_registers + 8;
}


//#pragma mark - PCI controller


status_t
PldaPCIController::ReadConfig(uint8 bus, uint8 device, uint8 function,
	uint16 offset, uint8 size, uint32& value)
{
	InterruptsSpinLocker lock(fLock);

	addr_t address = ConfigAddress(bus, device, function, offset);
	if (address == 0) {
		// filtered out or beyond the window: answer like an empty slot
		value = size >= 4 ? 0xffffffff : (1u << (size * 8)) - 1;
		return B_OK;
	}

	if (IsHiddenRootPortBar(bus, device, function, offset)) {
		value = 0;
		return B_OK;
	}

	switch (size) {
		case 1: value = *(vuint8*)address; break;
		case 2: value = *(vuint16*)address; break;
		case 4: value = *(vuint32*)address; break;
		default:
			return B_BAD_VALUE;
	}

	return B_OK;
}


status_t
PldaPCIController::WriteConfig(uint8 bus, uint8 device, uint8 function,
	uint16 offset, uint8 size, uint32 value)
{
	InterruptsSpinLocker lock(fLock);

	addr_t address = ConfigAddress(bus, device, function, offset);
	if (address == 0)
		return B_OK;

	if (IsHiddenRootPortBar(bus, device, function, offset))
		return B_OK;

	switch (size) {
		case 1: *(vuint8*)address = (uint8)value; break;
		case 2: *(vuint16*)address = (uint16)value; break;
		case 4: *(vuint32*)address = value; break;
		default:
			return B_BAD_VALUE;
	}

	// follow the secondary bus number of the root port
	if (bus == 0 && device == 0 && function == 0) {
		if (offset == PCI_secondary_bus && size == 1)
			fSecondaryBus = (uint8)value;
		else if (offset == PCI_primary_bus && size >= 2)
			fSecondaryBus = (uint8)(value >> 8);
	}

	return B_OK;
}


status_t
PldaPCIController::GetMaxBusDevices(int32& count)
{
	count = 32;
	return B_OK;
}


status_t
PldaPCIController::ReadIrq(uint8 bus, uint8 device, uint8 function,
	uint8 pin, uint8& irq)
{
	return B_UNSUPPORTED;
}


status_t
PldaPCIController::WriteIrq(uint8 bus, uint8 device, uint8 function,
	uint8 pin, uint8 irq)
{
	return B_UNSUPPORTED;
}


status_t
PldaPCIController::GetRange(uint32 index, pci_resource_range* range)
{
	if (index >= (uint32)fResourceRanges.Count())
		return B_BAD_INDEX;

	*range = fResourceRanges[index];
	return B_OK;
}
