/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	What the imported brcmsmac phy layer stands on.

	Those sources are taken from Linux unchanged, so they expect Linux's
	Broadcom bus and the surrounding driver to be there. Everything they
	reach for is here, backed by the window into BAR0 this driver already
	uses, and by what the chip's own SPROM says about the card.
*/


#include "linux/types.h"

#include <KernelExport.h>
#include <OS.h>

#include "glue_net.h"

#include "linux/bcma/bcma.h"
#include "phy_shim.h"
#include "pub.h"
#include "types.h"
#include "glue.h"
#include "linux/ssb/ssb_regs.h"


#define CORE_SIZE				0x1000
#define BAR0_WINDOW				0x80
#define BAR0_WINDOW2			0xac

#define WRAP_IOCTL				0x0408
#define WRAP_RESET_CTL			0x0800
#define  RESET_CTL_RESET		0x0001
#define  IOCTL_CLK				0x0001
#define  IOCTL_FGC				0x0002


struct bcma_window {
	addr_t		registers;
	uint32		coreAddress;
	uint32		coreWrapper;
};

static struct bcma_window sHardware;
static struct bcma_device sWirelessCore;
static struct bcma_device sChipCommon;
static struct bcma_device sPcieCore;
static struct bcma_bus sBus;


/*!	Pages a core, and its wrapper, into the window in BAR0. */
static void
select_core(struct bcma_device* core)
{
	if (sBus.mapped_core == core)
		return;

	brcms_glue_pci_write(BAR0_WINDOW, core->addr, 4);
	brcms_glue_pci_write(BAR0_WINDOW2, core->wrap, 4);
	brcms_glue_pci_read(BAR0_WINDOW2, 4);

	sBus.mapped_core = core;
}


/*!	Holds the window still for one access.

	There is one window and several cores, so reaching a register is two
	steps: point the window at the core, then touch the register. The two
	have to be one step as far as everything else is concerned.

	They were not. The interrupt handler reads the wireless core's status,
	the phy's calibration timer reads the phy, and both of those run in the
	interrupt, on top of whatever thread was partway through an access of
	its own. The thread then finished its access against whichever core the
	interrupt had left the window pointing at -- reading nonsense, or
	writing a reset or a clock request into a core that was never meant to
	receive one. It is intermittent by nature: it needs an interrupt to land
	in a window a few instructions wide.

	Moving the window also writes the card's PCI configuration space, which
	is not something to be doing underneath another thread doing the same.

	So: interrupts off and a spinlock, for the pair together. Every access
	is a handful of instructions, and the waits that spin on a register let
	go between reads.

	The hold nests. Reaching one register of the chip is often several
	accesses of this window -- the phy and the shared memory are both
	behind an address register and a data register -- and a caller that
	says so up front pays for the lock once instead of once per access.
	Nothing may sleep or wait on the chip inside a batch: interrupts are
	off for the length of it, so it stays a handful of accesses.
*/
static spinlock sWindow = B_SPINLOCK_INITIALIZER;
static volatile thread_id sWindowHolder = -1;
static volatile int32 sWindowDepth;


static inline cpu_status
enter_window(void)
{
	cpu_status state = disable_interrupts();
	thread_id self = find_thread(NULL);

	// Whoever holds the window has interrupts off until it lets go, so
	// nothing else runs on that processor in between and the holder is
	// still the thread that took it. A thread runs on one processor at a
	// time, so another processor's hold never reads as this one's.
	if (sWindowHolder != self) {
		acquire_spinlock(&sWindow);
		sWindowHolder = self;
	}

	sWindowDepth++;
	return state;
}


static inline void
leave_window(cpu_status state)
{
	if (--sWindowDepth == 0) {
		sWindowHolder = -1;
		release_spinlock(&sWindow);
	}

	restore_interrupts(state);
}


static inline cpu_status
hold_window(struct bcma_device* core)
{
	cpu_status state = enter_window();
	select_core(core);
	return state;
}


static inline void
release_window(cpu_status state)
{
	leave_window(state);
}


cpu_status
brcms_glue_window_hold(void)
{
	return enter_window();
}


void
brcms_glue_window_release(cpu_status state)
{
	leave_window(state);
}


static u32
read32(struct bcma_device* core, u16 offset)
{
	cpu_status state = hold_window(core);
	u32 value = *(volatile uint32*)(sHardware.registers + offset);
	release_window(state);
	return value;
}


static u16
read16(struct bcma_device* core, u16 offset)
{
	cpu_status state = hold_window(core);
	u16 value = *(volatile uint16*)(sHardware.registers + offset);
	release_window(state);
	return value;
}


static u8
read8(struct bcma_device* core, u16 offset)
{
	cpu_status state = hold_window(core);
	u8 value = *(volatile uint8*)(sHardware.registers + offset);
	release_window(state);
	return value;
}


static void
write32(struct bcma_device* core, u16 offset, u32 value)
{
	cpu_status state = hold_window(core);
	*(volatile uint32*)(sHardware.registers + offset) = value;
	release_window(state);
}


static void
write16(struct bcma_device* core, u16 offset, u16 value)
{
	cpu_status state = hold_window(core);
	*(volatile uint16*)(sHardware.registers + offset) = value;
	release_window(state);
}


static void
write8(struct bcma_device* core, u16 offset, u8 value)
{
	cpu_status state = hold_window(core);
	*(volatile uint8*)(sHardware.registers + offset) = value;
	release_window(state);
}


/*!	A core's own control registers live in its wrapper, not its body. */
static u32
aread32(struct bcma_device* core, u16 offset)
{
	cpu_status state = hold_window(core);
	u32 value = *(volatile uint32*)(sHardware.registers + CORE_SIZE + offset);
	release_window(state);
	return value;
}


static void
awrite32(struct bcma_device* core, u16 offset, u32 value)
{
	cpu_status state = hold_window(core);
	*(volatile uint32*)(sHardware.registers + CORE_SIZE + offset) = value;
	release_window(state);
}


const struct bcma_host_ops kHostOperations = {
	.read8 = read8,
	.read16 = read16,
	.read32 = read32,
	.write8 = write8,
	.write16 = write16,
	.write32 = write32,
	.aread32 = aread32,
	.awrite32 = awrite32,
};


bool
bcma_core_is_enabled(struct bcma_device* core)
{
	if ((bcma_aread32(core, WRAP_IOCTL) & (IOCTL_CLK | IOCTL_FGC))
			!= IOCTL_CLK) {
		return false;
	}

	return (bcma_aread32(core, WRAP_RESET_CTL) & RESET_CTL_RESET) == 0;
}


void
bcma_core_disable(struct bcma_device* core, u32 flags)
{
	if ((bcma_aread32(core, WRAP_RESET_CTL) & RESET_CTL_RESET) != 0)
		return;

	bcma_awrite32(core, WRAP_RESET_CTL, RESET_CTL_RESET);
	bcma_aread32(core, WRAP_RESET_CTL);
	snooze(1);

	bcma_awrite32(core, WRAP_IOCTL, flags);
	bcma_aread32(core, WRAP_IOCTL);
	snooze(10);
}


int
bcma_core_enable(struct bcma_device* core, u32 flags)
{
	bcma_core_disable(core, flags);

	bcma_awrite32(core, WRAP_IOCTL, IOCTL_CLK | IOCTL_FGC | flags);
	bcma_aread32(core, WRAP_IOCTL);

	bcma_awrite32(core, WRAP_RESET_CTL, 0);
	bcma_aread32(core, WRAP_RESET_CTL);
	snooze(1);

	bcma_awrite32(core, WRAP_IOCTL, IOCTL_CLK | flags);
	bcma_aread32(core, WRAP_IOCTL);
	snooze(1);

	return 0;
}


/*!	ChipCommon, the PLL and the clock, which the imported sources reach
	through these four calls rather than through plain register access.
	They are written here against the register definitions rather than
	taken from Linux's own bcma, which is under the GPL.
*/


#define BCMA_CLKCTLST_LOCAL			0x01e0
#define  CLKCTLST_FORCEHT			0x00000002
#define  CLKCTLST_HAVEHT			0x00020000


void
bcma_core_set_clockmode(struct bcma_device* core, enum bcma_clkmode mode)
{
	if (mode != BCMA_CLKMODE_FAST) {
		// Let the backplane fall back to whatever clock it wants.
		bcma_mask32(core, BCMA_CLKCTLST_LOCAL, ~CLKCTLST_FORCEHT);
		return;
	}

	bcma_set32(core, BCMA_CLKCTLST_LOCAL, CLKCTLST_FORCEHT);

	// The PLL needs a moment, and then up to fifteen milliseconds to say
	// the fast clock is there.
	snooze(64);
	for (int i = 0; i < 1500; i++) {
		if ((bcma_read32(core, BCMA_CLKCTLST_LOCAL) & CLKCTLST_HAVEHT)
				!= 0) {
			return;
		}

		snooze(10);
	}

	dprintf("broadcom43xxn: high speed clock never arrived\n");
}


void
bcma_chipco_chipctl_maskset(struct bcma_drv_cc* cc, u32 offset, u32 mask,
	u32 set)
{
	bcma_pmu_write32(cc, BCMA_CC_PMU_CHIPCTL_ADDR, offset);
	bcma_pmu_read32(cc, BCMA_CC_PMU_CHIPCTL_ADDR);
	bcma_pmu_maskset32(cc, BCMA_CC_PMU_CHIPCTL_DATA, mask, set);
}


u32
bcma_chipco_gpio_control(struct bcma_drv_cc* cc, u32 mask, u32 value)
{
	u32 current = bcma_cc_read32(cc, BCMA_CC_GPIOCTL);
	bcma_cc_write32(cc, BCMA_CC_GPIOCTL, (current & ~mask) | (value & mask));
	return current;
}


/*!	Moves the PLL off a frequency whose harmonics land in the channel.

	The values are the ones Broadcom gives for this part; there is nothing
	to derive them from. Only the 43224 family is handled, which is what
	this driver binds to.
*/
void
bcma_pmu_spuravoid_pllupdate(struct bcma_drv_cc* cc, int spuravoid)
{
	static const u32 kAvoid[6] = {
		0x11500010, 0x000C0C06, 0x0F600A08, 0x00000000, 0x2001E920,
		0x88888815
	};
	static const u32 kNormal[6] = {
		0x11100010, 0x000C0C06, 0x03000A08, 0x00000000, 0x200005C0,
		0x88888815
	};
	const u32* values = spuravoid == 1 ? kAvoid : kNormal;

	for (int i = 0; i < 6; i++) {
		bcma_pmu_write32(cc, BCMA_CC_PMU_PLLCTL_ADDR,
			BCMA_CC_PMU_PLL_CTL0 + i);
		bcma_pmu_write32(cc, BCMA_CC_PMU_PLLCTL_DATA, values[i]);
	}

	bcma_pmu_write32(cc, BCMA_CC_PMU_CTL,
		bcma_pmu_read32(cc, BCMA_CC_PMU_CTL) | BCMA_CC_PMU_CTL_PLL_UPD);
}


/*!	The imported sources read the chip's PCI vendor id back through this. */
int
pci_read_config_dword(struct pci_dev* dev, int offset, u32* value)
{
	*value = brcms_glue_pci_read(offset, 4);
	return 0;
}


/*!	Brings up the bus the imported sources run on.

	The core list has already been walked by the probe, so this only has to
	hand the addresses over and read out what the chip and the board are.
*/
// Diagnostic only. Register offsets are documented in the Linux BCMA
// register header; no Linux implementation is imported. Do not alter link
// power policy while determining whether ASPM is enabled on this machine.
static void
report_pcie_context(void)
{
	uint32 cap = brcms_glue_pci_read(0x34, 1) & 0xfc;
	for (uint attempt = 0; cap >= 0x40 && cap <= 0xe0 && attempt < 32;
		attempt++) {
		uint32 id = brcms_glue_pci_read(cap, 1);
		if (id == 0x10) {
			dprintf("broadcom43xxn: PCIe: devctl %#lx linkctl %#lx linksta %#lx\n",
				(unsigned long)brcms_glue_pci_read(cap + 8, 2),
				(unsigned long)brcms_glue_pci_read(cap + 16, 2),
				(unsigned long)brcms_glue_pci_read(cap + 18, 2));
			break;
		}
		cap = brcms_glue_pci_read(cap + 1, 1) & 0xfc;
	}
	if (sPcieCore.bus == NULL || sPcieCore.id.rev < 8)
		return;

	// Select the indirect PM-threshold register, read it and restore the
	// selector under the existing BAR-window lock. No PM data is written.
	cpu_status state = hold_window(&sPcieCore);
	u32 saved = bcma_read32(&sPcieCore, 0x130);
	bcma_write32(&sPcieCore, 0x130, 0x128);
	bcma_read32(&sPcieCore, 0x130);
	u32 threshold = bcma_read32(&sPcieCore, 0x134);
	bcma_write32(&sPcieCore, 0x130, saved);
	bcma_read32(&sPcieCore, 0x130);
	release_window(state);
	dprintf("broadcom43xxn: PCIe: core rev %u PM threshold %#lx extended %u\n",
		sPcieCore.id.rev, (unsigned long)threshold,
		(threshold != 0xffffffff && (threshold & 0x01000000) != 0));
}


status_t
brcms_glue_bus_init(addr_t registers, const struct brcms_glue_core* cores,
	int coreCount)
{
	sHardware.registers = registers;

	sBus.ops = &kHostOperations;
	sBus.hosttype = BCMA_HOSTTYPE_PCI;
	sBus.nr_cores = coreCount;
	sBus.mapped_core = NULL;

	for (int i = 0; i < coreCount; i++) {
		struct bcma_device* core = NULL;
		if (cores[i].id == BCMA_CORE_CHIPCOMMON)
			core = &sChipCommon;
		else if (cores[i].id == BCMA_CORE_80211)
			core = &sWirelessCore;
		else if (cores[i].id == 0x820) // PCI Express core
			core = &sPcieCore;
		else
			continue;

		core->bus = &sBus;
		core->id.manuf = cores[i].manufacturer;
		core->id.id = cores[i].id;
		core->id.rev = cores[i].revision;
		core->id.class = cores[i].classId;
		core->core_index = i;
		core->addr = cores[i].address;
		core->wrap = cores[i].wrapper;
	}

	if (sChipCommon.bus == NULL || sWirelessCore.bus == NULL)
		return B_DEVICE_NOT_FOUND;

	sBus.drv_cc.core = &sChipCommon;
	sBus.drv_cc.pmu.core = &sChipCommon;

	uint32 identity = bcma_read32(&sChipCommon, 0x0000);
	sBus.chipinfo.id = identity & 0xffff;
	sBus.chipinfo.rev = (identity >> 16) & 0xf;
	sBus.chipinfo.pkg = (identity >> 20) & 0xf;

	sBus.drv_cc.capabilities = bcma_read32(&sChipCommon, 0x0004);
	sBus.drv_cc.status = bcma_read32(&sChipCommon, 0x002c);

	// The subsystem identifiers, which say what board the chip is on.
	sBus.boardinfo.vendor = brcms_glue_pci_read(0x2c, 2);
	sBus.boardinfo.type = brcms_glue_pci_read(0x2e, 2);

	brcms_glue_read_sprom(&sChipCommon, &sBus.sprom);

	dprintf("broadcom43xxn: bus: chip %#x rev %u package %u, board %#x:%#x, "
		"%d cores\n", sBus.chipinfo.id, sBus.chipinfo.rev,
		sBus.chipinfo.pkg, sBus.boardinfo.vendor, sBus.boardinfo.type,
		coreCount);

	report_pcie_context();

	return B_OK;
}


struct bcma_device*
brcms_glue_d11core(void)
{
	return &sWirelessCore;
}


struct bcma_bus*
brcms_glue_bus(void)
{
	return &sBus;
}


/*!	Lets the card fetch from memory on its own, or stops it.

	Without bus mastering the DMA engine cannot read a descriptor, let alone
	a frame, and the rings sit there full while nothing happens.
*/
void
brcms_glue_set_bus_master(bool enable)
{
	brcms_glue_pci_set_master(enable);
}
