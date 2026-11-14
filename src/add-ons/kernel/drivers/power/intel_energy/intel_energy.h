/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef _INTEL_ENERGY_H
#define _INTEL_ENERGY_H


#include <Drivers.h>


/*!	Interface of the intel_energy driver, which publishes
	/dev/power/intel_energy. It reports the energy and C6 residency counters
	of Intel Atom processors based on Silvermont (CPUID 06_37H, 06_4AH,
	06_5AH, 06_5DH). All counters are raw: a reader takes two samples and
	uses the difference.
*/


#define INTEL_ENERGY_DEVICE			"/dev/power/intel_energy"
#define INTEL_ENERGY_MAX_CPUS		8


enum {
	INTEL_ENERGY_GET_SAMPLE = B_DEVICE_OP_CODES_END + 20101
};


typedef struct intel_energy_sample {
	uint32	cpu_count;
	uint32	energy_unit_nj;		// nanojoule per energy counter step

	uint32	package_energy;		// MSR_PKG_ENERGY_STATUS, wraps at 32 bits
	uint32	cores_energy;		// MSR_PP0_ENERGY_STATUS, wraps at 32 bits

	uint64	package_tsc;		// time stamp counter of CPU 0
	uint64	package_c6;			// MSR_PKG_C6_RESIDENCY, TSC ticks

	uint64	core_tsc[INTEL_ENERGY_MAX_CPUS];
	uint64	core_c6[INTEL_ENERGY_MAX_CPUS];	// MSR_CORE_C6_RESIDENCY
	uint32	core_mhz[INTEL_ENERGY_MAX_CPUS];	// from IA32_PERF_STATUS
} intel_energy_sample;


#endif	// _INTEL_ENERGY_H
