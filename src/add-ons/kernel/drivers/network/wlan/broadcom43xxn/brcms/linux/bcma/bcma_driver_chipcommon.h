/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	ChipCommon and the power management unit inside it.

	ChipCommon is core zero on every Broadcom AXI part: it holds the chip's
	identity, the SPROM shadow, the general purpose pins and, from this
	generation on, the power management unit that drives the PLLs. The
	imported brcmsmac sources address all of it by the names below.

	The register offsets are the ones brcmsmac's own chipcommon.h lays out
	in struct chipcregs -- gpiocontrol at 0x6c, pmucontrol at 0x600, the
	chip control pair at 0x650 and 0x654, the PLL control pair at 0x660 and
	0x664 -- which is where they are documented in a file this driver
	already carries under the ISC licence.

	The accessors are macros rather than functions so that this header can
	stand before the bus one that defines what they expand to.
*/
#ifndef BRCMS_LINUX_BCMA_DRIVER_CHIPCOMMON_H
#define BRCMS_LINUX_BCMA_DRIVER_CHIPCOMMON_H


#include "linux/types.h"


struct bcma_device;


#define BCMA_CC_ID					0x0000
#define BCMA_CC_CAPABILITIES		0x0004
#define BCMA_CC_CHIPSTATUS			0x002c
#define BCMA_CC_CHIPCTL				0x0028
#define BCMA_CC_GPIOCTL				0x006c
#define BCMA_CC_SPROM				0x0800

// The power management unit. Its chip control and PLL control registers are
// each an address and a data register: the index goes in the first and the
// value is then read or written through the second.
#define BCMA_CC_PMU_CTL				0x0600
#define  BCMA_CC_PMU_CTL_PLL_UPD	0x00000400
#define BCMA_CC_PMU_CAP				0x0604
#define BCMA_CC_PMU_CHIPCTL_ADDR	0x0650
#define BCMA_CC_PMU_CHIPCTL_DATA	0x0654
#define BCMA_CC_PMU_PLLCTL_ADDR		0x0660
#define BCMA_CC_PMU_PLLCTL_DATA		0x0664

// Which of the PLL control registers, by index rather than by address.
#define BCMA_CC_PMU_PLL_CTL0		0
#define BCMA_CC_PMU_PLL_CTL1		1
#define BCMA_CC_PMU_PLL_CTL2		2
#define BCMA_CC_PMU_PLL_CTL3		3
#define BCMA_CC_PMU_PLL_CTL4		4
#define BCMA_CC_PMU_PLL_CTL5		5


/*!	The power management unit, which on this part is inside ChipCommon and
	so is reached through the same core.
*/
struct bcma_chipcommon_pmu {
	struct bcma_device*		core;
	u8						rev;
};


struct bcma_drv_cc {
	struct bcma_device*			core;
	u32							status;
	u32							capabilities;
	u32							capabilities_ext;
	u16							fast_pwrup_delay;
	struct bcma_chipcommon_pmu	pmu;
};


#define bcma_cc_read32(cc, offset) \
	bcma_read32((cc)->core, offset)
#define bcma_cc_write32(cc, offset, value) \
	bcma_write32((cc)->core, offset, value)
#define bcma_cc_mask32(cc, offset, mask) \
	bcma_cc_write32(cc, offset, bcma_cc_read32(cc, offset) & (mask))
#define bcma_cc_set32(cc, offset, set) \
	bcma_cc_write32(cc, offset, bcma_cc_read32(cc, offset) | (set))
#define bcma_cc_maskset32(cc, offset, mask, set) \
	bcma_cc_write32(cc, offset, (bcma_cc_read32(cc, offset) & (mask)) | (set))

#define bcma_pmu_read32(cc, offset) \
	bcma_read32((cc)->pmu.core, offset)
#define bcma_pmu_write32(cc, offset, value) \
	bcma_write32((cc)->pmu.core, offset, value)
#define bcma_pmu_mask32(cc, offset, mask) \
	bcma_pmu_write32(cc, offset, bcma_pmu_read32(cc, offset) & (mask))
#define bcma_pmu_set32(cc, offset, set) \
	bcma_pmu_write32(cc, offset, bcma_pmu_read32(cc, offset) | (set))
#define bcma_pmu_maskset32(cc, offset, mask, set) \
	bcma_pmu_write32(cc, offset, \
		(bcma_pmu_read32(cc, offset) & (mask)) | (set))


void bcma_chipco_chipctl_maskset(struct bcma_drv_cc* cc, u32 offset, u32 mask,
	u32 set);
u32 bcma_chipco_gpio_control(struct bcma_drv_cc* cc, u32 mask, u32 value);
void bcma_pmu_spuravoid_pllupdate(struct bcma_drv_cc* cc, int spuravoid);


#endif	// BRCMS_LINUX_BCMA_DRIVER_CHIPCOMMON_H
