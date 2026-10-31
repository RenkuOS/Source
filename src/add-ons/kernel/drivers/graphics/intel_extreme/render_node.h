/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License for the native code.
 * Genode-modeled portions retain the license in COPYING.Genode.
 * Copyright (C) 2017-2024 Genode Labs GmbH (referenced portions).
 *
 * SPDX-License-Identifier: LicenseRef-Genode-AGPL-3.0-with-linking-exception AND MIT
 *
 * Gen9 render node for the intel_extreme driver: publishes a separate
 * graphics/intel_render_9x_* device per Gen9 card with its own device
 * hooks, GEM buffer objects and a private GGTT address region.
 */
#ifndef INTEL_RENDER_NODE_H
#define INTEL_RENDER_NODE_H


#include <Drivers.h>


struct intel_info;


// device hooks for the graphics/intel_render_9x_* nodes (render_node.cpp)
extern device_hooks gRenderDeviceHooks;

// called from the display driver's gen8 interrupt handler when
// MASTER_INT_CTL shows a render-pending interrupt; returns
// B_HANDLED_INTERRUPT/B_INVOKE_SCHEDULER when handled,
// B_UNHANDLED_INTERRUPT otherwise
extern "C" int32 render_node_interrupt(intel_info* info);

// tears down all render nodes (called from uninit_driver())
void render_node_uninit_all(void);


#endif	/* INTEL_RENDER_NODE_H */
