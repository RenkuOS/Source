/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <OS.h>

#include <arch_cpu.h>
#include <arch/system_info.h>
#include <boot/kernel_args.h>
#include <cpu.h>


static enum cpu_vendor
arm64_cpu_vendor(uint32 midr)
{
	switch (CPU_IMPL(midr)) {
		case CPU_IMPL_ARM:
			return B_CPU_VENDOR_ARM;
		case CPU_IMPL_APPLE:
			return B_CPU_VENDOR_APPLE;
		case CPU_IMPL_QUALCOMM:
			return B_CPU_VENDOR_QUALCOMM;
		default:
			return B_CPU_VENDOR_UNKNOWN;
	}
}


void
arch_fill_topology_node(cpu_topology_node_info* node, int32 cpu)
{
	switch (node->type) {
		case B_TOPOLOGY_ROOT:
			node->data.root.platform = B_CPU_ARM_64;
			break;
		case B_TOPOLOGY_PACKAGE:
			node->data.package.vendor = arm64_cpu_vendor(gCPU[cpu].arch.midr);
			node->data.package.cache_line_size = CACHE_LINE_SIZE;
			node->data.package.isa_level = gCPU[cpu].arch.isa_level;
			break;
		case B_TOPOLOGY_CORE:
			// MIDR_EL1 (implementer, variant, part, revision), decoded by
			// cpu_type.h. No architectural register gives the core clock.
			node->data.core.model = gCPU[cpu].arch.midr;
			node->data.core.default_frequency = 0;
			break;
		default:
			break;
	}
}


status_t
arch_system_info_init(struct kernel_args *args)
{
	return B_OK;
}


status_t
arch_get_frequency(uint64 *frequency, int32 cpu)
{
	*frequency = 0;
	return B_OK;
}
