/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */

#ifndef _NET80211_IEEE80211_ROAM_LOGIC_H_
#define _NET80211_IEEE80211_ROAM_LOGIC_H_

/*
 * When a station should look for, and move to, another access point of
 * its ESS. Pure logic without kernel dependencies: the caller passes
 * RSSI values in the driver's units (higher is better, about 1 dB per
 * unit) and times in milliseconds of a monotonic clock.
 *
 * The signal of the current AP is smoothed (EWMA), so that single frames
 * do not decide anything.
 *
 * Switching, after each background scan (ieee80211_roam_should_switch(),
 * with candidate 0 when the best AP found is the current one):
 *  - only to an AP that is at least rp_hysteresis stronger in two scans
 *    in a row (a scan sees one beacon per AP, which may be an outlier),
 *    and not within rp_dwell after the last switch;
 *  - below rp_weak_thres one scan is enough for an AP rp_hyst_fast
 *    stronger, also within rp_dwell;
 *  - below rp_lost_thres the AP is practically gone: take at once an AP
 *    that is not lost and at least rp_hysteresis / 2 stronger.
 *
 * Scanning (ieee80211_roam_should_scan()):
 *  - only while the smoothed signal is below rp_scan_thres, with a gap
 *    that grows from rp_scan_min to rp_scan_max while the scans find
 *    nothing and the signal stays put; a falling signal scans sooner;
 *  - below rp_weak_thres every rp_scan_weak (rp_scan_lost below
 *    rp_lost_thres), regardless of the dwell time; while the signal
 *    stays put the gap still grows, up to rp_scan_still;
 *  - a scan request the stack could not start is retried after
 *    IEEE80211_ROAM_RETRY (ieee80211_roam_scan_refused()).
 */

struct ieee80211_roam_params {
	int		rp_scan_thres;	/* scan for a better AP below this */
	int		rp_weak_thres;	/* below this the AP is weak */
	int		rp_lost_thres;	/* below this the AP is practically gone */
	int		rp_hysteresis;	/* a new AP must be this much stronger */
	int		rp_hyst_fast;	/* this much stronger: switch at once */
	int		rp_dwell;	/* msec to stay after a switch */
	int		rp_scan_min;	/* msec, first gap between scans */
	int		rp_scan_max;	/* msec, largest gap between scans */
	int		rp_scan_weak;	/* msec, gap between scans while weak */
	int		rp_scan_lost;	/* msec, gap between scans while lost */
	int		rp_scan_still;	/* msec, largest gap while weak */
};

/* Smoothed signal of one AP; all zero means "no sample yet". */
struct ieee80211_roam_avg {
	int		ra_value;	/* RSSI << IEEE80211_ROAM_AVG_SHIFT */
	long long	ra_stamp;	/* msec of the last sample taken */
};

/* Roaming state of the station; all zero is a valid start. */
struct ieee80211_roam_state {
	long long	rs_last_join;	/* msec of the last (re)association */
	unsigned	rs_cand;	/* id of the candidate to confirm, 0 none */
	long long	rs_last_scan;	/* msec of the last scan started */
	int		rs_scan_gap;	/* msec, current gap between scans */
	int		rs_scan_rssi;	/* smoothed RSSI at the last scan */
	long long	rs_retry;	/* msec, retry a refused scan then */
};

#define IEEE80211_ROAM_AVG_SHIFT	4	/* fixed point: 1/16 unit */
#define IEEE80211_ROAM_AVG_WEIGHT	8	/* a sample counts 1/8 */
#define IEEE80211_ROAM_AVG_PERIOD	100	/* msec, one sample per period */
#define IEEE80211_ROAM_RETRY		500	/* msec, after a refused scan */

#ifdef __cplusplus
extern "C" {
#endif

void	ieee80211_roam_avg_add(struct ieee80211_roam_avg *, int, long long);
int	ieee80211_roam_avg_get(const struct ieee80211_roam_avg *);

int	ieee80211_roam_should_switch(const struct ieee80211_roam_params *,
	    struct ieee80211_roam_state *, int, int, unsigned, long long);
void	ieee80211_roam_joined(const struct ieee80211_roam_params *,
	    struct ieee80211_roam_state *, long long);

int	ieee80211_roam_should_scan(const struct ieee80211_roam_params *,
	    struct ieee80211_roam_state *, int, long long);
void	ieee80211_roam_scan_done(const struct ieee80211_roam_params *,
	    struct ieee80211_roam_state *);
void	ieee80211_roam_scan_refused(struct ieee80211_roam_state *, long long);

#ifdef __cplusplus
}
#endif

#endif /* _NET80211_IEEE80211_ROAM_LOGIC_H_ */
