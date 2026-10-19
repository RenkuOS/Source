# SPDX-License-Identifier: MIT
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(os.environ.get('BRCMS_SOURCE', Path(__file__).resolve().parents[7] / 'add-ons/kernel/drivers/network/wlan/broadcom43xxn'))
s = (root / 'brcms/glue_brcms.c').read_text()
def function(name):
    a = s.index('void ' + name + '(')
    return s[a:s.index('\n}', a) + 2]

defines = '\n'.join(line for line in s.splitlines()
                    if re.match(r'#define EDCF_', line))
net = (root / 'brcms/glue_net.c').read_text()
a = net.index('void\nbrcms_glue_set_queue(')
set_queue = net[a:net.index('\n}', a) + 2]
bridge = (root / 'if_brcms.c').read_text()
a = bridge.index('static int\nbrcms_wme_update(')
update = bridge[a:bridge.index('\n}', a) + 2]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
enum {IEEE80211_AC_VO, IEEE80211_AC_VI, IEEE80211_AC_BE, IEEE80211_AC_BK};
#define IEEE80211_NUM_ACS 4
#define M_EDCF_QINFO 0
#define M_EDCF_QLEN 32
#define M_EDCF_STATUS_OFF 14
#define WME_STATUS_NEWAC 256
#define D11REGOFFS(x) 0
#define brcms_err(...) ((void)0)
struct edcf_acparam {u8 ACI, ECW; u16 TXOP;};
struct ieee80211_tx_queue_params {u16 txop, aifs, cw_min, cw_max;};
struct shm_acparams {u16 txop,cwmin,cwmax,cwcur,aifs,bslots,reggap,status,rsvd[8];};
struct brcms_hardware {bool clk; void* d11core;};
#define hw brcms_hardware
typedef uint8_t uint8; typedef uint16_t uint16; typedef unsigned uint;
#define TRACE(...) ((void)0)
#define WME_NUM_AC 4
#define IEEE80211_TXOP_TO_US(v) ((v)*32)
struct ieee80211vap {int unused;};
struct wmeParams {uint8 wmep_aifsn, wmep_logcwmin, wmep_logcwmax;uint16 wmep_txopLimit;};
struct brcms_c_info {bool clk; struct hw* hw; u16 edcf_txop[4];};
static const u8 wme_ac2fifo[] = {3,2,1,0};
static u16 shm[64];
static unsigned writes;
static u16 brcms_b_read_shm(struct hw* h, unsigned at) {return shm[at/2];}
static void brcms_b_write_shm(struct hw* h,unsigned at,u16 v) {assert(at<sizeof(shm));shm[at/2]=v;writes++;}
static u16 bcma_read16(void* core,unsigned at) {return 0;}
static void brcms_c_suspend_mac_and_wait(struct brcms_c_info* w) {}
static void brcms_c_enable_mac(struct brcms_c_info* w) {}
static void brcms_c_update_beacon(struct brcms_c_info* w) {}
static void brcms_c_update_probe_resp(struct brcms_c_info* w,bool b) {}
'''+defines+'\n'+function('brcms_c_wme_setparams')+'\n'+function('brcms_c_edcf_setparams')+r'''
static struct brcms_c_info* common;
static struct brcms_c_info* brcms_glue_common(void) {return common;}
static struct hw* brcms_glue_hardware(void) {return common->hw;}
static bool brcms_glue_pause(void) {return true;}
static void brcms_glue_resume(bool p) {}
'''+set_queue+'\n'+update+r'''
int main(void) {
 struct hw h={true,(void*)1}; struct brcms_c_info w={false,&h,{0}};
 brcms_c_edcf_setparams(&w,false);
 assert(writes==64);
 const u16 aifs[]={7,3,2,2},cwmin[]={15,15,7,3},cwmax[]={1023,1023,15,7},txop[]={0,0,3008,1504};
 for(unsigned fifo=0;fifo<4;fifo++) {
  struct shm_acparams a;memcpy(&a,&shm[fifo*16],sizeof(a));
  assert(a.aifs==aifs[fifo] && a.cwmin==cwmin[fifo]);
  assert(a.cwmax==cwmax[fifo] && a.txop==txop[fifo]);
  assert(a.status&WME_STATUS_NEWAC);
 }
 common=&w;
 struct wmeParams params[]={{3,4,10,0},{7,4,10,0},{2,3,4,94},{2,2,3,47}};
 memset(shm,0,sizeof(shm)); writes=0;
 brcms_wme_update(NULL,params);assert(writes==64);
 for(unsigned fifo=0;fifo<4;fifo++) {
  struct shm_acparams a;memcpy(&a,&shm[fifo*16],sizeof(a));
  assert(a.aifs==aifs[fifo] && a.cwmin==cwmin[fifo]);
  assert(a.cwmax==cwmax[fifo] && a.txop==txop[fifo]);
 }
 h.clk=false;w.clk=true;writes=0;
 brcms_c_edcf_setparams(&w,false);assert(writes==0);
 puts("PASS: all four hardware queues receive their own contention parameters; clock-off writes blocked");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d) / 'edca.c'
    p.write_text(code)
    subprocess.run([os.environ.get('CC','cc'), '-std=c11', '-fsanitize=address,undefined', str(p), '-o', str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
