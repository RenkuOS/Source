/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */

/*
 * Roaming decisions, see ieee80211_roam_logic.h. No kernel headers here,
 * so that the tests in src/tests/libs/compat/openbsd_wlan run on the
 * build host.
 */

#include "ieee80211_roam_logic.h"

/*
 * The RSSI of a frame or of a node on one scale, where higher is better
 * and 0 is none. ni_rssi is unsigned, so negative values wrap there.
 * - mwx reports dBm and leaves max_rssi at 0. Its values are negative,
 *   and wrapped they are still in order: -60 dBm is 196. (wpi has no
 *   max_rssi either, but positive values; it never scans while
 *   associated, so no roaming decision is made from them.)
 * - iwm and iwx clip to max_rssi, but not below 0: what is above max_rssi
 *   is a wrapped negative value. That is an AP below the scale, or a frame
 *   without any energy measured; both count as the weakest signal.
 */
int
ieee80211_roam_level(int rssi, int max_rssi)
{
	int level = rssi & 0xff;

	if (max_rssi != 0 && level > max_rssi)
		return 1;
	return level;
}

void
ieee80211_roam_avg_add(struct ieee80211_roam_avg *avg, int rssi,
    long long now)
{
	int sample = rssi << IEEE80211_ROAM_AVG_SHIFT;

	if (avg->ra_value == 0) {
		avg->ra_value = sample;
		avg->ra_stamp = now;
		return;
	}

	/*
	 * One sample per period, so that a burst of data frames does not
	 * weigh more than the beacons of a quiet link.
	 */
	if (now - avg->ra_stamp < IEEE80211_ROAM_AVG_PERIOD)
		return;

	avg->ra_value += (sample - avg->ra_value) / IEEE80211_ROAM_AVG_WEIGHT;
	if (avg->ra_value <= 0)
		avg->ra_value = 1;
	avg->ra_stamp = now;
}

int
ieee80211_roam_avg_get(const struct ieee80211_roam_avg *avg)
{
	if (avg->ra_value == 0)
		return 0;
	return (avg->ra_value + (1 << (IEEE80211_ROAM_AVG_SHIFT - 1))) >>
	    IEEE80211_ROAM_AVG_SHIFT;
}

static int
ieee80211_roam_lost(const struct ieee80211_roam_params *rp, int cur)
{
	return cur < rp->rp_lost_thres;
}

static int
ieee80211_roam_weak(const struct ieee80211_roam_params *rp, int cur)
{
	return cur < rp->rp_weak_thres;
}

int
ieee80211_roam_should_switch(const struct ieee80211_roam_params *rp,
    struct ieee80211_roam_state *rs, int cur, int cand, unsigned id,
    long long now)
{
	int fast;

	/* No other AP in this scan: nothing to confirm next time. */
	if (cand <= 0 || id == 0) {
		rs->rs_cand = 0;
		return 0;
	}

	/*
	 * The current AP is practically gone: take at once one that is
	 * not, and a bit stronger. Not one as weak, that would only start
	 * a hectic hopping between weak APs.
	 */
	if (ieee80211_roam_lost(rp, cur)) {
		rs->rs_cand = 0;
		return !ieee80211_roam_lost(rp, cand) &&
		    cand >= cur + rp->rp_hysteresis / 2;
	}

	if (cand < cur + rp->rp_hysteresis) {
		rs->rs_cand = 0;
		return 0;
	}

	/*
	 * The current AP is weak and the candidate far stronger: one scan
	 * is enough, also within the dwell time. For a good current AP a
	 * single outlier beacon must not cause a switch.
	 */
	fast = ieee80211_roam_weak(rp, cur) &&
	    cand >= cur + rp->rp_hyst_fast;
	if (fast) {
		rs->rs_cand = 0;
		return 1;
	}
	if (now - rs->rs_last_join < rp->rp_dwell) {
		rs->rs_cand = 0;
		return 0;
	}
	if (rs->rs_cand == id) {
		rs->rs_cand = 0;
		return 1;
	}

	/* Better in this scan; confirm with the next one. */
	rs->rs_cand = id;
	return 0;
}

void
ieee80211_roam_joined(const struct ieee80211_roam_params *rp,
    struct ieee80211_roam_state *rs, long long now)
{
	rs->rs_last_join = now;
	rs->rs_cand = 0;
}
