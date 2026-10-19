#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run ht_recv_action_ba_addba_request() from ieee80211_ht.c: an ADDBA
request the driver's ic_ampdu_rx_start() could not set up is answered with
IEEE80211_STATUS_UNSPECIFIED and counted in is_addba_reject; one it set up
is still accepted."""
from pathlib import Path
import os
import subprocess
import tempfile

root = (Path(os.environ['SOURCE_ROOT']) if 'SOURCE_ROOT' in os.environ
        else Path(__file__).resolve().parents[5])
source = (root / 'src/libs/compat/freebsd_wlan/net80211/ieee80211_ht.c').read_text()
start = source.index('static int\nht_recv_action_ba_addba_request(')
function = source[start:source.index('\n}\n', start) + 3]

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define __HAIKU__ 1
#define IEEE80211_NOTE(...) do { } while (0)
#define _IEEE80211_MASKSHIFT(_v, _f) (((_v) & _f) >> _f##_S)
#define _IEEE80211_SHIFTMASK(_v, _f) (((_v) << _f##_S) & _f)
#define IEEE80211_BAPS_BUFSIZ 0xffc0
#define IEEE80211_BAPS_BUFSIZ_S 6
#define IEEE80211_BAPS_TID 0x003c
#define IEEE80211_BAPS_TID_S 2
#define IEEE80211_BAPS_AMSDU 0x0001
#define IEEE80211_BAPS_POLICY_IMMEDIATE 0x0002
#define IEEE80211_NODE_AMPDU_RX 0x1
#define IEEE80211_NODE_AMSDU_RX 0x2
#define IEEE80211_FHT_AMPDU_RX 0x1
#define IEEE80211_HTC_RX_AMSDU_AMPDU 0x1
#define IEEE80211_ACTION_CAT_BA 3
#define IEEE80211_ACTION_BA_ADDBA_RESPONSE 1
enum { IEEE80211_STATUS_SUCCESS = 0, IEEE80211_STATUS_UNSPECIFIED = 1 };
struct ieee80211_frame;
struct ieee80211_rx_ampdu { int rxa_wnd; };
struct ieee80211_node;
struct ieee80211com {
	int (*ic_ampdu_rx_start)(struct ieee80211_node *,
	    struct ieee80211_rx_ampdu *, int, int, int);
	int (*ic_send_action)(struct ieee80211_node *, int, int, void *);
};
struct ieee80211vap { int iv_flags_ht, iv_htcaps;
	struct { int is_addba_reject; } iv_stats; };
struct ieee80211_node { struct ieee80211com *ni_ic; struct ieee80211vap *ni_vap;
	int ni_flags; struct ieee80211_rx_ampdu ni_rx_ampdu[16]; };
static uint16_t
le16dec(const void *p)
{
	const uint8_t *b = p;
	return b[0] | b[1] << 8;
}
'''
checks = r'''
static int driver_error, started, sent_status = -1;

static int
driver_rx_start(struct ieee80211_node *ni, struct ieee80211_rx_ampdu *rap,
    int baparamset, int batimeout, int baseqctl)
{
	assert(rap == &ni->ni_rx_ampdu[5]);
	started++;
	return driver_error;
}

static int
send_action(struct ieee80211_node *ni, int cat, int act, void *sa)
{
	assert(cat == IEEE80211_ACTION_CAT_BA);
	assert(act == IEEE80211_ACTION_BA_ADDBA_RESPONSE);
	sent_status = ((uint16_t *)sa)[1];
	return 0;
}

static int
run(int error, int *rejects)
{
	struct ieee80211com ic = { driver_rx_start, send_action };
	struct ieee80211vap vap = { IEEE80211_FHT_AMPDU_RX };
	struct ieee80211_node ni = { &ic, &vap, IEEE80211_NODE_AMPDU_RX };
	/* category, action, token 7, baparamset tid 5 bufsiz 64, timeout, seqctl */
	const uint16_t baps = 64 << IEEE80211_BAPS_BUFSIZ_S | 5 << IEEE80211_BAPS_TID_S;
	const uint8_t frm[9] = { 3, 0, 7, baps & 0xff, baps >> 8, 0, 0, 0x10, 0 };

	driver_error = error;
	started = 0;
	sent_status = -1;
	assert(ht_recv_action_ba_addba_request(&ni, NULL, frm, frm + 9) == 0);
	assert(started == 1);
	*rejects = vap.iv_stats.is_addba_reject;
	printf("driver returned %d: status %d, is_addba_reject %d\n", error,
	    sent_status, *rejects);
	return sent_status;
}

int
main(void)
{
	int rejects;

	/* the driver set the session up: accept */
	assert(run(0, &rejects) == IEEE80211_STATUS_SUCCESS);
	assert(rejects == 0);
	/* the driver failed (e.g. ENOENT for an unknown sta): reject */
	assert(run(2, &rejects) == IEEE80211_STATUS_UNSPECIFIED);
	assert(rejects == 1);
	puts("PASS ADDBA rejected when the driver could not set it up");
	return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'addba_reject.c'
    c.write_text(fixture + function + checks)
    exe = c.with_suffix('')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Werror',
        '-Wno-unused-function', '-fsanitize=address,undefined', str(c), '-o',
        str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
