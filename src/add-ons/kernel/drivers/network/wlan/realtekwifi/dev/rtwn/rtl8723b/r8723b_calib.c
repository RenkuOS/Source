/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* Copyright(c) 2018-2019  Realtek Corporation
 * Copyright(c) 2026  Realtek Corporation
 * Copyright(c) Michael Straube <straube.linux@gmail.com>
 * Copyright(c) 2024-2026 Luka Gejak <luka.gejak@linux.dev>
 * Copyright 2024 Fiona Klute
 *
 * IQ calibration of the RTL8723B (path A, one antenna), from the rtw88
 * driver (rtw8723b.c and rtw8723x.c/h), used here under the BSD-3-Clause
 * license.
 * Changes for RenkuOS by Akakor, 2026: rewritten for the rtwn driver; the
 * firmware is not told about the calibration, and the results are kept
 * only in the registers.
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

#include <dev/rtwn/rtl8723b/r8723b.h>


/* Registers (rtw88 reg.h, rtw8723x.h). */
#define R8723B_BB_SEL_BTG		0x948
#define R8723B_BT_CONTROL		0x764
#define R8723B_CCK_ANT_SEL		0xa04
#define R8723B_BB_RX_PATH		0xc04
#define R8723B_TRMUX			0xc08
#define R8723B_BB_PWR_SAV1		0x874
#define R8723B_A_RXIQI			0xc14
#define R8723B_OFDM0_XAAGC1		0xc50
#define R8723B_OFDM0_XBAGC1		0xc58
#define R8723B_OFDM0_ECCA_THRESHOLD	0xc4c
#define R8723B_OFDM0_XA_TX_IQ_IMB	0xc80
#define R8723B_TXIQK_MATRIXA_LSB2	0xc94
#define R8723B_RXIQK_MATRIX_LSB		0xca0
#define R8723B_FPGA0_IQK		0xe28
#define R8723B_TXIQK_TONE_A		0xe30
#define R8723B_RXIQK_TONE_A		0xe34
#define R8723B_TXIQK_PI_A		0xe38
#define R8723B_RXIQK_PI_A		0xe3c
#define R8723B_TXIQK			0xe40
#define R8723B_RXIQK			0xe44
#define R8723B_IQK_AGC_PTS		0xe48
#define R8723B_IQK_AGC_RSP		0xe4c
#define R8723B_TX_IQK_TONE_B		0xe50
#define R8723B_RX_IQK_TONE_B		0xe54
#define R8723B_TXIQK_PI_B		0xe58
#define R8723B_RXIQK_PI_B		0xe5c
#define R8723B_IQK_RES_TX		0xe94
#define R8723B_IQK_RES_TY		0xe9c
#define R8723B_IQK_RES_RX		0xea4
#define R8723B_IQK_RES_RY		0xeac

#define R8723B_IQK_TX_FAIL		0x10000000	/* in RES_RY */
#define R8723B_IQK_RX_FAIL		0x08000000	/* in RES_RY */
#define R8723B_IQK_RES(v)		(((v) >> 16) & 0x3ff)

/* RF registers. */
#define R8723B_RF_MODE_TABLE_ADDR	0x30
#define R8723B_RF_MODE_TABLE_DATA0	0x31
#define R8723B_RF_MODE_TABLE_DATA1	0x32
#define R8723B_RF_LUTDBG		0xdf
#define R8723B_RF_LUTWE			0xef

#define R8723B_ADDA_ON			0x01c00014
#define R8723B_IQK_DELAY		20000	/* us */
#define R8723B_IQK_RETRY		2
#define R8723B_IQK_TOLERANCE		5

#define IQK_TX_X_ERR	0x142
#define IQK_TX_Y_ERR	0x42
#define IQK_RX_X_ERR	0x132
#define IQK_RX_Y_ERR	0x36
#define IQK_RX_X_UPPER	0x11a
#define IQK_RX_X_LOWER	0xe6
#define IQK_RX_Y_LMT	0x1a
#define IQK_TX_OK	0x1
#define IQK_RX_OK	0x2

/* Results: TX X, TX Y, RX X, RX Y of path A (S1). */
enum { IQK_TX_X, IQK_TX_Y, IQK_RX_X, IQK_RX_Y, IQK_NR };
enum { IQK_ROUNDS = 3, IQK_HYBRID = 3 };

static const uint16_t adda_regs[] = {
	0x85c, 0xe6c, 0xe70, 0xe74, 0xe78, 0xe7c, 0xe80, 0xe84,
	0xe88, 0xe8c, 0xed0, 0xed4, 0xed8, 0xedc, 0xee0, 0xeec
};
static const uint16_t mac8_regs[] = { 0x522, 0x550, 0x551 };
static const uint16_t bb_regs[] = {
	0xc04, 0xc08, 0x874, 0xb68, 0xb6c, 0x870, 0x860, 0x864, 0xa04
};

struct r8723b_iqk_backup {
	uint32_t	adda[nitems(adda_regs)];
	uint8_t		mac8[nitems(mac8_regs)];
	uint32_t	mac32;		/* 0x40 */
	uint32_t	bb[nitems(bb_regs)];
	uint8_t		igia, igib;
	uint32_t	bb_sel_btg;
};


static int32_t
iqkxy_to_s32(int32_t val)
{
	/* 10 bit two's complement, Q10.8 */
	return (val & 0x200) ? (val | ~0x3ff) : (val & 0x3ff);
}


static int32_t
iqk_mult(int32_t x, int32_t y, int32_t *ext)
{
	int32_t t = x * y;

	if (ext != NULL)
		*ext = (t >> 7) & 0x1;
	return (t >> 8);
}


static void
r8723b_bb_write_mask(struct rtwn_softc *sc, uint16_t reg, uint32_t mask,
    uint32_t val)
{
	int shift = ffs(mask) - 1;

	rtwn_bb_setbits(sc, reg, mask, (val << shift) & mask);
}


static uint32_t
r8723b_bb_read_mask(struct rtwn_softc *sc, uint16_t reg, uint32_t mask)
{
	return ((rtwn_bb_read(sc, reg) & mask) >> (ffs(mask) - 1));
}


static void
r8723b_rf_write_mask(struct rtwn_softc *sc, uint8_t reg, uint32_t mask,
    uint32_t val)
{
	int shift = ffs(mask) - 1;
	uint32_t rf = rtwn_rf_read(sc, 0, reg);

	rtwn_rf_write(sc, 0, reg, (rf & ~mask) | ((val << shift) & mask));
}


static void
r8723b_iqk_backup_regs(struct rtwn_softc *sc, struct r8723b_iqk_backup *b)
{
	int i;

	for (i = 0; i < (int)nitems(adda_regs); i++)
		b->adda[i] = rtwn_bb_read(sc, adda_regs[i]);
	for (i = 0; i < (int)nitems(mac8_regs); i++)
		b->mac8[i] = rtwn_read_1(sc, mac8_regs[i]);
	b->mac32 = rtwn_read_4(sc, 0x40);
	for (i = 0; i < (int)nitems(bb_regs); i++)
		b->bb[i] = rtwn_bb_read(sc, bb_regs[i]);
	b->igia = rtwn_bb_read(sc, R8723B_OFDM0_XAAGC1) & 0xff;
	b->igib = rtwn_bb_read(sc, R8723B_OFDM0_XBAGC1) & 0xff;
	b->bb_sel_btg = rtwn_bb_read(sc, R8723B_BB_SEL_BTG);
}


static void
r8723b_iqk_restore_regs(struct rtwn_softc *sc,
    const struct r8723b_iqk_backup *b)
{
	int i;

	for (i = 0; i < (int)nitems(adda_regs); i++)
		rtwn_bb_write(sc, adda_regs[i], b->adda[i]);
	for (i = 0; i < (int)nitems(mac8_regs); i++)
		rtwn_write_1(sc, mac8_regs[i], b->mac8[i]);
	rtwn_write_4(sc, 0x40, b->mac32);
	for (i = 0; i < (int)nitems(bb_regs); i++)
		rtwn_bb_write(sc, bb_regs[i], b->bb[i]);

	rtwn_bb_setbits(sc, R8723B_OFDM0_XAAGC1, 0xff, 0x50);
	rtwn_bb_setbits(sc, R8723B_OFDM0_XAAGC1, 0xff, b->igia);
	rtwn_bb_setbits(sc, R8723B_OFDM0_XBAGC1, 0xff, 0x50);
	rtwn_bb_setbits(sc, R8723B_OFDM0_XBAGC1, 0xff, b->igib);

	rtwn_bb_write(sc, R8723B_TXIQK_TONE_A, 0x01008c00);
	rtwn_bb_write(sc, R8723B_RXIQK_TONE_A, 0x01008c00);
}


/* The antenna path for the calibration: S1 (0) unless the efuse says S0. */
static uint32_t
r8723b_iqk_ant_path(struct rtwn_softc *sc)
{
	struct r8723b_softc *rs = sc->sc_priv;
	bool shared = (rs->bt_setting & 0x01) != 0;
	bool path_b = (rs->bt_setting & R8723B_BT_SETTING_AUX_ANT) != 0;

	return ((shared && path_b) ? 0x280 : 0x0);
}


static void
r8723b_iqk_rf_mode(struct rtwn_softc *sc, uint32_t addr, uint32_t data0,
    uint32_t data1)
{
	r8723b_rf_write_mask(sc, R8723B_RF_LUTWE, 0x80000, 0x1);
	rtwn_rf_write(sc, 0, R8723B_RF_MODE_TABLE_ADDR, addr);
	rtwn_rf_write(sc, 0, R8723B_RF_MODE_TABLE_DATA0, data0);
	rtwn_rf_write(sc, 0, R8723B_RF_MODE_TABLE_DATA1, data1);
}


/* One shot of the calibration engine, with the antenna on the WLAN path. */
static void
r8723b_iqk_one_shot(struct rtwn_softc *sc)
{
	uint32_t path_sel = rtwn_bb_read(sc, R8723B_BB_SEL_BTG);

	/* Enter IQK mode. */
	r8723b_bb_write_mask(sc, R8723B_FPGA0_IQK, 0xffffff00, 0x808000);

	rtwn_bb_write(sc, R8723B_BB_SEL_BTG, r8723b_iqk_ant_path(sc));
	rtwn_write_4(sc, R8723B_BT_CONTROL, 0x00000800);	/* GNT_BT = 0 */

	rtwn_bb_write(sc, R8723B_IQK_AGC_PTS, 0xf9000000);
	rtwn_bb_write(sc, R8723B_IQK_AGC_PTS, 0xf8000000);
	rtwn_delay(sc, R8723B_IQK_DELAY);

	rtwn_bb_write(sc, R8723B_BB_SEL_BTG, path_sel);
	rtwn_write_4(sc, R8723B_BT_CONTROL, 0x00001800);	/* GNT_BT = 1 */

	/* Leave IQK mode. */
	r8723b_bb_write_mask(sc, R8723B_FPGA0_IQK, 0xffffff00, 0x000000);
}


static int
r8723b_iqk_tx_ok(struct rtwn_softc *sc)
{
	uint32_t ry = rtwn_bb_read(sc, R8723B_IQK_RES_RY);
	uint32_t x = R8723B_IQK_RES(rtwn_bb_read(sc, R8723B_IQK_RES_TX));
	uint32_t y = R8723B_IQK_RES(rtwn_bb_read(sc, R8723B_IQK_RES_TY));

	RTWN_DPRINTF(sc, RTWN_DEBUG_CALIB, "%s: 0xeac %#x x %#x y %#x\n",
	    __func__, ry, x, y);

	return (!(ry & R8723B_IQK_TX_FAIL) && x != IQK_TX_X_ERR
	    && y != IQK_TX_Y_ERR) ? IQK_TX_OK : 0;
}


static int
r8723b_iqk_rx_ok(struct rtwn_softc *sc)
{
	uint32_t ry = rtwn_bb_read(sc, R8723B_IQK_RES_RY);
	int32_t x = R8723B_IQK_RES(rtwn_bb_read(sc, R8723B_IQK_RES_RX));
	int32_t y = R8723B_IQK_RES(ry);

	RTWN_DPRINTF(sc, RTWN_DEBUG_CALIB, "%s: 0xeac %#x x %#x y %#x\n",
	    __func__, ry, x, y);
	y = abs(iqkxy_to_s32(y));
	return (!(ry & R8723B_IQK_RX_FAIL) && x != IQK_RX_X_ERR
	    && y != IQK_RX_Y_ERR && x < IQK_RX_X_UPPER && x > IQK_RX_X_LOWER
	    && y < IQK_RX_Y_LMT) ? IQK_RX_OK : 0;
}


static int
r8723b_iqk_tx_path_a(struct rtwn_softc *sc)
{
	r8723b_bb_write_mask(sc, R8723B_FPGA0_IQK, 0xffffff00, 0x000000);

	/* Path A PA on in TX IQK mode. */
	r8723b_iqk_rf_mode(sc, 0x20000, 0x0003f, 0xc7f87);

	rtwn_bb_write(sc, R8723B_TXIQK, 0x01007c00);
	rtwn_bb_write(sc, R8723B_RXIQK, 0x01004800);
	rtwn_bb_write(sc, R8723B_TXIQK_TONE_A, 0x18008c1c);
	rtwn_bb_write(sc, R8723B_RXIQK_TONE_A, 0x38008c1c);
	rtwn_bb_write(sc, R8723B_TX_IQK_TONE_B, 0x38008c1c);
	rtwn_bb_write(sc, R8723B_RX_IQK_TONE_B, 0x38008c1c);
	rtwn_bb_write(sc, R8723B_TXIQK_PI_A, 0x821403ea);
	rtwn_bb_write(sc, R8723B_RXIQK_PI_A, 0x28110000);
	rtwn_bb_write(sc, R8723B_TXIQK_PI_B, 0x82110000);
	rtwn_bb_write(sc, R8723B_RXIQK_PI_B, 0x28110000);
	rtwn_bb_write(sc, R8723B_IQK_AGC_RSP, 0x00462911);

	r8723b_iqk_one_shot(sc);

	return (r8723b_iqk_tx_ok(sc));
}


static int
r8723b_iqk_rx_path_a(struct rtwn_softc *sc)
{
	uint32_t e94, e9c;
	int status;

	/* Step 1: TX IQK with the RX settings, for the loopback signal. */
	r8723b_bb_write_mask(sc, R8723B_FPGA0_IQK, 0xffffff00, 0x000000);
	r8723b_iqk_rf_mode(sc, 0x30000, 0x0001f, 0xf7fb7);

	rtwn_bb_write(sc, R8723B_TXIQK, 0x01007c00);
	rtwn_bb_write(sc, R8723B_RXIQK, 0x01004800);
	rtwn_bb_write(sc, R8723B_TXIQK_TONE_A, 0x18008c1c);
	rtwn_bb_write(sc, R8723B_RXIQK_TONE_A, 0x38008c1c);
	rtwn_bb_write(sc, R8723B_TX_IQK_TONE_B, 0x38008c1c);
	rtwn_bb_write(sc, R8723B_RX_IQK_TONE_B, 0x38008c1c);
	rtwn_bb_write(sc, R8723B_TXIQK_PI_A, 0x82160ff0);
	rtwn_bb_write(sc, R8723B_RXIQK_PI_A, 0x28110000);
	rtwn_bb_write(sc, R8723B_TXIQK_PI_B, 0x82110000);
	rtwn_bb_write(sc, R8723B_RXIQK_PI_B, 0x28110000);
	rtwn_bb_write(sc, R8723B_IQK_AGC_RSP, 0x0046a911);

	r8723b_iqk_one_shot(sc);

	status = r8723b_iqk_tx_ok(sc);
	if (status == 0)
		return (0);

	e94 = rtwn_bb_read(sc, R8723B_IQK_RES_TX);
	e9c = rtwn_bb_read(sc, R8723B_IQK_RES_TY);
	rtwn_bb_write(sc, R8723B_TXIQK, 0x80007c00 | (e94 & 0x3ff0000)
	    | ((e9c & 0x3ff0000) >> 16));

	/* Step 2: RX IQK. */
	r8723b_bb_write_mask(sc, R8723B_FPGA0_IQK, 0xffffff00, 0x000000);
	r8723b_iqk_rf_mode(sc, 0x30000, 0x0001f, 0xf7d77);

	/* PA, PAD setting. */
	rtwn_rf_write(sc, 0, R8723B_RF_LUTDBG, 0xf80);
	rtwn_rf_write(sc, 0, 0x55, 0x4021f);

	rtwn_bb_write(sc, R8723B_RXIQK, 0x01004800);
	rtwn_bb_write(sc, R8723B_TXIQK_TONE_A, 0x38008c1c);
	rtwn_bb_write(sc, R8723B_RXIQK_TONE_A, 0x18008c1c);
	rtwn_bb_write(sc, R8723B_TX_IQK_TONE_B, 0x38008c1c);
	rtwn_bb_write(sc, R8723B_RX_IQK_TONE_B, 0x38008c1c);
	rtwn_bb_write(sc, R8723B_TXIQK_PI_A, 0x82110000);
	rtwn_bb_write(sc, R8723B_RXIQK_PI_A, 0x2816001f);
	rtwn_bb_write(sc, R8723B_TXIQK_PI_B, 0x82110000);
	rtwn_bb_write(sc, R8723B_RXIQK_PI_B, 0x28110000);
	rtwn_bb_write(sc, R8723B_IQK_AGC_RSP, 0x0046a8d1);

	r8723b_iqk_one_shot(sc);

	rtwn_rf_write(sc, 0, R8723B_RF_LUTDBG, 0x780);

	return (status | r8723b_iqk_rx_ok(sc));
}


static void
r8723b_iqk_one_round(struct rtwn_softc *sc, int32_t result[IQK_NR],
    const struct r8723b_iqk_backup *b)
{
	int i, ok;

	for (i = 0; i < (int)nitems(adda_regs); i++)
		rtwn_bb_write(sc, adda_regs[i], R8723B_ADDA_ON);

	/* MAC: pause TX, as the vendor driver does. */
	rtwn_write_1(sc, mac8_regs[0], 0x3f);
	for (i = 1; i < (int)nitems(mac8_regs); i++)
		rtwn_write_1(sc, mac8_regs[i], b->mac8[i] & ~0x08);
	rtwn_write_1(sc, 0x40, (b->mac32 & 0xff) & ~0x20);

	r8723b_bb_write_mask(sc, R8723B_CCK_ANT_SEL, 0x0f000000, 0xf);
	rtwn_bb_write(sc, R8723B_BB_RX_PATH, 0x03a05600);
	rtwn_bb_write(sc, R8723B_TRMUX, 0x000800e4);
	rtwn_bb_write(sc, R8723B_BB_PWR_SAV1, 0x22204000);

	/* RX IQ calibration setting against the D cut current issue. */
	r8723b_bb_write_mask(sc, R8723B_FPGA0_IQK, 0xffffff00, 0x000000);
	r8723b_iqk_rf_mode(sc, 0x30000, 0x0001f, 0xf7fb7);
	r8723b_rf_write_mask(sc, 0xed, 0x20, 0x1);
	rtwn_rf_write(sc, 0, 0x43, 0x60fbd);

	for (i = 0; i < R8723B_IQK_RETRY; i++) {
		ok = r8723b_iqk_tx_path_a(sc);
		if (ok != IQK_TX_OK) {
			result[IQK_TX_X] = 0x100;
			result[IQK_TX_Y] = 0x0;
			continue;
		}
		r8723b_bb_write_mask(sc, R8723B_FPGA0_IQK, 0xffffff00, 0x000000);
		result[IQK_TX_X] = R8723B_IQK_RES(
		    rtwn_bb_read(sc, R8723B_IQK_RES_TX));
		result[IQK_TX_Y] = R8723B_IQK_RES(
		    rtwn_bb_read(sc, R8723B_IQK_RES_TY));
		break;
	}

	for (i = 0; i < R8723B_IQK_RETRY; i++) {
		ok = r8723b_iqk_rx_path_a(sc);
		if (ok != (IQK_TX_OK | IQK_RX_OK)) {
			result[IQK_RX_X] = 0x100;
			result[IQK_RX_Y] = 0x0;
			continue;
		}
		result[IQK_RX_X] = R8723B_IQK_RES(
		    rtwn_bb_read(sc, R8723B_IQK_RES_RX));
		result[IQK_RX_Y] = R8723B_IQK_RES(
		    rtwn_bb_read(sc, R8723B_IQK_RES_RY));
		break;
	}

	r8723b_bb_write_mask(sc, R8723B_FPGA0_IQK, 0xffffff00, 0x000000);
}


/*!	Compares two rounds; returns true when they agree. On a partial match
	the matching TX or RX pair is copied to the hybrid round.
*/
static bool
r8723b_iqk_similar(int32_t result[][IQK_NR], int c1, int c2)
{
	uint32_t bitmap = 0;
	int candidate = -1;
	int i;

	for (i = 0; i < IQK_NR; i++) {
		int32_t diff = abs(iqkxy_to_s32(result[c1][i])
		    - iqkxy_to_s32(result[c2][i]));

		if (diff <= R8723B_IQK_TOLERANCE)
			continue;
		if (i == IQK_RX_X && bitmap == 0) {
			if (result[c1][i] + result[c1][i + 1] == 0)
				candidate = c2;
			else if (result[c2][i] + result[c2][i + 1] == 0)
				candidate = c1;
			else
				bitmap |= 1 << i;
		} else
			bitmap |= 1 << i;
	}

	if (bitmap == 0) {
		if (candidate < 0)
			return (true);
		result[IQK_HYBRID][IQK_TX_X] = result[candidate][IQK_TX_X];
		result[IQK_HYBRID][IQK_TX_Y] = result[candidate][IQK_TX_Y];
		return (false);
	}

	for (i = 0; i < IQK_NR; i++) {
		int j = i & ~1;	/* X and Y are a pair */

		if (bitmap & (3 << j))
			continue;
		result[IQK_HYBRID][i] = result[c1][i];
	}
	return (false);
}


static void
r8723b_iqk_fill_a(struct rtwn_softc *sc, const int32_t result[IQK_NR])
{
	int32_t old, x, y, a, a_ext, c, c_ext;

	if (result[IQK_TX_X] == 0)
		return;

	old = r8723b_bb_read_mask(sc, R8723B_OFDM0_XA_TX_IQ_IMB, 0xffc00000);

	x = iqkxy_to_s32(result[IQK_TX_X]);
	a = iqk_mult(x, old, &a_ext);
	r8723b_bb_write_mask(sc, R8723B_OFDM0_XA_TX_IQ_IMB, 0x3ff, a);
	r8723b_bb_write_mask(sc, R8723B_OFDM0_ECCA_THRESHOLD, 0x80000000,
	    a_ext);

	y = iqkxy_to_s32(result[IQK_TX_Y]);
	c = iqk_mult(y, old, &c_ext);
	r8723b_bb_write_mask(sc, R8723B_TXIQK_MATRIXA_LSB2, 0xf0000000,
	    (c & 0x3c0) >> 6);
	r8723b_bb_write_mask(sc, R8723B_OFDM0_XA_TX_IQ_IMB, 0x003f0000,
	    c & 0x3f);
	r8723b_bb_write_mask(sc, R8723B_OFDM0_ECCA_THRESHOLD, 0x20000000,
	    c_ext);

	if (result[IQK_RX_X] == 0)
		return;

	r8723b_bb_write_mask(sc, R8723B_A_RXIQI, 0x3ff, result[IQK_RX_X]);
	r8723b_bb_write_mask(sc, R8723B_A_RXIQI, 0xfc00,
	    result[IQK_RX_Y] & 0x3f);
	r8723b_bb_write_mask(sc, R8723B_RXIQK_MATRIX_LSB, 0xf0000000,
	    (result[IQK_RX_Y] >> 6) & 0xf);
}


void
r8723b_iq_calib(struct rtwn_softc *sc)
{
	struct r8723b_iqk_backup b;
	int32_t result[IQK_ROUNDS + 1][IQK_NR];
	uint32_t bt_control;
	int final = -1;
	int i, j;

	memset(result, 0, sizeof(result));

	r8723b_lc_calib(sc);

	r8723b_iqk_backup_regs(sc, &b);
	bt_control = rtwn_read_4(sc, R8723B_BT_CONTROL);

	for (i = 0; i < IQK_ROUNDS && final < 0; i++) {
		r8723b_iqk_one_round(sc, result[i], &b);

		if (i > 0) {
			r8723b_iqk_restore_regs(sc, &b);

			/* Restore the RX initial gain. */
			rtwn_bb_setbits(sc, R8723B_OFDM0_XAAGC1, 0xff, 0x50);
			rtwn_bb_setbits(sc, R8723B_OFDM0_XAAGC1, 0xff, b.igia);
			rtwn_bb_write(sc, R8723B_TXIQK_TONE_A, 0x01008c00);
			rtwn_bb_write(sc, R8723B_RXIQK_TONE_A, 0x01008c00);
		}

		for (j = 0; j < i; j++) {
			if (r8723b_iqk_similar(result, j, i)) {
				final = j;
				break;
			}
		}
	}
	if (final < 0)
		r8723b_iqk_restore_regs(sc, &b);

	if (final < 0) {
		int32_t sum = 0;

		for (i = 0; i < IQK_NR; i++)
			sum += result[IQK_HYBRID][i];
		if (sum != 0)
			final = IQK_HYBRID;
	}

	if (final >= 0)
		r8723b_iqk_fill_a(sc, result[final]);

	/* Restore the RF path, GNT_BT and the RX mode table. */
	rtwn_bb_write(sc, R8723B_BB_SEL_BTG, b.bb_sel_btg);
	rtwn_write_4(sc, R8723B_BT_CONTROL, bt_control);
	r8723b_iqk_rf_mode(sc, 0x18000, 0x0001f, 0xe6177);
	r8723b_rf_write_mask(sc, 0xed, 0x20, 0x1);
	rtwn_rf_write(sc, 0, 0x43, 0x300bd);

	if (final < 0) {
		device_printf(sc->sc_dev, "IQ calibration failed\n");
		return;
	}
	RTWN_DPRINTF(sc, RTWN_DEBUG_CALIB, "IQ calibration: round %d, TX %#x/%#x, "
	    "RX %#x/%#x; 0xc80 %#x 0xc94 %#x 0xc14 %#x 0xca0 %#x\n", final,
	    result[final][IQK_TX_X], result[final][IQK_TX_Y],
	    result[final][IQK_RX_X], result[final][IQK_RX_Y],
	    rtwn_bb_read(sc, R8723B_OFDM0_XA_TX_IQ_IMB),
	    rtwn_bb_read(sc, R8723B_TXIQK_MATRIXA_LSB2),
	    rtwn_bb_read(sc, R8723B_A_RXIQI),
	    rtwn_bb_read(sc, R8723B_RXIQK_MATRIX_LSB));
}
