/*
 * Copyright 2007, Marcus Overhagen. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "pci.h"
#include "pci_fixup.h"

#include <arch/int.h>

#include <KernelExport.h>


/*
 * We need to force controllers that have both SATA and PATA controllers to use
 * the split mode with the SATA controller at function 0 and the PATA
 * controller at function 1. This way the SATA controller will be picked up by
 * the AHCI driver and the IDE controller by the generic IDE driver.
 *
 * TODO(bga): This does not work when the SATA controller is configured for IDE
 * mode but this seems to be a problem with the device manager (it tries to load
 * the IDE driver for the AHCI controller for some reason).
 */
static void
jmicron_fixup_ahci(PCI *pci, uint8 domain, uint8 bus, uint8 device,
	uint8 function, uint16 deviceId)
{
	// We only care about function 0.
	if (function != 0)
		return;

	// And only devices with combined SATA/PATA.
	switch (deviceId) {
		case 0x2361: // 1 SATA, 1 PATA
		case 0x2363: // 2 SATA, 1 PATA
		case 0x2366: // 2 SATA, 2 PATA
			break;
		default:
			return;
	}

	dprintf("jmicron_fixup_ahci: domain %u, bus %u, device %u, function %u, "
		"deviceId 0x%04x\n", domain, bus, device, function, deviceId);

	// Read controller control register (0x40).
	uint32 val = pci->ReadConfig(domain, bus, device, function, 0x40, 4);
	dprintf("jmicron_fixup_ahci: Register 0x40 : 0x%08" B_PRIx32 "\n", val);

	// Clear bits.
	val &= ~(1 << 1);
	val &= ~(1 << 9);
	val &= ~(1 << 13);
	val &= ~(1 << 15);
	val &= ~(1 << 16);
	val &= ~(1 << 17);
	val &= ~(1 << 18);
	val &= ~(1 << 19);
	val &= ~(1 << 22);

	//Set bits.
	val |= (1 << 0);
	val |= (1 << 4);
	val |= (1 << 5);
	val |= (1 << 7);
	val |= (1 << 8);
	val |= (1 << 12);
	val |= (1 << 14);
	val |= (1 << 23);

	dprintf("jmicron_fixup_ahci: Register 0x40 : 0x%08" B_PRIx32 "\n", val);
	pci->WriteConfig(domain, bus, device, function, 0x40, 4, val);

	// Read IRQ from controller at function 0 and assign this IRQ to the
	// controller at function 1.
	uint8 irq = pci->ReadConfig(domain, bus, device, function, 0x3c, 1);
	dprintf("jmicron_fixup_ahci: Assigning IRQ %d at device "
		"function 1.\n", irq);
	pci->WriteConfig(domain, bus, device, 1, 0x3c, 1, irq);
}


static void
intel_fixup_ahci(PCI *pci, uint8 domain, uint8 bus, uint8 device,
	uint8 function, uint16 deviceId)
{
	// TODO(bga): disabled until the PCI manager can assign new resources.
	return;

	switch (deviceId) {
		case 0x2825: // ICH8 Desktop when in IDE emulation mode
			dprintf("intel_fixup_ahci: WARNING found ICH8 device id 0x2825\n");
			return;
		case 0x2926: // ICH9 Desktop when in IDE emulation mode
			dprintf("intel_fixup_ahci: WARNING found ICH9 device id 0x2926\n");
			return;

		case 0x27c0: // ICH7 Desktop non-AHCI and non-RAID mode
		case 0x27c4: // ICH7 Mobile non-AHCI and non-RAID mode
		case 0x2820: // ICH8 Desktop non-AHCI and non-RAID mode
		case 0x2828: // ICH8 Mobile non-AHCI and non-RAID mode
		case 0x2920: // ICH9 Desktop non-AHCI and non-RAID mode (4 ports)
		case 0x2921: // ICH9 Desktop non-AHCI and non-RAID mode (2 ports)
			break;
		default:
			return;
	}

	dprintf("intel_fixup_ahci: domain %u, bus %u, device %u, function %u, "
		"deviceId 0x%04x\n", domain, bus, device, function, deviceId);

	dprintf("intel_fixup_ahci: 0x24: 0x%08" B_PRIx32 "\n",
		pci->ReadConfig(domain, bus, device, function, 0x24, 4));
	dprintf("intel_fixup_ahci: 0x90: 0x%02" B_PRIx32 "\n",
		pci->ReadConfig(domain, bus, device, function, 0x90, 1));

	uint8 map = pci->ReadConfig(domain, bus, device, function, 0x90, 1);
	if ((map >> 6) == 0) {
		uint32 bar5 = pci->ReadConfig(domain, bus, device, function, 0x24, 4);
		uint16 pcicmd = pci->ReadConfig(domain, bus, device, function,
			PCI_command, 2);

		dprintf("intel_fixup_ahci: switching from IDE to AHCI mode\n");

		pci->WriteConfig(domain, bus, device, function, PCI_command, 2,
			pcicmd & ~(PCI_command_io | PCI_command_memory));

		pci->WriteConfig(domain, bus, device, function, 0x24, 4, 0xffffffff);
		dprintf("intel_fixup_ahci: ide-bar5 bits-1: 0x%08" B_PRIx32 "\n",
			pci->ReadConfig(domain, bus, device, function, 0x24, 4));
		pci->WriteConfig(domain, bus, device, function, 0x24, 4, 0);
		dprintf("intel_fixup_ahci: ide-bar5 bits-0: 0x%08" B_PRIx32 "\n",
			pci->ReadConfig(domain, bus, device, function, 0x24, 4));

		map &= ~0x03;
		map |= 0x40;
		pci->WriteConfig(domain, bus, device, function, 0x90, 1, map);

		pci->WriteConfig(domain, bus, device, function, 0x24, 4, 0xffffffff);
		dprintf("intel_fixup_ahci: ahci-bar5 bits-1: 0x%08" B_PRIx32 "\n",
			pci->ReadConfig(domain, bus, device, function, 0x24, 4));
		pci->WriteConfig(domain, bus, device, function, 0x24, 4, 0);
		dprintf("intel_fixup_ahci: ahci-bar5 bits-0: 0x%08" B_PRIx32 "\n",
			pci->ReadConfig(domain, bus, device, function, 0x24, 4));

		if (deviceId == 0x27c0 || deviceId == 0x27c4) // restore on ICH7
			pci->WriteConfig(domain, bus, device, function, 0x24, 4, bar5);

		pci->WriteConfig(domain, bus, device, function, PCI_command, 2, pcicmd);
	}

	dprintf("intel_fixup_ahci: 0x24: 0x%08" B_PRIx32 "\n",
		pci->ReadConfig(domain, bus, device, function, 0x24, 4));
	dprintf("intel_fixup_ahci: 0x90: 0x%02" B_PRIx32 "\n",
		pci->ReadConfig(domain, bus, device, function, 0x90, 1));
}


// The Poulsbo/SCH chipset this fixup exists for is 32-bit only, and taking
// USB away from the BIOS at PCI scan time is invasive enough that it should
// not be compiled into architectures where it cannot be needed and has never
// been exercised.
#ifdef __i386__

static void
intel_fixup_ehci_bar(PCI *pci, uint8 domain, uint8 bus, uint8 device,
	uint8 function, uint16 deviceId)
{
	if (deviceId != 0x8117)
		return;

	uint32 bar0 = pci->ReadConfig(domain, bus, device, function,
		PCI_base_registers, 4);
	if (bar0 == 0) {
		dprintf("intel_fixup_ehci_bar: domain %u, bus %u, device %u, function "
			"%u has an unassigned BAR0, assigning a fallback address\n",
			domain, bus, device, function);
		pci->WriteConfig(domain, bus, device, function, PCI_base_registers, 4,
			0xf0000000);
		bar0 = 0xf0000000;
	}

	// Take the EHCI controller away from the BIOS *now*, at PCI scan time,
	// long before any USB controller driver initializes. On this chipset
	// (Poulsbo/SCH, as found in the Sony VAIO P) the BIOS never
	// acknowledges the normal EHCI legacy-support handoff -- the EHCI
	// driver's own attempt later logs "bios won't give up control over the
	// host controller" on every boot. The EHCI driver does force-clear the
	// BIOS claim at that point, but by then it's too late for the
	// full-speed devices on the *companion UHCI controllers*: those are
	// lower PCI function numbers, so their drivers initialize and start
	// enumerating devices while the BIOS still believes it owns USB and
	// keeps intervening via SMM -- observed as UHCI "host process error"
	// halts and a companion-port device (the internal Bluetooth module)
	// failing SET_ADDRESS/GET_DESCRIPTOR forever during exactly that
	// window. Clearing the BIOS's claim and all of its SMI enables here
	// closes that window before any USB driver runs.
	uint16 pciCommand = pci->ReadConfig(domain, bus, device, function,
		PCI_command, 2);
	pci->WriteConfig(domain, bus, device, function, PCI_command, 2,
		pciCommand | PCI_command_memory);

	// The extended capability pointer (EECP) lives in the MMIO HCCPARAMS
	// register; the capability registers themselves are in PCI config
	// space. Map the register window just long enough to read it.
	uint8 eecp = 0;
	void *regs = NULL;
	area_id area = map_physical_memory("ehci bios handoff fixup",
		bar0 & ~(phys_addr_t)0xfff, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &regs);
	if (area >= 0) {
		uint32 hccparams
			= *(volatile uint32 *)((uint8 *)regs + 8);
		eecp = (hccparams >> 8) & 0xff;
		delete_area(area);
	}
	if (eecp < 0x40) {
		// Fall back to the SCH datasheet's fixed offset if the MMIO read
		// wasn't possible; validated by the capability ID check below
		// either way.
		eecp = 0x68;
	}

	uint32 legacySupport = pci->ReadConfig(domain, bus, device, function,
		eecp, 4);
	if ((legacySupport & 0xff) != 0x01) {
		dprintf("intel_fixup_ehci_bar: no legacy support capability at "
			"0x%02x (0x%08" B_PRIx32 "), skipping early BIOS handoff\n",
			eecp, legacySupport);
		pci->WriteConfig(domain, bus, device, function, PCI_command, 2,
			pciCommand);
		return;
	}

	if ((legacySupport & (1 << 16)) != 0) {
		// BIOS-owned: request ownership politely first.
		pci->WriteConfig(domain, bus, device, function, eecp + 3, 1, 1);
		for (int32 i = 0; i < 100; i++) {
			legacySupport = pci->ReadConfig(domain, bus, device, function,
				eecp, 4);
			if ((legacySupport & (1 << 16)) == 0)
				break;
			spin(5000);
		}
	}

	if ((legacySupport & (1 << 16)) != 0) {
		dprintf("intel_fixup_ehci_bar: BIOS did not release the EHCI "
			"controller, forcing the handoff\n");
	} else {
		dprintf("intel_fixup_ehci_bar: early EHCI BIOS handoff complete\n");
	}

	// Force the BIOS semaphore off and disable/clear every BIOS SMI source
	// (USBLEGCTLSTS at EECP+4) regardless -- same hard-force the EHCI
	// driver applies, just early enough to matter.
	pci->WriteConfig(domain, bus, device, function, eecp + 2, 1, 0);
	pci->WriteConfig(domain, bus, device, function, eecp + 4, 4, 0);

	pci->WriteConfig(domain, bus, device, function, PCI_command, 2,
		pciCommand);
}

#endif	// __i386__


static void
ati_fixup_ixp(PCI *pci, uint8 domain, uint8 bus, uint8 device, uint8 function,
	uint16 deviceId)
{
#if defined(__i386__) || defined(__x86_64__)
	/* ATI Technologies Inc, IXP chipset:
	 * This chipset seems broken, at least on my laptop I must force 
	 * the timer IRQ trigger mode, else no interrupt comes in.
	 * mmu_man.
	 */
	// XXX: should I use host or isa bridges for detection ??
	switch (deviceId) {
		// Host bridges
		case 0x5950:	// RS480 Host Bridge
		case 0x5830:
			break;
		// ISA bridges
		case 0x4377:	// IXP SB400 PCI-ISA Bridge 
		default:
			return;
	}
	dprintf("ati_fixup_ixp: domain %u, bus %u, device %u, function %u, deviceId 0x%04x\n",
		domain, bus, device, function, deviceId);

	dprintf("ati_fixup_ixp: found IXP chipset, forcing IRQ 0 as level triggered.\n");
	// XXX: maybe use pic_*() ?
	arch_int_configure_io_interrupt(0, B_LEVEL_TRIGGERED, B_LOW_ACTIVE_POLARITY);

#endif
}


void
pci_fixup_device(PCI *pci, uint8 domain, uint8 bus, uint8 device,
	uint8 function)
{
	uint16 vendorId = pci->ReadConfig(domain, bus, device, function,
		PCI_vendor_id, 2);
	uint16 deviceId = pci->ReadConfig(domain, bus, device, function,
		PCI_device_id, 2);

//	dprintf("pci_fixup_device: domain %u, bus %u, device %u, function %u\n",
//		domain, bus, device, function);

	switch (vendorId) {
		case 0x197b:
			jmicron_fixup_ahci(pci, domain, bus, device, function, deviceId);
			break;

		case 0x8086:
			intel_fixup_ahci(pci, domain, bus, device, function, deviceId);
#ifdef __i386__
			intel_fixup_ehci_bar(pci, domain, bus, device, function,
				deviceId);
#endif
			break;

		case 0x1002:
			ati_fixup_ixp(pci, domain, bus, device, function, deviceId);
			break;
	}
}

