# SPDX-License-Identifier: MIT
"""Verify both existing Rijndael implementations against known and random inputs."""
from pathlib import Path
import os
import platform
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[5]
wlan = root / 'src/libs/compat/freebsd_wlan'
source = wlan / 'crypto/rijndael/rijndael-alg-fst.c'
# AES-128/192/256 examples from FIPS 197, Appendix C.
code = r'''
// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern "C" {
int loopKeySetupEnc(uint32_t*, const uint8_t*, int);
int loopKeySetupDec(uint32_t*, const uint8_t*, int);
void loopEncrypt(const uint32_t*, int, const uint8_t*, uint8_t*);
void loopDecrypt(const uint32_t*, int, const uint8_t*, uint8_t*);
int fullKeySetupEnc(uint32_t*, const uint8_t*, int);
int fullKeySetupDec(uint32_t*, const uint8_t*, int);
void fullEncrypt(const uint32_t*, int, const uint8_t*, uint8_t*);
void fullDecrypt(const uint32_t*, int, const uint8_t*, uint8_t*);
}
static uint32_t seed = 1;
static uint8_t randomByte() { seed = seed * 1664525U + 1013904223U; return seed >> 24; }
int main(void)
{
    static const uint8_t expected[3][16] = {
        {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a},
        {0xdd,0xa9,0x7c,0xa4,0x86,0x4c,0xdf,0xe0,0x6e,0xaf,0x70,0xa0,0xec,0x0d,0x71,0x91},
        {0x8e,0xa2,0xb7,0xca,0x51,0x67,0x45,0xbf,0xea,0xfc,0x49,0x90,0x4b,0x49,0x60,0x89}};
    uint8_t key[32], input[17], a[17], b[17], restored[17];
    uint32_t le[60], ld[60], fe[60], fd[60];
    for (unsigned bits = 128; bits <= 256; bits += 64) {
        for (unsigned iteration = 0; iteration < 10001; iteration++) {
            for (unsigned i=0;i<32;i++) key[i]=iteration?randomByte():i;
            for (unsigned i=0;i<16;i++) input[i+1]=iteration?randomByte():i*17;
            int rounds=loopKeySetupEnc(le,key,bits);
            assert(loopKeySetupDec(ld,key,bits)==rounds);
            assert(fullKeySetupEnc(fe,key,bits)==rounds);
            assert(fullKeySetupDec(fd,key,bits)==rounds);
            assert(!memcmp(le,fe,4*(rounds+1)*sizeof(uint32_t)));
            assert(!memcmp(ld,fd,4*(rounds+1)*sizeof(uint32_t)));
            loopEncrypt(le,rounds,input+1,a+1);fullEncrypt(fe,rounds,input+1,b+1);
            assert(!memcmp(a+1,b+1,16));
            if (!iteration) assert(!memcmp(a+1,expected[(bits-128)/64],16));
            loopDecrypt(ld,rounds,a+1,restored+1);assert(!memcmp(restored+1,input+1,16));
            fullDecrypt(fd,rounds,b+1,restored+1);assert(!memcmp(restored+1,input+1,16));
            memcpy(a+1,input+1,16);memcpy(b+1,input+1,16);
            loopEncrypt(le,rounds,a+1,a+1);fullEncrypt(fe,rounds,b+1,b+1);assert(!memcmp(a+1,b+1,16));
            loopDecrypt(ld,rounds,a+1,a+1);fullDecrypt(fd,rounds,b+1,b+1);
            assert(!memcmp(a+1,input+1,16)&&!memcmp(b+1,input+1,16));
        }
    }
    puts("PASS AES-128/192/256 known vectors and 30000 differential keys, decrypt, unaligned and in-place buffers");
    return 0;
}
'''
COMPAT = '/* SPDX-License-Identifier: MIT */\n#include <sys/types.h>\n#include <stdint.h>\n#ifndef __FBSDID\n#define __FBSDID(x)\n#endif\ntypedef uint8_t u_int8_t;\ntypedef uint16_t u_int16_t;\ntypedef uint32_t u_int32_t;\ntypedef unsigned char u_char;\n'

with tempfile.TemporaryDirectory() as directory:
    build = Path(directory)
    (build / 'compat.h').write_text(COMPAT)
    (build / 'compare.cpp').write_text(code)
    flags = ['-O2', '-fno-strict-aliasing', '-fsanitize=address,undefined']
    kernel_flags = (['-mno-sse', '-mno-sse2', '-mno-mmx']
                    if platform.machine().lower() in ('x86_64', 'amd64', 'i386', 'i686') else [])
    objects = []
    for mode in ('loop', 'full'):
        output = build / (mode + '.o')
        renames = ['-Drijndael' + original + '=' + mode + original
                   for original in ('KeySetupEnc', 'KeySetupDec', 'Encrypt', 'Decrypt')]
        subprocess.run([os.environ.get('CC', 'cc')] + flags + kernel_flags +
                       (['-DFULL_UNROLL'] if mode == 'full' else []) + renames +
                       ['-include', str(build / 'compat.h'), '-I' + str(wlan),
                        '-c', str(source), '-o', str(output)], check=True)
        objects.append(str(output))
    binary = build / 'compare'
    subprocess.run([os.environ.get('CXX', 'c++')] + flags + ['-std=c++11',
                   str(build / 'compare.cpp')] + objects + ['-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
