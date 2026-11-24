/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef JH7110_CLOCK_TREE_H
#define JH7110_CLOCK_TREE_H


#include <SupportDefs.h>


/*!	Clock tree of the StarFive JH7110 system, always-on and STG clock
	generators, without any kernel dependency (the register access is
	provided by the caller), so that it can be tested on the build host.

	Every generator has one 32 bit register per clock at "id * 4":
	bit 31 enable (gate), bit 30 invert, bits 29-24 parent select, bits 23-0
	divider. Not every clock has every field. The PLLs are in the SYS
	syscon.
*/

enum jh7110_controller {
	JH7110_EXT = 0,		// external fixed clocks, see jh7110_ext_clock
	JH7110_PLL,			// PLL0..2 (SYS syscon)
	JH7110_SYS,			// syscrg, 0x13020000
	JH7110_AON,			// aoncrg, 0x17000000
	JH7110_STG,			// stgcrg, 0x10230000
	JH7110_CONTROLLER_COUNT
};

enum jh7110_ext_clock {
	JH7110_EXT_OSC = 0,			// 24 MHz
	JH7110_EXT_GMAC0_RMII_REFIN,
	JH7110_EXT_GMAC0_RGMII_RXIN,
	JH7110_EXT_GMAC1_RMII_REFIN,
	JH7110_EXT_GMAC1_RGMII_RXIN,
	JH7110_EXT_MCLK_EXT,		// audio master clock input, if the board has one
	JH7110_EXT_I2STX_BCLK_EXT,	// I2S TX bit and frame clock inputs (slave)
	JH7110_EXT_I2STX_LRCK_EXT,
	JH7110_EXT_LAST = JH7110_EXT_I2STX_LRCK_EXT
};

#define JH7110_OSC_RATE		24000000LL


struct jh7110_clock_ref {
	uint8	controller;
	uint8	id;
};


class Jh7110RegisterAccess {
public:
	virtual	uint32		Read(uint32 controller, uint32 offset) = 0;
	virtual	void		Write(uint32 controller, uint32 offset,
							uint32 value) = 0;

protected:
						~Jh7110RegisterAccess() = default;
};


class Jh7110ClockTree {
public:
						Jh7110ClockTree(Jh7110RegisterAccess& registers);

	static	bool		IsKnown(jh7110_clock_ref clock);
	static	const char*	Name(jh7110_clock_ref clock);

			bool		IsEnabled(jh7110_clock_ref clock);
			status_t	SetEnabled(jh7110_clock_ref clock, bool enable);
			int64		GetRate(jh7110_clock_ref clock);
			int64		SetRate(jh7110_clock_ref clock, int64 rate);

			int64		PllRate(uint32 pll);

private:
			bool		_Parent(jh7110_clock_ref clock,
							jh7110_clock_ref& parent);

private:
			Jh7110RegisterAccess& fRegisters;
};


#endif	// JH7110_CLOCK_TREE_H
