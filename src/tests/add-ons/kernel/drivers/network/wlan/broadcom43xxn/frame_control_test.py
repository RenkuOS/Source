# SPDX-License-Identifier: MIT
from pathlib import Path
import os, subprocess, tempfile
root = Path(os.environ.get('BRCMS_SOURCE', Path(__file__).resolve().parents[7] / 'add-ons/kernel/drivers/network/wlan/broadcom43xxn'))
def extract(path,name):
 s=(root/path).read_text();a=s.index('\n'+name+'(');a=s.rfind('\n',0,a-1)+1;b=s.index('\n}\n',a)+3
 return s[a:b]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint32_t u32;
typedef unsigned uint;
typedef int cpu_status;
struct bcma_device { int unused; };
struct brcms_hardware { struct bcma_device* d11core; bool clk; void* di[1]; };
struct d11txh { uint16 XtraFrameTypes, MacTxControlLow; };
struct sk_buff { void* data; };
static struct bcma_device core;
static struct brcms_hardware hw={&core,true,{0}};
static struct d11txh header;
static struct sk_buff packet={&header};
static unsigned cursor, reclaimed, callbacks, freed;
static uint32 states[8];
static bool noPacket;
static int sReclaiming, sTxRingLock;
static unsigned sFramesCompleted,sFramesAcknowledged,sFramesUnanswered;
static unsigned sSuppressed,sSuppressedSeen=40;
#define TXS_V 1
#define TX_STATUS_AMPDU 32
#define TX_STATUS_INTERMEDIATE 64
#define TX_STATUS_ACK_RCV 2
#define TX_STATUS_SUPR_MASK 28
#define TX_STATUS_SUPR_SHIFT 2
#define TX_STATUS_FRM_RTX_MASK 0xf000
#define TX_STATUS_FRM_RTX_SHIFT 12
#define TX_AC_BE_FIFO 0
#define DMA_RANGE_TRANSMITTED 0
#define XFTS_CHANNEL_SHIFT 8
#define D11REGOFFS(x) x
#define frmtxstatus 0
#define frmtxstatus2 1
#define le16_to_cpu(x) (x)
#define TRACE(...) ((void)0)
struct brcms_c_info;
static struct brcms_c_info* brcms_glue_common(void) {return NULL;}
static struct brcms_hardware* brcms_glue_hardware(void) {return &hw;}
static int atomic_test_and_set(int* p,int v,int old) {int r=*p;if(r==old)*p=v;return r;}
static void atomic_set(int* p,int v){*p=v;}
static cpu_status disable_interrupts(void){return 0;}
static void restore_interrupts(cpu_status s) {assert(!sTxRingLock);}
static void acquire_spinlock(int* p){assert(!*p);*p=1;}
static void release_spinlock(int* p){assert(*p);*p=0;}
static u32 bcma_read32(struct bcma_device* c,int reg){
 if(reg==1){cursor++;return 0;}return states[cursor];
}
static struct sk_buff* dma_getnexttxp(void* d,int r){reclaimed++;return noPacket?NULL:&packet;}
static void brcmu_pkt_buf_free_skb(struct sk_buff* p){freed++;}
static void brcms_glue_report_sent(bool ack,uint retries){assert(ack);callbacks++;}
'''
tx=(root/'brcms/glue_tx.c').read_text()
a=tx.index('\tu16 controlLow = TXC_IGNOREPMQ;')
b=tx.index('\n\t// Which band and how wide',a)
prefix += r'''
#define TXC_IGNOREPMQ 0x20
#define TXC_IMMEDACK 1
#define TXC_STARTMSDU 8
#define TXC_HWSEQ 0x10
#define TXC_AMPDU_MIDDLE 2
#define TXC_AMPDU_SHIFT 9
static uint16 control_for(uint8 fc,bool aggregated,bool expectAck) {
 uint8 frame[1]={fc};
'''+tx[a:b].replace('u16 controlLow','uint16 controlLow')+'\nreturn controlLow;\n}\n'
main=r''' 
int main(void){
 // Every QoS subtype retains net80211's per-TID sequence number.
 for(unsigned fc=0;fc<256;fc++) {
  bool qos=(fc&0x0c)==0x08 && (fc&0x80)!=0;
  assert(((control_for(fc,false,true)&TXC_HWSEQ)!=0)==!qos);
  assert((control_for(fc,true,true)&TXC_HWSEQ)==0);
 }
 // Intermediate RTS report must not reclaim or complete the pending MPDU.
 states[0]=TXS_V|TX_STATUS_INTERMEDIATE;
 states[1]=TXS_V|TX_STATUS_ACK_RCV|0x1000;
 brcms_glue_reclaim_sent();
 assert(reclaimed==1 && callbacks==1 && freed==1 && !sReclaiming);
 // Spurious final status without a DMA packet must not consume a node.
 cursor=0; states[0]=TXS_V|TX_STATUS_ACK_RCV;states[1]=0;noPacket=true;
 brcms_glue_reclaim_sent();
 assert(reclaimed==2 && callbacks==1 && freed==1);
 // All too-short buffers, including zero-length allocations, are rejected.
 for(unsigned len=0;len<24;len++){
  uint8* frame=calloc(len?len:1,1);
  for(unsigned fc=0;fc<256;fc++) {
   frame[0]=fc;
   assert(brcms_rx_header_valid(frame,len)==(len>=20 && fc==0x84));
  }
  free(frame);
 }
 uint8 frame[24]={0x08}; assert(brcms_rx_header_valid(frame,24));
 puts("PASS: intermediate/final TX ownership, empty DMA, short BAR and malformed frames");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(prefix+extract('brcms/glue_tx.c','brcms_glue_reclaim_sent')+extract('brcms/glue_rx.c','brcms_rx_header_valid')+main)
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
