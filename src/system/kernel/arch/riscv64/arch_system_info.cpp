/*
 * Copyright 2007, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 * 		François Revol <revol@free.fr>
 *
 * Copyright 2006, Ingo Weinhold <bonefish@cs.tu-berlin.de>.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include <KernelExport.h>
#include <OS.h>

#include <arch_cpu.h>
#include <arch/system_info.h>
#include <boot/kernel_args.h>
#include <platform/sbi/sbi_syscalls.h>


extern uint32 gPlatform;


static uint64 sCPUClockFrequency;
static uint64 sBusClockFrequency;
static uint16 sCPURevision;
static enum cpu_vendor sCPUVendor = B_CPU_VENDOR_UNKNOWN;
static uint32 sCPUModel;
static uint64 sTimerFrequency;
static bigtime_t sLastMeasurement;


/*!	The CPU clock, measured: the cycle counter against the timer, which runs
	at the fixed "timebase-frequency" of the device tree, for about 100 us.
	All harts share the clock, so the calling one measures for all of them.
	The result is kept for 100 ms, so that frequent queries cost nothing.
*/
static uint64
measure_cpu_frequency()
{
	if (sTimerFrequency == 0)
		return 0;

	bigtime_t now = system_time();
	if (sCPUClockFrequency != 0 && now - sLastMeasurement < 100000)
		return sCPUClockFrequency;

	cpu_status state = disable_interrupts();
	uint64 start = CpuTime();
	while (CpuTime() == start)
		;
	start = CpuTime();
	uint64 startCycles = CpuCycle();
	uint64 end = start + sTimerFrequency / 10000;
	while (CpuTime() < end)
		;
	uint64 cycles = CpuCycle() - startCycles;
	uint64 ticks = CpuTime() - start;
	restore_interrupts(state);

	if (ticks == 0)
		return sCPUClockFrequency;

	// rounded to MHz
	uint64 frequency = cycles * sTimerFrequency / ticks;
	frequency = (frequency + 500000) / 1000000 * 1000000;
	sCPUClockFrequency = frequency;
	sLastMeasurement = now;
	return frequency;
}


void
arch_fill_topology_node(cpu_topology_node_info* node, int32 cpu)
{
	switch (node->type) {
		case B_TOPOLOGY_ROOT:
			node->data.root.platform = B_CPU_RISC_V;
			break;

		case B_TOPOLOGY_PACKAGE:
			node->data.package.vendor = sCPUVendor;
			node->data.package.cache_line_size = CACHE_LINE_SIZE;
			break;

		case B_TOPOLOGY_CORE:
			node->data.core.model = sCPUModel;
			node->data.core.default_frequency = sCPUClockFrequency;
			break;

		default:
			break;
	}
}


status_t
arch_system_info_init(struct kernel_args *args)
{
	if (gPlatform == kPlatformSbi) {
		// JEDEC identifiers (bank and code) of the vendors we know
		switch (sbi_get_mvendorid().value) {
			case 0x489:
				sCPUVendor = B_CPU_VENDOR_SIFIVE;
				break;
		}
		sCPUModel = (uint32)sbi_get_marchid().value;
	}

	sTimerFrequency = args->arch_args.timerFrequency;
	if (measure_cpu_frequency() == 0)
		sCPUClockFrequency = 1000000000;
	dprintf("CPU clock %" B_PRIu64 " MHz\n", sCPUClockFrequency / 1000000);
	return B_OK;
}


status_t
arch_get_frequency(uint64 *frequency, int32 cpu)
{
	*frequency = measure_cpu_frequency();
	return B_OK;
}
