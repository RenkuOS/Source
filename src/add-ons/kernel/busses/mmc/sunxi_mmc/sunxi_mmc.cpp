/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */

/*	The SD/MMC host controllers ("SMHC") of Allwinner SoCs, as found on the
	A733 of the Radxa Cubie A7S: SMHC0 drives the SD card slot, SMHC2 the
	optional eMMC.

	The register interface is Allwinner's own (Linux: sunxi-mmc.c, U-Boot:
	sunxi_mmc.c). Data moves through the controller's internal DMA engine
	("IDMAC"), which walks a chain of descriptors. As in the Raspberry Pi's
	driver, the descriptors and the data live in one uncached, physically
	contiguous buffer that the operation's memory is copied through.

	The A733 controllers only run in the "new timing" mode, where the card
	clock comes straight from the module clock in the clock controller (CCU).
	Its rate is programmed the way U-Boot does it on these boards, which is
	what the boot firmware left running when Haiku takes over.

	Pins, card power and the card detect line stay as the boot firmware set
	them up. The slot is probed when the bus comes up. */


#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>

#include <arch/atomic.h>
#include <condition_variable.h>
#include <vm/vm.h>

#include "IOSchedulerSimple.h"
#include "mmc.h"


//#define TRACE_SUNXI_MMC
#ifdef TRACE_SUNXI_MMC
#	define TRACE(x...) dprintf("sunxi_mmc: " x)
#else
#	define TRACE(x...) ;
#endif
#define INFO(x...)	dprintf("sunxi_mmc: " x)
#define ERROR(x...)	dprintf("sunxi_mmc: " x)


#define SUNXI_MMC_DRIVER_MODULE_NAME	"busses/mmc/sunxi_mmc/driver_v1"
#define SUNXI_MMC_BUS_MODULE_NAME		"busses/mmc/sunxi_mmc/device/v1"


// controller registers
#define REG_GCTRL		0x000
#define REG_CLKCR		0x004
#define REG_TMOUT		0x008
#define REG_WIDTH		0x00c
#define REG_BLKSZ		0x010
#define REG_BCNTR		0x014
#define REG_CMDR		0x018
#define REG_CARG		0x01c
#define REG_RESP0		0x020
#define REG_RESP1		0x024
#define REG_RESP2		0x028
#define REG_RESP3		0x02c
#define REG_IMASK		0x030
#define REG_MISTA		0x034
#define REG_RINTR		0x038
#define REG_STAS		0x03c
#define REG_FTRGL		0x040
#define REG_FUNS		0x044
#define REG_DBGC		0x050
#define REG_NTSR		0x05c
#define REG_HWRST		0x078
#define REG_DMAC		0x080
#define REG_DLBA		0x084
#define REG_IDST		0x088
#define REG_IDIE		0x08c
#define REG_THLDC		0x100
#define REG_SAMP_DL		0x144

#define GCTRL_SOFT_RESET		(1u << 0)
#define GCTRL_FIFO_RESET		(1u << 1)
#define GCTRL_DMA_RESET			(1u << 2)
#define GCTRL_INTERRUPT_ENABLE	(1u << 4)
#define GCTRL_DMA_ENABLE		(1u << 5)
#define GCTRL_DDR_MODE			(1u << 10)
#define GCTRL_ACCESS_DONE_DIRECT (1u << 30)
#define GCTRL_ACCESS_BY_AHB		(1u << 31)
#define GCTRL_RESET_ALL			(GCTRL_SOFT_RESET | GCTRL_FIFO_RESET \
									| GCTRL_DMA_RESET)

#define CLKCR_DIVIDER_MASK		0xff
#define CLKCR_CARD_CLOCK_ON		(1u << 16)
#define CLKCR_LOW_POWER_ON		(1u << 17)
#define CLKCR_MASK_DATA0		(1u << 31)

#define CMD_RESPONSE			(1u << 6)
#define CMD_LONG_RESPONSE		(1u << 7)
#define CMD_CHECK_CRC			(1u << 8)
#define CMD_DATA				(1u << 9)
#define CMD_WRITE				(1u << 10)
#define CMD_AUTO_STOP			(1u << 12)
#define CMD_WAIT_PRE_OVER		(1u << 13)
#define CMD_SEND_INIT_SEQUENCE	(1u << 15)
#define CMD_UPDATE_CLOCK_ONLY	(1u << 21)
#define CMD_START				(1u << 31)

#define INT_RESP_ERROR			(1u << 1)
#define INT_COMMAND_DONE		(1u << 2)
#define INT_DATA_OVER			(1u << 3)
#define INT_RESP_CRC_ERROR		(1u << 6)
#define INT_DATA_CRC_ERROR		(1u << 7)
#define INT_RESP_TIMEOUT		(1u << 8)
#define INT_DATA_TIMEOUT		(1u << 9)
#define INT_FIFO_RUN_ERROR		(1u << 11)
#define INT_HARDWARE_LOCKED		(1u << 12)
#define INT_START_BIT_ERROR		(1u << 13)
#define INT_AUTO_COMMAND_DONE	(1u << 14)
#define INT_END_BIT_ERROR		(1u << 15)
#define INT_ERRORS				(INT_RESP_ERROR | INT_RESP_CRC_ERROR \
									| INT_DATA_CRC_ERROR | INT_RESP_TIMEOUT \
									| INT_DATA_TIMEOUT | INT_FIFO_RUN_ERROR \
									| INT_HARDWARE_LOCKED | INT_START_BIT_ERROR \
									| INT_END_BIT_ERROR)
#define INT_TIMEOUTS			(INT_RESP_TIMEOUT | INT_DATA_TIMEOUT)
#define INT_WANTED				(INT_COMMAND_DONE | INT_DATA_OVER \
									| INT_AUTO_COMMAND_DONE | INT_ERRORS)

// not a controller bit: the IDMAC finished receiving
#define EVENT_DMA_RECEIVED		(1u << 30)

#define STAS_CARD_DATA_BUSY		(1u << 9)

#define FUNS_CEATA_ON			(0xceaau << 16)

#define DMAC_SOFT_RESET			(1u << 0)
#define DMAC_FIX_BURST			(1u << 1)
#define DMAC_IDMA_ON			(1u << 7)

#define IDST_RECEIVE_INTERRUPT	(1u << 1)
#define IDST_ALL				0x337

#define DES0_DISABLE_INTERRUPT	(1u << 1)
#define DES0_LAST				(1u << 2)
#define DES0_FIRST				(1u << 3)
#define DES0_CHAINED			(1u << 4)
#define DES0_END_OF_RING		(1u << 5)
#define DES0_OWN				(1u << 31)

#define NTSR_NEW_TIMING			(1u << 31)
#define SAMP_DL_SW_ENABLE		(1u << 7)

#define THLDC_READ_ENABLE		(1u << 0)
#define THLDC_WRITE_ENABLE		(1u << 2)
#define THLDC_READ_THRESHOLD(x)	(((x) & 0xfff) << 16)

// The A733's clock controller: per controller a module clock register and
// a bus gate / reset register, 0x10 apart.
#define A733_CCU_BASE			0x02002000
#define A733_CCU_SIZE			0x1000
#define A733_CCU_MMC_CLOCK(n)	(0xd00 + 0x10 * (n))
#define A733_CCU_MMC_GATE(n)	(0xd0c + 0x10 * (n))
#define CCU_GATE_BUS			(1u << 0)
#define CCU_GATE_RESET			(1u << 16)
#define CCU_MMC_ENABLE			(1u << 31)
#define CCU_MMC_SOURCE_OSC24M	(0u << 24)
#define CCU_MMC_SOURCE_PERIPH0	(1u << 24)
#define CCU_MMC_N(n)			((uint32)(n) << 8)	// divides by n + 1
#define CCU_MMC_M(m)			((uint32)(m) - 1)		// divides by m

// What the sources yield as a card clock (U-Boot's arithmetic for the
// A733: PERIPH0's 400 MHz output halved by the new timing mode).
static const uint32 kOscillatorRate = 24000000;
static const uint32 kPeriph0Rate = 200000000;

// Descriptors carry at most 8 KiB (13 bits); use whole pages.
static const size_t kSegmentSize = 4096;
static const size_t kBufferSize = 512 * 1024;
static const size_t kDescriptorCount = kBufferSize / kSegmentSize;
static const size_t kDescriptorAreaSize = 4096;
static const uint32 kBlockSize = 512;

// Error bits of an R1 card status
static const uint32 kR1ErrorMask = 0xfff9a080;


struct idma_descriptor {
	uint32	config;
	uint32	size;
	uint32	buffer;		// physical address >> 2
	uint32	next;		// physical address >> 2
};


device_manager_info* gDeviceManager;


struct sunxi_mmc_device {
	device_node*	node;
	uint64			registers;
	uint64			registersSize;
	uint32			interrupt;
	uint32			index;			// SMHCn
	uint32			busWidth;
	uint32			maxFrequency;	// Hz
	bool			nonRemovable;
};


class SunxiMmcBus {
public:
								SunxiMmcBus(const sunxi_mmc_device& device);
								~SunxiMmcBus();

			status_t			InitCheck() const { return fStatus; }

			status_t			SetClock(uint32 kilohertz);
			status_t			ExecuteCommand(uint8 command, uint32 argument,
									uint32* response);
			status_t			DoIO(uint8 command, IOOperation* operation,
									bool offsetAsSectors);
			void				SetScanSemaphore(sem_id semaphore);
			void				SetBusWidth(int width);
			void				SetCardType(card_type type)
									{ fCardType = type; }
			status_t			ReadData(uint8 command, uint32 argument,
									void* buffer, size_t length);
			status_t			SetTiming(int timing, uint32 flags);
			void				Terminate();

			int32				HandleInterrupt();

private:
			uint32				_Read(uint32 reg)
									{ return *(volatile uint32*)(fRegisters + reg); }
			void				_Write(uint32 reg, uint32 value)
									{ *(volatile uint32*)(fRegisters + reg) = value; }
			uint32				_ReadCcu(uint32 reg)
									{ return *(volatile uint32*)(fCcu + reg); }
			void				_WriteCcu(uint32 reg, uint32 value)
									{ *(volatile uint32*)(fCcu + reg) = value; }

			status_t			_ResetController();
			void				_Recover();
			status_t			_UpdateClock();
			status_t			_SetModuleClock(uint32 hertz);
			status_t			_AllocateBuffer();
			void				_SetUpDescriptors(size_t size);
			void				_StartDma(bool isWrite);
			void				_StopDma();
			status_t			_WaitNotBusy(bigtime_t timeout);
			status_t			_Wait(uint32 mask, bigtime_t timeout);
			status_t			_SendCommand(uint8 command, uint32 argument,
									uint32 flags, uint32* response,
									size_t dataSize = 0,
									size_t blockSize = kBlockSize);
			status_t			_CopyVecs(bool toBuffer,
									const generic_io_vec* vecs, size_t count,
									size_t& index, generic_size_t& vecOffset,
									size_t size);
			status_t			_ReadData(uint8 command, uint32 argument,
									size_t size);

private:
			sunxi_mmc_device	fDevice;
			area_id				fRegisterArea;
			volatile uint8*		fRegisters;
			area_id				fCcuArea;
			volatile uint8*		fCcu;
			status_t			fStatus;
			bool				fInterruptInstalled;
			uint32				fClock;			// Hz, 0 while off
			int32				fEvents;
			ConditionVariable	fEventCondition;
			card_type			fCardType;
			area_id				fBufferArea;
			uint8*				fBuffer;
			phys_addr_t			fBufferAddress;
			idma_descriptor*	fDescriptors;
			phys_addr_t			fDescriptorAddress;
};


static int32
sunxi_mmc_interrupt(void* data)
{
	return ((SunxiMmcBus*)data)->HandleInterrupt();
}


SunxiMmcBus::SunxiMmcBus(const sunxi_mmc_device& device)
	:
	fDevice(device),
	fRegisterArea(-1),
	fRegisters(NULL),
	fCcuArea(-1),
	fCcu(NULL),
	fStatus(B_NO_INIT),
	fInterruptInstalled(false),
	fClock(0),
	fEvents(0),
	fCardType(CARD_TYPE_UNKNOWN),
	fBufferArea(-1),
	fBuffer(NULL),
	fBufferAddress(0),
	fDescriptors(NULL),
	fDescriptorAddress(0)
{
	fEventCondition.Init(this, "sunxi_mmc events");

	fRegisterArea = map_physical_memory("sunxi_mmc registers",
		fDevice.registers, fDevice.registersSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fRegisters);
	if (fRegisterArea < 0) {
		fStatus = fRegisterArea;
		return;
	}
	fCcuArea = map_physical_memory("sunxi_mmc clock controller", A733_CCU_BASE,
		A733_CCU_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fCcu);
	if (fCcuArea < 0) {
		fStatus = fCcuArea;
		return;
	}

	uint32 firmwareClock = _ReadCcu(A733_CCU_MMC_CLOCK(fDevice.index));
	uint32 firmwareGate = _ReadCcu(A733_CCU_MMC_GATE(fDevice.index));

	// bus clock on, out of reset
	_WriteCcu(A733_CCU_MMC_GATE(fDevice.index),
		firmwareGate | CCU_GATE_BUS | CCU_GATE_RESET);
	memory_full_barrier();

	fStatus = _AllocateBuffer();
	if (fStatus != B_OK)
		return;

	fStatus = _ResetController();
	if (fStatus != B_OK) {
		ERROR("SMHC%" B_PRIu32 " does not come out of reset\n", fDevice.index);
		return;
	}

	fStatus = install_io_interrupt_handler(fDevice.interrupt,
		sunxi_mmc_interrupt, this, 0);
	if (fStatus != B_OK) {
		ERROR("cannot install the handler for interrupt %" B_PRIu32 "\n",
			fDevice.interrupt);
		return;
	}
	fInterruptInstalled = true;
	_Write(REG_IMASK, INT_WANTED);

	fStatus = SetClock(400);
	if (fStatus != B_OK)
		return;

	INFO("SMHC%" B_PRIu32 " at %#" B_PRIx64 ", interrupt %" B_PRIu32
		", %" B_PRIu32 "-bit, up to %" B_PRIu32 " Hz; firmware clock %#"
		B_PRIx32 " gate %#" B_PRIx32 "\n", fDevice.index, fDevice.registers,
		fDevice.interrupt, fDevice.busWidth, fDevice.maxFrequency,
		firmwareClock, firmwareGate);
}


SunxiMmcBus::~SunxiMmcBus()
{
	if (fRegisters != NULL) {
		_Write(REG_IMASK, 0);
		_Write(REG_GCTRL, _Read(REG_GCTRL) & ~GCTRL_INTERRUPT_ENABLE);
	}
	if (fInterruptInstalled) {
		remove_io_interrupt_handler(fDevice.interrupt, sunxi_mmc_interrupt,
			this);
	}
	if (fBufferArea >= 0)
		delete_area(fBufferArea);
	if (fCcuArea >= 0)
		delete_area(fCcuArea);
	if (fRegisterArea >= 0)
		delete_area(fRegisterArea);
}


status_t
SunxiMmcBus::_ResetController()
{
	_Write(REG_GCTRL, GCTRL_RESET_ALL);
	bigtime_t timeout = system_time() + 250000;
	while ((_Read(REG_GCTRL) & GCTRL_RESET_ALL) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		spin(10);
	}

	// the card's reset line (eMMC only) goes through a pulse
	_Write(REG_HWRST, 0);
	spin(10);
	_Write(REG_HWRST, 1);
	spin(300);

	// Bursts of 8 words, receive at 7 words, transmit at 248 of the 256
	// word FIFO: the BSP's values for this controller (SMHC v5.3).
	_Write(REG_FTRGL, 0x200700f8);
	// The card read threshold only. The write threshold holds a block back
	// until that much of it is in the FIFO, which the DMA engine never fills
	// that far: writes stalled with a data request pending. The BSP leaves
	// it off as well.
	_Write(REG_THLDC, THLDC_READ_THRESHOLD(512) | THLDC_READ_ENABLE);
	_Write(REG_TMOUT, 0xffffffff);
	_Write(REG_IMASK, 0);
	_Write(REG_RINTR, 0xffffffff);
	_Write(REG_DBGC, 0xdeb);
	_Write(REG_FUNS, FUNS_CEATA_ON);
	_Write(REG_DLBA, (uint32)(fDescriptorAddress >> 2));
	_Write(REG_IDST, IDST_ALL);
	_Write(REG_IDIE, 0);

	// Data through the DMA engine, not the CPU (the firmware leaves the
	// FIFO to the CPU).
	uint32 control = _Read(REG_GCTRL);
	control |= GCTRL_INTERRUPT_ENABLE;
	control &= ~(GCTRL_ACCESS_DONE_DIRECT | GCTRL_ACCESS_BY_AHB
		| GCTRL_DDR_MODE);
	_Write(REG_GCTRL, control);
	memory_full_barrier();

	return B_OK;
}


/*!	After a failed command: reset the controller as the other drivers do,
	keeping clock and bus width, and forget what was signalled.
*/
void
SunxiMmcBus::_Recover()
{
	uint32 width = _Read(REG_WIDTH);
	_StopDma();
	_ResetController();
	_Write(REG_WIDTH, width);
	_UpdateClock();
	_Write(REG_IMASK, INT_WANTED);
	atomic_set(&fEvents, 0);
}


status_t
SunxiMmcBus::_UpdateClock()
{
	_Write(REG_CMDR, CMD_START | CMD_UPDATE_CLOCK_ONLY | CMD_WAIT_PRE_OVER);

	bigtime_t timeout = system_time() + 750000;
	while ((_Read(REG_CMDR) & CMD_START) != 0) {
		if (system_time() > timeout) {
			ERROR("the clock update does not finish\n");
			return B_TIMED_OUT;
		}
		spin(10);
	}

	// the update sets interrupt bits of its own
	_Write(REG_RINTR, _Read(REG_RINTR));
	return B_OK;
}


/*!	Programs the CCU module clock, which is the card clock in the new
	timing mode: source / (N + 1) / M with M up to 16 and N + 1 up to 4
	(U-Boot's sunxi_mmc.c for the A733).
*/
status_t
SunxiMmcBus::_SetModuleClock(uint32 hertz)
{
	uint32 source = hertz <= kOscillatorRate
		? CCU_MMC_SOURCE_OSC24M : CCU_MMC_SOURCE_PERIPH0;
	uint32 sourceRate = source == CCU_MMC_SOURCE_OSC24M
		? kOscillatorRate : kPeriph0Rate;

	uint32 divider = (sourceRate + hertz - 1) / hertz;
	uint32 shift = 0;
	while (divider > 16) {
		shift++;
		divider = (divider + 1) / 2;
	}
	if (shift > 3)
		return B_BAD_VALUE;
	uint32 n = (1u << shift) - 1;

	_WriteCcu(A733_CCU_MMC_CLOCK(fDevice.index), CCU_MMC_ENABLE | source
		| CCU_MMC_N(n) | CCU_MMC_M(divider));
	memory_full_barrier();

	fClock = sourceRate / (n + 1) / divider;
	return B_OK;
}


status_t
SunxiMmcBus::SetClock(uint32 kilohertz)
{
	if (kilohertz == 0)
		return B_BAD_VALUE;

	uint64 hertz = std::min((uint64)kilohertz * 1000,
		(uint64)fDevice.maxFrequency);

	// card clock off while the module clock changes; DATA0 masked meanwhile
	uint32 control = _Read(REG_CLKCR);
	control &= ~(CLKCR_CARD_CLOCK_ON | CLKCR_LOW_POWER_ON
		| CLKCR_DIVIDER_MASK);
	_Write(REG_CLKCR, control | CLKCR_MASK_DATA0);
	status_t status = _UpdateClock();
	if (status != B_OK)
		return status;
	fClock = 0;

	status = _SetModuleClock((uint32)hertz);
	if (status != B_OK) {
		ERROR("cannot make %" B_PRIu64 " Hz\n", hertz);
		return status;
	}

	_Write(REG_NTSR, _Read(REG_NTSR) | NTSR_NEW_TIMING);
	// sample delay: software set to 0, as Linux and Allwinner do below HS400
	_Write(REG_SAMP_DL, SAMP_DL_SW_ENABLE);

	_Write(REG_CLKCR, control | CLKCR_CARD_CLOCK_ON | CLKCR_MASK_DATA0);
	status = _UpdateClock();
	_Write(REG_CLKCR, _Read(REG_CLKCR) & ~CLKCR_MASK_DATA0);
	if (status != B_OK)
		return status;

	TRACE("clock: %" B_PRIu32 " kHz asked for, %" B_PRIu32 " Hz set\n",
		kilohertz, fClock);
	return B_OK;
}


status_t
SunxiMmcBus::_AllocateBuffer()
{
	// One contiguous piece: the descriptors, then the data.
	size_t size = kDescriptorAreaSize + kBufferSize;
	void* address;
	fBufferArea = create_area("sunxi_mmc DMA buffer", &address,
		B_ANY_KERNEL_ADDRESS, size, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (fBufferArea < 0)
		return fBufferArea;

	physical_entry entry;
	status_t status = get_memory_map(address, size, &entry, 1);
	if (status != B_OK)
		return status;
	if (entry.size < size || entry.address + size > (1ull << 34)) {
		// the descriptors hold addresses shifted by two in 32 bits
		return B_BAD_DATA;
	}

	// The controller reads and writes the memory behind the CPU's back:
	// take what the allocation left in the cache out of it, and keep the
	// buffer out of the cache from here on.
	for (addr_t line = (addr_t)address; line < (addr_t)address + size;
			line += 64) {
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	}
	memory_full_barrier();

	status = vm_set_area_memory_type(fBufferArea, entry.address,
		B_WRITE_COMBINING_MEMORY);
	if (status != B_OK)
		return status;

	fDescriptors = (idma_descriptor*)address;
	fDescriptorAddress = entry.address;
	fBuffer = (uint8*)address + kDescriptorAreaSize;
	fBufferAddress = entry.address + kDescriptorAreaSize;
	return B_OK;
}


void
SunxiMmcBus::_SetUpDescriptors(size_t size)
{
	size_t count = (size + kSegmentSize - 1) / kSegmentSize;
	for (size_t i = 0; i < count; i++) {
		idma_descriptor& descriptor = fDescriptors[i];
		size_t offset = i * kSegmentSize;
		descriptor.config = DES0_CHAINED | DES0_OWN | DES0_DISABLE_INTERRUPT;
		descriptor.size = (uint32)std::min(kSegmentSize, size - offset);
		descriptor.buffer = (uint32)((fBufferAddress + offset) >> 2);
		descriptor.next = (uint32)((fDescriptorAddress
			+ (i + 1) * sizeof(idma_descriptor)) >> 2);
	}

	fDescriptors[0].config |= DES0_FIRST;
	fDescriptors[count - 1].config |= DES0_LAST | DES0_END_OF_RING;
	fDescriptors[count - 1].config &= ~DES0_DISABLE_INTERRUPT;
	fDescriptors[count - 1].next = 0;

	// the descriptors must be in memory before the engine starts
	memory_full_barrier();
}


void
SunxiMmcBus::_StartDma(bool isWrite)
{
	uint32 control = _Read(REG_GCTRL) | GCTRL_DMA_ENABLE;
	_Write(REG_GCTRL, control);
	_Write(REG_GCTRL, control | GCTRL_DMA_RESET);
	_Write(REG_DMAC, DMAC_SOFT_RESET);
	_Write(REG_IDST, IDST_ALL);
	_Write(REG_IDIE, isWrite ? 0 : IDST_RECEIVE_INTERRUPT);
	_Write(REG_DMAC, DMAC_FIX_BURST | DMAC_IDMA_ON);
	memory_full_barrier();
}


void
SunxiMmcBus::_StopDma()
{
	_Write(REG_IDIE, 0);
	_Write(REG_IDST, IDST_ALL);
	_Write(REG_DMAC, 0);
	uint32 control = _Read(REG_GCTRL) | GCTRL_DMA_RESET;
	_Write(REG_GCTRL, control);
	control &= ~GCTRL_DMA_ENABLE;
	_Write(REG_GCTRL, control);
	_Write(REG_GCTRL, control | GCTRL_FIFO_RESET);
	memory_full_barrier();
}


status_t
SunxiMmcBus::_WaitNotBusy(bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	while ((_Read(REG_STAS) & STAS_CARD_DATA_BUSY) != 0) {
		if (system_time() > deadline)
			return B_TIMED_OUT;
		snooze(50);
	}
	return B_OK;
}


/*!	Waits until all events in \a mask were signalled, or an error was. */
status_t
SunxiMmcBus::_Wait(uint32 mask, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;

	while (true) {
		ConditionVariableEntry entry;
		fEventCondition.Add(&entry);

		uint32 events = atomic_get(&fEvents);
		if ((events & INT_ERRORS) != 0) {
			// A response timeout is reported before the command is done;
			// the controller only takes the next command after that.
			if ((events & INT_RESP_TIMEOUT) != 0
				&& (events & INT_COMMAND_DONE) == 0
				&& system_time() < deadline) {
				entry.Wait(B_ABSOLUTE_TIMEOUT, std::min(deadline,
					system_time() + 10000));
				continue;
			}
			return (events & INT_TIMEOUTS) != 0 ? B_TIMED_OUT : B_IO_ERROR;
		}
		if ((events & mask) == mask)
			return B_OK;

		status_t status = entry.Wait(B_ABSOLUTE_TIMEOUT, deadline);
		if (status != B_OK && status != B_INTERRUPTED) {
			ERROR("timeout waiting for %#" B_PRIx32 ", events %#" B_PRIx32
				", raw %#" B_PRIx32 ", status %#" B_PRIx32 ", dma %#" B_PRIx32
				"\n", mask, (uint32)atomic_get(&fEvents), _Read(REG_RINTR),
				_Read(REG_STAS), _Read(REG_IDST));
			return status;
		}
	}
}


/*!	Sends a command, and moves \a dataSize bytes between the card and the
	buffer if it has a data phase (CMD_DATA in \a flags).
*/
status_t
SunxiMmcBus::_SendCommand(uint8 command, uint32 argument, uint32 flags,
	uint32* response, size_t dataSize, size_t blockSize)
{
	// a card still programming a block does not take another command
	if (_WaitNotBusy(1000000) != B_OK) {
		ERROR("command %u: the card stays busy\n", command);
		_Recover();
		return B_BUSY;
	}

	atomic_set(&fEvents, 0);
	_Write(REG_RINTR, 0xffffffff);

	uint32 wanted = INT_COMMAND_DONE;
	if ((flags & CMD_DATA) != 0) {
		_SetUpDescriptors(dataSize);
		_Write(REG_BLKSZ, (uint32)blockSize);
		_Write(REG_BCNTR, (uint32)dataSize);
		_StartDma((flags & CMD_WRITE) != 0);

		wanted |= (flags & CMD_AUTO_STOP) != 0
			? INT_AUTO_COMMAND_DONE : INT_DATA_OVER;
		if ((flags & CMD_WRITE) == 0)
			wanted |= EVENT_DMA_RECEIVED;
	}

	if (command == GO_IDLE_STATE)
		flags |= CMD_SEND_INIT_SEQUENCE;

	_Write(REG_CARG, argument);
	memory_full_barrier();
	_Write(REG_CMDR, CMD_START | CMD_WAIT_PRE_OVER | flags | command);

	bigtime_t timeout = (flags & CMD_DATA) != 0 ? 5000000 : 1000000;
	status_t status = _Wait(wanted, timeout);
	if ((flags & CMD_DATA) != 0)
		_StopDma();
	if (status != B_OK) {
		// Cards that do not know a command do not answer; the bus manager
		// probes that way, so it is not worth a line in the log.
		TRACE("command %u (%#" B_PRIx32 ") failed: %s, events %#" B_PRIx32
			"\n", command, argument, strerror(status),
			(uint32)atomic_get(&fEvents));
		_Recover();
		return status;
	}

	if (response != NULL && (flags & CMD_RESPONSE) != 0) {
		if ((flags & CMD_LONG_RESPONSE) != 0) {
			// Bits 127..0 of an R2 reply are in RESP3..RESP0, CRC included;
			// the MMC stack wants bits 127..8, lowest word first.
			uint32 r0 = _Read(REG_RESP0);
			uint32 r1 = _Read(REG_RESP1);
			uint32 r2 = _Read(REG_RESP2);
			uint32 r3 = _Read(REG_RESP3);
			response[0] = (r0 >> 8) | (r1 << 24);
			response[1] = (r1 >> 8) | (r2 << 24);
			response[2] = (r2 >> 8) | (r3 << 24);
			response[3] = r3 >> 8;
		} else
			response[0] = _Read(REG_RESP0);
	}

	return B_OK;
}


#define RESPONSE_NONE	0
#define RESPONSE_R1		(CMD_RESPONSE | CMD_CHECK_CRC)
#define RESPONSE_R1B	(RESPONSE_R1 | (1u << 30))	// not a controller bit
#define RESPONSE_R2		(CMD_RESPONSE | CMD_LONG_RESPONSE | CMD_CHECK_CRC)
#define RESPONSE_R3		CMD_RESPONSE
#define RESPONSE_R6		RESPONSE_R1
#define RESPONSE_R7		RESPONSE_R1


status_t
SunxiMmcBus::ExecuteCommand(uint8 command, uint32 argument, uint32* response)
{
	if (fStatus != B_OK)
		return fStatus;

	uint32 flags;
	switch (command) {
		case GO_IDLE_STATE:
			flags = RESPONSE_NONE;
			break;

		case SD_APP_CMD:
		case SEND_STATUS:
		case SET_BLOCK_COUNT:
		case SD_ERASE_WR_BLK_START:
		case SD_ERASE_WR_BLK_END:
			flags = RESPONSE_R1;
			break;

		case SD_SET_BUS_WIDTH:
			// also MMC_SWITCH, which has R1b
			flags = is_mmc_card_type(fCardType) ? RESPONSE_R1B : RESPONSE_R1;
			break;

		case SELECT_DESELECT_CARD:
			flags = argument == 0 ? RESPONSE_NONE : RESPONSE_R1B;
			break;

		case SD_ERASE:
		case SD_STOP_TRANSMISSION:
			flags = RESPONSE_R1B;
			break;

		case ALL_SEND_CID:
		case SEND_CSD:
			flags = RESPONSE_R2;
			break;

		case MMC_SEND_OP_COND:
		case SD_SEND_OP_COND:
			flags = RESPONSE_R3;
			break;

		case SD_SEND_RELATIVE_ADDR:
			flags = is_mmc_card_type(fCardType) ? RESPONSE_R1 : RESPONSE_R6;
			break;

		case SD_SEND_IF_COND:
			if (is_mmc_card_type(fCardType))
				return B_NOT_ALLOWED;
					// MMC_SEND_EXT_CSD has a data phase: read_data()
			flags = RESPONSE_R7;
			break;

		default:
			ERROR("unknown command %u\n", command);
			return B_BAD_DATA;
	}

	if (response == NULL && flags != RESPONSE_NONE)
		return B_BAD_VALUE;

	bool busy = (flags & RESPONSE_R1B) == RESPONSE_R1B;
	status_t status = _SendCommand(command, argument, flags & ~(1u << 30),
		response);
	if (status == B_OK && busy) {
		// an eMMC flushing its cache or an erase can take a while
		status = _WaitNotBusy(command == SD_ERASE ? 60000000 : 5000000);
		if (status != B_OK)
			ERROR("command %u: busy for too long\n", command);
	}
	return status;
}


status_t
SunxiMmcBus::DoIO(uint8 command, IOOperation* operation, bool offsetAsSectors)
{
	if (fStatus != B_OK)
		return fStatus;
	if (operation == NULL || operation->Offset() < 0)
		return B_BAD_VALUE;

	bool isWrite = operation->IsWrite();
	if ((isWrite && command != SD_WRITE_MULTIPLE_BLOCKS
			&& command != SD_WRITE_SINGLE_BLOCK)
		|| (!isWrite && command != SD_READ_MULTIPLE_BLOCKS
			&& command != SD_READ_SINGLE_BLOCK)) {
		return B_BAD_VALUE;
	}

	uint64 offset = operation->Offset();
	generic_size_t length = operation->Length();
	if (offset % kBlockSize != 0 || length % kBlockSize != 0)
		return B_BAD_VALUE;

	bool multiple = command == SD_READ_MULTIPLE_BLOCKS
		|| command == SD_WRITE_MULTIPLE_BLOCKS;
	if (!multiple && length != kBlockSize)
		return B_BAD_VALUE;

	const generic_io_vec* vecs = operation->Vecs();
	size_t count = operation->VecCount();
	uint64 unit = offsetAsSectors ? kBlockSize : 1;

	// The pieces of memory are gathered in the buffer, so that the card gets
	// commands as long as the buffer allows.
	size_t vecIndex = 0;
	generic_size_t vecOffset = 0;

	while (length != 0) {
		size_t size = std::min(length, (generic_size_t)kBufferSize);
		if (!multiple)
			size = kBlockSize;

		if (isWrite) {
			status_t status = _CopyVecs(true, vecs, count, vecIndex,
				vecOffset, size);
			if (status != B_OK)
				return status;
			memory_full_barrier();
		}

		uint32 flags = RESPONSE_R1 | CMD_DATA;
		if (isWrite)
			flags |= CMD_WRITE;
		if (multiple)
			flags |= CMD_AUTO_STOP;

		uint32 response = 0;
		status_t status = _SendCommand(command, offset / unit, flags,
			&response, size);
		if (status == B_OK && (response & kR1ErrorMask) != 0)
			status = B_IO_ERROR;
		if (status != B_OK) {
			ERROR("%s of %" B_PRIuSIZE " bytes at %" B_PRIu64 " failed: "
				"%s, response %#" B_PRIx32 ", events %#" B_PRIx32 "\n",
				isWrite ? "write" : "read", size, offset, strerror(status),
				response, (uint32)atomic_get(&fEvents));
			return status;
		}

		if (!isWrite) {
			memory_full_barrier();
			status = _CopyVecs(false, vecs, count, vecIndex, vecOffset,
				size);
			if (status != B_OK)
				return status;
		}

		offset += size;
		length -= size;
	}

	return B_OK;
}


/*!	Copies the next \a size bytes of the operation's memory to the buffer
	or back, and moves \a index and \a vecOffset past them.
*/
status_t
SunxiMmcBus::_CopyVecs(bool toBuffer, const generic_io_vec* vecs, size_t count,
	size_t& index, generic_size_t& vecOffset, size_t size)
{
	uint8* buffer = fBuffer;

	while (size != 0) {
		if (index >= count)
			return B_BAD_VALUE;
		if (vecOffset == vecs[index].length) {
			index++;
			vecOffset = 0;
			continue;
		}

		size_t toCopy = std::min((generic_size_t)size,
			vecs[index].length - vecOffset);
		status_t status;
		if (toBuffer) {
			status = vm_memcpy_from_physical(buffer,
				vecs[index].base + vecOffset, toCopy, false);
		} else {
			status = vm_memcpy_to_physical(vecs[index].base + vecOffset,
				buffer, toCopy, false);
		}
		if (status != B_OK)
			return status;

		buffer += toCopy;
		vecOffset += toCopy;
		size -= toCopy;
	}

	return B_OK;
}


/*!	Sends a command that the card answers with \a size bytes of data, a
	single block, and leaves them in the buffer.
*/
status_t
SunxiMmcBus::_ReadData(uint8 command, uint32 argument, size_t size)
{
	uint32 response = 0;
	status_t status = _SendCommand(command, argument, RESPONSE_R1 | CMD_DATA,
		&response, size, size);
	if (status == B_OK && (response & kR1ErrorMask) != 0)
		status = B_IO_ERROR;
	memory_full_barrier();
	return status;
}


/*!	A command that reads one small block (at most 512 bytes) through the
	data lines, such as the eMMC EXT_CSD or the SD switch status.
*/
status_t
SunxiMmcBus::ReadData(uint8 command, uint32 argument, void* buffer,
	size_t length)
{
	if (fStatus != B_OK)
		return fStatus;
	if (buffer == NULL || length == 0 || length > kBlockSize)
		return B_BAD_VALUE;

	status_t status = _ReadData(command, argument, length);
	if (status == B_OK)
		memcpy(buffer, fBuffer, length);
	return status;
}


/*!	The card was switched to \a timing by the bus manager: clock it to
	match. The controller has no DDR or 1.8 V signaling here, so only the
	legacy and high speed timings are supported.
*/
status_t
SunxiMmcBus::SetTiming(int timing, uint32 flags)
{
	uint32 kilohertz;
	switch (timing) {
		case MMC_TIMING_LEGACY:
			kilohertz = 25000;
			break;
		case MMC_TIMING_HS:
		case MMC_TIMING_HS_SDR25:
			kilohertz = is_mmc_card_type(fCardType) ? 52000 : 50000;
			if (fDevice.maxFrequency < 50000000)
				return B_NOT_SUPPORTED;
			break;
		default:
			return B_NOT_SUPPORTED;
	}
	if ((flags & MMC_TIMING_QUERY) != 0)
		return B_OK;

	status_t status = SetClock(kilohertz);
	if (status == B_OK)
		INFO("%s timing: %" B_PRIu32 " Hz\n", mmc_timing_name(timing), fClock);
	return status;
}


void
SunxiMmcBus::SetScanSemaphore(sem_id semaphore)
{
	// Card detection is left to the boot firmware's pin setup; have the bus
	// look for a card right away.
	if (semaphore >= 0)
		release_sem(semaphore);
}


void
SunxiMmcBus::SetBusWidth(int width)
{
	_Write(REG_WIDTH, width == 8 ? 2 : width == 4 ? 1 : 0);
}


void
SunxiMmcBus::Terminate()
{
	_Write(REG_IMASK, 0);
	_Write(REG_CLKCR, _Read(REG_CLKCR) & ~CLKCR_CARD_CLOCK_ON);
	_UpdateClock();
}


int32
SunxiMmcBus::HandleInterrupt()
{
	uint32 events = _Read(REG_MISTA);
	uint32 dma = _Read(REG_IDST);
	if ((events == 0 && (dma & IDST_RECEIVE_INTERRUPT) == 0)
		|| events == 0xffffffff) {
		return B_UNHANDLED_INTERRUPT;
	}

	// acknowledge
	_Write(REG_RINTR, events);
	_Write(REG_IDST, dma);
	memory_full_barrier();

	events &= INT_WANTED;
	if ((dma & IDST_RECEIVE_INTERRUPT) != 0)
		events |= EVENT_DMA_RECEIVED;
	if (events != 0) {
		atomic_or(&fEvents, events);
		fEventCondition.NotifyAll();
	}

	return B_HANDLED_INTERRUPT;
}


//	#pragma mark - driver


static const char* kCompatible[] = {
	"allwinner,sun60i-a733-mmc",
	"allwinner,sun20i-d1-mmc",
};


static float
sunxi_mmc_supports_device(device_node* parent)
{
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK
		|| strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	const char* compatible;
	if (gDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK) {
		return 0.0f;
	}

	bool supported = false;
	for (size_t i = 0; i < B_COUNT_OF(kCompatible); i++) {
		if (strcmp(compatible, kCompatible[i]) == 0)
			supported = true;
	}
	if (!supported)
		return 0.0f;

	// controllers the board does not wire up are disabled
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (gDeviceManager->get_driver(parent, (driver_module_info**)&fdt,
			(void**)&device) != B_OK) {
		return 0.0f;
	}
	const char* status = (const char*)fdt->get_prop(device, "status", NULL);
	if (status != NULL && strcmp(status, "okay") != 0
		&& strcmp(status, "ok") != 0) {
		return 0.0f;
	}
	return 1.0f;
}


static status_t
sunxi_mmc_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{.string = "Allwinner SD/MMC controller"}},
		{}
	};

	return gDeviceManager->register_node(parent, SUNXI_MMC_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static uint32
get_cell(fdt_device_module_info* fdt, fdt_device* device, const char* name,
	uint32 defaultValue)
{
	int length;
	const uint32* property = (const uint32*)fdt->get_prop(device, name,
		&length);
	if (property == NULL || length != 4)
		return defaultValue;
	return B_BENDIAN_TO_HOST_INT32(*property);
}


static status_t
sunxi_mmc_init_driver(device_node* node, void** _cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* fdtDevice;
	status_t status = gDeviceManager->get_driver(parent,
		(driver_module_info**)&fdt, (void**)&fdtDevice);
	if (status != B_OK) {
		gDeviceManager->put_node(parent);
		return status;
	}

	sunxi_mmc_device* device = new(std::nothrow) sunxi_mmc_device;
	if (device == NULL) {
		gDeviceManager->put_node(parent);
		return B_NO_MEMORY;
	}
	device->node = node;

	uint64 interrupt = 0;
	if (!fdt->get_reg(fdtDevice, 0, &device->registers, &device->registersSize)
		|| !fdt->get_interrupt(fdtDevice, 0, NULL, &interrupt)) {
		ERROR("no registers or no interrupt in the device tree\n");
		gDeviceManager->put_node(parent);
		delete device;
		return B_BAD_DATA;
	}
	device->interrupt = interrupt;

	// SMHC0..2 follow each other at 4 KiB steps
	device->index = (uint32)((device->registers & 0xffff) >> 12);
	if (device->index > 2 || (device->registers & ~0xffffull) != 0x04020000) {
		ERROR("unexpected controller at %#" B_PRIx64 "\n", device->registers);
		gDeviceManager->put_node(parent);
		delete device;
		return B_BAD_DATA;
	}

	device->busWidth = get_cell(fdt, fdtDevice, "bus-width", 4);
	device->maxFrequency = std::min(get_cell(fdt, fdtDevice, "max-frequency",
		52000000), (uint32)52000000);
	device->nonRemovable = fdt->get_prop(fdtDevice, "non-removable", NULL)
		!= NULL;

	gDeviceManager->put_node(parent);

	*_cookie = device;
	return B_OK;
}


static void
sunxi_mmc_uninit_driver(void* cookie)
{
	delete (sunxi_mmc_device*)cookie;
}


static status_t
sunxi_mmc_register_child_devices(void* cookie)
{
	sunxi_mmc_device* device = (sunxi_mmc_device*)cookie;

	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{.string = device->nonRemovable ? "eMMC" : "SD card slot"}},
		{B_DEVICE_BUS, B_STRING_TYPE, {.string = "mmc"}},
		{B_DEVICE_FIXED_CHILD, B_STRING_TYPE, {.string = MMC_BUS_MODULE_NAME}},
		// whole blocks in pieces of memory that are copied through the
		// driver's own buffer, at most the buffer's size per transfer
		{B_DMA_ALIGNMENT, B_UINT32_TYPE, {.ui32 = kBlockSize - 1}},
		{B_DMA_MAX_SEGMENT_COUNT, B_UINT32_TYPE,
			{.ui32 = (uint32)(kBufferSize / B_PAGE_SIZE)}},
		{B_DMA_MAX_SEGMENT_BLOCKS, B_UINT32_TYPE,
			{.ui32 = (uint32)(kBufferSize / kBlockSize)}},
		{B_DMA_MAX_TRANSFER_BLOCKS, B_UINT32_TYPE,
			{.ui32 = (uint32)(kBufferSize / kBlockSize)}},
		{}
	};

	return gDeviceManager->register_node(device->node,
		SUNXI_MMC_BUS_MODULE_NAME, attrs, NULL, NULL);
}


//	#pragma mark - bus


static status_t
sunxi_mmc_init_bus(device_node* node, void** _cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	sunxi_mmc_device* device;
	status_t status = gDeviceManager->get_driver(parent, NULL,
		(void**)&device);
	gDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	SunxiMmcBus* bus = new(std::nothrow) SunxiMmcBus(*device);
	if (bus == NULL)
		return B_NO_MEMORY;

	status = bus->InitCheck();
	if (status != B_OK) {
		delete bus;
		return status;
	}

	*_cookie = bus;
	return B_OK;
}


static void
sunxi_mmc_uninit_bus(void* cookie)
{
	delete (SunxiMmcBus*)cookie;
}


static status_t
sunxi_mmc_set_clock(void* cookie, uint32_t kilohertz)
{
	return ((SunxiMmcBus*)cookie)->SetClock(kilohertz);
}


static status_t
sunxi_mmc_execute_command(void* cookie, uint8_t command, uint32_t argument,
	uint32_t* response)
{
	return ((SunxiMmcBus*)cookie)->ExecuteCommand(command, argument, response);
}


static status_t
sunxi_mmc_do_io(void* cookie, uint8_t command, IOOperation* operation,
	bool offsetAsSectors)
{
	return ((SunxiMmcBus*)cookie)->DoIO(command, operation, offsetAsSectors);
}


static void
sunxi_mmc_set_scan_semaphore(void* cookie, sem_id semaphore)
{
	((SunxiMmcBus*)cookie)->SetScanSemaphore(semaphore);
}


static void
sunxi_mmc_set_bus_width(void* cookie, int width)
{
	((SunxiMmcBus*)cookie)->SetBusWidth(width);
}


static void
sunxi_mmc_terminate_bus(void* cookie)
{
	((SunxiMmcBus*)cookie)->Terminate();
}


static void
sunxi_mmc_set_card_type(void* cookie, card_type type)
{
	((SunxiMmcBus*)cookie)->SetCardType(type);
}


static status_t
sunxi_mmc_read_data(void* cookie, uint8_t command, uint32_t argument,
	void* buffer, size_t length)
{
	return ((SunxiMmcBus*)cookie)->ReadData(command, argument, buffer, length);
}


static status_t
sunxi_mmc_set_timing(void* cookie, int timing, uint32_t flags)
{
	return ((SunxiMmcBus*)cookie)->SetTiming(timing, flags);
}


static driver_module_info sSunxiMmcDriver = {
	{
		SUNXI_MMC_DRIVER_MODULE_NAME,
		0,
		NULL
	},
	sunxi_mmc_supports_device,
	sunxi_mmc_register_device,
	sunxi_mmc_init_driver,
	sunxi_mmc_uninit_driver,
	sunxi_mmc_register_child_devices,
	NULL,	// rescan
	NULL,	// device removed
};

static mmc_bus_interface sSunxiMmcBus = {
	.info = {
		.info = {
			.name = SUNXI_MMC_BUS_MODULE_NAME,
		},
		.init_driver = sunxi_mmc_init_bus,
		.uninit_driver = sunxi_mmc_uninit_bus,
	},
	.set_clock = sunxi_mmc_set_clock,
	.execute_command = sunxi_mmc_execute_command,
	.do_io = sunxi_mmc_do_io,
	.set_scan_semaphore = sunxi_mmc_set_scan_semaphore,
	.set_bus_width = sunxi_mmc_set_bus_width,
	.terminate_bus = sunxi_mmc_terminate_bus,
	.set_card_type = sunxi_mmc_set_card_type,
	.read_data = sunxi_mmc_read_data,
	.set_timing = sunxi_mmc_set_timing
};

module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager},
	{}
};

module_info* modules[] = {
	(module_info*)&sSunxiMmcDriver,
	(module_info*)&sSunxiMmcBus,
	NULL
};
