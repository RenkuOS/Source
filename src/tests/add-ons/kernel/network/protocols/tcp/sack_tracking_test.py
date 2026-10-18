# SPDX-License-Identifier: MIT
"""Exercise actual queue construction, reset and receive insertion tracking."""
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
int main(){BufferQueue q(1000);assert(q.fLastAddedSequence==0);q.SetInitialSequence(100);assert(q.fLastAddedSequence==100);
 net_buffer a{{},200,10},b{{},400,10},c{{},150,10},duplicate{{},200,5},zero{{},500,0},old{{},80,10},trim{{},95,10};
 q.Add(&a,200);assert(q.fLastAddedSequence==200);
 q.Add(&b,400);assert(q.fLastAddedSequence==400);
 q.Add(&c,150);assert(q.fLastAddedSequence==150);
 q.Add(&duplicate,200);assert(duplicate.freed&&q.fLastAddedSequence==200);
 q.Add(&zero,500);assert(zero.freed&&q.fLastAddedSequence==200);
 q.Add(&old,80);assert(old.freed&&q.fLastAddedSequence==200);
 q.Add(&trim,95);assert(trim.sequence==100&&trim.size==5&&q.fLastAddedSequence==100);
 BufferQueue wrap(1000);wrap.SetInitialSequence(0xfffffff0);assert(wrap.fLastAddedSequence==0xfffffff0);
 net_buffer edge{{},0,16};wrap.Add(&edge,0);assert(wrap.fLastAddedSequence==0);
 puts("PASS actual queue tracking: constructor, reset, out-of-order insertion, duplicate ownership, rejected/trimmed data, zero/wrap");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'tracking.cpp';a=fixture.index('struct net_buffer');p.write_text(fixture[:a]+sequence+fixture[a:]+functions+checks)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
