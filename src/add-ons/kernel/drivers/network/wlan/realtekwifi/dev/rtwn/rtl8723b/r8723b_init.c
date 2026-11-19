/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* Copyright(c) 2018-2019  Realtek Corporation
 * Copyright(c) 2026  Realtek Corporation
 * Copyright(c) Michael Straube <straube.linux@gmail.com>
 * Copyright(c) 2024-2026 Luka Gejak <luka.gejak@linux.dev>
 * Copyright 2024 Fiona Klute
 * Copyright (C) 2021 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 * Copyright (C) 2021 Jernej Skrabec <jernej.skrabec@gmail.com>
 *
 * Initialization of the RTL8723B, from the rtw88 driver (rtw8723b.c,
 * rtw8723x.c and sdio.c), used here under the BSD-3-Clause license.
 * Changes for RenkuOS by Akakor, 2026: rewritten for the rtwn driver and
 * split over its chip methods. The steps that rtwn_init() already does
 * (LLT, queue pages, Rx filter, EDCA, beacon registers, retry limits) are
 * left to it.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "opt_wlan.h"

#include <sys/param.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>
#include <sys/bus.h>
#include <sys/endian.h>

#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>
#include <dev/rtwn/if_rtwn_debug.h>

#include <dev/rtwn/rtl8192c/r92c.h>
#include <dev/rtwn/rtl8192c/r92c_reg.h>
#include <dev/rtwn/rtl8192e/r92e_var.h>

#include <dev/rtwn/rtl8723b/r8723b.h>
#include <dev/rtwn/rtl8723b/r8723b_reg.h>


void
r8723b_init_bb(struct rtwn_softc *sc)
{
	struct r92e_softc *rs = sc->sc_priv;
	uint8_t xtal;
	int i, j;

	/* Enable BB and RF. */
	rtwn_setbits_2(sc, R92C_SYS_FUNC_EN, 0, R8723B_SYS_FUNC_EN_25_1 |
	    R92C_SYS_FUNC_EN_BB_GLB_RST | R92C_SYS_FUNC_EN_BBRSTB);

	/* Antenna switch to the WLAN path (S0) for SDIO. */
	rtwn_bb_write(sc, R8723B_BB_SEL_BTG, 0x280);

	/* A full write: keeping stray bits can wedge the RF bus. */
	rtwn_write_1(sc, R92C_RF_CTRL,
	    R92C_RF_CTRL_EN | R92C_RF_CTRL_RSTB | R92C_RF_CTRL_SDMRSTB);
	rtwn_delay(sc, 1000);
	rtwn_rf_write(sc, 0, R8723B_RF_WLINT, 0x0780);
	/* PPLL, PCIEA, DIO_PCIE, BB_GLB_RST and BBRSTB. */
	rtwn_write_1(sc, R92C_SYS_FUNC_EN, 0xe3);
	rtwn_write_1(sc, R8723B_AFE_CTRL1 + 1, 0x80);

	xtal = rs->crystalcap & 0x3f;
	rtwn_setbits_4(sc, R8723B_AFE_CTRL3, R8723B_AFE_CTRL3_XTAL_M,
	    (uint32_t)(xtal | xtal << 6) << R8723B_AFE_CTRL3_XTAL_S);

	/* Write BB initialization values. */
	for (i = 0; i < sc->bb_size; i++) {
		const struct rtwn_bb_prog *bb_prog = &sc->bb_prog[i];

		for (j = 0; j < bb_prog->count; j++) {
			rtwn_bb_write(sc, bb_prog->reg[j], bb_prog->val[j]);
			rtwn_delay(sc, 1);
		}
	}

	/* Write AGC values. */
	for (i = 0; i < sc->agc_size; i++) {
		const struct rtwn_agc_prog *agc_prog = &sc->agc_prog[i];

		for (j = 0; j < agc_prog->count; j++) {
			rtwn_bb_write(sc, R92C_OFDM0_AGCRSSITABLE,
			    agc_prog->val[j]);
			rtwn_delay(sc, 1);
		}
	}
	/* The end of the AGC table of rtw88, on other registers. */
	rtwn_bb_write(sc, R8723B_OFDM0_XAAGC1, 0x69553422);
	rtwn_delay(sc, 1);
	rtwn_bb_write(sc, R8723B_OFDM0_XAAGC1, 0x69553420);
	rtwn_delay(sc, 1);
	rtwn_bb_write(sc, R92C_HSSI_PARAM2(0), 0x00390204);
	rtwn_delay(sc, 1);

	if (rtwn_bb_read(sc, R92C_HSSI_PARAM2(0)) & R92C_HSSI_PARAM2_CCK_HIPWR)
		sc->sc_flags |= RTWN_FLAG_CCK_HIPWR;
}


static void
r8723b_lck(struct rtwn_softc *sc)
{
	uint32_t lc_cal;
	uint8_t ctx;
	int ntries;

	ctx = rtwn_read_1(sc, R8723B_CTX);
	if (ctx & R8723B_CTX_TYPE_M)
		rtwn_write_1(sc, R8723B_CTX, ctx & ~R8723B_CTX_TYPE_M);
	else
		rtwn_write_1(sc, R92C_TXPAUSE, 0xff);

	lc_cal = rtwn_rf_read(sc, 0, R92C_RF_CHNLBW);
	rtwn_rf_write(sc, 0, R92C_RF_CHNLBW, lc_cal | R92C_RF_CHNLBW_LCSTART);
	for (ntries = 0; ntries < 100; ntries++) {
		rtwn_delay(sc, 10000);
		if (!(rtwn_rf_read(sc, 0, R92C_RF_CHNLBW) &
		    R92C_RF_CHNLBW_LCSTART))
			break;
	}
	if (ntries == 100)
		device_printf(sc->sc_dev, "LC calibration timed out\n");
	rtwn_rf_write(sc, 0, R92C_RF_CHNLBW, lc_cal);

	if (ctx & R8723B_CTX_TYPE_M)
		rtwn_write_1(sc, R8723B_CTX, ctx);
	else
		rtwn_write_1(sc, R92C_TXPAUSE, 0);
}


/*!	The vendor driver switches the PFD LDO on around every LC calibration. */
void
r8723b_lc_calib(struct rtwn_softc *sc)
{
	rtwn_rf_write(sc, 0, R8723B_RF_SYN_PFD, 0xdfbe0);
	r8723b_lck(sc);
	rtwn_rf_write(sc, 0, R8723B_RF_SYN_PFD, 0xdffe0);
}


/*!	RF write through the LSSI parameter register, as rtw88 does
	(rtw_phy_write_rf_reg_sipi). The RTL8192E method that rtwn has toggles
	bit 17 of 0x818 around each write, which is something else on the
	RTL8723B and stays set afterwards; with it set, no OFDM frame decodes.
*/
void
r8723b_rf_write(struct rtwn_softc *sc, int chain, uint8_t addr, uint32_t val)
{
	rtwn_bb_write(sc, R92C_LSSI_PARAM(chain),
	    ((uint32_t)addr << 20) | (val & 0x000fffff));
	rtwn_delay(sc, 13);
}


void
r8723b_init_rf(struct rtwn_softc *sc)
{
	struct r92e_softc *rs = sc->sc_priv;
	uint32_t type;

	/* Save RF_ENV control type. */
	type = rtwn_bb_read(sc, R92C_FPGA0_RFIFACESW(0)) & 0x10;

	/* Set RF_ENV enable and output high. */
	rtwn_bb_setbits(sc, R92C_FPGA0_RFIFACEOE(0), 0, 0x100000);
	rtwn_delay(sc, 1);
	rtwn_bb_setbits(sc, R92C_FPGA0_RFIFACEOE(0), 0, 0x10);
	rtwn_delay(sc, 1);
	/* Set address and data lengths of RF registers. */
	rtwn_bb_setbits(sc, R92C_HSSI_PARAM2(0),
	    R92C_HSSI_PARAM2_ADDR_LENGTH, 0);
	rtwn_delay(sc, 1);
	rtwn_bb_setbits(sc, R92C_HSSI_PARAM2(0),
	    R92C_HSSI_PARAM2_DATA_LENGTH, 0);
	rtwn_delay(sc, 1);

	/* Write RF initialization values. */
	r92c_init_rf_chain(sc, &sc->rf_prog[0], 0);

	/* Restore RF_ENV control type. */
	rtwn_bb_setbits(sc, R92C_FPGA0_RFIFACESW(0), 0x10, type);

	/* The LC calibration belongs to the RF configuration. */
	rtwn_rf_write(sc, 0, R8723B_RF_SYN_PFD, 0xdfbe0);
	rtwn_rf_write(sc, 0, R92C_RF_CHNLBW, 0x8c01);
	rtwn_delay(sc, 200000);
	rtwn_rf_write(sc, 0, R8723B_RF_SYN_PFD, 0xdffe0);

	/* Cache RF register CHNLBW. */
	rs->rf_chnlbw[0] = rtwn_rf_read(sc, 0, R92C_RF_CHNLBW);

	/* Turn CCK and OFDM blocks on. */
	rtwn_bb_setbits(sc, R92C_FPGA0_RFMOD, 0,
	    R92C_RFMOD_CCK_EN | R92C_RFMOD_OFDM_EN);
}


int
r8723b_set_page_size(struct rtwn_softc *sc)
{
	/* 128 byte pages for both directions. */
	return (rtwn_write_1(sc, R8723B_PBP, 0x11));
}


/*!	Receive aggregation as the rtw88 SDIO driver sets it up for this chip:
	up to 6 pages or a timeout of 6, in DMA mode with three bursts.
*/
void
r8723b_init_rx_agg(struct rtwn_softc *sc)
{
	rtwn_setbits_4(sc, R8723B_RXDMA_AGG_PG_TH, 0,
	    R8723B_RXDMA_AGG_PRE_CALC);
	rtwn_setbits_1(sc, R92C_TRXDMA_CTRL, 0,
	    R92C_TRXDMA_CTRL_RXDMA_AGG_EN);
	rtwn_write_2(sc, R8723B_RXDMA_AGG_PG_TH, 0x06 | (0x06 << 8));
	rtwn_write_1(sc, R8723B_RXDMA_MODE, R8723B_RXDMA_MODE_DMA |
	    (3 << R8723B_RXDMA_MODE_BURST_CNT_S));
}


void
r8723b_post_init(struct rtwn_softc *sc)
{
	/*
	 * Falling edge triggered interrupts and GPIO9 interrupt mode: the
	 * power on sequence sets them before the firmware runs, so again.
	 */
	rtwn_setbits_4(sc, R8723B_GPIO_INTM, 0, 0x200);
	rtwn_setbits_4(sc, R8723B_GPIO_PIN_CTRL_2, 0, 0x02000000);
	rtwn_setbits_2(sc, R92C_APS_FSMCO, 0, 0x0010);	/* EN_PDN */
	rtwn_setbits_2(sc, R8723B_PWR_DATA, 0,
	    R8723B_PWR_DATA_EEPRPAD_RFE_CTRL_EN);
	rtwn_write_1(sc, R8723B_EARLY_MODE_CONTROL, 0);

	/* Every MACID may send. */
	rtwn_write_4(sc, R8723B_MACID_PKT_DROP0, 0);
	rtwn_write_4(sc, R8723B_MACID_PKT_SLEEP, 0);

	/* MAC settings of rtw8723x_mac_init(). */
	rtwn_write_4(sc, R8723B_INT_MIG, 0);
	rtwn_write_4(sc, R8723B_MCUTST_1, 0);
	rtwn_write_1(sc, R8723B_MISC_CTRL, 0x03);	/* no second CCA */
	rtwn_write_1(sc, R8723B_2ND_CCA_CTRL, 0);

	/* The firmware drops management frames with fewer basic rates. */
	rtwn_setbits_4(sc, R92C_RRSR, 0x000fffff, 0x000ffff1);

	/* Let the AP time out the ACK late enough. */
	rtwn_write_2(sc, R8723B_RESP_SIFS_CCK, 0x0808);
	rtwn_write_2(sc, R8723B_RESP_SIFS_OFDM, 0x0a0a);

	/* Packet burst. */
	rtwn_setbits_1(sc, R92C_FWHW_TXQ_CTRL, 0, 0x80);
	rtwn_setbits_1(sc, R8723B_SINGLE_AMPDU_CTRL, 0, 0x80);
	rtwn_write_1(sc, R8723B_RX_PKT_LIMIT, 0x18);
	rtwn_write_1(sc, R8723B_MAX_AGGR_NUM, 0x1f);
	rtwn_write_1(sc, R8723B_PIFS, 0);
	rtwn_setbits_1(sc, R92C_FWHW_TXQ_CTRL, 0x80, 0);
	rtwn_write_1(sc, R8723B_AMPDU_MAX_TIME, 0x70);

	/* 256 ms lifetime per AC. */
	rtwn_write_2(sc, R8723B_PKT_VO_VI_LIFE_TIME, 0x0400);
	rtwn_write_2(sc, R8723B_PKT_BE_BK_LIFE_TIME, 0x0400);

	rtwn_write_1(sc, R8723B_SLOT, 0x09);
	rtwn_write_2(sc, R8723B_ATIMWND, 0x2);

	/* The 8051 controls the antenna selection. */
	rtwn_write_1(sc, R92C_LEDCFG2, 0x82);

	rtwn_write_1(sc, R8723B_NAV_UPPER, 0xeb);

	/* ACK for transmitted management frames, Tx reports on. */
	rtwn_setbits_4(sc, R92C_FWHW_TXQ_CTRL, 0, 0x1000);
	rtwn_write_1(sc, R92C_FWHW_TXQ_CTRL + 1, 0x1f);

	rtwn_bb_setbits(sc, R8723B_OFDM0_XAAGC1, 0xff, 0x50);
	rtwn_bb_setbits(sc, R8723B_OFDM0_XAAGC1, 0xff, 0x20);

#ifndef RTWN_WITHOUT_UCODE
	if (sc->sc_flags & RTWN_FW_LOADED) {
		if (sc->sc_ratectl_sysctl == RTWN_RATECTL_FW) {
			/* TODO: implement */
			sc->sc_ratectl = RTWN_RATECTL_NET80211;
		} else
			sc->sc_ratectl = sc->sc_ratectl_sysctl;
	} else
#endif
		sc->sc_ratectl = RTWN_RATECTL_NONE;
}


#ifndef RTWN_WITHOUT_UCODE
/*!	The download sequence of rtw88 for chips with an 8051: restart the CPU,
	enable the download and switch the ROM download off.
*/
void
r8723b_fw_download_enable(struct rtwn_softc *sc, int enable)
{
	int ntries;

	if (!enable) {
		rtwn_setbits_1(sc, R92C_MCUFWDL, R92C_MCUFWDL_EN, 0);
		return;
	}

	/* CPU off and on, with the MCU IO wrapper. */
	rtwn_setbits_1_shift(sc, R92C_SYS_FUNC_EN, R92C_SYS_FUNC_EN_CPUEN, 0,
	    1);
	rtwn_setbits_1(sc, R92C_RSV_CTRL + 1, 0x01, 0);
	rtwn_setbits_1(sc, R92C_RSV_CTRL + 1, 0, 0x01);
	rtwn_setbits_1_shift(sc, R92C_SYS_FUNC_EN, 0, R92C_SYS_FUNC_EN_CPUEN,
	    1);

	for (ntries = 0; ntries < 10; ntries++) {
		rtwn_setbits_1(sc, R92C_MCUFWDL, 0, R92C_MCUFWDL_EN);
		if (rtwn_read_1(sc, R92C_MCUFWDL) & R92C_MCUFWDL_EN)
			break;
		rtwn_delay(sc, 20000);
	}
	if (ntries == 10)
		device_printf(sc->sc_dev, "firmware download not ready\n");

	rtwn_setbits_4(sc, R92C_MCUFWDL, R92C_MCUFWDL_ROM_DLEN, 0);
}
#endif
