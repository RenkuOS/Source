/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	What the card says it is.

	A Broadcom wireless card carries a small ROM describing the board it is
	soldered to: which aerials are fitted, what sits between the radio and
	them, how much power each rate may be sent at, and where the temperature
	sensor reads zero. The phy layer does not probe for any of this; it
	reads it out of the structure below and trusts it.

	The structure is named after the older ssb bus because Broadcom used the
	same ROM layout across both, and the imported sources reach for it by
	that name. Only the fields the n-phy actually reads are here: the ROM
	holds a good deal more that nothing in this driver looks at.

	glue_sprom.c fills it; linux/ssb/ssb_regs.h says where each field lives.
*/
#ifndef BRCMS_LINUX_SSB_SSB_H
#define BRCMS_LINUX_SSB_SSB_H


#include "linux/types.h"


/*!	The transmit power settings for one radio chain.

	A two by two part has two of these filled in. The amplifier
	coefficients are a quadratic fit of power against the setting, and the
	idle TSSI is what the power detector reads with nothing being sent.
*/
struct ssb_sprom_core_pwr_info {
	u8		itssi_2g, itssi_5g;
	u8		maxpwr_2g, maxpwr_5gl, maxpwr_5g, maxpwr_5gh;
	u16		pa_2g[4], pa_5gl[4], pa_5g[4], pa_5gh[4];
};


struct ssb_sprom {
	u8		revision;
	u8		il0mac[6];			// the card's own MAC address

	u16		board_rev;
	u32		boardflags;
	u32		boardflags2;
	u16		boardflags_lo;
	u16		boardflags_hi;
	u16		boardflags2_lo;
	u16		boardflags2_hi;

	u8		ant_available_a;	// which 5 GHz aerials are fitted
	u8		ant_available_bg;	// which 2.4 GHz aerials are fitted
	u8		antswitch;			// what kind of aerial switch the board has

	struct ssb_sprom_core_pwr_info core_pwr_info[4];

	/*!	The front end module: what is between the radio and the aerial.

		extpa_gain says whether the power amplifier is the one inside the
		chip or one on the board, which changes the whole transmit
		calibration; pdet_range is how far the power detector reaches,
		tr_iso how much the transmit and receive switch isolates, and
		antswlut which of the aerial switch tables applies.
	*/
	struct {
		struct {
			u8 tssipos, extpa_gain, pdet_range, tr_iso, antswlut;
		} ghz2;
		struct {
			u8 tssipos, extpa_gain, pdet_range, tr_iso, antswlut;
		} ghz5;
	} fem;

	// How much to back the transmitter off for each rate, in quarter dB.
	u16		cck2gpo;
	u32		ofdm2gpo;
	u32		ofdm5glpo;
	u32		ofdm5gpo;
	u32		ofdm5ghpo;
	u16		mcs2gpo[8];
	u16		mcs5gpo[8];
	u16		mcs5glpo[8];
	u16		mcs5ghpo[8];
	u16		cddpo;				// cyclic delay diversity
	u16		stbcpo;				// space time block coding
	u16		bw40po;				// forty megahertz wide
	u16		bwduppo;			// the duplicated half of a forty

	// The temperature sensor: where it reads zero, when to recalibrate,
	// and how far the die may drift before the phy calibrates again.
	u8		tempthresh;
	u8		tempoffset;
	u8		phycal_tempdelta;
};


#endif	// BRCMS_LINUX_SSB_SSB_H
