/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Where each thing sits in the card's description ROM.

	A Broadcom AXI part shadows its SPROM into ChipCommon at 0x800 and
	presents it sixteen bits at a time. The offsets below are byte offsets
	from the start of that shadow and are the revision 8 layout, which is
	what these parts carry; the revision itself is in the last word rather
	than at a fixed place.

	These were confirmed against a BCM43224 rather than taken on trust: the
	MAC address at 0x8c read back the one printed on the card, and the front
	end word at 0xae read 0x0325, which decodes to the internal power
	amplifier, detector range 4 and isolation 3 that this card is known to
	have.

	Only what the n-phy reads is written down here.
*/
#ifndef BRCMS_LINUX_SSB_SSB_REGS_H
#define BRCMS_LINUX_SSB_SSB_REGS_H


// The revision lives in the last word of the image, and only its low byte.
#define SSB_SPROM_REVISION_REV			0x00ff

#define SSB_SPROM8_BOARDREV				0x0082
#define SSB_SPROM8_BFLLO				0x0084	// board flags, bits 0-15
#define SSB_SPROM8_BFLHI				0x0086	// board flags, bits 16-31
#define SSB_SPROM8_BFL2LO				0x0088	// board flags 2, bits 0-15
#define SSB_SPROM8_BFL2HI				0x008a	// board flags 2, bits 16-31
#define SSB_SPROM8_IL0MAC				0x008c	// six bytes, big endian

// Which aerials are fitted, one bit each, 5 GHz in the high byte.
#define SSB_SPROM8_ANTAVAIL				0x009c
#define  SSB_SPROM8_ANTAVAIL_A			0xff00
#define  SSB_SPROM8_ANTAVAIL_A_SHIFT	8
#define  SSB_SPROM8_ANTAVAIL_BG			0x00ff
#define  SSB_SPROM8_ANTAVAIL_BG_SHIFT	0

// The transmit and receive chains, and what sort of aerial switch there is.
#define SSB_SPROM8_TXRXC				0x00a2
#define  SSB_SPROM8_TXRXC_TXCHAIN		0x000f
#define  SSB_SPROM8_TXRXC_TXCHAIN_SHIFT	0
#define  SSB_SPROM8_TXRXC_RXCHAIN		0x00f0
#define  SSB_SPROM8_TXRXC_RXCHAIN_SHIFT	4
#define  SSB_SPROM8_TXRXC_SWITCH		0xff00
#define  SSB_SPROM8_TXRXC_SWITCH_SHIFT	8

// The front end module, one word per band, packed the same way in each.
#define SSB_SPROM8_FEM2G				0x00ae
#define SSB_SPROM8_FEM5G				0x00b0
#define  SSB_SROM8_FEM_TSSIPOS			0x0001
#define  SSB_SROM8_FEM_TSSIPOS_SHIFT	0
#define  SSB_SROM8_FEM_EXTPA_GAIN		0x0006
#define  SSB_SROM8_FEM_EXTPA_GAIN_SHIFT	1
#define  SSB_SROM8_FEM_PDET_RANGE		0x00f8
#define  SSB_SROM8_FEM_PDET_RANGE_SHIFT	3
#define  SSB_SROM8_FEM_TR_ISO			0x0700
#define  SSB_SROM8_FEM_TR_ISO_SHIFT		8
#define  SSB_SROM8_FEM_ANTSWLUT			0xf800
#define  SSB_SROM8_FEM_ANTSWLUT_SHIFT	11

// The temperature sensor, and how far the die may drift before the phy
// calibrates itself again.
#define SSB_SPROM8_THERMAL				0x00b2
#define  SSB_SPROM8_THERMAL_OFFSET		0x00ff
#define  SSB_SPROM8_THERMAL_OFFSET_SHIFT	0
#define  SSB_SPROM8_THERMAL_TRESH		0xff00
#define  SSB_SPROM8_THERMAL_TRESH_SHIFT	8
#define SSB_SPROM8_TEMPDELTA			0x00bc
#define  SSB_SPROM8_TEMPDELTA_PHYCAL	0x00ff
#define  SSB_SPROM8_TEMPDELTA_PHYCAL_SHIFT	0

// Four blocks of transmit power settings, one per radio chain, each laid
// out the same way. The offsets below are from the start of a block.
#define SSB_SROM8_PWR_INFO_CORE0		0x00c0
#define SSB_SROM8_PWR_INFO_CORE1		0x00e0
#define SSB_SROM8_PWR_INFO_CORE2		0x0100
#define SSB_SROM8_PWR_INFO_CORE3		0x0120

#define SSB_SROM8_2G_MAXP_ITSSI			0x00
#define  SSB_SPROM8_2G_MAXP				0x00ff
#define  SSB_SPROM8_2G_ITSSI			0xff00
#define  SSB_SPROM8_2G_ITSSI_SHIFT		8
#define SSB_SROM8_2G_PA_0				0x02
#define SSB_SROM8_2G_PA_1				0x04
#define SSB_SROM8_2G_PA_2				0x06
#define SSB_SROM8_5G_MAXP_ITSSI			0x08
#define  SSB_SPROM8_5G_MAXP				0x00ff
#define  SSB_SPROM8_5G_ITSSI			0xff00
#define  SSB_SPROM8_5G_ITSSI_SHIFT		8
#define SSB_SPROM8_5GHL_MAXP			0x0a
#define  SSB_SPROM8_5GH_MAXP			0x00ff
#define  SSB_SPROM8_5GL_MAXP			0xff00
#define  SSB_SPROM8_5GL_MAXP_SHIFT		8
#define SSB_SROM8_5G_PA_0				0x0c
#define SSB_SROM8_5G_PA_1				0x0e
#define SSB_SROM8_5G_PA_2				0x10
#define SSB_SROM8_5GL_PA_0				0x12
#define SSB_SROM8_5GL_PA_1				0x14
#define SSB_SROM8_5GL_PA_2				0x16
#define SSB_SROM8_5GH_PA_0				0x18
#define SSB_SROM8_5GH_PA_1				0x1a
#define SSB_SROM8_5GH_PA_2				0x1c

// How much to back each rate off, in quarter dB. The OFDM entries are two
// words wide, the MCS ones eight.
#define SSB_SPROM8_CCK2GPO				0x0140
#define SSB_SPROM8_OFDM2GPO				0x0142
#define SSB_SPROM8_OFDM5GPO				0x0146
#define SSB_SPROM8_OFDM5GLPO			0x014a
#define SSB_SPROM8_OFDM5GHPO			0x014e
#define SSB_SPROM8_2G_MCSPO				0x0152
#define SSB_SPROM8_5G_MCSPO				0x0162
#define SSB_SPROM8_5GL_MCSPO			0x0172
#define SSB_SPROM8_5GH_MCSPO			0x0182
#define SSB_SPROM8_CDDPO				0x0192
#define SSB_SPROM8_STBCPO				0x0194
#define SSB_SPROM8_BW40PO				0x0196
#define SSB_SPROM8_BWDUPPO				0x0198


#endif	// BRCMS_LINUX_SSB_SSB_REGS_H
