# SPDX-License-Identifier: MIT
"""Run the production receive prefix with a grown queue and reordered data."""
from pathlib import Path
import os
import subprocess
import tempfile

if 'TCP_SOURCE' in os.environ:
    source_path = Path(os.environ['TCP_SOURCE'])
else:
    root = Path(__file__).resolve().parents[7]
    source_path = root / 'src/add-ons/kernel/network/protocols/tcp/TCPEndpoint.cpp'
source = source_path.read_text()
header = source_path.with_name('tcp.h').read_text()
sequence = header[header.index('class tcp_sequence {'):
                  header.index('// TCP flag constants')]
helpers = source[source.index('static inline bool\nin_window('):
                 source.index('static inline bool\nis_writable(')]
start = source.index('TCPEndpoint::_Receive(tcp_segment_header& segment,')
end = source.index('\n\tif ((segment.flags & TCP_FLAG_RESET) != 0) {', start)
# Keep the actual PAWS, fast-path and segment-acceptability code. Stop before
# unrelated connection-state processing; KEEP means it passed validation.
receive = 'int32\n' + source[start:end] + '\n\treturn KEEP;\n}\n'
fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <algorithm>
using uint32 = uint32_t; using int32 = int32_t;
#define ASSERT assert
#define TRACE(...) ((void)0)
#define max_c(a,b) std::max<uint32>(a,b)
enum { CLOSED, TIME_WAIT, ESTABLISHED, SYNCHRONIZE_SENT, LISTEN };
enum { DROP=2, KEEP=1, IMMEDIATE_ACKNOWLEDGE=4, ACKNOWLEDGE=8 };
enum { FLAG_OPTION_TIMESTAMP=1, FLAG_NO_RECEIVE=2 };
enum { TCP_HAS_TIMESTAMPS=1, TCP_FLAG_RESET=4, TCP_FLAG_PUSH=8 };
'''+sequence+r'''
struct net_buffer { size_t size=1448; };
struct tcp_segment_header {
 uint32 sequence=0, acknowledge=0, timestamp_value=0;
 uint32 flags=16, options=0;
 uint32 AdvertisedWindow(uint32) const { return 1000; }
 bool AcknowledgeOnly() const { return flags==16; }
};
'''+helpers+r'''
struct Queue {
 uint32 available=65535;
 bool IsContiguous() const { return true; }
 uint32 Free() const { return available; }
};
struct TCPEndpoint {
 uint32 fFlags=0, fReceivedTimestamp=0, fSendWindowShift=0, fSendWindow=1000;
 int fState=ESTABLISHED;
 tcp_sequence fReceiveNext=1000000, fSendNext=0, fSendMax=0, fSendUnacknowledged=0;
 uint32 fReceiveWindow=65535;
 Queue fReceiveQueue;
 void _UpdateTimestamps(tcp_segment_header&, size_t) {}
 void _Acknowledged(tcp_segment_header&) {}
 bool _AddData(tcp_segment_header&, net_buffer*) { return true; }
 void _NotifyReader() {}
 int32 _Receive(tcp_segment_header&, net_buffer*);
};
'''
tests = r'''
int main() {
 for (uint32 next : {1000000U, 0xffff0000U}) {
  TCPEndpoint e; net_buffer b; tcp_segment_header s;
  e.fReceiveNext=next;
  // Queue autotuning already advertised 1.2 MB while the validation window
  // remained 64 KB. A packet 110 KB ahead must reach buffering/SACK handling.
  e.fReceiveQueue.available=1246720;
  s.sequence=next+110000;
  assert(e._Receive(s,&b)==KEEP);
  assert(e.fReceiveWindow==1246720);
  s.sequence=next+1246720;
  assert(e._Receive(s,&b)==(DROP|IMMEDIATE_ACKNOWLEDGE));
  s.sequence=next-2000;
  assert(e._Receive(s,&b)==(DROP|IMMEDIATE_ACKNOWLEDGE));
  s.sequence=next-100; // partial overlap remains acceptable
  assert(e._Receive(s,&b)==KEEP);
  // Queued bytes must not shrink a window we already allowed.
  e.fReceiveQueue.available=0; s.sequence=next+110000;
  assert(e._Receive(s,&b)==KEEP);
 }
 TCPEndpoint zero; net_buffer b; tcp_segment_header s;
 zero.fReceiveWindow=zero.fReceiveQueue.available=0;
 s.sequence=zero.fReceiveNext.Number();
 s.flags=TCP_FLAG_PUSH;
 assert(zero._Receive(s,&b)==(DROP|IMMEDIATE_ACKNOWLEDGE));
 b.size=0; assert(zero._Receive(s,&b)==KEEP);
 // PAWS rejection must still precede window validation.
 TCPEndpoint paws; paws.fFlags=FLAG_OPTION_TIMESTAMP;
 paws.fReceivedTimestamp=10; s.options=TCP_HAS_TIMESTAMPS;
 s.timestamp_value=9;
 assert(paws._Receive(s,&b)==(DROP|IMMEDIATE_ACKNOWLEDGE));
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'test.cpp'
    cpp.write_text(fixture + receive + tests)
    binary = Path(directory) / 'test'
    subprocess.run(['c++', '-std=c++11', '-fsanitize=address,undefined',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: grown receive window, reordering, bounds, wraparound, zero window, PAWS')
