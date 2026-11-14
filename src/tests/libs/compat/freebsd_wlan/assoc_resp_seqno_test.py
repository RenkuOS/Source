#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run sta_input()'s management-frame duplicate check from
ieee80211_sta.c: a retried (re)assoc response reusing the auth response's
sequence number is accepted in ASSOC; everything else is still checked."""
from pathlib import Path
import os
import subprocess
import tempfile

root = (Path(os.environ['SOURCE_ROOT']) if 'SOURCE_ROOT' in os.environ
        else Path(__file__).resolve().parents[5])
source = (root / 'src/libs/compat/freebsd_wlan/net80211/ieee80211_sta.c').read_text()
anchor = source.index('Some APs (TP-Link Deco Wi-Fi 7 seen)')
start = source.rindex('#if defined(__HAIKU__)', 0, anchor)
end = source.index('goto out;', anchor) + len('goto out;')
block = source[start:end]

fixture = r'''
#include <assert.h>
#include <stdio.h>
#define __HAIKU__ 1
#define IEEE80211_FC0_TYPE_MGT 0x00
#define IEEE80211_FC0_TYPE_DATA 0x08
#define IEEE80211_FC0_SUBTYPE_ASSOC_RESP 0x10
#define IEEE80211_FC0_SUBTYPE_REASSOC_RESP 0x30
#define IEEE80211_FC0_SUBTYPE_AUTH 0xb0
enum { IEEE80211_S_SCAN = 1, IEEE80211_S_AUTH, IEEE80211_S_ASSOC,
	IEEE80211_S_RUN = 5 };
struct ieee80211vap { int iv_state; };
static int duplicate, checked;
static int
ieee80211_check_rxseq(void *ni, void *wh, void *bssid, void *rxs)
{
	checked++;
	return !duplicate;
}

/* 1 if the frame is dropped */
static int
dropped(struct ieee80211vap *vap, int type, int subtype)
{
	void *ni = 0, *wh = 0, *bssid = 0, *rxs = 0;
'''
checks = r'''
	return 0;
out:
	return 1;
}

int
main(void)
{
	struct ieee80211vap vap = { IEEE80211_S_ASSOC };
	duplicate = 1;	/* same seqno as the auth response */
	assert(!dropped(&vap, IEEE80211_FC0_TYPE_MGT,
	    IEEE80211_FC0_SUBTYPE_ASSOC_RESP));
	assert(!dropped(&vap, IEEE80211_FC0_TYPE_MGT,
	    IEEE80211_FC0_SUBTYPE_REASSOC_RESP));
	assert(checked == 0);

	/* auth retries, other states and data frames are still filtered */
	assert(dropped(&vap, IEEE80211_FC0_TYPE_MGT, IEEE80211_FC0_SUBTYPE_AUTH));
	vap.iv_state = IEEE80211_S_RUN;
	assert(dropped(&vap, IEEE80211_FC0_TYPE_MGT,
	    IEEE80211_FC0_SUBTYPE_ASSOC_RESP));
	vap.iv_state = IEEE80211_S_AUTH;
	assert(dropped(&vap, IEEE80211_FC0_TYPE_MGT,
	    IEEE80211_FC0_SUBTYPE_ASSOC_RESP));
	vap.iv_state = IEEE80211_S_ASSOC;
	assert(dropped(&vap, IEEE80211_FC0_TYPE_DATA, 0));
	duplicate = 0;
	assert(!dropped(&vap, IEEE80211_FC0_TYPE_MGT, IEEE80211_FC0_SUBTYPE_AUTH));
	assert(checked == 5);

	puts("PASS assoc responses reusing a seqno are accepted in ASSOC only");
	return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'assoc_resp_seqno.c'
    c.write_text(fixture + block + checks)
    exe = c.with_suffix('')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Werror',
        '-Wno-unused-parameter', '-fsanitize=address,undefined', str(c), '-o',
        str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
