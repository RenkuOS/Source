# SPDX-License-Identifier: MIT
"""Exercise the actual VAP sender with a consuming parent transmitter."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[6]
source = (root / 'src/libs/compat/freebsd_wlan/net80211/ieee80211_output.c').read_text()
a = source.index('ieee80211_vap_pkt_send_dest(')
sender = source[a:source.index('\n}', a) + 2]
fixture = r'''
#include <cassert>
#include <cstdio>
constexpr int ENOBUFS=-105, ENETDOWN=-100;
constexpr int IEEE80211_NODE_PWR_MGT=1, M_PWR_SAV=2, M_MCAST=4, M_BCAST=8;
constexpr int IEEE80211_NODE_AMPDU_TX=16, IEEE80211_FHT_AMPDU_TX=32;
constexpr int M_EAPOL=64, M_AMPDU_MPDU=128, IEEE80211_C_8023ENCAP=256;
constexpr int IFCOUNTER_OERRORS=0;
struct ifnet { unsigned errors=0; };
struct ieee80211com { bool locked=false; int ic_lastdata=0;
 int (*ic_ampdu_enable)(void*,void*)=nullptr; };
struct ieee80211vap { ieee80211com* iv_ic; ifnet* iv_ifp;
 unsigned iv_flags_ht=0,iv_caps=0; struct {unsigned is_tx_classify=0;} iv_stats; };
struct ieee80211_tx_ampdu {};
struct ieee80211_node { unsigned ni_flags=0; ieee80211_tx_ampdu ni_tx_ampdu[16]; };
struct mbuf {unsigned m_flags=0;struct {void* rcvif=nullptr;} m_pkthdr;};
int ticks=123, parentStatus=0; bool encapFails=false,classifyFails=false;
unsigned packets=0,nodes=0,parents=0;
#define IEEE80211_DISCARD_MAC(...)
#define BPF_MTAP(...)
#define IEEE80211_CONF_AMPDU_OFFLOAD(ic) true
#define WME_AC_TO_TID(ac) 0
#define M_WME_GETAC(m) 0
#define IEEE80211_AMPDU_RUNNING(tap) false
#define IEEE80211_AMPDU_REQUESTED(tap) false
#define IEEE80211_TX_LOCK(ic) do { assert(!(ic)->locked);(ic)->locked=true; } while(0)
#define IEEE80211_TX_UNLOCK(ic) do {assert((ic)->locked);(ic)->locked=false;} while(0)
#define __predict_true(x) (x)
void if_inc_counter(ifnet* ifp,int,unsigned n){ifp->errors+=n;}
void m_freem(mbuf* m){assert(packets==1);packets--;delete m;}
void ieee80211_free_node(ieee80211_node* n){assert(nodes==1);nodes--;delete n;}
int ieee80211_pwrsave(ieee80211_node*,mbuf* m){m_freem(m);return 0;}
int ieee80211_classify(ieee80211_node*,mbuf*){return classifyFails;}
void ieee80211_txampdu_count_packet(ieee80211_tx_ampdu*){}
void ieee80211_ampdu_request(ieee80211_node*,ieee80211_tx_ampdu*){}
mbuf* ieee80211_encap(ieee80211vap*,ieee80211_node*,mbuf* m){
 if(encapFails){m_freem(m);return nullptr;}return m;
}
int ieee80211_parent_xmitpkt(ieee80211com* ic,mbuf* m){
 assert(ic->locked);parents++;auto* n=(ieee80211_node*)m->m_pkthdr.rcvif;
 // The parent consumes the packet and node on both success and failure.
 m_freem(m);ieee80211_free_node(n);return parentStatus;
}
'''
checks = r'''
int main(){
 ieee80211com ic; ifnet ifp; ieee80211vap vap{&ic,&ifp};
 for(unsigned caps : {0u,(unsigned)IEEE80211_C_8023ENCAP}){
  vap.iv_caps=caps;
  for(int error : {0,ENOBUFS,ENETDOWN,-1}){
   parentStatus=error;auto* m=new mbuf;auto* n=new ieee80211_node;
   packets=nodes=1;unsigned before=parents;
   assert(ieee80211_vap_pkt_send_dest(&vap,m,n)==error);
   assert(!ic.locked&&packets==0&&nodes==0&&parents==before+1);
   assert(ic.ic_lastdata==ticks);
  }
 }
 // Encapsulation failure retains its old cleanup and error contract.
 vap.iv_caps=0;encapFails=true;packets=nodes=1;unsigned before=parents;
 assert(ieee80211_vap_pkt_send_dest(&vap,new mbuf,new ieee80211_node)==ENOBUFS);
 assert(!ic.locked&&packets==0&&nodes==0&&parents==before);
 encapFails=false;classifyFails=true;packets=nodes=1;
 assert(ieee80211_vap_pkt_send_dest(&vap,new mbuf,new ieee80211_node)==0);
 assert(!ic.locked&&packets==0&&nodes==0&&parents==before);
 classifyFails=false;packets=nodes=1;auto* sleeping=new ieee80211_node;
 sleeping->ni_flags=IEEE80211_NODE_PWR_MGT;
 assert(ieee80211_vap_pkt_send_dest(&vap,new mbuf,sleeping)==0);
 assert(!ic.locked&&packets==0&&nodes==0&&parents==before);
 puts("PASS actual VAP sender: parent status, both encapsulation modes, consumed ownership, unlock, early failures");
}
'''
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory) / 'wlan_tx_status.cpp'
    p.write_text('#include <initializer_list>\n' + fixture + '\nint\n' + sender + checks)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
        '-fsanitize=address,undefined', str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)
