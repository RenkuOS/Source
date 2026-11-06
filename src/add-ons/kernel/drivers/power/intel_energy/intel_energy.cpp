/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	Read-only access to the energy and C6 residency counters of Intel Atom
	processors based on Silvermont, as described in the Intel 64 and IA-32
	Architectures Software Developer's Manual, Volume 4, tables 2-6 to 2-8.
	Used by the power tool to show how much the processor uses and how long
	it sleeps.
*/


#include <Drivers.h>
#include <KernelExport.h>

#include <string.h>

#include <arch_cpu.h>
#include <cpu.h>
#include <kernel.h>
#include <smp.h>

#include "intel_energy.h"


#define ERROR(x...)	dprintf("intel_energy: " x)


#define MSR_FSB_FREQ				0xcd
#define MSR_PKG_C6_RESIDENCY		0x3fa
#define MSR_CORE_C6_RESIDENCY		0x3fd
#define MSR_RAPL_POWER_UNIT			0x606
#define MSR_PKG_ENERGY_STATUS		0x611
#define MSR_PP0_ENERGY_STATUS		0x639


int32 api_version = B_CUR_DRIVER_API_VERSION;

static const char* sDeviceNames[] = {
	"power/intel_energy",
	NULL
};

static uint32 sEnergyUnit;		// nanojoule per step
static uint32 sBusKHz;


static bool
is_supported_cpu()
{
	cpu_ent* cpu = get_cpu_struct();
	if (cpu->arch.vendor != VENDOR_INTEL || cpu->arch.family != 6)
		return false;

	uint8 model = cpu->arch.model + (cpu->arch.extended_model << 4);
	switch (model) {
		case 0x37:
		case 0x4a:
		case 0x5a:
		case 0x5d:
			return true;
	}

	return false;
}


static uint32
bus_khz()
{
	// MSR_FSB_FREQ bits 2:0, table 2-8 of the SDM, volume 4.
	switch (x86_read_msr(MSR_FSB_FREQ) & 0x7) {
		case 0:
			return 83300;
		case 1:
			return 100000;
		case 2:
			return 133300;
		case 3:
			return 116700;
		case 4:
			return 80000;
	}

	return 0;
}


static void
read_core(void* cookie, int cpu)
{
	if (cpu >= INTEL_ENERGY_MAX_CPUS)
		return;

	intel_energy_sample* sample = (intel_energy_sample*)cookie;
	sample->core_tsc[cpu] = x86_read_msr(IA32_MSR_TSC);
	sample->core_c6[cpu] = x86_read_msr(MSR_CORE_C6_RESIDENCY);

	uint32 ratio = (x86_read_msr(IA32_MSR_PERF_STATUS) >> 8) & 0x7f;
	sample->core_mhz[cpu] = ratio * sBusKHz / 1000;

	if (cpu == 0) {
		sample->package_tsc = sample->core_tsc[0];
		sample->package_c6 = x86_read_msr(MSR_PKG_C6_RESIDENCY);
		sample->package_energy = (uint32)x86_read_msr(MSR_PKG_ENERGY_STATUS);
		sample->cores_energy = (uint32)x86_read_msr(MSR_PP0_ENERGY_STATUS);
	}
}


//	#pragma mark - device hooks


static status_t
intel_energy_open(const char* name, uint32 flags, void** cookie)
{
	*cookie = NULL;
	return B_OK;
}


static status_t
intel_energy_close(void* cookie)
{
	return B_OK;
}


static status_t
intel_energy_free(void* cookie)
{
	return B_OK;
}


static status_t
intel_energy_read(void* cookie, off_t position, void* buffer,
	size_t* numBytes)
{
	*numBytes = 0;
	return B_NOT_ALLOWED;
}


static status_t
intel_energy_write(void* cookie, off_t position, const void* buffer,
	size_t* numBytes)
{
	*numBytes = 0;
	return B_NOT_ALLOWED;
}


static status_t
intel_energy_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	if (op != INTEL_ENERGY_GET_SAMPLE)
		return B_DEV_INVALID_IOCTL;

	if (buffer == NULL || length < sizeof(intel_energy_sample))
		return B_BAD_VALUE;

	intel_energy_sample sample;
	memset(&sample, 0, sizeof(sample));

	int32 count = smp_get_num_cpus();
	if (count > INTEL_ENERGY_MAX_CPUS)
		count = INTEL_ENERGY_MAX_CPUS;
	sample.cpu_count = count;
	sample.energy_unit_nj = sEnergyUnit;

	call_all_cpus_sync(read_core, &sample);

	if (!IS_USER_ADDRESS(buffer))
		return B_BAD_ADDRESS;
	return user_memcpy(buffer, &sample, sizeof(sample));
}


//	#pragma mark - driver hooks


status_t
init_hardware()
{
	return is_supported_cpu() ? B_OK : B_ERROR;
}


status_t
init_driver()
{
	if (!is_supported_cpu())
		return B_ERROR;

	// Energy Status Units, bits 12:8: one step is 2^ESU microjoule on these
	// processors (table 2-8), unlike the 1/2^ESU joule of the big cores.
	uint32 esu = (x86_read_msr(MSR_RAPL_POWER_UNIT) >> 8) & 0x1f;
	if (esu > 22) {
		ERROR("unexpected energy unit %" B_PRIu32 "\n", esu);
		return B_ERROR;
	}
	sEnergyUnit = (1U << esu) * 1000;
	sBusKHz = bus_khz();

	return B_OK;
}


void
uninit_driver()
{
}


const char**
publish_devices()
{
	return sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	static device_hooks hooks = {
		intel_energy_open,
		intel_energy_close,
		intel_energy_free,
		intel_energy_control,
		intel_energy_read,
		intel_energy_write,
	};

	return &hooks;
}
