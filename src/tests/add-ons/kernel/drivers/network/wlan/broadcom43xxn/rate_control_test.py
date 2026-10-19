# SPDX-License-Identifier: MIT
from pathlib import Path
import os
import subprocess
import tempfile
root=Path(os.environ.get('BRCMS_SOURCE',Path(__file__).resolve().parents[7]/'add-ons/kernel/drivers/network/wlan/broadcom43xxn'))
s=(root/'if_brcms.c').read_text()
a=s.index('static uint16_t sMcsMask')
b=s.index('\n}\n',s.index('static void\nbrcms_rate_answered('))+3
prefix='''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef unsigned uint;
typedef uint8_t uint8;
#define BRCMS_MCS_SETTLE 200
#define BRCMS_MCS_TOP 15
#define BRCMS_MCS_CLIMB 12
static uint8 sMcs;
static unsigned sSettled, sMcsGood, sMcsBad;
static bool sMcsUsable=true;
'''
test='''
int main(void) {
 for(int i=0;i<199;i++) brcms_rate_answered(true,1,false,12);
 assert(sSettled==199 && sMcs==0);
 brcms_rate_answered(false,1,false,12);
 assert(sSettled==0);
 for(int i=0;i<200;i++) brcms_rate_answered(true,1,false,12);
 assert(sSettled==200 && sMcsUsable && sMcs==0);
 for(int rate=0;rate<7;rate++) {
  for(int i=0;i<12;i++) brcms_rate_answered(true,1,true,rate);
  assert(sMcs==rate+1);
 }
 brcms_rate_answered(false,4,true,7);
 assert(sMcs==6);
 for(int i=0;i<63;i++) brcms_rate_answered(false,4,true,7);
 assert(sMcs==6 && sMcsUsable);
 for(int i=0;i<1000;i++) brcms_rate_answered(false,4,false,12);
 assert(sMcs==6 && sMcsUsable);
 while(sMcs) brcms_rate_answered(false,4,true,sMcs);
 for(int i=0;i<3;i++) brcms_rate_answered(false,4,true,0);
 assert(!sMcsUsable && sSettled==0);
 for(int i=0;i<200;i++) brcms_rate_answered(true,1,false,12);
 assert(sMcsUsable && sSettled==200 && sMcs==0);
 for(int i=0;i<100;i++) brcms_rate_answered(true,2,true,0);
 assert(sMcs==0);
 sMcsMask=0xffff;
 assert(brcms_adjacent_mcs(7,true)==12);
 assert(brcms_adjacent_mcs(12,false)==7);
 assert(brcms_adjacent_mcs(15,true)==15);
 for(unsigned mask=1;mask<65536;mask+=2) {
  sMcsMask=mask;
  unsigned current=0;
  for(int step=0;step<16;step++) {
   unsigned next=brcms_adjacent_mcs(current,true);
   assert(mask & (1U<<next));
   if(next==current) break;
   current=next;
   assert(step<15);
  }
 }
 puts("PASS: settle, climb, stale completion, legacy feedback, fallback recovery, retry hold");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c'; p.write_text(prefix+s[a:b]+test)
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
