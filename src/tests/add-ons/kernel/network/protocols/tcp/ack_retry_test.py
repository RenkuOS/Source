# SPDX-License-Identifier: MIT
"""Inject local ACK failures into the actual sender and timer callback."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[7]
source = (root / 'src/add-ons/kernel/network/protocols/tcp/TCPEndpoint.cpp').read_text()
def extract(name):
    start = source.index('TCPEndpoint::' + name + '(')
    return source[start:source.index('\n}', start) + 2]
fixture = r'''
#include <cassert>
#include <cstdio>
#include <initializer_list>
using status_t=int;using bigtime_t=long long;
#ifdef ENOBUFS
#undef ENOBUFS
#endif
#define ENOBUFS (-105)
constexpr status_t B_OK=0,B_ERROR=-1,B_NO_MEMORY=-12,B_WOULD_BLOCK=-11;
constexpr int CLOSED=0,LISTEN=1,ESTABLISHED=4;
constexpr bigtime_t TCP_DELAYED_ACKNOWLEDGE_TIMEOUT=100000;
constexpr unsigned FLAG_DESTROYING=0x800,FLAG_ACK_RETRY_PENDING=0x1000;
constexpr int TCP_FLAG_ACKNOWLEDGE=16;
#define T(x)
struct net_timer{};
struct Stack {
 bool active=false;bigtime_t delay=0;unsigned sets=0;
 bool is_timer_active(net_timer*){return active;}
 void set_timer(net_timer*,bigtime_t value){assert(value>=0&&value<=500000);active=true;delay=value;sets++;}
 void cancel_timer(net_timer*){active=false;}
} stack;
auto* gStackModule=&stack;
struct net_buffer{};
unsigned allocations=0,owned=0,sends=0;
bool allocationFails=false;
struct Buffers {
 net_buffer* create(unsigned size){assert(size==256);allocations++;
  if(allocationFails)return nullptr;owned++;return new net_buffer;}
} buffers;
auto* gBufferModule=&buffers;
struct tcp_segment_header{int flags=TCP_FLAG_ACKNOWLEDGE;unsigned acknowledge=0;};
struct Queue {bool IsContiguous(){return true;}};
bool canLock=true;
struct MutexLocker{MutexLocker(int&){}bool IsLocked(){return canLock;}};
struct TCPEndpoint {
 void* fRoute=(void*)1;int fState=ESTABLISHED,fLock=0;unsigned fFlags=0;
 unsigned fLastAcknowledgeSent=0,fReceiveNext=100,fSegmentsSinceAcknowledge=0;
 unsigned fReceiveMaxSegmentSize=1460;
 Queue fReceiveQueue;net_timer fDelayedAcknowledgeTimer;
 status_t result=B_OK;bool windowUpdate=false;
 int State(){return fState;}
 tcp_segment_header _PrepareSendSegment(){return {TCP_FLAG_ACKNOWLEDGE,fReceiveNext};}
 bool _ShouldSendSegment(tcp_segment_header&,int,int,int){return windowUpdate;}
 status_t _PrepareAndSend(tcp_segment_header& segment,net_buffer* buffer,bool retransmit){
  assert(!retransmit&&owned==1);sends++;delete buffer;owned--;
  if(result==B_OK){/*ACTUAL_ACK_SUCCESS*/}
  return result;
 }
 status_t _SendAcknowledge(bool force=false);
 status_t DelayedAcknowledge();
 static void _DelayedAcknowledgeTimer(net_timer*,void*);
};
'''
checks = r'''
void dispatch(TCPEndpoint& endpoint){
 stack.active=false;TCPEndpoint::_DelayedAcknowledgeTimer(&endpoint.fDelayedAcknowledgeTimer,&endpoint);
 assert(owned==0);
}
int main(){
 // A locally rejected ACK must be retried even if no more data arrives.
 TCPEndpoint endpoint;endpoint.result=ENOBUFS;dispatch(endpoint);
 assert(stack.active && stack.delay==10000);
 assert(endpoint.fLastAcknowledgeSent!=endpoint.fReceiveNext);
 // Further receive events must not flood a queue that already rejected us.
 unsigned deferredSends=sends,deferredAllocations=allocations,deferredSets=stack.sets;
 for(unsigned event=0;event<1000;event++){
  endpoint.fReceiveNext++;
  endpoint._SendAcknowledge(event%2);
 }
 assert(sends==deferredSends&&allocations==deferredAllocations&&stack.sets==deferredSets
  && "pending ACK retry must coalesce further local send attempts");
 for(unsigned event=0;event<1000;event++)assert(endpoint.DelayedAcknowledge()==B_OK);
 assert(stack.sets==deferredSets&&stack.delay==10000);
 assert(endpoint._SendAcknowledge(true)==B_WOULD_BLOCK);
 unsigned before=stack.sets;endpoint.result=B_WOULD_BLOCK;dispatch(endpoint);
 assert(stack.active&&stack.sets==before+1);
 endpoint.result=B_OK;dispatch(endpoint);
 assert(!stack.active&&endpoint.fLastAcknowledgeSent==endpoint.fReceiveNext);
 // A failed allocation has no packet to free, but also needs an ACK retry.
 endpoint.fReceiveNext++;allocationFails=true;before=sends;dispatch(endpoint);
 assert(stack.active&&sends==before);allocationFails=false;dispatch(endpoint);
 assert(!stack.active);
 // A forced duplicate ACK still needs retry with an unchanged cumulative ACK.
 endpoint.result=ENOBUFS;before=sends;endpoint._SendAcknowledge(true);
 assert(stack.active);endpoint.result=B_OK;dispatch(endpoint);
 assert(sends==before+2&&!stack.active&&(endpoint.fFlags&FLAG_ACK_RETRY_PENDING)==0);
 // A window reopening must survive a local failure despite an unchanged ACK number.
 endpoint.windowUpdate=true;endpoint.result=ENOBUFS;dispatch(endpoint);assert(stack.active);
 endpoint.result=B_OK;dispatch(endpoint);assert(!stack.active);
 // Never postpone an earlier scheduled ACK deadline.
 stack.active=true;stack.delay=1;before=stack.sets;endpoint.result=ENOBUFS;
 endpoint._SendAcknowledge();assert(stack.active&&stack.delay==1&&stack.sets==before);
 // Permanent errors do not create a retry loop.
 for(status_t error:{B_ERROR,-100,-101,-22}){
  endpoint.result=error;dispatch(endpoint);assert(!stack.active);
 }
 // Teardown and CLOSED callbacks cannot rearm the timer.
 endpoint.result=ENOBUFS;endpoint.fFlags=FLAG_DESTROYING;before=sends;
 dispatch(endpoint);assert(!stack.active&&sends==before);
 endpoint.fFlags=0;endpoint.fState=CLOSED;dispatch(endpoint);assert(!stack.active&&sends==before);
 // No route, listener, or redundant ACK produces a retry or allocation.
 endpoint.fState=ESTABLISHED;endpoint.fRoute=nullptr;before=allocations;
 assert(endpoint._SendAcknowledge()==B_ERROR&&!stack.active&&allocations==before);
 endpoint.fRoute=(void*)1;endpoint.fState=LISTEN;
 assert(endpoint._SendAcknowledge()==B_ERROR&&!stack.active&&allocations==before);
 endpoint.fState=ESTABLISHED;endpoint.windowUpdate=false;endpoint.result=B_OK;
 assert(endpoint._SendAcknowledge()==B_OK&&!stack.active&&allocations==before);
 assert(owned==0);
 puts("PASS actual ACK sender/callback: transient failure, allocation, window reopen, earlier deadline, permanent error, teardown, ownership");
}
'''
a = source.index('\tif (segment.flags & TCP_FLAG_ACKNOWLEDGE) {', source.index('TCPEndpoint::_PrepareAndSend('))
ackSuccess = source[a:source.index('\n\t}', a) + 3]
fixture = fixture.replace('/*ACTUAL_ACK_SUCCESS*/', ackSuccess)
code = fixture + '\nstatus_t\n' + extract('_SendAcknowledge')
code += '\nstatus_t\n' + extract('DelayedAcknowledge')
code += '\nvoid\n' + extract('_DelayedAcknowledgeTimer') + checks
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory) / 'ack_retry.cpp'
    p.write_text(code)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
        '-fsanitize=address,undefined', str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)
