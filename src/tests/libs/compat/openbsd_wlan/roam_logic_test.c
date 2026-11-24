/*
 * Copyright 2026, Michael Spanier.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests for net80211/ieee80211_roam_logic.c. Built and run on the build
 * host by roam_logic_test.py.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ieee80211_roam_logic.h"


static int sFailures;
static int sTests;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("  FAIL %s:%d: %s\n", __func__, __LINE__, #cond); \
		sFailures++; \
		return; \
	} \
} while (0)

#define SEC(s) ((long long)(s) * 1000)

/* As the kernel sets them for iwm, which has an ic_max_rssi of 67. */
static const struct ieee80211_roam_params kParams = {
	.rp_scan_thres = 40,	/* 60 %, the 2 GHz threshold */
	.rp_weak_thres = 25,	/* 38 %, -75 dBm */
	.rp_lost_thres = 13,	/* 20 % */
	.rp_hysteresis = 8,
	.rp_hyst_fast = 15,
	.rp_dwell = 30000,
	.rp_scan_min = 5000,
	.rp_scan_max = 60000,
	.rp_scan_weak = 2500,
	.rp_scan_lost = 1000,
	.rp_scan_still = 10000,
};

static const long long kStart = 100000;	/* uptime when tests begin */


static unsigned sSeed;

/* Deterministic noise in [-3, 3]. */
static int
noise(void)
{
	sSeed = sSeed * 1103515245 + 12345;
	return (int)((sSeed >> 16) % 7) - 3;
}


/* #pragma mark - smoothing */


static void
test_avg_empty(void)
{
	struct ieee80211_roam_avg avg;

	memset(&avg, 0, sizeof(avg));
	CHECK(ieee80211_roam_avg_get(&avg) == 0);
}


static void
test_avg_first_sample(void)
{
	struct ieee80211_roam_avg avg;

	memset(&avg, 0, sizeof(avg));
	ieee80211_roam_avg_add(&avg, 26, kStart);
	CHECK(ieee80211_roam_avg_get(&avg) == 26);
}


static void
test_avg_one_sample_per_period(void)
{
	struct ieee80211_roam_avg avg;

	memset(&avg, 0, sizeof(avg));
	ieee80211_roam_avg_add(&avg, 26, kStart);
	/* A burst of frames within one period is a single sample. */
	for (int i = 1; i < 100; i++)
		ieee80211_roam_avg_add(&avg, 60, kStart + i);
	CHECK(ieee80211_roam_avg_get(&avg) == 26);
}


static void
test_avg_single_outlier(void)
{
	struct ieee80211_roam_avg avg;
	long long now = kStart;

	memset(&avg, 0, sizeof(avg));
	for (int i = 0; i < 50; i++, now += 100)
		ieee80211_roam_avg_add(&avg, 26, now);
	ieee80211_roam_avg_add(&avg, 50, now);
	/* One frame 24 units too strong moves the average by 3 at most. */
	CHECK(ieee80211_roam_avg_get(&avg) <= 29);
}


static void
test_avg_noise(void)
{
	struct ieee80211_roam_avg avg;
	long long now = kStart;
	int low = 100, high = 0;

	sSeed = 1;
	memset(&avg, 0, sizeof(avg));
	for (int i = 0; i < 1000; i++, now += 100) {
		ieee80211_roam_avg_add(&avg, 26 + noise(), now);
		if (i < 20)
			continue;
		int value = ieee80211_roam_avg_get(&avg);
		if (value < low)
			low = value;
		if (value > high)
			high = value;
	}
	CHECK(low >= 24);
	CHECK(high <= 28);
}


static void
test_avg_follows_step(void)
{
	struct ieee80211_roam_avg avg;
	long long now = kStart;

	memset(&avg, 0, sizeof(avg));
	for (int i = 0; i < 50; i++, now += 100)
		ieee80211_roam_avg_add(&avg, 26, now);
	for (int i = 0; i < 30; i++, now += 100)
		ieee80211_roam_avg_add(&avg, 40, now);
	/* Three seconds after the step the average is there. */
	CHECK(ieee80211_roam_avg_get(&avg) >= 39);
}


/* #pragma mark - switching */


static void
joined_long_ago(struct ieee80211_roam_state* state)
{
	memset(state, 0, sizeof(*state));
	ieee80211_roam_joined(&kParams, state, kStart - SEC(60));
}


/* One decision about candidate AP 1. */
#define SWITCH(state, cur, cand, now) \
	ieee80211_roam_should_switch(&kParams, state, cur, cand, 1, now)

/* Two scans in a row, as a switch to a stronger AP needs them. */
#define SWITCH2(state, cur, cand, now) \
	(!SWITCH(state, cur, cand, now) \
		&& SWITCH(state, cur, cand, (now) + 5000))


static void
test_switch_equal(void)
{
	struct ieee80211_roam_state state;

	joined_long_ago(&state);
	CHECK(!SWITCH(&state, 26, 26, kStart));
	CHECK(!SWITCH(&state, 26, 26, kStart + SEC(5)));
}


static void
test_switch_small_gain(void)
{
	struct ieee80211_roam_state state;

	joined_long_ago(&state);
	CHECK(!SWITCH(&state, 26, 27, kStart));
	CHECK(!SWITCH(&state, 26, 27, kStart + SEC(5)));
	CHECK(!SWITCH(&state, 26, 33, kStart + SEC(10)));
	CHECK(!SWITCH(&state, 26, 33, kStart + SEC(15)));
}


static void
test_switch_clear_gain(void)
{
	struct ieee80211_roam_state state;

	joined_long_ago(&state);
	CHECK(SWITCH2(&state, 26, 34, kStart));
	CHECK(SWITCH2(&state, 26, 40, kStart + SEC(10)));
}


static void
test_switch_fast_gain(void)
{
	struct ieee80211_roam_state state;

	/* Weak and far stronger: no need to wait for the next scan. */
	joined_long_ago(&state);
	CHECK(SWITCH(&state, 18, 33, kStart));
	CHECK(!SWITCH(&state, 18, 32, kStart + SEC(5)));
	/* A good AP waits for the confirmation (outliers). */
	joined_long_ago(&state);
	CHECK(!SWITCH(&state, 26, 45, kStart + SEC(10)));
	CHECK(SWITCH(&state, 26, 45, kStart + SEC(15)));
}


static void
test_switch_fast_gain_within_dwell_when_weak(void)
{
	struct ieee80211_roam_state state;

	memset(&state, 0, sizeof(state));
	ieee80211_roam_joined(&kParams, &state, kStart);
	/* A good AP is kept for the dwell time, even for a far better one. */
	CHECK(!SWITCH(&state, 26, 45, kStart + SEC(10)));
	/* A weak one is left at once for a far better one, */
	CHECK(SWITCH(&state, 18, 33, kStart + SEC(10)));
	/* but not for one only clearly better. */
	CHECK(!SWITCH(&state, 18, 32, kStart + SEC(15)));
	CHECK(!SWITCH(&state, 18, 32, kStart + SEC(20)));
}


static void
test_switch_needs_confirmation(void)
{
	struct ieee80211_roam_state state;

	joined_long_ago(&state);
	/* The first scan with a clearly better AP does not switch yet... */
	CHECK(!SWITCH(&state, 26, 40, kStart));
	/* ...but asks for the next scan soon. */
	CHECK(state.rs_scan_gap == 0);
	CHECK(SWITCH(&state, 26, 40, kStart + SEC(5)));
}


static void
test_switch_confirmation_by_same_ap(void)
{
	struct ieee80211_roam_state state;

	joined_long_ago(&state);
	CHECK(!ieee80211_roam_should_switch(&kParams, &state, 26, 40, 1,
		kStart));
	/* Another AP is better in the next scan: start over with that one. */
	CHECK(!ieee80211_roam_should_switch(&kParams, &state, 26, 40, 2,
		kStart + SEC(5)));
	CHECK(ieee80211_roam_should_switch(&kParams, &state, 26, 40, 2,
		kStart + SEC(10)));
}


static void
test_switch_confirmation_in_a_row(void)
{
	struct ieee80211_roam_state state;

	joined_long_ago(&state);
	CHECK(!SWITCH(&state, 26, 40, kStart));
	/* A scan in between without the gain clears the candidate. */
	CHECK(!SWITCH(&state, 26, 30, kStart + SEC(5)));
	CHECK(!SWITCH(&state, 26, 40, kStart + SEC(10)));
	CHECK(SWITCH(&state, 26, 40, kStart + SEC(15)));
	/* So does a scan in which the current AP was the best. */
	CHECK(!SWITCH(&state, 26, 40, kStart + SEC(50)));
	CHECK(!SWITCH(&state, 26, 0, kStart + SEC(55)));
	CHECK(!SWITCH(&state, 26, 40, kStart + SEC(60)));
}


static void
test_switch_weaker(void)
{
	struct ieee80211_roam_state state;

	joined_long_ago(&state);
	CHECK(!SWITCH(&state, 30, 20, kStart));
	CHECK(!SWITCH(&state, 30, 20, kStart + SEC(5)));
	CHECK(!SWITCH(&state, 30, 0, kStart + SEC(10)));
}


static void
test_switch_dwell(void)
{
	struct ieee80211_roam_state state;

	memset(&state, 0, sizeof(state));
	ieee80211_roam_joined(&kParams, &state, kStart);
	CHECK(!SWITCH(&state, 26, 40, kStart + SEC(10)));
	CHECK(!SWITCH(&state, 26, 40, kStart + SEC(20)));
	/* A candidate seen within the dwell time is not remembered. */
	CHECK(!SWITCH(&state, 26, 40, kStart + SEC(29)));
	CHECK(!SWITCH(&state, 26, 40, kStart + SEC(30)));
	CHECK(SWITCH(&state, 26, 40, kStart + SEC(35)));
}


static void
test_switch_lost(void)
{
	struct ieee80211_roam_state state;

	/* The current AP is practically gone: a usable one, at once. */
	memset(&state, 0, sizeof(state));
	ieee80211_roam_joined(&kParams, &state, kStart);
	CHECK(SWITCH(&state, 8, 13, kStart + SEC(1)));
	CHECK(SWITCH(&state, 10, 14, kStart + SEC(1)));
	CHECK(!SWITCH(&state, 10, 13, kStart + SEC(1)));
	/* Not to one that is lost as well. */
	CHECK(!SWITCH(&state, 6, 12, kStart + SEC(1)));
	CHECK(!SWITCH(&state, 8, 8, kStart + SEC(1)));
	CHECK(!SWITCH(&state, 8, 5, kStart + SEC(1)));
}


static void
test_switch_unknown_current(void)
{
	struct ieee80211_roam_state state;

	/* No sample of the current AP at all counts as lost. */
	joined_long_ago(&state);
	CHECK(SWITCH(&state, 0, 20, kStart));
}


/* #pragma mark - scanning */


/*
 * Lets the logic scan count times without a switch, at a steady signal.
 * Leaves *now at the last scan. Fails if a scan does not come within
 * two minutes.
 */
static int
scan_times(struct ieee80211_roam_state* state, int rssi, int count,
	long long* now)
{
	for (int i = 0; i < count; i++) {
		long long limit = *now + SEC(120);
		while (!ieee80211_roam_should_scan(&kParams, state, rssi, *now)) {
			*now += 100;
			if (*now > limit)
				return 0;
		}
		ieee80211_roam_scan_done(&kParams, state);
	}
	return 1;
}


static void
test_scan_strong(void)
{
	struct ieee80211_roam_state state;

	joined_long_ago(&state);
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 40, kStart));
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 55, kStart));
}


static void
test_scan_no_sample(void)
{
	struct ieee80211_roam_state state;

	joined_long_ago(&state);
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 0, kStart));
}


static void
test_scan_weak(void)
{
	struct ieee80211_roam_state state;

	joined_long_ago(&state);
	CHECK(ieee80211_roam_should_scan(&kParams, &state, 39, kStart));
	/* Not again right away. */
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 39, kStart + 100));
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 39,
		kStart + 4999));
}


static void
test_scan_backoff(void)
{
	static const int kGaps[] = { 5, 10, 20, 40, 60, 60 };
	struct ieee80211_roam_state state;
	long long now = kStart;

	joined_long_ago(&state);
	CHECK(ieee80211_roam_should_scan(&kParams, &state, 26, now));
	ieee80211_roam_scan_done(&kParams, &state);
	for (unsigned i = 0; i < sizeof(kGaps) / sizeof(kGaps[0]); i++) {
		long long next = now + SEC(kGaps[i]);
		CHECK(!ieee80211_roam_should_scan(&kParams, &state, 26,
			next - 1));
		CHECK(ieee80211_roam_should_scan(&kParams, &state, 26, next));
		ieee80211_roam_scan_done(&kParams, &state);
		now = next;
	}
}


static void
test_scan_backoff_reset_when_good(void)
{
	struct ieee80211_roam_state state;
	long long now = kStart;

	joined_long_ago(&state);
	CHECK(scan_times(&state, 26, 4, &now));
	/* Gap is 40 s now. The signal recovers, later drops again. */
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 45, now + SEC(1)));
	CHECK(ieee80211_roam_should_scan(&kParams, &state, 30, now + SEC(5)));
}


static void
test_scan_no_reset_at_threshold(void)
{
	struct ieee80211_roam_state state;
	long long now = kStart;

	joined_long_ago(&state);
	CHECK(scan_times(&state, 38, 4, &now));
	/* Just touching the threshold does not reset the backoff. */
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 41, now + SEC(1)));
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 38, now + SEC(5)));
}


static void
test_scan_falling_signal(void)
{
	struct ieee80211_roam_state state;
	long long now = kStart;

	joined_long_ago(&state);
	CHECK(scan_times(&state, 30, 4, &now));
	/* Gap is 40 s, but the signal fell by half the hysteresis. */
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 27, now + SEC(5)));
	CHECK(ieee80211_roam_should_scan(&kParams, &state, 26, now + SEC(5)));
}


static void
test_scan_dwell(void)
{
	struct ieee80211_roam_state state;

	memset(&state, 0, sizeof(state));
	ieee80211_roam_joined(&kParams, &state, kStart);
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 26,
		kStart + SEC(10)));
	CHECK(ieee80211_roam_should_scan(&kParams, &state, 26,
		kStart + SEC(30)));
}


static void
test_scan_lost_ignores_dwell_and_backoff(void)
{
	struct ieee80211_roam_state state;
	long long now = kStart;

	joined_long_ago(&state);
	CHECK(scan_times(&state, 26, 4, &now));
	ieee80211_roam_joined(&kParams, &state, now);
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 10, now + 500));
	CHECK(ieee80211_roam_should_scan(&kParams, &state, 10, now + SEC(1)));
	ieee80211_roam_scan_done(&kParams, &state);
	/* Still falling: again soon. */
	CHECK(ieee80211_roam_should_scan(&kParams, &state, 6, now + SEC(2)));
	ieee80211_roam_scan_done(&kParams, &state);
	/* Staying put: the backoff, but at most rp_scan_still. */
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 6, now + SEC(3)));
	CHECK(ieee80211_roam_should_scan(&kParams, &state, 6, now + SEC(12)));
}


static void
test_scan_weak_ignores_dwell_and_backoff(void)
{
	struct ieee80211_roam_state state;
	long long now = kStart;

	joined_long_ago(&state);
	CHECK(scan_times(&state, 26, 4, &now));
	/* Gap is 40 s, and we just joined: but the AP is weak. */
	ieee80211_roam_joined(&kParams, &state, now);
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 18, now + 2000));
	CHECK(ieee80211_roam_should_scan(&kParams, &state, 18, now + 2500));
	ieee80211_roam_scan_done(&kParams, &state);
	/* Staying put: the backoff, but at most rp_scan_still. */
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 18, now + 5000));
	CHECK(ieee80211_roam_should_scan(&kParams, &state, 18, now + 12500));
	ieee80211_roam_scan_done(&kParams, &state);
	/* Falling again: every rp_scan_weak. */
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 14, now + 14999));
	CHECK(ieee80211_roam_should_scan(&kParams, &state, 14, now + 15000));
}


static void
test_scan_refused_retries_soon(void)
{
	struct ieee80211_roam_state state;
	long long now = kStart;

	joined_long_ago(&state);
	CHECK(scan_times(&state, 26, 4, &now));
	CHECK(ieee80211_roam_should_scan(&kParams, &state, 26, now + SEC(40)));
	/* The stack could not start the scan: do not wait another 40 s. */
	now += SEC(40);
	ieee80211_roam_scan_refused(&state, now);
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 26, now + 400));
	CHECK(ieee80211_roam_should_scan(&kParams, &state, 26, now + 500));
	/* Started this time: the gap counts again. */
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 26, now + 1000));
}


static void
test_scan_refused_not_when_good(void)
{
	struct ieee80211_roam_state state;

	joined_long_ago(&state);
	CHECK(ieee80211_roam_should_scan(&kParams, &state, 26, kStart));
	ieee80211_roam_scan_refused(&state, kStart);
	/* Meanwhile the signal recovered. */
	CHECK(!ieee80211_roam_should_scan(&kParams, &state, 45, kStart + 500));
}


static void
test_join_halves_gap(void)
{
	struct ieee80211_roam_state state;
	long long now = kStart;

	joined_long_ago(&state);
	CHECK(scan_times(&state, 26, 4, &now));
	CHECK(state.rs_scan_gap == 40000);
	ieee80211_roam_joined(&kParams, &state, now);
	CHECK(state.rs_scan_gap == 20000);
	ieee80211_roam_joined(&kParams, &state, now);
	ieee80211_roam_joined(&kParams, &state, now);
	ieee80211_roam_joined(&kParams, &state, now);
	CHECK(state.rs_scan_gap <= kParams.rp_scan_min);
}


/* #pragma mark - simulation */


#define MAX_APS 4

typedef int (*level_func)(int ap, long long t);

struct sim {
	level_func	level;
	int		aps;
	int		current;
	int		switches;
	int		scans;
	int		spikes;		/* some beacons 12 units too strong */
	long long	switch_time[16];
	int		switch_to[16];
};


/*
 * Every 100 ms a frame of the current AP feeds the average, like in
 * ieee80211_input(). When the logic asks for a scan, every AP is
 * measured once with noise, like a background scan, and the strongest
 * becomes the candidate, like in ieee80211_node_choose_bss().
 */
static void
simulate(struct sim* sim, long long from, long long to)
{
	struct ieee80211_roam_state state;
	struct ieee80211_roam_avg avg;

	memset(&state, 0, sizeof(state));
	memset(&avg, 0, sizeof(avg));
	ieee80211_roam_joined(&kParams, &state, from);
	sSeed = 7;

	for (long long now = from; now < to; now += 100) {
		ieee80211_roam_avg_add(&avg,
			sim->level(sim->current, now - from) + noise(), now);
		int cur = ieee80211_roam_avg_get(&avg);
		if (!ieee80211_roam_should_scan(&kParams, &state, cur, now))
			continue;

		sim->scans++;
		int best = -1, bestValue = 0;
		for (int ap = 0; ap < sim->aps; ap++) {
			int value = sim->level(ap, now - from) + noise();
			if (sim->spikes && sim->scans % 7 == ap)
				value += 12;
			if (value > 0 && (best < 0 || value > bestValue)) {
				best = ap;
				bestValue = value;
			}
		}
		if (best == sim->current)
			best = -1;
		if (!ieee80211_roam_should_switch(&kParams, &state, cur,
				best < 0 ? 0 : bestValue, best + 1, now)) {
			ieee80211_roam_scan_done(&kParams, &state);
			continue;
		}

		if (sim->switches < 16) {
			sim->switch_time[sim->switches] = now - from;
			sim->switch_to[sim->switches] = best;
		}
		sim->switches++;
		sim->current = best;
		memset(&avg, 0, sizeof(avg));
		ieee80211_roam_joined(&kParams, &state, now);
	}
}


static int
level_equal(int ap, long long t)
{
	/*
	 * Two APs of one mesh node measured 52 each, far above the
	 * threshold, where no scan happens at all. 30 is below it, where
	 * every scan is a chance to switch.
	 */
	return 30;
}


static void
test_sim_equal_with_noise(void)
{
	struct sim sim = { level_equal, 2, 0 };

	/* Two equal APs, noise +-3, for 20 minutes. */
	simulate(&sim, kStart, kStart + SEC(1200));
	printf("  equal: %d scans, %d switches\n", sim.scans, sim.switches);
	CHECK(sim.switches == 0);
	CHECK(sim.scans <= 40);
}


static int
level_mesh(int ap, long long t)
{
	/* Four APs of a mesh within 3 units of each other. */
	static const int kLevel[] = { 30, 32, 31, 29 };
	return kLevel[ap];
}


static void
test_sim_mesh_with_noise(void)
{
	struct sim sim = { level_mesh, 4, 0 };

	simulate(&sim, kStart, kStart + SEC(3600));
	printf("  mesh: %d scans, %d switches\n", sim.scans, sim.switches);
	CHECK(sim.switches == 0);
}


static void
test_sim_equal_with_outliers(void)
{
	struct sim sim = { level_equal, 2, 0 };

	/* Every seventh scan one AP's single beacon is 12 units too strong. */
	sim.spikes = 1;
	simulate(&sim, kStart, kStart + SEC(3600));
	printf("  outliers: %d scans, %d switches\n", sim.scans, sim.switches);
	CHECK(sim.switches == 0);
}


static int
level_far(int ap, long long t)
{
	/* A room far from every AP: all of them lost or nearly. */
	static const int kLevels[] = { 11, 12, 10 };

	return kLevels[ap];
}


static int
level_far_weak(int ap, long long t)
{
	/* Or merely weak, -75 to -80 dBm. */
	static const int kLevels[] = { 22, 24, 20 };

	return kLevels[ap];
}


static void
test_sim_all_weak(void)
{
	struct sim sim = { level_far, 3, 0 };

	/* Hectic switching between weak APs does not help anybody. */
	simulate(&sim, kStart, kStart + SEC(600));
	printf("  all lost: %d scans, %d switches\n", sim.scans, sim.switches);
	CHECK(sim.switches <= 3);
	/* Not a scan storm either: one per rp_scan_still at most. */
	CHECK(sim.scans <= 600 / 10 + 5);

	struct sim weak = { level_far_weak, 3, 0 };

	simulate(&weak, kStart, kStart + SEC(600));
	printf("  all weak: %d scans, %d switches\n", weak.scans,
		weak.switches);
	CHECK(weak.switches <= 3);
	CHECK(weak.scans <= 600 / 10 + 5);

	struct sim spikes = { level_far_weak, 3, 0 };

	spikes.spikes = 1;
	simulate(&spikes, kStart, kStart + SEC(600));
	printf("  all weak, outliers: %d scans, %d switches\n",
		spikes.scans, spikes.switches);
	CHECK(spikes.switches <= 3);
}


static int
level_walk(int ap, long long t)
{
	/* A falls from 40 to 10 within 120 s, B rises from 10 to 40. */
	if (t > SEC(120))
		t = SEC(120);
	int delta = (int)(30 * t / SEC(120));
	return ap == 0 ? 40 - delta : 10 + delta;
}


static void
test_sim_walk(void)
{
	struct sim sim = { level_walk, 2, 0 };

	simulate(&sim, kStart, kStart + SEC(300));
	printf("  walk: %d scans, %d switches, at %lld ms\n", sim.scans,
		sim.switches, sim.switch_time[0]);
	CHECK(sim.switches == 1);
	CHECK(sim.switch_to[0] == 1);
	/* Levels cross at 60 s, B leads by 8 at 76 s. */
	CHECK(sim.switch_time[0] >= SEC(64));
	CHECK(sim.switch_time[0] <= SEC(100));
}


static int
level_dwell(int ap, long long t)
{
	/* B gets better at 40 s, then A much better at 60 s. */
	if (ap == 0)
		return t >= SEC(60) ? 55 : 24;
	return t >= SEC(40) ? 38 : 24;
}


static void
test_sim_dwell(void)
{
	struct sim sim = { level_dwell, 2, 0 };

	simulate(&sim, kStart, kStart + SEC(200));
	printf("  dwell: %d switches, at %lld and %lld ms\n", sim.switches,
		sim.switch_time[0], sim.switch_time[1]);
	CHECK(sim.switches == 2);
	CHECK(sim.switch_to[0] == 1);
	CHECK(sim.switch_time[0] >= SEC(40));
	CHECK(sim.switch_time[0] <= SEC(70));
	/* Not back to A before the dwell time is over. */
	CHECK(sim.switch_to[1] == 0);
	CHECK(sim.switch_time[1] >= sim.switch_time[0] + SEC(30));
	CHECK(sim.switch_time[1] <= sim.switch_time[0] + SEC(41));
}


static int
level_vanish(int ap, long long t)
{
	/* A disappears 10 s after joining, B is weaker but there. */
	if (ap == 0)
		return t >= SEC(10) ? 4 : 26;
	return 20;
}


static void
test_sim_current_vanishes(void)
{
	struct sim sim = { level_vanish, 2, 0 };

	simulate(&sim, kStart, kStart + SEC(60));
	printf("  vanish: %d switches, at %lld ms\n", sim.switches,
		sim.switch_time[0]);
	CHECK(sim.switches == 1);
	CHECK(sim.switch_to[0] == 1);
	/* Within the dwell time, but at once: average falls, one scan. */
	CHECK(sim.switch_time[0] >= SEC(10));
	CHECK(sim.switch_time[0] <= SEC(17));
}


/* #pragma mark - replay of a walk through a mesh */


/*
 * The signal of the current AP, one value per second, logged on a walk
 * through a 5-AP mesh with an earlier version of these rules (8 units
 * and two scans, nothing for weak APs). Candidates are what the scans
 * showed meanwhile. The values come from "ifconfig list", which shows
 * half of ni_rssi: replay() doubles them.
 */

/* Down the stairs, away from the AP. Switched after 50 s. */
static const int kStairsDown[] = {
	31, 27, 26, 27, 25, 28, 30, 29, 29, 28, 27, 28, 28, 28, 28, 27, 25,
	24, 23, 20, 20, 24, 20, 20, 21, 21, 19, 20, 17, 17, 17, 13, 14, 10,
	11, 12, 13, 11, 10, 10, 12, 13, 13, 14, 14, 11, 10, 11, 8, 11
};

/* Up the stairs, two floors away from the AP. Link lost after 45 s. */
static const int kStairsUp[] = {
	27, 28, 27, 26, 26, 25, 25, 22, 24, 22, 21, 18, 15, 15, 14, 14, 15,
	17, 13, 13, 10, 9, 9, 9, 10, 8, 8, 8, 9, 8, 7, 8, 7, 6, 6, 6, 6, 6,
	6, 6, 6, 5, 5, 5, 5
};

/* Leaving the top floor. Switched after 23 s. */
static const int kLeavingTop[] = {
	25, 24, 26, 26, 23, 21, 22, 22, 19, 19, 16, 16, 16, 14, 10, 8, 7, 8,
	8, 7, 8, 7, 7
};

/* Leaving the middle floor. Switched after 39 s. */
static const int kLeavingMiddle[] = {
	7, 7, 11, 11, 12, 11, 9, 6, 6, 7, 7, 10, 9, 9, 9, 10, 8, 7, 8, 8, 7,
	8, 7, 8, 7, 9, 8, 8, 8, 8, 9, 8, 9, 9, 9, 10, 10, 10, 10
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define REPLAY_SCAN 4000	/* msec, one background scan of the iwm */
#define REPLAY_SCALE 2		/* "ifconfig list" shows half the RSSI */

/* The APs of the walk are 5 GHz ones: scan below 50 % of 67. */
static const struct ieee80211_roam_params kParams5 = {
	.rp_scan_thres = 33,
	.rp_weak_thres = 25,
	.rp_lost_thres = 13,
	.rp_hysteresis = 8,
	.rp_hyst_fast = 15,
	.rp_dwell = 30000,
	.rp_scan_min = 5000,
	.rp_scan_max = 60000,
	.rp_scan_weak = 2500,
	.rp_scan_lost = 1000,
	.rp_scan_still = 10000,
};

struct replay {
	const int*	cur;		/* current AP, one value per second */
	int		seconds;
	int		(*cand)(int second);
	int		joined;		/* seconds before the start, 0 long ago */
	int		refuse;		/* every other scan request refused */
	int		switched;	/* second of the switch, -1 none */
	int		scans;
};


/*
 * Ten minutes of standing still at 56 first, near the AP. Then the
 * series: a frame every 100 ms, scans take REPLAY_SCAN, the decision
 * comes at the end of the scan.
 */
static void
replay(struct replay* r)
{
	struct ieee80211_roam_state state;
	struct ieee80211_roam_avg avg;
	long long now = kStart, scanEnd = 0, from;
	int requests = 0;

	joined_long_ago(&state);
	memset(&avg, 0, sizeof(avg));
	sSeed = 7;
	for (; now < kStart + SEC(600); now += 100) {
		ieee80211_roam_avg_add(&avg, 56 + noise(), now);
		int cur = ieee80211_roam_avg_get(&avg);
		if (ieee80211_roam_should_scan(&kParams5, &state, cur, now)) {
			ieee80211_roam_should_switch(&kParams5, &state, cur, 0, 0,
				now);
			ieee80211_roam_scan_done(&kParams5, &state);
		}
	}

	from = now;
	if (r->joined != 0) {
		/* As ieee80211_roam_join(): a new AP, a new average. */
		ieee80211_roam_joined(&kParams5, &state, from - SEC(r->joined));
		memset(&avg, 0, sizeof(avg));
	}
	r->switched = -1;
	for (; now < from + SEC(r->seconds); now += 100) {
		int second = (int)((now - from) / 1000);
		int cur;

		ieee80211_roam_avg_add(&avg,
		    REPLAY_SCALE * r->cur[second] + noise(), now);
		cur = ieee80211_roam_avg_get(&avg);
		if (scanEnd != 0) {
			if (now < scanEnd)
				continue;
			scanEnd = 0;
			if (ieee80211_roam_should_switch(&kParams5, &state, cur,
					REPLAY_SCALE * r->cand(second), 2, now)) {
				r->switched = second;
				return;
			}
			ieee80211_roam_scan_done(&kParams5, &state);
		}
		if (!ieee80211_roam_should_scan(&kParams5, &state, cur, now))
			continue;
		if (r->refuse && requests++ % 2 == 0) {
			ieee80211_roam_scan_refused(&state, now);
			continue;
		}
		r->scans++;
		scanEnd = now + REPLAY_SCAN;
	}
}


static int
cand_stairs_down(int second)
{
	/* The AP downstairs comes up, 30 against 14 after 32 s. */
	return second < 32 ? 14 + second / 2 : 30;
}


static void
test_replay_stairs_down(void)
{
	struct replay r = { kStairsDown, COUNT(kStairsDown), cand_stairs_down };

	replay(&r);
	printf("  stairs down: switch at %d s (was 50), %d scans\n",
		r.switched, r.scans);
	CHECK(r.switched >= 20);
	CHECK(r.switched <= 36);
}


static int
cand_stairs_up(int second)
{
	return 18;
}


static void
test_replay_stairs_up(void)
{
	/* Joined 40 s before; the stack refuses every other scan. */
	struct replay r = { kStairsUp, COUNT(kStairsUp), cand_stairs_up, 40, 1 };

	replay(&r);
	printf("  stairs up: switch at %d s (link lost at 45), %d scans\n",
		r.switched, r.scans);
	CHECK(r.switched >= 11);
	CHECK(r.switched <= 25);
}


static int
cand_leaving_top(int second)
{
	return 17;
}


static void
test_replay_leaving_top_floor(void)
{
	struct replay r = { kLeavingTop, COUNT(kLeavingTop), cand_leaving_top,
		20 };

	replay(&r);
	printf("  leaving the top floor: switch at %d s (was 23), %d scans\n",
		r.switched, r.scans);
	CHECK(r.switched >= 8);
	CHECK(r.switched <= 19);
}


static int
cand_leaving_middle(int second)
{
	return second < 28 ? 20 + second / 4 : 27;
}


static void
test_replay_leaving_middle_floor(void)
{
	struct replay r = { kLeavingMiddle, COUNT(kLeavingMiddle), cand_leaving_middle, 5, 1 };

	replay(&r);
	printf("  leaving the middle floor: switch at %d s (was 39), %d scans\n",
		r.switched, r.scans);
	CHECK(r.switched >= 0);
	CHECK(r.switched <= 8);
}


/* #pragma mark - */


typedef void (*test_func)(void);

#define TEST(name) { #name, name }

static const struct {
	const char*	name;
	test_func	func;
} kTests[] = {
	TEST(test_avg_empty),
	TEST(test_avg_first_sample),
	TEST(test_avg_one_sample_per_period),
	TEST(test_avg_single_outlier),
	TEST(test_avg_noise),
	TEST(test_avg_follows_step),
	TEST(test_switch_equal),
	TEST(test_switch_small_gain),
	TEST(test_switch_clear_gain),
	TEST(test_switch_fast_gain),
	TEST(test_switch_fast_gain_within_dwell_when_weak),
	TEST(test_switch_weaker),
	TEST(test_switch_needs_confirmation),
	TEST(test_switch_confirmation_by_same_ap),
	TEST(test_switch_confirmation_in_a_row),
	TEST(test_switch_dwell),
	TEST(test_switch_lost),
	TEST(test_switch_unknown_current),
	TEST(test_scan_strong),
	TEST(test_scan_no_sample),
	TEST(test_scan_weak),
	TEST(test_scan_backoff),
	TEST(test_scan_backoff_reset_when_good),
	TEST(test_scan_no_reset_at_threshold),
	TEST(test_scan_falling_signal),
	TEST(test_scan_dwell),
	TEST(test_scan_lost_ignores_dwell_and_backoff),
	TEST(test_scan_weak_ignores_dwell_and_backoff),
	TEST(test_scan_refused_retries_soon),
	TEST(test_scan_refused_not_when_good),
	TEST(test_join_halves_gap),
	TEST(test_sim_equal_with_noise),
	TEST(test_sim_mesh_with_noise),
	TEST(test_sim_equal_with_outliers),
	TEST(test_sim_all_weak),
	TEST(test_sim_walk),
	TEST(test_sim_dwell),
	TEST(test_sim_current_vanishes),
	TEST(test_replay_stairs_down),
	TEST(test_replay_stairs_up),
	TEST(test_replay_leaving_top_floor),
	TEST(test_replay_leaving_middle_floor),
};


int
main(void)
{
	for (unsigned i = 0; i < sizeof(kTests) / sizeof(kTests[0]); i++) {
		int before = sFailures;
		sTests++;
		kTests[i].func();
		printf("%s %s\n", sFailures == before ? "PASS" : "FAIL",
			kTests[i].name);
	}
	printf("%d tests, %d failed\n", sTests, sFailures);
	return sFailures == 0 ? 0 : 1;
}
