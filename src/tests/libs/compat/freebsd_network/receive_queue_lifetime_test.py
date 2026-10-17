#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the production ifnet free path and queue-draining macros."""
from pathlib import Path
import os
import subprocess
import tempfile

root = (Path(os.environ['SOURCE_ROOT']) if 'SOURCE_ROOT' in os.environ
        else Path(__file__).resolve().parents[5])
source = (root / 'src/libs/compat/freebsd_network/if.c').read_text()
header = (root / 'src/libs/compat/freebsd_network/compat/net/if_var.h').read_text()
start = source.index('void\nif_free_inplace(')
function = source[start:source.index('\n}', start) + 2]

def macro(name):
    lines = header.splitlines()
    for i, line in enumerate(lines):
        if line.startswith('#define') and line.split()[1].startswith(name + '('):
            end = i
            while lines[end].endswith('\\'):
                end += 1
            return '\n'.join(lines[i:end + 1])
    raise AssertionError(name)

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
struct mbuf { struct mbuf *m_next, *m_nextpkt; };
struct ifqueue { struct mbuf *ifq_head, *ifq_tail; unsigned ifq_len; bool locked, dead; };
struct ifnet { int if_type, if_index, receive_sem; bool address_dead; struct ifqueue receive_queue; };
#define IFT_ETHER 6
static unsigned live, removed, sem_deleted, queues_destroyed;
static bool index_locked;
static void m_freem(struct mbuf *m) {
    while (m) { struct mbuf *next = m->m_next; assert(!m->m_nextpkt && live); live--; free(m); m = next; }
}
static void lock_queue(struct ifqueue *q) { assert(!q->dead && !q->locked); q->locked = true; }
static void unlock_queue(struct ifqueue *q) { assert(!q->dead && q->locked); q->locked = false; }
#define IF_LOCK(q) lock_queue(q)
#define IF_UNLOCK(q) unlock_queue(q)
#define IFNET_WLOCK() do { assert(!index_locked); index_locked = true; } while (0)
#define IFNET_WUNLOCK() do { assert(index_locked); index_locked = false; } while (0)
#define IF_ADDR_LOCK_DESTROY(p) do { assert(!(p)->address_dead); (p)->address_dead = true; } while (0)
static void remove_from_device_name_list(struct ifnet *p) { removed++; }
static void ifindex_free_locked(int index) { assert(index_locked); }
static void delete_sem(int sem) { sem_deleted++; }
static void ifq_uninit(struct ifqueue *q) { assert(!q->dead && !q->locked); q->dead = true; queues_destroyed++; }
''' + '\n'.join(macro(name) for name in ('_IF_DEQUEUE', '_IF_DRAIN', 'IF_DRAIN')) + '\n' + function + r'''
static void enqueue(struct ifqueue *q, unsigned fragments) {
    struct mbuf *first = NULL;
    for (unsigned i = 0; i < fragments; i++) {
        struct mbuf *m = calloc(1, sizeof(*m)); assert(m); live++;
        m->m_next = first; first = m;
    }
    if (q->ifq_tail) q->ifq_tail->m_nextpkt = first; else q->ifq_head = first;
    q->ifq_tail = first; q->ifq_len++;
}
int main(void) {
    for (unsigned kind = 0; kind < 2; kind++) {
        for (unsigned packets = 0; packets <= 256; packets++) {
            struct ifnet p = {0}; p.if_type = kind ? IFT_ETHER : 71;
            for (unsigned i = 0; i < packets; i++) enqueue(&p.receive_queue, 1 + i % 4);
            unsigned before = removed;
            if_free_inplace(&p);
            assert(live == 0); // A cache containing any mbuf cannot be destroyed.
            assert(!p.receive_queue.ifq_head && !p.receive_queue.ifq_tail && !p.receive_queue.ifq_len);
            assert(p.receive_queue.dead && p.address_dead && !index_locked);
            assert(removed == before + (kind ? 1 : 0));
        }
    }
    assert(sem_deleted == 514 && queues_destroyed == 514);
    puts("PASS: queued packet chains are released before receive-queue teardown");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'test.c'
    path.write_text(code)
    binary = Path(directory) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11',
                    '-fsanitize=address,undefined', str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
