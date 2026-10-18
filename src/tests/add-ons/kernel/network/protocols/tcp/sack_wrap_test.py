# SPDX-License-Identifier: MIT
"""Regression for production SACK ranges around TCP sequence wrap."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(os.environ['TCP_SOURCE']) if 'TCP_SOURCE' in os.environ else (Path(__file__).resolve().parents[7]/'src/add-ons/kernel/network/protocols/tcp')
s=(root/'BufferQueue.cpp').read_text();a=s.index('int\nBufferQueue::PopulateSackInfo(');f=s[a:s.index('\n}',a)+2]
h=(root/'tcp.h').read_text();a=h.index('class tcp_sequence {');sequence=h[a:h.index('// TCP flag constants',a)]
fixture=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>
#include <arpa/inet.h>
typedef uint32_t uint32;typedef int32_t int32;
#define TRACE(x)
#define B_PRIu32 "u"
struct tcp_sack {uint32 left_edge,right_edge;};
struct net_buffer {uint32 sequence;size_t size;};
struct SegmentList {
 std::vector<net_buffer> items;
 struct ReverseIterator{std::vector<net_buffer>*v;size_t i;
  net_buffer*Next(){return i?&(*v)[--i]:nullptr;}};
 ReverseIterator GetReverseIterator(){return {&items,items.size()};}
};
struct BufferQueue {SegmentList fList;int PopulateSackInfo(tcp_sequence,int,tcp_sack*);};
'''
checks=r'''
static void check(uint32 base,std::vector<net_buffer> items,std::vector<tcp_sack> expected,int max=4){
 BufferQueue q;q.fList.items=items;struct {tcp_sack b[4];uint32 canary;} out{};
 out.canary=0x12345678;int n=q.PopulateSackInfo(base,max,out.b);
 assert(n==(int)expected.size());assert(out.canary==0x12345678);
 for(int i=0;i<n;i++){
  assert(ntohl(out.b[i].left_edge)==expected[i].left_edge);
  assert(ntohl(out.b[i].right_edge)==expected[i].right_edge);
 }
}
int main(){
 // The missing [0xfffffff0,0x10) bytes must never be advertised as received.
 check(0xffffffd0,{{0xffffffe0,16},{0x10,16}},{{0x10,0x20},{0xffffffe0,0xfffffff0}});
 // Zero is a valid block edge, never an empty-output sentinel.
 check(0xfffffff0,{{0,16}},{{0,16}});
 check(0xffffffe0,{{0xfffffff0,16}},{{0xfffffff0,0}});
 check(0xffffffe0,{{0xfffffff0,16},{0,16}},{{0xfffffff0,16}});
 check(10,{},{});check(10,{{10,10}},{});check(10,{{20,10}},{},0);
 for(uint32 base: {0u,0x7ffffff0u,0xfffffff0u}){
  for(unsigned mask=1;mask<256;mask++){
   std::vector<net_buffer> in;std::vector<tcp_sack> want;
   for(unsigned k=0;k<8;k++)if(mask&(1u<<k))in.push_back({base+16+k*16,16});
   for(int k=7;k>=0;){if(!(mask&(1u<<k))){k--;continue;}
    unsigned end=k+1;while(k>=0&&(mask&(1u<<k)))k--;
    want.push_back({base+16+(k+1)*16,base+16+end*16});
   }
   for(int max=1;max<=4;max++){
    auto limited=want;if(limited.size()>(unsigned)max)limited.resize(max);
    check(base,in,limited,max);
   }
  }
 }
 puts("PASS actual SACK generation: disjoint/adjacent ranges, zero edges, wrap, 3060 differential masks/limits, output bounds");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'sack.cpp';p.write_text(fixture[:fixture.index('struct BufferQueue')].replace('struct tcp_sack',sequence+'\nstruct tcp_sack',1)+fixture[fixture.index('struct BufferQueue'):]+f+checks)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
