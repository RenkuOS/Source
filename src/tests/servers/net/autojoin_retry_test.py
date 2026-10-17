# SPDX-License-Identifier: MIT
from pathlib import Path
import os, subprocess, tempfile
root=Path(os.environ.get('NETJOIN_SOURCE') or Path(__file__).resolve().parents[3]/'servers/net')
s=(root/'AutoconfigLooper.cpp').read_text()
a=s.index('\t\tcase B_NETWORK_WLAN_SCANNED:');b=s.index('\n\t\t}',a)+4
scan=s[a:b]
a=s.index('void\nAutoconfigLooper::MessageReceived(');method=s[a:]
a=s.index("void\nAutoconfigLooper::_RetryAutoJoin()");b=s.index("\n}\n",a)+3
retry=s[a:b]
a=s.index("\t\tcase B_NETWORK_DEVICE_LINK_CHANGED:");b=s.index("\n\t\t}",a)+4
link=s[a:b]
prefix=r'''
#include <assert.h>
#include <stdint.h>
typedef int status_t; typedef int32_t int32;
enum {B_OK=0, B_NETWORK_WLAN_SCANNED=1,kMsgAutoJoinNetwork=2,kMsgJoinNetwork=3,kMsgReadyToRun=4,kMsgAutoConfigureFailed=5,B_NETWORK_MONITOR=6,kMsgLeaveNetwork=7,kMsgRetryAutoJoin=8,B_NETWORK_DEVICE_LINK_CHANGED=9,IFF_UP=2,IFM_ACTIVE=1};
struct BMessage {int what,status;bool valid=false;BMessage(int w):what(w){} void AddString(const char*,const char*){} int FindInt32(const char*,int* p){*p=status;return valid?0:-1;}};
struct BLooper {void MessageReceived(BMessage*){}};
struct Target {unsigned calls=0;int result=0;int SendMessage(BMessage*,void*){calls++;return result;}};
struct DeviceName {const char* String(){return "test";} operator const char*(){return String();}};
static bool linkUp=false, enabled=true, wireless=true; static unsigned scans;
struct BNetworkInterface {BNetworkInterface(const char*){} int Flags(){return enabled?IFF_UP:0;} bool HasLink(){return linkUp;}};
struct BNetworkDevice {BNetworkDevice(const char*){} bool IsWireless(){return wireless;} void Scan(bool wait){assert(!wait);scans++;}};
struct AutoconfigLooper : BLooper {
 bool fJoiningNetwork=false,fJoinRequestPending=false,fAutoJoinDisabled=false;int fLastMediaStatus=0;DeviceName fDevice;Target fTarget;
 void _RetryAutoJoin(); void _RemoveClient(){} void _ConfigureIPv4(){} void _ReadyToRun(){} void _ConfigureIPv4Failed(){} void _NetworkMonitorNotification(BMessage*){}
 void MessageReceived(BMessage*);
 void changeLink(int media){BMessage event(0);event.valid=true;event.status=media;BMessage* message=&event;switch(B_NETWORK_DEVICE_LINK_CHANGED){
'''+link+r'''
 }}
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
 AutoconfigLooper lost;lost.fLastMediaStatus=IFM_ACTIVE;lost.fJoiningNetwork=true;
 lost.changeLink(0);assert(!lost.fJoiningNetwork);lost._RetryAutoJoin();assert(scans==1);
 lost.scan();assert(lost.fTarget.calls==1);lost._RetryAutoJoin();assert(scans==1);
 BMessage leave(kMsgLeaveNetwork);lost.MessageReceived(&leave);
 result.status=-1;lost.MessageReceived(&result);lost.scan();lost._RetryAutoJoin();
 assert(lost.fTarget.calls==1 && scans==1);
 lost.MessageReceived(&manual);assert(!lost.fAutoJoinDisabled && lost.fJoiningNetwork);
 lost.fLastMediaStatus=IFM_ACTIVE;lost.changeLink(0);lost._RetryAutoJoin();assert(scans==2);
 AutoconfigLooper quiet;enabled=false;quiet._RetryAutoJoin();enabled=true;
 linkUp=true;quiet._RetryAutoJoin();linkUp=false;wireless=false;quiet._RetryAutoJoin();wireless=true;
 assert(scans==2);quiet._RetryAutoJoin();assert(scans==3);
 AutoconfigLooper d;d.fLastMediaStatus=IFM_ACTIVE;d.scan();assert(d.fTarget.calls==0);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp';p.write_text(prefix+retry+method+main)
 subprocess.run(['c++','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('PASS: initial retry, link-loss retry, explicit leave, disabled/wired/linked scan suppression, pending join ownership')
