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
	.rp_weak_thres = 25,	/* 38 %, -75 dBm */
	.rp_lost_thres = 13,	/* 20 % */
	.rp_hysteresis = 8,
	.rp_hyst_fast = 15,
	.rp_dwell = 30000,
};

/* As the kernel sets them for mwx, which reports dBm (ic_max_rssi is 0). */
static const struct ieee80211_roam_params kDbmParams = {
	.rp_weak_thres = (unsigned char)-75,
	.rp_lost_thres = (unsigned char)-85,
	.rp_hysteresis = 8,
	.rp_hyst_fast = 15,
	.rp_dwell = 30000,
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
	/* The first scan with a clearly better AP does not switch yet. */
	CHECK(!SWITCH(&state, 26, 40, kStart));
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


/* #pragma mark - the scale of the driver */


static void
test_level_relative(void)
{
	/* iwm: 0 is -100 dBm, 67 the maximum. */
	CHECK(ieee80211_roam_level(1, 67) == 1);
	CHECK(ieee80211_roam_level(26, 67) == 26);
	CHECK(ieee80211_roam_level(67, 67) == 67);
	/* Below -100 dBm, and without any energy measured (-256 dBm). */
	CHECK(ieee80211_roam_level(-1, 67) == 1);
	CHECK(ieee80211_roam_level(-156, 67) == 1);
	/* The same from ni_rssi, which wraps. */
	CHECK(ieee80211_roam_level((unsigned char)-1, 67) == 1);
	CHECK(ieee80211_roam_level((unsigned char)-156, 67) == 1);
}


static void
test_level_relative_below_scale(void)
{
	struct ieee80211_roam_avg avg;
	struct ieee80211_roam_state state;

	/* An AP below -100 dBm is the weakest one, not the strongest. */
	joined_long_ago(&state);
	CHECK(!SWITCH(&state, 18, ieee80211_roam_level((unsigned char)-1, 67),
		kStart));
	CHECK(!SWITCH(&state, 18, ieee80211_roam_level((unsigned char)-1, 67),
		kStart + SEC(5)));

	memset(&avg, 0, sizeof(avg));
	for (int i = 0; i < 100; i++)
		ieee80211_roam_avg_add(&avg, ieee80211_roam_level(-1, 67),
			kStart + i * 100);
	CHECK(ieee80211_roam_avg_get(&avg) == 1);
}


static void
test_level_dbm(void)
{
	int last = 0;

	/* Received and stored in ni_rssi are the same, and higher is better. */
	for (int dbm = -128; dbm < 0; dbm++) {
		int level = ieee80211_roam_level(dbm, 0);
		CHECK(level == ieee80211_roam_level((unsigned char)dbm, 0));
		CHECK(level > last);
		last = level;
	}
}


/* The average after some seconds of frames with this signal. */
static int
dbm_average(int dbm)
{
	struct ieee80211_roam_avg avg;

	memset(&avg, 0, sizeof(avg));
	for (int i = 0; i < 50; i++)
		ieee80211_roam_avg_add(&avg, ieee80211_roam_level(dbm, 0),
			kStart + i * 100);
	return ieee80211_roam_avg_get(&avg);
}


/* One decision about candidate AP 1, with its signal as in ni_rssi. */
static int
dbm_switch(struct ieee80211_roam_state* state, int cur, int cand,
	long long now)
{
	return ieee80211_roam_should_switch(&kDbmParams, state,
		dbm_average(cur), ieee80211_roam_level((unsigned char)cand, 0), 1,
		now);
}


static void
dbm_joined_long_ago(struct ieee80211_roam_state* state)
{
	memset(state, 0, sizeof(*state));
	ieee80211_roam_joined(&kDbmParams, state, kStart - SEC(60));
}


static void
test_dbm_first_sample(void)
{
	struct ieee80211_roam_avg avg;

	memset(&avg, 0, sizeof(avg));
	ieee80211_roam_avg_add(&avg, ieee80211_roam_level(-60, 0), kStart);
	/* On the scale of ni_rssi, where candidates and thresholds are. */
	CHECK(ieee80211_roam_avg_get(&avg) == (unsigned char)-60);
}


static void
test_dbm_average(void)
{
	CHECK(dbm_average(-40) == (unsigned char)-40);
	CHECK(dbm_average(-60) == (unsigned char)-60);
	CHECK(dbm_average(-90) == (unsigned char)-90);
	/* What mwx reports without a valid chain. */
	CHECK(dbm_average(-128) == (unsigned char)-128);
}


static void
test_dbm_average_follows_step(void)
{
	struct ieee80211_roam_avg avg;
	long long now = kStart;

	memset(&avg, 0, sizeof(avg));
	for (int i = 0; i < 50; i++, now += 100)
		ieee80211_roam_avg_add(&avg, ieee80211_roam_level(-60, 0), now);
	for (int i = 0; i < 40; i++, now += 100)
		ieee80211_roam_avg_add(&avg, ieee80211_roam_level(-80, 0), now);
	CHECK(ieee80211_roam_avg_get(&avg) >= (unsigned char)-80);
	CHECK(ieee80211_roam_avg_get(&avg) <= (unsigned char)-78);
}


static void
test_dbm_first_sample_is_not_lost(void)
{
	struct ieee80211_roam_avg avg;
	struct ieee80211_roam_state state;

	/* Right after a join, with a single frame of the new AP. */
	memset(&avg, 0, sizeof(avg));
	ieee80211_roam_avg_add(&avg, ieee80211_roam_level(-60, 0), kStart);
	memset(&state, 0, sizeof(state));
	ieee80211_roam_joined(&kDbmParams, &state, kStart);
	CHECK(!ieee80211_roam_should_switch(&kDbmParams, &state,
		ieee80211_roam_avg_get(&avg),
		ieee80211_roam_level((unsigned char)-59, 0), 1, kStart + SEC(1)));
}


static void
test_dbm_switch_clear_gain(void)
{
	struct ieee80211_roam_state state;

	dbm_joined_long_ago(&state);
	CHECK(!dbm_switch(&state, -60, -53, kStart));
	CHECK(!dbm_switch(&state, -60, -53, kStart + SEC(5)));
	CHECK(!dbm_switch(&state, -60, -52, kStart + SEC(10)));
	CHECK(dbm_switch(&state, -60, -52, kStart + SEC(15)));
}


static void
test_dbm_switch_weak(void)
{
	struct ieee80211_roam_state state;

	/* -75 dBm is not weak yet: wait for the confirmation. */
	dbm_joined_long_ago(&state);
	CHECK(!dbm_switch(&state, -75, -60, kStart));
	CHECK(dbm_switch(&state, -75, -60, kStart + SEC(5)));
	/* -76 dBm is: 15 dB more are enough at once. */
	CHECK(!dbm_switch(&state, -76, -62, kStart + SEC(10)));
	dbm_joined_long_ago(&state);
	CHECK(dbm_switch(&state, -76, -61, kStart + SEC(15)));
}


static void
test_dbm_switch_lost(void)
{
	struct ieee80211_roam_state state;

	memset(&state, 0, sizeof(state));
	ieee80211_roam_joined(&kDbmParams, &state, kStart);
	CHECK(dbm_switch(&state, -89, -85, kStart + SEC(1)));
	/* Not to one that is lost as well, nor when it is not lost itself. */
	CHECK(!dbm_switch(&state, -92, -86, kStart + SEC(1)));
	CHECK(!dbm_switch(&state, -85, -80, kStart + SEC(1)));
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
	TEST(test_level_relative),
	TEST(test_level_relative_below_scale),
	TEST(test_level_dbm),
	TEST(test_dbm_first_sample),
	TEST(test_dbm_average),
	TEST(test_dbm_average_follows_step),
	TEST(test_dbm_first_sample_is_not_lost),
	TEST(test_dbm_switch_clear_gain),
	TEST(test_dbm_switch_weak),
	TEST(test_dbm_switch_lost),
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
