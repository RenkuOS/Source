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
 * The firmware brings the link up ("pci enum" in U-Boot): clocks, resets,
 * PHY, PERST# and the root port setup are left as they are. This driver only
 * checks that the port is alive, provides configuration space access and
 * handles MSIs.
 */


#include "PldaPCIController.h"

#include <bus/FDT.h>

#include <AutoDeleterDrivers.h>
#include <util/AutoLock.h>

#include <string.h>
#include <new>


// JH7110 clock/reset generator and system controller of the STG domain,
// only used to check that the firmware enabled the port
#define JH7110_SYSCRG_BASE				0x13020000
#define JH7110_SYSCLK_NOC_BUS_STG_AXI	96
#define JH7110_STGCRG_BASE				0x10230000
#define JH7110_STGCRG_RESET_ASSERT		0x74
#define JH7110_STG_SYSCON_BASE			0x10240000
#define JH7110_STG_SYSCON_PCIE0_BASE	0x48
#define JH7110_STG_SYSCON_PCIE1_BASE	0x1f8
#define JH7110_STG_SYSCON_LNKSTA		0x170
#define JH7110_STG_DATA_LINK_ACTIVE		(1u << 5)
#define JH7110_CLK_ENABLE				(1u << 31)


static uint32
read_physical_reg(phys_addr_t address)
{
	phys_addr_t pageBase = address & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	void* virtualBase;
	area_id area = map_physical_memory("jh7110 pcie check", pageBase,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &virtualBase);
	if (area < 0)
		return 0;

	uint32 value = *(vuint32*)((addr_t)virtualBase + (address - pageBase));
	delete_area(area);
	return value;
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


/*!	The port is set up by the firmware. Touching the bridge registers while
	its clocks are gated or its resets asserted would hang the bus, so check
	that first, through the clock/reset generator that is always accessible.
*/
status_t
PldaPCIController::CheckJH7110Port()
{
	if (fDomain > 1)
		return B_BAD_VALUE;

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
			"%s, resets %#" B_PRIx32 "); run \"pci enum\" in U-Boot\n",
			fDomain, clocksOn ? "on" : "off", resets);
		return B_DEV_NOT_READY;
	}

	uint32 linkStatus = read_physical_reg(JH7110_STG_SYSCON_BASE
		+ (fDomain == 0 ? JH7110_STG_SYSCON_PCIE0_BASE
			: JH7110_STG_SYSCON_PCIE1_BASE)
		+ JH7110_STG_SYSCON_LNKSTA);
	if ((linkStatus & JH7110_STG_DATA_LINK_ACTIVE) == 0) {
		dprintf("plda: PCIe%" B_PRIu32 " link down (status %#" B_PRIx32
			")\n", fDomain, linkStatus);
		return B_DEVICE_NOT_FOUND;
	}

	dprintf("plda: PCIe%" B_PRIu32 " link up\n", fDomain);
	return B_OK;
}


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

	CHECK_RET(CheckJH7110Port());

	fRegsArea.SetTo(map_physical_memory("PCI PLDA regs", fRegsPhysBase,
		fRegsSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fRegsBase));
	CHECK_RET(fRegsArea.Get());

	fConfigArea.SetTo(map_physical_memory("PCI PLDA config", fConfigPhysBase,
		fConfigSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fConfigBase));
	CHECK_RET(fConfigArea.Get());

#ifdef TRACE_PLDA
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

	// keep the secondary bus number set by the firmware
	uint32 busNumbers = *(vuint32*)(fConfigBase + PCI_primary_bus);
	if (((busNumbers >> 8) & 0xff) != 0)
		fSecondaryBus = (busNumbers >> 8) & 0xff;

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
		value = 0xffffffff;
		return ERANGE;
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
		return ERANGE;

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
