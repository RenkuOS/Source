# SPDX-License-Identifier: MIT
"""Exercise production Connect and SYN retransmission under local backpressure."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(os.environ['TCP_SOURCE']) if 'TCP_SOURCE' in os.environ else Path(__file__).resolve().parents[7]/'src/add-ons/kernel/network/protocols/tcp'
s=(root/'TCPEndpoint.cpp').read_text()
def method(name):
 a=s.index(name+'(');a=s.rfind('\n',0,a)+1
 return s[a:s.index('\n}',a)+2]
code=r'''
#include <cassert>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cerrno>
// Haiku errno constants are negative status_t values, unlike Linux errno.
#undef ENETUNREACH
#undef ENOBUFS
#undef ENETDOWN
#undef EACCES
const int ENETUNREACH=-200,ENOBUFS=-201,ENETDOWN=-202,EACCES=-203;
using std::min;
typedef int status_t;typedef long long bigtime_t;
const int B_OK=0,B_NO_MEMORY=-100,B_WOULD_BLOCK=-101,B_ERROR=-102;
const int CLOSED=0,LISTEN=1,SYNCHRONIZE_SENT=2,ESTABLISHED=4;
const int FLAG_CAN_NOTIFY=1,FLAG_RECOVERY=2;
const bigtime_t TCP_CONNECTION_TIMEOUT=75000000,TCP_SYN_RETRANSMIT_TIMEOUT=1000000,TCP_MAX_RETRANSMIT_TIMEOUT=64000000;
#define TRACE(...) ((void)0)
#define T(...) ((void)0)
#define min_c(a,b) std::min(a,b)
#define B_PRIdBIGTIME "lld"
struct sockaddr{};struct sockaddr_storage:sockaddr{};struct sockaddr_in:sockaddr{int sin_port;};
struct MutexLocker{MutexLocker(int&){};};
struct Timer{bool active=false;bigtime_t delay=0;};
struct Stack{bool restart=false;int sets=0;bool is_restarted_syscall(){return restart;}
 bigtime_t restore_syscall_restart_timeout(){return 123;}
 bigtime_t set_syscall_restart_timeout(bigtime_t t){return t;}
 void set_timer(Timer*t,bigtime_t d){t->active=true;t->delay=d;sets++;}
}stack,*gStackModule=&stack;
struct Address{bool family=true,empty=false;bool is_same_family(const sockaddr*){return family;}
 bool is_empty_address(const sockaddr*,bool){return empty;}
 void get_loopback_address(sockaddr*){}};
struct Sock{struct {bigtime_t timeout=TCP_CONNECTION_TIMEOUT;}send;};
struct Sockets{int backlogs=0;void set_max_backlog(Sock*,int){backlogs++;}}sockets,*gSocketModule=&sockets;
struct Seq{uint32_t v=0;uint32_t Number(){return v;}};
static int posix_error(int s){return s;}
class TCPEndpoint{public:
 int fLock=0,fState=CLOSED,fFlags=0,fRetransmitInitialCount=99;
 int fCongestionWindow=8000,fSendMaxSegmentSize=1448,fDuplicateAcknowledgeCount=0;
 bigtime_t fRetransmitTimeout=TCP_SYN_RETRANSMIT_TIMEOUT;
 Seq fSendNext{100},fSendUnacknowledged{100};uint32_t fRecover=0;Timer fRetransmitTimer;
 Sock storage,*socket=&storage;Address address;
 std::vector<int> outcomes;int sends=0,closed=0,prepared=0,prepareStatus=B_OK,waits=0;
 bool loopback=false,completeInWait=true;bigtime_t waited=0;
 Address* AddressModule(){return &address;}
 status_t _PrepareSendPath(const sockaddr*){prepared++;return prepareStatus;}
 status_t _SendAcknowledge(){assert(sends<(int)outcomes.size());int result=outcomes[sends++];
  if(result==B_OK){fSendNext.v=fSendUnacknowledged.v+1;if(loopback)fState=ESTABLISHED;}return result;}
 void _Close(){closed++;fState=CLOSED;fRetransmitTimer.active=false;}
 void _ResetSlowStart(){fCongestionWindow=fSendMaxSegmentSize;}
 void _SendQueued(){assert(false);}
 status_t _WaitForEstablished(MutexLocker&,bigtime_t timeout){waits++;waited=timeout;
  if(completeInWait){assert(fRetransmitTimer.active);fRetransmitTimer.active=false;
   _Retransmit();assert(sends==2);fState=ESTABLISHED;fRetransmitTimer.active=false;return B_OK;}
  return ETIMEDOUT;}
 status_t Connect(const sockaddr*);void _Retransmit();
};
'''+'\nstatus_t\n'+method('TCPEndpoint::Connect')+'\nvoid\n'+method('TCPEndpoint::_Retransmit')+r'''
int main(){sockaddr_in a{};
 for(int transient:{ENOBUFS,B_NO_MEMORY,B_WOULD_BLOCK}){
  TCPEndpoint e;e.outcomes={transient,B_OK};
  assert(e.Connect(&a)==B_OK);assert(e.closed==0&&e.waits==1&&e.sends==2);
  assert(e.fRetransmitInitialCount==1&&e.waited==TCP_CONNECTION_TIMEOUT);
  TCPEndpoint n;n.socket->send.timeout=0;n.outcomes={transient,transient,B_OK};
  assert(n.Connect(&a)==EINPROGRESS);assert(n.closed==0&&n.fState==SYNCHRONIZE_SENT);
  assert(n.fRetransmitTimer.active&&n.fRetransmitTimer.delay==TCP_SYN_RETRANSMIT_TIMEOUT);
  assert(n.fRetransmitInitialCount==0&&n.fSendNext.v==100);
  n.fRetransmitTimer.active=false;n._Retransmit();assert(n.fRetransmitInitialCount==0);
  assert(n.fSendNext.v==100&&n.fRetransmitTimer.active);
  n.fRetransmitTimer.active=false;n._Retransmit();assert(n.fRetransmitInitialCount==1&&n.fSendNext.v==101);
  TCPEndpoint t;t.outcomes={transient};t.completeInWait=false;t.socket->send.timeout=123456;
  assert(t.Connect(&a)==ETIMEDOUT&&t.waited==123456&&t.closed==0);
 }
 for(int hard:{ENETUNREACH,ENETDOWN,EACCES,B_ERROR}){
  TCPEndpoint e;e.outcomes={hard};assert(e.Connect(&a)==hard);
  assert(e.closed==1&&!e.fRetransmitTimer.active&&e.waits==0);
 }
 TCPEndpoint success;success.outcomes={B_OK,B_OK};assert(success.Connect(&a)==B_OK);
 TCPEndpoint loop;loop.loopback=true;loop.outcomes={B_OK};assert(loop.Connect(&a)==B_OK&&loop.waits==0&&!loop.fRetransmitTimer.active);
 TCPEndpoint family;family.address.family=false;assert(family.Connect(&a)==EAFNOSUPPORT&&family.prepared==0);
 TCPEndpoint route;route.prepareStatus=ENETUNREACH;assert(route.Connect(&a)==ENETUNREACH&&route.sends==0);
 TCPEndpoint connected;connected.fState=ESTABLISHED;assert(connected.Connect(&a)==EISCONN&&connected.sends==0);
 TCPEndpoint pending;pending.fState=SYNCHRONIZE_SENT;assert(pending.Connect(&a)==EALREADY&&pending.sends==0);
 TCPEndpoint listen;listen.fState=LISTEN;listen.outcomes={ENOBUFS,B_OK};assert(listen.Connect(&a)==B_OK&&sockets.backlogs==1);
 puts("PASS actual Connect/SYN retry: transient failures, timer, blocking/nonblocking, timeout, hard errors, loopback and state guards");
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
