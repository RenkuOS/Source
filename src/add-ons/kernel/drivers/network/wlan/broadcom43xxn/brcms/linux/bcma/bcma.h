/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	The Broadcom AXI backplane, as the imported brcmsmac sources address it.

	Those sources are taken from Linux unchanged and reach the chip through
	this interface, so the names here are theirs. Nothing of Linux's own bus
	implementation is: that is under the GPL, and this driver is not. What
	is described below is the chip -- its core identifiers, the registers in
	a core's wrapper, and the shape of the accessors -- and all of it was
	read off a BCM43224 by this driver's own enumeration before any of it
	was written down.

	A core is paged into a window in BAR0 one at a time, its body into the
	first four kilobytes and its wrapper into the second, so every access
	goes through the host operations below rather than a mapping.
*/
#ifndef BRCMS_LINUX_BCMA_BCMA_H
#define BRCMS_LINUX_BCMA_BCMA_H


#include "linux/types.h"

#include "linux/bcma/bcma_driver_chipcommon.h"
#include "linux/ssb/ssb.h"


struct bcma_bus;
struct bcma_device;
struct pci_dev;


enum bcma_hosttype {
	BCMA_HOSTTYPE_PCI,
	BCMA_HOSTTYPE_SDIO,
	BCMA_HOSTTYPE_SOC,
};


/*!	Whether the backplane is held at its fast clock or left to settle. */
enum bcma_clkmode {
	BCMA_CLKMODE_FAST,
	BCMA_CLKMODE_DYNAMIC,
};


// The two cores this driver has anything to say to. The identifier is the
// one the enumeration ROM reports.
#define BCMA_CORE_CHIPCOMMON		0x800
#define BCMA_CORE_80211				0x812

// Named only so that the imported dma.c can rule them out: neither is on
// this card, and both have errata it works around.
#define BCMA_CORE_SDIO_DEV			0x829
#define BCMA_CORE_I2S				0x834

// The chips the imported sources know about. For the 43xxx parts the
// identifier is the part number in decimal, which is how a BCM43224 reads
// back 0xa8d8 from ChipCommon.
#define BCMA_CHIP_ID_BCM4313		0x4313
#define BCMA_CHIP_ID_BCM43224		43224
#define BCMA_CHIP_ID_BCM43225		43225
#define BCMA_CHIP_ID_BCM43421		43421
#define BCMA_CHIP_ID_BCM43431		43431
#define BCMA_CHIP_ID_BCM4331		0x4331
#define BCMA_CHIP_ID_BCM4716		0x4716
#define BCMA_CHIP_ID_BCM4748		0x4748
#define BCMA_CHIP_ID_BCM4749		0x4749
#define BCMA_CHIP_ID_BCM47162		47162
#define BCMA_CHIP_ID_BCM5357		0x5357
#define BCMA_CHIP_ID_BCM53572		53572

// Which fabrication a part came out of, which decides a few of the phy
// workarounds. The MacBook7,1's card reports 8, not either of these.
#define BCMA_PKG_ID_BCM43224_FAB_SMIC	0xa
#define BCMA_PKG_ID_BCM4717				9
#define BCMA_PKG_ID_BCM4718				10

// Registers in a core's own body.
#define BCMA_CLKCTLST				0x01e0
#define  BCMA_CLKCTLST_FORCEALP		0x00000001
#define  BCMA_CLKCTLST_FORCEHT		0x00000002
#define  BCMA_CLKCTLST_HAVEALP		0x00010000
#define  BCMA_CLKCTLST_HAVEHT		0x00020000
#define  BCMA_CLKCTLST_HTAREQ		0x00000010

// Registers in a core's wrapper, which is where it is reset and clocked.
#define BCMA_IOCTL					0x0408
#define BCMA_IOST					0x0500
#define BCMA_RESET_CTL				0x0800
#define  BCMA_RESET_CTL_RESET		0x0001
#define BCMA_RESET_ST				0x0804


/*!	A core, as the enumeration ROM described it. */
struct bcma_device_id {
	u16		manuf;
	u16		id;
	u8		rev;
	u8		class;
};


/*!	What ChipCommon's first register says the part is. */
struct bcma_chipinfo {
	u16		id;
	u8		rev;
	u8		pkg;
};


/*!	What the card around the chip is, which comes from PCI rather than the
	backplane.
*/
struct bcma_boardinfo {
	u16		vendor;
	u16		type;
};


/*!	How a core is reached. Everything the imported sources read or write on
	the chip arrives here, and the implementation pages the core in.
*/
struct bcma_host_ops {
	u8		(*read8)(struct bcma_device* core, u16 offset);
	u16		(*read16)(struct bcma_device* core, u16 offset);
	u32		(*read32)(struct bcma_device* core, u16 offset);
	void	(*write8)(struct bcma_device* core, u16 offset, u8 value);
	void	(*write16)(struct bcma_device* core, u16 offset, u16 value);
	void	(*write32)(struct bcma_device* core, u16 offset, u32 value);
	u32		(*aread32)(struct bcma_device* core, u16 offset);
	void	(*awrite32)(struct bcma_device* core, u16 offset, u32 value);
};


struct bcma_device {
	struct bcma_bus*		bus;
	struct bcma_device_id	id;
	struct device			dev;

	// What the DMA engine is addressed through. There is no mapping layer
	// here, so this is only ever passed about.
	struct device*			dma_dev;

	u8						core_index;
	u8						core_unit;
	u32						addr;
	u32						wrap;
	void*					drvdata;
};


struct bcma_bus {
	const struct bcma_host_ops*	ops;
	enum bcma_hosttype			hosttype;
	struct pci_dev*				host_pci;
	struct bcma_chipinfo		chipinfo;
	struct bcma_boardinfo		boardinfo;
	struct bcma_device*			mapped_core;
	u8							nr_cores;
	struct bcma_drv_cc			drv_cc;

	// The SPROM is shared with the older ssb bus, so the imported sources
	// read the board out of the same structure either way.
	struct ssb_sprom			sprom;
};


static inline u8
bcma_read8(struct bcma_device* core, u16 offset)
{
	return core->bus->ops->read8(core, offset);
}


static inline u16
bcma_read16(struct bcma_device* core, u16 offset)
{
	return core->bus->ops->read16(core, offset);
}


static inline u32
bcma_read32(struct bcma_device* core, u16 offset)
{
	return core->bus->ops->read32(core, offset);
}


static inline void
bcma_write8(struct bcma_device* core, u16 offset, u8 value)
{
	core->bus->ops->write8(core, offset, value);
}


static inline void
bcma_write16(struct bcma_device* core, u16 offset, u16 value)
{
	core->bus->ops->write16(core, offset, value);
}


static inline void
bcma_write32(struct bcma_device* core, u16 offset, u32 value)
{
	core->bus->ops->write32(core, offset, value);
}


static inline u32
bcma_aread32(struct bcma_device* core, u16 offset)
{
	return core->bus->ops->aread32(core, offset);
}


static inline void
bcma_awrite32(struct bcma_device* core, u16 offset, u32 value)
{
	core->bus->ops->awrite32(core, offset, value);
}


static inline void
bcma_mask16(struct bcma_device* core, u16 offset, u16 mask)
{
	bcma_write16(core, offset, bcma_read16(core, offset) & mask);
}


static inline void
bcma_set16(struct bcma_device* core, u16 offset, u16 set)
{
	bcma_write16(core, offset, bcma_read16(core, offset) | set);
}


static inline void
bcma_maskset16(struct bcma_device* core, u16 offset, u16 mask, u16 set)
{
	bcma_write16(core, offset, (bcma_read16(core, offset) & mask) | set);
}


static inline void
bcma_mask32(struct bcma_device* core, u16 offset, u32 mask)
{
	bcma_write32(core, offset, bcma_read32(core, offset) & mask);
}


static inline void
bcma_set32(struct bcma_device* core, u16 offset, u32 set)
{
	bcma_write32(core, offset, bcma_read32(core, offset) | set);
}


static inline void
bcma_maskset32(struct bcma_device* core, u16 offset, u32 mask, u32 set)
{
	bcma_write32(core, offset, (bcma_read32(core, offset) & mask) | set);
}


bool bcma_core_is_enabled(struct bcma_device* core);
void bcma_core_disable(struct bcma_device* core, u32 flags);
int bcma_core_enable(struct bcma_device* core, u32 flags);
void bcma_core_set_clockmode(struct bcma_device* core,
	enum bcma_clkmode clkmode);


#endif	// BRCMS_LINUX_BCMA_BCMA_H
