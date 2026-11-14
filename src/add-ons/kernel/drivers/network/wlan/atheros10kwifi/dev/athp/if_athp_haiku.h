/*
 * Copyright 2026, RenkuOS contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef	__IF_ATHP_HAIKU_H__
#define	__IF_ATHP_HAIKU_H__

/*
 * Definitions athp(4) expects from the FreeBSD kernel or from its own
 * net80211 tree that the Haiku compat layer does not provide.
 */

#include <sys/libkern.h>
#include <net/ethernet.h>

#ifndef WITNESS_WARN
#define	WITNESS_WARN(flags, lock, fmt, ...)	do { } while (0)
#endif

void thread_yield(void);
#define	kern_yield(prio)	thread_yield()

/* Only used as a timed sleep: nothing calls wakeup() on the channel. */
#define	tsleep(chan, pri, wmesg, timo)	pause((wmesg), (timo))

#define	kdb_backtrace()		do { } while (0)

#ifndef isascii
#define	isascii(c)		(((c) & ~0x7f) == 0)
#endif

/* From FreeBSD <sys/libkern.h>; the compat copy predates them. */
#define	ilog2(n)		(flsll(n) - 1)

static __inline unsigned long
roundup_pow_of_two(unsigned long n)
{

	return (n <= 1 ? 1 : 1UL << flsl(n - 1));
}

/*
 * Per-channel survey results. athp(4) only collects these internally;
 * nothing hands them to net80211 yet.
 */
#define	IEEE80211_F_SURVEY_NOISE_DBM	0x00000001
#define	IEEE80211_F_SURVEY_TIME		0x00000002
#define	IEEE80211_F_SURVEY_TIME_BUSY	0x00000004

struct ieee80211_channel_survey {
	uint32_t	s_flags;
	int8_t		s_noise;
	uint64_t	s_time;
	uint64_t	s_time_busy;
};

#endif	/* __IF_ATHP_HAIKU_H__ */
