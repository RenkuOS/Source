# SPDX-License-Identifier: MIT
"""Regress actual receive-prefix accounting for larger overlapping retransmissions."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(os.environ['TCP_SOURCE']) if 'TCP_SOURCE' in os.environ else (Path(__file__).resolve().parents[7]/'src/add-ons/kernel/network/protocols/tcp')
s=(root/'BufferQueue.cpp').read_text();h=(root/'tcp.h').read_text();a=h.index('class tcp_sequence {');sequence=h[a:h.index('// TCP flag constants',a)]
def extract(marker):
 a=s.index(marker);return s[a:s.index('\n}',a)+2]
functions='\n'.join(extract(x) for x in ('BufferQueue::BufferQueue(size_t maxBytes)','void\nBufferQueue::SetInitialSequence(', 'void\nBufferQueue::Add(net_buffer *buffer, tcp_sequence sequence)'))
fixture=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <cstdio>
typedef uint32_t uint32;typedef int32_t int32;
#define TRACE(x)
#define VERIFY()
#define ASSERT(x) assert(x)
struct net_buffer {struct {net_buffer*next;} link{};uint32 sequence,size;bool freed=false;};
struct SegmentList {
 std::vector<net_buffer*> items;
 void refresh(){for(size_t i=0;i<items.size();i++)items[i]->link.next=i+1<items.size()?items[i+1]:nullptr;}
 bool IsEmpty(){return items.empty();}
 void Add(net_buffer*b){items.push_back(b);refresh();}
 void Remove(net_buffer*b){auto i=std::find(items.begin(),items.end(),b);assert(i!=items.end());items.erase(i);refresh();}
 void InsertBefore(net_buffer*n,net_buffer*b){auto i=std::find(items.begin(),items.end(),n);items.insert(i,b);refresh();}
 struct ReverseIterator {std::vector<net_buffer*>*v;size_t i;net_buffer*Next(){return i?(*v)[--i]:nullptr;}};
 ReverseIterator GetReverseIterator(){return {&items,items.size()};}
};
struct Buffers {void free(net_buffer*b){assert(!b->freed);b->freed=true;}
 void remove_header(net_buffer*b,size_t n){assert(n<=b->size);b->size-=n;}
 void remove_trailer(net_buffer*b,size_t n){assert(n<=b->size);b->size-=n;}
} storage,*gBufferModule=&storage;
struct BufferQueue {
 SegmentList fList;size_t fMaxBytes,fNumBytes,fContiguousBytes;
 tcp_sequence fFirstSequence,fLastSequence,fPushPointer,fLastAddedSequence;
 BufferQueue(size_t);void SetInitialSequence(tcp_sequence);void Add(net_buffer*,tcp_sequence);
};
'''
checks=r'''

int main(){
 // A longer retransmission replaces the short contiguous head while a
 // later hole remains. The old code leaves Available() at 10, not 20.
 BufferQueue q(1000);q.SetInitialSequence(100);
 net_buffer shortHead{{},100,10},later{{},130,10},longer{{},100,20};
 q.Add(&shortHead,100);q.Add(&later,130);q.Add(&longer,100);
 assert(shortHead.freed&&q.fNumBytes==30&&q.fContiguousBytes==20);
 // Cover several old prefix segments without filling the final hole.
 BufferQueue many(1000);many.SetInitialSequence(100);
 net_buffer aa{{},100,5},bb{{},105,5},cc{{},140,10},replacement{{},100,30};
 many.Add(&aa,100);many.Add(&bb,105);many.Add(&cc,140);many.Add(&replacement,100);
 assert(aa.freed&&bb.freed&&many.fContiguousBytes==30&&many.fNumBytes==40);
 uint32 rng=0x7315a9u;unsigned checked=0;
 const uint32 bases[]={0,1,0xfffffff0u,0xffffff80u,0x7ffffff0u};
 for(uint32 base:bases)for(unsigned trial=0;trial<100;trial++){
  BufferQueue queue(1000);queue.SetInitialSequence(base);
  bool known[128]={false};std::vector<net_buffer> buffers;buffers.reserve(128);
  for(unsigned i=0;i<128;i++){
   rng=rng*1664525u+1013904223u;int offset=(int)(rng%136)-8;
   rng=rng*1664525u+1013904223u;unsigned length=1+(rng%32);
   if(offset+(int)length>128)length=128-offset;
   buffers.push_back({{},(uint32)(base+offset),length});
   queue.Add(&buffers.back(),tcp_sequence(base+offset));
   for(int j=offset;j<offset+(int)length;j++)if(j>=0&&j<128)known[j]=true;
   size_t expected=0,total=0;while(expected<128&&known[expected])expected++;
   for(bool bit:known)total+=bit;
   assert(queue.fContiguousBytes==expected&&queue.fNumBytes==total);
   unsigned listed=0;for(auto*b:queue.fList.items){assert(!b->freed);listed+=b->size;}
   assert(listed==total);checked++;
  }
 }
 printf("PASS actual receive insertion: longer head retransmission, multiple replaced prefix buffers, %u overlapping/wrapped union and ownership checks\n",checked);
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'tracking.cpp';a=fixture.index('struct net_buffer');p.write_text(fixture[:a]+sequence+fixture[a:]+functions+checks)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
