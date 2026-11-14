#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run taskqueue_thread_enqueue() from taskqueue.c: the worker is woken
with rescheduling from thread context, and without it from interrupt
context (interrupts disabled)."""
from pathlib import Path
import os
import subprocess
import tempfile

root = (Path(os.environ['SOURCE_ROOT']) if 'SOURCE_ROOT' in os.environ
        else Path(__file__).resolve().parents[5])
source = (root / 'src/libs/compat/freebsd_network/taskqueue.c').read_text()
start = source.index('void\ntaskqueue_thread_enqueue(')
function = source[start:source.index('\n}\n', start) + 3]

fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define B_DO_NOT_RESCHEDULE 0x2
typedef int32_t sem_id;
struct taskqueue { sem_id tq_sem; };
static bool interrupts;
static int calls, last_count;
static uint32_t last_flags;
static sem_id last_sem;
static bool __attribute__((unused))
are_interrupts_enabled(void) { return interrupts; }
static int
release_sem_etc(sem_id sem, int count, uint32_t flags)
{
	calls++;
	last_sem = sem;
	last_count = count;
	last_flags = flags;
	return 0;
}
'''
checks = r'''
int
main(void)
{
	struct taskqueue queue = { 42 }, *tq = &queue;

	interrupts = true;	/* thread context */
	taskqueue_thread_enqueue(&tq);
	assert(calls == 1 && last_sem == 42 && last_count == 1);
	assert((last_flags & B_DO_NOT_RESCHEDULE) == 0);

	interrupts = false;	/* interrupt handler */
	taskqueue_thread_enqueue(&tq);
	assert(calls == 2 && last_sem == 42 && last_count == 1);
	assert((last_flags & B_DO_NOT_RESCHEDULE) != 0);

	puts("PASS taskqueue wakeups reschedule only outside interrupt context");
	return 0;
}
'''

with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'taskqueue_wakeup.c'
    c.write_text(fixture + function + checks)
    exe = c.with_suffix('')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Werror',
        '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
