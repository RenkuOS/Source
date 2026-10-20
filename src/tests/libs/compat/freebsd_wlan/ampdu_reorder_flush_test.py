#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run ieee80211_ampdu_reorder() and ampdu_rx_flush() from ieee80211_ht.c:
when the reorder queue ages out, held frames above the current one are not
released before it (frames go up in ascending order), held subframes of the
current frame's A-MSDU are not released ahead of it, and a flush moves the BA
window past the frames it dispatched."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = (Path(os.environ['SOURCE_ROOT']) if 'SOURCE_ROOT' in os.environ
        else Path(__file__).resolve().parents[5])
source = (root / 'src/libs/compat/freebsd_wlan/net80211/ieee80211_ht.c').read_text()


def extract(signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


functions = ''.join(extract(s) for s in (
    'static int\nampdu_dispatch_slot(', 'static void\nampdu_rx_moveup(',
    'static void\nampdu_rx_dispatch(', 'static void\nampdu_rx_flush(',
    'static void\nampdu_rx_flush_upto(', 'int\nieee80211_ampdu_reorder('))

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define __HAIKU__ 1
#define KASSERT(c, msg) assert(c)
#define IEEE80211_NOTE(...) do { } while (0)
#define IEEE80211_DISCARD_MAC(...) do { } while (0)
#define IEEE80211_NODE_STAT(ni, s) do { } while (0)
#define IEEE80211_SEQ_RANGE 4096
#define IEEE80211_SEQ_ADD(seq, incr) (((seq) + (incr)) & (IEEE80211_SEQ_RANGE-1))
#define IEEE80211_SEQ_INC(seq) IEEE80211_SEQ_ADD(seq,1)
#define IEEE80211_SEQ_SUB(a, b) (((a) + IEEE80211_SEQ_RANGE - (b)) & (IEEE80211_SEQ_RANGE-1))
#define IEEE80211_SEQ_BA_RANGE 2048
#define IEEE80211_SEQ_BA_BEFORE(a, b) (IEEE80211_SEQ_SUB(b, a+1) < IEEE80211_SEQ_BA_RANGE-1)
enum { IEEE80211_SEQ_FRAG_MASK = 0xf, IEEE80211_SEQ_SEQ_SHIFT = 4,
	IEEE80211_QOS_TID = 0xf, IEEE80211_NODE_HT = 1, M_AMPDU = 1, M_AMPDU_MPDU = 2,
	IEEE80211_AGGR_XCHGPEND = 1, IEEE80211_AGGR_WAITRX = 2 };
#define IEEE80211_IS_QOSDATA(wh) 1
#define IEEE80211_IS_MULTICAST(a) 0
static int cur_amsdu, cur_amsdu_end;	/* of the frame being received */
#define ieee80211_check_rxseq_amsdu(rxs) cur_amsdu
#define ieee80211_check_rxseq_amsdu_more(rxs) cur_amsdu_end
#define ieee80211_getqos(wh) ((wh)->i_qos)
#define le16toh(x) (x)
#define mtod(m, t) ((t)&(m)->m_wh)
typedef uint16_t ieee80211_seq;
struct ieee80211_rx_stats;
struct ieee80211_qosframe { uint16_t i_seq[1]; uint8_t i_qos[2]; };
struct mbuf { int m_flags; struct { int len; } m_pkthdr; struct ieee80211_qosframe m_wh; };
struct mbufq { struct mbuf *m[4]; int n; };
#define mbufq_len(q) ((q)->n)
#define mbufq_empty(q) ((q)->n == 0)
static struct mbuf *
mbufq_dequeue(struct mbufq *q)
{
	struct mbuf *m = q->n ? q->m[0] : NULL;
	if (q->n)
		memmove(q->m, q->m + 1, --q->n * sizeof(q->m[0]));
	return m;
}
#define mbufq_enqueue(q, x) (assert((q)->n < 4), (q)->m[(q)->n++] = (x))
#define mbufq_concat(d, s) do { while (!mbufq_empty(s)) \
	mbufq_enqueue(d, mbufq_dequeue(s)); } while (0)
struct ieee80211_rx_ampdu { int rxa_flags, rxa_qbytes, rxa_qframes, rxa_wnd;
	int rxa_age, rxa_nframes; ieee80211_seq rxa_start; struct mbufq rxa_mq[64]; };
struct ieee80211vap { struct { int is_ampdu_rx_drop, is_ampdu_rx_oor,
	is_ampdu_rx_copy, is_ampdu_rx_age, is_ampdu_rx_move; } iv_stats; };
struct ieee80211_node { struct ieee80211vap *ni_vap; int ni_flags;
	struct ieee80211_rx_ampdu ni_rx_ampdu[16]; };
static int ticks, ieee80211_ampdu_age = 500;
static char got[64];	/* sequence numbers in the order they went up */
#define m_freem(m) assert(0)
static void
ampdu_dispatch(struct ieee80211_node *ni, struct mbuf *m)
{
	m->m_flags |= M_AMPDU_MPDU;
	snprintf(got + strlen(got), sizeof(got) - strlen(got), " %d",
	    m->m_wh.i_seq[0] >> IEEE80211_SEQ_SEQ_SHIFT);
}
static int
ampdu_rx_add_slot(struct ieee80211_rx_ampdu *rap, int off, int tid,
    ieee80211_seq rxseq, struct ieee80211_node *ni, struct mbuf *m,
    const struct ieee80211_rx_stats *rxs)
{
	mbufq_enqueue(&rap->rxa_mq[off], m);
	rap->rxa_qframes++; rap->rxa_qbytes += m->m_pkthdr.len;
	return 1;
}
'''
checks = r'''
static struct ieee80211vap vap;
static struct ieee80211_node ni = { &vap, IEEE80211_NODE_HT };
static struct mbuf frames[16];
static int nframes;
static void
rx(int seq)
{
	struct mbuf *m = &frames[nframes++ % 16];
	m->m_flags = M_AMPDU;
	m->m_pkthdr.len = 100;
	m->m_wh.i_seq[0] = seq << IEEE80211_SEQ_SEQ_SHIFT;
	if (ieee80211_ampdu_reorder(&ni, m, NULL) == 0)
		ampdu_dispatch(&ni, m);		/* caller processes it */
}
#define expect(want) (printf("released:%s\n", got), assert(strcmp(got, want) == 0))
int
main(int argc, char **argv)
{
	struct ieee80211_rx_ampdu *rap = &ni.ni_rx_ampdu[0];

	if (argv[1][0] == 'r') {
		/* 3 and 5 held behind a hole at 0; the queue ages out at 2 */
		*rap = (struct ieee80211_rx_ampdu){ IEEE80211_AGGR_XCHGPEND, .rxa_wnd = 64 };
		rx(3); rx(5);
		ticks = 1000;
		rx(2);
		expect(" 2 3");	/* 5 must not go up before 2 */
		assert(rap->rxa_start == 4 && rap->rxa_qframes == 1);
		rx(4);
		expect(" 2 3 4 5");
		assert(rap->rxa_start == 6 && rap->rxa_qframes == 0);
		puts("PASS aged-out reorder releases frames in order");
	} else if (argv[1][0] == 'a') {
		/* the first subframe of A-MSDU 2 and frame 3 are held */
		*rap = (struct ieee80211_rx_ampdu){ IEEE80211_AGGR_XCHGPEND, .rxa_wnd = 64 };
		cur_amsdu = 1; rx(2);
		cur_amsdu = 0; rx(3);
		ticks = 1000;
		cur_amsdu = cur_amsdu_end = 1; rx(2);	/* its last subframe */
		cur_amsdu = cur_amsdu_end = 0;
		expect("");			/* held with the first one */
		assert(rap->rxa_qframes == 3);
		rx(4);
		expect(" 2 2 3 4");
		puts("PASS aged-out reorder keeps A-MSDU subframes together");
	} else {
		/* a flush moves the window past the last frame it dispatched */
		*rap = (struct ieee80211_rx_ampdu){ IEEE80211_AGGR_XCHGPEND,
		    .rxa_wnd = 64, .rxa_start = 10 };
		rx(11); rx(14);
		ampdu_rx_flush(&ni, rap);
		expect(" 11 14");
		printf("rxa_start after flush: %d\n", rap->rxa_start);
		assert(rap->rxa_start == 15 && rap->rxa_qframes == 0);
		rx(15);				/* in order now, not held */
		expect(" 11 14 15");
		puts("PASS flush advances the BA window");
	}
	return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'ampdu_reorder_flush.c'
    c.write_text(fixture + functions + checks)
    exe = c.with_suffix('')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Werror',
        '-Wno-unused-function', '-fsanitize=address,undefined', str(c), '-o',
        str(exe)], check=True)
    failed = [case for case in ('reorder', 'amsdu', 'flush')
        if subprocess.run([str(exe), case]).returncode != 0]
    if failed:
        sys.exit('FAIL ' + ' '.join(failed))
