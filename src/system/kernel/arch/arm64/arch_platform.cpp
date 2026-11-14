/*
 * Copyright 2019-2022 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <KernelExport.h>
#include <arch/platform.h>
#include <boot/kernel_args.h>
#include <boot_item.h>

#include <string.h>


void *gFDT = NULL;
phys_addr_t sACPIRootPointer = 0;
static addr_range sCacheableMemoryRanges[MAX_CACHEABLE_MEMORY_RANGES];


status_t
arch_platform_init(struct kernel_args *kernelArgs)
{
	gFDT = kernelArgs->arch_args.fdt;
	return B_OK;
}


status_t
arch_platform_init_post_vm(struct kernel_args *kernelArgs)
{
	if (kernelArgs->arch_args.acpi_root) {
		sACPIRootPointer = kernelArgs->arch_args.acpi_root.Get();
		add_boot_item("ACPI_ROOT_POINTER", &sACPIRootPointer, sizeof(sACPIRootPointer));
	}

	// The ACPI module needs to know which physical memory it may map
	// cacheable (see AcpiOsMapMemory()).
	uint32 count = min_c(kernelArgs->arch_args.num_cacheable_memory_ranges,
		(uint32)MAX_CACHEABLE_MEMORY_RANGES);
	if (count > 0) {
		memcpy(sCacheableMemoryRanges,
			kernelArgs->arch_args.cacheable_memory_range,
			count * sizeof(addr_range));
		add_boot_item("CACHEABLE_MEMORY_RANGES", sCacheableMemoryRanges,
			count * sizeof(addr_range));
	}

	return B_OK;
}


status_t
arch_platform_init_post_thread(struct kernel_args *kernelArgs)
{
	return B_OK;
}
