# SPDX-License-Identifier: MIT
"""Exercise production authentication timer/state handling with ASan/UBSan."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[5]
net = root / 'src/libs/compat/freebsd_wlan/net80211'
source = (net / 'ieee80211_output.c').read_text()
current = source
if os.environ.get('AUTH_RETRY_BEFORE'):
    source = Path(os.environ['AUTH_RETRY_BEFORE']).read_text()

def function(text, name):
    start = text.index('\n' + name + '(')
    start = text.rfind('\nstatic void\n', 0, start + 1) + 1
    return text[start:text.index('\n}\n', start) + 3]

sta = (net / 'ieee80211_sta.c').read_text()
start = sta.index('\tif (ni != NULL &&\n\t    (nstate != IEEE80211_S_AUTH')
reset = sta[start:sta.index('ni->ni_auth_tx_retries = 0;', start) + len('ni->ni_auth_tx_retries = 0;')]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define IEEE80211_S_INIT 0
#define IEEE80211_S_SCAN 1
#define IEEE80211_S_AUTH 2
#define IEEE80211_S_ASSOC 3
#define IEEE80211_S_RUN 4
#define IEEE80211_F_SCAN 1
#define IEEE80211_M_STA 1
#define IEEE80211_M_HOSTAP 2
#define IEEE80211_AUTH_SHARED 2
#define IEEE80211_STATUS_TIMEOUT 16
#define IEEE80211_FC0_SUBTYPE_DEAUTH 0xc0
#define IEEE80211_SCAN_FAIL_TIMEOUT 1
#define IEEE80211_TRANS_WAIT 5
#define IEEE80211_DPRINTF(...) ((void)0)
#define MAX(a,b) ((a)>(b)?(a):(b))
enum ieee80211_state { UNUSED_STATE };
struct ieee80211com { unsigned ic_flags; int locked; };
struct callout { int delay; void(*fn)(void*); void*arg; unsigned resets; };
struct ieee80211vap;
struct ieee80211_node { struct ieee80211vap*ni_vap; unsigned ni_authmode; uint64_t ni_auth_tx_retries; };
struct ieee80211vap { struct ieee80211com*iv_ic; int iv_state,iv_opmode,iv_nstate_n; struct ieee80211_node*iv_bss; struct callout iv_mgtsend; };
static int hz=100,transitions,nextState,nextArg;
#define IEEE80211_LOCK(ic) do { assert(!(ic)->locked);(ic)->locked=1; } while(0)
#define IEEE80211_UNLOCK(ic) do { assert((ic)->locked);(ic)->locked=0; } while(0)
static void ieee80211_new_state_locked(struct ieee80211vap*v,int state,int arg) {
 assert(v->iv_ic->locked);transitions++;nextState=state;nextArg=arg;
}
static void callout_reset(struct callout*c,int delay,void(*fn)(void*),void*arg) {
 c->delay=delay;c->fn=fn;c->arg=arg;c->resets++;
}
''' + function(current, 'ieee80211_tx_auth_retry') + function(source, 'ieee80211_tx_mgt_timeout') + function(source, 'ieee80211_tx_mgt_cb') + r'''
static void reset_counter(struct ieee80211vap*v,int ostate,int nstate) {
 struct ieee80211_node*ni=v->iv_bss;
''' + reset + r'''
}
int main(void) {
 struct ieee80211com ic={0};
 struct ieee80211vap vap={.iv_ic=&ic,.iv_state=IEEE80211_S_AUTH,.iv_opmode=IEEE80211_M_STA};
 struct ieee80211_node node={.ni_vap=&vap};vap.iv_bss=&node;
 for(unsigned attempt=0;attempt<3;attempt++) {
  transitions=0;ieee80211_tx_mgt_cb(&node,(void*)(uintptr_t)IEEE80211_S_AUTH,1);
  assert(vap.iv_mgtsend.delay==20 && vap.iv_mgtsend.fn==ieee80211_tx_auth_retry);
  vap.iv_mgtsend.fn(vap.iv_mgtsend.arg);
  assert(transitions==1 && !ic.locked);
  if(attempt<2) {
   assert(nextState==IEEE80211_S_AUTH && nextArg==((16<<8)|0xc0));
   assert(node.ni_auth_tx_retries==attempt+1);
   reset_counter(&vap,IEEE80211_S_AUTH,IEEE80211_S_AUTH);
  } else assert(nextState==IEEE80211_S_SCAN && nextArg==IEEE80211_SCAN_FAIL_TIMEOUT);
 }
 for(int old=0;old<=4;old++) for(int next=0;next<=4;next++) {
  node.ni_auth_tx_retries=2;reset_counter(&vap,old,next);
  assert(node.ni_auth_tx_retries==((old==2&&next==2)?2:0));
 }
 // A stopped callout that had already begun cannot override a state change.
 for(int state=0;state<=4;state++) {
  if(state==IEEE80211_S_AUTH)continue;
  vap.iv_state=state;transitions=0;ieee80211_tx_auth_retry(&vap);assert(!transitions&&!ic.locked);
 }
 vap.iv_state=IEEE80211_S_AUTH;
 ic.ic_flags=IEEE80211_F_SCAN;transitions=0;ieee80211_tx_auth_retry(&vap);assert(!transitions);ic.ic_flags=0;
 vap.iv_nstate_n=1;ieee80211_tx_auth_retry(&vap);assert(!transitions);vap.iv_nstate_n=0;
 vap.iv_bss=NULL;ieee80211_tx_auth_retry(&vap);assert(!transitions);vap.iv_bss=&node;
 // Late results from a previous AP must not replace this AP's timer.
 struct ieee80211_node stale={.ni_vap=&vap};unsigned before=vap.iv_mgtsend.resets;
 ieee80211_tx_mgt_cb(&stale,(void*)(uintptr_t)IEEE80211_S_AUTH,1);
 assert(vap.iv_mgtsend.resets==before);
 // Successful TX and other exchange types retain their existing policy.
 ieee80211_tx_mgt_cb(&node,(void*)(uintptr_t)IEEE80211_S_AUTH,0);
 assert(vap.iv_mgtsend.delay==5*hz&&vap.iv_mgtsend.fn==ieee80211_tx_mgt_timeout);
 node.ni_authmode=IEEE80211_AUTH_SHARED;
 ieee80211_tx_mgt_cb(&node,(void*)(uintptr_t)IEEE80211_S_AUTH,1);
 assert(!vap.iv_mgtsend.delay&&vap.iv_mgtsend.fn==ieee80211_tx_mgt_timeout);node.ni_authmode=0;
 vap.iv_state=IEEE80211_S_ASSOC;ieee80211_tx_mgt_cb(&node,(void*)(uintptr_t)IEEE80211_S_ASSOC,1);
 assert(!vap.iv_mgtsend.delay&&vap.iv_mgtsend.fn==ieee80211_tx_mgt_timeout);
 vap.iv_state=IEEE80211_S_AUTH;vap.iv_opmode=IEEE80211_M_HOSTAP;
 ieee80211_tx_mgt_cb(&node,(void*)(uintptr_t)IEEE80211_S_AUTH,1);
 assert(!vap.iv_mgtsend.delay&&vap.iv_mgtsend.fn==ieee80211_tx_mgt_timeout);
 vap.iv_opmode=IEEE80211_M_STA;hz=1;ieee80211_tx_mgt_cb(&node,(void*)(uintptr_t)IEEE80211_S_AUTH,1);
 assert(vap.iv_mgtsend.delay==1);
 puts("PASS actual authentication callbacks/timers: two bounded retries, state/AP cancellation, reset, shared-key/AP/association exclusions");
}
'''
with tempfile.TemporaryDirectory() as directory:
    source_file = Path(directory) / 'auth.c'
    binary = Path(directory) / 'auth'
    source_file.write_text(code)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Werror=implicit-function-declaration',
                    '-fsanitize=address,undefined', str(source_file), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
