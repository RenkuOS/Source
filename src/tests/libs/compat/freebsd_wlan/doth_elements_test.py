#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the 802.11h Power Capability and Supported Channels elements of
a (re)association request with the functions from ieee80211_output.c."""
from pathlib import Path
import os
import subprocess
import tempfile

root = (Path(os.environ['SOURCE_ROOT']) if 'SOURCE_ROOT' in os.environ
        else Path(__file__).resolve().parents[5])
source = (root / 'src/libs/compat/freebsd_wlan/net80211/ieee80211_output.c').read_text()
def function(name):
    start = source.index('static uint8_t *\n' + name + '(')
    return source[start:source.index('\n}\n', start) + 3]

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define __HAIKU__ 1
#define NBBY 8
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#define nitems(x) (sizeof(x) / sizeof((x)[0]))
#define IEEE80211_CHAN_MAX 1024
#define IEEE80211_CHAN_BYTES howmany(IEEE80211_CHAN_MAX, NBBY)
#define IEEE80211_CHAN_2GHZ 0x0080
#define IEEE80211_CHAN_5GHZ 0x0100
#define IEEE80211_CHAN_HT20 0x10000
#define IEEE80211_IS_CHAN_2GHZ(c) (((c)->ic_flags & IEEE80211_CHAN_2GHZ) != 0)
#define IEEE80211_IS_CHAN_5GHZ(c) (((c)->ic_flags & IEEE80211_CHAN_5GHZ) != 0)
#define IEEE80211_ELEMID_PWRCAP 33
#define IEEE80211_ELEMID_SUPPCHAN 36
struct ieee80211_channel { uint32_t ic_flags; uint8_t ic_ieee;
	int8_t ic_maxpower, ic_minpower; };
struct ieee80211com { const struct ieee80211_channel *ic_curchan;
	int ic_nchans; struct ieee80211_channel ic_channels[128];
	uint8_t ic_chan_avail[IEEE80211_CHAN_BYTES]; };
'''
checks = r'''
static void
add(struct ieee80211com *ic, uint32_t band, int first, int last, int step)
{
	/* one legacy and one HT20 entry per channel, as net80211 registers */
	for (int ch = first; ch <= last; ch += step) {
		for (int ht = 0; ht < 2; ht++) {
			struct ieee80211_channel *c = &ic->ic_channels[ic->ic_nchans++];
			c->ic_flags = band | (ht ? IEEE80211_CHAN_HT20 : 0);
			c->ic_ieee = ch;
			c->ic_maxpower = 40;	/* .5 dBm units: 20 dBm */
			c->ic_minpower = 0;
		}
	}
}

int
main(void)
{
	static struct ieee80211com ic;
	add(&ic, IEEE80211_CHAN_2GHZ, 1, 13, 1);
	add(&ic, IEEE80211_CHAN_5GHZ, 36, 64, 4);
	add(&ic, IEEE80211_CHAN_5GHZ, 100, 144, 4);
	add(&ic, IEEE80211_CHAN_5GHZ, 149, 165, 4);
	uint8_t buf[2 + 26 + 8], *end;

	/* Power Capability is in dBm */
	end = ieee80211_add_powercapability(buf, &ic.ic_channels[0]);
	assert(end == buf + 4);
	assert(buf[0] == 33 && buf[1] == 2 && buf[2] == 0 && buf[3] == 20);

	/* 5 GHz: (first, count) tuples for runs spaced 4 apart */
	const uint8_t want5[] = { 36, 2 * 3, 36, 8, 100, 12, 149, 5 };
	ic.ic_curchan = &ic.ic_channels[2 * 13 + 2 * 3];	/* channel 48 */
	memset(buf, 0xee, sizeof(buf));
	end = ieee80211_add_supportedchannels(buf, &ic);
	assert(end == buf + 2 + 6);
	assert(buf[0] == want5[0] && buf[1] == want5[1]);
	assert(memcmp(buf + 2, want5 + 2, 6) == 0);
	assert(buf[8] == 0xee);

	/* 2.4 GHz: one run of 13 */
	ic.ic_curchan = &ic.ic_channels[0];
	end = ieee80211_add_supportedchannels(buf, &ic);
	assert(end == buf + 4 && buf[1] == 2 && buf[2] == 1 && buf[3] == 13);

	/* never more than the 13 tuples the caller reserves room for */
	static struct ieee80211com sparse;
	add(&sparse, IEEE80211_CHAN_5GHZ, 36, 36 + 8 * 20, 8);
	sparse.ic_curchan = &sparse.ic_channels[0];
	memset(buf, 0xee, sizeof(buf));
	end = ieee80211_add_supportedchannels(buf, &sparse);
	assert(buf[1] == 26 && end == buf + 28 && buf[28] == 0xee);

	puts("PASS 802.11h Power Capability in dBm, Supported Channels as tuples");
	return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'doth_elements.c'
    c.write_text(fixture + function('ieee80211_add_powercapability')
        + function('ieee80211_add_supportedchannels') + checks)
    exe = c.with_suffix('')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Werror',
        '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
