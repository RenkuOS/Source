/*
 * Copyright 2026, Bryan Irvine
 * All rights reserved. Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 *
 * SDIO host side for the "Arasan" SDHCI controller of the BCM2835 family
 * (ACPI HID "BCM2847"), which is wired to the WLAN chip on the Raspberry Pi
 * 3 and 4.
 *
 * Controller quirks (see Linux' sdhci-iproc.c):
 *  - only 32-bit register accesses work,
 *  - successive register writes closer than two SD clock cycles to each other
 *    may be lost, which matters at identification clock speeds.
 * All transfers are done by PIO and completion is polled; only the SDIO card
 * interrupt is used as an interrupt.
 */


#include "brcmfmac.h"

#include <string.h>


enum {
	REG_ARG2			= 0x00,
	REG_BLKSIZECNT		= 0x04,
	REG_ARG1			= 0x08,
	REG_CMDTM			= 0x0c,
	REG_RESP0			= 0x10,
	REG_RESP1			= 0x14,
	REG_RESP2			= 0x18,
	REG_RESP3			= 0x1c,
	REG_DATA			= 0x20,
	REG_STATUS			= 0x24,
	REG_CONTROL0		= 0x28,
	REG_CONTROL1		= 0x2c,
	REG_INTERRUPT		= 0x30,
	REG_IRPT_MASK		= 0x34,
	REG_IRPT_EN			= 0x38,
	REG_CONTROL2		= 0x3c,
	REG_CAPABILITIES	= 0x40,
	REG_SLOTISR_VER		= 0xfc,
};

enum {
	STATUS_CMD_INHIBIT	= 1 << 0,
	STATUS_DAT_INHIBIT	= 1 << 1,
};

enum {
	CONTROL0_4BIT			= 1 << 1,
	CONTROL0_HIGH_SPEED		= 1 << 2,
	CONTROL0_DMA_MASK		= 3 << 3,
	CONTROL0_8BIT			= 1 << 5,
	CONTROL0_POWER_ON		= 1 << 8,
	CONTROL0_VOLTAGE_MASK	= 7 << 9,
	CONTROL0_VOLTAGE_3V3	= 7 << 9,
};

enum {
	CONTROL1_CLK_INTERNAL_EN	= 1 << 0,
	CONTROL1_CLK_STABLE			= 1 << 1,
	CONTROL1_CLK_SD_EN			= 1 << 2,
	CONTROL1_CLK_GENSEL			= 1 << 5,
	CONTROL1_CLK_FREQ_MASK		= 0xffc0,
	CONTROL1_DATA_TIMEOUT_MASK	= 0xf << 16,
	CONTROL1_DATA_TIMEOUT_MAX	= 0xe << 16,
	CONTROL1_RESET_ALL			= 1 << 24,
	CONTROL1_RESET_CMD			= 1 << 25,
	CONTROL1_RESET_DATA			= 1 << 26,
};

enum {
	INT_CMD_DONE		= 1 << 0,
	INT_DATA_DONE		= 1 << 1,
	INT_WRITE_READY		= 1 << 4,
	INT_READ_READY		= 1 << 5,
	INT_CARD_INSERT		= 1 << 6,
	INT_CARD_REMOVE		= 1 << 7,
	INT_CARD			= 1 << 8,
	INT_ERROR			= 1 << 15,
	INT_CMD_TIMEOUT		= 1 << 16,
	INT_ALL_ERRORS		= 0xffff0000,
};

enum {
	TM_BLOCK_COUNT		= 1 << 1,
	TM_READ				= 1 << 4,
	TM_MULTI_BLOCK		= 1 << 5,
};

enum {
	CMD_RESP_NONE		= 0,
	CMD_RESP_136		= 1,
	CMD_RESP_48			= 2,
	CMD_RESP_48_BUSY	= 3,
	CMD_CRC_CHECK		= 1 << 3,
	CMD_INDEX_CHECK		= 1 << 4,
	CMD_DATA			= 1 << 5,
};

// R5 response flags that indicate an error
static const uint32 kR5ErrorMask = 0xcf00;


SdioHost::SdioHost(addr_t registers, uint32 baseClock)
	:
	fRegisters((volatile uint32*)registers),
	fBaseClock(baseClock),
	fClock(0),
	fVersion(0)
{
	for (int i = 0; i < 8; i++)
		fBlockSize[i] = 64;
	B_INITIALIZE_SPINLOCK(&fInterruptLock);
}


void
SdioHost::_Write(uint32 offset, uint32 value)
{
	fRegisters[offset / 4] = value;
	// Writes closer than two SD clocks to each other can get lost.
	if (fClock <= 400000)
		spin(10);
	else
		spin(1);
}


status_t
SdioHost::Reset()
{
	fVersion = _Read(REG_SLOTISR_VER) >> 16;
	fClock = 0;

	_Write(REG_CONTROL1, _Read(REG_CONTROL1) | CONTROL1_RESET_ALL);
	bigtime_t timeout = system_time() + 100000;
	while ((_Read(REG_CONTROL1) & CONTROL1_RESET_ALL) != 0) {
		if (system_time() > timeout) {
			ERROR("SDIO host reset timed out\n");
			return B_TIMED_OUT;
		}
		spin(10);
	}

	// We poll: report all status bits, but signal nothing (the card
	// interrupt is enabled separately).
	_Write(REG_IRPT_EN, 0);
	_Write(REG_IRPT_MASK, ~(uint32)(INT_CARD | INT_CARD_INSERT
		| INT_CARD_REMOVE));
	_Write(REG_INTERRUPT, 0xffffffff);

	uint32 control0 = _Read(REG_CONTROL0);
	control0 &= ~(CONTROL0_4BIT | CONTROL0_8BIT | CONTROL0_HIGH_SPEED
		| CONTROL0_DMA_MASK | CONTROL0_VOLTAGE_MASK);
	control0 |= CONTROL0_VOLTAGE_3V3;
	_Write(REG_CONTROL0, control0);
	_Write(REG_CONTROL0, control0 | CONTROL0_POWER_ON);
	snooze(10000);

	SetClock(400000);
	return B_OK;
}


void
SdioHost::SetClock(uint32 hertz)
{
	uint32 divider = 0;
	if (hertz < fBaseClock) {
		divider = (fBaseClock + 2 * hertz - 1) / (2 * hertz);
		if (divider > 0x3ff)
			divider = 0x3ff;
	}

	// switch to slow write pacing while changing the clock
	fClock = 0;

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
			ERROR("SDIO clock did not become stable\n");
			break;
		}
		spin(10);
	}

	_Write(REG_CONTROL1, _Read(REG_CONTROL1) | CONTROL1_CLK_SD_EN);
	snooze(1000);

	fClock = divider == 0 ? fBaseClock : fBaseClock / (2 * divider);
	TRACE("SDIO clock %" B_PRIu32 " Hz (divider %" B_PRIu32 ")\n", fClock,
		divider);
}


void
SdioHost::SetBusWidth(int width)
{
	uint32 control0 = _Read(REG_CONTROL0) & ~(CONTROL0_4BIT | CONTROL0_8BIT);
	if (width == 4)
		control0 |= CONTROL0_4BIT;
	_Write(REG_CONTROL0, control0);
}


void
SdioHost::_ResetLines(uint32 bits)
{
	_Write(REG_CONTROL1, _Read(REG_CONTROL1) | bits);
	bigtime_t timeout = system_time() + 100000;
	while ((_Read(REG_CONTROL1) & bits) != 0) {
		if (system_time() > timeout)
			break;
		spin(10);
	}
	_Write(REG_INTERRUPT, 0xffffffff);
}


status_t
SdioHost::_WaitStatusClear(uint32 bits, bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	while ((_Read(REG_STATUS) & bits) != 0) {
		if (system_time() > end)
			return B_TIMED_OUT;
		spin(5);
	}
	return B_OK;
}


status_t
SdioHost::_WaitInterrupt(uint32 bits, bigtime_t timeout, uint32* _status)
{
	bigtime_t end = system_time() + timeout;
	int32 spins = 0;
	while (true) {
		uint32 status = _Read(REG_INTERRUPT);
		uint32 wanted = bits | INT_ERROR | INT_ALL_ERRORS;
		if ((status & wanted) != 0) {
			_Write(REG_INTERRUPT, status & wanted);
			*_status = status;
			if ((status & (INT_ERROR | INT_ALL_ERRORS)) != 0)
				return B_IO_ERROR;
			return B_OK;
		}
		if (system_time() > end) {
			*_status = status;
			return B_TIMED_OUT;
		}
		if (++spins < 2000)
			spin(1);
		else
			snooze(50);
	}
}


status_t
SdioHost::_SendCommand(uint8 index, uint32 argument, uint32 responseType,
	bool data, bool read, uint32 blockSize, uint32 blockCount,
	uint32* response)
{
	uint32 command;
	switch (responseType) {
		case SDIO_RESP_NONE:
			command = CMD_RESP_NONE;
			break;
		case SDIO_RESP_R1B:
			command = CMD_RESP_48_BUSY | CMD_CRC_CHECK | CMD_INDEX_CHECK;
			break;
		case SDIO_RESP_R4:
			command = CMD_RESP_48;
			break;
		case SDIO_RESP_R1:
		default:
			command = CMD_RESP_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK;
			break;
	}

	uint32 transferMode = 0;
	if (data) {
		command |= CMD_DATA;
		if (read)
			transferMode |= TM_READ;
		if (blockCount > 1)
			transferMode |= TM_MULTI_BLOCK | TM_BLOCK_COUNT;
	}

	uint32 inhibit = STATUS_CMD_INHIBIT;
	if (data || responseType == SDIO_RESP_R1B)
		inhibit |= STATUS_DAT_INHIBIT;
	if (_WaitStatusClear(inhibit, 500000) != B_OK) {
		ERROR("CMD%u: controller busy (status %#" B_PRIx32 ")\n", index,
			_Read(REG_STATUS));
		_ResetLines(CONTROL1_RESET_CMD | CONTROL1_RESET_DATA);
		if (_WaitStatusClear(inhibit, 100000) != B_OK)
			return B_BUSY;
	}

	_Write(REG_INTERRUPT, 0xffffffff & ~(uint32)INT_CARD);

	if (data)
		_Write(REG_BLKSIZECNT, (blockCount << 16) | blockSize);
	_Write(REG_ARG1, argument);
	_Write(REG_CMDTM, ((uint32)index << 24) | (command << 16) | transferMode);

	uint32 status;
	status_t result = _WaitInterrupt(INT_CMD_DONE, 1000000, &status);
	if (result != B_OK) {
		// CMD5 legitimately times out while the chip is still powering up
		if (index != 5) {
			ERROR("CMD%u (arg %#" B_PRIx32 ") failed: %s, interrupt %#"
				B_PRIx32 "\n", index, argument, strerror(result), status);
		}
		_ResetLines(CONTROL1_RESET_CMD | CONTROL1_RESET_DATA);
		return (status & INT_CMD_TIMEOUT) != 0 ? B_TIMED_OUT : result;
	}

	if (response != NULL)
		*response = _Read(REG_RESP0);

	if (responseType == SDIO_RESP_R1B && !data) {
		result = _WaitInterrupt(INT_DATA_DONE, 1000000, &status);
		if (result != B_OK
			&& _WaitStatusClear(STATUS_DAT_INHIBIT, 100000) != B_OK) {
			ERROR("CMD%u: busy wait failed, interrupt %#" B_PRIx32 "\n", index,
				status);
			_ResetLines(CONTROL1_RESET_DATA);
			return B_TIMED_OUT;
		}
	}

	return B_OK;
}


status_t
SdioHost::_Transfer(bool read, uint8* buffer, uint32 blockSize,
	uint32 blockCount)
{
	uint32 status;
	for (uint32 block = 0; block < blockCount; block++) {
		status_t result = _WaitInterrupt(read ? INT_READ_READY : INT_WRITE_READY,
			1000000, &status);
		if (result != B_OK) {
			ERROR("%s block %" B_PRIu32 "/%" B_PRIu32 " (size %" B_PRIu32
				") failed, interrupt %#" B_PRIx32 "\n", read ? "read" : "write",
				block, blockCount, blockSize, status);
			_ResetLines(CONTROL1_RESET_CMD | CONTROL1_RESET_DATA);
			return result;
		}

		uint8* data = buffer + block * blockSize;
		if (read) {
			for (uint32 i = 0; i < blockSize; i += 4) {
				uint32 word = fRegisters[REG_DATA / 4];
				memcpy(data + i, &word, 4);
			}
		} else {
			for (uint32 i = 0; i < blockSize; i += 4) {
				uint32 word;
				memcpy(&word, data + i, 4);
				_WriteData(word);
			}
		}
	}

	status_t result = _WaitInterrupt(INT_DATA_DONE, 1000000, &status);
	if (result != B_OK) {
		ERROR("%s transfer did not complete, interrupt %#" B_PRIx32 "\n",
			read ? "read" : "write", status);
		_ResetLines(CONTROL1_RESET_CMD | CONTROL1_RESET_DATA);
		return result;
	}
	return B_OK;
}


status_t
SdioHost::Command(uint8 index, uint32 argument, uint32 responseType,
	uint32* response)
{
	return _SendCommand(index, argument, responseType, false, false, 0, 0,
		response);
}


status_t
SdioHost::ReadDirect(int function, uint32 address, uint8* _value)
{
	uint32 response;
	status_t status = _SendCommand(52, ((uint32)(function & 7) << 28)
		| ((address & 0x1ffff) << 9), SDIO_RESP_R1, false, false, 0, 0,
		&response);
	if (status != B_OK)
		return status;
	if ((response & kR5ErrorMask) != 0) {
		ERROR("CMD52 read F%d %#" B_PRIx32 ": response %#" B_PRIx32 "\n",
			function, address, response);
		return B_IO_ERROR;
	}
	*_value = response & 0xff;
	return B_OK;
}


status_t
SdioHost::WriteDirect(int function, uint32 address, uint8 value)
{
	uint32 response = 0;
	status_t status = B_ERROR;
	for (int retry = 0; retry < 3; retry++) {
		status = _SendCommand(52, (1u << 31) | ((uint32)(function & 7) << 28)
			| ((address & 0x1ffff) << 9) | value, SDIO_RESP_R1, false, false,
			0, 0, &response);
		if (status == B_OK && (response & kR5ErrorMask) == 0)
			return B_OK;
	}
	ERROR("CMD52 write F%d %#" B_PRIx32 " = %#x failed: %s, response %#"
		B_PRIx32 "\n", function, address, value, strerror(status), response);
	return status != B_OK ? status : B_IO_ERROR;
}


status_t
SdioHost::_ExtendedChunk(bool write, int function, uint32 address,
	bool increment, bool blockMode, uint8* buffer, uint32 count,
	uint32 blockSize)
{
	uint32 argument = (write ? 1u << 31 : 0) | ((uint32)(function & 7) << 28)
		| (blockMode ? 1u << 27 : 0) | (increment ? 1u << 26 : 0)
		| ((address & 0x1ffff) << 9) | (count & 0x1ff);

	uint32 hostBlockSize = blockMode ? blockSize : count;
	uint32 hostBlockCount = blockMode ? count : 1;

	uint32 response;
	status_t status = _SendCommand(53, argument, SDIO_RESP_R1, true, !write,
		hostBlockSize, hostBlockCount, &response);
	if (status != B_OK)
		return status;
	if ((response & kR5ErrorMask) != 0) {
		ERROR("CMD53 %s F%d %#" B_PRIx32 ": response %#" B_PRIx32 "\n",
			write ? "write" : "read", function, address, response);
		_ResetLines(CONTROL1_RESET_CMD | CONTROL1_RESET_DATA);
		return B_IO_ERROR;
	}

	return _Transfer(!write, buffer, hostBlockSize, hostBlockCount);
}


/*!	CMD53 transfer. \a length must be a multiple of 4. Large transfers are
	split into block mode transfers plus a byte mode remainder.
*/
status_t
SdioHost::Extended(bool write, int function, uint32 address, bool increment,
	void* _buffer, size_t length)
{
	uint8* buffer = (uint8*)_buffer;
	uint32 blockSize = fBlockSize[function];

	while (length > 0) {
		bool blockMode;
		uint32 count;
		size_t chunk;
		if (length >= 511 * blockSize) {
			blockMode = true;
			count = 511;
			chunk = count * blockSize;
		} else if (length > blockSize) {
			blockMode = true;
			count = length / blockSize;
			chunk = count * blockSize;
		} else {
			blockMode = false;
			count = length;
			chunk = length;
		}

		status_t status = _ExtendedChunk(write, function, address, increment,
			blockMode, buffer, count, blockSize);
		if (status != B_OK)
			return status;

		buffer += chunk;
		length -= chunk;
		if (increment)
			address += chunk;
	}

	return B_OK;
}


// #pragma mark - card interrupt


void
SdioHost::EnableCardInterrupt(bool enable)
{
	cpu_status state = disable_interrupts();
	acquire_spinlock(&fInterruptLock);

	uint32 mask = fRegisters[REG_IRPT_MASK / 4];
	uint32 signal = fRegisters[REG_IRPT_EN / 4];
	if (enable) {
		mask |= INT_CARD;
		signal |= INT_CARD;
	} else {
		mask &= ~(uint32)INT_CARD;
		signal &= ~(uint32)INT_CARD;
	}
	fRegisters[REG_IRPT_MASK / 4] = mask;
	spin(1);
	fRegisters[REG_IRPT_EN / 4] = signal;
	spin(1);

	release_spinlock(&fInterruptLock);
	restore_interrupts(state);
}


/*!	Called from the interrupt handler: returns whether the card interrupt is
	pending, and masks it if so (it is level triggered, and only goes away
	once the chip has been serviced).
*/
bool
SdioHost::InterruptPending()
{
	acquire_spinlock(&fInterruptLock);

	bool pending = false;
	uint32 signal = fRegisters[REG_IRPT_EN / 4];
	if ((signal & INT_CARD) != 0
		&& (fRegisters[REG_INTERRUPT / 4] & INT_CARD) != 0) {
		pending = true;
		fRegisters[REG_IRPT_EN / 4] = signal & ~(uint32)INT_CARD;
		spin(1);
		fRegisters[REG_IRPT_MASK / 4] = fRegisters[REG_IRPT_MASK / 4]
			& ~(uint32)INT_CARD;
		spin(1);
	}

	release_spinlock(&fInterruptLock);
	return pending;
}
