/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* Copyright(c) 2018-2019  Realtek Corporation
 * Copyright 2024 Fiona Klute
 *
 * Dynamic mechanisms of the RTL8723B: false alarm statistics, dynamic
 * initial gain (DIG) and CCK packet detection threshold, from the rtw88
 * driver (phy.c and rtw8723x.c), used here under the BSD-3-Clause license.
 * Changes for RenkuOS by Akakor, 2026: rewritten for the rtwn driver, for
 * one station link on 2.4 GHz with one RF path.
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

#include <sys/cdefs.h>
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
#include <sys/linker.h>

#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>
#include <dev/rtwn/if_rtwn_debug.h>

#include <dev/rtwn/rtl8192e/r92e_var.h>

#include <dev/rtwn/rtl8723b/r8723b.h>


/* Baseband registers. */
#define R8723B_CCK_PD_TH		0xa0a
#define R8723B_CCK_FA_RST		0xa2c
#define R8723B_CCK_FA_RST_CNT_KEEP	0x00001000
#define R8723B_CCK_FA_RST_CNT_KPEN	0x00003000
#define R8723B_CCK_FA_RST_CNT_EN	0x00002000
#define R8723B_CCK_FA_RST_FA_KEEP	0x00004000
#define R8723B_CCK_FA_RST_FA_KPEN	0x0000c000
#define R8723B_CCK_FA_RST_FA_EN		0x00008000
#define R8723B_CCK_FA_MSB		0xa58	/* byte 3 */
#define R8723B_CCK_FA_LSB		0xa5c	/* byte 0 */
#define R8723B_OFDM_FA_HOLDC		0xc00
#define R8723B_OFDM_FA_HOLDC_KEEP	0x80000000
#define R8723B_OFDM_FA_RSTC		0xc0c
#define R8723B_OFDM_FA_RSTC_RST		0x80000000
#define R8723B_IGI_A			0xc50
#define R8723B_IGI_MASK			0x0000007f
#define R8723B_OFDM_FA_TYPE1		0xcf0
#define R8723B_OFDM_FA_RSTD		0xd00
#define R8723B_OFDM_FA_RSTD_RST		0x08000000
#define R8723B_OFDM_FA_RSTD_KEEP	0x80000000
#define R8723B_OFDM_FA_TYPE2		0xda0
#define R8723B_OFDM_FA_TYPE3		0xda4
#define R8723B_OFDM_FA_TYPE4		0xda8
#define R8723B_PAGE_F_RST		0xf14
#define R8723B_PAGE_F_RST_ALL		0x00010000

/* DIG, with a link ("performance") and without one ("coverage"). */
#define R8723B_DIG_PERF_FA_TH_LOW	250
#define R8723B_DIG_PERF_FA_TH_HIGH	500
#define R8723B_DIG_PERF_FA_TH_EXTRA_HIGH 750
#define R8723B_DIG_PERF_MAX		0x5a
#define R8723B_DIG_PERF_MID		0x40
#define R8723B_DIG_PERF_MIN		0x20	/* dig_min of this chip */
#define R8723B_DIG_CVRG_FA_TH_LOW	2000
#define R8723B_DIG_CVRG_FA_TH_HIGH	4000
#define R8723B_DIG_CVRG_FA_TH_EXTRA_HIGH 5000
#define R8723B_DIG_CVRG_MAX		0x2a
#define R8723B_DIG_CVRG_MID		0x26
#define R8723B_DIG_CVRG_MIN		0x1c
#define R8723B_DIG_RSSI_GAIN_OFFSET	15

/*
 * The initial gain while scanning: the value of the AGC table, which is
 * the most sensitive one that the link code uses as well.
 */
#define R8723B_DIG_SCAN			0x20

/* CCK packet detection levels. */
#define R8723B_CCK_PD_LV0		0
#define R8723B_CCK_PD_LV1		1
#define R8723B_CCK_PD_LV2		2
#define R8723B_CCK_PD_LV3		3
#define R8723B_CCK_PD_LV4		4
#define R8723B_CCK_PD_LV_MAX		5
#define R8723B_CCK_PD_LV_UNKNOWN	0xff
#define R8723B_CCK_FA_AVG_RESET		0xffffffff
#define R8723B_CCK_PD_FA_LV1_MIN	1000
#define R8723B_CCK_PD_FA_LV0_MAX	500
#define R8723B_CCK_PD_IGI_LV4_VAL	0x38
#define R8723B_CCK_PD_IGI_LV3_VAL	0x2a
#define R8723B_CCK_PD_IGI_LV2_VAL	0x24
#define R8723B_CCK_PD_RSSI_LV4_VAL	32
#define R8723B_CCK_PD_RSSI_LV3_VAL	32
#define R8723B_CCK_PD_RSSI_LV2_VAL	24


static uint8_t
r8723b_dm_clamp(int value, int low, int high)
{
	if (value < low)
		return (low);
	if (value > high)
		return (high);
	return (value);
}


/*
 * The averaged signal strength of our access point, on the scale that the
 * chip uses (dBm + 100, as the IGI), or -1 without a link or before the
 * first frame with PHY status.
 */
static int
r8723b_dm_link_rssi(struct rtwn_softc *sc)
{
	struct ieee80211vap *vap;
	int i;

	RTWN_ASSERT_LOCKED(sc);

	if (sc->vaps_running <= sc->monvaps_running)
		return (-1);

	for (i = 0; i < RTWN_PORT_COUNT; i++) {
		if (sc->vaps[i] == NULL)
			continue;
		vap = &sc->vaps[i]->vap;
		if (vap->iv_opmode != IEEE80211_M_STA ||
		    vap->iv_state != IEEE80211_S_RUN || vap->iv_bss == NULL)
			continue;
		return (RTWN_NODE(vap->iv_bss)->avg_pwdb);
	}
	return (-1);
}


static void
r8723b_dm_write_igi(struct rtwn_softc *sc, uint8_t igi)
{
	rtwn_bb_setbits(sc, R8723B_IGI_A, R8723B_IGI_MASK, igi);
}


/* Read the false alarm counters of the last period and restart them. */
static void
r8723b_dm_false_alarm(struct rtwn_softc *sc)
{
	struct r8723b_softc *rs = sc->sc_priv;
	uint32_t cck, ofdm, val;

	/* Hold the counters. */
	rtwn_bb_setbits(sc, R8723B_OFDM_FA_HOLDC, 0,
	    R8723B_OFDM_FA_HOLDC_KEEP);
	rtwn_bb_setbits(sc, R8723B_OFDM_FA_RSTD, 0, R8723B_OFDM_FA_RSTD_KEEP);
	rtwn_bb_setbits(sc, R8723B_CCK_FA_RST, 0,
	    R8723B_CCK_FA_RST_CNT_KEEP | R8723B_CCK_FA_RST_FA_KEEP);

	cck = rtwn_bb_read(sc, R8723B_CCK_FA_LSB) & 0xff;
	cck += (rtwn_bb_read(sc, R8723B_CCK_FA_MSB) >> 24) << 8;

	val = rtwn_bb_read(sc, R8723B_OFDM_FA_TYPE1);
	ofdm = (val & 0xffff) + (val >> 16);	/* fast and short sync */
	val = rtwn_bb_read(sc, R8723B_OFDM_FA_TYPE2);
	ofdm += val >> 16;			/* parity */
	val = rtwn_bb_read(sc, R8723B_OFDM_FA_TYPE3);
	ofdm += (val & 0xffff) + (val >> 16);	/* rate and CRC8 */
	val = rtwn_bb_read(sc, R8723B_OFDM_FA_TYPE4);
	ofdm += val & 0xffff;			/* MCS */

	rs->dm_cck_fa = cck;
	rs->dm_total_fa = cck + ofdm;

	/* Reset the counters and let them run again. */
	rtwn_bb_setbits(sc, R8723B_OFDM_FA_RSTC, 0, R8723B_OFDM_FA_RSTC_RST);
	rtwn_bb_setbits(sc, R8723B_OFDM_FA_RSTC, R8723B_OFDM_FA_RSTC_RST, 0);
	rtwn_bb_setbits(sc, R8723B_OFDM_FA_RSTD, 0, R8723B_OFDM_FA_RSTD_RST);
	rtwn_bb_setbits(sc, R8723B_OFDM_FA_RSTD, R8723B_OFDM_FA_RSTD_RST, 0);
	rtwn_bb_setbits(sc, R8723B_OFDM_FA_HOLDC, R8723B_OFDM_FA_HOLDC_KEEP,
	    0);
	rtwn_bb_setbits(sc, R8723B_OFDM_FA_RSTD, R8723B_OFDM_FA_RSTD_KEEP, 0);
	rtwn_bb_setbits(sc, R8723B_CCK_FA_RST, R8723B_CCK_FA_RST_CNT_KPEN, 0);
	rtwn_bb_setbits(sc, R8723B_CCK_FA_RST, R8723B_CCK_FA_RST_CNT_KPEN,
	    R8723B_CCK_FA_RST_CNT_EN);
	rtwn_bb_setbits(sc, R8723B_CCK_FA_RST, R8723B_CCK_FA_RST_FA_KPEN, 0);
	rtwn_bb_setbits(sc, R8723B_CCK_FA_RST, R8723B_CCK_FA_RST_FA_KPEN,
	    R8723B_CCK_FA_RST_FA_EN);
	rtwn_bb_setbits(sc, R8723B_PAGE_F_RST, 0, R8723B_PAGE_F_RST_ALL);
	rtwn_bb_setbits(sc, R8723B_PAGE_F_RST, R8723B_PAGE_F_RST_ALL, 0);
}


/*
 * Whether the gain is swinging up and down with the false alarms, without
 * the signal strength changing. Then it is left alone for a while.
 */
static int
r8723b_dm_dig_check_damping(struct r8723b_softc *rs)
{
	const uint16_t fa_lo = R8723B_DIG_PERF_FA_TH_LOW;
	const uint16_t fa_hi = R8723B_DIG_PERF_FA_TH_HIGH;
	const uint8_t *igi = rs->dm_igi_history;
	const uint16_t *fa = rs->dm_fa_history;
	uint8_t diff;
	int damping = 0;

	if (rs->dm_damping) {
		diff = rs->dm_min_rssi > rs->dm_damping_rssi ?
		    rs->dm_min_rssi - rs->dm_damping_rssi :
		    rs->dm_damping_rssi - rs->dm_min_rssi;
		if (diff > 3 || rs->dm_damping_cnt++ > 20) {
			rs->dm_damping = 0;
			return (0);
		}
		return (1);
	}

	switch (rs->dm_igi_bitmap & 0xf) {
	case 5:
		/* down -> up -> down -> up */
		if (igi[0] > igi[1] && igi[2] > igi[3] &&
		    igi[0] - igi[1] >= 2 && igi[2] - igi[3] >= 2 &&
		    fa[0] > fa_hi && fa[1] < fa_lo &&
		    fa[2] > fa_hi && fa[3] < fa_lo)
			damping = 1;
		break;
	case 9:
		/* up -> down -> down -> up */
		if (igi[0] > igi[1] && igi[3] > igi[2] &&
		    igi[0] - igi[1] >= 4 && igi[3] - igi[2] >= 2 &&
		    fa[0] > fa_hi && fa[1] < fa_lo &&
		    fa[2] < fa_lo && fa[3] > fa_hi)
			damping = 1;
		break;
	default:
		return (0);
	}

	if (damping) {
		rs->dm_damping = 1;
		rs->dm_damping_cnt = 0;
		rs->dm_damping_rssi = rs->dm_min_rssi;
	}

	return (damping);
}


/* Record the gain and the false alarms, and the direction of the gain. */
static void
r8723b_dm_dig_record(struct r8723b_softc *rs, uint8_t igi, uint16_t fa)
{
	uint8_t bitmap;

	bitmap = (rs->dm_igi_bitmap << 1) & 0xfe;
	if (igi > rs->dm_igi_history[0])
		bitmap |= 1;
	rs->dm_igi_bitmap = bitmap;

	rs->dm_igi_history[3] = rs->dm_igi_history[2];
	rs->dm_igi_history[2] = rs->dm_igi_history[1];
	rs->dm_igi_history[1] = rs->dm_igi_history[0];
	rs->dm_igi_history[0] = igi;

	rs->dm_fa_history[3] = rs->dm_fa_history[2];
	rs->dm_fa_history[2] = rs->dm_fa_history[1];
	rs->dm_fa_history[1] = rs->dm_fa_history[0];
	rs->dm_fa_history[0] = fa;
}


/*
 * Dynamic initial gain: raise the gain index (less sensitive) with many
 * false alarms and lower it slowly otherwise, within bounds that follow
 * the signal strength of the access point.
 */
static void
r8723b_dm_dig(struct rtwn_softc *sc, int linked)
{
	struct r8723b_softc *rs = sc->sc_priv;
	uint16_t fa_th[3], fa_cnt;
	uint8_t step[3] = { 4, 3, 2 };
	uint8_t dig_max, dig_mid, dig_min, min_rssi, lower, upper;
	uint8_t pre_igi;
	int cur_igi, level;

	if (r8723b_dm_dig_check_damping(rs))
		return;

	fa_cnt = MIN(rs->dm_total_fa, 0xffff);
	pre_igi = rs->dm_igi_history[0];

	if (linked) {
		fa_th[0] = R8723B_DIG_PERF_FA_TH_EXTRA_HIGH;
		fa_th[1] = R8723B_DIG_PERF_FA_TH_HIGH;
		fa_th[2] = R8723B_DIG_PERF_FA_TH_LOW;
		if (rs->dm_pre_min_rssi > rs->dm_min_rssi) {
			step[0] = 6;
			step[1] = 4;
			step[2] = 2;
		}
	} else {
		fa_th[0] = R8723B_DIG_CVRG_FA_TH_EXTRA_HIGH;
		fa_th[1] = R8723B_DIG_CVRG_FA_TH_HIGH;
		fa_th[2] = R8723B_DIG_CVRG_FA_TH_LOW;
	}

	/*
	 * Test from the highest threshold down and step up accordingly.
	 * Every round also steps 2 down.
	 */
	cur_igi = pre_igi;
	for (level = 0; level < 3; level++) {
		if (fa_cnt > fa_th[level]) {
			cur_igi += step[level];
			break;
		}
	}
	cur_igi -= 2;

	/* The bounds, from the signal strength. */
	if (linked) {
		dig_max = R8723B_DIG_PERF_MAX;
		dig_mid = R8723B_DIG_PERF_MID;
		dig_min = R8723B_DIG_PERF_MIN;
		min_rssi = MAX(rs->dm_min_rssi, dig_min);
	} else {
		dig_max = R8723B_DIG_CVRG_MAX;
		dig_mid = R8723B_DIG_CVRG_MID;
		dig_min = R8723B_DIG_CVRG_MIN;
		min_rssi = dig_min;
	}
	dig_max = MIN(dig_max, min_rssi + R8723B_DIG_RSSI_GAIN_OFFSET);
	lower = r8723b_dm_clamp(min_rssi, dig_min, dig_mid);
	upper = r8723b_dm_clamp(lower + R8723B_DIG_RSSI_GAIN_OFFSET, dig_min,
	    dig_max);

	cur_igi = r8723b_dm_clamp(cur_igi, lower, upper);

	r8723b_dm_dig_record(rs, cur_igi, fa_cnt);

	if (cur_igi != pre_igi) {
		RTWN_DPRINTF(sc, RTWN_DEBUG_CALIB,
		    "%s: IGI %#x -> %#x (false alarms %u, RSSI %u)\n",
		    __func__, pre_igi, cur_igi, fa_cnt, rs->dm_min_rssi);
		r8723b_dm_write_igi(sc, cur_igi);
	}
}


/*
 * CCK packet detection threshold: a higher level ignores weaker CCK
 * signals, which lowers the number of false alarms.
 */
static void
r8723b_dm_cck_pd(struct rtwn_softc *sc, int linked)
{
	static const uint8_t pd[R8723B_CCK_PD_LV_MAX] =
	    { 0x40, 0x83, 0xcd, 0xdd, 0xed };
	struct r8723b_softc *rs = sc->sc_priv;
	uint8_t igi = rs->dm_igi_history[0];
	uint8_t rssi = rs->dm_min_rssi;
	uint8_t level;

	if (rs->dm_cck_fa_avg == R8723B_CCK_FA_AVG_RESET)
		rs->dm_cck_fa_avg = rs->dm_cck_fa;
	else
		rs->dm_cck_fa_avg = (rs->dm_cck_fa_avg * 3 + rs->dm_cck_fa) >> 2;

	if (linked && igi > R8723B_CCK_PD_IGI_LV4_VAL &&
	    rssi > R8723B_CCK_PD_RSSI_LV4_VAL)
		level = R8723B_CCK_PD_LV4;
	else if (linked && igi > R8723B_CCK_PD_IGI_LV3_VAL &&
	    rssi > R8723B_CCK_PD_RSSI_LV3_VAL)
		level = R8723B_CCK_PD_LV3;
	else if (linked && (igi > R8723B_CCK_PD_IGI_LV2_VAL ||
	    rssi > R8723B_CCK_PD_RSSI_LV2_VAL))
		level = R8723B_CCK_PD_LV2;
	else if (rs->dm_cck_fa_avg > R8723B_CCK_PD_FA_LV1_MIN)
		level = R8723B_CCK_PD_LV1;
	else if (rs->dm_cck_fa_avg < R8723B_CCK_PD_FA_LV0_MAX)
		level = R8723B_CCK_PD_LV0;
	else
		return;

	/* With a link this chip goes by the signal strength first. */
	if (linked) {
		if (rssi > 60)
			level = R8723B_CCK_PD_LV3;
		else if (rssi > 35)
			level = R8723B_CCK_PD_LV2;
		else if (rssi > 20) {
			if (rs->dm_cck_fa_avg > 500)
				level = R8723B_CCK_PD_LV2;
			else if (rs->dm_cck_fa_avg < 250)
				level = R8723B_CCK_PD_LV1;
			else
				return;
		} else
			level = R8723B_CCK_PD_LV1;
	}

	if (rs->dm_cck_pd_lv == level)
		return;

	RTWN_DPRINTF(sc, RTWN_DEBUG_CALIB, "%s: CCK PD level %u -> %u\n",
	    __func__, rs->dm_cck_pd_lv, level);

	rs->dm_cck_fa_avg = R8723B_CCK_FA_AVG_RESET;
	rs->dm_cck_pd_lv = level;
	rtwn_write_1(sc, R8723B_CCK_PD_TH, pd[level]);
}


/*
 * Called every two seconds while an interface runs (rtwn_calib_cb()), as
 * the watchdog of rtw88 is.
 */
void
r8723b_dm_watchdog(struct rtwn_softc *sc)
{
	struct r8723b_softc *rs = sc->sc_priv;
	struct ieee80211com *ic = &sc->sc_ic;
	int linked, rssi;

	RTWN_ASSERT_LOCKED(sc);

	r8723b_dm_false_alarm(sc);

	/* On other channels the counters and the gain are not ours. */
	if (ic->ic_flags & IEEE80211_F_SCAN)
		return;

	rssi = r8723b_dm_link_rssi(sc);
	linked = (rssi >= 0);
	if (linked) {
		rs->dm_pre_min_rssi = rs->dm_min_rssi;
		rs->dm_min_rssi = rssi;
		if (!rs->dm_linked)
			rs->dm_pre_min_rssi = rssi;
	}
	rs->dm_linked = linked;

	r8723b_dm_dig(sc, linked);
	r8723b_dm_cck_pd(sc, linked);
}


/* After the baseband was set up: start from what the tables wrote. */
void
r8723b_dm_init(struct rtwn_softc *sc)
{
	struct r8723b_softc *rs = sc->sc_priv;
	uint8_t igi;
	int i;

	igi = rtwn_bb_read(sc, R8723B_IGI_A) & R8723B_IGI_MASK;
	for (i = 0; i < 4; i++) {
		rs->dm_igi_history[i] = igi;
		rs->dm_fa_history[i] = 0;
	}
	rs->dm_igi_bitmap = 0;
	rs->dm_damping = 0;
	rs->dm_linked = 0;
	rs->dm_min_rssi = 0;
	rs->dm_pre_min_rssi = 0;
	rs->dm_cck_fa_avg = R8723B_CCK_FA_AVG_RESET;
	rs->dm_cck_pd_lv = R8723B_CCK_PD_LV_UNKNOWN;
}


/*
 * While scanning the gain is at its most sensitive, to hear the weak
 * access points as well. After it the gain of the link comes back.
 */
static void
r8723b_dm_scan_start(struct ieee80211com *ic)
{
	struct rtwn_softc *sc = ic->ic_softc;
	struct r8723b_softc *rs = sc->sc_priv;

	RTWN_LOCK(sc);
	if (sc->sc_flags & RTWN_RUNNING)
		r8723b_dm_write_igi(sc, R8723B_DIG_SCAN);
	RTWN_UNLOCK(sc);

	rs->dm_scan_start(ic);
}

static void
r8723b_dm_scan_end(struct ieee80211com *ic)
{
	struct rtwn_softc *sc = ic->ic_softc;
	struct r8723b_softc *rs = sc->sc_priv;

	RTWN_LOCK(sc);
	if (sc->sc_flags & RTWN_RUNNING) {
		if (r8723b_dm_link_rssi(sc) < 0) {
			/* No link: stay sensitive, the DIG starts there. */
			rs->dm_igi_history[0] = R8723B_DIG_SCAN;
		}
		r8723b_dm_write_igi(sc, rs->dm_igi_history[0]);
	}
	RTWN_UNLOCK(sc);

	rs->dm_scan_end(ic);
}

void
r8723b_dm_attach(struct rtwn_softc *sc)
{
	struct r8723b_softc *rs = sc->sc_priv;
	struct ieee80211com *ic = &sc->sc_ic;

	rs->dm_scan_start = ic->ic_scan_start;
	ic->ic_scan_start = r8723b_dm_scan_start;
	rs->dm_scan_end = ic->ic_scan_end;
	ic->ic_scan_end = r8723b_dm_scan_end;
}
