#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the vap SIOCSIFMEDIA request filter from ieee80211_ioctl() on the
media word wpa_supplicant writes back (Haiku's active media word)."""
from pathlib import Path
import os
import subprocess
import tempfile

root = (Path(os.environ['SOURCE_ROOT']) if 'SOURCE_ROOT' in os.environ
        else Path(__file__).resolve().parents[5])
source = (root / 'src/libs/compat/freebsd_wlan/net80211/ieee80211_ioctl.c').read_text()
start = source.index('\t\tif (cmd == SIOCSIFMEDIA && vap->iv_media.ifm_cur != NULL) {')
block = source[start:source.index('\n\t\t}\n', start) + 4]

fixture = r'''
#include <assert.h>
#include <stdio.h>
#define SIOCSIFMEDIA 1
#define SIOCGIFMEDIA 2
#define IFM_IEEE80211 0x00000080
#define IFM_AUTO 0
#define IFM_IEEE80211_MCS 24
#define IFM_IEEE80211_ADHOC 0x00000100
#define IFM_IEEE80211_11NA 0x00050000
#define IFM_IEEE80211_11NG 0x00060000
#define IFM_OMASK 0x0000ff00
struct ifmedia_entry { int ifm_media; };
struct ifmedia { struct ifmedia_entry *ifm_cur; };
struct ieee80211vap { struct ifmedia iv_media; };
struct ifreq { int ifr_media; };

static void
filter(struct ieee80211vap *vap, struct ifreq *ifr, unsigned long cmd)
{
'''
checks = r'''
}

int
main(void)
{
	struct ifmedia_entry autoselect = { IFM_IEEE80211 | IFM_AUTO };
	struct ieee80211vap vap = { { &autoselect } };
	/* What Haiku's SIOCGIFMEDIA returns while associated on 5/2.4 GHz HT. */
	const int active[] = {
		IFM_IEEE80211 | IFM_IEEE80211_11NA | IFM_IEEE80211_MCS,
		IFM_IEEE80211 | IFM_IEEE80211_11NG | IFM_IEEE80211_MCS,
	};
	for (unsigned i = 0; i < sizeof(active) / sizeof(active[0]); i++) {
		/* bsd_set_mediaopt(drv, IFM_OMASK, 0): clear options, write back */
		struct ifreq ifr = { active[i] & ~IFM_OMASK };
		filter(&vap, &ifr, SIOCSIFMEDIA);
		assert(ifr.ifr_media == (IFM_IEEE80211 | IFM_AUTO));

		/* option bits are still taken from the request (IBSS) */
		ifr.ifr_media = (active[i] & ~IFM_OMASK) | IFM_IEEE80211_ADHOC;
		filter(&vap, &ifr, SIOCSIFMEDIA);
		assert(ifr.ifr_media == (IFM_IEEE80211 | IFM_IEEE80211_ADHOC));
	}

	/* reads and a vap without configured media are left alone */
	struct ifreq get = { active[0] };
	filter(&vap, &get, SIOCGIFMEDIA);
	assert(get.ifr_media == active[0]);
	struct ieee80211vap bare = { { NULL } };
	struct ifreq set = { active[0] };
	filter(&bare, &set, SIOCSIFMEDIA);
	assert(set.ifr_media == active[0]);

	puts("PASS SIOCSIFMEDIA keeps the configured mode, takes only option bits");
	return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'media_request.c'
    c.write_text(fixture + block + checks)
    exe = c.with_suffix('')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Werror',
        '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
