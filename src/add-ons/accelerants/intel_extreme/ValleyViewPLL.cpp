/*
 * Copyright 2006-2016, Intel Corporation.
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


//!	The display PLL and the DPIO PHY of Valleyview (Bay Trail) for HDMI.
// Both sit behind the IOSF sideband of the display. The divider search, the
// PLL and PHY sequences and their magic values come from Linux's i915
// (intel_dpll.c, intel_dpio_phy.c, g4x_hdmi.c), which is MIT licensed.


#include "ValleyViewPLL.h"

#include "accelerant.h"

#include <KernelExport.h>


#undef TRACE
#define TRACE_VLV_PLL
#ifdef TRACE_VLV_PLL
#	define TRACE(x...) _sPrintf("intel_extreme: " x)
#else
#	define TRACE(x...)
#endif

#define ERROR(x...) _sPrintf("intel_extreme: " x)


// IOSF sideband of the display
#define IOSF_DOORBELL			(REGS_FLAT | (VLV_DISPLAY_BASE + 0x2100))
#define IOSF_DATA				(REGS_FLAT | (VLV_DISPLAY_BASE + 0x2104))
#define IOSF_ADDRESS			(REGS_FLAT | (VLV_DISPLAY_BASE + 0x2108))
#define IOSF_BUSY				(1 << 0)
#define IOSF_BYTE_ENABLES		(0xf << 4)
#define IOSF_PORT_DPIO			(0x12 << 8)
#define IOSF_OPCODE_READ		(0x00 << 16)
#define IOSF_OPCODE_WRITE		(0x01 << 16)

// DPIO registers; ch is the PLL channel of a pipe or the PHY channel of a
// port.
#define DPIO_PLL(ch, dw)		(0x8000 + (ch) * 0x20 + (dw) * 4)
#define DPIO_REF_DW11			(0x80a0 + (11 - 8) * 4)
#define DPIO_CMN_DW0			0x8100
#define DPIO_PCS(ch, dw)		(0x8200 + (ch) * 0x200 + (dw) * 4)
#define DPIO_PCS_DW17_BCAST		(0xc000 + 17 * 4)
#define DPIO_TX(ch, dw)			(0x8280 + (ch) * 0x200 + (dw) * 4)
#define DPIO_TX_LANE(ch, lane, dw) \
	(0x80 + (ch) * 0x2400 + (lane) * 0x200 + (dw) * 4)

#define DPIO_S1_DIV_HDMIDP		(1 << 28)
#define DPIO_K_DIV_1			(1 << 24)
#define DPIO_P1_SHIFT			21
#define DPIO_P2_SHIFT			16
#define DPIO_N_SHIFT			12
#define DPIO_ENABLE_CALIBRATION	(1 << 11)
#define DPIO_M1_SHIFT			8

#define DPIO_PCS_TX_LANE2_RESET	(1 << 16)
#define DPIO_PCS_TX_LANE1_RESET	(1 << 7)
#define DPIO_PCS_CLK_DEFAULTS	((1 << 22) | (1 << 21) | (1 << 6) | (1 << 5))
#define DPIO_PCS_USEDCLKCHANNEL	(1 << 21)
#define DPIO_PCS_USEDCLKCHANNEL_OVERRIDE (1 << 20)
#define DPIO_TX_OCALINIT_EN		(1u << 31)

#define DPLL_VCO_ENABLE			(1u << 31)
#define DPLL_EXT_BUFFER_ENABLE	(1 << 30)
#define DPLL_REF_CLK_ENABLE		(1 << 29)
#define DPLL_VGA_MODE_DISABLE	(1 << 28)
#define DPLL_LOCK				(1 << 15)
#define DPLL_INTEGRATED_CRI_CLK	(1 << 14)
#define DPLL_INTEGRATED_REF_CLK	(1 << 13)

#define PIPE_STATE_ENABLED		(1 << 30)

#define REFERENCE_CLOCK			100000


struct vlv_dividers {
	uint32	n, m1, m2, p1, p2;
	uint32	dot;
};


static status_t
iosf_wait()
{
	for (int32 i = 0; i < 1000; i++) {
		if ((read32(IOSF_DOORBELL) & IOSF_BUSY) == 0)
			return B_OK;
		spin(10);
	}
	ERROR("%s: IOSF sideband stays busy\n", __func__);
	return B_TIMED_OUT;
}


static uint32
dpio_read(uint32 reg)
{
	iosf_wait();
	write32(IOSF_ADDRESS, reg);
	write32(IOSF_DATA, 0);
	write32(IOSF_DOORBELL, IOSF_OPCODE_READ | IOSF_PORT_DPIO
		| IOSF_BYTE_ENABLES | IOSF_BUSY);
	iosf_wait();
	return read32(IOSF_DATA);
}


static void
dpio_write(uint32 reg, uint32 value)
{
	iosf_wait();
	write32(IOSF_ADDRESS, reg);
	write32(IOSF_DATA, value);
	write32(IOSF_DOORBELL, IOSF_OPCODE_WRITE | IOSF_PORT_DPIO
		| IOSF_BYTE_ENABLES | IOSF_BUSY);
	iosf_wait();
}


/*!	vlv_find_best_dpll() of Linux: dot = 100 MHz * m1 * m2 / n
	/ (p1 * p2 * 5), the VCO between 4 and 6 GHz. Prefers a small n, then a
	big p when the error is below 100 ppm.
*/
static bool
find_dividers(uint32 target, vlv_dividers& best)
{
	uint32 bestPpm = 1000000;
	bool found = false;
	vlv_dividers clock;

	for (clock.n = 1; clock.n <= 5; clock.n++) {
			// up to the 19.2 MHz minimum update rate
		for (clock.p1 = 3; clock.p1 >= 2; clock.p1--) {
			for (clock.p2 = 20; clock.p2 >= 2;
					clock.p2 -= clock.p2 > 10 ? 2 : 1) {
				uint32 p = clock.p1 * clock.p2 * 5;
				for (clock.m1 = 2; clock.m1 <= 3; clock.m1++) {
					clock.m2 = ((uint64)target * p * clock.n
						+ REFERENCE_CLOCK * clock.m1 / 2)
						/ (REFERENCE_CLOCK * clock.m1);
					if (clock.m2 < 11 || clock.m2 > 156)
						continue;

					uint32 vco = ((uint64)REFERENCE_CLOCK * clock.m1 * clock.m2
						+ clock.n / 2) / clock.n;
					clock.dot = (vco + p / 2) / p;
					if (vco < 4000000 || vco > 6000000
						|| clock.dot < 25000 || clock.dot > 270000) {
						continue;
					}

					uint32 difference = clock.dot > target
						? clock.dot - target : target - clock.dot;
					uint32 ppm = (uint64)1000000 * difference / target;
					if (ppm < 100 && p > best.p1 * best.p2 * 5)
						ppm = 0;
					else if (ppm + 10 >= bestPpm)
						continue;

					best = clock;
					bestPpm = ppm;
					found = true;
				}
			}
		}
	}
	return found;
}


static void
prepare_pll(uint32 pipe, uint32 dividers)
{
	if (pipe == 1) {
		// PLL B's opamp always calibrates to its maximum; set a sane value
		dpio_write(DPIO_PLL(1, 17),
			(dpio_read(DPIO_PLL(1, 17)) & 0xffffff00) | 0x30);
		dpio_write(DPIO_REF_DW11,
			(dpio_read(DPIO_REF_DW11) & 0x00ffffff) | 0x8c000000);
		dpio_write(DPIO_PLL(1, 17), dpio_read(DPIO_PLL(1, 17)) & 0xffffff00);
		dpio_write(DPIO_REF_DW11,
			(dpio_read(DPIO_REF_DW11) & 0x00ffffff) | 0xb0000000);
	}

	dpio_write(DPIO_PCS_DW17_BCAST, 0x0100000f);
	dpio_write(DPIO_PLL(pipe, 16), dpio_read(DPIO_PLL(pipe, 16)) & 0x00ffffff);
	dpio_write(DPIO_CMN_DW0, 0x610);

	dpio_write(DPIO_PLL(pipe, 3), dividers);
	dpio_write(DPIO_PLL(pipe, 3), dividers | DPIO_ENABLE_CALIBRATION);

	// HDMI loop filter and the bend clock source
	dpio_write(DPIO_PLL(pipe, 18), 0x009f0003);
	dpio_write(DPIO_PLL(pipe, 5), pipe == 0 ? 0x0df70000 : 0x0df40000);
	dpio_write(DPIO_PLL(pipe, 7),
		(dpio_read(DPIO_PLL(pipe, 7)) & 0x0000ff00) | 0x01c00000);
	dpio_write(DPIO_PLL(pipe, 19), 0x87871000);
}


/*!	Runs the HDMI port of \a portRegister on \a pipe at \a pixelClock (kHz),
	in the order of Linux's modeset: port and pipe off, PHY lanes in reset,
	PLL off, the PHY's lane defaults, the PLL, then the PHY's clock channel
	and HDMI signal levels and the port again. The caller programs the
	timings and enables the pipe.
*/
status_t
vlv_set_hdmi_clock(pipe_index pipeIndex, port_index portIndex,
	addr_t portRegister, uint32 pixelClock)
{
	vlv_dividers clock = {};
	if (!find_dividers(pixelClock, clock)) {
		ERROR("%s: no PLL setting for %" B_PRIu32 " kHz\n", __func__,
			pixelClock);
		return B_BAD_VALUE;
	}

	uint32 pipe = pipeIndex == INTEL_PIPE_B ? 1 : 0;
	uint32 channel = portIndex == INTEL_PORT_C ? 1 : 0;
	uint32 dpllRegister = pipe == 0 ? INTEL_DISPLAY_A_PLL : INTEL_DISPLAY_B_PLL;
	uint32 dividers = DPIO_S1_DIV_HDMIDP | DPIO_K_DIV_1
		| clock.p1 << DPIO_P1_SHIFT | clock.p2 << DPIO_P2_SHIFT
		| clock.n << DPIO_N_SHIFT | clock.m1 << DPIO_M1_SHIFT | clock.m2;

	TRACE("%s: %" B_PRIu32 " kHz: n %" B_PRIu32 " m1 %" B_PRIu32 " m2 %"
		B_PRIu32 " p1 %" B_PRIu32 " p2 %" B_PRIu32 " = %" B_PRIu32 " kHz\n",
		__func__, pixelClock, clock.n, clock.m1, clock.m2, clock.p1, clock.p2,
		clock.dot);

	// Nothing to do when the PLL already runs at that clock, as after boot;
	// the firmware may have picked other dividers for it.
	if ((read32(dpllRegister) & DPLL_VCO_ENABLE) != 0) {
		uint32 current = dpio_read(DPIO_PLL(pipe, 3));
		uint32 n = (current >> DPIO_N_SHIFT) & 0xf;
		uint32 p = ((current >> DPIO_P1_SHIFT) & 0x7)
			* ((current >> DPIO_P2_SHIFT) & 0x1f) * 5;
		if (n != 0 && p != 0) {
			uint32 vco = ((uint64)REFERENCE_CLOCK
				* ((current >> DPIO_M1_SHIFT) & 0x7) * (current & 0xff)
				+ n / 2) / n;
			if ((vco + p / 2) / p == clock.dot)
				return B_OK;
		}
	}

	uint32 port = read32(portRegister);
	write32(portRegister, port & ~DISPLAY_MONITOR_PORT_ENABLED);

	uint32 pipeControl = INTEL_DISPLAY_A_PIPE_CONTROL
		+ pipe * INTEL_DISPLAY_OFFSET;
	write32(pipeControl, read32(pipeControl) & ~INTEL_PIPE_ENABLED);
	for (int32 i = 0; i < 100
		&& (read32(pipeControl) & PIPE_STATE_ENABLED) != 0; i++) {
		snooze(1000);
	}

	dpio_write(DPIO_PCS(channel, 0), 0);
	dpio_write(DPIO_PCS(channel, 1), 0x00e00060);

	uint32 dpll = DPLL_INTEGRATED_REF_CLK | DPLL_REF_CLK_ENABLE
		| DPLL_VGA_MODE_DISABLE | (pipe != 0 ? DPLL_INTEGRATED_CRI_CLK : 0);
	write32(dpllRegister, dpll);

	dpio_write(DPIO_PCS(channel, 0),
		DPIO_PCS_TX_LANE2_RESET | DPIO_PCS_TX_LANE1_RESET);
	dpio_write(DPIO_PCS(channel, 1), DPIO_PCS_CLK_DEFAULTS);
	dpio_write(DPIO_PCS(channel, 12), 0x00750f00);
	dpio_write(DPIO_TX(channel, 11), 0x00001500);
	dpio_write(DPIO_TX(channel, 14), 0x40400000);

	prepare_pll(pipe, dividers);
	write32(dpllRegister, dpll | DPLL_VCO_ENABLE | DPLL_EXT_BUFFER_ENABLE);
	read32(dpllRegister);
	spin(150);
	bool locked = false;
	for (int32 i = 0; i < 10 && !locked; i++) {
		locked = (read32(dpllRegister) & DPLL_LOCK) != 0;
		if (!locked)
			spin(100);
	}
	if (!locked)
		ERROR("%s: DPLL %c failed to lock\n", __func__, (char)('A' + pipe));
	write32(INTEL_DISPLAY_A_PLL_MD + pipe * 4, 0);

	dpio_write(DPIO_PCS(channel, 8), DPIO_PCS_USEDCLKCHANNEL_OVERRIDE
		| (pipe != 0 ? DPIO_PCS_USEDCLKCHANNEL : 0) | 0xc4);
	dpio_write(DPIO_PCS(channel, 14), 0x00760018);
	dpio_write(DPIO_PCS(channel, 23), 0x00400888);

	// HDMI 1.0 V, -2 dB
	dpio_write(DPIO_TX(channel, 5), 0);
	dpio_write(DPIO_TX(channel, 4), 0x2b245f5f);
	dpio_write(DPIO_TX(channel, 2), 0x5578b83a);
	dpio_write(DPIO_TX(channel, 3), 0x0c782040);
	dpio_write(DPIO_TX_LANE(channel, 3, 4), 0x2b247878);
	dpio_write(DPIO_PCS(channel, 11), 0x00030000);
	dpio_write(DPIO_PCS(channel, 9), 0x00002000);
	dpio_write(DPIO_TX(channel, 5), DPIO_TX_OCALINIT_EN);

	write32(portRegister, port | DISPLAY_MONITOR_PORT_ENABLED);
	read32(portRegister);
	return locked ? B_OK : B_ERROR;
}
