# SPDX-License-Identifier: MIT
"""Regression for RFC 2018 first-block selection with bounded output."""
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
struct net_buffer {uint32 sequence;uint32 size;};
struct SegmentList {
 std::vector<net_buffer> items;
 struct ReverseIterator{std::vector<net_buffer>*v;size_t i;
  net_buffer*Next(){return i?&(*v)[--i]:nullptr;}};
 ReverseIterator GetReverseIterator(){return {&items,items.size()};}
};
struct BufferQueue {tcp_sequence fLastAddedSequence;SegmentList fList;int PopulateSackInfo(tcp_sequence,int,tcp_sack*);};
'''
checks=r'''
static void check(uint32 base,std::vector<net_buffer> items,std::vector<tcp_sack> expected,int max,uint32 recent){
 BufferQueue q;q.fLastAddedSequence=recent;q.fList.items=items;struct {tcp_sack b[4];uint32 canary;} out{};
 out.canary=0x12345678;int n=q.PopulateSackInfo(base,max,out.b);
 assert(n==(int)expected.size());assert(out.canary==0x12345678);
 for(int i=0;i<n;i++){
  assert(ntohl(out.b[i].left_edge)==expected[i].left_edge);
  assert(ntohl(out.b[i].right_edge)==expected[i].right_edge);
 }
}
int main(){
 check(100,{{200,10},{400,10},{600,10},{800,10}},
  {{200,210},{800,810},{600,610}},3,200);
 unsigned cases=0;
 for(uint32 base: {0u,0x7ffffff0u,0xfffffff0u}){
  for(unsigned mask=1;mask<256;mask++){
   std::vector<net_buffer> in;std::vector<tcp_sack> groups;
   for(unsigned k=0;k<8;k++)if(mask&(1u<<k))in.push_back({base+16+k*16,16});
   for(int k=7;k>=0;){if(!(mask&(1u<<k))){k--;continue;}
    unsigned end=k+1;while(k>=0&&(mask&(1u<<k)))k--;
    groups.push_back({base+16+(k+1)*16,base+16+end*16});
   }
   for(unsigned slot=0;slot<8;slot++)if(mask&(1u<<slot)){
    uint32 recent=base+16+slot*16;
    auto want=groups;
    for(size_t i=0;i<want.size();i++)if(uint32(recent-want[i].left_edge)<uint32(want[i].right_edge-want[i].left_edge)){
     auto selected=want[i];want.erase(want.begin()+i);want.insert(want.begin(),selected);break;
    }
    for(int max=1;max<=4;max++){
     auto limited=want;if(limited.size()>(unsigned)max)limited.resize(max);
     check(base,in,limited,max,recent);cases++;
    }
   }
  }
 }
 // At capacity the newest low range must displace a high range, and
 // traversal/output remain bounded with thousands of separated ranges.
 std::vector<net_buffer> many;for(unsigned i=0;i<3000;i++)many.push_back({100+32*i,16});
 check(0,many,{{100,116},{100+32*2999,116+32*2999},{100+32*2998,116+32*2998}},3,100);
 printf("PASS actual first SACK block: %u recent-range/wrap/output-limit cases, merged ranges, 3000 blocks\n",cases);
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'sack.cpp';p.write_text(fixture[:fixture.index('struct BufferQueue')].replace('struct tcp_sack',sequence+'\nstruct tcp_sack',1)+fixture[fixture.index('struct BufferQueue'):]+f+checks)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
