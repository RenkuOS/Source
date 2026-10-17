# SPDX-License-Identifier: MIT
"""Run the production ifnet allocation/attach/detach/free lifecycle."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(os.environ.get('IFNET_SOURCE') or Path(__file__).resolve().parents[4] / 'libs/compat/freebsd_network')
source = (root / 'if.c').read_text()
def function(name):
    start = source.index('\n' + name + '(')
    start = source.rfind('\n', 0, start - 1)
    return source[start:source.index('\n}\n', start) + 3]

prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef char* caddr_t; typedef unsigned short u_short; typedef unsigned char u_char;
#define B_OK 0
#define ENOERR 0
#define IFT_ETHER 1
#define AF_LINK 2
#define IFNAMSIZ 16
#define M_WAITOK 0
#define IFNET_HOLD ((struct ifnet*)1)
#define IFNET_WLOCK() ((void)0)
#define IFNET_WUNLOCK() ((void)0)
#define TAILQ_INIT(x) ((void)0)
#define HAIKU_DRIVER_REQUIRES(x) 1
#define FBSD_SWI_TASKQUEUE 1
#define dprintf(...) ((void)0)
#define bcopy(src,dst,n) memcpy(dst,src,n)
#define roundup2(n,a) (((n)+(a)-1)&~((a)-1))
struct ifqueue { int initialized; };
struct sockaddr { int unused; };
struct sockaddr_dl { unsigned sdl_family,sdl_len,sdl_nlen,sdl_index,sdl_type; char sdl_data[32]; };
struct ifnet;
struct ifaddr { struct ifnet* ifa_ifp; struct sockaddr *ifa_addr,*ifa_netmask; };
struct ifnet { int receive_sem,link_state_sem,open_count,flags,if_type,if_addr_mtx;
 unsigned if_index,if_addrlen; char if_xname[IFNAMSIZ];
 struct ifqueue receive_queue,if_snd; struct sockaddr_dl if_lladdr;
 void *if_transmit,*if_qflush,*if_input,*if_requestencap;
 struct ifaddr* if_addr; int if_linktask;
};
static const char* gDriverName="test";
static void *if_transmit_default,*if_qflush,*if_input_default,*if_requestencap_default,*taskqueue_swi;
static unsigned initialized,destroyed,deleted;
static void lock_init(int* p) { initialized++; *p=1; }
static void lock_destroy(int* p) { assert(*p==1); *p=-32768; destroyed++; }
#define IF_ADDR_LOCK_INIT(p) lock_init(&(p)->if_addr_mtx)
#define IF_ADDR_LOCK_DESTROY(p) lock_destroy(&(p)->if_addr_mtx)
static int create_sem(int n,const char* name) { return 3; }
static void delete_sem(int id) { assert(id==3); deleted++; }
static void ifq_init(struct ifqueue* q,const char* name) { assert(q->initialized==0); q->initialized=1; }
static void ifq_uninit(struct ifqueue* q) { assert(q->initialized==1); q->initialized=0; }
static int ifindex_alloc_locked(u_short* p) { *p=1; return 0; }
static void ifnet_setbyindex_locked(unsigned i,struct ifnet* p) {}
static void ifnet_setbyindex(unsigned i,struct ifnet* p) {}
static void ifindex_free_locked(unsigned i) { assert(i==1); }
static void remove_from_device_name_list(struct ifnet* p) {}
static void panic(const char* p) { abort(); }
static void taskqueue_drain(void* q,int* t) {}
static struct ifaddr* ifa_alloc(unsigned size,int flags) { return calloc(1,size); }
'''
main = r'''
int main(void) {
 for(unsigned n=0;n<1000;n++) {
  struct ifnet p={0}; p.if_addrlen=6; strcpy(p.if_xname,"test0");
  assert(if_alloc_inplace(&p,IFT_ETHER)==0);
  assert(p.if_addr_mtx==1);
  if_attach(&p);
  if_detach(&p);
  // An allocated interface retains its address lock until final free.
  assert(p.if_addr_mtx==1);
  free(p.if_addr);p.if_addr=NULL;
  if_attach(&p);if_detach(&p);free(p.if_addr);
  if_free_inplace(&p);
  assert(p.if_addr_mtx==-32768);
 }
 struct ifnet unattached={0};
 assert(if_alloc_inplace(&unattached,IFT_ETHER)==0);
 if_free_inplace(&unattached);
 assert(initialized==1001 && destroyed==1001 && deleted==1001);
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory)/'test.c'
    p.write_text(prefix+'\n'.join(function(n) for n in ['if_alloc_inplace','if_attach','if_detach','if_free_inplace'])+main)
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-g',str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
print('PASS: one address-lock init/destroy per allocated ifnet, attach/detach/reattach and unattached free')
