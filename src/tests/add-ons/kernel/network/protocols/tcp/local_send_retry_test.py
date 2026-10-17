# SPDX-License-Identifier: MIT
"""Reject the first queued TCP send with no network event left to wake it."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[7]
source=(root/'src/add-ons/kernel/network/protocols/tcp/TCPEndpoint.cpp').read_text()
def extract(name):
 a=source.index('TCPEndpoint::'+name+'(');return source[a:source.index('\n}',a)+2]
fixture=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <algorithm>
using uint32=uint32_t;using status_t=int;using bigtime_t=long long;
#ifdef ENOBUFS
#undef ENOBUFS
#endif
#define ENOBUFS (-105)
constexpr status_t B_OK=0,B_ERROR=-1,B_NO_MEMORY=-12,B_WOULD_BLOCK=-11;
constexpr int CLOSED=0,ESTABLISHED=4,FINISH_SENT=5;
constexpr unsigned FLAG_DESTROYING=0x800,FLAG_LOCAL_SEND_RETRY=0x2000,FLAG_USER_CLOSED=0x400;
constexpr int TCP_FLAG_FINISH=1,TCP_FLAG_SYNCHRONIZE=2,TCP_FLAG_RESET=4,TCP_FLAG_PUSH=8;
#define TRACE(...)
#define T(x)
#define min_c(a,b) std::min(a,b)
struct Seq{
 uint32 n;Seq(uint32 v=0):n(v){}uint32 Number()const{return n;}
 Seq operator+(uint32 v)const{return Seq(n+v);}Seq operator-(Seq v)const{return Seq(n-v.n);}
 bool operator==(Seq v)const{return n==v.n;}bool operator<(Seq v)const{return n<v.n;}
 Seq&operator+=(uint32 v){n+=v;return *this;}
};
struct net_timer{bool active=false;bigtime_t delay=0;unsigned sets=0;};
struct Stack{
 bool is_timer_active(net_timer*t){return t->active;}
 void set_timer(net_timer*t,bigtime_t value){assert(value>=0);t->active=true;t->delay=value;t->sets++;}
} stack;
auto*gStackModule=&stack;
unsigned owned=0,sendCalls=0,shouldChecks=0,failSendAt=0;bool allocationFails=false;
struct net_buffer{uint32 size=0;};
struct Buffers{
 net_buffer*create(unsigned n){assert(n==256);if(allocationFails)return nullptr;owned++;return new net_buffer;}
 void free(net_buffer*p){assert(owned);owned--;delete p;}
} buffers;
auto*gBufferModule=&buffers;
struct Queue{
 uint32 bytes=8;status_t error=B_OK;Seq first{100};
 uint32 Available(Seq seq=Seq(100)){uint32 used=(seq-first).Number();return used>=bytes?0:bytes-used;}
 Seq LastSequence(){return first+bytes;}
 status_t Get(net_buffer*b,Seq seq,uint32 n){assert(n<=Available(seq));b->size=n;return error;}
};
struct tcp_segment_header{int flags=0;};
uint32 tcp_options_length(tcp_segment_header&){return 0;}
bool state_needs_finish(int state){return state==FINISH_SENT;}
struct MutexLocker{MutexLocker(int&){}bool IsLocked(){return true;}};
struct TCPEndpoint{
 void*fRoute=(void*)1;int fState=ESTABLISHED,fLock=0;unsigned fFlags=0;
 uint32 fSendWindow=65535,fCongestionWindow=32768,fDuplicateAcknowledgeCount=0,fSendMaxSegmentSize=1460;
 Seq fSendMax{100},fSendNext{100},fSendUnacknowledged{100};Queue fSendQueue;
 net_timer fRetransmitTimer,fPersistTimer;bigtime_t fRetransmitTimeout=1000000;
 bool allowSend=true;status_t result=B_OK;
 int State(){return fState;}
 tcp_segment_header _PrepareSendSegment(){return {};}
 bool _ShouldSendSegment(tcp_segment_header&,uint32,uint32,uint32){shouldChecks++;return allowSend;}
 void _StartPersistTimer(){stack.set_timer(&fPersistTimer,5000000);}
 status_t _PrepareAndSend(tcp_segment_header&,net_buffer*b,bool){
  sendCalls++;uint32 n=b->size;buffers.free(b);
  status_t status=failSendAt==sendCalls?ENOBUFS:result;
  if(status==B_OK){fSendNext+=n;if(fSendMax<fSendNext)fSendMax=fSendNext;}
  return status;
 }
 status_t _SendQueued(bool force=false);status_t _RetryLocalSend(status_t);
 static void _PersistTimer(net_timer*,void*);
};
'''
checks=r'''
void dispatch(TCPEndpoint&e){e.fPersistTimer.active=false;TCPEndpoint::_PersistTimer(&e.fPersistTimer,&e);assert(!owned);}
int main(){
 TCPEndpoint e;e.result=ENOBUFS;assert(e._SendQueued()==ENOBUFS);
 assert(e.fPersistTimer.active&&e.fPersistTimer.delay==10000&&!e.fRetransmitTimer.active);
 assert(e.fSendNext.Number()==100&&e.fSendQueue.Available(e.fSendNext)==8&&!owned);
 e.result=B_WOULD_BLOCK;dispatch(e);assert(e.fPersistTimer.active&&e.fPersistTimer.delay==10000);
 unsigned before=shouldChecks;e.result=B_OK;dispatch(e);
 assert(shouldChecks==before+1&&e.fSendNext.Number()==108&&e.fRetransmitTimer.active);
 assert(!(e.fFlags&FLAG_LOCAL_SEND_RETRY)&&!e.fPersistTimer.active);
 // Buffer allocation and queue-copy failure keep queued data and release temporary ownership.
 TCPEndpoint allocation;allocationFails=true;assert(allocation._SendQueued()==B_NO_MEMORY);
 assert(allocation.fPersistTimer.active&&!owned);allocationFails=false;dispatch(allocation);
 assert(allocation.fSendNext.Number()==108);
 TCPEndpoint copy;copy.fSendQueue.error=B_NO_MEMORY;assert(copy._SendQueued()==B_NO_MEMORY);
 assert(copy.fPersistTimer.active&&!owned);copy.fSendQueue.error=B_OK;dispatch(copy);
 assert(copy.fSendNext.Number()==108);
 // Already existing deadlines are neither replaced nor postponed.
 for(bool retransmit:{false,true}){
  TCPEndpoint active;active.result=ENOBUFS;
  net_timer&timer=retransmit?active.fRetransmitTimer:active.fPersistTimer;
  timer.active=true;timer.delay=1;assert(active._SendQueued()==ENOBUFS);
  assert(timer.active&&timer.delay==1&&timer.sets==0&&!(active.fFlags&FLAG_LOCAL_SEND_RETRY));
 }
 // A later failed segment has an actual retransmission timer from the first.
 TCPEndpoint partial;partial.fSendMaxSegmentSize=4;failSendAt=sendCalls+2;
 assert(partial._SendQueued()==ENOBUFS);assert(partial.fSendNext.Number()==104);
 assert(partial.fRetransmitTimer.active&&!partial.fPersistTimer.active);failSendAt=0;
 // Permanent failures and endpoint teardown never create retry loops.
 for(status_t error:{B_ERROR,-100,-101,-22}){
  TCPEndpoint permanent;permanent.result=error;assert(permanent._SendQueued()==error);
  assert(!permanent.fPersistTimer.active&&!permanent.fRetransmitTimer.active);
 }
 TCPEndpoint dying;dying.fFlags=FLAG_DESTROYING;dying.result=ENOBUFS;
 assert(dying._SendQueued()==ENOBUFS&&!dying.fPersistTimer.active);
 dying.fFlags=0;dying.fState=CLOSED;assert(dying._SendQueued()==B_ERROR&&!dying.fPersistTimer.active);
 // Regular zero-window persist still forces a probe; local retries obey send policy.
 TCPEndpoint closedWindow;closedWindow.fSendWindow=0;
 assert(closedWindow._SendQueued()==B_OK&&closedWindow.fPersistTimer.delay==5000000);
 before=shouldChecks;dispatch(closedWindow);assert(shouldChecks==before);
 TCPEndpoint policy;policy.result=ENOBUFS;assert(policy._SendQueued()==ENOBUFS);
 policy.result=B_OK;policy.allowSend=false;before=sendCalls;dispatch(policy);
 assert(sendCalls==before&&policy.fPersistTimer.active&&policy.fPersistTimer.delay==5000000);
 assert(!owned);
 puts("PASS actual queued sender/retry/persist: first rejection, eventual delivery, allocation/copy ownership, existing deadlines, partial send, permanent errors, teardown and zero-window policy");
}
'''
code=fixture
if 'TCPEndpoint::_RetryLocalSend(' in source:code+='\nstatus_t\n'+extract('_RetryLocalSend')
code+='\nstatus_t\n'+extract('_SendQueued')+'\nvoid\n'+extract('_PersistTimer')+checks
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory)/'local_retry.cpp';p.write_text(code)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
