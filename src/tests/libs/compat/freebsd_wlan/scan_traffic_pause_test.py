#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run cancel_scan() from ieee80211_scan_sw.c: outbound traffic
(cancel_anyscan) may pause a background scan but not a requested one;
explicit cancels still work."""
from pathlib import Path
import os
import subprocess
import tempfile

root = (Path(os.environ['SOURCE_ROOT']) if 'SOURCE_ROOT' in os.environ
        else Path(__file__).resolve().parents[5])
source = (root / 'src/libs/compat/freebsd_wlan/net80211/ieee80211_scan_sw.c').read_text()
start = source.index('static void\ncancel_scan(')
function = source[start:source.index('\n}\n', start) + 3]

fixture = r'''
#include <assert.h>
#include <stdio.h>
#define __HAIKU__ 1
#define IEEE80211_F_SCAN 0x00000080
#define IEEE80211_SCAN_NOPICK 0x0001
#define IEEE80211_SCAN_ACTIVE 0x0002
#define IEEE80211_SCAN_BGSCAN 0x0008
#define ISCAN_INTERRUPT 0x0004
#define ISCAN_CANCEL 0x0008
#define ISCAN_PAUSE (ISCAN_INTERRUPT | ISCAN_CANCEL)
struct ieee80211vap;
struct ieee80211_scan_state { struct ieee80211vap *ss_vap; int ss_flags; };
struct scan_state { struct ieee80211_scan_state base; unsigned ss_iflags; };
#define SCAN_PRIVATE(ss) ((struct scan_state *)(ss))
struct ieee80211com { struct ieee80211_scan_state *ic_scan; int ic_flags; int locked; };
struct ieee80211vap { struct ieee80211com *iv_ic; };
#define IEEE80211_LOCK(ic) do { assert(!(ic)->locked); (ic)->locked = 1; } while (0)
#define IEEE80211_UNLOCK(ic) do { assert((ic)->locked); (ic)->locked = 0; } while (0)
#define IEEE80211_DPRINTF(...) do { } while (0)
static unsigned signalled;
static void
scan_signal_locked(struct ieee80211_scan_state *ss, int signal)
{
	SCAN_PRIVATE(ss)->ss_iflags |= signal;
	signalled = signal;
}
'''
checks = r'''
static unsigned
run(int ssflags, int any)
{
	struct scan_state st = { { 0, ssflags }, 0 };
	struct ieee80211com ic = { &st.base, IEEE80211_F_SCAN, 0 };
	struct ieee80211vap vap = { &ic };
	st.base.ss_vap = &vap;
	signalled = 0;
	cancel_scan(&vap, any, "test");
	assert(!ic.locked);
	return signalled;
}

int
main(void)
{
	/* traffic does not pause a requested (foreground) scan... */
	assert(run(IEEE80211_SCAN_ACTIVE, 1) == 0);
	assert(run(IEEE80211_SCAN_ACTIVE | IEEE80211_SCAN_NOPICK, 1) == 0);
	/* ...but still pauses a background scan */
	assert(run(IEEE80211_SCAN_BGSCAN, 1) == ISCAN_PAUSE);
	/* explicit cancels are unchanged */
	assert(run(IEEE80211_SCAN_ACTIVE, 0) == ISCAN_CANCEL);
	assert(run(IEEE80211_SCAN_BGSCAN, 0) == ISCAN_CANCEL);
	puts("PASS traffic only pauses background scans");
	return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'scan_traffic_pause.c'
    c.write_text(fixture + function + checks)
    exe = c.with_suffix('')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Werror',
        '-Wno-unused-function', '-fsanitize=address,undefined', str(c), '-o',
        str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
