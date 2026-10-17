# SPDX-License-Identifier: MIT
"""Exercise the production delayed-ACK decision with timestamp-sized packets."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[7]
source = (root / 'src/add-ons/kernel/network/protocols/tcp/TCPEndpoint.cpp').read_text()
a = source.index('status_t\nTCPEndpoint::DelayedAcknowledge()')
function = source[a:source.index('\n}', a) + 2]
a = source.index('\tif (segment.flags & TCP_FLAG_ACKNOWLEDGE) {', source.index('fReceiveMaxAdvertised = fReceiveNext + segment.AdvertisedWindow'))
reset = source[a:source.index('\n\t}', a) + 3]
fixture = r'''
#include <cassert>
#include <cstdint>
using status_t = int; using bigtime_t = long long;
constexpr int B_OK = 0;
constexpr int TCP_FLAG_ACKNOWLEDGE = 16;
constexpr bigtime_t TCP_DELAYED_ACKNOWLEDGE_TIMEOUT = 100000;
#define T(x) ((void)0)
struct Timer { bool active = false; int sets = 0; bigtime_t delay = -1; };
struct Stack {
 bool is_timer_active(Timer* t) { return t->active; }
 void set_timer(Timer* t, bigtime_t delay) { t->active=true; t->delay=delay; t->sets++; }
 void cancel_timer(Timer* t) { t->active=false; }
} stack;
auto* gStackModule=&stack;
struct TCPEndpoint {
 uint32_t fReceiveNext=0, fLastAcknowledgeSent=0, fReceiveMaxSegmentSize=1460;
 uint8_t fSegmentsSinceAcknowledge=0;
 Timer fDelayedAcknowledgeTimer;
 status_t DelayedAcknowledge();
 void received(unsigned bytes) { fReceiveNext+=bytes; DelayedAcknowledge(); }
 void sent_ack() {
  struct { int flags; uint32_t acknowledge; } segment={TCP_FLAG_ACKNOWLEDGE,fReceiveNext};
'''+reset+r'''
 }
};
'''
tests = r'''
int main() {
 TCPEndpoint e;
 e.received(1448);
 assert(e.fDelayedAcknowledgeTimer.delay==100000);
 e.received(1448);
 assert(e.fDelayedAcknowledgeTimer.delay==0);
 e.sent_ack();
 assert(!e.fDelayedAcknowledgeTimer.active && e.fSegmentsSinceAcknowledge==0);
 e.received(1448);
 assert(e.fDelayedAcknowledgeTimer.delay==100000);
 e.received(1448);
 assert(e.fDelayedAcknowledgeTimer.delay==0);
 for(int i=0;i<1000;i++) e.received(1);
 assert(e.fSegmentsSinceAcknowledge==2 && e.fDelayedAcknowledgeTimer.delay==0);
 e.sent_ack();
 e.received(1);
 assert(e.fDelayedAcknowledgeTimer.delay==100000);
 e.received(1);
 assert(e.fDelayedAcknowledgeTimer.delay==0);
 e.sent_ack();
 e.received(2920);
 assert(e.fDelayedAcknowledgeTimer.delay==0);
 TCPEndpoint waiting;
 stack.set_timer(&waiting.fDelayedAcknowledgeTimer, 12345);
 waiting.received(1448);
 assert(waiting.fDelayedAcknowledgeTimer.delay==12345);
 assert(waiting.fDelayedAcknowledgeTimer.sets==1);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'test.cpp'
    path.write_text(fixture + function + tests)
    executable = str(path.with_suffix(''))
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
                    '-fsanitize=address,undefined', str(path), '-o', executable], check=True)
    subprocess.run([executable], check=True)
print('PASS: timestamp-sized pairs ACK immediately, timer is bounded, count resets')
