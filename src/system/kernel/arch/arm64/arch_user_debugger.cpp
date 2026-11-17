/*
 * Copyright 2019-2026 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include <string.h>

#include <debugger.h>
#include <interrupts.h>
#include <thread.h>
#include <arch/user_debugger.h>

#include "arm_registers.h"


void
arch_clear_team_debug_info(struct arch_team_debug_info *info)
{
}


void
arch_destroy_team_debug_info(struct arch_team_debug_info *info)
{
}


void
arch_clear_thread_debug_info(struct arch_thread_debug_info *info)
{
}


void
arch_destroy_thread_debug_info(struct arch_thread_debug_info *info)
{
}


static void
get_cpu_state(iframe* frame, debug_cpu_state* cpuState)
{
	// iframe stores x0-x28 in x[], with x29 (the frame pointer) separate
	memcpy(cpuState->x, frame->x, sizeof(frame->x));
	cpuState->x[29] = frame->fp;
	cpuState->lr = frame->lr;
	cpuState->sp = frame->sp;
	cpuState->elr = frame->elr;
	cpuState->spsr = frame->spsr;
}


static iframe*
arm64_get_user_iframe(Thread* thread)
{
	iframe_stack* iframes = &thread->arch_info.iframes;

	// walk outwards from the innermost frame, skipping any kernel frames
	for (int32 i = iframes->index - 1; i >= 0; i--) {
		iframe* frame = iframes->frames[i];
		if ((frame->spsr & PSR_M_MASK) == PSR_M_EL0t)
			return frame;
	}

	return NULL;
}


void
arch_update_thread_single_step()
{
	Thread* thread = thread_get_current_thread();
	if (iframe* frame = arm64_get_user_iframe(thread)) {
		// SPSR.SS=1 lets exactly one instruction run after the eret; the
		// matching MDSCR_EL1.SS is set on kernel exit (see InterruptScope)
		if ((thread->debug_info.flags & B_THREAD_DEBUG_SINGLE_STEP) != 0)
			frame->spsr |= PSR_SS;
		else
			frame->spsr &= ~(uint64)PSR_SS;
	}
}


void
arch_set_debug_cpu_state(const debug_cpu_state *cpuState)
{
	iframe* frame = arm64_get_user_iframe(thread_get_current_thread());
	if (frame == NULL)
		return;

	memcpy(frame->x, cpuState->x, sizeof(frame->x));
	frame->fp = cpuState->x[29];
	frame->lr = cpuState->lr;
	frame->sp = cpuState->sp;
	frame->elr = cpuState->elr;
	// only the condition flags; mode, mask and step bits stay the kernel's
	frame->spsr = (frame->spsr & ~(uint64)PSR_FLAGS)
		| (cpuState->spsr & PSR_FLAGS);
}


void
arch_get_debug_cpu_state(debug_cpu_state *cpuState)
{
	if (iframe* frame = arm64_get_user_iframe(thread_get_current_thread()))
		get_cpu_state(frame, cpuState);
}


status_t
arch_get_thread_debug_cpu_state(Thread *thread, debug_cpu_state *cpuState)
{
	iframe* frame = arm64_get_user_iframe(thread);
	if (frame == NULL)
		return B_BAD_VALUE;

	get_cpu_state(frame, cpuState);
	return B_OK;
}


status_t
arch_set_breakpoint(void *address)
{
	return B_ERROR;
}


status_t
arch_clear_breakpoint(void *address)
{
	return B_ERROR;
}


status_t
arch_set_watchpoint(void *address, uint32 type, int32 length)
{
	return B_ERROR;
}


status_t
arch_clear_watchpoint(void *address)
{
	return B_ERROR;
}


bool
arch_has_breakpoints(struct arch_team_debug_info *info)
{
	return false;
}
