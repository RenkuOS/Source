# SPDX-License-Identifier: MIT
from pathlib import Path
import os,subprocess,tempfile
root=Path(os.environ.get('BRCMS_SOURCE',Path(__file__).resolve().parents[7]/'add-ons/kernel/drivers/network/wlan/broadcom43xxn'))
s=(root/'brcms/glue_net.c').read_text();a=s.index('void\nbrcms_glue_poll_frames');b=s.index('\n}\n',a)+3
code='''#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef unsigned uint;
struct brcms_hardware {int unused;};
static struct brcms_hardware hw;
static unsigned batches,completed,polls,available;
#define RX_FIFO 0
static struct brcms_hardware* brcms_glue_hardware(void){return &hw;}
static void brcms_glue_rx_polled(void){polls++;}
static bool brcms_b_recv(struct brcms_hardware* h,int q,bool bounded){
 assert(completed==batches);assert(bounded);batches++;return batches<available;
}
static void brcms_glue_reclaim_sent(void){completed++;}
'''+s[a:b]+'''
int main(void) {
 available=20;brcms_glue_poll_frames();assert(batches==20 && completed==20 && polls==1);
 batches=completed=0;available=1000;brcms_glue_poll_frames();assert(batches==64 && completed==64 && polls==2);
 batches=completed=0;available=0;brcms_glue_poll_frames();assert(batches==1 && completed==1);
 puts("PASS: TX service between RX batches, empty RX, bounded drain");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
