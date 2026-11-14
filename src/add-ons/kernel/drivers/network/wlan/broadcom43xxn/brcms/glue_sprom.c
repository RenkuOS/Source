/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	What the card says it is.

	The imported phy layer does not probe the board; it reads a filled in
	struct ssb_sprom and trusts it. On a bcma part the SPROM is shadowed
	into ChipCommon, sixteen bits at a time, in the revision 8 layout, so
	this reads it out into the fields the n-phy actually looks at. The
	offsets are the ones in linux/ssb/ssb_regs.h, which is where the
	layout of this ROM is written down.
*/


#include "linux/types.h"

#include "linux/bcma/bcma.h"
#include "linux/ssb/ssb_regs.h"


// The shadow, and how much of it a revision 8 SPROM fills. Its revision is
// in the last word rather than at a fixed place.
#define SPROM_SHADOW			0x0800
#define SPROM_WORDS				220

#define TRACE(x...)				dprintf("broadcom43xxn: " x)


static inline u16
sprom(struct bcma_device* chipCommon, u16 offset)
{
	return bcma_read16(chipCommon, SPROM_SHADOW + offset);
}


static inline u8
field8(u16 value, u16 mask, int shift)
{
	return (u8)((value & mask) >> shift);
}


/*!	Reads one of the four per-core power blocks, which share a layout. */
static void
read_power_info(struct bcma_device* chipCommon, u16 base,
	struct ssb_sprom_core_pwr_info* out)
{
	u16 word = sprom(chipCommon, base + SSB_SROM8_2G_MAXP_ITSSI);
	out->maxpwr_2g = field8(word, SSB_SPROM8_2G_MAXP, 0);
	out->itssi_2g = field8(word, SSB_SPROM8_2G_ITSSI,
		SSB_SPROM8_2G_ITSSI_SHIFT);

	out->pa_2g[0] = sprom(chipCommon, base + SSB_SROM8_2G_PA_0);
	out->pa_2g[1] = sprom(chipCommon, base + SSB_SROM8_2G_PA_1);
	out->pa_2g[2] = sprom(chipCommon, base + SSB_SROM8_2G_PA_2);

	word = sprom(chipCommon, base + SSB_SROM8_5G_MAXP_ITSSI);
	out->maxpwr_5g = field8(word, SSB_SPROM8_5G_MAXP, 0);
	out->itssi_5g = field8(word, SSB_SPROM8_5G_ITSSI,
		SSB_SPROM8_5G_ITSSI_SHIFT);

	word = sprom(chipCommon, base + SSB_SPROM8_5GHL_MAXP);
	out->maxpwr_5gh = field8(word, SSB_SPROM8_5GH_MAXP, 0);
	out->maxpwr_5gl = field8(word, SSB_SPROM8_5GL_MAXP,
		SSB_SPROM8_5GL_MAXP_SHIFT);

	out->pa_5g[0] = sprom(chipCommon, base + SSB_SROM8_5G_PA_0);
	out->pa_5g[1] = sprom(chipCommon, base + SSB_SROM8_5G_PA_1);
	out->pa_5g[2] = sprom(chipCommon, base + SSB_SROM8_5G_PA_2);
	out->pa_5gl[0] = sprom(chipCommon, base + SSB_SROM8_5GL_PA_0);
	out->pa_5gl[1] = sprom(chipCommon, base + SSB_SROM8_5GL_PA_1);
	out->pa_5gl[2] = sprom(chipCommon, base + SSB_SROM8_5GL_PA_2);
	out->pa_5gh[0] = sprom(chipCommon, base + SSB_SROM8_5GH_PA_0);
	out->pa_5gh[1] = sprom(chipCommon, base + SSB_SROM8_5GH_PA_1);
	out->pa_5gh[2] = sprom(chipCommon, base + SSB_SROM8_5GH_PA_2);
}


/*!	Reads the front end module word, which says what is between the radio
	and the aerial: an internal or an external power amplifier, how far the
	power detector reaches, and how much isolation the switch gives.
*/
static void
read_fem(u16 word, u8* tssipos, u8* extpagain, u8* pdetrange, u8* triso,
	u8* antswlut)
{
	*tssipos = field8(word, SSB_SROM8_FEM_TSSIPOS,
		SSB_SROM8_FEM_TSSIPOS_SHIFT);
	*extpagain = field8(word, SSB_SROM8_FEM_EXTPA_GAIN,
		SSB_SROM8_FEM_EXTPA_GAIN_SHIFT);
	*pdetrange = field8(word, SSB_SROM8_FEM_PDET_RANGE,
		SSB_SROM8_FEM_PDET_RANGE_SHIFT);
	*triso = field8(word, SSB_SROM8_FEM_TR_ISO, SSB_SROM8_FEM_TR_ISO_SHIFT);
	*antswlut = field8(word, SSB_SROM8_FEM_ANTSWLUT,
		SSB_SROM8_FEM_ANTSWLUT_SHIFT);
}


void
brcms_glue_read_sprom(struct bcma_device* chipCommon, struct ssb_sprom* out)
{
	int i;
	u16 word;

	memset(out, 0, sizeof(*out));

	out->revision = sprom(chipCommon, (SPROM_WORDS - 1) * 2)
		& SSB_SPROM_REVISION_REV;

	out->board_rev = sprom(chipCommon, SSB_SPROM8_BOARDREV);
	out->boardflags_lo = sprom(chipCommon, SSB_SPROM8_BFLLO);
	out->boardflags_hi = sprom(chipCommon, SSB_SPROM8_BFLHI);
	out->boardflags2_lo = sprom(chipCommon, SSB_SPROM8_BFL2LO);
	out->boardflags2_hi = sprom(chipCommon, SSB_SPROM8_BFL2HI);
	out->boardflags = out->boardflags_lo | (out->boardflags_hi << 16);
	out->boardflags2 = out->boardflags2_lo | (out->boardflags2_hi << 16);

	for (i = 0; i < 3; i++) {
		word = sprom(chipCommon, SSB_SPROM8_IL0MAC + i * 2);
		out->il0mac[i * 2] = word >> 8;
		out->il0mac[i * 2 + 1] = word & 0xff;
	}

	word = sprom(chipCommon, SSB_SPROM8_ANTAVAIL);
	out->ant_available_a = field8(word, SSB_SPROM8_ANTAVAIL_A,
		SSB_SPROM8_ANTAVAIL_A_SHIFT);
	out->ant_available_bg = field8(word, SSB_SPROM8_ANTAVAIL_BG,
		SSB_SPROM8_ANTAVAIL_BG_SHIFT);

	word = sprom(chipCommon, SSB_SPROM8_TXRXC);
	out->antswitch = field8(word, SSB_SPROM8_TXRXC_SWITCH,
		SSB_SPROM8_TXRXC_SWITCH_SHIFT);

	read_fem(sprom(chipCommon, SSB_SPROM8_FEM2G), &out->fem.ghz2.tssipos,
		&out->fem.ghz2.extpa_gain, &out->fem.ghz2.pdet_range,
		&out->fem.ghz2.tr_iso, &out->fem.ghz2.antswlut);
	read_fem(sprom(chipCommon, SSB_SPROM8_FEM5G), &out->fem.ghz5.tssipos,
		&out->fem.ghz5.extpa_gain, &out->fem.ghz5.pdet_range,
		&out->fem.ghz5.tr_iso, &out->fem.ghz5.antswlut);

	word = sprom(chipCommon, SSB_SPROM8_THERMAL);
	out->tempoffset = field8(word, SSB_SPROM8_THERMAL_OFFSET,
		SSB_SPROM8_THERMAL_OFFSET_SHIFT);
	out->tempthresh = field8(word, SSB_SPROM8_THERMAL_TRESH,
		SSB_SPROM8_THERMAL_TRESH_SHIFT);

	word = sprom(chipCommon, SSB_SPROM8_TEMPDELTA);
	out->phycal_tempdelta = field8(word, SSB_SPROM8_TEMPDELTA_PHYCAL,
		SSB_SPROM8_TEMPDELTA_PHYCAL_SHIFT);

	read_power_info(chipCommon, SSB_SROM8_PWR_INFO_CORE0,
		&out->core_pwr_info[0]);
	read_power_info(chipCommon, SSB_SROM8_PWR_INFO_CORE1,
		&out->core_pwr_info[1]);
	read_power_info(chipCommon, SSB_SROM8_PWR_INFO_CORE2,
		&out->core_pwr_info[2]);
	read_power_info(chipCommon, SSB_SROM8_PWR_INFO_CORE3,
		&out->core_pwr_info[3]);

	// Per-rate power offsets. These are what the transmit power control
	// works from, so a zero here is not harmless.
	out->cck2gpo = sprom(chipCommon, SSB_SPROM8_CCK2GPO);
	out->ofdm2gpo = sprom(chipCommon, SSB_SPROM8_OFDM2GPO)
		| ((u32)sprom(chipCommon, SSB_SPROM8_OFDM2GPO + 2) << 16);
	out->ofdm5gpo = sprom(chipCommon, SSB_SPROM8_OFDM5GPO)
		| ((u32)sprom(chipCommon, SSB_SPROM8_OFDM5GPO + 2) << 16);
	out->ofdm5glpo = sprom(chipCommon, SSB_SPROM8_OFDM5GLPO)
		| ((u32)sprom(chipCommon, SSB_SPROM8_OFDM5GLPO + 2) << 16);
	out->ofdm5ghpo = sprom(chipCommon, SSB_SPROM8_OFDM5GHPO)
		| ((u32)sprom(chipCommon, SSB_SPROM8_OFDM5GHPO + 2) << 16);

	for (i = 0; i < 8; i++) {
		out->mcs2gpo[i] = sprom(chipCommon, SSB_SPROM8_2G_MCSPO + i * 2);
		out->mcs5gpo[i] = sprom(chipCommon, SSB_SPROM8_5G_MCSPO + i * 2);
		out->mcs5glpo[i] = sprom(chipCommon, SSB_SPROM8_5GL_MCSPO + i * 2);
		out->mcs5ghpo[i] = sprom(chipCommon, SSB_SPROM8_5GH_MCSPO + i * 2);
	}

	out->cddpo = sprom(chipCommon, SSB_SPROM8_CDDPO);
	out->stbcpo = sprom(chipCommon, SSB_SPROM8_STBCPO);
	out->bw40po = sprom(chipCommon, SSB_SPROM8_BW40PO);
	out->bwduppo = sprom(chipCommon, SSB_SPROM8_BWDUPPO);

	TRACE("sprom: rev %u board rev %u flags %#lx/%#lx\n", out->revision,
		out->board_rev, (unsigned long)out->boardflags,
		(unsigned long)out->boardflags2);
	TRACE("sprom: fem2g tssipos %u extpagain %u pdetrange %u triso %u "
		"antswlut %u\n", out->fem.ghz2.tssipos, out->fem.ghz2.extpa_gain,
		out->fem.ghz2.pdet_range, out->fem.ghz2.tr_iso,
		out->fem.ghz2.antswlut);
	// The aerials and the power tables are read every boot and have not
	// varied; they are summarised rather than listed, and anything out of
	// the ordinary still shows.
	TRACE("sprom: antavail bg %u a %u, maxpwr2g %u, itssi %u, cck2gpo "
		"%#x\n", out->ant_available_bg, out->ant_available_a,
		out->core_pwr_info[0].maxpwr_2g, out->core_pwr_info[0].itssi_2g,
		out->cck2gpo);
}
