# SPDX-License-Identifier: MIT
from pathlib import Path
import os,subprocess,tempfile
root = Path(os.environ.get('BRCMS_SOURCE', Path(__file__).resolve().parents[7] / 'add-ons/kernel/drivers/network/wlan/broadcom43xxn'))
def function(path,needle):
 s=path.read_text();a=s.index(needle);b=s.index('\n}\n',a)+3;return s[a:b]
init=function(root/'brcms/phy/phy_cmn.c','void wlc_phy_stf_chain_init(')
war=function(root/'brcms/phy/phy_n.c','static void wlc_phy_war_txchain_upd_nphy(')
s=(root/'brcms/glue_attach.c').read_text();a=s.index('wlc_phy_stf_chain_init(sHardware.band->pi,');b=s.index(';',a)+1;call=s[a:b]
code='''#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef uint8_t u8;
struct brcms_phy_pub {u8 phy_corenum;};
struct shared_phy {u8 hw_phytxchain,hw_phyrxchain,phytxchain,phyrxchain;};
struct brcms_phy {struct brcms_phy_pub pubpi_ro,pubpi;struct shared_phy* sh;};
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define hweight8(x) __builtin_popcount(x)
#define PHY_CORE_0 0
#define PHY_CORE_1 1
static unsigned forced;
static void wlc_phy_war_force_trsw_to_R_cliplo_nphy(struct brcms_phy* p,unsigned core){forced|=1U<<core;}
struct band {struct brcms_phy_pub* pi;};
static struct {struct band* band;} sHardware;
static struct {u8 hw_txchain,hw_rxchain;} sSingleStreamFilter={3,3};
'''+init+war+'''
int main(void){
 struct shared_phy shared={0};struct brcms_phy phy={0};phy.sh=&shared;
 struct band band={&phy.pubpi_ro};sHardware.band=&band;
 wlc_phy_war_txchain_upd_nphy(&phy,shared.hw_phytxchain);assert(forced==3);
 '''+call+'''
 assert(shared.hw_phytxchain==3 && shared.phytxchain==3);
 assert(shared.hw_phyrxchain==3 && shared.phyrxchain==3 && phy.pubpi.phy_corenum==2);
 forced=0;wlc_phy_war_txchain_upd_nphy(&phy,shared.hw_phytxchain);assert(forced==0);
 puts("PASS: attach initializes PHY masks before zero-mask workaround disables both TX chains");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
