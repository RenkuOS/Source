# SPDX-License-Identifier: MIT
from pathlib import Path
import subprocess
import tempfile
import os

root = Path(os.environ.get('BRCMS_SOURCE', Path(__file__).resolve().parents[7] / 'add-ons/kernel/drivers/network/wlan/broadcom43xxn'))
source = (root / 'if_brcms.c').read_text()
def function(name):
    start = source.index('\n' + name + '(')
    start = source.rfind('\nstatic ', 0, start) + 1
    end = source.index('\n}\n', start) + 3
    return source[start:end]

prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "if_brcms_tx_queue.h"
#define BRCMS_IN_FLIGHT 64
struct ieee80211_node { int freed; };
struct mbuf { int freed; int fail; struct ieee80211_node* node; };
struct mtx { bool locked; };
struct task { int unused; };
struct brcms_softc;
struct ieee80211com { struct brcms_softc* sc; };
struct brcms_softc {
    struct mtx sc_txLock;
    bool sc_running;
    struct brcms_tx_queue sc_txPending;
    unsigned sc_txQueueFull, sc_queued, sc_completed;
    struct task sc_txTask;
    struct ieee80211com sc_ic;
};
static void* taskqueue_thread;
static unsigned wakeups;
static struct brcms_softc* active;
static struct ieee80211_node* sentNodes[1024];
static unsigned sentCount;
static void mtx_lock(struct mtx* m) { assert(!m->locked); m->locked=true; }
static void mtx_unlock(struct mtx* m) { assert(m->locked); m->locked=false; }
static void taskqueue_enqueue(void* q, struct task* t) { wakeups++; }
static void m_freem(struct mbuf* m) { assert(m->freed++ == 0); }
static void ieee80211_free_node(struct ieee80211_node* n) {
    assert(n->freed++ == 0);
}
static int brcms_send_mbuf(struct ieee80211_node* n, struct mbuf* m,
    int rate, bool mcs, bool protect, bool wide) {
    if (m->fail) return EIO;
    assert(active->sc_queued - active->sc_completed < BRCMS_IN_FLIGHT-1);
    active->sc_queued++;
    sentNodes[sentCount++] = n;
    m_freem(m);
    return 0;
}
static int brcms_transmit_now(struct ieee80211com* ic, struct mbuf* m) {
    return brcms_send_mbuf(m->node,m,7,true,false,false);
}
static void complete_all(void) {
    for(unsigned i=0;i<sentCount;i++) ieee80211_free_node(sentNodes[i]);
    sentCount=0;
    active->sc_completed=active->sc_queued;
}
'''
main = r'''
int main(void) {
    struct brcms_softc sc={0};
    struct mbuf packets[300]={0};
    struct ieee80211_node nodes[300]={0};
    active=&sc;
    sc.sc_ic.sc=&sc;
    sc.sc_running=true;
    sc.sc_queued=60;
    for(unsigned i=0;i<100;i++) {
        packets[i].node=&nodes[i];
        assert(brcms_queue_frame(&sc,&nodes[i],&packets[i],i%2)==0);
    }
    brcms_tx_task(&sc,0);
    assert(sc.sc_queued==63 && sc.sc_txPending.count==97);
    assert(sentCount==3);
    // Completion wakes the worker and lets it use the recovered space.
    complete_all();
    packets[5].fail=1;
    while(sc.sc_txPending.count) {
        brcms_tx_task(&sc,0);
        complete_all();
    }
    for(unsigned i=0;i<100;i++) {
        assert(packets[i].freed==1 && nodes[i].freed==1);
    }
    assert(sc.sc_queued==159); // 60 initial + 99 accepted by hardware
    // Full software queue rejects ownership, with no silent free.
    memset(packets,0,sizeof(packets));
    memset(nodes,0,sizeof(nodes));
    for(unsigned i=0;i<BRCMS_TX_PENDING;i++) {
        packets[i].node=&nodes[i];
        assert(brcms_queue_frame(&sc,&nodes[i],&packets[i],false)==0);
    }
    assert(brcms_queue_frame(&sc,&nodes[256],&packets[256],false)==ENOBUFS);
    assert(!packets[256].freed && !nodes[256].freed);
    sc.sc_running=false;
    brcms_tx_task(&sc,0);
    assert(sc.sc_txPending.count==BRCMS_TX_PENDING);
    brcms_discard_pending(&sc);
    assert(sc.sc_txPending.count==0);
    for(unsigned i=0;i<BRCMS_TX_PENDING;i++)
        assert(packets[i].freed==1 && nodes[i].freed==1);
    assert(brcms_queue_frame(&sc,&nodes[256],&packets[256],true)==ENETDOWN);
    assert(!packets[256].freed && !nodes[256].freed);
    // Exercise wrapped head/tail and FIFO order from the production header.
    struct brcms_tx_queue queue={0};
    for(unsigned round=0;round<10000;round++) {
        for(unsigned i=0;i<173;i++) {
            struct brcms_pending_tx entry={&packets[i],&nodes[i],i%2};
            assert(brcms_tx_enqueue(&queue,entry));
        }
        for(unsigned i=0;i<173;i++) {
            struct brcms_pending_tx entry;
            assert(brcms_tx_dequeue(&queue,&entry));
            assert(entry.packet==&packets[i] && entry.node==&nodes[i]);
            assert(entry.raw==(bool)(i%2));
        }
    }
    assert(wakeups==356 && !sc.sc_txLock.locked);
    puts("PASS: DMA backpressure, wakeup, failure ownership, stop, full queue, FIFO wrap");
}
'''
with tempfile.TemporaryDirectory(prefix='brcms-tx-test-') as directory:
    path=Path(directory)/'test.c'
    path.write_text(prefix + '\n'.join(function(n) for n in
        ['brcms_queue_frame','brcms_discard_pending','brcms_tx_task']) + main)
    subprocess.run([os.environ.get('CC', 'cc'),'-std=c11','-I',str(root),'-fsanitize=address,undefined',
        '-g',str(path),'-o',str(path.with_suffix(''))],check=True)
    subprocess.run([str(path.with_suffix(''))],check=True)
