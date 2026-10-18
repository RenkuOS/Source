# SPDX-License-Identifier: MIT
from pathlib import Path
import os, subprocess, tempfile
p=Path(__file__).resolve().parent
root=Path(os.environ.get("NETJOIN_SOURCE") or p.parents[2]/"servers/net")

def function(s,key):
 start=s.index(key);start=s.rfind('status_t\n',0,start)
 brace=s.index('{',start);depth=1;i=brace+1
 while depth:
  if s[i]=='{':depth+=1
  if s[i]=='}':depth-=1
  i+=1
 return s[start:i]

common=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
using int32=int32_t;using uint32=uint32_t;using status_t=int;
constexpr int B_OK=0,B_BAD_VALUE=-1,B_CANCELED=-2,B_NO_INIT=-3,B_ERROR=-4,AF_LINK=18;
constexpr unsigned kMsgJoinNetwork=1,kMsgAutoJoinNetwork=2,B_REPLY=3;
int32 atomic_get(int32* p){return __atomic_load_n(p,__ATOMIC_SEQ_CST);}
int32 atomic_add(int32* p,int32 n){return __atomic_fetch_add(p,n,__ATOMIC_SEQ_CST);}
struct BMessage {
 unsigned what;std::map<std::string,std::string> strings;std::map<std::string,int32> ints;
 BMessage(unsigned w=0):what(w){}
 int AddString(const char*k,const char*v){strings[k]=v;return B_OK;}
 int AddInt32(const char*k,int32 v){ints[k]=v;return B_OK;}
 int FindString(const char*k,const char**v)const{auto i=strings.find(k);*v=i==strings.end()?nullptr:i->second.c_str();return *v?B_OK:B_BAD_VALUE;}
 int FindInt32(const char*k,int32*v)const{auto i=ints.find(k);if(i==ints.end())return B_BAD_VALUE;*v=i->second;return B_OK;}
 void SendReply(BMessage*){}
};
struct BNetworkAddress {void SetTo(int,const char*){}};
struct wireless_network {char name[32];};
struct BNetworkDevice {BNetworkDevice(const char*){} int GetNetwork(const BNetworkAddress&,wireless_network&n){std::strcpy(n.name,"saved");return B_OK;}int GetNetwork(const char*,wireless_network&n){std::strcpy(n.name,"saved");return B_OK;}};
struct Settings {int GetNextNetwork(uint32& cookie,BMessage&m){if(cookie++!=0)return B_BAD_VALUE;m.AddString("name","saved");return B_OK;}};
struct Names {std::set<std::string> names;bool HasString(const char*n){return names.count(n);}void Remove(const char*n){names.erase(n);}};
struct AutoconfigLooper {int32 fAutoJoinGeneration=0;
 int32 AutoJoinGeneration(){return atomic_get(&fAutoJoinGeneration);}
 void InvalidateAutoJoinRequests(){atomic_add(&fAutoJoinGeneration,1);}
};
struct NetServer {
 Settings fSettings;Names fAutoJoinDisabled;std::map<std::string,AutoconfigLooper*> loopers;std::vector<unsigned> requests;status_t joinStatus=B_OK;
 AutoconfigLooper* _LooperForDevice(const char*n){auto i=loopers.find(n);return i==loopers.end()?nullptr:i->second;}
 status_t _JoinNetwork(const BMessage&m,const BNetworkAddress* =nullptr,const char* =nullptr){if(joinStatus==B_OK)requests.push_back(m.what);return joinStatus;}
 status_t _AutoJoinNetwork(const BMessage&);
 void MessageReceived(BMessage*);
};
'''
main=r'''
BMessage request(unsigned what,const char* device,int generation) {BMessage m(what);m.AddString("device",device);m.AddInt32("autoJoinGeneration",generation);return m;}
int main(){
 AutoconfigLooper wifi,other;NetServer server;server.loopers["wifi"]=&wifi;server.loopers["other"]=&other;
 // Real race: auto request left the looper, manual request is handled first,
 // then that previously queued auto request reaches net_server.
 BMessage automatic=request(kMsgAutoJoinNetwork,"wifi",0);
 BMessage manual=request(kMsgJoinNetwork,"wifi",0);
 server.MessageReceived(&manual);
 status_t status=server._AutoJoinNetwork(automatic);
 assert(status==B_CANCELED);
 assert(server.requests.size()==1 && server.requests.back()==kMsgJoinNetwork);
 // A genuinely later auto request is still allowed; this is not a global
 // auto-connect disable and does not claim to solve every reconnect race.
 BMessage fresh=request(kMsgAutoJoinNetwork,"wifi",wifi.AutoJoinGeneration());
 assert(server._AutoJoinNetwork(fresh)==B_OK);
 BMessage independent=request(kMsgAutoJoinNetwork,"other",0);
 assert(server._AutoJoinNetwork(independent)==B_OK);
 // Repeated manual requests invalidate each older generation independently.
 BMessage queued=fresh;server.MessageReceived(&manual);
 assert(server._AutoJoinNetwork(queued)==B_CANCELED);
 // A failed manual request does not supersede an already valid auto request.
 int before=wifi.AutoJoinGeneration();server.joinStatus=B_BAD_VALUE;
 server.MessageReceived(&manual);assert(wifi.AutoJoinGeneration()==before);
 server.joinStatus=B_OK;
 BMessage current=request(kMsgAutoJoinNetwork,"wifi",before);
 assert(server._AutoJoinNetwork(current)==B_OK);
 // Opt-out remains authoritative; untagged or vanished-looper requests stop.
 server.fAutoJoinDisabled.names.insert("wifi");assert(server._AutoJoinNetwork(current)==B_CANCELED);
 server.fAutoJoinDisabled.names.clear();current.ints.clear();assert(server._AutoJoinNetwork(current)==B_CANCELED);
 server.loopers.erase("wifi");assert(server._AutoJoinNetwork(fresh)==B_CANCELED);
 std::puts("actual net_server handlers: stale queued auto rejected, manual/fresh/device/failure/opt-out cases PASS");
}
'''
s=(root/'NetServer.cpp').read_text()
start=s.index('\t\tcase kMsgJoinNetwork:');end=s.index('\t\tcase kMsgLeaveNetwork:',start)
dispatcher='void NetServer::MessageReceived(BMessage* message) {switch(message->what){\n'+s[start:end]+'default:break;}}\n'
with tempfile.TemporaryDirectory() as directory:
 source=Path(directory)/'test.cpp'
 source.write_text(common+dispatcher+function(s,'NetServer::_AutoJoinNetwork(')+main)
 binary=source.with_suffix('')
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O1','-g','-fsanitize=address,undefined',str(source),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
