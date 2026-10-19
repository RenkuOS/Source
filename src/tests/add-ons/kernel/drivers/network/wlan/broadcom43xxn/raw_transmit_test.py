# SPDX-License-Identifier: MIT
from pathlib import Path
import os,subprocess,tempfile
root=Path(os.environ.get('BRCMS_SOURCE',Path(__file__).resolve().parents[7]/'add-ons/kernel/drivers/network/wlan/broadcom43xxn'))
s=(root/'if_brcms.c').read_text();a=s.index('static int\nbrcms_raw_xmit');b=s.index('\n}\n',a)+3
code='''#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
struct brcms_softc {bool sc_running;};
struct ieee80211com {struct brcms_softc* ic_softc;};
struct ieee80211_node {struct ieee80211com* ni_ic;};
struct ieee80211_bpf_params {int unused;};
struct mbuf {unsigned freed;};
static unsigned submitted,channel=36,sentChannel;
static int result;
static void m_freem(struct mbuf* m){assert(m->freed++==0);}
static int brcms_send_mbuf(struct ieee80211_node* ni,struct mbuf* m,int r,bool ht,bool protect,bool wide){
 assert(r==0 && !ht && !protect && !wide);
 if(result)return result;
 submitted++;sentChannel=channel;m_freem(m);return 0;
}
'''+s[a:b]+'''
int main(void){
 struct brcms_softc sc={true};struct ieee80211com ic={&sc};struct ieee80211_node ni={&ic};
 struct mbuf m={0};assert(brcms_raw_xmit(&ni,&m,NULL)==0);
 channel=40;assert(submitted==1 && sentChannel==36 && m.freed==1);
 m.freed=0;result=ENOBUFS;assert(brcms_raw_xmit(&ni,&m,NULL)==ENOBUFS && m.freed==1 && submitted==1);
 m.freed=0;sc.sc_running=false;assert(brcms_raw_xmit(&ni,&m,NULL)==ENETDOWN && m.freed==1 && submitted==1);
 puts("PASS: management submitted before retune, full/stopped mbuf ownership");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
