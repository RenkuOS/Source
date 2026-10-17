# SPDX-License-Identifier: MIT
"""Dispatch actual TCP callbacks concurrently with the actual destructor."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[7]
source = (root / 'src/add-ons/kernel/network/protocols/tcp/TCPEndpoint.cpp').read_text()

def extract(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]

fixture = r"""
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#define T(x)
constexpr int CLOSED=0, ESTABLISHED=4;
constexpr unsigned FLAG_CLOSED=8, FLAG_DELETE_ON_CLOSE=16, FLAG_DESTROYING=0x800;
struct mutex { std::mutex native; std::atomic<bool> alive{true}, owned{false}; };
int mutex_lock(mutex* m) {
 assert(m->alive && "callback attempted to lock a destroyed mutex");
 m->native.lock(); assert(m->alive); m->owned=true; return 0;
}
void mutex_unlock(mutex* m) { assert(m->alive && m->owned);m->owned=false;m->native.unlock(); }
void mutex_destroy(mutex* m) {
 assert(m->alive);m->alive=false;
 if(m->owned.exchange(false))m->native.unlock();
}
struct MutexLocker {
 mutex* value;bool locked;
 MutexLocker(mutex& m):value(&m),locked(mutex_lock(&m)==0){}
 ~MutexLocker(){if(locked)mutex_unlock(value);}
 bool IsLocked(){return locked;}
 void Unlock(){mutex_unlock(value);locked=false;}
};
struct net_timer { bool scheduled=false; };
std::mutex control;
std::condition_variable changed;
net_timer* running=nullptr;
bool waiting=false;
thread_local bool onTimerThread=false;
unsigned work=0,releases=0,routes=0;
struct Stack {
 bool cancel_timer(net_timer* t){t->scheduled=false;return true;}
 bool is_timer_active(net_timer* t){return t->scheduled;}
 int wait_for_timer(net_timer* t) {
  if(onTimerThread)return -1; // production wait_for_timer does not wait for itself
  std::unique_lock<std::mutex> lock(control);
  if(running==t){waiting=true;changed.notify_all();}
  changed.wait(lock,[&]{return running!=t;});assert(!t->scheduled);return 0;
 }
} stack;
auto* gStackModule=&stack;
struct Manager {void Unbind(void*){}};
void put_endpoint_manager(Manager*){}
struct Datalink {void put_route(int,void*){routes++;}} datalink;
auto* gDatalinkModule=&datalink;
struct Sockets {bool deleting=false;void release_socket(void*); } sockets;
auto* gSocketModule=&sockets;
struct TCPEndpoint {
 mutex fLock;
 unsigned fFlags=0;int fState=ESTABLISHED;
 Manager* fManager=nullptr;void* fRoute=nullptr;void* socket=this;
 net_timer fRetransmitTimer,fPersistTimer,fDelayedAcknowledgeTimer,fTimeWaitTimer;
 ~TCPEndpoint();
 int Domain(){return 0;}int State(){return fState;}
 void _CancelConnectionTimers(){
  stack.cancel_timer(&fRetransmitTimer);stack.cancel_timer(&fPersistTimer);
  stack.cancel_timer(&fDelayedAcknowledgeTimer);
 }
 void _Retransmit(){work++;}void _SendQueued(bool){work++;}void _SendAcknowledge(){work++;}
 static void _RetransmitTimer(net_timer*,void*);
 static void _PersistTimer(net_timer*,void*);
 static void _DelayedAcknowledgeTimer(net_timer*,void*);
 static void _TimeWaitTimer(net_timer*,void*);
};
void Sockets::release_socket(void* endpoint){releases++;if(deleting)delete (TCPEndpoint*)endpoint;}
"""
checks = r"""
using Hook=void(*)(net_timer*,void*);
int main(){
 Hook hooks[]={TCPEndpoint::_RetransmitTimer,TCPEndpoint::_PersistTimer,
  TCPEndpoint::_DelayedAcknowledgeTimer,TCPEndpoint::_TimeWaitTimer};
 // A timer has been dispatched, but has not entered its callback yet.
 // Cancellation cannot retract that callback. Pause destruction at its
 // real wait_for_timer call, then invoke the actual callback.
 for(unsigned kind=0;kind<4;kind++)for(unsigned iteration=0;iteration<32;iteration++){
  auto* endpoint=new TCPEndpoint;
  net_timer* timers[]={&endpoint->fRetransmitTimer,&endpoint->fPersistTimer,
   &endpoint->fDelayedAcknowledgeTimer,&endpoint->fTimeWaitTimer};
  for(auto* timer:timers)timer->scheduled=true;
  running=timers[kind];waiting=false;unsigned oldWork=work,oldReleases=releases;
  std::thread destroy([&]{delete endpoint;});
  {
   std::unique_lock<std::mutex> lock(control);
   assert(changed.wait_for(lock,std::chrono::seconds(3),[]{return waiting;}));
  }
  onTimerThread=true;hooks[kind](timers[kind],endpoint);onTimerThread=false;
  {std::lock_guard<std::mutex> lock(control);running=nullptr;changed.notify_all();}
  destroy.join();assert(work==oldWork && releases==oldReleases);
 }
 // Live endpoints retain their normal callback behavior.
 for(unsigned kind=0;kind<4;kind++){
  auto* endpoint=new TCPEndpoint;
  net_timer* timers[]={&endpoint->fRetransmitTimer,&endpoint->fPersistTimer,
   &endpoint->fDelayedAcknowledgeTimer,&endpoint->fTimeWaitTimer};
  unsigned before=work;hooks[kind](timers[kind],endpoint);
  if(kind<3)assert(work==before+1);
  else assert(endpoint->fFlags&FLAG_DELETE_ON_CLOSE);
  delete endpoint;
 }
 // TIME_WAIT can release the last socket reference on the timer thread.
 // The production wait API returns immediately in this self-delete case.
 auto* endpoint=new TCPEndpoint;endpoint->fFlags=FLAG_CLOSED;
 running=&endpoint->fTimeWaitTimer;sockets.deleting=true;onTimerThread=true;
 auto* timer=running;TCPEndpoint::_TimeWaitTimer(timer,endpoint);
 onTimerThread=false;running=nullptr;
 assert(releases==1 && routes==133);
 puts("PASS actual TCP destructor/callbacks: dispatched timers, no rearm/work, live hooks, timer-thread self-delete");
}
"""
code = fixture + '\n' + extract('TCPEndpoint::~TCPEndpoint()')
for name in ('_RetransmitTimer', '_PersistTimer', '_DelayedAcknowledgeTimer', '_TimeWaitTimer'):
    code += '\nvoid\n' + extract('TCPEndpoint::' + name + '(')
code += checks
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory) / 'timer_lifetime.cpp'
    p.write_text(code)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-pthread',
        '-fsanitize=address,undefined', str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)
