/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * The clock structure (which fields each clock has and its parents) follows
 * the JH7110 TRM and the upstream device tree binding; it was cross-checked
 * with the U-Boot and Linux JH7110 clock drivers.
 */


#include "Jh7110ClockTree.h"

#include <stddef.h>

#include "dt-bindings/clock/starfive,jh7110-crg.h"


#define CLK_ENABLE			(1u << 31)
#define CLK_MUX_SHIFT		24
#define CLK_DIV_MASK		0x00ffffffu

// field flags
enum {
	kGate	= 1 << 0,
	kInvert	= 1 << 1,	// only the clock polarity, nothing to manage
	kFrac	= 1 << 2,	// divider with FRAC_BITS fractional bits
};

// The fractional dividers (UART3-5 core, TRM tables 2-202, 2-204 and 2-206):
// 17 bits in all, 8 of them fractional; default and minimum 2560 (10.0).
#define FRAC_BITS			8
#define FRAC_MIN			2560
#define FRAC_MAX			131071


struct ClockDef {
	uint8			controller;
	uint8			id;
	uint8			flags;
	uint8			muxBits;
	uint8			divBits;
	uint8			parentCount;
	jh7110_clock_ref parents[2];
	const char*		name;
};


#define EXT(x)	{ JH7110_EXT, JH7110_EXT_##x }
#define PLL(x)	{ JH7110_PLL, JH7110_PLLCLK_##x }
#define SYS(x)	{ JH7110_SYS, JH7110_SYSCLK_##x }
#define AON(x)	{ JH7110_AON, JH7110_AONCLK_##x }
#define STG(x)	{ JH7110_STG, JH7110_STGCLK_##x }

// clock, flags, mux bits, divider bits, parents
#define DEF1(ctrl, id, flags, mux, div, p0) \
	{ JH7110_##ctrl, JH7110_##ctrl##CLK_##id, flags, mux, div, 1, { p0, {} }, \
		#id }
#define DEF2(ctrl, id, flags, mux, div, p0, p1) \
	{ JH7110_##ctrl, JH7110_##ctrl##CLK_##id, flags, mux, div, 2, { p0, p1 }, \
		#id }


static const ClockDef kClocks[] = {
	// SYS
	DEF2(SYS, CPU_ROOT, 0, 1, 0, EXT(OSC), PLL(PLL0_OUT)),
	DEF1(SYS, CPU_CORE, 0, 0, 3, SYS(CPU_ROOT)),
	DEF1(SYS, CPU_BUS, 0, 0, 2, SYS(CPU_CORE)),
	DEF2(SYS, PERH_ROOT, 0, 1, 2, PLL(PLL0_OUT), PLL(PLL2_OUT)),
	DEF2(SYS, BUS_ROOT, 0, 1, 0, EXT(OSC), PLL(PLL2_OUT)),
	DEF1(SYS, NOCSTG_BUS, 0, 0, 3, SYS(BUS_ROOT)),
	DEF1(SYS, AXI_CFG0, 0, 0, 2, SYS(BUS_ROOT)),
	DEF1(SYS, STG_AXIAHB, 0, 0, 2, SYS(AXI_CFG0)),
	DEF1(SYS, AHB0, kGate, 0, 0, SYS(STG_AXIAHB)),
	DEF1(SYS, AHB1, kGate, 0, 0, SYS(STG_AXIAHB)),
	DEF1(SYS, APB_BUS, 0, 0, 4, SYS(STG_AXIAHB)),
	DEF1(SYS, APB0, kGate, 0, 0, SYS(APB_BUS)),
	DEF1(SYS, AUDIO_ROOT, 0, 0, 4, PLL(PLL2_OUT)),
	DEF1(SYS, MCLK_INNER, 0, 0, 7, SYS(AUDIO_ROOT)),
	DEF2(SYS, MCLK, 0, 1, 0, SYS(MCLK_INNER), EXT(MCLK_EXT)),
	DEF1(SYS, PWMDAC_APB, kGate, 0, 0, SYS(APB0)),
	DEF1(SYS, PWMDAC_CORE, kGate, 0, 9, SYS(AUDIO_ROOT)),
	DEF1(SYS, I2STX0_APB, kGate, 0, 0, SYS(APB0)),
	DEF1(SYS, I2STX0_BCLK_MST, kGate, 0, 6, SYS(MCLK)),
	DEF1(SYS, I2STX0_BCLK_MST_INV, kInvert, 0, 0, SYS(I2STX0_BCLK_MST)),
	DEF2(SYS, I2STX0_LRCK_MST, 0, 1, 7, SYS(I2STX0_BCLK_MST_INV),
		SYS(I2STX0_BCLK_MST)),
	DEF2(SYS, I2STX0_BCLK, 0, 1, 0, SYS(I2STX0_BCLK_MST),
		EXT(I2STX_BCLK_EXT)),
	DEF1(SYS, I2STX0_BCLK_INV, kInvert, 0, 0, SYS(I2STX0_BCLK)),
	DEF2(SYS, I2STX0_LRCK, 0, 1, 0, SYS(I2STX0_LRCK_MST),
		EXT(I2STX_LRCK_EXT)),
	DEF1(SYS, QSPI_AHB, kGate, 0, 0, SYS(AHB1)),
	DEF1(SYS, QSPI_APB, kGate, 0, 0, SYS(APB_BUS)),
	DEF1(SYS, QSPI_REF_SRC, 0, 0, 5, PLL(PLL0_OUT)),
	DEF2(SYS, QSPI_REF, kGate, 1, 0, EXT(OSC), SYS(QSPI_REF_SRC)),
	DEF1(SYS, SDIO0_AHB, kGate, 0, 0, SYS(AHB0)),
	DEF1(SYS, SDIO1_AHB, kGate, 0, 0, SYS(AHB0)),
	DEF1(SYS, SDIO0_SDCARD, kGate, 0, 4, SYS(AXI_CFG0)),
	DEF1(SYS, SDIO1_SDCARD, kGate, 0, 4, SYS(AXI_CFG0)),
	DEF1(SYS, USB_125M, 0, 0, 4, PLL(PLL0_OUT)),
	DEF1(SYS, NOC_BUS_STG_AXI, kGate, 0, 0, SYS(NOCSTG_BUS)),
	DEF1(SYS, VOUT_TOP_HDMITX0_MCLK, kGate, 0, 0, SYS(MCLK)),
	DEF1(SYS, GMAC1_AHB, kGate, 0, 0, SYS(AHB0)),
	DEF1(SYS, GMAC1_AXI, kGate, 0, 0, SYS(STG_AXIAHB)),
	DEF1(SYS, GMAC_SRC, 0, 0, 3, PLL(PLL0_OUT)),
	DEF1(SYS, GMAC1_GTXCLK, 0, 0, 4, PLL(PLL0_OUT)),
	DEF1(SYS, GMAC1_GTXC, kGate, 0, 0, SYS(GMAC1_GTXCLK)),
	DEF1(SYS, GMAC1_RMII_RTX, 0, 0, 5, EXT(GMAC1_RMII_REFIN)),
	DEF1(SYS, GMAC1_PTP, kGate, 0, 5, SYS(GMAC_SRC)),
	DEF2(SYS, GMAC1_RX, 0, 1, 0, EXT(GMAC1_RGMII_RXIN),
		SYS(GMAC1_RMII_RTX)),
	DEF2(SYS, GMAC1_TX, kGate, 1, 0, SYS(GMAC1_GTXCLK), SYS(GMAC1_RMII_RTX)),
	DEF1(SYS, GMAC1_TX_INV, kInvert, 0, 0, SYS(GMAC1_TX)),
	DEF1(SYS, GMAC0_GTXCLK, kGate, 0, 4, PLL(PLL0_OUT)),
	DEF1(SYS, GMAC0_PTP, kGate, 0, 5, SYS(GMAC_SRC)),
	DEF1(SYS, GMAC0_GTXC, kGate, 0, 0, SYS(GMAC0_GTXCLK)),
	DEF1(SYS, UART0_APB, kGate, 0, 0, SYS(APB0)),
	DEF1(SYS, UART0_CORE, kGate, 0, 0, EXT(OSC)),
	DEF1(SYS, UART1_APB, kGate, 0, 0, SYS(APB0)),
	DEF1(SYS, UART1_CORE, kGate, 0, 0, EXT(OSC)),
	DEF1(SYS, UART2_APB, kGate, 0, 0, SYS(APB0)),
	DEF1(SYS, UART2_CORE, kGate, 0, 0, EXT(OSC)),
	DEF1(SYS, UART3_APB, kGate, 0, 0, SYS(APB0)),
	DEF1(SYS, UART3_CORE, kGate | kFrac, 0, 17, SYS(PERH_ROOT)),
	DEF1(SYS, UART4_APB, kGate, 0, 0, SYS(APB0)),
	DEF1(SYS, UART4_CORE, kGate | kFrac, 0, 17, SYS(PERH_ROOT)),
	DEF1(SYS, UART5_APB, kGate, 0, 0, SYS(APB0)),
	DEF1(SYS, UART5_CORE, kGate | kFrac, 0, 17, SYS(PERH_ROOT)),
	DEF1(SYS, I2C2_APB, kGate, 0, 0, SYS(APB0)),
	DEF1(SYS, I2C5_APB, kGate, 0, 0, SYS(APB0)),
	DEF1(SYS, WDT_APB, kGate, 0, 0, SYS(APB0)),
	DEF1(SYS, WDT_CORE, kGate, 0, 0, EXT(OSC)),
	DEF1(SYS, TEMP_APB, kGate, 0, 0, SYS(APB_BUS)),
	DEF1(SYS, TEMP_CORE, kGate, 0, 5, EXT(OSC)),

	// AON
	DEF1(AON, OSC_DIV4, 0, 0, 5, EXT(OSC)),
	DEF2(AON, APB_FUNC, 0, 1, 0, AON(OSC_DIV4), EXT(OSC)),
	DEF1(AON, GMAC0_AHB, kGate, 0, 0, SYS(STG_AXIAHB)),
	DEF1(AON, GMAC0_AXI, kGate, 0, 0, SYS(STG_AXIAHB)),
	DEF1(AON, GMAC0_RMII_RTX, 0, 0, 5, EXT(GMAC0_RMII_REFIN)),
	DEF2(AON, GMAC0_TX, kGate, 1, 0, SYS(GMAC0_GTXCLK), AON(GMAC0_RMII_RTX)),
	DEF1(AON, GMAC0_TX_INV, kInvert, 0, 0, AON(GMAC0_TX)),
	DEF1(AON, OTPC_APB, kGate, 0, 0, SYS(APB_BUS)),

	// STG
	DEF1(STG, USB0_APB, kGate, 0, 0, SYS(APB_BUS)),
	DEF1(STG, USB0_UTMI_APB, kGate, 0, 0, SYS(APB_BUS)),
	DEF1(STG, USB0_AXI, kGate, 0, 0, SYS(STG_AXIAHB)),
	DEF1(STG, USB0_LPM, kGate, 0, 2, EXT(OSC)),
	DEF1(STG, USB0_STB, kGate, 0, 3, EXT(OSC)),
	DEF1(STG, USB0_APP_125, kGate, 0, 0, SYS(USB_125M)),
	DEF1(STG, USB0_REFCLK, 0, 0, 2, EXT(OSC)),
	DEF1(STG, PCIE0_AXI_MST0, kGate, 0, 0, SYS(STG_AXIAHB)),
	DEF1(STG, PCIE0_APB, kGate, 0, 0, SYS(APB_BUS)),
	DEF1(STG, PCIE0_TL, kGate, 0, 0, SYS(STG_AXIAHB)),
	DEF1(STG, PCIE1_AXI_MST0, kGate, 0, 0, SYS(STG_AXIAHB)),
	DEF1(STG, PCIE1_APB, kGate, 0, 0, SYS(APB_BUS)),
	DEF1(STG, PCIE1_TL, kGate, 0, 0, SYS(STG_AXIAHB)),
	DEF1(STG, SEC_AHB, kGate, 0, 0, SYS(STG_AXIAHB)),
	DEF1(STG, SEC_MISC_AHB, kGate, 0, 0, SYS(STG_AXIAHB)),
	DEF1(STG, DMA1P_AXI, kGate, 0, 0, SYS(STG_AXIAHB)),
	DEF1(STG, DMA1P_AHB, kGate, 0, 0, SYS(STG_AXIAHB)),
};


// PLL registers in the SYS syscon: [pd/frac/postdiv, prediv, fbdiv,
// dacpd/dsmpd] offsets and the fbdiv/dacpd/dsmpd fields, which differ
// between PLL0 and PLL1/2
struct PllDef {
	uint16	fracOffset;		// frac [23:0], postdiv1 [29:28]
	uint16	predivOffset;	// prediv [5:0]
	uint16	fbdivOffset;
	uint8	fbdivShift;		// fbdiv is 12 bits wide
	uint16	modeOffset;
	uint8	dacpdShift;
	uint8	dsmpdShift;
};

static const PllDef kPlls[] = {
	{ 0x20, 0x24, 0x1c, 0, 0x18, 24, 25 },
	{ 0x28, 0x2c, 0x24, 17, 0x24, 15, 16 },
	{ 0x30, 0x34, 0x2c, 17, 0x2c, 15, 16 },
};


static const ClockDef*
find_clock(jh7110_clock_ref clock)
{
	for (size_t i = 0; i < sizeof(kClocks) / sizeof(kClocks[0]); i++) {
		if (kClocks[i].controller == clock.controller
			&& kClocks[i].id == clock.id)
			return &kClocks[i];
	}
	return NULL;
}


//	#pragma mark -


Jh7110ClockTree::Jh7110ClockTree(Jh7110RegisterAccess& registers)
	:
	fRegisters(registers)
{
}


/*static*/ bool
Jh7110ClockTree::IsKnown(jh7110_clock_ref clock)
{
	switch (clock.controller) {
		case JH7110_EXT:
			return clock.id <= JH7110_EXT_LAST;
		case JH7110_PLL:
			return clock.id < JH7110_PLLCLK_END;
		default:
			return find_clock(clock) != NULL;
	}
}


/*static*/ const char*
Jh7110ClockTree::Name(jh7110_clock_ref clock)
{
	switch (clock.controller) {
		case JH7110_EXT:
			return clock.id == JH7110_EXT_OSC ? "osc" : "external";
		case JH7110_PLL:
			return clock.id == 0 ? "pll0" : clock.id == 1 ? "pll1" : "pll2";
	}
	const ClockDef* def = find_clock(clock);
	return def != NULL ? def->name : "?";
}


int64
Jh7110ClockTree::PllRate(uint32 pll)
{
	if (pll >= sizeof(kPlls) / sizeof(kPlls[0]))
		return B_BAD_VALUE;

	const PllDef& def = kPlls[pll];
	uint32 fracReg = fRegisters.Read(JH7110_PLL, def.fracOffset);
	uint32 prediv = fRegisters.Read(JH7110_PLL, def.predivOffset) & 0x3f;
	uint32 fbdiv = (fRegisters.Read(JH7110_PLL, def.fbdivOffset)
		>> def.fbdivShift) & 0xfff;
	uint32 mode = fRegisters.Read(JH7110_PLL, def.modeOffset);
	uint32 dacpd = (mode >> def.dacpdShift) & 1;
	uint32 dsmpd = (mode >> def.dsmpdShift) & 1;
	uint32 postdiv = 1u << ((fracReg >> 28) & 3);
	uint64 frac = fracReg & 0xffffff;

	if (prediv == 0)
		return B_ERROR;

	// Fvco = Fref * (fbdiv + frac / 2^24) / prediv, Fout = Fvco / postdiv;
	// integer mode when dacpd and dsmpd are both set
	if (dacpd == 1 && dsmpd == 1)
		frac = 0;
	else if (dacpd != 0 || dsmpd != 0)
		return B_ERROR;

	uint64 numerator = (uint64)JH7110_OSC_RATE * (((uint64)fbdiv << 24) + frac);
	return (int64)(numerator / ((uint64)prediv * postdiv << 24));
}


bool
Jh7110ClockTree::_Parent(jh7110_clock_ref clock, jh7110_clock_ref& parent)
{
	const ClockDef* def = find_clock(clock);
	if (def == NULL)
		return false;

	uint32 index = 0;
	if (def->muxBits != 0) {
		uint32 value = fRegisters.Read(def->controller, def->id * 4);
		index = (value >> CLK_MUX_SHIFT) & ((1u << def->muxBits) - 1);
	}
	if (index >= def->parentCount)
		return false;

	parent = def->parents[index];
	return true;
}


bool
Jh7110ClockTree::IsEnabled(jh7110_clock_ref clock)
{
	if (clock.controller == JH7110_EXT || clock.controller == JH7110_PLL)
		return true;

	const ClockDef* def = find_clock(clock);
	if (def == NULL)
		return false;

	if ((def->flags & kGate) != 0
		&& (fRegisters.Read(def->controller, def->id * 4) & CLK_ENABLE) == 0)
		return false;

	jh7110_clock_ref parent;
	if (!_Parent(clock, parent))
		return false;
	return IsEnabled(parent);
}


status_t
Jh7110ClockTree::SetEnabled(jh7110_clock_ref clock, bool enable)
{
	if (clock.controller == JH7110_EXT || clock.controller == JH7110_PLL)
		return enable ? B_OK : B_NOT_ALLOWED;

	const ClockDef* def = find_clock(clock);
	if (def == NULL)
		return B_BAD_VALUE;

	if (enable) {
		// parents first; disabling never walks up, others may need them
		jh7110_clock_ref parent;
		if (!_Parent(clock, parent))
			return B_ERROR;
		status_t status = SetEnabled(parent, true);
		if (status != B_OK)
			return status;
	}

	if ((def->flags & kGate) == 0)
		return enable ? B_OK : B_NOT_ALLOWED;

	uint32 offset = def->id * 4;
	uint32 value = fRegisters.Read(def->controller, offset);
	if (enable)
		value |= CLK_ENABLE;
	else
		value &= ~CLK_ENABLE;
	fRegisters.Write(def->controller, offset, value);
	return B_OK;
}


int64
Jh7110ClockTree::GetRate(jh7110_clock_ref clock)
{
	switch (clock.controller) {
		case JH7110_EXT:
			// only the oscillator rate is fixed; the GMAC reference inputs
			// and the audio clock inputs depend on the board
			return clock.id == JH7110_EXT_OSC ? JH7110_OSC_RATE : B_UNSUPPORTED;
		case JH7110_PLL:
			return PllRate(clock.id);
	}

	const ClockDef* def = find_clock(clock);
	if (def == NULL)
		return B_BAD_VALUE;

	jh7110_clock_ref parent;
	if (!_Parent(clock, parent))
		return B_ERROR;

	int64 rate = GetRate(parent);
	if (rate < 0 || def->divBits == 0)
		return rate;

	uint32 divider = fRegisters.Read(def->controller, def->id * 4)
		& CLK_DIV_MASK & ((1u << def->divBits) - 1);
	if (divider == 0)
		return 0;
	if ((def->flags & kFrac) != 0)
		return (rate << FRAC_BITS) / divider;
	return rate / divider;
}


int64
Jh7110ClockTree::SetRate(jh7110_clock_ref clock, int64 rate)
{
	if (rate <= 0)
		return B_BAD_VALUE;
	if (clock.controller == JH7110_EXT || clock.controller == JH7110_PLL)
		return B_NOT_ALLOWED;

	const ClockDef* def = find_clock(clock);
	if (def == NULL)
		return B_BAD_VALUE;

	jh7110_clock_ref parent;
	if (!_Parent(clock, parent))
		return B_ERROR;

	if (def->divBits == 0) {
		// gates, inverters and muxes pass the request on to the current
		// parent; the parent selection itself is never changed
		return SetRate(parent, rate);
	}

	int64 parentRate = GetRate(parent);
	if (parentRate <= 0)
		return parentRate < 0 ? parentRate : B_ERROR;

	uint32 minDivider = 1;
	uint32 maxDivider = (1u << def->divBits) - 1;
	if ((def->flags & kFrac) != 0) {
		parentRate <<= FRAC_BITS;
		minDivider = FRAC_MIN;
		maxDivider = FRAC_MAX;
	}
	uint64 divider = (parentRate + rate / 2) / rate;
	if (divider < minDivider)
		divider = minDivider;
	if (divider > maxDivider)
		divider = maxDivider;

	uint32 offset = def->id * 4;
	uint32 value = fRegisters.Read(def->controller, offset);
	value = (value & ~CLK_DIV_MASK) | (uint32)divider;
	fRegisters.Write(def->controller, offset, value);

	return parentRate / (int64)divider;
}
