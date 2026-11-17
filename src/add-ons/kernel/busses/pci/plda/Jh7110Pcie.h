/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * Register layout used to bring up a PCIe port of the StarFive JH7110, from
 * the U-Boot drivers pcie_starfive_jh7110.c, phy-jh7110-pcie.c and
 * pinctrl-starfive.c. No kernel dependency, so that the arithmetic can be
 * tested on the build host.
 */
#ifndef _JH7110_PCIE_H_
#define _JH7110_PCIE_H_


#include <SupportDefs.h>


// STG system controller, one block of registers per port
#define JH7110_STG_SYSCON_BASE			0x10240000
#define JH7110_STG_SYSCON_PCIE0_BASE	0x48
#define JH7110_STG_SYSCON_PCIE1_BASE	0x1f8

#define JH7110_STG_AR					0x78
#define JH7110_STG_AR_FUNC_MASK			0x007fff00	// bits 22-8
#define JH7110_STG_AR_FUNC_SHIFT		8
#define JH7110_STG_AW					0x7c
#define JH7110_STG_AW_FUNC_MASK			0x00007fff	// bits 14-0
#define JH7110_STG_AW_CKREF_SRC_MASK	0x000c0000	// bits 19-18
#define JH7110_STG_AW_CKREF_SRC_SHIFT	18
#define JH7110_STG_AW_CLKREQ			(1u << 22)
#define JH7110_STG_RP_NEP				0xe8
#define JH7110_STG_K_RP_NEP				(1u << 8)
#define JH7110_STG_LNKSTA				0x170
#define JH7110_STG_DATA_LINK_ACTIVE		(1u << 5)

// physical functions of the bridge; all but #0 are disabled
#define JH7110_PLDA_FUNC_COUNT			4
#define JH7110_PLDA_PHY_FUNC_SHIFT		9

// PCIe PHYs (KVCO fine tuning as written by U-Boot and Linux at PHY probe)
#define JH7110_PCIE_PHY0_BASE			0x10210000
#define JH7110_PCIE_PHY1_BASE			0x10220000
#define JH7110_PHY_KVCO_LEVEL			0x28
#define JH7110_PHY_KVCO_LEVEL_VALUE		0x91
#define JH7110_PHY_KVCO_TUNE_SIGNAL		0x80
#define JH7110_PHY_KVCO_TUNE_VALUE		0x0c
#define JH7110_PHY_PLL_CTL				0x7c

// SYS GPIO controller: one byte per GPIO in the DOEN and DOUT registers
#define JH7110_SYS_GPIO_BASE			0x13040000
#define JH7110_SYS_GPIO_DOEN			0x000
#define JH7110_SYS_GPIO_DOUT			0x040
#define JH7110_GPIO_DOEN_MASK			0x3f
#define JH7110_GPIO_DOUT_MASK			0x7f
#define JH7110_GPOEN_ENABLE				0
#define JH7110_GPOEN_DISABLE			1

// PERST# lines of the upstream device trees (active low), if the firmware
// device tree has no "perst-gpios"
#define JH7110_PCIE0_PERST_GPIO			26
#define JH7110_PCIE1_PERST_GPIO			28


static inline uint32
jh7110_update_bits(uint32 value, uint32 mask, uint32 bits)
{
	return (value & ~mask) | (bits & mask);
}


static inline uint32
jh7110_stg_port_base(uint32 domain)
{
	return domain == 0 ? JH7110_STG_SYSCON_PCIE0_BASE
		: JH7110_STG_SYSCON_PCIE1_BASE;
}


static inline uint32
jh7110_gpio_offset(uint32 pin)
{
	return 4 * (pin / 4);
}


static inline uint32
jh7110_gpio_shift(uint32 pin)
{
	return 8 * (pin % 4);
}


//!	DOUT register value driving \a pin to \a high
static inline uint32
jh7110_gpio_dout(uint32 value, uint32 pin, bool high)
{
	uint32 shift = jh7110_gpio_shift(pin);
	return jh7110_update_bits(value, JH7110_GPIO_DOUT_MASK << shift,
		(high ? 1u : 0u) << shift);
}


//!	DOEN register value enabling (or disabling) the output of \a pin
static inline uint32
jh7110_gpio_doen(uint32 value, uint32 pin, bool outputEnable)
{
	uint32 shift = jh7110_gpio_shift(pin);
	return jh7110_update_bits(value, JH7110_GPIO_DOEN_MASK << shift,
		(outputEnable ? JH7110_GPOEN_ENABLE : JH7110_GPOEN_DISABLE) << shift);
}


//!	Memory base or limit register of a PCI-to-PCI bridge (1 MB granularity)
static inline uint16
jh7110_bridge_memory_reg(uint64 address)
{
	return (uint16)((address >> 16) & 0xfff0);
}


#endif	// _JH7110_PCIE_H_
