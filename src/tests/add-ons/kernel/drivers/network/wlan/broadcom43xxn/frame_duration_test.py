# SPDX-License-Identifier: MIT
from pathlib import Path
import os, subprocess, tempfile
root=Path(os.environ.get('BRCMS_SOURCE',Path(__file__).resolve().parents[7]/'add-ons/kernel/drivers/network/wlan/broadcom43xxn'))
s=(root/'brcms/glue_tx.c').read_text();a=s.index('static void\nset_frame_duration');b=s.index('\n}\n',a)+3
code='''#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef uint8_t uint8;
typedef uint16_t uint16;
struct d11txh { uint16 FragDurFallback, TxFesTimeNormal, TxFesTimeFallback; };
#define cpu_to_le16(x) (x)
'''+s[a:b]+'''
int main(void) {
 struct d11txh h={0};
 uint8 data[24]={0x08};
 set_frame_duration(&h,data,44);
 assert(data[2]==44 && !data[3] && h.FragDurFallback==44);
 assert(!h.TxFesTimeNormal && !h.TxFesTimeFallback);
 data[4]=1;set_frame_duration(&h,data,44);
 assert(!data[2] && !data[3] && !h.FragDurFallback);
 uint8 auth[24]={0xb0};set_frame_duration(&h,auth,314);
 assert(auth[2]==58 && auth[3]==1 && h.FragDurFallback==314);
 uint8 poll[24]={0xa4,0,0x42,0xc0};set_frame_duration(&h,poll,44);
 assert(poll[2]==0x42 && poll[3]==0xc0 && h.FragDurFallback==0xc042);
 uint8 fragment[24]={8,4,0x99,0};set_frame_duration(&h,fragment,44);
 assert(fragment[2]==0x99 && h.FragDurFallback==0x99);
 fragment[1]=0;fragment[22]=1;set_frame_duration(&h,fragment,44);
 assert(fragment[2]==0x99);
 puts("PASS: data/auth NAV, multicast, PS-Poll AID, fragment preservation, FES disabled");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
