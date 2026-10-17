# SPDX-License-Identifier: MIT
"""Check production window encoding/decoding for SYN and established traffic."""
from pathlib import Path
import os
import subprocess
import tempfile

if 'TCP_HEADER' in os.environ:
    header = Path(os.environ['TCP_HEADER'])
else:
    header = Path(__file__).resolve().parents[7] / 'src/add-ons/kernel/network/protocols/tcp/tcp.h'
source = header.read_text()
start = source.index('\tuint32 AdvertisedWindow(uint8 windowShift) const')
end = source.index('\n};', start)
methods = source[start:end]
code = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include <algorithm>
using uint8=uint8_t; using uint16=uint16_t; using uint32=uint32_t;
#define min_c(a,b) std::min<size_t>(a,b)
constexpr int TCP_MAX_WINDOW=65535, TCP_FLAG_SYNCHRONIZE=2;
struct Header {
 uint8 flags=0;
 uint16 advertised_window=0;
'''+methods+r'''
};
int main() {
 for (uint8 flags : {0, 2, 16, 18, 17}) {
  for (uint8 shift : {0, 1, 7, 8, 14}) {
   for (size_t bytes : {0U, 1U, 255U, 65535U, 65536U, 1048576U, 1073741824U}) {
    Header h; h.flags=flags; h.SetAdvertisedWindow(bytes,shift);
    const uint8 effective=(flags&2)?0:shift;
    const uint32 wire=std::min<size_t>(65535,bytes>>effective);
    assert(h.advertised_window==wire);
    assert(h.AdvertisedWindow(shift)==(wire<<effective));
    h.advertised_window=65535;
    assert(h.AdvertisedWindow(shift)==(65535U<<effective));
   }
  }
 }
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'test.cpp'
    cpp.write_text(code)
    binary = Path(directory) / 'test'
    subprocess.run(['c++', '-std=c++11', '-fsanitize=address,undefined',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: unscaled SYN/SYN-ACK and scaled established windows, bounds and zero')
