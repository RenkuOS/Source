/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * The GPU side of the PowerVR BXE-4-32 driver: power, clocks and resets of
 * the JH7110, the MIPS firmware (loaded and started as Linux'
 * drm/imagination pvr_fw_mips.c and pvr_fw_startstop.c do), its interface
 * objects and the firmware interrupt.
 */


#include "PowerVRDevice.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <KernelExport.h>
#include <driver_settings.h>
#include <kernel.h>
#include <syscalls.h>
#include <vm/vm.h>


// JH7110
#define PMU_BASE						0x17030000
#define PMU_SW_TURN_ON					0x0c
#define PMU_SW_ENCOURAGE				0x44
#define PMU_CURRENT_MODE				0x80
#define PMU_DOMAIN_GPUA					(1u << 2)
#define SYSCRG_BASE						0x13020000
#define SYSCRG_GATE_ENABLE				(1u << 31)
#define SYSCRG_RESET_ASSERT0			0x2f8
#define SYSCRG_RESET_STATUS0			0x308
#define CCACHE_BASE						0x02010000
#define CCACHE_FLUSH64					0x200
#define CCACHE_LINE						64
#define GPU_BASE						0x18000000
#define GPU_SIZE						0x100000
#define SYSSYSCON_BASE					0x13030000
#define PLL2_FBDIV						0x2c		// bits 28:17
#define PLL2_FRAC						0x30		// postdiv1 in bits 29:28
#define PLL2_PREDIV						0x34		// bits 5:0
#define SYSCRG_GPU_ROOT					3			// mux: PLL2, PLL1
#define SYSCRG_GPU_CORE					45			// divider of gpu_root
#define OSCILLATOR_HZ					24000000
#define GPU_CORE_CLOCK					396000000	// PLL2 1188 MHz / 3
#define GPU_CORE_CLOCK_MAX				600000000
	// JH7110 datasheet: up to 600 MHz (400 MHz by default); 594 MHz is
	// PLL2 / 2. A driver settings file "powervr_jh7110" with
	// "core_clock_mhz 396" chooses a lower clock.
#define GPU_CORE_DIVIDER_MASK			0xffffff

// SYS clocks gpu_core_clk, gpu_sys_clk, gpu_apb, gpu_rtc_toggle,
// noc_bus_gpu_axi (gpu_core, 45, is a divider without a gate)
static const uint32 kClockGates[] = { 46, 47, 48, 49, 50 };
// SYS resets noc_bus_gpu_axi, gpu_apb, gpu_doma
static const uint32 kResets[] = { 27, 21, 22 };

// Rogue registers (pvr_rogue_cr_defs.h)
#define CR_POWER_EVENT					0x0038
#define CR_SOFT_RESET					0x0100
#define CR_SOFT_RESET2					0x0108
#define CR_EVENT_STATUS					0x0130
#define CR_EVENT_CLEAR					0x0138
#define CR_MIPS_WRAPPER_CONFIG			0x0810
#define CR_MIPS_ADDR_REMAP1_CONFIG1		0x0818
#define CR_MIPS_ADDR_REMAP1_CONFIG2		0x0820
#define CR_MIPS_ADDR_REMAP2_CONFIG1		0x0828
#define CR_MIPS_ADDR_REMAP2_CONFIG2		0x0830
#define CR_MIPS_ADDR_REMAP3_CONFIG1		0x0838
#define CR_MIPS_ADDR_REMAP3_CONFIG2		0x0840
#define CR_MIPS_ADDR_REMAP5_CONFIG1		0x0858
#define CR_MIPS_ADDR_REMAP5_CONFIG2		0x0860
#define CR_MIPS_WRAPPER_IRQ_ENABLE		0x08a0
#define CR_MIPS_WRAPPER_IRQ_STATUS		0x08a8
#define CR_MIPS_WRAPPER_IRQ_CLEAR		0x08b0
#define CR_MIPS_DEBUG_CONFIG			0x08c8
#define CR_MARS_IDLE					0x08f8
#define CR_MTS_SCHEDULE					0x0b00
#define CR_MTS_GARTEN_WRAPPER_CONFIG	0x0b50
#define CR_SLC_CTRL_MISC				0x3800
#define CR_AXI_ACE_LITE_CONFIGURATION	0x38c0
#define CR_SYS_BUS_SECURE				0xa100

#define POWER_EVENT_POWER_UP_RASCAL_DUST	0xff000703u
	// power up, request, domains CLUSTER0 and SPU0, bit 10, multicore mask
#define EVENT_POWER_COMPLETE			0x00000400u
#define MARS_IDLE_CPU_SYSARB0			0x00000006u
#define SOFT_RESET_PBE2_XE_MASKFULL		0xffeffffffffffc3dULL
#define SOFT_RESET2_MASKFULL			0x00000000001fffffULL
#define SOFT_RESET_RASCALDUSTS			0x000001f8e0000000ULL
#define SOFT_RESET_GARTEN				0x0000000100000000ULL
#define REMAP_ADDR_OUT_CLRMSK			0xffffff0000000fffULL
#define REMAP_ENABLE					1
#define REMAP_LOG2_SEGMENT_SIZE			12
#define BOOT_REMAP_PHYS_ADDR_IN			0x1fc00000
#define DATA_REMAP_PHYS_ADDR_IN			0x1fc01000
#define CODE_REMAP_PHYS_ADDR_IN			0x1fc02000
#define REGISTERS_VIRTUAL_BASE			0xcf800000
#define WRAPPER_CONFIG_BOOT_ISA_MICROMIPS	0x00010000
#define AXI_ACE_LITE_VALUE				0x8882af00ULL
#define SLC_PSG_HAZARD_CHECK			0x2
#define SLC_ADDR_DECODE_PVR_HASH1		0x200000
#define SLC_BYPASS_BURST_COMBINER		0x1

// MIPS firmware
#define FIRMWARE_NAME					"rogue_36.50.54.182_v1.fw"
#define FIRMWARE_MAX_SIZE				(512 * 1024)
#define EXPECTED_BVNC					0x00240032003600b6ULL
#define PAGE_TABLE_PAGES				4
#define PTE_PFN_MASK					0x3fffffc0
#define PTE_UNCACHED_READ_WRITE			(0x04 | 0x02 | 0x01 | (2 << 3))
#define HEAP_OFFSET_MASK				0xffffff
#define HEAP_SIZE						(16 * 1024 * 1024)
#define HEAP_ANY_START					0x100000
#define MIPS_HEAP_BASE					0xc0000000u
#define FIRMWARE_MEMORY_SIZE			(8 * 1024 * 1024)

enum {
	MIPS_CODE = 4, MIPS_EXCEPTIONS_CODE, MIPS_BOOT_CODE, MIPS_PRIVATE_DATA,
	MIPS_BOOT_DATA, MIPS_STACK
};
enum { FW_CODE = 1, FW_DATA = 2 };

struct fw_info_header {
	uint32	info_version;
	uint32	header_len;
	uint32	layout_entry_num;
	uint32	layout_entry_size;
	uint64	bvnc;
	uint32	fw_page_size;
	uint32	flags;
	uint16	fw_version_major;
	uint16	fw_version_minor;
	uint32	fw_version_build;
	uint32	device_info_size;
	uint32	padding;
} _PACKED;

struct fw_layout_entry {
	uint32	id;
	uint32	type;
	uint32	base_addr;
	uint32	max_size;
	uint32	alloc_size;
	uint32	alloc_offset;
} _PACKED;

struct mipsfw_boot_data {
	uint64	stack_phys_addr;
	uint64	reg_base;
	uint64	pt_phys_addr[PAGE_TABLE_PAGES];
	uint32	pt_log2_page_size;
	uint32	pt_num_pages;
	uint32	reserved1;
	uint32	reserved2;
} _PACKED;

struct elf32_header {
	uint8	ident[16];
	uint16	type;
	uint16	machine;
	uint32	version;
	uint32	entry;
	uint32	phoff;
	uint32	shoff;
	uint32	flags;
	uint16	ehsize;
	uint16	phentsize;
	uint16	phnum;
} _PACKED;

struct elf32_program_header {
	uint32	type;
	uint32	offset;
	uint32	vaddr;
	uint32	paddr;
	uint32	filesz;
	uint32	memsz;
	uint32	flags;
	uint32	align;
} _PACKED;

static const char* const kFirmwareDirectories[] = {
	"/boot/system/non-packaged/data/firmware/powervr",
	"/boot/system/data/firmware/powervr",
};


static inline void
memory_fence()
{
	__asm__ __volatile__("fence rw, rw" ::: "memory");
}


//	#pragma mark - FirmwareHeap


FirmwareHeap::FirmwareHeap()
	:
	fArea(-1),
	fBase(NULL),
	fPhysicalBase(0),
	fSize(0),
	fPageBitmap(NULL),
	fPageCount(0)
{
	memset(fOffsetBitmap, 0, sizeof(fOffsetBitmap));
}


FirmwareHeap::~FirmwareHeap()
{
	Uninit();
}


void
FirmwareHeap::Uninit()
{
	free(fPageBitmap);
	fPageBitmap = NULL;
	if (fArea >= 0)
		delete_area(fArea);
	fArea = -1;
	fBase = NULL;
	fPhysicalBase = 0;
	fSize = 0;
	fPageCount = 0;
	memset(fOffsetBitmap, 0, sizeof(fOffsetBitmap));
}


status_t
FirmwareHeap::Init(size_t size)
{
	fArea = create_area("powervr firmware memory", (void**)&fBase,
		B_ANY_KERNEL_ADDRESS, size, B_32_BIT_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (fArea < 0)
		return fArea;

	physical_entry entry;
	status_t status = get_memory_map(fBase, size, &entry, 1);
	if (status != B_OK)
		return status;

	fPhysicalBase = entry.address;
	fSize = size;
	fPageCount = size / B_PAGE_SIZE;
	fPageBitmap = (uint8*)calloc(1, (fPageCount + 7) / 8);
	if (fPageBitmap == NULL)
		return B_NO_MEMORY;
	memset(fOffsetBitmap, 0, sizeof(fOffsetBitmap));
	return B_OK;
}


status_t
FirmwareHeap::_Find(uint8* bitmap, uint32 count, uint32 pages,
	uint32 alignPages, uint32 first, uint32* _index)
{
	for (uint32 index = ROUNDUP(first, alignPages); index + pages <= count;
			index += alignPages) {
		uint32 i = 0;
		while (i < pages
			&& (bitmap[(index + i) / 8] & (1 << ((index + i) % 8))) == 0) {
			i++;
		}
		if (i == pages) {
			*_index = index;
			return B_OK;
		}
	}
	return B_NO_MEMORY;
}


void
FirmwareHeap::_Mark(uint8* bitmap, uint32 index, uint32 pages, bool used)
{
	for (uint32 i = index; i < index + pages; i++) {
		if (used)
			bitmap[i / 8] |= 1 << (i % 8);
		else
			bitmap[i / 8] &= ~(1 << (i % 8));
	}
}


status_t
FirmwareHeap::AllocatePages(size_t size, size_t alignment, uint8** _cpu,
	phys_addr_t* _phys)
{
	uint32 pages = ROUNDUP(size, B_PAGE_SIZE) / B_PAGE_SIZE;
	uint32 alignPages = max_c(alignment, (size_t)B_PAGE_SIZE) / B_PAGE_SIZE;
	uint32 index;
	status_t status = _Find(fPageBitmap, fPageCount, pages, alignPages, 0,
		&index);
	if (status != B_OK)
		return status;

	_Mark(fPageBitmap, index, pages, true);
	*_cpu = fBase + (size_t)index * B_PAGE_SIZE;
	*_phys = fPhysicalBase + (phys_addr_t)index * B_PAGE_SIZE;
	memset(*_cpu, 0, (size_t)pages * B_PAGE_SIZE);
	return B_OK;
}


void
FirmwareHeap::FreePages(void* cpu, size_t size)
{
	uint32 index = ((uint8*)cpu - fBase) / B_PAGE_SIZE;
	_Mark(fPageBitmap, index, ROUNDUP(size, B_PAGE_SIZE) / B_PAGE_SIZE,
		false);
}


status_t
FirmwareHeap::AllocateOffset(size_t size, size_t alignment, uint32* _offset)
{
	uint32 pages = ROUNDUP(size, B_PAGE_SIZE) / B_PAGE_SIZE;
	uint32 alignPages = max_c(alignment, (size_t)B_PAGE_SIZE) / B_PAGE_SIZE;
	uint32 index;
	status_t status = _Find(fOffsetBitmap, HEAP_SIZE / B_PAGE_SIZE, pages,
		alignPages, HEAP_ANY_START / B_PAGE_SIZE, &index);
	if (status != B_OK)
		return status;

	_Mark(fOffsetBitmap, index, pages, true);
	*_offset = index * B_PAGE_SIZE;
	return B_OK;
}


status_t
FirmwareHeap::ReserveOffset(uint32 offset, size_t size)
{
	uint32 index = offset / B_PAGE_SIZE;
	uint32 pages = ROUNDUP(size, B_PAGE_SIZE) / B_PAGE_SIZE;
	if (index + pages > HEAP_SIZE / B_PAGE_SIZE)
		return B_BAD_VALUE;
	_Mark(fOffsetBitmap, index, pages, true);
	return B_OK;
}


void
FirmwareHeap::FreeOffset(uint32 offset, size_t size)
{
	_Mark(fOffsetBitmap, offset / B_PAGE_SIZE,
		ROUNDUP(size, B_PAGE_SIZE) / B_PAGE_SIZE, false);
}


//	#pragma mark - PowerVRDevice


PowerVRDevice::PowerVRDevice()
	:
	interrupts(0),
	mmuPageFaults(0),
	hardwareRecoveries(0),
	firmwareStarts(0),
	fRunning(false),
	fLost(false),
	fResetHook(NULL),
	fResetCookie(NULL),
	fEventHook(NULL),
	fEventCookie(NULL),
	fEventWorker(-1),
	fEventWorkerQuit(false),
	fRegisters(NULL),
	fPMU(NULL),
	fClocks(NULL),
	fFlush64(NULL),
	fImage(NULL),
	fImageSize(0),
	fPageTable(NULL),
	fInterruptInstalled(false),
	fBVNC(0),
	fFirmwareMajor(0),
	fFirmwareMinor(0),
	fFirmwareBuild(0),
	fCoreClock(GPU_CORE_CLOCK),
	fLastContextID(0)
{
	mutex_init(&fLock, "powervr device");
	fEvent.Init(this, "powervr firmware event");
	for (int i = 0; i < 4; i++)
		fRegisterAreas[i] = -1;

	memset(&fContextEnvironment, 0, sizeof(fContextEnvironment));
	fContextEnvironment.cookie = this;
	fContextEnvironment.fw_alloc = FirmwareAllocate;
	fContextEnvironment.fw_free = FirmwareFree;
	fContextEnvironment.sync = CacheSync;
	fContextEnvironment.mts_schedule = MTSSchedule;
	fContextEnvironment.cr_read64 = ReadRegister64Hook;
	fContextEnvironment.log = NULL;

	memset(&fMMUEnvironment, 0, sizeof(fMMUEnvironment));
	fMMUEnvironment.cookie = this;
	fMMUEnvironment.gpu_alloc = GPUAllocate;
	fMMUEnvironment.gpu_free = GPUFree;
	fMMUEnvironment.sync = CacheSync;
	fMMUEnvironment.log = NULL;
	fMMUEnvironment.phys_bus_width = 36;
}


PowerVRDevice::~PowerVRDevice()
{
	Stop();
	ReleaseMemory();
	_UnmapRegisters();
	mutex_destroy(&fLock);
}


status_t
PowerVRDevice::_MapRegisters()
{
	if (fRegisterAreas[0] >= 0)
		return B_OK;

	struct {
		phys_addr_t	base;
		size_t		size;
		void**		address;
		const char*	name;
	} maps[] = {
		{ GPU_BASE, GPU_SIZE, (void**)&fRegisters, "powervr registers" },
		{ PMU_BASE, B_PAGE_SIZE, (void**)&fPMU, "powervr pmu" },
		{ SYSCRG_BASE, B_PAGE_SIZE, (void**)&fClocks, "powervr syscrg" },
		{ CCACHE_BASE, B_PAGE_SIZE, NULL, "powervr ccache" },
	};
	for (int i = 0; i < 4; i++) {
		void* address;
		fRegisterAreas[i] = map_physical_memory(maps[i].name, maps[i].base,
			maps[i].size, B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
		if (fRegisterAreas[i] < 0) {
			status_t status = fRegisterAreas[i];
			_UnmapRegisters();
			return status;
		}
		if (maps[i].address != NULL)
			*maps[i].address = address;
		else
			fFlush64 = (volatile uint64*)((addr_t)address + CCACHE_FLUSH64);
	}
	return B_OK;
}


void
PowerVRDevice::_UnmapRegisters()
{
	for (int i = 0; i < 4; i++) {
		if (fRegisterAreas[i] >= 0)
			delete_area(fRegisterAreas[i]);
		fRegisterAreas[i] = -1;
	}
	fRegisters = NULL;
	fPMU = NULL;
	fClocks = NULL;
	fFlush64 = NULL;
}


void
PowerVRDevice::Sync(phys_addr_t physical, size_t size)
{
	phys_addr_t end = physical + size;
	physical &= ~(phys_addr_t)(CCACHE_LINE - 1);
	memory_fence();
	for (; physical < end; physical += CCACHE_LINE)
		*fFlush64 = physical;
	memory_fence();
}


/*!	The GPUA power domain, the clocks and the resets of the GPU, as StarFive's
	u0_img_gpu_enable() does; the resets are asserted first, so the GPU
	starts from its reset state even if it ran before.
*/
status_t
PowerVRDevice::_PowerUp()
{
	if ((fPMU[PMU_CURRENT_MODE / 4] & PMU_DOMAIN_GPUA) == 0) {
		// only the GPUA bit: the other domains (VOUT) must not switch
		cpu_status state = disable_interrupts();
		fPMU[PMU_SW_TURN_ON / 4] = PMU_DOMAIN_GPUA;
		fPMU[PMU_SW_ENCOURAGE / 4] = 0xff;
		fPMU[PMU_SW_ENCOURAGE / 4] = 0x05;
		fPMU[PMU_SW_ENCOURAGE / 4] = 0x50;
		restore_interrupts(state);
		bigtime_t start = system_time();
		while ((fPMU[PMU_CURRENT_MODE / 4] & PMU_DOMAIN_GPUA) == 0) {
			if (system_time() - start > 10000) {
				POWERVR_TRACE("power domain GPUA does not come up\n");
				return B_TIMED_OUT;
			}
			spin(1);
		}
	}

	// with the clocks still gated
	_SetCoreClock();

	for (size_t i = 0; i < B_COUNT_OF(kClockGates); i++) {
		cpu_status state = disable_interrupts();
		fClocks[kClockGates[i]] |= SYSCRG_GATE_ENABLE;
		restore_interrupts(state);
		if ((fClocks[kClockGates[i]] & SYSCRG_GATE_ENABLE) == 0) {
			POWERVR_TRACE("clock %" B_PRIu32 " does not turn on\n",
				kClockGates[i]);
			return B_ERROR;
		}
	}

	cpu_status state = disable_interrupts();
	fClocks[SYSCRG_RESET_ASSERT0 / 4] |= (1u << 21) | (1u << 22);
	restore_interrupts(state);
	spin(100);

	for (size_t i = 0; i < B_COUNT_OF(kResets); i++) {
		uint32 bit = 1u << kResets[i];
		state = disable_interrupts();
		fClocks[SYSCRG_RESET_ASSERT0 / 4] &= ~bit;
		restore_interrupts(state);
		bigtime_t start = system_time();
		while ((fClocks[SYSCRG_RESET_STATUS0 / 4] & bit) == 0) {
			if (system_time() - start > 10000) {
				POWERVR_TRACE("reset %" B_PRIu32 " not released\n",
					kResets[i]);
				return B_TIMED_OUT;
			}
			spin(1);
		}
	}
	return B_OK;
}


/*!	Puts the GPU back into reset. The clocks and the power domain stay as
	they are: the domain is shared with nothing, but switching it is left
	for when the driver can restore everything it holds.
*/
void
PowerVRDevice::_PowerDown()
{
	cpu_status state = disable_interrupts();
	fClocks[SYSCRG_RESET_ASSERT0 / 4] |= (1u << 21) | (1u << 22);
	restore_interrupts(state);
}


/*!	gpu_core = gpu_root / divider, gpu_root = PLL2 (or PLL1, not used by the
	firmware the Mars boots with): the firmware and its timers need the real
	rate.
*/
//!	The rate of PLL2, which gpu_root selects; 0 if it cannot be read.
uint64
PowerVRDevice::_PLL2Rate()
{
	void* address;
	area_id area = map_physical_memory("powervr sys syscon", SYSSYSCON_BASE,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, &address);
	if (area < 0)
		return 0;
	volatile uint32* syscon = (volatile uint32*)address;
	uint64 fbdiv = (syscon[PLL2_FBDIV / 4] >> 17) & 0xfff;
	uint64 prediv = syscon[PLL2_PREDIV / 4] & 0x3f;
	uint64 postdiv1 = (syscon[PLL2_FRAC / 4] >> 28) & 0x3;
	delete_area(area);
	if (prediv == 0 || fbdiv == 0)
		return 0;
	return OSCILLATOR_HZ * fbdiv / prediv >> postdiv1;
}


/*!	Sets gpu_core to the fastest rate of PLL2 / divider within the limit,
	GPU_CORE_CLOCK_MAX or the driver settings' core_clock_mhz. Leaves an
	unexpected clock setup alone.
*/
void
PowerVRDevice::_SetCoreClock()
{
	uint64 limit = GPU_CORE_CLOCK_MAX;
	void* settings = load_driver_settings("powervr_jh7110");
	if (settings != NULL) {
		const char* value = get_driver_parameter(settings, "core_clock_mhz",
			NULL, NULL);
		if (value != NULL) {
			uint64 mhz = strtoul(value, NULL, 10);
			if (mhz >= 100 && mhz * 1000000 <= GPU_CORE_CLOCK_MAX)
				limit = mhz * 1000000;
		}
		unload_driver_settings(settings);
	}

	uint32 root = (fClocks[SYSCRG_GPU_ROOT] >> 24) & 0x3f;
	uint64 pll2 = _PLL2Rate();
	if (root != 0 || pll2 == 0)
		return;
	uint32 divider = (pll2 + limit - 1) / limit;
	if (divider < 2)
		divider = 2;
	if (divider > 7)
		divider = 7;
	cpu_status state = disable_interrupts();
	fClocks[SYSCRG_GPU_CORE] = (fClocks[SYSCRG_GPU_CORE]
		& ~(uint32)GPU_CORE_DIVIDER_MASK) | divider;
	restore_interrupts(state);
}


status_t
PowerVRDevice::_MeasureCoreClock()
{
	uint64 pll2 = _PLL2Rate();
	uint32 root = (fClocks[SYSCRG_GPU_ROOT] >> 24) & 0x3f;
	uint32 divider = fClocks[SYSCRG_GPU_CORE] & GPU_CORE_DIVIDER_MASK;
	if (root != 0 || pll2 == 0 || divider == 0) {
		POWERVR_TRACE("unexpected GPU clock setup (root %" B_PRIu32
			", divider %" B_PRIu32 "), assuming %d Hz\n", root, divider,
			GPU_CORE_CLOCK);
		fCoreClock = GPU_CORE_CLOCK;
		return B_OK;
	}
	fCoreClock = pll2 / divider;
	return B_OK;
}


status_t
PowerVRDevice::_LoadFirmware()
{
	int fd = -1;
	for (size_t i = 0; i < B_COUNT_OF(kFirmwareDirectories) && fd < 0; i++) {
		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "%s/%s", kFirmwareDirectories[i],
			FIRMWARE_NAME);
		fd = _kern_open(-1, path, O_RDONLY, 0);
	}
	if (fd < 0) {
		POWERVR_TRACE("firmware %s not found\n", FIRMWARE_NAME);
		return B_ENTRY_NOT_FOUND;
	}

	struct stat stat;
	status_t status = _kern_read_stat(fd, NULL, false, &stat, sizeof(stat));
	if (status == B_OK && (stat.st_size < 8192
			|| stat.st_size > FIRMWARE_MAX_SIZE || stat.st_size % 4096 != 0)) {
		status = B_BAD_DATA;
	}
	if (status == B_OK) {
		fImage = (uint8*)malloc(stat.st_size);
		if (fImage == NULL)
			status = B_NO_MEMORY;
	}
	if (status == B_OK) {
		ssize_t bytesRead = _kern_read(fd, 0, fImage, stat.st_size);
		if (bytesRead != stat.st_size)
			status = bytesRead < 0 ? bytesRead : B_IO_ERROR;
	}
	_kern_close(fd);

	if (status != B_OK) {
		free(fImage);
		fImage = NULL;
		return status;
	}
	fImageSize = stat.st_size;
	return B_OK;
}


static const fw_layout_entry*
find_layout_entry(const fw_info_header* header, uint32 id)
{
	const fw_layout_entry* entries = (const fw_layout_entry*)
		((const uint8*)header + header->header_len);
	for (uint32 i = 0; i < header->layout_entry_num; i++) {
		if (entries[i].id == id)
			return &entries[i];
	}
	return NULL;
}


void
PowerVRDevice::_MapFirmwarePages(uint32 heapOffset, phys_addr_t phys,
	size_t size)
{
	uint32 first = (heapOffset & HEAP_OFFSET_MASK) / B_PAGE_SIZE;
	uint32 pages = ROUNDUP(size, B_PAGE_SIZE) / B_PAGE_SIZE;
	for (uint32 i = 0; i < pages; i++) {
		fPageTable[first + i] = ((uint32)((phys + (phys_addr_t)i
				* B_PAGE_SIZE) >> 12 << 6) & PTE_PFN_MASK)
			| PTE_UNCACHED_READ_WRITE;
	}
	Sync(fPageTablePhysical + first * sizeof(uint32), pages * sizeof(uint32));
}


/*!	The firmware image into its code and data objects (the PT_LOAD segments
	of the ELF image, each into the section of the layout table that holds
	it), the MIPS page table, the boot data and the firmware interface.
*/
status_t
PowerVRDevice::_PrepareFirmware()
{
	const fw_info_header* header = (const fw_info_header*)
		(fImage + fImageSize - 4096);
	if (header->info_version != 3 || header->header_len != sizeof(*header)
		|| header->layout_entry_size != sizeof(fw_layout_entry)
		|| header->layout_entry_num > 8 || (header->flags & 1) == 0) {
		POWERVR_TRACE("unexpected firmware header\n");
		return B_BAD_DATA;
	}
	fBVNC = header->bvnc;
	fFirmwareMajor = header->fw_version_major;
	fFirmwareMinor = header->fw_version_minor;
	fFirmwareBuild = header->fw_version_build;
	if (fBVNC != EXPECTED_BVNC) {
		POWERVR_TRACE("firmware for BVNC %#" B_PRIx64 "\n", fBVNC);
		return B_BAD_DATA;
	}

	const fw_layout_entry* bootCode = find_layout_entry(header,
		MIPS_BOOT_CODE);
	const fw_layout_entry* exceptionCode = find_layout_entry(header,
		MIPS_EXCEPTIONS_CODE);
	const fw_layout_entry* bootData = find_layout_entry(header,
		MIPS_BOOT_DATA);
	const fw_layout_entry* stack = find_layout_entry(header, MIPS_STACK);
	const fw_layout_entry* privateData = find_layout_entry(header,
		MIPS_PRIVATE_DATA);
	if (bootCode == NULL || exceptionCode == NULL || bootData == NULL
		|| stack == NULL || privateData == NULL) {
		POWERVR_TRACE("firmware without the MIPS sections\n");
		return B_BAD_DATA;
	}

	size_t codeSize = 0;
	size_t dataSize = 0;
	const fw_layout_entry* entries = (const fw_layout_entry*)
		((const uint8*)header + header->header_len);
	for (uint32 i = 0; i < header->layout_entry_num; i++) {
		size_t end = entries[i].alloc_offset + entries[i].alloc_size;
		if (entries[i].type == FW_CODE)
			codeSize = max_c(codeSize, end);
		else if (entries[i].type == FW_DATA)
			dataSize = max_c(dataSize, end);
	}
	codeSize = ROUNDUP(codeSize, B_PAGE_SIZE);
	dataSize = ROUNDUP(dataSize, B_PAGE_SIZE);
	uint32 dataOffset = privateData->base_addr & HEAP_OFFSET_MASK;

	uint8* code;
	uint8* data;
	uint8* pageTable;
	status_t status = fHeap.AllocatePages(codeSize, B_PAGE_SIZE, &code,
		&fCodePhysical);
	if (status == B_OK) {
		status = fHeap.AllocatePages(dataSize, B_PAGE_SIZE, &data,
			&fDataPhysical);
	}
	if (status == B_OK) {
		status = fHeap.AllocatePages(PAGE_TABLE_PAGES * B_PAGE_SIZE,
			B_PAGE_SIZE, &pageTable, &fPageTablePhysical);
	}
	if (status == B_OK)
		status = fHeap.ReserveOffset(0, codeSize);
	if (status == B_OK)
		status = fHeap.ReserveOffset(dataOffset, dataSize);
	if (status != B_OK)
		return status;
	fPageTable = (uint32*)pageTable;

	const elf32_header* elf = (const elf32_header*)fImage;
	if (memcmp(elf->ident, "\177ELF", 4) != 0 || elf->machine != 8) {
		POWERVR_TRACE("not a MIPS ELF image\n");
		return B_BAD_DATA;
	}
	for (uint32 i = 0; i < elf->phnum; i++) {
		const elf32_program_header* segment = (const elf32_program_header*)
			(fImage + elf->phoff + i * elf->phentsize);
		if (segment->type != 1)
			continue;
		const fw_layout_entry* target = NULL;
		for (uint32 e = 0; e < header->layout_entry_num; e++) {
			if (segment->vaddr >= entries[e].base_addr
				&& segment->vaddr + segment->memsz
					<= entries[e].base_addr + entries[e].alloc_size) {
				target = &entries[e];
				break;
			}
		}
		if (target == NULL || segment->offset + segment->filesz > fImageSize)
			return B_BAD_DATA;
		uint8* base = target->type == FW_CODE ? code : data;
		memcpy(base + target->alloc_offset
				+ (segment->vaddr - target->base_addr),
			fImage + segment->offset, segment->filesz);
	}

	mipsfw_boot_data* boot = (mipsfw_boot_data*)
		(data + bootData->alloc_offset);
	boot->stack_phys_addr = fDataPhysical + stack->alloc_offset;
	boot->reg_base = GPU_BASE;
	for (int i = 0; i < PAGE_TABLE_PAGES; i++)
		boot->pt_phys_addr[i] = fPageTablePhysical + i * B_PAGE_SIZE;
	boot->pt_log2_page_size = 12;
	boot->pt_num_pages = PAGE_TABLE_PAGES;

	fBootCodePhysical = fCodePhysical + bootCode->alloc_offset;
	fExceptionPhysical = fCodePhysical + exceptionCode->alloc_offset;
	fBootDataPhysical = fDataPhysical + bootData->alloc_offset;

	_MapFirmwarePages(0, fCodePhysical, codeSize);
	_MapFirmwarePages(dataOffset, fDataPhysical, dataSize);

	pvr_fwif_env environment;
	memset(&environment, 0, sizeof(environment));
	environment.cookie = this;
	environment.alloc = FirmwareAllocate;
	environment.log = NULL;
	environment.core_clock_hz = fCoreClock;
	environment.slc_size_in_kilobytes = 16;
	environment.num_clusters = 1;
	environment.trace_group_mask = 0;
	memset(&fFirmware, 0, sizeof(fFirmware));
	status = pvr_fwif_create(environment, &fFirmware);
	if (status != B_OK) {
		POWERVR_TRACE("firmware interface: %s\n", strerror(status));
		return status;
	}

	Sync(fHeap.PhysicalBase(), fHeap.Size());
	return B_OK;
}


/*!	pvr_fw_start() for a MIPS core with LAYOUT_MARS, plus the power up of
	Rascal and Dust the BXE-4-32 needs (Icenowy Zheng, Dominique Belhachemi:
	"drm/imagination: power up rascal+dust via RGX_CR_POWER_EVENT on
	BXE-4-32"): without it the SLC, the MMU and the data masters never run.
*/
status_t
PowerVRDevice::_StartFirmware()
{
	_Write32(CR_SYS_BUS_SECURE, 0);
	(void)_Read32(CR_SYS_BUS_SECURE);

	_Write64(CR_SOFT_RESET, SOFT_RESET_PBE2_XE_MASKFULL);
	_Write64(CR_SOFT_RESET2, SOFT_RESET2_MASKFULL);
	(void)_Read64(CR_SOFT_RESET);
	(void)_Read64(CR_SOFT_RESET2);

	_Write32(CR_POWER_EVENT, POWER_EVENT_POWER_UP_RASCAL_DUST);
	bigtime_t start = system_time();
	while ((_Read32(CR_EVENT_STATUS) & EVENT_POWER_COMPLETE) == 0) {
		if (system_time() - start > 100000) {
			_Write32(CR_POWER_EVENT, 0);
			POWERVR_TRACE("Rascal and Dust do not power up\n");
			return B_TIMED_OUT;
		}
		spin(10);
	}
	_Write32(CR_POWER_EVENT, 0);
	_Write32(CR_EVENT_CLEAR, EVENT_POWER_COMPLETE);

	_Write64(CR_SOFT_RESET, SOFT_RESET_PBE2_XE_MASKFULL
		^ SOFT_RESET_RASCALDUSTS);
	_Write64(CR_SOFT_RESET2, 0);
	(void)_Read64(CR_SOFT_RESET);
	(void)_Read64(CR_SOFT_RESET2);
	_Write64(CR_SOFT_RESET, SOFT_RESET_GARTEN);
	_Write64(CR_SOFT_RESET2, 0);
	(void)_Read64(CR_SOFT_RESET);
	(void)_Read64(CR_SOFT_RESET2);

	// rogue_slc_init(): 512 bit lines, multicore support (no LAZYWB)
	_Write32(CR_SLC_CTRL_MISC, (_Read32(CR_SLC_CTRL_MISC)
			& SLC_PSG_HAZARD_CHECK)
		| SLC_ADDR_DECODE_PVR_HASH1 | SLC_BYPASS_BURST_COMBINER);

	// pvr_mips_wrapper_init(), with BRN63553 (REMAP5)
	_Write32(CR_MIPS_WRAPPER_CONFIG, (REGISTERS_VIRTUAL_BASE >> 16)
		| WRAPPER_CONFIG_BOOT_ISA_MICROMIPS);
	_Write64(CR_MIPS_ADDR_REMAP1_CONFIG1, BOOT_REMAP_PHYS_ADDR_IN
		| REMAP_ENABLE);
	_Write64(CR_MIPS_ADDR_REMAP1_CONFIG2, (fBootCodePhysical
		& ~REMAP_ADDR_OUT_CLRMSK) | REMAP_LOG2_SEGMENT_SIZE);
	_Write64(CR_MIPS_ADDR_REMAP5_CONFIG1, REMAP_ENABLE);
	_Write64(CR_MIPS_ADDR_REMAP5_CONFIG2, (fBootCodePhysical
		& ~REMAP_ADDR_OUT_CLRMSK) | REMAP_LOG2_SEGMENT_SIZE);
	_Write64(CR_MIPS_ADDR_REMAP2_CONFIG1, DATA_REMAP_PHYS_ADDR_IN
		| REMAP_ENABLE);
	_Write64(CR_MIPS_ADDR_REMAP2_CONFIG2, (fBootDataPhysical
		& ~REMAP_ADDR_OUT_CLRMSK) | REMAP_LOG2_SEGMENT_SIZE);
	_Write64(CR_MIPS_ADDR_REMAP3_CONFIG1, CODE_REMAP_PHYS_ADDR_IN
		| REMAP_ENABLE);
	_Write64(CR_MIPS_ADDR_REMAP3_CONFIG2, (fExceptionPhysical
		& ~REMAP_ADDR_OUT_CLRMSK) | REMAP_LOG2_SEGMENT_SIZE);
	_Write64(CR_MTS_GARTEN_WRAPPER_CONFIG, 0);
	_Write32(CR_MIPS_DEBUG_CONFIG, 0);

	// rogue_axi_ace_list_init()
	_Write64(CR_AXI_ACE_LITE_CONFIGURATION, AXI_ACE_LITE_VALUE);

	spin(3);
	_Write64(CR_SOFT_RESET, 0);
	(void)_Read64(CR_SOFT_RESET);
	spin(3);

	start = system_time();
	while (true) {
		Sync(fFirmware.sysinit_phys, B_PAGE_SIZE);
		if (pvr_fwif_firmware_started(&fFirmware))
			break;
		if (system_time() - start > 2000000) {
			POWERVR_TRACE("the firmware does not start\n");
			_Write64(CR_SOFT_RESET, SOFT_RESET_PBE2_XE_MASKFULL);
			return B_TIMED_OUT;
		}
		snooze(1000);
	}
	POWERVR_TRACE("firmware %u.%u build %" B_PRIu32 " started after %"
		B_PRId64 " us\n", fFirmwareMajor, fFirmwareMinor, fFirmwareBuild,
		system_time() - start);
	return B_OK;
}


/*!	pvr_fw_stop() for LAYOUT_MARS: SIDEKICK and SLC are powered down by the
	firmware; wait for the MIPS and its bus arbiter to idle, then hold
	everything in soft reset.
*/
status_t
PowerVRDevice::_StopFirmware()
{
	status_t status = B_OK;
	bigtime_t start = system_time();
	while ((_Read32(CR_MARS_IDLE) & MARS_IDLE_CPU_SYSARB0)
			!= MARS_IDLE_CPU_SYSARB0) {
		if (system_time() - start > 100000) {
			status = B_TIMED_OUT;
			break;
		}
		spin(10);
	}
	_Write64(CR_SOFT_RESET, SOFT_RESET_PBE2_XE_MASKFULL);
	return status;
}


status_t
PowerVRDevice::Start()
{
	if (fRunning)
		return B_OK;

	ReleaseMemory();
	status_t status = _MapRegisters();
	if (status == B_OK && fImage == NULL)
		status = _LoadFirmware();
	if (status == B_OK)
		status = _PowerUp();
	if (status == B_OK && *(volatile uint64*)(fRegisters + 0x20)
			!= EXPECTED_BVNC) {
		POWERVR_TRACE("not a BXE-4-32: CORE_ID %#" B_PRIx64 "\n",
			*(volatile uint64*)(fRegisters + 0x20));
		status = B_DEVICE_NOT_FOUND;
	}
	if (status == B_OK)
		status = _MeasureCoreClock();
	if (status == B_OK)
		status = fHeap.Init(FIRMWARE_MEMORY_SIZE);
	if (status == B_OK)
		status = _PrepareFirmware();
	if (status == B_OK)
		status = _StartFirmware();
	if (status == B_OK) {
		_Write32(CR_MIPS_WRAPPER_IRQ_CLEAR, 1);
		_Write32(CR_MIPS_WRAPPER_IRQ_ENABLE, 1);
		status = install_io_interrupt_handler(82, &_HandleInterrupt, this, 0);
		fInterruptInstalled = status == B_OK;
	}
	if (status == B_OK) {
		fEventWorkerQuit = false;
		fEventWorker = spawn_kernel_thread(&_EventWorker,
			"powervr firmware events", B_URGENT_DISPLAY_PRIORITY, this);
		status = fEventWorker < 0 ? fEventWorker : resume_thread(fEventWorker);
		if (status != B_OK)
			fEventWorker = -1;
	}
	if (status != B_OK) {
		POWERVR_TRACE("start failed: %s\n", strerror(status));
		Stop();
		ReleaseMemory();
		return status;
	}

	firmwareStarts++;
	fRunning = true;
	fLost = false;
	return B_OK;
}


void
PowerVRDevice::Stop()
{
	if (fEventWorker >= 0) {
		// the worker never blocks on the device lock, which the caller
		// may hold
		fEventWorkerQuit = true;
		fEvent.NotifyAll();
		status_t result;
		wait_for_thread(fEventWorker, &result);
		fEventWorker = -1;
	}
	if (fInterruptInstalled) {
		_Write32(CR_MIPS_WRAPPER_IRQ_ENABLE, 0);
		remove_io_interrupt_handler(82, &_HandleInterrupt, this);
		fInterruptInstalled = false;
	}
	if (fRunning) {
		if (_StopFirmware() != B_OK)
			POWERVR_TRACE("the firmware did not idle before the reset\n");
		_PowerDown();
		fRunning = false;
	}
}


void
PowerVRDevice::ReleaseMemory()
{
	// the heap goes with everything still in it
	fHeap.Uninit();
	fPageTable = NULL;
	memset(&fFirmware, 0, sizeof(fFirmware));
}


//	#pragma mark - events


int32
PowerVRDevice::_HandleInterrupt(void* data)
{
	PowerVRDevice* device = (PowerVRDevice*)data;
	if ((device->_Read32(CR_MIPS_WRAPPER_IRQ_STATUS) & 1) == 0)
		return B_UNHANDLED_INTERRUPT;

	device->_Write32(CR_MIPS_WRAPPER_IRQ_CLEAR, 1);
	device->interrupts++;
	device->fEvent.NotifyAll();
	return B_INVOKE_SCHEDULER;
}


/*!	Services the firmware CCB as soon as the firmware writes to it: a
	render can stall on a free list grow request until the host answers,
	with nobody waiting on the device. It only tries the device lock, so
	that Stop() can wait for it under the lock; whoever holds the lock
	drains the CCB as well, or the next wake-up does.
*/
status_t
PowerVRDevice::_EventWorker(void* data)
{
	PowerVRDevice* device = (PowerVRDevice*)data;
	while (!device->fEventWorkerQuit) {
		device->WaitForEvent(10000);
		if (device->fEventWorkerQuit)
			break;
		if (mutex_trylock(&device->fLock) != B_OK)
			continue;
		device->ProcessFirmwareEvents();
		mutex_unlock(&device->fLock);
	}
	return B_OK;
}


status_t
PowerVRDevice::WaitForEvent(bigtime_t timeout)
{
	ConditionVariableEntry entry;
	fEvent.Add(&entry);
	return entry.Wait(B_RELATIVE_TIMEOUT | B_KILL_CAN_INTERRUPT, timeout);
}


/*!	Drains the firmware CCB (pvr_fwccb_process()): the firmware reports
	context resets after a hardware recovery and page faults there.
*/
void
PowerVRDevice::ProcessFirmwareEvents()
{
	pvr_fwif_ccb& ccb = fFirmware.fwccb;
	if (ccb.ctrl == NULL)
		return;

	Sync(ccb.ctrl_phys, sizeof(*ccb.ctrl));
	uint32 readOffset = ccb.ctrl->read_offset;
	uint32 writeOffset = ccb.ctrl->write_offset;
	if (readOffset == writeOffset)
		return;

	Sync(ccb.ring_phys, ccb.num_cmds * ccb.cmd_size);
	while (readOffset != writeOffset) {
		const rogue_fwif_fwccb_cmd* command = (const rogue_fwif_fwccb_cmd*)
			((uint8*)ccb.ring + readOffset * ccb.cmd_size);
		uint32 type = command->cmd_type & 0xffff;
		const uint32* words = (const uint32*)command->cmd_data.raw;
		switch (type) {
			case 105:
			{
				// CONTEXT_RESET_NOTIFICATION:
				// rogue_fwif_fwccb_cmd_context_reset_data
				uint32 contextID = words[0];
				uint32 reason = words[1];
				uint32 flags = words[4];
				uint64 faultAddress = words[8] | (uint64)words[9] << 32;
				hardwareRecoveries++;
				if ((flags & 1) != 0)	// CONTEXT_RESET_FLAG_PF
					mmuPageFaults++;
				POWERVR_TRACE("context %" B_PRIu32 " reset (reason %"
					B_PRIu32 ", flags %#" B_PRIx32 ", fault at %#" B_PRIx64
					")\n", contextID, reason, flags, faultAddress);
				if (fResetHook != NULL) {
					fResetHook(fResetCookie, contextID, reason, flags,
						faultAddress);
				}
				break;
			}
			case 109:
				// REQUEST_GPU_RESTART: nothing runs until the last client
				// closed the device and the next open restarts the firmware
				fLost = true;
				POWERVR_TRACE("the firmware asks for a restart\n");
				break;
			case 112:
				// CONTEXT_FW_PF_NOTIFICATION: the firmware faulted itself
				fLost = true;
				POWERVR_TRACE("firmware page fault at %#" B_PRIx64 "\n",
					words[0] | (uint64)words[1] << 32);
				break;
			default:
				if (fEventHook != NULL)
					fEventHook(fEventCookie, type, words);
				else
					POWERVR_TRACE("firmware event %" B_PRIu32 "\n", type);
				break;
		}
		readOffset = (readOffset + 1) & ccb.ctrl->wrap_mask;
	}
	ccb.ctrl->read_offset = readOffset;
	Sync(ccb.ctrl_phys, sizeof(*ccb.ctrl));
}


//	#pragma mark - hooks of the rogue modules


status_t
PowerVRDevice::FirmwareAllocate(void* cookie, size_t size, size_t alignment,
	uint32 heapOffset, void** _cpu, uint64* _phys, uint32* _fwAddress)
{
	PowerVRDevice* device = (PowerVRDevice*)cookie;
	size = ROUNDUP(size, B_PAGE_SIZE);

	status_t status;
	if (heapOffset == PVR_FWIF_ANY_OFFSET)
		status = device->fHeap.AllocateOffset(size, alignment, &heapOffset);
	else
		status = device->fHeap.ReserveOffset(heapOffset, size);
	if (status != B_OK)
		return status;

	uint8* cpu;
	phys_addr_t phys;
	status = device->fHeap.AllocatePages(size, alignment, &cpu, &phys);
	if (status != B_OK) {
		device->fHeap.FreeOffset(heapOffset, size);
		return status;
	}

	device->Sync(phys, size);
	device->_MapFirmwarePages(heapOffset, phys, size);
	*_cpu = cpu;
	*_phys = phys;
	*_fwAddress = MIPS_HEAP_BASE | heapOffset;
	return B_OK;
}


void
PowerVRDevice::FirmwareFree(void* cookie, void* cpu, uint64 phys,
	uint32 fwAddress, size_t size)
{
	PowerVRDevice* device = (PowerVRDevice*)cookie;
	size = ROUNDUP(size, B_PAGE_SIZE);
	uint32 offset = fwAddress & HEAP_OFFSET_MASK;
	uint32 first = offset / B_PAGE_SIZE;
	for (uint32 i = 0; i < size / B_PAGE_SIZE; i++)
		device->fPageTable[first + i] = 0;
	device->Sync(device->fPageTablePhysical + first * sizeof(uint32),
		size / B_PAGE_SIZE * sizeof(uint32));
	device->fHeap.FreeOffset(offset, size);
	device->fHeap.FreePages(cpu, size);
}


status_t
PowerVRDevice::GPUAllocate(void* cookie, size_t size, size_t alignment,
	void** _cpu, uint64* _phys)
{
	PowerVRDevice* device = (PowerVRDevice*)cookie;
	uint8* cpu;
	phys_addr_t phys;
	status_t status = device->fHeap.AllocatePages(size, alignment, &cpu,
		&phys);
	if (status != B_OK)
		return status;
	*_cpu = cpu;
	*_phys = phys;
	return B_OK;
}


void
PowerVRDevice::GPUFree(void* cookie, void* cpu, uint64 phys, size_t size)
{
	((PowerVRDevice*)cookie)->fHeap.FreePages(cpu, size);
}


void
PowerVRDevice::CacheSync(void* cookie, uint64 phys, size_t size)
{
	((PowerVRDevice*)cookie)->Sync(phys, size);
}


void
PowerVRDevice::MTSSchedule(void* cookie, uint32 value)
{
	PowerVRDevice* device = (PowerVRDevice*)cookie;
	device->_Write32(CR_MTS_SCHEDULE, value);
	(void)device->_Read32(CR_MTS_SCHEDULE);
}


uint64
PowerVRDevice::ReadRegister64Hook(void* cookie, uint32 offset)
{
	return ((PowerVRDevice*)cookie)->_Read64(offset);
}


void
PowerVRDevice::Log(void* cookie, const char* format, ...)
{
	char buffer[256];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	dprintf("powervr_jh7110: %s", buffer);
}
