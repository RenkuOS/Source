#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run ieee80211_scanreq() from ieee80211_ioctl.c in INIT: a scan request
the state machine discarded (-1) or dropped (EAGAIN) fails with EBUSY unless
the last queued transition is a SCAN, and FEXT_SCANREQ is only cleared when
no SCAN is queued. Errnos are negative, as on Haiku."""
from pathlib import Path
import os
import subprocess
import tempfile

root = (Path(os.environ['SOURCE_ROOT']) if 'SOURCE_ROOT' in os.environ
        else Path(__file__).resolve().parents[5])
source = (root / 'src/libs/compat/freebsd_wlan/net80211/ieee80211_ioctl.c').read_text()
start = source.index('static int\nieee80211_scanreq(')
function = source[start:source.index('\n}\n', start) + 3]

fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define __HAIKU__ 1
#define EINVAL (-22)
#define ENXIO (-6)
#define EAGAIN (-11)
#define EBUSY (-16)
#define EINPROGRESS (-36)
#define IEEE80211_IOC_SCAN_NOPICK 0x00001
#define IEEE80211_IOC_SCAN_ACTIVE 0x00002
#define IEEE80211_IOC_SCAN_PICK1ST 0x00004
#define IEEE80211_IOC_SCAN_BGSCAN 0x00008
#define IEEE80211_IOC_SCAN_ONCE 0x00010
#define IEEE80211_IOC_SCAN_NOBCAST 0x00020
#define IEEE80211_IOC_SCAN_NOJOIN 0x00040
#define IEEE80211_IOC_SCAN_FLUSH 0x10000
#define IEEE80211_IOC_SCAN_CHECK 0x20000
#define IEEE80211_IOC_SCAN_FOREVER 0x7fffffff
#define IEEE80211_SCAN_FOREVER 0x7fffffff
#define IEEE80211_IOC_SCAN_DURATION_MIN 1
#define IEEE80211_IOC_SCAN_DURATION_MAX 0x7fffffff
#define IEEE80211_SCAN_MAX_SSID 1
#define IEEE80211_NWID_LEN 32
#define IEEE80211_C_BGSCAN 0x1
#define IEEE80211_F_BGSCAN 0x1
#define IEEE80211_M_STA 1
#define IFF_UP 0x1
#define IEEE80211_FEXT_SCANREQ 0x2
#define NET80211_IV_NSTATE_NUM 8
#define msecs_to_ticks(x) (x)
enum ieee80211_state { IEEE80211_S_INIT, IEEE80211_S_SCAN, IEEE80211_S_AUTH,
	IEEE80211_S_RUN = 5 };
static const char *ieee80211_state_name[8];
#define IEEE80211_DPRINTF(...) ((void)ieee80211_state_name)
struct ieee80211_scan_ssid { int len; unsigned char ssid[IEEE80211_NWID_LEN]; };
struct ieee80211_scan_req {
	int sr_flags, sr_duration, sr_mindwell, sr_maxdwell, sr_nssid;
	struct ieee80211_scan_ssid sr_ssid[IEEE80211_SCAN_MAX_SSID];
};
struct ifnet { int if_flags; };
struct ieee80211com { int ic_nrunning; int locked; };
struct ieee80211vap {
	struct ieee80211com *iv_ic; struct ifnet *iv_ifp;
	int iv_caps, iv_flags, iv_flags_ext, iv_opmode;
	enum ieee80211_state iv_state;
	enum ieee80211_state iv_nstates[NET80211_IV_NSTATE_NUM];
	int iv_nstate_b, iv_nstate_n;
	int iv_scanreq_flags, iv_scanreq_duration, iv_scanreq_nssid;
	struct ieee80211_scan_ssid iv_scanreq_ssid[IEEE80211_SCAN_MAX_SSID];
};
#define IEEE80211_LOCK(ic) do { assert(!(ic)->locked); (ic)->locked = 1; } while (0)
#define IEEE80211_UNLOCK(ic) do { assert((ic)->locked); (ic)->locked = 0; } while (0)
static int scripted, calls;
static int
ieee80211_new_state_locked(struct ieee80211vap *vap, enum ieee80211_state s,
    int arg)
{
	assert(vap->iv_ic->locked && s == IEEE80211_S_SCAN);
	calls++;
	return scripted;
}
static int
ieee80211_new_state(struct ieee80211vap *vap, enum ieee80211_state s, int arg)
{
	assert(!vap->iv_ic->locked && s == IEEE80211_S_SCAN);
	calls++;
	return scripted;
}
static int
ieee80211_check_scan(struct ieee80211vap *vap, int f, int d, int mn, int mx,
    int n, const struct ieee80211_scan_ssid *s)
{
	return 0;
}
static int
ieee80211_start_scan(struct ieee80211vap *vap, int f, int d, int mn, int mx,
    int n, const struct ieee80211_scan_ssid *s)
{
	return 0;
}
'''
checks = r'''
static int flags_ext;

static int
run(int result, int base, int n, const enum ieee80211_state *queue)
{
	struct ieee80211com ic = { 1, 0 };
	struct ifnet ifp = { IFF_UP };
	struct ieee80211vap vap;
	struct ieee80211_scan_req sr = { IEEE80211_IOC_SCAN_ACTIVE, 100, 0, 0, 0 };
	int error, i;
	memset(&vap, 0, sizeof(vap));
	vap.iv_ic = &ic;
	vap.iv_ifp = &ifp;
	vap.iv_opmode = IEEE80211_M_STA;
	vap.iv_state = IEEE80211_S_INIT;
	vap.iv_nstate_b = base;
	vap.iv_nstate_n = n;
	for (i = 0; i < NET80211_IV_NSTATE_NUM; i++)
		vap.iv_nstates[i] = IEEE80211_S_RUN;	/* stale slots */
	for (i = 0; i < n; i++)
		vap.iv_nstates[(base + i) % NET80211_IV_NSTATE_NUM] = queue[i];
	scripted = result;
	calls = 0;
	error = ieee80211_scanreq(&vap, &sr);
	assert(!ic.locked);
	assert(calls == 1);
	flags_ext = vap.iv_flags_ext;
	return error;
}

#define SCANREQ_KEPT (flags_ext & IEEE80211_FEXT_SCANREQ)

int
main(void)
{
	const enum ieee80211_state scan[] = { IEEE80211_S_SCAN };
	const enum ieee80211_state scan_init[] = { IEEE80211_S_SCAN, IEEE80211_S_INIT };
	const enum ieee80211_state init[] = { IEEE80211_S_INIT };
	const enum ieee80211_state wrap[] = { IEEE80211_S_INIT, IEEE80211_S_SCAN };

	/* (a) queued: success, the SCAN will use the stashed params */
	assert(run(EINPROGRESS, 0, 0, NULL) == 0 && SCANREQ_KEPT);
	/* (b) discarded with nothing queued: no scan-done would ever come */
	assert(run(-1, 0, 0, NULL) == EBUSY && !SCANREQ_KEPT);
	/* (c) discarded, but a SCAN is already the last queued transition */
	assert(run(-1, 0, 1, scan) == 0 && SCANREQ_KEPT);
	/* ...also when the ring wraps */
	assert(run(-1, 7, 2, wrap) == 0 && SCANREQ_KEPT);
	/* (d) a queued SCAN followed by INIT: fail, but leave its SCANREQ */
	assert(run(-1, 0, 2, scan_init) == EBUSY && SCANREQ_KEPT);
	/* (e) dropped with full task slots, only an INIT queued */
	assert(run(EAGAIN, 3, 1, init) == EBUSY && !SCANREQ_KEPT);
	puts("PASS dropped scan requests fail");
	return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'scanreq_dropped.c'
    c.write_text(fixture + function + checks)
    exe = c.with_suffix('')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Werror',
        '-Wno-unused-function', '-fsanitize=address,undefined', str(c), '-o',
        str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
