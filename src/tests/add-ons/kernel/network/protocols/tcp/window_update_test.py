# SPDX-License-Identifier: MIT
"""Exercise production window-update decisions with actual sequence arithmetic."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(os.environ['TCP_SOURCE']) if 'TCP_SOURCE' in os.environ else Path(__file__).resolve().parents[7]/'src/add-ons/kernel/network/protocols/tcp'
s=(root/'TCPEndpoint.cpp').read_text()
a=s.index('TCPEndpoint::_ShouldSendSegment(')
f='bool\n'+s[a:s.index('\n}',a)+2]
h=(root/'tcp.h').read_text();a=h.index('class tcp_sequence {');sequence=h[a:h.index('// TCP flag constants',a)]
fixture=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
using uint32=uint32_t;using int32=int32_t;
constexpr int ESTABLISHED=4;
constexpr uint32 TCP_NODELAY=1,TCP_FLAG_SYNCHRONIZE=2,TCP_FLAG_FINISH=1,TCP_FLAG_RESET=4;
'''+sequence+r'''
struct tcp_segment_header {
 uint32 advertised_window=0,flags=0;
 uint32 AdvertisedWindow(unsigned shift){return advertised_window<<shift;}
};
struct Queue {tcp_sequence last=0;tcp_sequence LastSequence(){return last;}};
struct Socket {struct {uint32 buffer_size=262144;} receive;} sock;
struct TCPEndpoint {
 int fState=ESTABLISHED;uint32 fSendMaxSegments=1,fOptions=0,fSendMaxWindow=0;
 unsigned fReceiveWindowShift=0;tcp_sequence fSendNext=0,fSendUrgentOffset=0;
 tcp_sequence fReceiveNext=1000,fReceiveMaxAdvertised=132048;
 uint32 fReceiveMaxSegmentSize=1460;Queue fSendQueue;Socket*socket=&sock;
 bool _ShouldSendSegment(tcp_segment_header&,uint32,uint32,uint32);
};
'''
checks=r'''
int main(){
 TCPEndpoint e;tcp_segment_header s;s.advertised_window=131047;
 assert(!e._ShouldSendSegment(s,0,0,0)&&"a smaller window is not an update");
 unsigned cases=0;
 for(unsigned shift=0;shift<=14;shift++)for(uint32 base:{1000u,0xfffffff0u}){
  e.fReceiveWindowShift=shift;e.fReceiveNext=base;
  for(uint32 raw:{1u,10u,100u,1000u,32767u,65535u}){
   uint32 advertised=raw<<shift;s.advertised_window=raw;
   for(uint32 remaining:{0u,1u,advertised-1,advertised,advertised+1,advertised+1024}){
    e.fReceiveMaxAdvertised=base+remaining;
    uint32 delta=advertised>remaining?advertised-remaining:0;
    bool want=delta>=2920||delta>=131072;
    assert(e._ShouldSendSegment(s,0,0,0)==want);cases++;
   }
  }
 }
 e.fReceiveWindowShift=0;e.fReceiveNext=1000;e.fReceiveMaxAdvertised=1000;
 s.advertised_window=2919;assert(!e._ShouldSendSegment(s,0,0,0));
 s.advertised_window=2920;assert(e._ShouldSendSegment(s,0,0,0));
 // A shrinking window must not suppress data or SYN/FIN/RST/urgent sends.
 s.advertised_window=1;e.fReceiveMaxAdvertised=2000;
 for(uint32 flags:{TCP_FLAG_SYNCHRONIZE,TCP_FLAG_FINISH,TCP_FLAG_RESET}){
  s.flags=flags;assert(e._ShouldSendSegment(s,0,0,0));}
 s.flags=0;e.fSendUrgentOffset=1;assert(e._ShouldSendSegment(s,0,0,0));
 e.fSendUrgentOffset=0;e.fSendQueue.last=1460;
 assert(e._ShouldSendSegment(s,1460,1460,0));
 // Preserve the existing FIN/zero-window special case.
 e.fReceiveNext=2001;e.fReceiveMaxAdvertised=2000;s.advertised_window=65535;
 assert(!e._ShouldSendSegment(s,0,0,0));
 printf("PASS actual window update decision: %u scaled/wrapped cases, shrink/equality/thresholds, data/control/urgent and FIN boundary\n",cases);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'window.cpp';p.write_text(fixture+f+checks)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
