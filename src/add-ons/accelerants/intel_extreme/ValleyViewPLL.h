/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef VALLEYVIEW_PLL_H
#define VALLEYVIEW_PLL_H


#include "intel_extreme.h"


// The HDMI TMDS clock limit of Valleyview, in kHz (Linux i915 for display
// version 7).
#define VLV_HDMI_MAX_PIXEL_CLOCK	225000


status_t vlv_set_hdmi_clock(pipe_index pipe, port_index port,
	addr_t portRegister, uint32 pixelClock);


#endif /* VALLEYVIEW_PLL_H */
