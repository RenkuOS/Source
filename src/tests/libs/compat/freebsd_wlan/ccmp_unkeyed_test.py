#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run ccmp_encrypt()/ccmp_decrypt() from ieee80211_crypto_ccmp.c: an
attached but never keyed context (Nr 0) drops the frame without reaching
rijndael; the hardware receive hooks and keyed software CCMP still work."""
from pathlib import Path
import os
import subprocess
import tempfile

root = (Path(os.environ['SOURCE_ROOT']) if 'SOURCE_ROOT' in os.environ
        else Path(__file__).resolve().parents[5])
source = (root / 'src/libs/compat/freebsd_wlan/net80211/ieee80211_crypto_ccmp.c').read_text()


def extract(begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start) + len(end)]


functions = (extract('struct ccmp_ctx {', '\n};\n')
    + extract('static void\nccmp_encrypt_block(', '\n}\n')
    + extract('static void\nccmp_init_blocks(', '\n}\n')
    + extract('#define\tCCMP_ENCRYPT(', '#undef CCMP_DECRYPT\n'))

fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define __HAIKU__ 1
#define AES_BLOCK_LEN 16
#define IEEE80211_CIPHER_AES_CCM 3
#define IEEE80211_RX_F_DECRYPTED 0x2
#define IEEE80211_RX_F_MIC_STRIP 0x8
typedef uint64_t u_int64_t;
typedef unsigned u_int;
typedef uint32_t uint32;
typedef int32_t int32;
typedef struct { int decrypt; int Nr; uint32_t ek[60]; } rijndael_ctx;
static int aes_calls;
static void
rijndael_encrypt(const rijndael_ctx *ctx, const uint8_t *in, uint8_t *out)
{
	int i;
	assert(ctx->Nr != 0);	/* never with an unkeyed context */
	aes_calls++;
	for (i = 0; i < AES_BLOCK_LEN; i++)
		out[i] = (uint8_t)((in[i] ^ ctx->ek[0]) + i * 37);
}
#if defined(__x86_64__)
static void (*renku_vpaes_ccmp_blocks)(uint8_t *, size_t, const void *,
	uint8_t *, uint8_t *, unsigned);
static void (*renku_vpaes_encrypt)(const uint8_t *, uint8_t *, const void *);
#endif
struct ieee80211_frame { uint8_t i_fc[2], i_dur[2], i_addr1[6], i_addr2[6],
	i_addr3[6], i_seq[2]; };
struct ieee80211_rx_stats { uint32 c_pktflags; };
struct mbuf { uint8_t *m_data; int m_len; struct mbuf *m_next;
	struct { int len; } m_pkthdr; struct ieee80211_rx_stats *rxs; };
struct ieee80211_cipher { int ic_cipher; };
struct ieee80211_key { void *wk_private; const struct ieee80211_cipher *wk_cipher;
	u_int64_t wk_keytsc; };
struct ieee80211com;
struct ieee80211vap { struct { int is_crypto_ccmp, is_rx_ccmpmic; } iv_stats; };
#define mtod(m, t) ((t)(m)->m_data)
#define min(a, b) ((a) < (b) ? (a) : (b))
#define atomic_get(p) (*(p))
#define atomic_set(p, v) (*(p) = (v))
#define KASSERT(e, msg) assert(e)
#define IEEE80211_NOTE_MAC(...) do { } while (0)
#define IEEE80211_ADDR_COPY(d, s) memcpy(d, s, 6)
#define ieee80211_get_rx_params_ptr(m) ((m)->rxs)
#define ccmp_get_header_len(k) 8
#define ccmp_get_trailer_len(k) 8
#define ccmp_get_ccm_m(k) 8
#define ieee80211_crypto_ccmp_init_nonce_flags(wh, b0) ((b0)[1] = 0)
#define m_copydata(m, off, len, cp) memcpy(cp, (m)->m_data + (off), len)
static int
ieee80211_crypto_init_aad(const struct ieee80211_frame *wh, uint8_t *aad, int len)
{
	memset(aad, 0, len);
	memcpy(aad, wh, 16);
	return 0;
}
static void
xor_block(uint8_t *b, const uint8_t *a, size_t len)
{
	while (len-- > 0)
		*b++ ^= *a++;
}
static int
m_append(struct mbuf *m, int len, const uint8_t *cp)
{
	memcpy(m->m_data + m->m_len, cp, len);
	m->m_len += len;
	m->m_pkthdr.len += len;
	return 1;
}
'''
checks = r'''
enum { HDR = 24, IV = 8, DATA = 40, MIC = 8 };
static const struct ieee80211_cipher cipher = { IEEE80211_CIPHER_AES_CCM };
static struct ieee80211vap vap;
static struct ccmp_ctx ctx;
static struct ieee80211_key key = { &ctx, &cipher, 1 };
static uint8_t frame[HDR + IV + DATA + MIC];
static struct mbuf m;

static void
reset(int len, struct ieee80211_rx_stats *rxs)
{
	m.m_data = frame;
	m.m_len = m.m_pkthdr.len = len;
	m.m_next = NULL;
	m.rxs = rxs;
	aes_calls = 0;
}

int
main(void)
{
	struct ieee80211_rx_stats hw = { 0x80000000U | (5U << 24) };
	struct ieee80211_rx_stats decrypted = { IEEE80211_RX_F_DECRYPTED };
	uint8_t plain[DATA];
	int i;

	ctx.cc_vap = &vap;
	ctx.cc_rx_slot = 5;
	for (i = 0; i < DATA; i++)
		plain[i] = frame[HDR + IV + i] = (uint8_t)i;

	/* attached but unkeyed: drop, never touch rijndael */
	reset(HDR + IV + DATA, NULL);
	assert(ccmp_encrypt(&key, &m, HDR) == 0 && aes_calls == 0);
	reset(HDR + IV + DATA + MIC, NULL);
	assert(ccmp_decrypt(&key, 1, &m, HDR) == 0 && aes_calls == 0);
	assert(vap.iv_stats.is_crypto_ccmp == 0);

	/* the hardware receive paths come first and are unaffected */
	reset(HDR + IV + DATA + MIC, &hw);
	assert(ccmp_decrypt(&key, 1, &m, HDR) == 1);
	ctx.cc_rx_slot = 6;
	assert(ccmp_decrypt(&key, 1, &m, HDR) == 0);
	reset(HDR + IV + DATA + MIC, &decrypted);
	assert(ccmp_decrypt(&key, 1, &m, HDR) == 1 && aes_calls == 0);

	/* keyed software CCMP still round-trips and checks the MIC */
	ctx.cc_aes.Nr = 10;
	ctx.cc_aes.ek[0] = 0xa5;
	reset(HDR + IV + DATA, NULL);
	assert(ccmp_encrypt(&key, &m, HDR) == 1 && aes_calls > 0);
	assert(m.m_pkthdr.len == HDR + IV + DATA + MIC);
	assert(memcmp(frame + HDR + IV, plain, DATA) != 0);
	reset(HDR + IV + DATA + MIC, NULL);
	assert(ccmp_decrypt(&key, 1, &m, HDR) == 1 && aes_calls > 0);
	assert(memcmp(frame + HDR + IV, plain, DATA) == 0);
	reset(HDR + IV + DATA + MIC, NULL);
	assert(ccmp_decrypt(&key, 1, &m, HDR) == 0);	/* now plaintext */
	assert(vap.iv_stats.is_rx_ccmpmic == 1);
	puts("PASS unkeyed CCMP context drops frames");
	return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'ccmp_unkeyed.c'
    c.write_text(fixture + functions + checks)
    exe = c.with_suffix('')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Werror',
        '-Wno-unused-function', '-fsanitize=address,undefined', str(c), '-o',
        str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
