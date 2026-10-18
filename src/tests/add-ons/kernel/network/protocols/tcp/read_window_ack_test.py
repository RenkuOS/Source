# SPDX-License-Identifier: MIT
"""Execute the actual TCP read path with and without a receive gap."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(os.environ['TCP_SOURCE']) if 'TCP_SOURCE' in os.environ else Path(__file__).resolve().parents[7]/'src/add-ons/kernel/network/protocols/tcp'
s=(root/'TCPEndpoint.cpp').read_text();a=s.index('TCPEndpoint::ReadData(');f='status_t\n'+s[a:s.index('\n}',a)+2]
h=(root/'tcp.h').read_text();a=h.index('class tcp_sequence {');sequence=h[a:h.index('// TCP flag constants',a)]
fixture=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <sys/socket.h>
using uint32=uint32_t;using int32=int32_t;using bigtime_t=long long;using status_t=int;
#define TRACE(...)
#define T(...)
constexpr int CLOSED=0,LISTEN=1,SYNCHRONIZE_SENT=2,SYNCHRONIZE_RECEIVED=3,ESTABLISHED=4;
constexpr int CLOSING=5,WAIT_FOR_FINISH_ACKNOWLEDGE=6,TIME_WAIT=7,FINISH_RECEIVED=8;
constexpr int FLAG_NO_RECEIVE=1,B_OK=0,B_WOULD_BLOCK=-11,B_TIMED_OUT=-12,B_INTERRUPTED=-13;
int posix_error(int s){return s;}
'''+sequence+r'''
struct net_buffer{} payload;
struct MutexLocker{MutexLocker(int&){}void Unlock(){}void Lock(){}};
struct Condition{void NotifyAll(){}};
struct Stack{bool is_restarted_syscall(){return false;}
 bigtime_t restore_syscall_restart_timeout(){return 0;}
 bigtime_t set_syscall_restart_timeout(bigtime_t t){return t;}}stack;
auto*gStackModule=&stack;
struct Socket {int error=B_OK;struct {bigtime_t timeout=0;size_t low_water_mark=1;}receive;}sock;
struct Queue {
 bool contiguous=false;size_t available=1000,size=65535;
 size_t Available(){return available;}size_t PushedData(){return 0;}size_t Size(){return size;}
 bool IsContiguous(){return contiguous;}
 status_t Get(size_t n,bool remove,net_buffer**out){assert(available>0);*out=&payload;
  if(remove)available-=n<available?n:available;return B_OK;}
};
struct TCPEndpoint {
 int fLock=0,fState=ESTABLISHED;unsigned fFlags=0;Socket*socket=&sock;
 Queue fReceiveQueue;Condition fReceiveCondition;unsigned acks=0;
 tcp_sequence fReceiveNext=1000,fReceiveMaxAdvertised=2000;
 status_t _WaitForEstablished(MutexLocker&,bigtime_t){return B_TIMED_OUT;}
 status_t _WaitForCondition(Condition&,MutexLocker&,bigtime_t){return B_TIMED_OUT;}
 status_t _SendAcknowledge(){acks++;return B_OK;}
 status_t ReadData(size_t,uint32,net_buffer**);
};
'''
checks=r'''
int main(){
 TCPEndpoint e;net_buffer*out=nullptr;
 assert(e.ReadData(100,MSG_DONTWAIT,&out)==B_OK&&out==&payload);
 assert(e.acks==0&&"reading behind a gap cannot advertise a window update");
 // Keep ordinary receive-window reopening when the queue is contiguous.
 e=TCPEndpoint();e.fReceiveQueue.contiguous=true;
 assert(e.ReadData(100,MSG_DONTWAIT,&out)==B_OK&&e.acks==1);
 // No reopening is needed while the old window still has ample space.
 e=TCPEndpoint();e.fReceiveQueue.contiguous=true;e.fReceiveMaxAdvertised=60000;
 assert(e.ReadData(100,MSG_DONTWAIT,&out)==B_OK&&e.acks==0);
 // Peeking consumes nothing and never generates an update, with or without gaps.
 for(bool contiguous:{false,true}){
  e=TCPEndpoint();e.fReceiveQueue.contiguous=contiguous;
  assert(e.ReadData(100,MSG_DONTWAIT|MSG_PEEK,&out)==B_OK);
  assert(e.acks==0&&e.fReceiveQueue.available==1000);
 }
 e=TCPEndpoint();e.fReceiveQueue.available=0;
 assert(e.ReadData(100,MSG_DONTWAIT,&out)==B_WOULD_BLOCK&&e.acks==0);
 // Once a gap is repaired, the next read can reopen the window normally.
 e=TCPEndpoint();assert(e.ReadData(100,MSG_DONTWAIT,&out)==B_OK&&e.acks==0);
 e.fReceiveQueue.contiguous=true;
 assert(e.ReadData(100,MSG_DONTWAIT,&out)==B_OK&&e.acks==1);
 puts("PASS actual TCP ReadData: no read-driven ACK while gap freezes window, contiguous reopening, peek, empty read and gap repair");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'read.cpp';p.write_text(fixture+f+checks)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-include','initializer_list','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
