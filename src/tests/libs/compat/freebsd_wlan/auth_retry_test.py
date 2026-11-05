#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run ieee80211_tx_mgt_timeout() and ieee80211_tx_auth_retry() from
ieee80211_output.c: an open-system STA authentication whose response was
lost is retried, sharing the two-retry budget with a failed AUTH transmit,
before the vap falls back to SCAN. Shared-key authentication, ASSOC, a
queued transition, a running scan and INIT keep the old behaviour."""
from pathlib import Path
import os
import subprocess
import tempfile

root = (Path(os.environ['SOURCE_ROOT']) if 'SOURCE_ROOT' in os.environ
        else Path(__file__).resolve().parents[5])
source = (root / 'src/libs/compat/freebsd_wlan/net80211/ieee80211_output.c').read_text()
# From the end of the function before the AUTH retry code to the end of
# ieee80211_tx_mgt_timeout().
start = source.rindex('\n}\n', 0, source.index('\nieee80211_tx_auth_retry')) + 3
timeout = source.index('\nieee80211_tx_mgt_timeout(void *arg)\n{', start)
function = source[start:source.index('\n}\n', timeout) + 3]

fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define IEEE80211_M_STA 1
#define IEEE80211_AUTH_OPEN 1
#define IEEE80211_AUTH_SHARED 2
#define IEEE80211_F_SCAN 0x00000080
#define IEEE80211_FC0_SUBTYPE_DEAUTH 0xc0
#define IEEE80211_STATUS_TIMEOUT 16
#define IEEE80211_SCAN_FAIL_TIMEOUT 1
#define AUTH_RETRY_ARG ((IEEE80211_STATUS_TIMEOUT << 8) | \
	IEEE80211_FC0_SUBTYPE_DEAUTH)
enum ieee80211_state { IEEE80211_S_INIT, IEEE80211_S_SCAN, IEEE80211_S_AUTH,
	IEEE80211_S_ASSOC, IEEE80211_S_CAC, IEEE80211_S_RUN };
static const char *ieee80211_state_name[8];
static const char *ieee80211_opmode_name[8];
#define IEEE80211_DPRINTF(...) \
	((void)ieee80211_state_name, (void)ieee80211_opmode_name)
struct ieee80211com { int ic_flags; int locked; };
struct ieee80211_node { int ni_authmode; uint64_t ni_auth_tx_retries; };
struct ieee80211vap {
	struct ieee80211com *iv_ic;
	struct ieee80211_node *iv_bss;
	int iv_opmode, iv_nstate_n;
	enum ieee80211_state iv_state;
};
#define IEEE80211_LOCK(ic) do { assert(!(ic)->locked); (ic)->locked = 1; } while (0)
#define IEEE80211_UNLOCK(ic) do { assert((ic)->locked); (ic)->locked = 0; } while (0)
#define IEEE80211_LOCK_ASSERT(ic) assert((ic)->locked)
static int calls;
static enum ieee80211_state last_state;
static int last_arg;
/* AUTH -> AUTH keeps iv_state and the retry budget, as sta_newstate does. */
static int
ieee80211_new_state_locked(struct ieee80211vap *vap, enum ieee80211_state s,
    int arg)
{
	assert(vap->iv_ic->locked);
	calls++;
	last_state = s;
	last_arg = arg;
	vap->iv_state = s;
	return 0;
}
'''
checks = r'''
static struct ieee80211com ic;
static struct ieee80211_node bss;
static struct ieee80211vap vap;

static void
setup(enum ieee80211_state state, int authmode)
{
	memset(&ic, 0, sizeof(ic));
	memset(&bss, 0, sizeof(bss));
	memset(&vap, 0, sizeof(vap));
	bss.ni_authmode = authmode;
	vap.iv_ic = &ic;
	vap.iv_bss = &bss;
	vap.iv_opmode = IEEE80211_M_STA;
	vap.iv_state = state;
}

/* Fire one callout; returns the number of state requests it made. */
static int
fire(void (*callout)(void *))
{
	calls = 0;
	callout(&vap);
	assert(!ic.locked);
	return calls;
}

#define RETRIED (last_state == IEEE80211_S_AUTH && last_arg == AUTH_RETRY_ARG)
#define FELL_BACK (last_state == IEEE80211_S_SCAN && \
	last_arg == IEEE80211_SCAN_FAIL_TIMEOUT)

int
main(void)
{
	/* (a) lost AUTH responses: two retries, then SCAN */
	setup(IEEE80211_S_AUTH, IEEE80211_AUTH_OPEN);
	assert(fire(ieee80211_tx_mgt_timeout) == 1 && RETRIED);
	assert(bss.ni_auth_tx_retries == 1);
	assert(fire(ieee80211_tx_mgt_timeout) == 1 && RETRIED);
	assert(bss.ni_auth_tx_retries == 2);
	assert(fire(ieee80211_tx_mgt_timeout) == 1 && FELL_BACK);
	assert(bss.ni_auth_tx_retries == 2);

	/* (b) a failed AUTH transmit and lost responses share the budget */
	setup(IEEE80211_S_AUTH, IEEE80211_AUTH_OPEN);
	assert(fire(ieee80211_tx_auth_retry) == 1 && RETRIED);
	assert(fire(ieee80211_tx_mgt_timeout) == 1 && RETRIED);
	assert(fire(ieee80211_tx_auth_retry) == 1 && FELL_BACK);
	setup(IEEE80211_S_AUTH, IEEE80211_AUTH_OPEN);
	assert(fire(ieee80211_tx_mgt_timeout) == 1 && RETRIED);
	assert(fire(ieee80211_tx_auth_retry) == 1 && RETRIED);
	assert(fire(ieee80211_tx_mgt_timeout) == 1 && FELL_BACK);

	/* (c) shared-key authentication is not retried */
	setup(IEEE80211_S_AUTH, IEEE80211_AUTH_SHARED);
	assert(fire(ieee80211_tx_mgt_timeout) == 1 && FELL_BACK);
	assert(bss.ni_auth_tx_retries == 0);

	/* (d) a lost ASSOC response still goes straight to SCAN */
	setup(IEEE80211_S_ASSOC, IEEE80211_AUTH_OPEN);
	assert(fire(ieee80211_tx_mgt_timeout) == 1 && FELL_BACK);
	assert(bss.ni_auth_tx_retries == 0);

	/* (e) a queued transition: no retry; the timeout keeps its fallback */
	setup(IEEE80211_S_AUTH, IEEE80211_AUTH_OPEN);
	vap.iv_nstate_n = 1;
	assert(fire(ieee80211_tx_auth_retry) == 0);
	assert(fire(ieee80211_tx_mgt_timeout) == 1 && FELL_BACK);
	assert(bss.ni_auth_tx_retries == 0);

	/* (f) no BSS node: no retry */
	setup(IEEE80211_S_AUTH, IEEE80211_AUTH_OPEN);
	vap.iv_bss = NULL;
	assert(fire(ieee80211_tx_auth_retry) == 0);
	assert(fire(ieee80211_tx_mgt_timeout) == 1 && FELL_BACK);

	/* (g) scanning or INIT: neither callout acts */
	setup(IEEE80211_S_AUTH, IEEE80211_AUTH_OPEN);
	ic.ic_flags = IEEE80211_F_SCAN;
	assert(fire(ieee80211_tx_mgt_timeout) == 0);
	assert(fire(ieee80211_tx_auth_retry) == 0);
	setup(IEEE80211_S_INIT, IEEE80211_AUTH_OPEN);
	assert(fire(ieee80211_tx_mgt_timeout) == 0);
	assert(fire(ieee80211_tx_auth_retry) == 0);
	assert(bss.ni_auth_tx_retries == 0);

	puts("PASS lost authentication responses are retried");
	return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'auth_retry.c'
    c.write_text(fixture + function + checks)
    exe = c.with_suffix('')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Werror',
        '-Wno-unused-function', '-fsanitize=address,undefined', str(c), '-o',
        str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
