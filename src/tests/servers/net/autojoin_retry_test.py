# SPDX-License-Identifier: MIT
from pathlib import Path
import os, subprocess, tempfile
root=Path(os.environ.get('NETJOIN_SOURCE',Path(__file__).resolve().parents[3]/'servers/net'))
s=(root/'AutoconfigLooper.cpp').read_text()
a=s.index('\t\tcase B_NETWORK_WLAN_SCANNED:');b=s.index('\n\t\t}',a)+4
scan=s[a:b]
a=s.index('void\nAutoconfigLooper::MessageReceived(');method=s[a:]
prefix=r'''
#include <assert.h>
#include <stdint.h>
typedef int status_t;
enum {B_OK=0, B_NETWORK_WLAN_SCANNED=1,kMsgAutoJoinNetwork=2,kMsgJoinNetwork=3,kMsgReadyToRun=4,kMsgAutoConfigureFailed=5,B_NETWORK_MONITOR=6,IFM_ACTIVE=1};
struct BMessage {int what,status;bool valid=false;BMessage(int w):what(w){} void AddString(const char*,const char*){} int FindInt32(const char*,int* p){*p=status;return valid?0:-1;}};
struct BLooper {void MessageReceived(BMessage*){}};
struct Target {unsigned calls=0;int result=0;int SendMessage(BMessage*,void*){calls++;return result;}};
struct AutoconfigLooper : BLooper {
 bool fJoiningNetwork=false,fJoinRequestPending=false;int fLastMediaStatus=0;const char* fDevice="test";Target fTarget;
 void _ReadyToRun(){} void _ConfigureIPv4Failed(){} void _NetworkMonitorNotification(BMessage*){}
 void MessageReceived(BMessage*);
 void scan(){switch(B_NETWORK_WLAN_SCANNED){
'''+scan+r'''
 }}
};
'''
main=r'''
int main(){
 AutoconfigLooper a;a.scan();assert(a.fJoinRequestPending && !a.fJoiningNetwork && a.fTarget.calls==1);
 a.scan();assert(a.fTarget.calls==1);
 BMessage result(kMsgAutoJoinNetwork);result.valid=true;result.status=-1;a.MessageReceived(&result);
 assert(!a.fJoinRequestPending && !a.fJoiningNetwork);a.scan();assert(a.fTarget.calls==2);
 result.status=0;a.MessageReceived(&result);a.scan();assert(a.fJoiningNetwork && a.fTarget.calls==2);
 AutoconfigLooper b;b.fTarget.result=-1;b.scan();assert(!b.fJoinRequestPending);b.scan();assert(b.fTarget.calls==2);
 AutoconfigLooper c;c.scan();BMessage manual(kMsgJoinNetwork);c.MessageReceived(&manual);
 result.status=-1;c.MessageReceived(&result);c.scan();assert(c.fJoiningNetwork && c.fTarget.calls==1);
 AutoconfigLooper d;d.fLastMediaStatus=IFM_ACTIVE;d.scan();assert(d.fTarget.calls==0);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(prefix+method+main)
 subprocess.run(['c++','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('PASS: failed initial autojoin retries, pending/success suppress duplicates, manual join survives stale failure')
