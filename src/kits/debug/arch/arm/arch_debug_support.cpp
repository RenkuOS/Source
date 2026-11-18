/*
 * Copyright 2012, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Ithamar R. Adema <ithamar@upgrade-android.com>
 */


#include <debug_support.h>

#include "arch_debug_support.h"

status_t
arch_debug_get_instruction_pointer(debug_context *context, thread_id thread,
	void **ip, void **stackFrameAddress)
{
	debug_cpu_state cpuState;
	status_t error = debug_get_cpu_state(context, thread, NULL, &cpuState);
	if (error != B_OK)
		return error;

	*ip = (void*)cpuState.r15;
	*stackFrameAddress = (void*)cpuState.r11;
		// the frame pointer in ARM state; arch_debug_get_stack_frame() does
		// not walk the frames yet

	return B_OK;
}


status_t
arch_debug_get_stack_frame(debug_context *context, void *stackFrameAddress,
	debug_stack_frame_info *stackFrameInfo)
{
	return B_ERROR;
}
