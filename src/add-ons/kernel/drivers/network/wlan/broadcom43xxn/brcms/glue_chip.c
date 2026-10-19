/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Enumerates the cores behind a Broadcom AXI (bcma) wireless chip.

	The wireless chips from BCM4313 onwards are not a single device: the PCI
	function is a bridge onto an interconnect carrying a set of cores, and
	nothing can be read from the chip until the right core has been paged in
	through a window in BAR0. Before a driver can be written against one, the
	core layout of the actual part has to be known.

	This publishes no device. It walks the enumeration ROM once and writes
	what it found to the syslog, so that a machine that is not to hand can
	report back what it has.
*/


#include <KernelExport.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <FindDirectory.h>

#include "brcmu_wifi.h"
#include "glue.h"
#include "glue_net.h"
#include "linux/bcma/bcma.h"


#define TRACE(x...)	dprintf("broadcom43xxn: " x)

#define VENDOR_BROADCOM			0x14e4

// The chips this is meant for. Both are BCM43224 silicon; Apple ships the
// second one in the MacBook7,1.
static const uint16 kDeviceIds[] = { 0x0576, 0x4353, 0x4357, 0x4727 };

// A core's registers are paged into the first 4 KiB of BAR0, and its wrapper
// into the second. Which core that is comes from a backplane address written
// to these two config registers.
#define BAR0_WINDOW				0x80
#define BAR0_WINDOW2			0xac
#define CORE_SIZE				0x1000

#define ADDR_BASE				0x18000000

#define CC_ID					0x0000
#define  CC_ID_ID				0x0000ffff
#define  CC_ID_REV				0x000f0000
#define  CC_ID_REV_SHIFT		16
#define  CC_ID_PKG				0x00f00000
#define  CC_ID_PKG_SHIFT		20
#define  CC_ID_NRCORES			0x0f000000
#define  CC_ID_NRCORES_SHIFT	24
#define CC_CAPABILITIES			0x0004
#define CC_CHIPSTATUS			0x002c
#define CC_EROM					0x00fc
#define CC_SROM_CONTROL			0x0190

#define CC_SPROM				0x0800

// A core is reset and clocked through its wrapper, which is paged into the
// second 4 KiB of BAR0 by the same mechanism as the core itself.
#define WRAP_IOCTL				0x0408
#define  IOCTL_CLK				0x0001
#define  IOCTL_FGC				0x0002
#define WRAP_RESET_CTL			0x0800
#define  RESET_CTL_RESET		0x0001
#define WRAP_RESET_ST			0x0804

// Clock control, present in ChipCommon, the PCIe core and the 802.11 core.
#define CLKCTLST				0x01e0
#define  CLKCTLST_FORCEHT		0x00000002
#define  CLKCTLST_HAVEHT		0x00020000

// From d11 core revision 18 on, the MAC no longer turns the PHY clock on by
// itself when a PHY register is reached through it, so the core has to be
// brought up with this set or its registers read back as ones.
#define D11_IOCTL_FGC			0x0002
#define D11_IOCTL_PCLKE			0x0004
#define D11_IOCTL_PRST			0x0008
#define D11_IOCTL_BWMASK		0x00c0
#define  D11_IOCTL_BW20			0x0040

// Radio power control, in PHY register 0x78.
// Reading a radio register is not the same as writing one: the address has
// to carry a bit saying so, and which bit depends on the radio generation.
// Up to n-phy revision 6 it is this one.
// The PHY's internal tables are reached through an address register and a
// pair of data registers.
#define PHY_TABLE_ADDRESS		0x72
#define PHY_TABLE_DATA_HIGH		0x73
#define PHY_TABLE_DATA_LOW		0x74
#define NPHY_TBL_ID_RFSEQ		7
#define NPHY_TBL_ID_AFECTRL		8
#define NPHY_TBL_ID_ANTSWCTRLLUT	9
#define NPHY_TBL_ID_NOISEVAR	16
#define NPHY_TBL_ID_WEIGHTS		30

// What this card is, read out of its own SPROM rather than assumed: an
// internal power amplifier on 2.4 GHz, power detector range 4, isolation 3.
#define FEM_EXTPA_GAIN_INTERNAL	2

// Radio register offsets, taken from brcmsmac rather than written out by
// hand: they are not in any order a reader could guess.
#define SYN_COM_CTRL	0x8
#define SYN_COM_PU	0x9
#define SYN_COM_RESET	0xb
#define SYN_RCCAL_CTRL0	0x2e
#define SYN_PLL_MAST2	0x3d
#define SYN_PLL_MAST3	0x3e
#define SYN_PLL_VCOCAL1	0x56
#define SYN_PLL_VCOCAL2	0x57
#define SYN_PLL_VCOCAL12	0x60
#define SYN_PLL_REFDIV	0x46
#define SYN_PLL_MMD2	0x51
#define SYN_PLL_MMD1	0x50
#define SYN_PLL_LOOPFILTER1	0x4b
#define SYN_PLL_LOOPFILTER2	0x4c
#define SYN_PLL_LOOPFILTER3	0x4d
#define SYN_PLL_LOOPFILTER4	0x4e
#define SYN_PLL_LOOPFILTER5	0x4f
#define SYN_PLL_CP2	0x49
#define SYN_RESERVED_ADDR27	0x1b
#define SYN_RESERVED_ADDR28	0x1c
#define SYN_RESERVED_ADDR29	0x1d
#define SYN_LOGEN_VCOBUF1	0x70
#define SYN_LOGEN_MIXER2	0x72
#define SYN_LOGEN_BUF3	0x75
#define SYN_LOGEN_BUF4	0x76
#define SYN_PLL_MONITOR1	0x54
#define SYN_PLL_MONITOR2	0x55
#define SYN_PLL_STATUS1	0x63
#define SYN_PLL_STATUS2	0x64
#define SYN_PLL_STATUS3	0x65
#define SYN_TEMPPROCSENSE	0x25
#define RX_LNAG_TUNE	0x34
#define TX_INTPAG_BOOST_TUNE	0x41
#define TX_PADG_BOOST_TUNE	0x53
#define TX_PGAG_BOOST_TUNE	0x5e
#define TX_MIXG_BOOST_TUNE	0x64

// The PHY runs the radio through a sequencer. Writing the radio's registers
// settles what it will do; the radio only changes when a sequence is run.
#define PHY_RFSEQ_MODE			0xa1
#define  RFSEQ_MODE_CORE_ACTIVE	0x0001
#define  RFSEQ_MODE_TRIGGER		0x0002
#define PHY_RFSEQ_TRIGGER		0xa3
#define PHY_RFSEQ_STATUS		0xa4
#define  RFSEQ_RESET_TO_RX		0x0020

#define RADIO_READ_OFFSET		0x0100

#define RFCC_CHIP0_PU			0x0400
#define RFCC_OE_POR_FORCE		0x0080

#define D11_PHYVERSION			0x03e0

// Object memory is reached through an address and a data register. The
// microcode lives in the one selected by UCM, and the address walks itself
// on every access when AUTO_INC is asked for.
// The PHY and the radio sit behind address/data register pairs of their own.
#define D11_RADIOREGADDR		0x03d8
#define D11_RADIOREGDATA		0x03da
#define D11_PHY4WADDR			0x03f6
#define D11_PHY4WDATAHI			0x03f8
#define D11_PHY4WDATALO			0x03fa
#define D11_PHYREGADDR			0x03fc
#define D11_PHYREGDATA			0x03fe

#define RADIO_IDCODE			0x01
#define  IDCODE_VER_MASK		0x0000000f
#define  IDCODE_ID_MASK			0x0ffff000
#define  IDCODE_ID_SHIFT		12
#define  IDCODE_REV_MASK		0xf0000000
#define  IDCODE_REV_SHIFT		28
#define  RADIO_BCM2056			0x2056

// The initialisation values the microcode expects, as a table in the
// firmware: pairs of a register and what to put in it, ended by 0xffff.
#define  TAG_NPHY_INITVALS		9
#define  TAG_NPHY_BSINITVALS	8

#define D11_MACCONTROL			0x0120
#define  MCTL_EN_MAC			(1 << 0)
#define  MCTL_PSM_RUN			(1 << 1)
#define  MCTL_PSM_JMP_0			(1 << 2)
#define  MCTL_IHR_EN			(1 << 10)
#define  MCTL_INFRA			(1 << 17)
#define  MCTL_WAKE				(1 << 26)
#define D11_MACINTSTATUS		0x0128
#define  MI_MACSSPNDD			(1 << 0)

// Shared memory, where the microcode leaves the revision it was built from.
#define  OBJADDR_SHM_SEL		0x00010000
#define  SHM_BOM_REV_MAJOR		0x0000
#define  SHM_BOM_REV_MINOR		0x0002

#define D11_OBJADDR				0x0160
#define D11_OBJDATA				0x0164
#define  OBJADDR_UCM_SEL		0x00000000
#define  OBJADDR_AUTO_INC		0x03000000

// The firmware is one blob plus a table saying what is where in it. Each
// table entry is an offset, a length and a tag.
#define FIRMWARE_DIRECTORY		"firmware/broadcom43xxn"
#define FIRMWARE_IMAGE			"bcm43xx-0.fw"
#define FIRMWARE_TABLE			"bcm43xx_hdr-0.fw"
#define  TAG_MIMO_UCODE			10
#define  TAG_MIMO_UCODE_SIZE	11

#define CORE_80211				0x812

// Enumeration ROM entries.
#define ER_VALID				0x00000001
#define ER_TAG					0x0000000e
#define  ER_TAG_CI				0x00000000
#define  ER_TAG_MP				0x00000002
#define  ER_TAG_ADDR			0x00000004
#define  ER_TAG_END				0x0000000e
#define ER_TAGX					0x00000006

#define CIA_CLASS				0x000000f0
#define CIA_CLASS_SHIFT			4
#define CIA_ID					0x000fff00
#define CIA_ID_SHIFT			8
#define CIA_MANUF				0xfff00000
#define CIA_MANUF_SHIFT			20

#define CIB_NMP					0x000001f0
#define CIB_NMP_SHIFT			4
#define CIB_NSP					0x00003e00
#define CIB_NSP_SHIFT			9
#define CIB_NMW					0x0007c000
#define CIB_NMW_SHIFT			14
#define CIB_NSW					0x00f80000
#define CIB_NSW_SHIFT			19
#define CIB_REV					0xff000000
#define CIB_REV_SHIFT			24

#define ADDR_AG32				0x00000008
#define ADDR_SZ					0x00000030
#define  ADDR_SZ_SZD			0x00000030
#define ADDR_TYPE				0x000000c0
#define  ADDR_TYPE_SLAVE		0x00000000
#define  ADDR_TYPE_SWRAP		0x00000080
#define  ADDR_TYPE_MWRAP		0x000000c0
#define ADDR_PORT				0x00000f00
#define ADDR_PORT_SHIFT			8
#define ADDR_ADDR				0xfffff000
#define SIZE_SG32				0x00000008


static struct brcms_glue_core sCores[16];
static bool sPhyRunning;
static uint32 sWirelessAddress;
static uint8 sWirelessRevision;
static uint32 sWirelessWrapper;


static inline uint32
read32(addr_t base, uint32 offset)
{
	return *(volatile uint32*)(base + offset);
}


static inline void
write16(addr_t base, uint32 offset, uint16 value)
{
	*(volatile uint16*)(base + offset) = value;
}


static inline uint16
read16(addr_t base, uint32 offset)
{
	return *(volatile uint16*)(base + offset);
}


static inline void
write32(addr_t base, uint32 offset, uint32 value)
{
	*(volatile uint32*)(base + offset) = value;
}


static void
switch_core(uint32 address)
{
	brcms_glue_pci_write(BAR0_WINDOW, address, 4);
	// A read back, so the window is in place before anything is read through
	// it. The bridge posts the write otherwise.
	brcms_glue_pci_read(BAR0_WINDOW, 4);
}


/*!	Reads one enumeration ROM word and steps past it. */
static inline uint32
erom_next(addr_t erom, uint32* index)
{
	uint32 value = read32(erom, *index * 4);
	(*index)++;
	return value;
}


/*!	Steps over the ports and address descriptors of a component, leaving the
	index on the word that starts the next one.
*/
static void
erom_skip_component(addr_t erom, uint32* index, uint32 limit)
{
	while (*index < limit) {
		uint32 entry = erom_next(erom, index);
		if ((entry & ER_VALID) != 0 && (entry & ER_TAG) == ER_TAG_CI)
			break;
		if (entry == (ER_TAG_END | ER_VALID))
			break;
	}

	if (*index > 0)
		(*index)--;
}


/*!	Reads the address of a component's first slave port, which is where its
	registers live on the backplane.
*/
static uint32
erom_slave_address(addr_t erom, uint32* index, uint32 limit, uint8 masterPorts)
{
	uint32 address = 0;

	// The master ports come first and say nothing about where the core is.
	while (masterPorts-- > 0 && *index < limit) {
		uint32 entry = erom_next(erom, index);
		if ((entry & ER_VALID) == 0 || (entry & ER_TAG) != ER_TAG_MP) {
			(*index)--;
			break;
		}
	}

	if (*index >= limit)
		return 0;

	uint32 entry = erom_next(erom, index);
	if ((entry & ER_VALID) == 0 || (entry & ER_TAGX) != ER_TAG_ADDR
		|| (entry & ADDR_TYPE) != ADDR_TYPE_SLAVE
		|| ((entry & ADDR_PORT) >> ADDR_PORT_SHIFT) != 0) {
		(*index)--;
		return 0;
	}

	address = entry & ADDR_ADDR;

	// A 64 bit address and an explicit size each take a further word.
	if ((entry & ADDR_AG32) != 0)
		erom_next(erom, index);
	if ((entry & ADDR_SZ) == ADDR_SZ_SZD) {
		uint32 size = erom_next(erom, index);
		if ((size & SIZE_SG32) != 0)
			erom_next(erom, index);
	}

	return address;
}


/*!	Reads the address of a component's master wrapper, which is where its
	reset and clock control live.
*/
static uint32
erom_wrapper_address(addr_t erom, uint32* index, uint32 limit)
{
	if (*index >= limit)
		return 0;

	uint32 entry = erom_next(erom, index);
	if ((entry & ER_VALID) == 0 || (entry & ER_TAGX) != ER_TAG_ADDR
		|| ((entry & ADDR_TYPE) != ADDR_TYPE_MWRAP
			&& (entry & ADDR_TYPE) != ADDR_TYPE_SWRAP)) {
		(*index)--;
		return 0;
	}

	uint32 address = entry & ADDR_ADDR;

	if ((entry & ADDR_AG32) != 0)
		erom_next(erom, index);
	if ((entry & ADDR_SZ) == ADDR_SZ_SZD) {
		uint32 size = erom_next(erom, index);
		if ((size & SIZE_SG32) != 0)
			erom_next(erom, index);
	}

	return address;
}


/*!	Pages a core and its wrapper in, and brings the core out of reset.

	The sequence is the one every driver for this backplane uses: hold the
	core in reset with its clock forced on, let go of reset, then drop back
	to a gated clock.
*/
static void
enable_core(addr_t registers, uint32 address, uint32 wrapper, uint32 flags)
{
	brcms_glue_pci_write(BAR0_WINDOW, address, 4);
	brcms_glue_pci_write(BAR0_WINDOW2, wrapper, 4);
	brcms_glue_pci_read(BAR0_WINDOW2, 4);

	addr_t wrap = registers + CORE_SIZE;

	write32(wrap, WRAP_RESET_CTL, RESET_CTL_RESET);
	read32(wrap, WRAP_RESET_CTL);
	snooze(1);

	write32(wrap, WRAP_IOCTL, IOCTL_CLK | IOCTL_FGC | flags);
	read32(wrap, WRAP_IOCTL);
	snooze(10);

	write32(wrap, WRAP_RESET_CTL, 0);
	read32(wrap, WRAP_RESET_CTL);
	snooze(1);

	write32(wrap, WRAP_IOCTL, IOCTL_CLK | flags);
	read32(wrap, WRAP_IOCTL);
	snooze(1);


	// Coming out of reset only gates the clock on. The core's own registers
	// stay unreadable until the backplane is actually running at high speed,
	// so ask for that and wait for the chip to say it has it.
	write32(registers, CLKCTLST,
		read32(registers, CLKCTLST) | CLKCTLST_FORCEHT);
	read32(registers, CLKCTLST);

	uint32 status = 0;
	for (int32 i = 0; i < 1500; i++) {
		status = read32(registers, CLKCTLST);
		if ((status & CLKCTLST_HAVEHT) != 0)
			break;
		snooze(10);
	}

}


/*!	Brings the chip up as far as a tuned radio.

	Walks the enumeration ROM for the core list, hands that to the bus
	layer, and lets the imported phy layer loose on what it finds. After
	this the radio is on a channel and the microcode is running; nothing is
	being received until the receiver is opened.
*/
status_t
brcms_glue_hardware_up(addr_t registers)
{
	switch_core(ADDR_BASE);

	uint32 identifier = read32(registers, CC_ID);
	uint16 chip = identifier & CC_ID_ID;
	uint8 revision = (identifier & CC_ID_REV) >> CC_ID_REV_SHIFT;
	uint8 cores = (identifier & CC_ID_NRCORES) >> CC_ID_NRCORES_SHIFT;

	TRACE("chip 0x%04x rev %u, %u cores on the backplane\n", chip,
		revision, cores);

	if (identifier == 0xffffffff || identifier == 0) {
		TRACE("chipcommon reads back as 0x%08" B_PRIx32 ", the window is not "
			"mapped\n", identifier);
		return B_ERROR;
	}

	uint32 erom = read32(registers, CC_EROM);
	if (erom == 0 || erom == 0xffffffff)
		return B_ERROR;

	switch_core(erom);

	const uint32 limit = CORE_SIZE / sizeof(uint32);
	uint32 index = 0;
	int32 found = 0;

	while (index < limit) {
		uint32 cia = erom_next(registers, &index);
		if ((cia & ER_VALID) == 0)
			break;
		if (cia == (ER_TAG_END | ER_VALID))
			break;
		if ((cia & ER_TAG) != ER_TAG_CI)
			continue;

		if (index >= limit)
			break;
		uint32 cib = erom_next(registers, &index);

		uint16 id = (cia & CIA_ID) >> CIA_ID_SHIFT;
		uint16 manufacturer = (cia & CIA_MANUF) >> CIA_MANUF_SHIFT;
		uint8 rev = (cib & CIB_REV) >> CIB_REV_SHIFT;
		uint8 masterPorts = (cib & CIB_NMP) >> CIB_NMP_SHIFT;

		uint32 address = erom_slave_address(registers, &index, limit,
			masterPorts);
		uint32 wrapper = erom_wrapper_address(registers, &index, limit);
		erom_skip_component(registers, &index, limit);

		if (id == CORE_80211 && manufacturer == 0x4bf) {
			sWirelessAddress = address;
			sWirelessWrapper = wrapper;
			sWirelessRevision = rev;
		}

		if (found < (int32)(sizeof(sCores) / sizeof(sCores[0]))) {
			sCores[found].manufacturer = manufacturer;
			sCores[found].id = id;
			sCores[found].revision = rev;
			sCores[found].classId = (cia & CIA_CLASS) >> CIA_CLASS_SHIFT;
			sCores[found].address = address;
			sCores[found].wrapper = wrapper;
		}

		found++;
	}


	if (sWirelessAddress == 0) {
		TRACE("no 80211 core found\n");
		return B_NOT_SUPPORTED;
	}

	enable_core(registers, sWirelessAddress, sWirelessWrapper,
		D11_IOCTL_PCLKE);
	TRACE("d11 rev %u, phyversion 0x%04x\n", sWirelessRevision,
		read16(registers, D11_PHYVERSION));

	// From here on the radio is driven by brcmsmac's own phy layer, which
	// is imported under brcms/ and reaches the chip through this window.
	if (brcms_glue_bus_init(registers, sCores, found) != B_OK) {
		TRACE("the bus layer would not start\n");
		return B_ERROR;
	}

	// Channel one, 2412 MHz, 20 MHz wide. The stack above moves it from
	// here.
	status_t status = brcms_glue_phy_init(ch20mhz_chspec(1));
	TRACE("phy init %s\n", status == B_OK ? "succeeded" : "failed");

	return status;
}

