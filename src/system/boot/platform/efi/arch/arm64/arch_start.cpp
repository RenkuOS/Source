/*
 * Copyright 2019-2022 Haiku, Inc. All rights reserved.
 * Released under the terms of the MIT License.
 */


#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/stdio.h>

#include "arm_registers.h"
#include "efi_platform.h"
#include "generic_mmu.h"
#include "mmu.h"
#include "serial.h"
#include "smp.h"

#include "aarch64.h"


extern const char* granule_type_str(int tg);


// From entry.S
extern "C" void arch_enter_kernel(struct kernel_args* kernelArgs,
	addr_t kernelEntry, addr_t kernelStackTop, uint32 cpu);

// From arch_mmu.cpp
extern void arch_mmu_post_efi_setup(size_t memoryMapSize,
	efi_memory_descriptor *memoryMap, size_t descriptorSize,
	uint32_t descriptorVersion);

extern phys_addr_t arch_mmu_generate_post_efi_page_tables(size_t memoryMapSize,
	efi_memory_descriptor *memoryMap, size_t descriptorSize,
	uint32_t descriptorVersion);


void arm64_mmu_setup();

// Primary CPU EL2 register values (TTBR0_EL2, MAIR_EL2, VBAR_EL2)
static uint64 sTranslationBaseRegister;
static uint64 sMemoryBaseRegister;
static uint64 sVectorBaseRegister;


void
arch_convert_kernel_args(void)
{
	fix_address(gKernelArgs.arch_args.fdt);
}


/*!	Reads the EFI clock as seconds since 1970, or 0 if there's no valid time.
	Like the CMOS clock on x86, the clock is in whatever time the
	RTC_time_settings say (local or GMT).
*/
static uint64
read_efi_clock()
{
	efi_time time;
	efi_status status = kRuntimeServices->GetTime(&time, NULL);
	if (status != EFI_SUCCESS) {
		dprintf("EFI GetTime failed: %#lx\n", status);
		return 0;
	}
	dprintf("EFI time: %04u-%02u-%02u %02u:%02u:%02u, time zone %d, "
		"daylight %#x\n", time.Year, time.Month, time.Day, time.Hour,
		time.Minute, time.Second, time.TimeZone, time.Daylight);

	static const uint8 kDaysInMonth[12]
		= {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	bool leapYear = (time.Year % 4 == 0 && time.Year % 100 != 0)
		|| time.Year % 400 == 0;
	if (time.Year < 1970 || time.Month < 1 || time.Month > 12 || time.Day < 1
		|| time.Day > kDaysInMonth[time.Month - 1]
			+ (time.Month == 2 && leapYear ? 1 : 0)
		|| time.Hour > 23 || time.Minute > 59 || time.Second > 59) {
		dprintf("EFI time is invalid, ignoring it\n");
		return 0;
	}

	// Julian day number, as rtc_tm_to_secs() in the kernel computes it
	int year = time.Year, month = time.Month;
	int days = time.Day - 32075 - 2440588
		+ 1461 * (year + 4800 + (month - 14) / 12) / 4
		+ 367 * (month - 2 - 12 * ((month - 14) / 12)) / 12
		- 3 * ((year + 4900 + (month - 14) / 12) / 100) / 4;
	int64 seconds = (int64)days * 86400 + time.Hour * 3600 + time.Minute * 60
		+ time.Second;

	// TimeZone is ignored: the kernel takes this as RTC time, and would
	// apply the zone offset again if RTC_time_settings say local time.

	return seconds > 0 ? seconds : 0;
}


void
arm64_common_cpu_startup(uint32 cpu)
{
	arch_cache_disable();

	// Enable EL2 host bits if FEAT_VHE is available
	uint64 el = arch_exception_level();
	bool e2h = false;
	if (el == 2) {
		uint64 id_aa64mmfr1_el1 = READ_SPECIALREG(ID_AA64MMFR1_EL1);
		if (ID_AA64MMFR1_VH(id_aa64mmfr1_el1) == ID_AA64MMFR1_VH_IMPL) {
			e2h = true;
			WRITE_SPECIALREG(HCR_EL2, HCR_RW | HCR_TGE | HCR_E2H | HCR_AMO | HCR_IMO | HCR_FMO);
			WRITE_SPECIALREG(CPACR_EL1, CPACR_FPEN_TRAP_NONE);
		}
	}

	// EL2 with E2H enabled behaves as a superset of EL1
	//
	// EL2 without E2H enabled does not, so we need to drop to EL1
	if (el == 1 || e2h) {
		arm64_mmu_setup();
	} else {
		arm64_mmu_setup();
		// A secondary CPU released from a spin table starts with EL2 registers uninitialized
		// so we need to copy them from the primary CPU before calling _arch_transition_EL2_EL1
		if (cpu == 0) {
			sTranslationBaseRegister = READ_SPECIALREG(TTBR0_EL2);
			sMemoryBaseRegister = READ_SPECIALREG(MAIR_EL2);
			sVectorBaseRegister = READ_SPECIALREG(VBAR_EL2);
		} else {
			WRITE_SPECIALREG(TTBR0_EL2, sTranslationBaseRegister);
			WRITE_SPECIALREG(MAIR_EL2, sMemoryBaseRegister);
			WRITE_SPECIALREG(VBAR_EL2, sVectorBaseRegister);
			arm64_isb();
		}
		_arch_transition_EL2_EL1();
	}

	// Allow userland to read the counter (system_time() in the commpage),
	// whichever exception level we were started in.
	WRITE_SPECIALREG(CNTKCTL_EL1, 0b11);

	WRITE_SPECIALREG(SCTLR_EL1, SCTLR_LSMAOE | SCTLR_nTLSMD
		| SCTLR_UCI | SCTLR_SPAN | SCTLR_IESB | SCTLR_nTWE | SCTLR_nTWI
		| SCTLR_UCT | SCTLR_DZE | SCTLR_SED | SCTLR_SA0 | SCTLR_SA);

	arch_cache_enable();
}


void
arch_start_kernel(addr_t kernelEntry)
{
	// There's no RTC driver in the kernel; EFI is our only clock.
	gKernelArgs.rtc_boot_seconds = read_efi_clock();
	// main() trimmed kernel_args to v1 as there's no microcode; the clock
	// is past that, so pass all of it.
	gKernelArgs.kernel_args_size = sizeof(kernel_args);

	// Prepare to exit EFI boot services.
	// Read the memory map.
	// First call is to determine the buffer size.
	size_t memoryMapSize = 0;
	efi_memory_descriptor dummy;
	size_t mapKey;
	size_t descriptorSize;
	uint32_t descriptorVersion;
	if (kBootServices->GetMemoryMap(&memoryMapSize, &dummy, &mapKey,
			&descriptorSize, &descriptorVersion) != EFI_BUFFER_TOO_SMALL) {
		panic("Unable to determine size of system memory map");
	}

	// Allocate a buffer twice as large as needed just in case it gets bigger
	// between calls to ExitBootServices.
	size_t actualMemoryMapSize = memoryMapSize * 2;
	efi_memory_descriptor *memoryMap
		= (efi_memory_descriptor *)kernel_args_malloc(actualMemoryMapSize);

	if (memoryMap == NULL)
		panic("Unable to allocate memory map.");

	// Read (and print) the memory map.
	memoryMapSize = actualMemoryMapSize;
	if (kBootServices->GetMemoryMap(&memoryMapSize, memoryMap, &mapKey,
			&descriptorSize, &descriptorVersion) != EFI_SUCCESS) {
		panic("Unable to fetch system memory map.");
	}

	addr_t addr = (addr_t)memoryMap;
	efi_physical_addr loaderCode = 0LL;
	dprintf("System provided memory map:\n");
	for (size_t i = 0; i < memoryMapSize / descriptorSize; i++) {
		efi_memory_descriptor *entry
			= (efi_memory_descriptor *)(addr + i * descriptorSize);
		dprintf("  phys: 0x%08" PRIx64 "-0x%08" PRIx64
			", virt: 0x%08" PRIx64 "-0x%08" PRIx64
			", size = 0x%08" PRIx64 ", type: %s (%#x), attr: %#" PRIx64 "\n",
			entry->PhysicalStart,
			entry->PhysicalStart + entry->NumberOfPages * B_PAGE_SIZE,
			entry->VirtualStart,
			entry->VirtualStart + entry->NumberOfPages * B_PAGE_SIZE,
			entry->NumberOfPages * B_PAGE_SIZE,
			memory_region_type_str(entry->Type), entry->Type,
			entry->Attribute);
		if (entry->Type == EfiLoaderCode)
			loaderCode = entry->PhysicalStart;
	}
	// This is where our efi loader got relocated, therefore we need to use this
	// offset for properly align symbols
	dprintf("Efi loader symbols offset: 0x%0lx:\n", loaderCode);

	/*
	*   "The AArch64 exception model is made up of a number of exception levels
	*    (EL0 - EL3), with EL0 and EL1 having a secure and a non-secure
	*    counterpart.  EL2 is the hypervisor level and exists only in non-secure
	*    mode. EL3 is the highest priority level and exists only in secure mode."
	*
	*	"2.3 UEFI System Environment and Configuration
	*    The resident UEFI boot-time environment shall use the highest non-secure
	*    privilege level available. The exact meaning of this is architecture
	*    dependent, as detailed below."

	*	"2.3.1 AArch64 Exception Levels
	*    On AArch64 UEFI shall execute as 64-bit code at either EL1 or EL2,
	*    depending on whether or not virtualization is available at OS load time."
	*/
	uint64 el = arch_exception_level();
	dprintf("Current Exception Level EL%1lx\n", el);
	dprintf("TTBR0: %" B_PRIx64 " TTBRx: %" B_PRIx64 " SCTLR: %" B_PRIx64 " TCR: %" B_PRIx64 "\n",
		arch_mmu_base_register(), arch_mmu_base_register(true), _arch_mmu_get_sctlr(),
		_arch_mmu_get_tcr());

	if (arch_mmu_enabled()) {
		dprintf("MMU Enabled, Granularity %s, bits %d\n", granule_type_str(arch_mmu_user_granule()),
			arch_mmu_user_address_bits());

		dprintf("Kernel entry accessibility W: %x R: %x\n", arch_mmu_write_access(kernelEntry),
			arch_mmu_read_access(kernelEntry));
	}

	// Generate page tables for use after ExitBootServices.
	uint64 ttbr1 = arch_mmu_generate_post_efi_page_tables(
		memoryMapSize, memoryMap, descriptorSize, descriptorVersion);

	// Attempt to fetch the memory map and exit boot services.
	// This needs to be done in a loop, as ExitBootServices can change the
	// memory map.
	// Even better: Only GetMemoryMap and ExitBootServices can be called after
	// the first call to ExitBootServices, as the firmware is permitted to
	// partially exit. This is why twice as much space was allocated for the
	// memory map, as it's impossible to allocate more now.
	// A changing memory map shouldn't affect the generated page tables, as
	// they only needed to know about the maximum address, not any specific
	// entry.

	dprintf("Calling ExitBootServices. So long, EFI!\n");
	serial_disable();

	while (true) {
		if (kBootServices->ExitBootServices(kImage, mapKey) == EFI_SUCCESS) {
			// Disconnect from EFI serial_io / stdio services
			serial_kernel_handoff();
			dprintf("Unhooked from EFI serial services\n");
			break;
		}

		memoryMapSize = actualMemoryMapSize;
		if (kBootServices->GetMemoryMap(&memoryMapSize, memoryMap, &mapKey,
				&descriptorSize, &descriptorVersion) != EFI_SUCCESS) {
			panic("Unable to fetch system memory map.");
		}
	}

	// Update EFI, generate final kernel physical memory map, etc.
	arch_mmu_post_efi_setup(memoryMapSize, memoryMap,
		descriptorSize, descriptorVersion);

	// Re-init and activate serial in a horrific post-EFI landscape. Clowns roam the land freely.
	serial_init();
	serial_enable();

	arm64_common_cpu_startup(0);

	smp_boot_other_cpus(ttbr1, kernelEntry, (addr_t)&gKernelArgs);

	if (arch_mmu_read_access(kernelEntry)
		&& arch_mmu_read_access(gKernelArgs.cpu_kstack[0].start)) {
		// Enter the kernel!
		arch_enter_kernel(&gKernelArgs, kernelEntry,
			gKernelArgs.cpu_kstack[0].start + gKernelArgs.cpu_kstack[0].size, 0);
	} else {
		// _arch_exception_panic("Kernel or Stack memory not accessible\n", __LINE__);
		panic("Kernel or Stack memory not accessible\n");
	}
}
