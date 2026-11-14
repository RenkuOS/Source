/*
 * Copyright 2026, Bryan Irvine
 * All rights reserved. Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 *
 * Driver for the Broadcom BCM2711 "EMMC2" SD host controller, which drives
 * the micro SD card slot of the Raspberry Pi 4 and Pi 400. It attaches to
 * the ACPI device (HID "BRCME88C") exposed by the Raspberry Pi 4 UEFI
 * firmware.
 *
 * The controller is SDHCI-like, but:
 *  - it is only safe to access its registers with 32-bit accesses,
 *  - it does not report a usable base clock, so the rate is asked from the
 *    VideoCore firmware through the mailbox,
 *  - DMA on the BCM2711 is not cache coherent.
 * To keep things simple and robust, this driver uses PIO transfers and polls
 * the interrupt status register instead of relying on interrupts.
 */


#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>

#include <ACPI.h>
#include <bus/PCI.h>
#include "acpi.h"

#include <KernelExport.h>
#include <vm/vm.h>

#include "IOSchedulerSimple.h"
#include "mmc.h"


//#define TRACE_EMMC2
#ifdef TRACE_EMMC2
#	define TRACE(x...) dprintf("emmc2: " x)
#else
#	define TRACE(x...) ;
#endif
#define ERROR(x...) dprintf("emmc2: " x)


#define EMMC2_DEVICE_MODULE_NAME "busses/mmc/bcm2711_emmc2/driver_v1"
#define EMMC2_BUS_MODULE_NAME "busses/mmc/bcm2711_emmc2/device/v1"


// #pragma mark - registers


enum {
	REG_ARG2 = 0x00,
	REG_BLKSIZECNT = 0x04,
	REG_ARG1 = 0x08,
	REG_CMDTM = 0x0c,
	REG_RESP0 = 0x10,
	REG_RESP1 = 0x14,
	REG_RESP2 = 0x18,
	REG_RESP3 = 0x1c,
	REG_DATA = 0x20,
	REG_STATUS = 0x24,
	REG_CONTROL0 = 0x28,
	REG_CONTROL1 = 0x2c,
	REG_INTERRUPT = 0x30,
	REG_IRPT_MASK = 0x34,
	REG_IRPT_EN = 0x38,
	REG_CONTROL2 = 0x3c,
	REG_CAPABILITIES = 0x40,
	REG_CAPABILITIES_HI = 0x44,
	REG_SLOTISR_VER = 0xfc,
};

// STATUS (present state)
enum {
	STATUS_CMD_INHIBIT = 1 << 0,
	STATUS_DAT_INHIBIT = 1 << 1,
	STATUS_WRITE_AVAILABLE = 1 << 10,
	STATUS_READ_AVAILABLE = 1 << 11,
	STATUS_CARD_INSERTED = 1 << 16,
};

// CONTROL0: host control (byte 0), power control (byte 1)
enum {
	CONTROL0_4BIT = 1 << 1,
	CONTROL0_HIGH_SPEED = 1 << 2,
	CONTROL0_DMA_MASK = 3 << 3,
	CONTROL0_8BIT = 1 << 5,
	CONTROL0_POWER_ON = 1 << 8,
	CONTROL0_VOLTAGE_MASK = 7 << 9,
	CONTROL0_VOLTAGE_3V3 = 7 << 9,
};

// CONTROL1: clock control (bytes 0-1), timeout (byte 2), reset (byte 3)
enum {
	CONTROL1_CLK_INTERNAL_EN = 1 << 0,
	CONTROL1_CLK_STABLE = 1 << 1,
	CONTROL1_CLK_SD_EN = 1 << 2,
	CONTROL1_CLK_GENSEL = 1 << 5,
	CONTROL1_CLK_FREQ_MASK = 0xffc0,
	CONTROL1_DATA_TIMEOUT_MASK = 0xf << 16,
	CONTROL1_DATA_TIMEOUT_MAX = 0xe << 16,
	CONTROL1_RESET_ALL = 1 << 24,
	CONTROL1_RESET_CMD = 1 << 25,
	CONTROL1_RESET_DATA = 1 << 26,
};

// INTERRUPT
enum {
	INT_CMD_DONE = 1 << 0,
	INT_DATA_DONE = 1 << 1,
	INT_WRITE_READY = 1 << 4,
	INT_READ_READY = 1 << 5,
	INT_CARD_INSERT = 1 << 6,
	INT_CARD_REMOVE = 1 << 7,
	INT_CARD = 1 << 8,
	INT_ERROR = 1 << 15,
	INT_CMD_TIMEOUT = 1 << 16,
	INT_CMD_CRC = 1 << 17,
	INT_CMD_END_BIT = 1 << 18,
	INT_CMD_INDEX = 1 << 19,
	INT_DATA_TIMEOUT = 1 << 20,
	INT_DATA_CRC = 1 << 21,
	INT_DATA_END_BIT = 1 << 22,
	INT_AUTO_CMD = 1 << 24,
	INT_ADMA = 1 << 25,

	INT_CMD_ERRORS = INT_CMD_TIMEOUT | INT_CMD_CRC | INT_CMD_END_BIT
		| INT_CMD_INDEX,
	INT_DATA_ERRORS = INT_DATA_TIMEOUT | INT_DATA_CRC | INT_DATA_END_BIT
		| INT_AUTO_CMD | INT_ADMA,
	INT_ALL_ERRORS = 0xffff0000,
};

// CMDTM: transfer mode (low 16 bits)
enum {
	TM_DMA = 1 << 0,
	TM_BLOCK_COUNT = 1 << 1,
	TM_AUTO_CMD12 = 1 << 2,
	TM_READ = 1 << 4,
	TM_MULTI_BLOCK = 1 << 5,
};

// CMDTM: command (high 16 bits, shifted down here)
enum {
	CMD_RESP_NONE = 0,
	CMD_RESP_136 = 1,
	CMD_RESP_48 = 2,
	CMD_RESP_48_BUSY = 3,
	CMD_CRC_CHECK = 1 << 3,
	CMD_INDEX_CHECK = 1 << 4,
	CMD_DATA = 1 << 5,

	RESP_R0 = CMD_RESP_NONE,
	RESP_R1 = CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK,
	RESP_R1b = CMD_RESP_48_BUSY | CMD_CRC_CHECK | CMD_INDEX_CHECK,
	RESP_R2 = CMD_RESP_136 | CMD_CRC_CHECK,
	RESP_R3 = CMD_RESP_48,
	RESP_R6 = CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK,
	RESP_R7 = CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK,
};


static const uint32 kBlockSize = 512;
static const uint32 kDefaultBaseClock = 100000000;

device_manager_info* gDeviceManager;


// #pragma mark - VideoCore mailbox


static const phys_addr_t kMailboxBase = 0xfe00b880;
static const uint32 kMailboxRead = 0x00;
static const uint32 kMailboxStatus = 0x18;
static const uint32 kMailboxWrite = 0x20;
static const uint32 kMailboxFull = 1u << 31;
static const uint32 kMailboxEmpty = 1u << 30;
static const uint32 kMailboxPropertyChannel = 8;
static const uint32 kVideoCoreBusAlias = 0xc0000000;
static const uint32 kTagGetClockRate = 0x00030002;
static const uint32 kClockIdEmmc2 = 12;


static inline void
clean_invalidate_dcache(void* address, size_t size)
{
#ifdef __aarch64__
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	size_t line = 4 << ((ctr >> 16) & 0xf);
	addr_t start = (addr_t)address & ~(line - 1);
	addr_t end = (addr_t)address + size;
	for (addr_t p = start; p < end; p += line)
		asm volatile("dc civac, %0" : : "r"(p) : "memory");
	asm volatile("dsb sy" : : : "memory");
#endif
}


/*!	Asks the VideoCore firmware for the rate of the EMMC2 clock.
	Returns 0 on failure.
*/
static uint32
mailbox_get_emmc2_clock()
{
	// The firmware can only access the first GB of RAM.
	void* bufferAddress;
	virtual_address_restrictions virtualRestrictions = {};
	virtualRestrictions.address_specification = B_ANY_KERNEL_ADDRESS;
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = 0x40000000;
	area_id bufferArea = create_area_etc(B_SYSTEM_TEAM, "emmc2 mailbox",
		B_PAGE_SIZE, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		0, 0, &virtualRestrictions, &physicalRestrictions, &bufferAddress);
	if (bufferArea < 0)
		return 0;

	physical_entry entry;
	if (get_memory_map(bufferAddress, B_PAGE_SIZE, &entry, 1) != B_OK) {
		delete_area(bufferArea);
		return 0;
	}

	void* mailboxPage;
	area_id mailboxArea = map_physical_memory("videocore mailbox",
		kMailboxBase & ~(B_PAGE_SIZE - 1), B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&mailboxPage);
	if (mailboxArea < 0) {
		delete_area(bufferArea);
		return 0;
	}
	volatile uint32* mailbox = (volatile uint32*)((addr_t)mailboxPage
		+ (kMailboxBase & (B_PAGE_SIZE - 1)));

	volatile uint32* message = (volatile uint32*)bufferAddress;
	message[0] = 8 * 4;				// total size
	message[1] = 0;					// request
	message[2] = kTagGetClockRate;
	message[3] = 8;					// value buffer size
	message[4] = 0;					// request code
	message[5] = kClockIdEmmc2;
	message[6] = 0;
	message[7] = 0;					// end tag
	clean_invalidate_dcache(bufferAddress, B_PAGE_SIZE);

	uint32 rate = 0;
	uint32 busAddress = (uint32)entry.address | kVideoCoreBusAlias;
	bigtime_t timeout = system_time() + 100000;

	// drain stale replies
	while ((mailbox[kMailboxStatus / 4] & kMailboxEmpty) == 0
		&& system_time() < timeout) {
		(void)mailbox[kMailboxRead / 4];
	}
	while ((mailbox[kMailboxStatus / 4] & kMailboxFull) != 0
		&& system_time() < timeout) {
		spin(1);
	}

	if (system_time() < timeout) {
		mailbox[kMailboxWrite / 4] = busAddress | kMailboxPropertyChannel;

		while (system_time() < timeout) {
			if ((mailbox[kMailboxStatus / 4] & kMailboxEmpty) != 0) {
				spin(10);
				continue;
			}
			uint32 reply = mailbox[kMailboxRead / 4];
			if ((reply & 0xf) != kMailboxPropertyChannel)
				continue;

			clean_invalidate_dcache(bufferAddress, B_PAGE_SIZE);
			if (message[1] == 0x80000000 && (message[4] & 0x80000000) != 0
				&& message[5] == kClockIdEmmc2) {
				rate = message[6];
			}
			break;
		}
	}

	delete_area(mailboxArea);
	delete_area(bufferArea);
	return rate;
}


/*!	Only talk to the VideoCore mailbox if the ACPI tables were really made
	by the Raspberry Pi firmware.
*/
static bool
is_raspberry_pi_firmware()
{
	acpi_module_info* acpi;
	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi) != B_OK)
		return false;

	bool result = false;
	ACPI_TABLE_HEADER* header;
	if (acpi->get_table(ACPI_SIG_DSDT, 0, (void**)&header) == B_OK)
		result = strncmp(header->OemId, "RPIFDN", 6) == 0;

	put_module(B_ACPI_MODULE_NAME);
	return result;
}


// #pragma mark - Emmc2Bus


class Emmc2Bus {
public:
							Emmc2Bus(area_id area, addr_t registers);
							~Emmc2Bus();

			status_t		InitCheck() const { return fStatus; }

			status_t		ExecuteCommand(uint8 command, uint32 argument,
								uint32* response);
			status_t		DoIO(uint8 command, IOOperation* operation,
								bool offsetAsSectors);
			void			SetClock(uint32 kilohertz);
			void			SetBusWidth(int width);
			void			SetCardType(card_type type) { fCardType = type; }
			void			SetScanSemaphore(sem_id sem);
			void			Terminate();

private:
	inline	uint32			_Read(uint32 offset)
								{ return fRegisters[offset / 4]; }
	inline	void			_Write(uint32 offset, uint32 value)
								{ fRegisters[offset / 4] = value; }

			status_t		_Reset(uint32 bits);
			status_t		_WaitStatusClear(uint32 bits, bigtime_t timeout);
			status_t		_WaitInterrupt(uint32 bits, bigtime_t timeout,
								uint32* _status);
			uint32			_ResponseType(uint8 command, bool& isData,
								uint32& transferMode);
			status_t		_SendCommand(uint8 command, uint32 argument,
								uint32* response, uint32 blockCount);
			status_t		_TransferBlocks(bool isWrite, phys_addr_t address,
								uint32 count);

private:
			area_id			fArea;
			volatile uint32* fRegisters;
			status_t		fStatus;
			uint32			fBaseClock;
			card_type		fCardType;
			uint32			fBuffer[kBlockSize / 4];
};


Emmc2Bus::Emmc2Bus(area_id area, addr_t registers)
	:
	fArea(area),
	fRegisters((volatile uint32*)registers),
	fStatus(B_NO_INIT),
	fBaseClock(0),
	fCardType(CARD_TYPE_UNKNOWN)
{
	uint32 caps = _Read(REG_CAPABILITIES);
	TRACE("controller version %#" B_PRIx32 ", capabilities %#" B_PRIx32
		" %#" B_PRIx32 "\n", _Read(REG_SLOTISR_VER) >> 16, caps,
		_Read(REG_CAPABILITIES_HI));

	if (is_raspberry_pi_firmware())
		fBaseClock = mailbox_get_emmc2_clock();
	if (fBaseClock == 0)
		fBaseClock = ((caps >> 8) & 0xff) * 1000000;
	if (fBaseClock == 0)
		fBaseClock = kDefaultBaseClock;
	TRACE("base clock %" B_PRIu32 " Hz\n", fBaseClock);

	if (_Reset(CONTROL1_RESET_ALL) != B_OK) {
		ERROR("controller reset timed out\n");
		fStatus = B_TIMED_OUT;
		return;
	}

	// We poll, so enable all status bits but do not signal any interrupt.
	_Write(REG_IRPT_EN, 0);
	_Write(REG_IRPT_MASK, 0xffffffff & ~(INT_CARD | INT_CARD_INSERT
		| INT_CARD_REMOVE));
	_Write(REG_INTERRUPT, 0xffffffff);

	// Power on at 3.3V, no DMA, 1 bit bus
	uint32 control0 = _Read(REG_CONTROL0);
	control0 &= ~(CONTROL0_4BIT | CONTROL0_8BIT | CONTROL0_HIGH_SPEED
		| CONTROL0_DMA_MASK | CONTROL0_VOLTAGE_MASK);
	control0 |= CONTROL0_VOLTAGE_3V3;
	_Write(REG_CONTROL0, control0);
	_Write(REG_CONTROL0, control0 | CONTROL0_POWER_ON);
	snooze(10000);

	SetClock(400);

	TRACE("status after init %#" B_PRIx32 "\n", _Read(REG_STATUS));
	fStatus = B_OK;
}


Emmc2Bus::~Emmc2Bus()
{
	Terminate();
	delete_area(fArea);
}


status_t
Emmc2Bus::_Reset(uint32 bits)
{
	_Write(REG_CONTROL1, _Read(REG_CONTROL1) | bits);
	bigtime_t timeout = system_time() + 100000;
	while ((_Read(REG_CONTROL1) & bits) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		spin(10);
	}
	return B_OK;
}


status_t
Emmc2Bus::_WaitStatusClear(uint32 bits, bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	while ((_Read(REG_STATUS) & bits) != 0) {
		if (system_time() > end)
			return B_TIMED_OUT;
		spin(10);
	}
	return B_OK;
}


/*!	Polls the interrupt status register until one of \a bits, or an error,
	is signalled. The bits found are cleared and returned in \a _status.
*/
status_t
Emmc2Bus::_WaitInterrupt(uint32 bits, bigtime_t timeout, uint32* _status)
{
	bigtime_t end = system_time() + timeout;
	int32 spins = 0;
	while (true) {
		uint32 status = _Read(REG_INTERRUPT);
		if ((status & (bits | INT_ERROR | INT_ALL_ERRORS)) != 0) {
			_Write(REG_INTERRUPT, status & (bits | INT_ERROR | INT_ALL_ERRORS));
			*_status = status;
			if ((status & (INT_ERROR | INT_ALL_ERRORS)) != 0)
				return B_ERROR;
			return B_OK;
		}
		if (system_time() > end) {
			*_status = status;
			return B_TIMED_OUT;
		}
		if (++spins < 1000)
			spin(1);
		else
			snooze(100);
	}
}


uint32
Emmc2Bus::_ResponseType(uint8 command, bool& isData, uint32& transferMode)
{
	bool isMMC = fCardType == CARD_TYPE_MMC
		|| fCardType == CARD_TYPE_MMC_EXTENDED_CAPACITY;
	isData = false;
	transferMode = 0;

	switch (command) {
		case GO_IDLE_STATE:
			return RESP_R0;
		case ALL_SEND_CID:
		case SEND_CSD:
			return RESP_R2;
		case MMC_SEND_OP_COND:
		case SD_SEND_OP_COND:
			return RESP_R3;
		case SELECT_DESELECT_CARD:
		case SD_ERASE:
		case SD_STOP_TRANSMISSION:
			return RESP_R1b;
		case SD_SET_BUS_WIDTH:
			return isMMC ? RESP_R1b : RESP_R1;
		case SD_SEND_RELATIVE_ADDR:
			return isMMC ? RESP_R1 : RESP_R6;
		case SD_SEND_IF_COND:
			return isMMC ? RESP_R1 : RESP_R7;
		case SD_READ_SINGLE_BLOCK:
			isData = true;
			transferMode = TM_READ;
			return RESP_R1 | CMD_DATA;
		case SD_READ_MULTIPLE_BLOCKS:
			isData = true;
			transferMode = TM_READ | TM_MULTI_BLOCK | TM_BLOCK_COUNT
				| TM_AUTO_CMD12;
			return RESP_R1 | CMD_DATA;
		case SD_WRITE_SINGLE_BLOCK:
			isData = true;
			return RESP_R1 | CMD_DATA;
		case SD_WRITE_MULTIPLE_BLOCKS:
			isData = true;
			transferMode = TM_MULTI_BLOCK | TM_BLOCK_COUNT | TM_AUTO_CMD12;
			return RESP_R1 | CMD_DATA;
		case SD_APP_CMD:
		case SD_ERASE_WR_BLK_START:
		case SD_ERASE_WR_BLK_END:
		default:
			return RESP_R1;
	}
}


status_t
Emmc2Bus::_SendCommand(uint8 command, uint32 argument, uint32* response,
	uint32 blockCount)
{
	bool isData;
	uint32 transferMode;
	uint32 type = _ResponseType(command, isData, transferMode);

	uint32 inhibit = STATUS_CMD_INHIBIT;
	if (isData || (type & 3) == CMD_RESP_48_BUSY)
		inhibit |= STATUS_DAT_INHIBIT;
	if (_WaitStatusClear(inhibit, 500000) != B_OK) {
		ERROR("command %u: controller busy (status %#" B_PRIx32 ")\n",
			command, _Read(REG_STATUS));
		_Reset(CONTROL1_RESET_CMD | CONTROL1_RESET_DATA);
		return B_BUSY;
	}

	_Write(REG_INTERRUPT, 0xffffffff);

	if (isData)
		_Write(REG_BLKSIZECNT, (blockCount << 16) | kBlockSize);

	_Write(REG_ARG1, argument);
	_Write(REG_CMDTM, ((uint32)command << 24) | (type << 16) | transferMode);

	uint32 status;
	status_t result = _WaitInterrupt(INT_CMD_DONE, 1000000, &status);
	if (result != B_OK) {
		bool timeout = result == B_TIMED_OUT
			|| (status & INT_CMD_TIMEOUT) != 0;
		// CMD8 and CMD1 legitimately time out on some cards while probing
		if (command != SD_SEND_IF_COND && command != MMC_SEND_OP_COND) {
			ERROR("command %u (arg %#" B_PRIx32 ") failed, interrupt status %#"
				B_PRIx32 "\n", command, argument, status);
		}
		_Reset(CONTROL1_RESET_CMD | CONTROL1_RESET_DATA);
		_Write(REG_INTERRUPT, 0xffffffff);
		return timeout ? B_TIMED_OUT : B_IO_ERROR;
	}

	if (response != NULL) {
		switch (type & 3) {
			case CMD_RESP_136:
				response[0] = _Read(REG_RESP0);
				response[1] = _Read(REG_RESP1);
				response[2] = _Read(REG_RESP2);
				response[3] = _Read(REG_RESP3);
				break;
			case CMD_RESP_48:
			case CMD_RESP_48_BUSY:
				response[0] = _Read(REG_RESP0);
				break;
		}
	}

	if ((type & 3) == CMD_RESP_48_BUSY && !isData) {
		// Wait for the card to release the data line.
		result = _WaitInterrupt(INT_DATA_DONE, 3000000, &status);
		if (result != B_OK && _WaitStatusClear(STATUS_DAT_INHIBIT, 100000)
				!= B_OK) {
			ERROR("command %u: busy wait failed, status %#" B_PRIx32 "\n",
				command, status);
			_Reset(CONTROL1_RESET_DATA);
			return B_TIMED_OUT;
		}
	}

	return B_OK;
}


status_t
Emmc2Bus::ExecuteCommand(uint8 command, uint32 argument, uint32* response)
{
	return _SendCommand(command, argument, response, 0);
}


status_t
Emmc2Bus::_TransferBlocks(bool isWrite, phys_addr_t address, uint32 count)
{
	uint32 status;
	for (uint32 block = 0; block < count; block++) {
		status_t result = _WaitInterrupt(
			isWrite ? INT_WRITE_READY : INT_READ_READY, 1000000, &status);
		if (result != B_OK) {
			ERROR("%s: block %" B_PRIu32 "/%" B_PRIu32 " failed, status %#"
				B_PRIx32 "\n", isWrite ? "write" : "read", block, count,
				status);
			return result == B_TIMED_OUT ? B_TIMED_OUT : B_IO_ERROR;
		}

		phys_addr_t blockAddress = address + block * kBlockSize;
		if (isWrite) {
			vm_memcpy_from_physical(fBuffer, blockAddress, kBlockSize, false);
			for (uint32 i = 0; i < kBlockSize / 4; i++)
				_Write(REG_DATA, fBuffer[i]);
		} else {
			for (uint32 i = 0; i < kBlockSize / 4; i++)
				fBuffer[i] = _Read(REG_DATA);
			vm_memcpy_to_physical(blockAddress, fBuffer, kBlockSize, false);
		}
	}

	status_t result = _WaitInterrupt(INT_DATA_DONE, 3000000, &status);
	if (result != B_OK) {
		ERROR("transfer did not complete, status %#" B_PRIx32 "\n", status);
		return result == B_TIMED_OUT ? B_TIMED_OUT : B_IO_ERROR;
	}
	return B_OK;
}


status_t
Emmc2Bus::DoIO(uint8 command, IOOperation* operation, bool offsetAsSectors)
{
	bool isWrite = operation->IsWrite();
	off_t offset = operation->Offset();
	generic_size_t length = operation->Length();

	if (offset % kBlockSize != 0 || length % kBlockSize != 0)
		return B_BAD_VALUE;

	// The upper layers pick single/multiple block commands; we only need to
	// honour the command for each contiguous piece.
	bool multiple = command == SD_READ_MULTIPLE_BLOCKS
		|| command == SD_WRITE_MULTIPLE_BLOCKS;

	const generic_io_vec* vecs = operation->Vecs();
	generic_size_t vecOffset = 0;

	while (length > 0) {
		generic_size_t toTransfer = std::min(length, vecs->length - vecOffset);
		if (toTransfer == 0) {
			vecs++;
			vecOffset = 0;
			continue;
		}
		if (toTransfer % kBlockSize != 0)
			return B_BAD_VALUE;

		uint32 blocks = toTransfer / kBlockSize;
		if (blocks > 0xffff)
			blocks = 0xffff;
		if (!multiple)
			blocks = 1;
		toTransfer = blocks * kBlockSize;

		uint32 argument = offsetAsSectors ? offset / kBlockSize : offset;
		uint8 thisCommand = command;
		if (multiple && blocks == 1) {
			thisCommand = isWrite ? SD_WRITE_SINGLE_BLOCK
				: SD_READ_SINGLE_BLOCK;
		}

		uint32 response;
		status_t result = _SendCommand(thisCommand, argument, &response,
			blocks);
		if (result == B_OK)
			result = _TransferBlocks(isWrite, vecs->base + vecOffset, blocks);
		if (result != B_OK) {
			_Reset(CONTROL1_RESET_CMD | CONTROL1_RESET_DATA);
			_Write(REG_INTERRUPT, 0xffffffff);
			return result;
		}

		length -= toTransfer;
		vecOffset += toTransfer;
		offset += toTransfer;
	}

	return B_OK;
}


void
Emmc2Bus::SetClock(uint32 kilohertz)
{
	uint32 target = kilohertz * 1000;
	uint32 divider = 0;
	if (target < fBaseClock) {
		divider = (fBaseClock + 2 * target - 1) / (2 * target);
		if (divider > 0x3ff)
			divider = 0x3ff;
	}

	uint32 control1 = _Read(REG_CONTROL1);
	control1 &= ~(CONTROL1_CLK_SD_EN | CONTROL1_RESET_ALL | CONTROL1_RESET_CMD
		| CONTROL1_RESET_DATA);
	_Write(REG_CONTROL1, control1);
	snooze(10);

	control1 &= ~(CONTROL1_CLK_FREQ_MASK | CONTROL1_CLK_GENSEL
		| CONTROL1_DATA_TIMEOUT_MASK);
	control1 |= ((divider & 0xff) << 8) | (((divider >> 8) & 3) << 6)
		| CONTROL1_CLK_INTERNAL_EN | CONTROL1_DATA_TIMEOUT_MAX;
	_Write(REG_CONTROL1, control1);

	bigtime_t timeout = system_time() + 100000;
	while ((_Read(REG_CONTROL1) & CONTROL1_CLK_STABLE) == 0) {
		if (system_time() > timeout) {
			ERROR("clock did not become stable\n");
			break;
		}
		spin(10);
	}

	_Write(REG_CONTROL1, _Read(REG_CONTROL1) | CONTROL1_CLK_SD_EN);
	snooze(1000);

	TRACE("clock set to %" B_PRIu32 " kHz (divider %" B_PRIu32 ", actual %"
		B_PRIu32 " kHz)\n", kilohertz, divider,
		(divider == 0 ? fBaseClock : fBaseClock / (2 * divider)) / 1000);
}


void
Emmc2Bus::SetBusWidth(int width)
{
	uint32 control0 = _Read(REG_CONTROL0) & ~(CONTROL0_4BIT | CONTROL0_8BIT);
	if (width == 4)
		control0 |= CONTROL0_4BIT;
	else if (width == 8)
		control0 |= CONTROL0_8BIT;
	_Write(REG_CONTROL0, control0);
	TRACE("bus width %d\n", width);
}


void
Emmc2Bus::SetScanSemaphore(sem_id sem)
{
	// The card slot of the Pi 4 has no usable card detect, and we were
	// booted from it: always scan.
	release_sem(sem);
}


void
Emmc2Bus::Terminate()
{
	_Write(REG_CONTROL1, _Read(REG_CONTROL1) & ~CONTROL1_CLK_SD_EN);
}


// #pragma mark - device manager glue


struct emmc2_crs {
	phys_addr_t	address;
	size_t		length;
};


static acpi_status
emmc2_parse_crs(ACPI_RESOURCE* resource, void* context)
{
	emmc2_crs* crs = (emmc2_crs*)context;
	switch (resource->Type) {
		case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
			if (crs->address == 0) {
				crs->address = resource->Data.FixedMemory32.Address;
				crs->length = resource->Data.FixedMemory32.AddressLength;
			}
			break;
		case ACPI_RESOURCE_TYPE_MEMORY32:
			if (crs->address == 0) {
				crs->address = resource->Data.Memory32.Minimum;
				crs->length = resource->Data.Memory32.AddressLength;
			}
			break;
	}
	return B_OK;
}


// Test hook: define PI_QEMU_TEST to also drive a PCI SDHCI (e.g. QEMU's
// sdhci-pci) with this driver, to exercise it without Raspberry Pi hardware.
//#define PI_QEMU_TEST


static float
supports_device(device_node* parent)
{
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK) {
		return 0.0f;
	}
#ifdef PI_QEMU_TEST
	if (strcmp(bus, "pci") == 0) {
		uint16 type, subType;
		if (gDeviceManager->get_attr_uint16(parent, B_DEVICE_TYPE, &type,
				false) == B_OK
			&& gDeviceManager->get_attr_uint16(parent, B_DEVICE_SUB_TYPE,
				&subType, false) == B_OK
			&& type == PCI_base_peripheral && subType == PCI_sd_host) {
			TRACE("test mode: using PCI SD host controller\n");
			return 1.0f;
		}
		return 0.0f;
	}
#endif
	if (strcmp(bus, "acpi") != 0)
		return 0.0f;

	uint32 type;
	if (gDeviceManager->get_attr_uint32(parent, ACPI_DEVICE_TYPE_ITEM, &type,
			false) != B_OK || type != ACPI_TYPE_DEVICE) {
		return 0.0f;
	}

	const char* hid;
	if (gDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK || strcmp(hid, "BRCME88C") != 0) {
		return 0.0f;
	}

	TRACE("found BCM2711 EMMC2 controller\n");
	return 0.9f;
}


static status_t
register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "BCM2711 EMMC2 SD Host Controller" }},
		{}
	};

	return gDeviceManager->register_node(parent, EMMC2_DEVICE_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
init_device(device_node* node, void** _cookie)
{
	*_cookie = node;
	return B_OK;
}


static void
uninit_device(void* cookie)
{
}


static status_t
register_child_devices(void* cookie)
{
	device_node* node = (device_node*)cookie;

	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "SD bus" }},
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{ .string = MMC_BUS_MODULE_NAME }},
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "mmc" }},

		// Transfers are done by PIO one sector at a time, but the layers
		// above still hand us physical vectors: keep them sector aligned.
		{ B_DMA_ALIGNMENT, B_UINT32_TYPE, { .ui32 = kBlockSize - 1 }},
		{ B_DMA_MAX_SEGMENT_COUNT, B_UINT32_TYPE, { .ui32 = 1 }},
		{ B_DMA_MAX_SEGMENT_BLOCKS, B_UINT32_TYPE, { .ui32 = (1 << 10) - 1 }},
		{ NULL }
	};

	return gDeviceManager->register_node(node, EMMC2_BUS_MODULE_NAME, attrs,
		NULL, NULL);
}


static status_t
init_bus(device_node* node, void** _cookie)
{
	// node -> our driver node -> ACPI device node
	device_node* parent = gDeviceManager->get_parent_node(node);
	device_node* busNode = gDeviceManager->get_parent_node(parent);
	gDeviceManager->put_node(parent);

	emmc2_crs crs = {};
	const char* busName = NULL;
	gDeviceManager->get_attr_string(busNode, B_DEVICE_BUS, &busName, false);
#ifdef PI_QEMU_TEST
	if (busName != NULL && strcmp(busName, "pci") == 0) {
		pci_device_module_info* pci;
		pci_device* device;
		gDeviceManager->get_driver(busNode, (driver_module_info**)&pci,
			(void**)&device);
		pci_info info;
		pci->get_pci_info(device, &info);
		crs.address = info.u.h0.base_registers[0];
		crs.length = info.u.h0.base_register_sizes[0];
		uint16 command = pci->read_pci_config(device, PCI_command, 2);
		pci->write_pci_config(device, PCI_command, 2,
			command | PCI_command_memory);
		busName = NULL;
	}
#endif
	if (busName != NULL) {
		acpi_device_module_info* acpi;
		acpi_device device;
		gDeviceManager->get_driver(busNode, (driver_module_info**)&acpi,
			(void**)&device);
		if (acpi->walk_resources(device, (ACPI_STRING)"_CRS", emmc2_parse_crs,
				&crs) != B_OK) {
			crs.address = 0;
		}
	}
	gDeviceManager->put_node(busNode);

	if (crs.address == 0) {
		ERROR("no register resource\n");
		return B_DEVICE_NOT_FOUND;
	}
	if (crs.length < 0x100)
		crs.length = 0x100;
	TRACE("registers at %#" B_PRIxPHYSADDR ", length %#" B_PRIxSIZE "\n",
		crs.address, crs.length);

	void* registers;
	area_id area = map_physical_memory("emmc2 registers", crs.address,
		(crs.length + B_PAGE_SIZE - 1) & ~(size_t)(B_PAGE_SIZE - 1), B_ANY_KERNEL_BLOCK_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &registers);
	if (area < 0)
		return area;

	Emmc2Bus* bus = new(std::nothrow) Emmc2Bus(area, (addr_t)registers);
	if (bus == NULL) {
		delete_area(area);
		return B_NO_MEMORY;
	}
	status_t status = bus->InitCheck();
	if (status != B_OK) {
		delete bus;
		return status;
	}

	*_cookie = bus;
	return B_OK;
}


static void
uninit_bus(void* cookie)
{
	delete (Emmc2Bus*)cookie;
}


static void
bus_removed(void* cookie)
{
}


static status_t
set_clock(void* controller, uint32_t kilohertz)
{
	((Emmc2Bus*)controller)->SetClock(kilohertz);
	return B_OK;
}


static status_t
execute_command(void* controller, uint8_t command, uint32_t argument,
	uint32_t* response)
{
	return ((Emmc2Bus*)controller)->ExecuteCommand(command, argument,
		response);
}


static status_t
do_io(void* controller, uint8_t command, IOOperation* operation,
	bool offsetAsSectors)
{
	return ((Emmc2Bus*)controller)->DoIO(command, operation, offsetAsSectors);
}


static void
set_scan_semaphore(void* controller, sem_id sem)
{
	((Emmc2Bus*)controller)->SetScanSemaphore(sem);
}


static void
set_bus_width(void* controller, int width)
{
	((Emmc2Bus*)controller)->SetBusWidth(width);
}


static void
terminate_bus(void* controller)
{
	((Emmc2Bus*)controller)->Terminate();
}


static void
set_card_type(void* controller, card_type type)
{
	((Emmc2Bus*)controller)->SetCardType(type);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{}
};


static driver_module_info sEmmc2Device = {
	{
		EMMC2_DEVICE_MODULE_NAME,
		0,
		NULL
	},
	supports_device,
	register_device,
	init_device,
	uninit_device,
	register_child_devices,
	NULL,	// rescan
	NULL,	// device removed
};


static mmc_bus_interface sEmmc2BusModule = {
	.info = {
		.info = {
			.name = EMMC2_BUS_MODULE_NAME,
		},
		.init_driver = init_bus,
		.uninit_driver = uninit_bus,
		.device_removed = bus_removed,
	},

	.set_clock = set_clock,
	.execute_command = execute_command,
	.do_io = do_io,
	.set_scan_semaphore = set_scan_semaphore,
	.set_bus_width = set_bus_width,
	.terminate_bus = terminate_bus,
	.set_card_type = set_card_type,
};


module_info* modules[] = {
	(module_info*)&sEmmc2Device,
	(module_info*)&sEmmc2BusModule,
	NULL
};
